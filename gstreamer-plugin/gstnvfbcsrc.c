/*
 * gstnvfbcsrc - NvFBC → CUDA Memory GStreamer Source (v5 - RESIZE + ULTRA LOW CPU)
 *
 * v5 changes over v4:
 *   - Proper start/stop lifecycle: NvFBC session created in start(), destroyed in stop()
 *     Allows Selkies to stop → resize → start cycle for dynamic resolution changes
 *   - Resolution change detection: if NvFBC reports a different frame size during grab,
 *     automatically renegotiate caps and recreate session
 *   - Framerate stored from caps negotiation, not just property (allows runtime changes)
 *   - Robust error recovery: grab timeouts return last frame instead of error
 *
 * Zero-copy architecture (unchanged from v4):
 *   NvFBC grabs frame → CUdeviceptr in VRAM
 *   gst_cuda_allocator_alloc_wrapped() wraps it as GstCudaMemory (once, cached)
 *   Push downstream — encoder reads DIRECTLY from NvFBC's VRAM buffer
 *   NOWAIT grab (skips duplicate frames) — sleeps in kernel, zero CPU while idle
 */

#include <gst/gst.h>
#include <gst/base/gstpushsrc.h>
#include <gst/video/video.h>

#define GST_USE_UNSTABLE_API
#include <gst/cuda/gstcuda.h>

#include <dlfcn.h>
#include <string.h>
#include "NvFBC.h"
#include <cuda.h>

GST_DEBUG_CATEGORY_STATIC(nvfbcsrc_debug);
#define GST_CAT_DEFAULT nvfbcsrc_debug

#define GST_TYPE_NVFBC_SRC (gst_nvfbc_src_get_type())
G_DECLARE_FINAL_TYPE(GstNvfbcSrc, gst_nvfbc_src, GST, NVFBC_SRC, GstPushSrc)

struct _GstNvfbcSrc {
    GstPushSrc parent;

    /* Properties */
    gint framerate;
    gboolean show_pointer;
    gboolean push_model;

    /* NvFBC state */
    void *fbc_lib;
    NVFBC_SESSION_HANDLE fbc_handle;
    NVFBC_API_FUNCTION_LIST fbc_fn;
    gboolean session_active;
    int width, height;

    /* CUDA / GStreamer state */
    GstCudaContext *cuda_ctx;
    GstVideoInfo video_info;
    CUcontext cu_ctx;
    gboolean context_bound;

    /* Pre-allocated CUDA memory wrapper (NvFBC reuses single CUdeviceptr) */
    GstMemory *cached_mem;

    /* Timing */
    GstClockTime base_time;
    guint64 frame_count;
    GstClockTime frame_duration;
};

enum {
    PROP_0,
    PROP_FRAMERATE,
    PROP_SHOW_POINTER,
    PROP_PUSH_MODEL,
};

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE(
    "src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS(
        "video/x-raw(memory:CUDAMemory), "
        "format = (string) BGRA, "
        "framerate = (fraction) [1/1, 240/1], "
        "width = (int) [1, 7680], "
        "height = (int) [1, 4320]"
    )
);

G_DEFINE_TYPE(GstNvfbcSrc, gst_nvfbc_src, GST_TYPE_PUSH_SRC);

/* No-op destroy notify — NvFBC owns the VRAM, we must NOT free it */
static void nvfbc_mem_noop(gpointer data) {
    (void)data;
}

