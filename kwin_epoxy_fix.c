#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <GL/gl.h>
#include <GL/glx.h>

typedef GLXContext (*create_arb_fn)(Display*, GLXFBConfig, GLXContext, Bool, const int*);
typedef int (*query_ctx_fn)(Display*, GLXContext, int, int*);
typedef void (*funcptr_t)(void);
typedef funcptr_t (*getproc_fn)(const GLubyte*);

static create_arb_fn orig_create_arb = NULL;
static query_ctx_fn orig_query_ctx = NULL;
static int patched_create = 0;
static int patched_query = 0;

// Track contexts we patched
#define MAX_CTX 16
static GLXContext patched_contexts[MAX_CTX];
static int num_patched = 0;

static void mark_patched(GLXContext ctx) {
    if (num_patched < MAX_CTX) patched_contexts[num_patched++] = ctx;
}

static int is_patched(GLXContext ctx) {
    for (int i = 0; i < num_patched; i++)
        if (patched_contexts[i] == ctx) return 1;
    return 0;
}

static GLXContext my_create_arb(Display *dpy, GLXFBConfig config,
                                 GLXContext share, Bool direct,
                                 const int *attribs) {
    fprintf(stderr, "[EP3] CreateCtxAttribs share=%p\n", (void*)share);
    
    // Build patched attribs: Core 4.6
    int pat[32], j = 0, has_prof = 0;
    if (attribs) {
        for (int i = 0; attribs[i] != 0 && j < 24; i += 2) {
            pat[j] = attribs[i]; pat[j+1] = attribs[i+1];
            if (attribs[i] == 0x2091) { pat[j+1] = 4; }   // MAJOR -> 4
            if (attribs[i] == 0x2092) { pat[j+1] = 6; }   // MINOR -> 6
            if (attribs[i] == 0x9126) { pat[j+1] = 0x1; has_prof = 1; } // CORE
            j += 2;
        }
    }
    if (!has_prof) { pat[j++] = 0x9126; pat[j++] = 0x1; } // Add CORE profile
    pat[j] = 0;
    
    GLXContext ctx = orig_create_arb(dpy, config, share, direct, pat);
    if (ctx) {
        mark_patched(ctx);
        fprintf(stderr, "[EP3] -> %p OK (Core 4.6)\n", (void*)ctx);
    } else {
        fprintf(stderr, "[EP3] Core 4.6 FAILED, fallback\n");
        ctx = orig_create_arb(dpy, config, share, direct, attribs);
        fprintf(stderr, "[EP3] -> %p %s\n", (void*)ctx, ctx ? "OK" : "FAIL");
    }
    return ctx;
}

static int my_query_ctx(Display *dpy, GLXContext ctx, int attribute, int *value) {
    int ret = orig_query_ctx(dpy, ctx, attribute, value);
    
    // For patched contexts, override broken glXQueryContext results
    if (is_patched(ctx)) {
        switch (attribute) {
            case 0x2091: // GLX_CONTEXT_MAJOR_VERSION_ARB
                *value = 4;
                fprintf(stderr, "[EP3] QueryCtx MAJOR: 0->4\n");
                break;
            case 0x2092: // GLX_CONTEXT_MINOR_VERSION_ARB
                *value = 6;
                fprintf(stderr, "[EP3] QueryCtx MINOR: 0->6\n");
                break;
            case 0x9126: // GLX_CONTEXT_PROFILE_MASK_ARB
                *value = 0x1; // CORE
                fprintf(stderr, "[EP3] QueryCtx PROFILE: 0->CORE\n");
                break;
            case 0x2094: // GLX_CONTEXT_FLAGS_ARB
                *value = 0x4; // ROBUST_ACCESS
                fprintf(stderr, "[EP3] QueryCtx FLAGS: 0->0x4\n");
                break;
        }
    }
    return ret;
}

static void resolve_and_patch(const char *sym_name, void **orig_ptr, void *replacement, int *flag) {
    void *glx = dlopen("libGLX.so.0", RTLD_LAZY | RTLD_NOLOAD);
    if (!glx) glx = dlopen("libGLX.so.0", RTLD_LAZY);
    if (!glx) return;
    
    getproc_fn gpa = (getproc_fn)dlsym(glx, "glXGetProcAddress");
    if (!gpa) return;
    
    // For extension functions, use GetProcAddress
    funcptr_t real_fn = gpa((const GLubyte*)sym_name);
    if (!real_fn) {
        // For core functions, try dlsym
        real_fn = (funcptr_t)dlsym(glx, sym_name);
    }
    if (!real_fn) return;
    
    *orig_ptr = (void*)real_fn;
    
    // Find and patch epoxy dispatch table
    char epoxy_sym[128];
    snprintf(epoxy_sym, sizeof(epoxy_sym), "epoxy_%s", sym_name);
    void **slot = (void**)dlsym(RTLD_DEFAULT, epoxy_sym);
    if (slot) {
        *slot = replacement;
        *flag = 1;
        fprintf(stderr, "[EP3] Patched %s dispatch\n", epoxy_sym);
    }
}

__attribute__((constructor(200)))
static void init_patches(void) {
    fprintf(stderr, "[EP3] Init\n");
    
    resolve_and_patch("glXCreateContextAttribsARB", 
                      (void**)&orig_create_arb, (void*)my_create_arb, &patched_create);
    resolve_and_patch("glXQueryContext",
                      (void**)&orig_query_ctx, (void*)my_query_ctx, &patched_query);
    
    fprintf(stderr, "[EP3] create=%d query=%d\n", patched_create, patched_query);
}
