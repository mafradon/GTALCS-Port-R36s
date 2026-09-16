/* egl_patch.c -- EGL interposition for the GTA:LCS loader
 *
 * WHY THIS FILE EXISTS
 *
 * libGTALcs.so imports eglCreateContext, eglMakeCurrent, eglGetCurrentContext
 * and eglCreatePbufferSurface — but NOT eglCreateWindowSurface.  That is the
 * Android GLSurfaceView shape: the Java side owns the on-screen context and
 * window surface, and the native side only ever creates a *shared* context on
 * a pbuffer, for streaming resources on a background thread.
 *
 * So the split here is:
 *   - WE create the window and the main GL context (SDL2, EGL backend).
 *   - The game asks for the current context to share against, then builds its
 *     own loader context beside it.
 *
 * THE REAL PROBLEM THIS SOLVES: THERE ARE TWO EGL LIBRARIES IN THE PROCESS.
 *
 * On the RG353V (and any Arm device shipping a closed Mali driver), the setup
 * at runtime is:
 *
 *   libGLESv2.so.2 -> libMali.so   (GL calls go straight to the vendor driver)
 *   libEGL.so.1    -> libglvnd     (a dispatch layer whose vendor list, on this
 *                                   image, contains ONLY Mesa)
 *
 * The launcher/profile points SDL at the vendor EGL directly
 * (SDL_VIDEO_EGL_DRIVER=libEGL.so, which on this device is a symlink to
 * libMali.so), so SDL's window/context are created *by libMali*.  But our own
 * link-time libEGL.so.1 is glvnd, and glvnd with no Mali vendor JSON answers
 * EGL queries itself or routes them to Mesa.  Every symptom we chased was the
 * two libraries refusing to recognise each other's objects:
 *
 *   - eglGetCurrentContext() (via glvnd) reports EGL_NO_CONTEXT even though
 *     glGetString(GL_RENDERER) says "Mali-G52" on the same thread — Mali has a
 *     current context; glvnd was never told.
 *   - eglQueryContext(EGL_CONFIG_ID) fails — Mali's context against glvnd's
 *     display is EGL_BAD_DISPLAY.  Hence config_id=0, have_config=0.
 *   - No candidate display "knows" the SDL context, because we kept asking the
 *     wrong library about it.
 *
 * THE FIX: discover which EGL library actually owns the context — dlopen the
 * vendor library the same way SDL did — and route EVERY real EGL call through
 * dlsym'd pointers from that library.  One library, one object namespace.
 * On devices where everything is glvnd already (PortMaster targets with Mesa,
 * or where the Mali vendor JSON is installed), the candidates fail to load and
 * we fall back to RTLD_DEFAULT — our own libEGL — which is then correct by
 * construction, exactly like the old code.
 *
 * Still true from the original design:
 *   - eglGetCurrentContext() is answered with the captured MAIN context when
 *     the caller's thread has none (the game's loader thread shares against it).
 *   - ChooseConfig/GetConfigs hand back the config the main context was built
 *     with, so the game's context is share-compatible by construction.
 *
 * No EGL entry point takes or returns a float by value, so unlike most of this
 * port nothing here needs SOFTFP bridging.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
#include <pthread.h>

#include <EGL/egl.h>
#include <SDL2/SDL.h>

#include "egl_patch.h"

/* ── The EGL library that owns the real context ──────────────────────────── */

static void  *g_lib = RTLD_DEFAULT;      /* where real EGL calls go */
static int    g_lib_resolved = 0;

/* eglGetPlatformDisplay tokens not in older headers */
#ifndef EGL_PLATFORM_GBM_KHR
#define EGL_PLATFORM_GBM_KHR      0x31D7
#endif
#ifndef EGL_PLATFORM_DEVICE_EXT
#define EGL_PLATFORM_DEVICE_EXT   0x313F
#endif