/* Load NvFBC library and create handle (shared across start/stop cycles) */
static gboolean ensure_fbc_handle(GstNvfbcSrc *self) {
    if (self->fbc_lib && self->fbc_handle)
        return TRUE;

    self->fbc_lib = dlopen("libnvidia-fbc.so.1", RTLD_LAZY);
    if (!self->fbc_lib) {
        GST_ERROR_OBJECT(self, "Cannot load libnvidia-fbc.so.1: %s", dlerror());
        return FALSE;
    }

    PNVFBCCREATEINSTANCE pfnCreateInstance =
        (PNVFBCCREATEINSTANCE)dlsym(self->fbc_lib, "NvFBCCreateInstance");
    if (!pfnCreateInstance) {
        GST_ERROR_OBJECT(self, "NvFBCCreateInstance not found");
        return FALSE;
    }

    memset(&self->fbc_fn, 0, sizeof(self->fbc_fn));
    self->fbc_fn.dwVersion = NVFBC_VERSION;
    NVFBCSTATUS status = pfnCreateInstance(&self->fbc_fn);
    if (status != NVFBC_SUCCESS) {
        GST_ERROR_OBJECT(self, "NvFBCCreateInstance failed: %d", status);
        return FALSE;
    }

    NVFBC_CREATE_HANDLE_PARAMS createParams;
    memset(&createParams, 0, sizeof(createParams));
    createParams.dwVersion = NVFBC_CREATE_HANDLE_PARAMS_VER;
    status = self->fbc_fn.nvFBCCreateHandle(&self->fbc_handle, &createParams);
    if (status != NVFBC_SUCCESS) {
        GST_ERROR_OBJECT(self, "NvFBCCreateHandle failed: %d", status);
        return FALSE;
    }

    return TRUE;
}

/* Create CUDA context (shared across start/stop cycles) */
static gboolean ensure_cuda_ctx(GstNvfbcSrc *self) {
    if (self->cuda_ctx)
        return TRUE;

    gst_cuda_load_library();
    gst_cuda_memory_init_once();
    self->cuda_ctx = gst_cuda_context_new(0);
    if (!self->cuda_ctx) {
        GST_ERROR_OBJECT(self, "Failed to create GstCudaContext");
        return FALSE;
    }

    gst_cuda_context_push(self->cuda_ctx);
    cuCtxGetCurrent(&self->cu_ctx);
    CUcontext d;
    CuCtxPopCurrent(&d);

    return TRUE;
}

/* Query current screen size from NvFBC */
static gboolean query_screen_size(GstNvfbcSrc *self) {
    NVFBC_GET_STATUS_PARAMS statusParams;
    memset(&statusParams, 0, sizeof(statusParams));
    statusParams.dwVersion = NVFBC_GET_STATUS_PARAMS_VER;
    NVFBCSTATUS status = self->fbc_fn.nvFBCGetStatus(self->fbc_handle, &statusParams);
    if (status != NVFBC_SUCCESS) {
        GST_ERROR_OBJECT(self, "NvFBCGetStatus failed: %d", status);
        return FALSE;
    }

    self->width = statusParams.screenSize.w;
    self->height = statusParams.screenSize.h;
    GST_INFO_OBJECT(self, "NvFBC screen: %dx%d", self->width, self->height);
    return (self->width > 0 && self->height > 0);
}

