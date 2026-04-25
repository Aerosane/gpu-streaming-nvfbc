/*
 * gstnvfbcenc - NvFBC Capture + NVENC Encode in a single GStreamer element
 *
 * Architecture:
 *   NvFBC captures desktop → BGRA CUdeviceptr in VRAM
 *   NVENC encodes directly from the CUDA pointer → H264 bitstream
 *   No cudaconvert, no GStreamer buffer copies, no intermediate elements
 *   Output: video/x-h264 stream-format=byte-stream
 *
 * NVENC accepts BGRA (ARGB) input natively — does colorspace internally.
 * This eliminates GStreamer's cudaconvert + nvcudah264enc overhead,
 * doing capture→encode in ~2 API calls on the GPU.
 *
 * Features:
 *   - NOWAIT capture + wall-clock pacing (consistent fps, not screen-update limited)
 *   - Force-keyframe via GStreamer event (PLI/FIR from WebRTC)
 *   - Resolution change detection and encoder reinit
 *   - VBV buffer sized for CBR stability (2× bitrate)
 */

#include <gst/gst.h>
#include <gst/base/gstpushsrc.h>
#include <gst/video/video.h>

#include <dlfcn.h>
#include <string.h>
#include <time.h>
#include "NvFBC.h"
#include "nvEncodeAPI.h"
#include <cuda.h>

GST_DEBUG_CATEGORY_STATIC(nvfbcenc_debug);
#define GST_CAT_DEFAULT nvfbcenc_debug

#define GST_TYPE_NVFBC_ENC (gst_nvfbc_enc_get_type())
G_DECLARE_FINAL_TYPE(GstNvfbcEnc, gst_nvfbc_enc, GST, NVFBC_ENC, GstPushSrc)

/* NVENC function pointers loaded at runtime */
typedef NVENCSTATUS (NVENCAPI *PFN_NvEncodeAPICreateInstance)(NV_ENCODE_API_FUNCTION_LIST *);

struct _GstNvfbcEnc {
    GstPushSrc parent;

    /* Properties */
    gint framerate;
    gint bitrate;       /* kbit/s */
    gint vbv_buffer;    /* kbit, 0=auto (2× bitrate) */
    gboolean show_pointer;
    gboolean push_model;

    /* NvFBC state */
    void *fbc_lib;
    NVFBC_SESSION_HANDLE fbc_handle;
    NVFBC_API_FUNCTION_LIST fbc_fn;
    gboolean session_active;
    int width, height;

    /* NVENC state */
    void *nvenc_lib;
    NV_ENCODE_API_FUNCTION_LIST nvenc_fn;
    void *nvenc_session;
    NV_ENC_REGISTERED_PTR registered_res;
    NV_ENC_INPUT_PTR mapped_input;
    NV_ENC_OUTPUT_PTR output_bitstream;
    CUdeviceptr last_fbc_ptr;   /* track if NvFBC pointer changes */

    /* CUDA */
    CUcontext cu_ctx;
    gboolean context_bound;

    /* Timing — NOWAIT + wall-clock pacing */
    GstClockTime base_time;
    guint64 frame_count;
    GstClockTime frame_duration;
    gint64 next_capture_time;   /* monotonic usec deadline for NOWAIT pacing */
    gboolean need_keyframe;
};

enum {
    PROP_0,
    PROP_FRAMERATE,
    PROP_BITRATE,
    PROP_VBV_BUFFER,
    PROP_SHOW_POINTER,
    PROP_PUSH_MODEL,
};

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE(
    "src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS(
        "video/x-h264, "
        "stream-format = (string) byte-stream, "
        "alignment = (string) au, "
        "profile = (string) { main, high }"
    )
);

G_DEFINE_TYPE(GstNvfbcEnc, gst_nvfbc_enc, GST_TYPE_PUSH_SRC);

/* ======================== NvFBC helpers ======================== */

static int query_screen_size(GstNvfbcEnc *self, int *w, int *h) {
    NVFBC_GET_STATUS_PARAMS statusParams;
    memset(&statusParams, 0, sizeof(statusParams));
    statusParams.dwVersion = NVFBC_GET_STATUS_PARAMS_VER;
    NVFBCSTATUS st = self->fbc_fn.nvFBCGetStatus(self->fbc_handle, &statusParams);
    if (st != NVFBC_SUCCESS) return -1;
    if (!statusParams.bIsCapturePossible) return -1;
    *w = statusParams.screenSize.w;
    *h = statusParams.screenSize.h;
    GST_INFO_OBJECT(self, "NvFBC screen: %dx%d", *w, *h);
    return 0;
}