typedef EGLDisplay  (*PFN_GETDISPLAY)(EGLNativeDisplayType);
typedef EGLDisplay  (*PFN_GETCURRENTDISPLAY)(void);
typedef EGLDisplay  (*PFN_GETPLATFORMDISPLAY)(EGLint, void *, const EGLint *);
typedef EGLContext  (*PFN_GETCURRENTCONTEXT)(void);
typedef EGLBoolean  (*PFN_INITIALIZE)(EGLDisplay, EGLint *, EGLint *);
typedef EGLBoolean  (*PFN_CHOOSECONFIG)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
typedef EGLBoolean  (*PFN_GETCONFIGS)(EGLDisplay, EGLConfig *, EGLint, EGLint *);
typedef EGLBoolean  (*PFN_GETCONFIGATTRIB)(EGLDisplay, EGLConfig, EGLint, EGLint *);
typedef EGLBoolean  (*PFN_QUERYCONTEXT)(EGLDisplay, EGLContext, EGLint, EGLint *);
typedef EGLContext  (*PFN_CREATECONTEXT)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
typedef EGLSurface  (*PFN_CREATEPBUFFERSURFACE)(EGLDisplay, EGLConfig, const EGLint *);
typedef EGLBoolean  (*PFN_MAKECURRENT)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
typedef EGLBoolean  (*PFN_BINDAPI)(EGLenum);
typedef void       *(*PFN_GETPROCADDRESS)(const char *);
typedef EGLint      (*PFN_GETERROR)(void);

static PFN_GETDISPLAY           p_eglGetDisplay;
static PFN_GETCURRENTDISPLAY    p_eglGetCurrentDisplay;
static PFN_GETPLATFORMDISPLAY   p_eglGetPlatformDisplay;
static PFN_GETCURRENTCONTEXT    p_eglGetCurrentContext;
static PFN_INITIALIZE           p_eglInitialize;
static PFN_CHOOSECONFIG         p_eglChooseConfig;
static PFN_GETCONFIGS           p_eglGetConfigs;
static PFN_GETCONFIGATTRIB      p_eglGetConfigAttrib;
static PFN_QUERYCONTEXT         p_eglQueryContext;
static PFN_CREATECONTEXT        p_eglCreateContext;
static PFN_CREATEPBUFFERSURFACE p_eglCreatePbufferSurface;
static PFN_MAKECURRENT          p_eglMakeCurrent;
static PFN_BINDAPI              p_eglBindAPI;
static PFN_GETPROCADDRESS       p_eglGetProcAddress;
static PFN_GETERROR             p_eglGetError;
typedef EGLSurface (*PFN_GETCURRENTSURFACE)(EGLint);
static PFN_GETCURRENTSURFACE    p_eglGetCurrentSurface;

/* Vendor EGL libraries, most-specific first.  RTLD_NOLOAD finds the instance
 * SDL already dlopen'd (same soname, same mapping) without loading a copy;
 * only if none is resident do we try a real dlopen of the vendor name. */
static const char *kMaliCandidates[] = {
    "libmali.so",
    "libMali.so",
    "libmali-bifrost-g52-g13p0-gbm.so",
    "libmali-bifrost-g31-g13p0-gbm.so",
    "libmali-utgard-400-r7p0.so",
};