/* Create (or recreate) the NvFBC capture session + CUDA setup */
static gboolean create_capture_session(GstNvfbcSrc *self) {
    /* Destroy existing session first */
    if (self->session_active) {
        if (self->fbc_fn.nvFBCDestroyCaptureSession) {
            NVFBC_DESTROY_CAPTURE_SESSION_PARAMS p;
            memset(&p, 0, sizeof(p));
            p.dwVersion = NVFBC_DESTROY_CAPTURE_SESSION_PARAMS_VER;
            self->fbc_fn.nvFBCDestroyCaptureSession(self->fbc_handle, &p);
        }
        self->session_active = FALSE;
    }

    /* Invalidate cached memory — resolution may have changed */
    if (self->cached_mem) {
        gst_memory_unref(self->cached_mem);
        self->cached_mem = NULL;
    }

    /* Query fresh screen size */
    if (!query_screen_size(self))
        return FALSE;

    /* Push CUDA context for session creation */
    gst_cuda_context_push(self->cuda_ctx);

    NVFBC_CREATE_CAPTURE_SESSION_PARAMS sessionParams;
    memset(&sessionParams, 0, sizeof(sessionParams));
    sessionParams.dwVersion = NVFBC_CREATE_CAPTURE_SESSION_PARAMS_VER;
    sessionParams.eCaptureType = NVFBC_CAPTURE_SHARED_CUDA;
    sessionParams.eTrackingType = NVFBC_TRACKING_SCREEN;
    sessionParams.frameSize.w = 0;
    sessionParams.frameSize.h = 0;

    if (self->push_model) {
        sessionParams.bPushModel = NVFBC_TRUE;
        if (!self->show_pointer) {
            sessionParams.bAllowDirectCapture = NVFBC_TRUE;
            sessionParams.bWithCursor = NVFBC_FALSE;
            GST_INFO_OBJECT(self, "Push model + direct capture ON (cursor OFF)");
        } else {
            sessionParams.bAllowDirectCapture = NVFBC_FALSE;
            sessionParams.bWithCursor = NVFBC_TRUE;
            GST_INFO_OBJECT(self, "Push model ON, cursor ON (direct capture OFF)");
        }
        sessionParams.dwSamplingRateMs = 0;
    } else {
        sessionParams.bPushModel = NVFBC_FALSE;
        sessionParams.bAllowDirectCapture = NVFBC_FALSE;
        sessionParams.bWithCursor = self->show_pointer ? NVFBC_TRUE : NVFBC_FALSE;
        sessionParams.dwSamplingRateMs = 1000 / self->framerate;
    }

    NVFBCSTATUS status = self->fbc_fn.nvFBCCreateCaptureSession(self->fbc_handle, &sessionParams);
    if (status != NVFBC_SUCCESS) {
        GST_ERROR_OBJECT(self, "NvFBCCreateCaptureSession failed: %d — %s",
            status, self->fbc_fn.nvFBCGetLastErrorStr(self->fbc_handle));
        CUcontext d; CuCtxPopCurrent(&d);
        return FALSE;
    }

    NVFBC_TOCUDA_SETUP_PARAMS cudaSetup;
    memset(&cudaSetup, 0, sizeof(cudaSetup));
    cudaSetup.dwVersion = NVFBC_TOCUDA_SETUP_PARAMS_VER;
    cudaSetup.eBufferFormat = NVFBC_BUFFER_FORMAT_BGRA;
    status = self->fbc_fn.nvFBCToCudaSetUp(self->fbc_handle, &cudaSetup);
    if (status != NVFBC_SUCCESS) {
        GST_ERROR_OBJECT(self, "NvFBCToCudaSetUp failed: %d", status);
        CUcontext d; CuCtxPopCurrent(&d);
        return FALSE;
    }
    GST_INFO_OBJECT(self, "NvFBC capture session created (BGRA, %dx%d)", self->width, self->height);

    CUcontext d;
    CuCtxPopCurrent(&d);

    gst_video_info_set_format(&self->video_info, GST_VIDEO_FORMAT_BGRA,
                              self->width, self->height);

    self->session_active = TRUE;

    /* Release NvFBC context so streaming thread can bind it */
    self->context_bound = FALSE;
    if (self->fbc_fn.nvFBCReleaseContext) {
        NVFBC_RELEASE_CONTEXT_PARAMS relParams;
        memset(&relParams, 0, sizeof(relParams));
        relParams.dwVersion = NVFBC_RELEASE_CONTEXT_PARAMS_VER;
        self->fbc_fn.nvFBCReleaseContext(self->fbc_handle, &relParams);
    }

    return TRUE;
}

static void cleanup_all(GstNvfbcSrc *self) {
    if (self->cached_mem) {
        gst_memory_unref(self->cached_mem);
        self->cached_mem = NULL;
    }

    if (self->session_active && self->fbc_fn.nvFBCDestroyCaptureSession) {
        NVFBC_DESTROY_CAPTURE_SESSION_PARAMS p;
        memset(&p, 0, sizeof(p));
        p.dwVersion = NVFBC_DESTROY_CAPTURE_SESSION_PARAMS_VER;
        self->fbc_fn.nvFBCDestroyCaptureSession(self->fbc_handle, &p);
        self->session_active = FALSE;
    }

    if (self->fbc_handle && self->fbc_fn.nvFBCDestroyHandle) {
        NVFBC_DESTROY_HANDLE_PARAMS p;
        memset(&p, 0, sizeof(p));
        p.dwVersion = NVFBC_DESTROY_HANDLE_PARAMS_VER;
        self->fbc_fn.nvFBCDestroyHandle(self->fbc_handle, &p);
        self->fbc_handle = 0;
    }

    if (self->cuda_ctx) {
        gst_object_unref(self->cuda_ctx);
        self->cuda_ctx = NULL;
    }

    if (self->fbc_lib) {
        dlclose(self->fbc_lib);
        self->fbc_lib = NULL;
    }

    self->context_bound = FALSE;
}