static gboolean ensure_fbc_handle(GstNvfbcEnc *self) {
    if (self->fbc_lib && self->fbc_handle)
        return TRUE;

    self->fbc_lib = dlopen("libnvidia-fbc.so.1", RTLD_LAZY);
    if (!self->fbc_lib) {
        GST_ERROR_OBJECT(self, "Failed to load libnvidia-fbc.so.1: %s", dlerror());
        return FALSE;
    }

    typedef NVFBCSTATUS (*PFN_Create)(NVFBC_API_FUNCTION_LIST *);
    PFN_Create pfnCreate = (PFN_Create)dlsym(self->fbc_lib, "NvFBCCreateInstance");
    if (!pfnCreate) {
        GST_ERROR_OBJECT(self, "NvFBCCreateInstance not found");
        return FALSE;
    }

    memset(&self->fbc_fn, 0, sizeof(self->fbc_fn));
    self->fbc_fn.dwVersion = NVFBC_VERSION;
    if (pfnCreate(&self->fbc_fn) != NVFBC_SUCCESS) {
        GST_ERROR_OBJECT(self, "NvFBCCreateInstance failed");
        return FALSE;
    }

    NVFBC_CREATE_HANDLE_PARAMS createParams;
    memset(&createParams, 0, sizeof(createParams));
    createParams.dwVersion = NVFBC_CREATE_HANDLE_PARAMS_VER;
    createParams.bExternallyManagedContext = NVFBC_FALSE;
    if (self->fbc_fn.nvFBCCreateHandle(&self->fbc_handle, &createParams) != NVFBC_SUCCESS) {
        GST_ERROR_OBJECT(self, "nvFBCCreateHandle failed");
        return FALSE;
    }
    return TRUE;
}

static gboolean create_capture_session(GstNvfbcEnc *self) {
    if (self->session_active) {
        NVFBC_DESTROY_CAPTURE_SESSION_PARAMS dparams;
        memset(&dparams, 0, sizeof(dparams));
        dparams.dwVersion = NVFBC_DESTROY_CAPTURE_SESSION_PARAMS_VER;
        self->fbc_fn.nvFBCDestroyCaptureSession(self->fbc_handle, &dparams);
        self->session_active = FALSE;
    }

    NVFBC_CREATE_CAPTURE_SESSION_PARAMS sessParams;
    memset(&sessParams, 0, sizeof(sessParams));
    sessParams.dwVersion = NVFBC_CREATE_CAPTURE_SESSION_PARAMS_VER;
    sessParams.eCaptureType = NVFBC_CAPTURE_SHARED_CUDA;
    sessParams.bWithCursor = self->show_pointer ? NVFBC_TRUE : NVFBC_FALSE;
    sessParams.eTrackingType = NVFBC_TRACKING_SCREEN;
    sessParams.dwSamplingRateMs = 1000 / self->framerate;
    if (sessParams.dwSamplingRateMs < 1) sessParams.dwSamplingRateMs = 1;
    sessParams.bPushModel = self->push_model ? NVFBC_TRUE : NVFBC_FALSE;
    sessParams.bAllowDirectCapture = NVFBC_FALSE;

    NVFBC_TOCUDA_SETUP_PARAMS cudaSetup;
    memset(&cudaSetup, 0, sizeof(cudaSetup));
    cudaSetup.dwVersion = NVFBC_TOCUDA_SETUP_PARAMS_VER;
    cudaSetup.eBufferFormat = NVFBC_BUFFER_FORMAT_BGRA;

    if (self->fbc_fn.nvFBCCreateCaptureSession(self->fbc_handle, &sessParams) != NVFBC_SUCCESS) {
        GST_ERROR_OBJECT(self, "nvFBCCreateCaptureSession failed: %s",
            self->fbc_fn.nvFBCGetLastErrorStr(self->fbc_handle));
        return FALSE;
    }
    if (self->fbc_fn.nvFBCToCudaSetUp(self->fbc_handle, &cudaSetup) != NVFBC_SUCCESS) {
        GST_ERROR_OBJECT(self, "nvFBCToCudaSetUp failed: %s",
            self->fbc_fn.nvFBCGetLastErrorStr(self->fbc_handle));
        return FALSE;
    }

    self->session_active = TRUE;
    GST_INFO_OBJECT(self, "NvFBC capture session created (BGRA, %dx%d)", self->width, self->height);
    return TRUE;
}

/* ======================== NVENC helpers ======================== */

static gboolean init_nvenc(GstNvfbcEnc *self) {
    self->nvenc_lib = dlopen("libnvidia-encode.so.1", RTLD_LAZY);
    if (!self->nvenc_lib) {
        GST_ERROR_OBJECT(self, "Failed to load libnvidia-encode.so.1: %s", dlerror());
        return FALSE;
    }

    PFN_NvEncodeAPICreateInstance pfn =
        (PFN_NvEncodeAPICreateInstance)dlsym(self->nvenc_lib, "NvEncodeAPICreateInstance");
    if (!pfn) {
        GST_ERROR_OBJECT(self, "NvEncodeAPICreateInstance not found");
        return FALSE;
    }

    memset(&self->nvenc_fn, 0, sizeof(self->nvenc_fn));
    self->nvenc_fn.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    NVENCSTATUS nst = pfn(&self->nvenc_fn);
    if (nst != NV_ENC_SUCCESS) {
        GST_ERROR_OBJECT(self, "NvEncodeAPICreateInstance failed: %d", nst);
        return FALSE;
    }

    GST_INFO_OBJECT(self, "NVENC API loaded successfully");
    return TRUE;
}