static void resolve_egl_lib(void) {
    if (g_lib_resolved) return;
    g_lib_resolved = 1;

    for (unsigned i = 0; i < sizeof(kMaliCandidates)/sizeof(kMaliCandidates[0]); i++) {
        void *h = dlopen(kMaliCandidates[i], RTLD_LAZY | RTLD_NOLOAD);
        if (h) { g_lib = h;
                 fprintf(stderr, "[egl] vendor EGL library found resident: %s\n",
                         kMaliCandidates[i]);
                 break; }
    }
    if (g_lib == RTLD_DEFAULT) {
        fprintf(stderr, "[egl] no vendor EGL resident; using our own libEGL"
                        " (glvnd/system)\n");
    }

    p_eglGetDisplay           = (PFN_GETDISPLAY)          dlsym(g_lib, "eglGetDisplay");
    p_eglGetCurrentDisplay    = (PFN_GETCURRENTDISPLAY)   dlsym(g_lib, "eglGetCurrentDisplay");
    p_eglGetPlatformDisplay   = (PFN_GETPLATFORMDISPLAY)  dlsym(g_lib, "eglGetPlatformDisplayEXT");
    if (!p_eglGetPlatformDisplay)
        p_eglGetPlatformDisplay = (PFN_GETPLATFORMDISPLAY) dlsym(g_lib, "eglGetPlatformDisplay");
    p_eglGetCurrentContext    = (PFN_GETCURRENTCONTEXT)   dlsym(g_lib, "eglGetCurrentContext");
    p_eglInitialize           = (PFN_INITIALIZE)          dlsym(g_lib, "eglInitialize");
    p_eglChooseConfig         = (PFN_CHOOSECONFIG)        dlsym(g_lib, "eglChooseConfig");
    p_eglGetConfigs           = (PFN_GETCONFIGS)          dlsym(g_lib, "eglGetConfigs");
    p_eglGetConfigAttrib      = (PFN_GETCONFIGATTRIB)     dlsym(g_lib, "eglGetConfigAttrib");
    p_eglQueryContext         = (PFN_QUERYCONTEXT)        dlsym(g_lib, "eglQueryContext");
    p_eglCreateContext        = (PFN_CREATECONTEXT)       dlsym(g_lib, "eglCreateContext");
    p_eglCreatePbufferSurface = (PFN_CREATEPBUFFERSURFACE)dlsym(g_lib, "eglCreatePbufferSurface");
    p_eglMakeCurrent          = (PFN_MAKECURRENT)         dlsym(g_lib, "eglMakeCurrent");
    p_eglBindAPI              = (PFN_BINDAPI)             dlsym(g_lib, "eglBindAPI");
    p_eglGetProcAddress       = (PFN_GETPROCADDRESS)      dlsym(g_lib, "eglGetProcAddress");
    p_eglGetError             = (PFN_GETERROR)            dlsym(g_lib, "eglGetError");
    p_eglGetCurrentSurface    = (PFN_GETCURRENTSURFACE)   dlsym(g_lib, "eglGetCurrentSurface");
}

/* ── Captured from the real context SDL created ──────────────────────────── */
static EGLDisplay g_display = EGL_NO_DISPLAY;
static EGLContext g_main_ctx = EGL_NO_CONTEXT;
static EGLConfig  g_config   = NULL;
static int        g_have_config = 0;

/* Returns non-zero and fills *cfg_id_out if d recognises ctx. */
static int display_owns(EGLDisplay d, EGLContext ctx, EGLint *cfg_id_out) {
    if (d == EGL_NO_DISPLAY || !p_eglQueryContext) return 0;
    EGLint v = -1;
    if (p_eglGetError) p_eglGetError();  /* clear: query failure is the probe */
    if (!p_eglQueryContext(d, ctx, EGL_CONFIG_ID, &v) || v < 0) return 0;
    *cfg_id_out = v;
    return 1;
}