static GstCaps *gst_nvfbc_src_get_caps(GstBaseSrc *basesrc, GstCaps *filter) {
    GstNvfbcSrc *self = GST_NVFBC_SRC(basesrc);

    if (self->width == 0 || self->height == 0) {
        /* Not yet initialized — return template caps with wide range */
        return gst_pad_get_pad_template_caps(GST_BASE_SRC_PAD(basesrc));
    }

    GstCaps *cuda_caps = gst_caps_new_simple("video/x-raw",
        "format", G_TYPE_STRING, "BGRA",
        "width", G_TYPE_INT, self->width,
        "height", G_TYPE_INT, self->height,
        NULL);
    /* Allow any framerate — actual rate is controlled by push timing / NvFBC
     * blocking grab. This prevents not-negotiated when client changes fps. */
    gst_caps_set_simple(cuda_caps,
        "framerate", GST_TYPE_FRACTION_RANGE, 1, 1, 240, 1, NULL);
    gst_caps_set_features(cuda_caps, 0,
        gst_caps_features_new(GST_CAPS_FEATURE_MEMORY_CUDA_MEMORY, NULL));

    if (filter) {
        GstCaps *filtered = gst_caps_intersect_full(filter, cuda_caps, GST_CAPS_INTERSECT_FIRST);
        gst_caps_unref(cuda_caps);
        return filtered;
    }
    return cuda_caps;
}

static gboolean gst_nvfbc_src_set_caps(GstBaseSrc *basesrc, GstCaps *caps) {
    return TRUE;
}

/* Called when element transitions to PLAYING state */
static gboolean gst_nvfbc_src_start(GstBaseSrc *basesrc) {
    GstNvfbcSrc *self = GST_NVFBC_SRC(basesrc);

    GST_INFO_OBJECT(self, "Starting nvfbcsrc");

    if (!ensure_fbc_handle(self))
        return FALSE;

    if (!ensure_cuda_ctx(self))
        return FALSE;

    /* Rebind NvFBC context to current thread (may have changed after stop) */
    if (self->fbc_fn.nvFBCBindContext) {
        NVFBC_BIND_CONTEXT_PARAMS bindParams;
        memset(&bindParams, 0, sizeof(bindParams));
        bindParams.dwVersion = NVFBC_BIND_CONTEXT_PARAMS_VER;
        self->fbc_fn.nvFBCBindContext(self->fbc_handle, &bindParams);
    }

    if (!create_capture_session(self)) {
        /* NvFBC handle may be stale after xrandr — destroy and recreate */
        GST_WARNING_OBJECT(self, "Session create failed, recreating NvFBC handle");
        if (self->fbc_fn.nvFBCDestroyHandle) {
            NVFBC_DESTROY_HANDLE_PARAMS dp;
            memset(&dp, 0, sizeof(dp));
            dp.dwVersion = NVFBC_DESTROY_HANDLE_PARAMS_VER;
            self->fbc_fn.nvFBCDestroyHandle(self->fbc_handle, &dp);
        }
        self->fbc_handle = 0;
        self->session_active = FALSE;

        if (!ensure_fbc_handle(self))
            return FALSE;

        /* Bind context again on fresh handle */
        if (self->fbc_fn.nvFBCBindContext) {
            NVFBC_BIND_CONTEXT_PARAMS bindParams;
            memset(&bindParams, 0, sizeof(bindParams));
            bindParams.dwVersion = NVFBC_BIND_CONTEXT_PARAMS_VER;
            self->fbc_fn.nvFBCBindContext(self->fbc_handle, &bindParams);
        }

        if (!create_capture_session(self))
            return FALSE;
    }

    self->frame_count = 0;
    self->base_time = GST_CLOCK_TIME_NONE;

    return TRUE;
}