static gboolean create_nvenc_session(GstNvfbcEnc *self) {
    /* Open encode session on our CUDA context */
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sessParams;
    memset(&sessParams, 0, sizeof(sessParams));
    sessParams.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    sessParams.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
    sessParams.device = (void *)self->cu_ctx;
    sessParams.apiVersion = NVENCAPI_VERSION;

    NVENCSTATUS nst = self->nvenc_fn.nvEncOpenEncodeSessionEx(&sessParams, &self->nvenc_session);
    if (nst != NV_ENC_SUCCESS) {
        GST_ERROR_OBJECT(self, "nvEncOpenEncodeSessionEx failed: %d", nst);
        return FALSE;
    }

    /* Initialize encoder — H264, CBR, ultra-low-latency */
    NV_ENC_INITIALIZE_PARAMS initParams;
    memset(&initParams, 0, sizeof(initParams));
    initParams.version = NV_ENC_INITIALIZE_PARAMS_VER;
    initParams.encodeGUID = NV_ENC_CODEC_H264_GUID;
    initParams.presetGUID = NV_ENC_PRESET_P7_GUID;
    initParams.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    initParams.encodeWidth = self->width;
    initParams.encodeHeight = self->height;
    initParams.darWidth = self->width;
    initParams.darHeight = self->height;
    initParams.frameRateNum = self->framerate;
    initParams.frameRateDen = 1;
    initParams.enablePTD = 1;
    initParams.enableEncodeAsync = 0;

    /* Get preset config as base */
    NV_ENC_PRESET_CONFIG presetConfig;
    memset(&presetConfig, 0, sizeof(presetConfig));
    presetConfig.version = NV_ENC_PRESET_CONFIG_VER;
    presetConfig.presetCfg.version = NV_ENC_CONFIG_VER;

    nst = self->nvenc_fn.nvEncGetEncodePresetConfigEx(self->nvenc_session,
        NV_ENC_CODEC_H264_GUID, NV_ENC_PRESET_P7_GUID,
        NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &presetConfig);
    if (nst != NV_ENC_SUCCESS) {
        GST_ERROR_OBJECT(self, "nvEncGetEncodePresetConfigEx failed: %d", nst);
        return FALSE;
    }

    NV_ENC_CONFIG encConfig;
    memcpy(&encConfig, &presetConfig.presetCfg, sizeof(encConfig));
    encConfig.version = NV_ENC_CONFIG_VER;

    /* CBR rate control with proper VBV buffer */
    guint32 vbv = self->vbv_buffer > 0 ? (guint32)(self->vbv_buffer * 1000)
                                        : (guint32)(self->bitrate * 2000);
    encConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    encConfig.rcParams.averageBitRate = self->bitrate * 1000;
    encConfig.rcParams.maxBitRate = self->bitrate * 1000;
    encConfig.rcParams.vbvBufferSize = vbv;
    encConfig.rcParams.vbvInitialDelay = vbv;
    encConfig.rcParams.zeroReorderDelay = 1;
    encConfig.rcParams.enableAQ = 1;
    encConfig.rcParams.multiPass = NV_ENC_MULTI_PASS_DISABLED;

    /* H264-specific: Main profile, no B-frames, CABAC, repeat SPS/PPS */
    encConfig.profileGUID = NV_ENC_H264_PROFILE_MAIN_GUID;
    encConfig.encodeCodecConfig.h264Config.repeatSPSPPS = 1;
    encConfig.encodeCodecConfig.h264Config.idrPeriod = NVENC_INFINITE_GOPLENGTH;
    encConfig.encodeCodecConfig.h264Config.maxNumRefFrames = 1;
    encConfig.encodeCodecConfig.h264Config.entropyCodingMode = NV_ENC_H264_ENTROPY_CODING_MODE_CABAC;
    encConfig.encodeCodecConfig.h264Config.disableSPSPPS = 0;
    encConfig.gopLength = NVENC_INFINITE_GOPLENGTH;
    encConfig.frameIntervalP = 1; /* no B-frames */

    initParams.encodeConfig = &encConfig;

    nst = self->nvenc_fn.nvEncInitializeEncoder(self->nvenc_session, &initParams);
    if (nst != NV_ENC_SUCCESS) {
        GST_ERROR_OBJECT(self, "nvEncInitializeEncoder failed: %d", nst);
        return FALSE;
    }

    /* Create output bitstream buffer */
    NV_ENC_CREATE_BITSTREAM_BUFFER bsbParams;
    memset(&bsbParams, 0, sizeof(bsbParams));
    bsbParams.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    nst = self->nvenc_fn.nvEncCreateBitstreamBuffer(self->nvenc_session, &bsbParams);
    if (nst != NV_ENC_SUCCESS) {
        GST_ERROR_OBJECT(self, "nvEncCreateBitstreamBuffer failed: %d", nst);
        return FALSE;
    }
    self->output_bitstream = bsbParams.bitstreamBuffer;

    GST_INFO_OBJECT(self, "NVENC H264 encoder initialized: %dx%d @ %dfps, %d kbps CBR, VBV %d kbits",
        self->width, self->height, self->framerate, self->bitrate, vbv / 1000);
    return TRUE;
}