void egl_patch_capture(void) {
    resolve_egl_lib();

    /* 1. Context: the owning library's answer first; SDL's pointer otherwise.
     *    On a two-EGL device p_eglGetCurrentContext is Mali's, and Mali *does*
     *    know the current context — the old NULL came from asking glvnd. */
    g_main_ctx = p_eglGetCurrentContext ? p_eglGetCurrentContext() : EGL_NO_CONTEXT;
    if (g_main_ctx == EGL_NO_CONTEXT) {
        SDL_GLContext sdl_ctx = SDL_GL_GetCurrentContext();
        if (sdl_ctx) {
            g_main_ctx = (EGLContext)sdl_ctx;
            fprintf(stderr, "[egl] context taken from SDL (%p); "
                            "eglGetCurrentContext() reported none\n", sdl_ctx);
        }
    }
    if (g_main_ctx == EGL_NO_CONTEXT) {
        fprintf(stderr, "[egl] WARNING: no context to capture\n");
        return;
    }

    /* 2. Display: try candidates in order of likelihood; the right one answers
     *    eglQueryContext about our context.  This replaces the old single
     *    guess that could never work across two EGL libraries. */
    EGLint cfg_id = -1;
    EGLDisplay d;

    d = p_eglGetCurrentDisplay ? p_eglGetCurrentDisplay() : EGL_NO_DISPLAY;
    if (display_owns(d, g_main_ctx, &cfg_id)) {
        g_display = d;
    } else if (p_eglGetPlatformDisplay) {
        d = p_eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, NULL, NULL);
        if (display_owns(d, g_main_ctx, &cfg_id)) {
            g_display = d;
        } else {
            d = p_eglGetPlatformDisplay(EGL_PLATFORM_DEVICE_EXT, NULL, NULL);
            if (display_owns(d, g_main_ctx, &cfg_id)) {
                g_display = d;
            }
        }
    }
    if (g_display == EGL_NO_DISPLAY) {
        d = p_eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (d != EGL_NO_DISPLAY && p_eglInitialize) {
            EGLint mj = 0, mn = 0;
            p_eglInitialize(d, &mj, &mn);
        }
        if (display_owns(d, g_main_ctx, &cfg_id)) {
            g_display = d;
        }
    }
    if (g_display == EGL_NO_DISPLAY) {
        /* Nothing claims it — proceed with whatever the library calls default
         * so later hooks have non-nil handles rather than seeding EGL_NO_*. */
        g_display = p_eglGetDisplay(EGL_DEFAULT_DISPLAY);
        fprintf(stderr, "[egl] WARNING: no display recognises the context; "
                        "proceeding with default %p\n", g_display);
    }

    /* 3. Config: map the config id back to a handle from the SAME library. */
    g_have_config = 0;
    if (cfg_id >= 0) {
        EGLint attrs[] = { EGL_CONFIG_ID, cfg_id, EGL_NONE };
        EGLConfig cfg; EGLint n = 0;
        if (p_eglChooseConfig(g_display, attrs, &cfg, 1, &n) && n == 1) {
            g_config = cfg;
            g_have_config = 1;
        }
    }
    if (!g_have_config) {
        EGLint attrs[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
        EGLConfig cfg; EGLint n = 0;
        if (p_eglChooseConfig(g_display, attrs, &cfg, 1, &n) && n >= 1) {
            g_config = cfg;
            g_have_config = 1;
            fprintf(stderr, "[egl] config recovered via ES2-any search (%d found)\n", n);
        }
    }

    fprintf(stderr, "[egl] captured display=%p ctx=%p config_id=%d have_config=%d\n",
            g_display, g_main_ctx, cfg_id, g_have_config);
}

EGLDisplay egl_patch_display(void) { return g_display; }
EGLContext egl_patch_main_context(void) { return g_main_ctx; }

/* ── Interposed entry points ─────────────────────────────────────────────── */

/* Always the display SDL used — see note 2 above. */
static EGLDisplay eglGetDisplay_hook(EGLNativeDisplayType native) {
    (void)native;
    if (g_display != EGL_NO_DISPLAY) return g_display;
    return p_eglGetDisplay(EGL_DEFAULT_DISPLAY);
}

/* The display is already initialised by SDL.  Re-initialising is legal and
 * refcounted, but reporting our known-good version avoids driver quirks. */
static EGLBoolean eglInitialize_hook(EGLDisplay dpy, EGLint *major, EGLint *minor) {
    (void)dpy;
    if (major) *major = 1;
    if (minor) *minor = 4;
    return EGL_TRUE;
}

/* Hand back the config the main context uses, whatever was asked for, so the
 * game's context is share-compatible with ours. */
static EGLBoolean eglChooseConfig_hook(EGLDisplay dpy, const EGLint *attrib_list,
                                       EGLConfig *configs, EGLint config_size,
                                       EGLint *num_config) {
    (void)dpy;
    if (g_have_config && configs && config_size >= 1) {
        configs[0] = g_config;
        if (num_config) *num_config = 1;
        return EGL_TRUE;
    }
    return p_eglChooseConfig(g_display, attrib_list, configs, config_size, num_config);
}