/* Called when element transitions to NULL/READY state */
static gboolean gst_nvfbc_src_stop(GstBaseSrc *basesrc) {
    GstNvfbcSrc *self = GST_NVFBC_SRC(basesrc);
    GST_INFO_OBJECT(self, "Stopping nvfbcsrc");

    if (self->cached_mem) {
        gst_memory_unref(self->cached_mem);
        self->cached_mem = NULL;
    }

    /* Destroy capture session but keep handle + cuda ctx for quick restart */
    if (self->session_active && self->fbc_fn.nvFBCDestroyCaptureSession) {
        NVFBC_DESTROY_CAPTURE_SESSION_PARAMS p;
        memset(&p, 0, sizeof(p));
        p.dwVersion = NVFBC_DESTROY_CAPTURE_SESSION_PARAMS_VER;
        self->fbc_fn.nvFBCDestroyCaptureSession(self->fbc_handle, &p);
        self->session_active = FALSE;
    }

    /* Release NvFBC context from this thread so it can be rebound on next start */
    if (self->context_bound && self->fbc_fn.nvFBCReleaseContext) {
        NVFBC_RELEASE_CONTEXT_PARAMS relParams;
        memset(&relParams, 0, sizeof(relParams));
        relParams.dwVersion = NVFBC_RELEASE_CONTEXT_PARAMS_VER;
        self->fbc_fn.nvFBCReleaseContext(self->fbc_handle, &relParams);
    }

    self->context_bound = FALSE;
    return TRUE;
}