static void destroy_nvenc_session(GstNvfbcEnc *self) {
    if (!self->nvenc_session) return;

    if (self->registered_res) {
        if (self->mapped_input) {
            self->nvenc_fn.nvEncUnmapInputResource(self->nvenc_session, self->mapped_input);
            self->mapped_input = NULL;
        }
        self->nvenc_fn.nvEncUnregisterResource(self->nvenc_session, self->registered_res);
        self->registered_res = NULL;
    }

    if (self->output_bitstream) {
        self->nvenc_fn.nvEncDestroyBitstreamBuffer(self->nvenc_session, self->output_bitstream);
        self->output_bitstream = NULL;
    }

    self->nvenc_fn.nvEncDestroyEncoder(self->nvenc_session);
    self->nvenc_session = NULL;
}

/* Register an NvFBC CUDA pointer with NVENC for encoding */
static gboolean register_cuda_resource(GstNvfbcEnc *self, CUdeviceptr ptr) {
    if (self->registered_res) {
        /* Unmap and unregister old resource */
        if (self->mapped_input) {
            self->nvenc_fn.nvEncUnmapInputResource(self->nvenc_session, self->mapped_input);
            self->mapped_input = NULL;
        }
        self->nvenc_fn.nvEncUnregisterResource(self->nvenc_session, self->registered_res);
        self->registered_res = NULL;
    }

    NV_ENC_REGISTER_RESOURCE regParams;
    memset(&regParams, 0, sizeof(regParams));
    regParams.version = NV_ENC_REGISTER_RESOURCE_VER;
    regParams.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR;
    regParams.resourceToRegister = (void *)ptr;
    regParams.width = self->width;
    regParams.height = self->height;
    regParams.pitch = self->width * 4; /* BGRA = 4 bytes per pixel */
    regParams.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
    regParams.bufferUsage = NV_ENC_INPUT_IMAGE;

    NVENCSTATUS nst = self->nvenc_fn.nvEncRegisterResource(self->nvenc_session, &regParams);
    if (nst != NV_ENC_SUCCESS) {
        GST_ERROR_OBJECT(self, "nvEncRegisterResource failed: %d", nst);
        return FALSE;
    }
    self->registered_res = regParams.registeredResource;

    /* Map the resource for encoding */
    NV_ENC_MAP_INPUT_RESOURCE mapParams;
    memset(&mapParams, 0, sizeof(mapParams));
    mapParams.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    mapParams.registeredResource = self->registered_res;

    nst = self->nvenc_fn.nvEncMapInputResource(self->nvenc_session, &mapParams);
    if (nst != NV_ENC_SUCCESS) {
        GST_ERROR_OBJECT(self, "nvEncMapInputResource failed: %d", nst);
        return FALSE;
    }
    self->mapped_input = mapParams.mappedResource;

    GST_INFO_OBJECT(self, "Registered CUDA resource 0x%lx with NVENC", (unsigned long)ptr);
    return TRUE;
}

/* ======================== GstPushSrc virtual methods ======================== */

static GstCaps *gst_nvfbc_enc_get_caps(GstBaseSrc *base, GstCaps *filter) {
    GstNvfbcEnc *self = GST_NVFBC_ENC(base);

    GstCaps *caps = gst_caps_new_simple("video/x-h264",
        "stream-format", G_TYPE_STRING, "byte-stream",
        "alignment", G_TYPE_STRING, "au",
        "profile", G_TYPE_STRING, "main",
        NULL);

    if (self->width > 0 && self->height > 0) {
        gst_caps_set_simple(caps,
            "width", G_TYPE_INT, self->width,
            "height", G_TYPE_INT, self->height,
            "framerate", GST_TYPE_FRACTION, self->framerate, 1,
            NULL);
    }

    if (filter) {
        GstCaps *filtered = gst_caps_intersect_full(filter, caps, GST_CAPS_INTERSECT_FIRST);
        gst_caps_unref(caps);
        return filtered;
    }
    return caps;
}

static gboolean gst_nvfbc_enc_set_caps(GstBaseSrc *base, GstCaps *caps) {
    (void)base; (void)caps;
    return TRUE;
}