static EGLBoolean eglGetConfigs_hook(EGLDisplay dpy, EGLConfig *configs,
                                     EGLint config_size, EGLint *num_config) {
    (void)dpy;
    if (g_have_config) {
        if (!configs) {                       /* query-count form */
            if (num_config) *num_config = 1;
            return EGL_TRUE;
        }
        if (config_size >= 1) {
            configs[0] = g_config;
            if (num_config) *num_config = 1;
            return EGL_TRUE;
        }
    }
    return p_eglGetConfigs(g_display, configs, config_size, num_config);
}

static EGLBoolean eglGetConfigAttrib_hook(EGLDisplay dpy, EGLConfig config,
                                          EGLint attribute, EGLint *value) {
    (void)dpy;
    return p_eglGetConfigAttrib(g_display, config, attribute, value);
}

/* The heart of it: the game passes the context it got from
 * eglGetCurrentContext() as share_context.  Force our config, and force our
 * main context as the share target even if it passed EGL_NO_CONTEXT. */
static EGLContext eglCreateContext_hook(EGLDisplay dpy, EGLConfig config,
                                        EGLContext share_context,
                                        const EGLint *attrib_list) {
    (void)dpy;
    EGLConfig  cfg   = g_have_config ? g_config : config;
    EGLContext share = (share_context == EGL_NO_CONTEXT) ? g_main_ctx : share_context;

    EGLContext ctx = p_eglCreateContext(g_display, cfg, share, attrib_list);
    if (ctx == EGL_NO_CONTEXT)
        fprintf(stderr, "[egl] eglCreateContext FAILED (share=%p) err=0x%04x\n",
                share, p_eglGetError());
    else
        fprintf(stderr, "[egl] eglCreateContext OK -> %p (share=%p main=%p)\n",
                ctx, share, g_main_ctx);
    return ctx;
}

static EGLSurface eglCreatePbufferSurface_hook(EGLDisplay dpy, EGLConfig config,
                                               const EGLint *attrib_list) {
    (void)dpy;
    EGLConfig cfg = g_have_config ? g_config : config;
    EGLSurface s = p_eglCreatePbufferSurface(g_display, cfg, attrib_list);
    if (s == EGL_NO_SURFACE)
        fprintf(stderr, "[egl] eglCreatePbufferSurface FAILED err=0x%04x\n",
                p_eglGetError());
    else
        fprintf(stderr, "[egl] eglCreatePbufferSurface OK -> %p\n", s);
    return s;
}

static EGLBoolean eglMakeCurrent_hook(EGLDisplay dpy, EGLSurface draw,
                                      EGLSurface read, EGLContext ctx) {
    /* NULL display is legal input the game can pass; substitute ours. */
    EGLBoolean r = p_eglMakeCurrent(dpy == EGL_NO_DISPLAY ? g_display : dpy,
                                    draw, read, ctx);
    /* If the engine ever draws from a thread other than the render thread,
     * this is where it would reveal itself: EGL_BAD_ACCESS because the context
     * is current on the render thread.  Log every non-trivial call. */
    static int logged;
    if (!r || ctx != g_main_ctx || logged < 16) {
        logged++;
        fprintf(stderr, "[egl] eglMakeCurrent(d=%p draw=%p read=%p ctx=%p main=%p)"
                        " -> %d err=0x%04x thread=%p\n",
                dpy, draw, read, ctx, g_main_ctx, (int)r, p_eglGetError(),
                (void *)(uintptr_t)pthread_self());
    }
    return r;
}

/* See note 1: report the main context, not this thread's (usually none). */
static EGLContext eglGetCurrentContext_hook(void) {
    EGLContext cur = p_eglGetCurrentContext ? p_eglGetCurrentContext()
                                            : EGL_NO_CONTEXT;
    return (cur != EGL_NO_CONTEXT) ? cur : g_main_ctx;
}

static EGLBoolean eglQueryContext_hook(EGLDisplay dpy, EGLContext ctx,
                                       EGLint attribute, EGLint *value) {
    (void)dpy;
    return p_eglQueryContext(g_display, ctx, attribute, value);
}