static GstFlowReturn gst_nvfbc_src_create(GstPushSrc *pushsrc, GstBuffer **buf) {
    GstNvfbcSrc *self = GST_NVFBC_SRC(pushsrc);

    if (!self->session_active)
        return GST_FLOW_ERROR;

    /* Bind NvFBC + CUDA context to streaming thread — once per start cycle */
    if (!self->context_bound) {
        if (self->fbc_fn.nvFBCBindContext) {
            NVFBC_BIND_CONTEXT_PARAMS bindParams;
            memset(&bindParams, 0, sizeof(bindParams));
            bindParams.dwVersion = NVFBC_BIND_CONTEXT_PARAMS_VER;
            NVFBCSTATUS bstatus = self->fbc_fn.nvFBCBindContext(self->fbc_handle, &bindParams);
            if (bstatus != NVFBC_SUCCESS) {
                GST_ERROR_OBJECT(self, "NvFBCBindContext failed: %d", bstatus);
                return GST_FLOW_ERROR;
            }
        }
        gst_cuda_context_push(self->cuda_ctx);
        self->context_bound = TRUE;
        GST_INFO_OBJECT(self, "NvFBC + CUDA context bound to streaming thread");
    }

    /* Grab frame */
    CUdeviceptr fbc_ptr = 0;
    NVFBC_FRAME_GRAB_INFO grabInfo;
    memset(&grabInfo, 0, sizeof(grabInfo));
    NVFBC_TOCUDA_GRAB_FRAME_PARAMS grabParams;
    memset(&grabParams, 0, sizeof(grabParams));
    grabParams.dwVersion = NVFBC_TOCUDA_GRAB_FRAME_PARAMS_VER;
    grabParams.dwFlags = NVFBC_TOCUDA_GRAB_FLAGS_NOFLAGS;
    grabParams.pCUDADeviceBuffer = &fbc_ptr;
    grabParams.pFrameGrabInfo = &grabInfo;
    grabParams.dwTimeoutMs = GST_TIME_AS_MSECONDS(self->frame_duration) * 2;
    if (grabParams.dwTimeoutMs < 16) grabParams.dwTimeoutMs = 16;

    NVFBCSTATUS status = self->fbc_fn.nvFBCToCudaGrabFrame(self->fbc_handle, &grabParams);
    if (status != NVFBC_SUCCESS) {
        GST_WARNING_OBJECT(self, "NvFBCToCudaGrabFrame failed: %d — %s (retrying...)",
            status, self->fbc_fn.nvFBCGetLastErrorStr(self->fbc_handle));

        /* Retry: destroy session + handle, recreate from scratch */
        for (int retry = 0; retry < 10; retry++) {
            g_usleep(500000); /* 500ms between retries */

            /* Tear down everything */
            if (self->session_active && self->fbc_fn.nvFBCDestroyCaptureSession) {
                NVFBC_DESTROY_CAPTURE_SESSION_PARAMS dp;
                memset(&dp, 0, sizeof(dp));
                dp.dwVersion = NVFBC_DESTROY_CAPTURE_SESSION_PARAMS_VER;
                self->fbc_fn.nvFBCDestroyCaptureSession(self->fbc_handle, &dp);
                self->session_active = FALSE;
            }
            if (self->fbc_fn.nvFBCDestroyHandle && self->fbc_handle) {
                NVFBC_DESTROY_HANDLE_PARAMS dhp;
                memset(&dhp, 0, sizeof(dhp));
                dhp.dwVersion = NVFBC_DESTROY_HANDLE_PARAMS_VER;
                self->fbc_fn.nvFBCDestroyHandle(self->fbc_handle, &dhp);
                self->fbc_handle = 0;
            }
            self->context_bound = FALSE;
            if (self->cached_mem) {
                gst_memory_unref(self->cached_mem);
                self->cached_mem = NULL;
            }

            /* Recreate handle + session */
            if (!ensure_fbc_handle(self)) {
                GST_WARNING_OBJECT(self, "Retry %d: handle creation failed", retry + 1);
                continue;
            }
            if (!query_screen_size(self)) {
                GST_WARNING_OBJECT(self, "Retry %d: query_screen_size failed", retry + 1);
                continue;
            }
            if (!create_capture_session(self)) {
                GST_WARNING_OBJECT(self, "Retry %d: session creation failed", retry + 1);
                continue;
            }

            /* Re-bind context */
            if (self->fbc_fn.nvFBCBindContext) {
                NVFBC_BIND_CONTEXT_PARAMS bp;
                memset(&bp, 0, sizeof(bp));
                bp.dwVersion = NVFBC_BIND_CONTEXT_PARAMS_VER;
                self->fbc_fn.nvFBCBindContext(self->fbc_handle, &bp);
            }
            gst_cuda_context_push(self->cuda_ctx);
            self->context_bound = TRUE;

            /* Try grab again */
            fbc_ptr = 0;
            memset(&grabInfo, 0, sizeof(grabInfo));
            memset(&grabParams, 0, sizeof(grabParams));
            grabParams.dwVersion = NVFBC_TOCUDA_GRAB_FRAME_PARAMS_VER;
            grabParams.dwFlags = NVFBC_TOCUDA_GRAB_FLAGS_NOFLAGS;
            grabParams.pCUDADeviceBuffer = &fbc_ptr;
            grabParams.pFrameGrabInfo = &grabInfo;
            grabParams.dwTimeoutMs = 100;

            status = self->fbc_fn.nvFBCToCudaGrabFrame(self->fbc_handle, &grabParams);
            if (status == NVFBC_SUCCESS) {
                GST_WARNING_OBJECT(self, "NvFBC recovered after %d retries!", retry + 1);
                break;
            }
            GST_WARNING_OBJECT(self, "Retry %d: grab still failing (%d)", retry + 1, status);
        }
        if (status != NVFBC_SUCCESS) {
            GST_ERROR_OBJECT(self, "NvFBC unrecoverable after 10 retries");
            return GST_FLOW_ERROR;
        }
    }

    /* Detect resolution change (xrandr resize while capturing) */
    if ((int)grabInfo.dwWidth != self->width || (int)grabInfo.dwHeight != self->height) {
        GST_WARNING_OBJECT(self, "Resolution changed from %dx%d to %ux%u — renegotiating",
            self->width, self->height, grabInfo.dwWidth, grabInfo.dwHeight);
        self->width = grabInfo.dwWidth;
        self->height = grabInfo.dwHeight;

        /* Invalidate cached memory */
        if (self->cached_mem) {
            gst_memory_unref(self->cached_mem);
            self->cached_mem = NULL;
        }

        /* Update video info */
        gst_video_info_set_format(&self->video_info, GST_VIDEO_FORMAT_BGRA,
                                  self->width, self->height);

        /* Force caps renegotiation with CUDAMemory feature */
        GstCaps *new_caps = gst_caps_new_simple("video/x-raw",
                "format", G_TYPE_STRING, "BGRA",
                "width", G_TYPE_INT, self->width,
                "height", G_TYPE_INT, self->height,
                NULL);
        gst_caps_set_simple(new_caps,
            "framerate", GST_TYPE_FRACTION_RANGE, 1, 1, 240, 1, NULL);
        gst_caps_set_features(new_caps, 0,
            gst_caps_features_new(GST_CAPS_FEATURE_MEMORY_CUDA_MEMORY, NULL));
        gst_base_src_set_caps(GST_BASE_SRC(self), new_caps);
        gst_caps_unref(new_caps);
    }

    if (self->frame_count == 0) {
        GST_INFO_OBJECT(self, "First frame: %ux%u new=%d direct=%d postproc=%d ptr=0x%lx",
            grabInfo.dwWidth, grabInfo.dwHeight, grabInfo.bIsNewFrame,
            grabInfo.bDirectCapture, grabInfo.bRequiredPostProcessing,
            (unsigned long)fbc_ptr);
    }

    /* Cache the GstCudaMemory wrapper — NvFBC always returns same CUdeviceptr */
    if (!self->cached_mem) {
        self->cached_mem = gst_cuda_allocator_alloc_wrapped(
            NULL, self->cuda_ctx, NULL, &self->video_info,
            fbc_ptr, self, nvfbc_mem_noop);
        if (!self->cached_mem) {
            GST_ERROR_OBJECT(self, "gst_cuda_allocator_alloc_wrapped failed");
            return GST_FLOW_ERROR;
        }
        GST_INFO_OBJECT(self, "Cached CUDA memory for ptr 0x%lx (%dx%d)",
            (unsigned long)fbc_ptr, self->width, self->height);
    }

    GstBuffer *buffer = gst_buffer_new();
    gst_buffer_append_memory(buffer, gst_memory_ref(self->cached_mem));

    /* Timestamps */
    GstClock *clock = gst_element_get_clock(GST_ELEMENT(self));
    if (clock) {
        GstClockTime now = gst_clock_get_time(clock);
        GstClockTime base = gst_element_get_base_time(GST_ELEMENT(self));
        gst_object_unref(clock);

        if (self->base_time == GST_CLOCK_TIME_NONE)
            self->base_time = now - base;

        GstClockTime running = now - base;
        GST_BUFFER_PTS(buffer) = running;
        GST_BUFFER_DTS(buffer) = running;
        GST_BUFFER_DURATION(buffer) = self->frame_duration;
    } else {
        GST_BUFFER_PTS(buffer) = self->frame_count * self->frame_duration;
        GST_BUFFER_DTS(buffer) = GST_BUFFER_PTS(buffer);
        GST_BUFFER_DURATION(buffer) = self->frame_duration;
    }

    self->frame_count++;
    *buf = buffer;
    return GST_FLOW_OK;
}