static gboolean gst_nvfbc_enc_start(GstBaseSrc *base) {
    GstNvfbcEnc *self = GST_NVFBC_ENC(base);
    GST_INFO_OBJECT(self, "Starting nvfbcenc");

    /* Initialize CUDA driver API */
    cuInit(0);

    if (!ensure_fbc_handle(self))
        return FALSE;

    if (query_screen_size(self, &self->width, &self->height) != 0) {
        GST_ERROR_OBJECT(self, "Could not query screen size");
        return FALSE;
    }

    /* Create and push CUDA context before NvFBC setup */
    CUdevice dev;
    cuDeviceGet(&dev, 0);
    CUresult cures = cuCtxCreate(&self->cu_ctx, 0, dev);
    if (cures != CUDA_SUCCESS) {
        /* Try getting existing context */
        cures = cuCtxGetCurrent(&self->cu_ctx);
        if (cures != CUDA_SUCCESS || !self->cu_ctx) {
            GST_ERROR_OBJECT(self, "Failed to create/get CUDA context: %d", cures);
            return FALSE;
        }
    }
    /* CUDA context must be current for NvFBC ToCudaSetUp */
    cuCtxPushCurrent(self->cu_ctx);

    if (!create_capture_session(self)) {
        cuCtxPopCurrent(NULL);
        return FALSE;
    }

    /* Initialize NVENC */
    if (!init_nvenc(self))
        return FALSE;

    /* CUDA context already pushed above */
    if (!create_nvenc_session(self)) {
        cuCtxPopCurrent(NULL);
        return FALSE;
    }

    cuCtxPopCurrent(NULL);

    /* Release NvFBC context from main thread — will rebind on streaming thread */
    if (self->fbc_fn.nvFBCReleaseContext) {
        NVFBC_RELEASE_CONTEXT_PARAMS relParams;
        memset(&relParams, 0, sizeof(relParams));
        relParams.dwVersion = NVFBC_RELEASE_CONTEXT_PARAMS_VER;
        self->fbc_fn.nvFBCReleaseContext(self->fbc_handle, &relParams);
    }

    self->frame_count = 0;
    self->base_time = GST_CLOCK_TIME_NONE;
    self->need_keyframe = TRUE;

    /* Set caps on the pad */
    GstCaps *caps = gst_caps_new_simple("video/x-h264",
        "stream-format", G_TYPE_STRING, "byte-stream",
        "alignment", G_TYPE_STRING, "au",
        "profile", G_TYPE_STRING, "main",
        "width", G_TYPE_INT, self->width,
        "height", G_TYPE_INT, self->height,
        "framerate", GST_TYPE_FRACTION, self->framerate, 1,
        NULL);
    gst_base_src_set_caps(base, caps);
    gst_caps_unref(caps);

    return TRUE;
}

static gboolean gst_nvfbc_enc_stop(GstBaseSrc *base) {
    GstNvfbcEnc *self = GST_NVFBC_ENC(base);
    GST_INFO_OBJECT(self, "Stopping nvfbcenc");

    if (self->cu_ctx) {
        cuCtxPushCurrent(self->cu_ctx);
        destroy_nvenc_session(self);
        cuCtxPopCurrent(NULL);
    }

    if (self->session_active) {
        NVFBC_DESTROY_CAPTURE_SESSION_PARAMS dparams;
        memset(&dparams, 0, sizeof(dparams));
        dparams.dwVersion = NVFBC_DESTROY_CAPTURE_SESSION_PARAMS_VER;
        self->fbc_fn.nvFBCDestroyCaptureSession(self->fbc_handle, &dparams);
        self->session_active = FALSE;
    }

    if (self->context_bound && self->fbc_fn.nvFBCReleaseContext) {
        NVFBC_RELEASE_CONTEXT_PARAMS relParams;
        memset(&relParams, 0, sizeof(relParams));
        relParams.dwVersion = NVFBC_RELEASE_CONTEXT_PARAMS_VER;
        self->fbc_fn.nvFBCReleaseContext(self->fbc_handle, &relParams);
    }
    self->context_bound = FALSE;

    /* Destroy FBC handle so _start() can re-create cleanly (resize cycle) */
    if (self->fbc_handle && self->fbc_fn.nvFBCDestroyHandle) {
        NVFBC_DESTROY_HANDLE_PARAMS dhParams;
        memset(&dhParams, 0, sizeof(dhParams));
        dhParams.dwVersion = NVFBC_DESTROY_HANDLE_PARAMS_VER;
        self->fbc_fn.nvFBCDestroyHandle(self->fbc_handle, &dhParams);
        self->fbc_handle = 0;
    }

    /* Destroy CUDA context so _start() creates a fresh one */
    if (self->cu_ctx) {
        cuCtxDestroy(self->cu_ctx);
        self->cu_ctx = NULL;
    }

    /* Reset NVENC registered resource state */
    self->registered_res = NULL;

    return TRUE;
}