static EGLBoolean eglBindAPI_hook(EGLenum api) {
    return p_eglBindAPI(api);
}

/* Extension pointers must come from the SAME library as the GL calls, which on
 * a Mali device is the vendor lib, not glvnd — dlsym'd pointer again.
 *
 * This is the opposite of what the Chinatown Wars port does (it stubs
 * eglGetProcAddress to NULL so its engine falls back to GOT imports, because
 * the driver's HARD-FLOAT pointers would be called with soft-float
 * convention).  It does not transfer to LCS for two measured reasons:
 *
 *   1. Every function LCS asks for here is float-free.  The full request set,
 *      captured on device, is 18 entry points — VAOs, occlusion queries,
 *      buffer mapping and debug labels.  All take only integers and pointers,
 *      so there is no ABI boundary to get wrong.
 *   2. None of them is a GOT import — they are extension entry points, absent
 *      from the .so's 408 undefined symbols, obtainable ONLY through this
 *      call.  There is no fallback to fall back TO.
 *
 * The logging stays: if a float-taking extension ever shows up in that list it
 * needs a SOFTFP thunk, and the log is what will reveal it. */
static __eglMustCastToProperFunctionPointerType
eglGetProcAddress_hook(const char *procname) {
    __eglMustCastToProperFunctionPointerType fn =
        p_eglGetProcAddress ? p_eglGetProcAddress(procname) : NULL;
    fprintf(stderr, "[egl] eglGetProcAddress('%s') -> %p\n",
            procname ? procname : "?", (void *)fn);
    return fn;
}

/* Bring-up diagnostic: what is actually bound on the CALLING thread right now.
 * The engine creates its own shared context on a pbuffer; if it ever leaves
 * that current on the render thread, our glReadPixels reads the pbuffer and
 * our SDL_GL_SwapWindow posts a surface nobody drew into.  This is the call
 * that tells the two apart. */
void egl_patch_report_current(const char *tag) {
    resolve_egl_lib();
    EGLContext ctx  = p_eglGetCurrentContext ? p_eglGetCurrentContext() : (EGLContext)0;
    EGLSurface draw = p_eglGetCurrentSurface ? p_eglGetCurrentSurface(EGL_DRAW) : (EGLSurface)0;
    EGLSurface read = p_eglGetCurrentSurface ? p_eglGetCurrentSurface(EGL_READ) : (EGLSurface)0;
    fprintf(stderr, "[egl] current@%s: ctx=%p (main=%p, %s) draw=%p read=%p\n",
            tag, ctx, g_main_ctx, ctx == g_main_ctx ? "OURS" : "NOT OURS",
            draw, read);
}

/* ── Table consumed by main.c's symbol resolver ──────────────────────────── */
const so_default_dynlib egl_dynlib[] = {
    { "eglBindAPI",              (uintptr_t)&eglBindAPI_hook              },
    { "eglChooseConfig",         (uintptr_t)&eglChooseConfig_hook         },
    { "eglCreateContext",        (uintptr_t)&eglCreateContext_hook        },
    { "eglCreatePbufferSurface", (uintptr_t)&eglCreatePbufferSurface_hook },
    { "eglGetConfigAttrib",      (uintptr_t)&eglGetConfigAttrib_hook      },
    { "eglGetConfigs",           (uintptr_t)&eglGetConfigs_hook           },
    { "eglGetCurrentContext",    (uintptr_t)&eglGetCurrentContext_hook    },
    { "eglGetDisplay",           (uintptr_t)&eglGetDisplay_hook           },
    { "eglGetProcAddress",       (uintptr_t)&eglGetProcAddress_hook       },
    { "eglInitialize",           (uintptr_t)&eglInitialize_hook           },
    { "eglMakeCurrent",          (uintptr_t)&eglMakeCurrent_hook          },
    { "eglQueryContext",         (uintptr_t)&eglQueryContext_hook         },
};

const int egl_dynlib_count = sizeof(egl_dynlib) / sizeof(egl_dynlib[0]);