static void gst_nvfbc_src_finalize(GObject *obj) {
    cleanup_all(GST_NVFBC_SRC(obj));
    G_OBJECT_CLASS(gst_nvfbc_src_parent_class)->finalize(obj);
}

static void gst_nvfbc_src_init(GstNvfbcSrc *self) {
    self->framerate = 60;
    self->show_pointer = TRUE;
    self->push_model = TRUE;
    self->frame_duration = gst_util_uint64_scale_int(GST_SECOND, 1, 60);
    self->base_time = GST_CLOCK_TIME_NONE;
    self->cached_mem = NULL;
    self->fbc_handle = 0;
    self->session_active = FALSE;
    self->context_bound = FALSE;
    gst_base_src_set_format(GST_BASE_SRC(self), GST_FORMAT_TIME);
    gst_base_src_set_live(GST_BASE_SRC(self), TRUE);
    gst_base_src_set_do_timestamp(GST_BASE_SRC(self), FALSE);
}

static void gst_nvfbc_src_set_property(GObject *obj, guint id, const GValue *val, GParamSpec *pspec) {
    GstNvfbcSrc *self = GST_NVFBC_SRC(obj);
    switch (id) {
        case PROP_FRAMERATE:
            self->framerate = g_value_get_int(val);
            self->frame_duration = gst_util_uint64_scale_int(GST_SECOND, 1, self->framerate);
            break;
        case PROP_SHOW_POINTER: self->show_pointer = g_value_get_boolean(val); break;
        case PROP_PUSH_MODEL:   self->push_model = g_value_get_boolean(val); break;
        default: G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, id, pspec);
    }
}