static GstFlowReturn gst_nvfbc_enc_create(GstPushSrc *pushsrc, GstBuffer **buf) {
    GstNvfbcEnc *self = GST_NVFBC_ENC(pushsrc);

    if (!self->session_active)
        return GST_FLOW_ERROR;

    /* Bind context to streaming thread — once per start cycle */
    if (!self->context_bound) {
        if (self->fbc_fn.nvFBCBindContext) {
            NVFBC_BIND_CONTEXT_PARAMS bindParams;
            memset(&bindParams, 0, sizeof(bindParams));
            bindParams.dwVersion = NVFBC_BIND_CONTEXT_PARAMS_VER;
            self->fbc_fn.nvFBCBindContext(self->fbc_handle, &bindParams);
        }
        cuCtxPushCurrent(self->cu_ctx);
        self->context_bound = TRUE;
        self->next_capture_time = g_get_monotonic_time();
        GST_INFO_OBJECT(self, "NvFBC + CUDA context bound to streaming thread");
    }

    /* ---- NOWAIT wall-clock pacing ---- */
    gint64 now = g_get_monotonic_time();
    gint64 sleep_us = self->next_capture_time - now;
    if (sleep_us > 1000) {
        g_usleep(sleep_us);
    }
    self->next_capture_time += GST_TIME_AS_USECONDS(self->frame_duration);
    /* Don't let deadline drift too far behind */
    now = g_get_monotonic_time();
    if (self->next_capture_time < now - (gint64)500000)
        self->next_capture_time = now;

    /* ---- Step 1: NvFBC grab (NOWAIT — always returns immediately) ---- */
    CUdeviceptr fbc_ptr = 0;
    NVFBC_FRAME_GRAB_INFO grabInfo;
    memset(&grabInfo, 0, sizeof(grabInfo));

    NVFBC_TOCUDA_GRAB_FRAME_PARAMS grabParams;
    memset(&grabParams, 0, sizeof(grabParams));
    grabParams.dwVersion = NVFBC_TOCUDA_GRAB_FRAME_PARAMS_VER;
    grabParams.dwFlags = NVFBC_TOCUDA_GRAB_FLAGS_NOWAIT;
    grabParams.pCUDADeviceBuffer = &fbc_ptr;
    grabParams.pFrameGrabInfo = &grabInfo;
    grabParams.dwTimeoutMs = 0;

    NVFBCSTATUS fbc_st = self->fbc_fn.nvFBCToCudaGrabFrame(self->fbc_handle, &grabParams);
    if (fbc_st != NVFBC_SUCCESS) {
        GST_ERROR_OBJECT(self, "NvFBCToCudaGrabFrame failed: %d — %s",
            fbc_st, self->fbc_fn.nvFBCGetLastErrorStr(self->fbc_handle));
        return GST_FLOW_ERROR;
    }

    /* Handle resolution change */
    if ((int)grabInfo.dwWidth != self->width || (int)grabInfo.dwHeight != self->height) {
        GST_WARNING_OBJECT(self, "Resolution changed %dx%d -> %ux%u, reinitializing",
            self->width, self->height, grabInfo.dwWidth, grabInfo.dwHeight);
        self->width = grabInfo.dwWidth;
        self->height = grabInfo.dwHeight;

        /* Unregister old resource */
        if (self->mapped_input) {
            self->nvenc_fn.nvEncUnmapInputResource(self->nvenc_session, self->mapped_input);
            self->mapped_input = NULL;
        }
        if (self->registered_res) {
            self->nvenc_fn.nvEncUnregisterResource(self->nvenc_session, self->registered_res);
            self->registered_res = NULL;
        }
        self->last_fbc_ptr = 0;

        /* Recreate NVENC session with new resolution */
        destroy_nvenc_session(self);
        if (!create_nvenc_session(self))
            return GST_FLOW_ERROR;

        /* Update caps */
        GstCaps *caps = gst_caps_new_simple("video/x-h264",
            "stream-format", G_TYPE_STRING, "byte-stream",
            "alignment", G_TYPE_STRING, "au",
            "profile", G_TYPE_STRING, "main",
            "width", G_TYPE_INT, self->width,
            "height", G_TYPE_INT, self->height,
            "framerate", GST_TYPE_FRACTION, self->framerate, 1,
            NULL);
        gst_base_src_set_caps(GST_BASE_SRC(self), caps);
        gst_caps_unref(caps);
        self->need_keyframe = TRUE;
    }

    /* Register CUDA pointer with NVENC (once, or if NvFBC pointer changes) */
    if (!self->registered_res || fbc_ptr != self->last_fbc_ptr) {
        if (!register_cuda_resource(self, fbc_ptr))
            return GST_FLOW_ERROR;
        self->last_fbc_ptr = fbc_ptr;
    }

    /* ---- Step 2: NVENC encode ---- */
    NV_ENC_PIC_PARAMS picParams;
    memset(&picParams, 0, sizeof(picParams));
    picParams.version = NV_ENC_PIC_PARAMS_VER;
    picParams.inputBuffer = self->mapped_input;
    picParams.bufferFmt = NV_ENC_BUFFER_FORMAT_ARGB;
    picParams.inputWidth = self->width;
    picParams.inputHeight = self->height;
    picParams.inputPitch = self->width * 4;
    picParams.outputBitstream = self->output_bitstream;
    picParams.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    picParams.encodePicFlags = 0;

    if (self->need_keyframe) {
        picParams.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
        self->need_keyframe = FALSE;
    }

    if (self->frame_count == 0) {
        GST_INFO_OBJECT(self, "First frame: %ux%u ptr=0x%lx",
            grabInfo.dwWidth, grabInfo.dwHeight, (unsigned long)fbc_ptr);
    }

    NVENCSTATUS enc_st = self->nvenc_fn.nvEncEncodePicture(self->nvenc_session, &picParams);
    if (enc_st != NV_ENC_SUCCESS) {
        GST_ERROR_OBJECT(self, "nvEncEncodePicture failed: %d", enc_st);
        return GST_FLOW_ERROR;
    }

    /* ---- Step 3: Retrieve encoded bitstream ---- */
    NV_ENC_LOCK_BITSTREAM lockParams;
    memset(&lockParams, 0, sizeof(lockParams));
    lockParams.version = NV_ENC_LOCK_BITSTREAM_VER;
    lockParams.outputBitstream = self->output_bitstream;

    enc_st = self->nvenc_fn.nvEncLockBitstream(self->nvenc_session, &lockParams);
    if (enc_st != NV_ENC_SUCCESS) {
        GST_ERROR_OBJECT(self, "nvEncLockBitstream failed: %d", enc_st);
        return GST_FLOW_ERROR;
    }

    /* Wrap bitstream in GstBuffer */
    GstBuffer *buffer = gst_buffer_new_allocate(NULL, lockParams.bitstreamSizeInBytes, NULL);
    gst_buffer_fill(buffer, 0, lockParams.bitstreamBufferPtr, lockParams.bitstreamSizeInBytes);

    self->nvenc_fn.nvEncUnlockBitstream(self->nvenc_session, self->output_bitstream);

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

    /* Mark keyframes */
    if (lockParams.pictureType == NV_ENC_PIC_TYPE_IDR ||
        lockParams.pictureType == NV_ENC_PIC_TYPE_I) {
        GST_BUFFER_FLAG_UNSET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
    } else {
        GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
    }

    self->frame_count++;
    *buf = buffer;
    return GST_FLOW_OK;
}