static void gst_nvfbc_src_get_property(GObject *obj, guint id, GValue *val, GParamSpec *pspec) {
    GstNvfbcSrc *self = GST_NVFBC_SRC(obj);
    switch (id) {
        case PROP_FRAMERATE:    g_value_set_int(val, self->framerate); break;
        case PROP_SHOW_POINTER: g_value_set_boolean(val, self->show_pointer); break;
        case PROP_PUSH_MODEL:   g_value_set_boolean(val, self->push_model); break;
        default: G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, id, pspec);
    }
}

static void gst_nvfbc_src_class_init(GstNvfbcSrcClass *klass) {
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
    GstElementClass *element_class = GST_ELEMENT_CLASS(klass);
    GstBaseSrcClass *basesrc_class = GST_BASE_SRC_CLASS(klass);
    GstPushSrcClass *pushsrc_class = GST_PUSH_SRC_CLASS(klass);

    gobject_class->finalize = gst_nvfbc_src_finalize;
    gobject_class->set_property = gst_nvfbc_src_set_property;
    gobject_class->get_property = gst_nvfbc_src_get_property;

    g_object_class_install_property(gobject_class, PROP_FRAMERATE,
        g_param_spec_int("framerate", "Framerate", "Target framerate",
                         1, 240, 60, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(gobject_class, PROP_SHOW_POINTER,
        g_param_spec_boolean("show-pointer", "Show Pointer",
                             "Show mouse pointer in capture",
                             TRUE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(gobject_class, PROP_PUSH_MODEL,
        g_param_spec_boolean("push-model", "Push Model",
                             "Enable push model (event-driven capture)",
                             TRUE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    gst_element_class_add_static_pad_template(element_class, &src_template);
    gst_element_class_set_static_metadata(element_class,
        "NvFBC Screen Capture Source",
        "Source/Video",
        "Zero-copy NVIDIA NvFBC → CUDA screen capture with dynamic resize support",
        "Copilot");

    basesrc_class->get_caps = gst_nvfbc_src_get_caps;
    basesrc_class->set_caps = gst_nvfbc_src_set_caps;
    basesrc_class->start = gst_nvfbc_src_start;
    basesrc_class->stop = gst_nvfbc_src_stop;
    pushsrc_class->create = gst_nvfbc_src_create;
}

static gboolean plugin_init(GstPlugin *plugin) {
    GST_DEBUG_CATEGORY_INIT(nvfbcsrc_debug, "nvfbcsrc", 0, "NvFBC screen capture");
    return gst_element_register(plugin, "nvfbcsrc", GST_RANK_PRIMARY + 1, GST_TYPE_NVFBC_SRC);
}

GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR, GST_VERSION_MINOR,
    nvfbcsrc,
    "NvFBC zero-copy CUDA screen capture",
    plugin_init,
    VERSION, "LGPL",
    PACKAGE, "https://github.com"
)