/* ======================== Cleanup ======================== */

static void cleanup_all(GstNvfbcEnc *self) {
    if (self->nvenc_session) {
        if (self->cu_ctx) cuCtxPushCurrent(self->cu_ctx);
        destroy_nvenc_session(self);
        if (self->cu_ctx) cuCtxPopCurrent(NULL);
    }
    if (self->session_active && self->fbc_fn.nvFBCDestroyCaptureSession) {
        NVFBC_DESTROY_CAPTURE_SESSION_PARAMS dparams;
        memset(&dparams, 0, sizeof(dparams));
        dparams.dwVersion = NVFBC_DESTROY_CAPTURE_SESSION_PARAMS_VER;
        self->fbc_fn.nvFBCDestroyCaptureSession(self->fbc_handle, &dparams);
    }
    if (self->fbc_handle && self->fbc_fn.nvFBCDestroyHandle) {
        NVFBC_DESTROY_HANDLE_PARAMS dhParams;
        memset(&dhParams, 0, sizeof(dhParams));
        dhParams.dwVersion = NVFBC_DESTROY_HANDLE_PARAMS_VER;
        self->fbc_fn.nvFBCDestroyHandle(self->fbc_handle, &dhParams);
        self->fbc_handle = 0;
    }
    if (self->fbc_lib) { dlclose(self->fbc_lib); self->fbc_lib = NULL; }
    if (self->nvenc_lib) { dlclose(self->nvenc_lib); self->nvenc_lib = NULL; }
    if (self->cu_ctx) { cuCtxDestroy(self->cu_ctx); self->cu_ctx = NULL; }
}

static void gst_nvfbc_enc_finalize(GObject *obj) {
    cleanup_all(GST_NVFBC_ENC(obj));
    G_OBJECT_CLASS(gst_nvfbc_enc_parent_class)->finalize(obj);
}

/* ======================== GObject boilerplate ======================== */

static void gst_nvfbc_enc_init(GstNvfbcEnc *self) {
    self->framerate = 144;
    self->bitrate = 25000;
    self->vbv_buffer = 0;   /* auto = 2× bitrate */
    self->show_pointer = TRUE;
    self->push_model = TRUE;
    self->frame_duration = gst_util_uint64_scale_int(GST_SECOND, 1, 144);
    self->base_time = GST_CLOCK_TIME_NONE;
    self->next_capture_time = 0;
    self->fbc_handle = 0;
    self->session_active = FALSE;
    self->context_bound = FALSE;
    self->nvenc_session = NULL;
    self->registered_res = NULL;
    self->mapped_input = NULL;
    self->output_bitstream = NULL;
    self->last_fbc_ptr = 0;
    self->need_keyframe = TRUE;
    gst_base_src_set_format(GST_BASE_SRC(self), GST_FORMAT_TIME);
    gst_base_src_set_live(GST_BASE_SRC(self), TRUE);
    gst_base_src_set_do_timestamp(GST_BASE_SRC(self), FALSE);
}

static void gst_nvfbc_enc_set_property(GObject *obj, guint id, const GValue *val, GParamSpec *pspec) {
    GstNvfbcEnc *self = GST_NVFBC_ENC(obj);
    switch (id) {
        case PROP_FRAMERATE:
            self->framerate = g_value_get_int(val);
            self->frame_duration = gst_util_uint64_scale_int(GST_SECOND, 1, self->framerate);
            break;
        case PROP_BITRATE:     self->bitrate = g_value_get_int(val); break;
        case PROP_VBV_BUFFER:  self->vbv_buffer = g_value_get_int(val); break;
        case PROP_SHOW_POINTER: self->show_pointer = g_value_get_boolean(val); break;
        case PROP_PUSH_MODEL:   self->push_model = g_value_get_boolean(val); break;
        default: G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, id, pspec);
    }
}

static void gst_nvfbc_enc_get_property(GObject *obj, guint id, GValue *val, GParamSpec *pspec) {
    GstNvfbcEnc *self = GST_NVFBC_ENC(obj);
    switch (id) {
        case PROP_FRAMERATE:    g_value_set_int(val, self->framerate); break;
        case PROP_BITRATE:      g_value_set_int(val, self->bitrate); break;
        case PROP_VBV_BUFFER:   g_value_set_int(val, self->vbv_buffer); break;
        case PROP_SHOW_POINTER: g_value_set_boolean(val, self->show_pointer); break;
        case PROP_PUSH_MODEL:   g_value_set_boolean(val, self->push_model); break;
        default: G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, id, pspec);
    }
}

/* Handle force-keyframe events from WebRTC (PLI/FIR) */
static gboolean gst_nvfbc_enc_event(GstBaseSrc *base, GstEvent *event) {
    GstNvfbcEnc *self = GST_NVFBC_ENC(base);

    if (GST_EVENT_TYPE(event) == GST_EVENT_CUSTOM_UPSTREAM) {
        const GstStructure *s = gst_event_get_structure(event);
        if (s && gst_structure_has_name(s, "GstForceKeyUnit")) {
            GST_INFO_OBJECT(self, "Force keyframe requested");
            self->need_keyframe = TRUE;
            return TRUE;
        }
    }
    return GST_BASE_SRC_CLASS(gst_nvfbc_enc_parent_class)->event(base, event);
}

static void gst_nvfbc_enc_class_init(GstNvfbcEncClass *klass) {
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
    GstElementClass *element_class = GST_ELEMENT_CLASS(klass);
    GstBaseSrcClass *basesrc_class = GST_BASE_SRC_CLASS(klass);
    GstPushSrcClass *pushsrc_class = GST_PUSH_SRC_CLASS(klass);

    gobject_class->finalize = gst_nvfbc_enc_finalize;
    gobject_class->set_property = gst_nvfbc_enc_set_property;
    gobject_class->get_property = gst_nvfbc_enc_get_property;

    g_object_class_install_property(gobject_class, PROP_FRAMERATE,
        g_param_spec_int("framerate", "Framerate", "Target capture framerate",
                         1, 240, 144, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(gobject_class, PROP_BITRATE,
        g_param_spec_int("bitrate", "Bitrate", "Target bitrate in kbit/s",
                         100, 200000, 25000, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(gobject_class, PROP_VBV_BUFFER,
        g_param_spec_int("vbv-buffer-size", "VBV Buffer", "VBV buffer size in kbit (0=auto: 2× bitrate)",
                         0, 500000, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
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
        "NvFBC Capture + NVENC Encode",
        "Source/Video/Encoder",
        "Zero-copy NvFBC capture with direct NVENC H264 encoding — no cudaconvert",
        "Copilot");

    basesrc_class->get_caps = gst_nvfbc_enc_get_caps;
    basesrc_class->set_caps = gst_nvfbc_enc_set_caps;
    basesrc_class->start = gst_nvfbc_enc_start;
    basesrc_class->stop = gst_nvfbc_enc_stop;
    basesrc_class->event = gst_nvfbc_enc_event;
    pushsrc_class->create = gst_nvfbc_enc_create;
}

static gboolean plugin_init(GstPlugin *plugin) {
    GST_DEBUG_CATEGORY_INIT(nvfbcenc_debug, "nvfbcenc", 0, "NvFBC + NVENC capture encoder");
    return gst_element_register(plugin, "nvfbcenc", GST_RANK_PRIMARY + 2, GST_TYPE_NVFBC_ENC);
}

GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR, GST_VERSION_MINOR,
    nvfbcenc,
    "NvFBC zero-copy capture with direct NVENC H264 encoding",
    plugin_init,
    VERSION, "LGPL",
    PACKAGE, "https://github.com"
)
