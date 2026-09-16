/* jni_patch.c -- Fake Java runtime for GTA: Liberty City Stories
 *
 * Structurally different from the Chinatown Wars port's version, because the
 * two games boot in completely different ways:
 *
 *   CTW  : JNI_OnLoad -> RegisterNatives -> grab natives[1].fnPtr -> call it.
 *   LCS  : no RegisterNatives at all.  The engine exports named JNI entry
 *          points (Java_com_rockstargames_...), and the Java side calls them
 *          in a specific order.  We resolve them by symbol and drive that
 *          order ourselves — see jni_boot_lcs() at the bottom.
 *
 * The fake JNIEnv still matters, because native code calls *up* into Java for
 * two things: com/rockstargames/gtalcs/CommonAPI (video, playlist, loading
 * screens, Social Club, locale) and com/wardrumstudios/utils/WarGamepad (pad
 * state).  Those are the only up-call surfaces with JNI signature strings in
 * the binary — the other HAL classes are referenced but not called this way.
 *
 * SOFT-FLOAT, BOTH DIRECTIONS.  libGTALcs.so is soft-float; we are hard-float.
 *   - Calling INTO the engine with a float (viewOnDrawFrame, setJoyAxis,
 *     setAccelerometer) needs SOFTFP on the function-pointer type.
 *   - Returning a float UP to the engine (GetGamepadAxis) needs SOFTFP on
 *     CallFloatMethodV.
 * Getting either wrong yields plausible-but-wrong values, not a crash.
 */

/* pthread_setname_np is a GNU extension — without this it was implicitly
 * declared, so the compiler never checked the call. */
#define _GNU_SOURCE

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/input.h>

#include <pthread.h>
#include <SDL2/SDL.h>
#include <GLES2/gl2.h>

#include "config.h"
#include "so_util.h"
#include "egl_patch.h"
#include "jni_patch.h"

#define SOFTFP __attribute__((pcs("aapcs")))

extern so_module gtalcs_mod;
extern char g_data_path[512];

extern SDL_Window         *g_window;
extern SDL_GLContext       g_gl_ctx;
extern SDL_GameController *g_gamepad;
extern int                 g_gamepad_buttons;
extern float               g_gamepad_axis[6];   /* LX LY RX RY L2 R2 */

/* ── Up-call method IDs ──────────────────────────────────────────────────── */

enum MethodID {
    UNKNOWN = 0,
    /* CommonAPI */
    GET_DEVICE_LANGUAGE, GET_VERSION_NAME,
    PLAY_VIDEO, IS_VIDEO_PLAYING,
    PLAYLIST_INIT, PLAYLIST_PLAY, PLAYLIST_PAUSE,
    PLAYLIST_PLAYING, PLAYLIST_HAS_SONGS, GOOGLE_PLAYLIST_AVAILABLE,
    GO_TO_URL,
    SHOW_LOADING_SCREEN, HIDE_LOADING_SCREEN,
    SHOW_LOADING_BAR, SHOW_LOADING_BAR_PROGRESS_HACK,
    START_SOCIAL_CLUB, CALL_SOCIAL_CLUB_UPDATE, CALL_SOCIAL_CLUB_SIGN_OUT,
    QUIT_APP, REMOVE_FRAME_RATE_LOCK, SET_KEEP_SCREEN_ON, SET_IN_UI,
    /* WarGamepad */
    GET_GAMEPAD_TYPE, GET_GAMEPAD_BUTTONS, GET_GAMEPAD_AXIS, GET_GAMEPAD_TRACK,
    PROCESS_TOUCHPAD_AS_POINTER,
    /* android/view/ViewRoot */
    SET_PROCESS_POSITION_EVENTS,
    /* Observed during bring-up: HAL screen/view construction and Social Club */
    IS_NETWORK_REACHABLE, GET_FILE, RUN_ON_MAIN_THREAD, SET_STRING,
    CREATE_VIEW, CREATE_SCREEN, CREATE_LOADING_SCREEN,
    GET_TOTAL_MEMORY_BYTES, ADD_VIEW_TO_SCREEN, ADD_SUBVIEW, SET_BOUNDS,
    UNPACK_IMAGE, TO_IMAGE, LOAD_IMAGE_FROM_BYTES, GET_BYTE_DATA,
    GET_TEXT_WIDTH, GET_TEXT_HEIGHT, GET_CACHED_W, GET_CACHED_H,
    GET_STRING, READ_USER_FILE, GET_ACTIVITY, GET_SYSTEM_SERVICE,
    CREATE_SHAPE, RANDOM_UUID, GET_DHCP_INFO,

    /* Everything past here is handed out dynamically by GetMethodID for names
     * we have not mapped yet.  It must stay LAST. */
    DYNAMIC_ID_BASE,
};

static const struct { const char *name; int id; } method_map[] = {
    { "GetDeviceLanguage",          GET_DEVICE_LANGUAGE             },
    { "GetVersionName",             GET_VERSION_NAME                },
    { "PlayVideo",                  PLAY_VIDEO                      },
    { "isVideoPlaying",             IS_VIDEO_PLAYING                },
    { "PlaylistInit",               PLAYLIST_INIT                   },
    { "PlaylistPlay",               PLAYLIST_PLAY                   },
    { "PlaylistPause",              PLAYLIST_PAUSE                  },
    { "PlaylistPlaying",            PLAYLIST_PLAYING                },
    { "PlaylistHasSongs",           PLAYLIST_HAS_SONGS              },
    { "GooglePlaylistAvailable",    GOOGLE_PLAYLIST_AVAILABLE       },
    { "GoToUrl",                    GO_TO_URL                       },
    { "ShowLoadingScreen",          SHOW_LOADING_SCREEN             },
    { "HideLoadingScreen",          HIDE_LOADING_SCREEN             },
    { "ShowLoadingBar",             SHOW_LOADING_BAR                },
    { "ShowLoadingBarProgressHack", SHOW_LOADING_BAR_PROGRESS_HACK  },
    { "StartSocialClub",            START_SOCIAL_CLUB               },
    { "CallSocialClubUpdate",       CALL_SOCIAL_CLUB_UPDATE         },
    { "CallSocialClubSignOut",      CALL_SOCIAL_CLUB_SIGN_OUT       },
    { "QuitApp",                    QUIT_APP                        },
    { "RemoveFrameRateLock",        REMOVE_FRAME_RATE_LOCK          },
    { "setKeepScreenOn",            SET_KEEP_SCREEN_ON              },
    { "setInUI",                    SET_IN_UI                       },
    { "GetGamepadType",             GET_GAMEPAD_TYPE                },
    { "GetGamepadButtons",          GET_GAMEPAD_BUTTONS             },
    { "GetGamepadAxis",             GET_GAMEPAD_AXIS                },
    { "GetGamepadTrack",            GET_GAMEPAD_TRACK               },
    { "processTouchpadAsPointer",   PROCESS_TOUCHPAD_AS_POINTER     },
    { "setProcessPositionEvents",   SET_PROCESS_POSITION_EVENTS     },
    { "IsNetworkReachable",         IS_NETWORK_REACHABLE            },
    { "getFile",                    GET_FILE                        },
    { "runOnMainThread",            RUN_ON_MAIN_THREAD              },
    { "SetString",                  SET_STRING                      },
    { "createView",                 CREATE_VIEW                     },
    { "createScreen",               CREATE_SCREEN                   },
    { "createLoadingScreen",        CREATE_LOADING_SCREEN           },
    { "getTotalMemoryBytes",        GET_TOTAL_MEMORY_BYTES          },
    { "addViewToScreen",            ADD_VIEW_TO_SCREEN              },
    { "addSubview",                 ADD_SUBVIEW                     },
    { "setBounds",                  SET_BOUNDS                      },
    { "unpackImage",                UNPACK_IMAGE                    },
    { "toImage",                    TO_IMAGE                        },
    { "loadImageFromBytes",         LOAD_IMAGE_FROM_BYTES           },
    { "getByteData",                GET_BYTE_DATA                   },
    { "getTextWidth",               GET_TEXT_WIDTH                  },
    { "getTextHeight",              GET_TEXT_HEIGHT                 },
    { "getCachedW",                 GET_CACHED_W                    },
    { "getCachedH",                 GET_CACHED_H                    },
    { "GetString",                  GET_STRING                      },
    { "readUserFile",               READ_USER_FILE                  },
    { "getActivity",                GET_ACTIVITY                    },
    { "getSystemService",           GET_SYSTEM_SERVICE              },
    { "createShape",                CREATE_SHAPE                    },
    { "randomUUID",                 RANDOM_UUID                     },
    { "getDhcpInfo",                GET_DHCP_INFO                   },
};

/* ── fake_vm / fake_env ──────────────────────────────────────────────────── */

char fake_vm[0x1000];
char fake_env[0x1000];

static int  ret0(void) { return 0; }
static int  ret1(void) { return 1; }

static int jni_unimpl(void) {
    uintptr_t lr   = (uintptr_t)__builtin_return_address(0);
    uintptr_t base = (uintptr_t)fake_env;
    if (lr >= base && lr < base + sizeof(fake_env))
        fprintf(stderr, "JNI: unimplemented env slot at offset 0x%x\n",
                (unsigned)(lr - base - 4));
    else
        fprintf(stderr, "JNI: unimplemented call (LR=0x%08x)\n", (unsigned)lr);
    return 0;
}

static void fill_table_with_stub(void *table, size_t size) {
    uintptr_t *p = table;
    for (size_t i = 0; i < size / sizeof(uintptr_t); i++)
        p[i] = (uintptr_t)jni_unimpl;
}

/* ── Up-call implementations ─────────────────────────────────────────────── */

/* Non-NULL, distinct-per-name class tokens.  The engine only ever compares
 * these against NULL and hands them back to us, so the value is arbitrary —
 * but it must not be NULL, or the engine treats the class as missing. */
static void *FindClass(void *env, const char *name) {
    (void)env;
    static char  class_tokens[64];
    static char *seen[64];              /* OWNED copies — see below */
    static int   n = 0;
    if (!name) return &class_tokens[0];

    for (int i = 0; i < n; i++)
        if (strcmp(seen[i], name) == 0) return &class_tokens[i];

    /* Copy the name rather than storing the caller's pointer.
     *
     * The previous version kept `name` itself, which makes every later lookup
     * depend on the engine keeping that string alive and unmodified forever.
     * Class names are probably rodata literals today, but nothing guarantees
     * it, and the failure mode is silent: a freed or reused buffer makes the
     * strcmp above read stale memory and can hand back the WRONG class token.
     * 64 names, once each, is a few hundred bytes that are never freed. */
    if (n < 64) {
        char *copy = strdup(name);
        if (!copy) return &class_tokens[0];
        seen[n] = copy;
        return &class_tokens[n++];
    }
    fprintf(stderr, "JNI: FindClass table full (64) — reusing token for '%s'\n", name);
    return &class_tokens[0];
}

/* Names we have not implemented still need a NON-ZERO id.
 *
 * The engine's getClassAndMethod() treats a zero jmethodID as "class or method
 * missing" and calls halDebug::abort(), which is a deliberate `str r3,[r3]`
 * with r3=0 — an intentional segfault.  So returning UNKNOWN(0) for anything
 * the engine looks up turns a merely-unimplemented method into a hard abort
 * during boot.  Handing back a unique non-zero id instead lets the lookup
 * succeed; the call itself then lands in the default case of the Call*Method
 * family and harmlessly returns 0/NULL.
 *
 * Each unmapped name is logged exactly once, so the log is a precise worklist
 * of what the engine actually wants rather than a repeating flood. */
static const char *dyn_names[256];
static int         dyn_count = 0;

static int GetMethodID(void *env, void *cls, const char *name, const char *sig) {
    (void)env; (void)cls;
    for (int i = 0; i < (int)(sizeof(method_map)/sizeof(method_map[0])); i++)
        if (strcmp(name, method_map[i].name) == 0)
            return method_map[i].id;

    for (int i = 0; i < dyn_count; i++)
        if (strcmp(dyn_names[i], name) == 0)
            return DYNAMIC_ID_BASE + i;

    if (dyn_count < (int)(sizeof(dyn_names)/sizeof(dyn_names[0]))) {
        /* The engine passes string literals from its own rodata, which stays
         * mapped for the process lifetime, so keeping the pointer is safe. */
        dyn_names[dyn_count] = name;
        fprintf(stderr, "JNI: unmapped GetMethodID('%s', '%s') -> stub id %d\n",
                name, sig ? sig : "?", DYNAMIC_ID_BASE + dyn_count);
        return DYNAMIC_ID_BASE + dyn_count++;
    }
    return DYNAMIC_ID_BASE;   /* table full: still non-zero, still survivable */
}

/* ── Fake Java objects ───────────────────────────────────────────────────────
 * Same lesson as GetMethodID: the engine treats NULL as "the Java side failed"
 * and reacts by asserting or throwing.  Every object-returning up-call must
 * hand back something non-NULL.  These handles are opaque — the engine only
 * passes them back to us — so a unique dummy address per call is enough, and
 * far cheaper than modelling andView/andImage for real.  Behaviour comes later
 * if a screen actually needs it. */
static char  obj_pool[4096];
static int   obj_next = 0;

static void *fake_object(void) {
    if (obj_next >= (int)sizeof(obj_pool)) obj_next = 0;   /* wrap; still non-NULL */
    return &obj_pool[obj_next++];
}

/* ── Fake jbyteArray ─────────────────────────────────────────────────────────
 * The engine asks Java for file contents as a byte[] (getFile), then reads it
 * back through GetArrayLength / GetByteArrayElements.  Both ends are ours, so
 * the "array" is just a length-prefixed buffer we hand out as an opaque
 * jbyteArray and interpret again on the way back. */
typedef struct { int len; unsigned char data[]; } fake_array;

/* Lifetime.  On Android the VM owns a jbyteArray once created and frees it on
 * GC; GetByteArrayElements hands back a view the caller may keep reading, and
 * Release hands ownership back without invalidating the object.  The engine
 * follows that contract: it can Release the same array twice, and it can read
 * elements again after a Release.  Freeing in Release — as an earlier revision
 * did — is a double-free on the first repeat and a use-after-free on every
 * re-read.  Both were measured (glibc: "malloc_consolidate(): unaligned
 * fastbin chunk"; with MALLOC_PERTURB_: a 0xA5 read in getClassCached).
 *
 * So arrays live in a bounded live-set instead: Release frees at most once per
 * array (repeat Releases are no-ops), reads-after-Release stay valid, and if
 * the engine never Releases at all, the oldest arrays are evicted when the set
 * fills — a miniature GC with a hard memory bound. */
#define FAKE_ARR_MAX 512
static fake_array *g_live[FAKE_ARR_MAX];
static int         g_live_count;

static void fake_array_retire(fake_array *a) {
    for (int i = 0; i < FAKE_ARR_MAX; i++) {
        if (g_live[i] != a) continue;
        g_live[i] = NULL;             /* hole; filled by the next add */
        g_live_count--;
        free(a);
        return;
    }
    /* Not live: an already-released (or foreign) handle — ignore. */
}

static fake_array *fake_array_from_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }

    fake_array *a = malloc(sizeof(fake_array) + (size_t)n);
    if (!a) { fclose(f); return NULL; }
    a->len = (int)n;
    size_t got = fread(a->data, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { free(a); return NULL; }

    if (g_live_count == FAKE_ARR_MAX) {       /* GC: evict the oldest survivor */
        int i = 0;
        while (!g_live[i]) i++;
        fake_array_retire(g_live[i]);
    }
    int i = 0;
    while (g_live[i]) i++;                    /* any hole; none guaranteed */
    g_live[i] = a;
    g_live_count++;
    return a;
}

/* getFile(String, String, String) -> byte[]
 *
 * Confirmed from the engine's own call during bring-up:
 *     args = ("json", "socialClubAssets", "json")
 * i.e. (directory, basename, extension) relative to the APK's assets/ root,
 * naming assets/json/socialClubAssets.json.  Everything the Social Club HAL
 * loads comes through here, so a NULL return leaves its resource maps empty
 * and the first image lookup asserts. */
static fake_array *hal_get_file(const char *a0, const char *a1, const char *a2) {
    char path[1024];
    const char *root = ASSETS_PATH;

    if (!a0 || !a1) return NULL;

    if (a2 && *a2)
        snprintf(path, sizeof(path), "%s/%s/%s.%s", root, a0, a1, a2);
    else
        snprintf(path, sizeof(path), "%s/%s/%s", root, a0, a1);

    fake_array *r = fake_array_from_file(path);
    if (r) {
        fprintf(stderr, "[getFile] %s (%d bytes)\n", path, r->len);
        return r;
    }
    fprintf(stderr, "[getFile] MISSING: %s  (args '%s' | '%s' | '%s')\n",
            path, a0, a1, a2 ? a2 : "(null)");
    return NULL;
}

static void CallVoidMethodV(void *env, void *obj, int id, uintptr_t *args) {
    (void)env; (void)obj; (void)args;
    switch (id) {
    case QUIT_APP:
        /* _exit, NOT exit.
         *
         * exit() runs the static destructors libGTALcs.so registered through
         * __cxa_atexit, and those hand bionic-allocated pointers to glibc's
         * free() — which aborted the process with "munmap_chunk(): invalid
         * pointer" every time the player quit.  Nothing in the .so's teardown
         * is worth running: the process is going away, the OS reclaims
         * everything, and the launcher's trap restores the console.
         *
         * SDL_Quit() still runs first, because it hands the DRM/KMS mode back;
         * skipping it leaves the panel in the game's mode.  The line after it
         * exists to attribute the abort if one ever returns here — if it does
         * not print, SDL_Quit itself is the culprit and teardown has to move
         * out of this call and into the frame loop instead. */
        fprintf(stderr, "JNI: QuitApp — exiting\n");
        fflush(NULL);
        SDL_Quit();
        fprintf(stderr, "JNI: QuitApp — SDL_Quit returned, _exit(0)\n");
        fflush(NULL);
        _exit(0);
        break;
    /* Deliberate no-ops: loading screens and the frame-rate lock are Android
     * UI concerns, and the video path is skipped entirely (see IS_VIDEO_PLAYING). */
    case PLAY_VIDEO: case SHOW_LOADING_SCREEN: case HIDE_LOADING_SCREEN:
    case SHOW_LOADING_BAR: case SHOW_LOADING_BAR_PROGRESS_HACK:
    case REMOVE_FRAME_RATE_LOCK: case SET_KEEP_SCREEN_ON: case SET_IN_UI:
    case PLAYLIST_INIT: case PLAYLIST_PLAY: case PLAYLIST_PAUSE:
    case GO_TO_URL: case START_SOCIAL_CLUB:
    case CALL_SOCIAL_CLUB_UPDATE: case CALL_SOCIAL_CLUB_SIGN_OUT:
    case SET_PROCESS_POSITION_EVENTS:
    default:
        break;
    }
}

static int CallBooleanMethodV(void *env, void *obj, int id, uintptr_t *args) {
    (void)env; (void)obj; (void)args;
    switch (id) {
    /* False, so the engine proceeds straight past the intro movie instead of
     * waiting for a playback that will never finish. */
    case IS_VIDEO_PLAYING:           return 0;
    /* No user music library: the radio runs through the engine's own OpenAL
     * streaming, not Android's media player. */
    case PLAYLIST_PLAYING:           return 0;
    case PLAYLIST_HAS_SONGS:         return 0;
    case GOOGLE_PLAYLIST_AVAILABLE:  return 0;
    case PROCESS_TOUCHPAD_AS_POINTER:return 0;
    /* No network: pushes Social Club down its offline path instead of leaving
     * it waiting on a request that will never complete. */
    case IS_NETWORK_REACHABLE:       return 0;
    default:                         return 0;
    }
}

static int GetGamepadType(int port)    { return (port == 0 && g_gamepad) ? 1 : 0; }
static int GetGamepadButtons(int port) { return (port == 0) ? g_gamepad_buttons : 0; }

static int CallIntMethodV(void *env, void *obj, int id, uintptr_t *args) {
    (void)env; (void)obj;
    switch (id) {
    case GET_GAMEPAD_TYPE:    return GetGamepadType((int)args[0]);
    case GET_GAMEPAD_BUTTONS: return GetGamepadButtons((int)args[0]);
    case GET_GAMEPAD_TRACK:   return 0;   /* touchpad track delta — no touchpad */
    /* Text/layout metrics.  Returning 0 risks divide-by-zero in layout code, so
     * report small plausible values until real text measurement exists. */
    case GET_TEXT_WIDTH:      return 64;
    case GET_TEXT_HEIGHT:     return 16;
    case GET_CACHED_W:        return SCREEN_W;
    case GET_CACHED_H:        return SCREEN_H;
    case CREATE_SHAPE:        return 1;    /* non-zero shape handle */
    default:                  return 0;
    }
}

/* Float RETURN to soft-float caller: the value must land in r0, not s0. */
static SOFTFP float CallFloatMethodV(void *env, void *obj, int id, uintptr_t *args) {
    (void)env; (void)obj;
    switch (id) {
    case GET_GAMEPAD_AXIS: {
        int axis = (int)args[1];
        if (axis < 0 || axis > 5) return 0.0f;
        return g_gamepad_axis[axis];
    }
    default: return 0.0f;
    }
}

static void *CallObjectMethodV(void *env, void *obj, int id, uintptr_t *args) {
    (void)env; (void)obj; (void)args;
    switch (id) {
    case GET_DEVICE_LANGUAGE: return (void *)"en";
    case GET_VERSION_NAME:    return (void *)"2.4";
    case GET_FILE:            return hal_get_file((const char *)args[0],
                                                  (const char *)args[1],
                                                  (const char *)args[2]);
    case GET_STRING:          return (void *)"";
    case READ_USER_FILE:      return (void *)"";
    /* Views, images, activities, services: opaque non-NULL handles. */
    case CREATE_VIEW: case CREATE_SCREEN:
    case UNPACK_IMAGE: case TO_IMAGE: case LOAD_IMAGE_FROM_BYTES:
    case GET_ACTIVITY: case GET_SYSTEM_SERVICE:
    case RANDOM_UUID:  case GET_DHCP_INFO:
    case GET_BYTE_DATA:
                              return fake_object();
    default:                  return NULL;
    }
}

/* getTotalMemoryBytes()J and friends return a 64-bit value; AAPCS puts it in
 * r0:r1, which a plain long long return already does. */
static long long CallLongMethodV(void *env, void *obj, int id, uintptr_t *args) {
    (void)env; (void)obj; (void)args;
    switch (id) {
    default: return (long long)DEVICE_MEMORY_MB * 1024 * 1024;
    }
}

/* The engine calls both the varargs and the va_list forms.  AAPCS spreads
 * varargs across r3 + stack, so they cannot be read as a flat array directly —
 * marshal them into one here and delegate to the V implementation.  Reading a
 * fixed 8 words is the usual practice in these loaders: surplus reads are
 * harmless, and no up-call in this engine takes more. */
#define VARARG_MARSHAL(ap, arr)                       \
    do { va_start(ap, id);                            \
         for (int _i = 0; _i < 8; _i++)               \
             (arr)[_i] = va_arg(ap, uintptr_t);       \
         va_end(ap); } while (0)

static void CallVoidMethod(void *env, void *obj, int id, ...) {
    va_list ap; uintptr_t a[8]; VARARG_MARSHAL(ap, a);
    CallVoidMethodV(env, obj, id, a);
}
static int CallBooleanMethod(void *env, void *obj, int id, ...) {
    va_list ap; uintptr_t a[8]; VARARG_MARSHAL(ap, a);
    return CallBooleanMethodV(env, obj, id, a);
}
static int CallIntMethod(void *env, void *obj, int id, ...) {
    va_list ap; uintptr_t a[8]; VARARG_MARSHAL(ap, a);
    return CallIntMethodV(env, obj, id, a);
}
static long long CallLongMethod(void *env, void *obj, int id, ...) {
    va_list ap; uintptr_t a[8]; VARARG_MARSHAL(ap, a);
    return CallLongMethodV(env, obj, id, a);
}
static void *CallObjectMethod(void *env, void *obj, int id, ...) {
    va_list ap; uintptr_t a[8]; VARARG_MARSHAL(ap, a);
    return CallObjectMethodV(env, obj, id, a);
}
static SOFTFP float CallFloatMethod(void *env, void *obj, int id, ...) {
    va_list ap; uintptr_t a[8]; VARARG_MARSHAL(ap, a);
    return CallFloatMethodV(env, obj, id, a);
}

static int GetArrayLength(void *env, fake_array *a) {
    (void)env; return a ? a->len : 0;
}
static void *GetByteArrayElements(void *env, fake_array *a, int *isCopy) {
    (void)env; if (isCopy) *isCopy = 0; return a ? a->data : NULL;
}
static void ReleaseByteArrayElements(void *env, fake_array *a, void *elems, int mode) {
    (void)env; (void)elems; (void)mode;
    fake_array_retire(a);    /* idempotent; reads-after-release stay valid */
}
static void GetByteArrayRegion(void *env, fake_array *a, int start, int len, void *buf) {
    (void)env;
    if (!a || !buf || start < 0 || len < 0 || start + len > a->len) return;
    memcpy(buf, a->data + start, (size_t)len);
}

/* Strings: we hand the engine plain C pointers and take them back unchanged. */
static char *NewStringUTF(void *env, char *bytes)                 { (void)env; return bytes; }
static char *GetStringUTFChars(void *env, char *str, int *isCopy) { (void)env; if (isCopy) *isCopy = 0; return str; }
static int   GetStringUTFLength(void *env, char *str)             { (void)env; return str ? (int)strlen(str) : 0; }
static void *NewGlobalRef(void *env, void *obj)                   { (void)env; return obj ? obj : (void *)0x42424242; }

/* AttachCurrentThread MUST write *env.
 *
 * Wiring this to ret0 "succeeds" while leaving the caller's JNIEnv* NULL, and
 * the engine then dereferences it to reach the vtable.  That is what killed
 * viewOnInit: the renderer runs on a different thread from the UI, the engine
 * attaches it, gets NULL back, and faults inside its own
 * _JNIEnv::CallStaticObjectMethod wrapper with r0=0.
 *
 * There is only one JNIEnv here — we have no real VM and no per-thread state —
 * so every thread attaches to the same fake_env. */
static int AttachCurrentThread(void *vm, void **env, void *args) {
    (void)vm; (void)args;
    if (env) *env = fake_env;
    return 0;
}

static int GetEnv(void *vm, void **env, int version) {
    (void)vm; (void)version;
    *env = fake_env;
    return 0;
}

void jni_init(void) {
    fill_table_with_stub(fake_vm,  sizeof(fake_vm));
    fill_table_with_stub(fake_env, sizeof(fake_env));

    *(uintptr_t *)(fake_vm + 0x00) = (uintptr_t)fake_vm;
    *(uintptr_t *)(fake_vm + 0x0C) = (uintptr_t)ret0;      /* DestroyJavaVM */
    *(uintptr_t *)(fake_vm + 0x10) = (uintptr_t)AttachCurrentThread;
    *(uintptr_t *)(fake_vm + 0x14) = (uintptr_t)ret0;      /* DetachCurrentThread */
    *(uintptr_t *)(fake_vm + 0x18) = (uintptr_t)GetEnv;
    *(uintptr_t *)(fake_vm + 0x1C) = (uintptr_t)AttachCurrentThread; /* AsDaemon */

    /* Standard JNIEnv vtable offsets (32-bit). */
    *(uintptr_t *)(fake_env + 0x00)  = (uintptr_t)fake_env;
    *(uintptr_t *)(fake_env + 0x18)  = (uintptr_t)FindClass;
    *(uintptr_t *)(fake_env + 0x3C)  = (uintptr_t)ret0;    /* ExceptionOccurred */
    *(uintptr_t *)(fake_env + 0x40)  = (uintptr_t)ret0;    /* ExceptionDescribe */
    *(uintptr_t *)(fake_env + 0x44)  = (uintptr_t)ret0;    /* ExceptionClear */
    *(uintptr_t *)(fake_env + 0x54)  = (uintptr_t)NewGlobalRef;
    *(uintptr_t *)(fake_env + 0x58)  = (uintptr_t)ret0;    /* DeleteGlobalRef */
    *(uintptr_t *)(fake_env + 0x5C)  = (uintptr_t)ret0;    /* DeleteLocalRef */
    *(uintptr_t *)(fake_env + 0x84)  = (uintptr_t)GetMethodID;
    *(uintptr_t *)(fake_env + 0x88)  = (uintptr_t)CallObjectMethod;
    *(uintptr_t *)(fake_env + 0x8C)  = (uintptr_t)CallObjectMethodV;
    *(uintptr_t *)(fake_env + 0x94)  = (uintptr_t)CallBooleanMethod;
    *(uintptr_t *)(fake_env + 0x98)  = (uintptr_t)CallBooleanMethodV;
    *(uintptr_t *)(fake_env + 0xC4)  = (uintptr_t)CallIntMethod;
    *(uintptr_t *)(fake_env + 0xC8)  = (uintptr_t)CallIntMethodV;
    *(uintptr_t *)(fake_env + 0xD0)  = (uintptr_t)CallLongMethod;
    *(uintptr_t *)(fake_env + 0xD4)  = (uintptr_t)CallLongMethodV;
    *(uintptr_t *)(fake_env + 0xDC)  = (uintptr_t)CallFloatMethod;
    *(uintptr_t *)(fake_env + 0xE0)  = (uintptr_t)CallFloatMethodV;
    *(uintptr_t *)(fake_env + 0xF4)  = (uintptr_t)CallVoidMethod;
    *(uintptr_t *)(fake_env + 0xF8)  = (uintptr_t)CallVoidMethodV;
    *(uintptr_t *)(fake_env + 0x1C4) = (uintptr_t)GetMethodID;  /* GetStaticMethodID */
    *(uintptr_t *)(fake_env + 0x1C8) = (uintptr_t)CallObjectMethod;
    *(uintptr_t *)(fake_env + 0x1CC) = (uintptr_t)CallObjectMethodV;
    *(uintptr_t *)(fake_env + 0x1D4) = (uintptr_t)CallBooleanMethod;
    *(uintptr_t *)(fake_env + 0x1D8) = (uintptr_t)CallBooleanMethodV;
    *(uintptr_t *)(fake_env + 0x214) = (uintptr_t)CallLongMethodV;   /* static */
    *(uintptr_t *)(fake_env + 0x204) = (uintptr_t)CallIntMethod;
    *(uintptr_t *)(fake_env + 0x208) = (uintptr_t)CallIntMethodV;
    *(uintptr_t *)(fake_env + 0x21C) = (uintptr_t)CallFloatMethod;
    *(uintptr_t *)(fake_env + 0x220) = (uintptr_t)CallFloatMethodV;
    *(uintptr_t *)(fake_env + 0x234) = (uintptr_t)CallVoidMethod;
    *(uintptr_t *)(fake_env + 0x238) = (uintptr_t)CallVoidMethodV;
    *(uintptr_t *)(fake_env + 0x29C) = (uintptr_t)NewStringUTF;
    *(uintptr_t *)(fake_env + 0x2A0) = (uintptr_t)GetStringUTFLength;
    *(uintptr_t *)(fake_env + 0x2A4) = (uintptr_t)GetStringUTFChars;
    *(uintptr_t *)(fake_env + 0x2A8) = (uintptr_t)ret0;    /* ReleaseStringUTFChars */
    /* Array accessors — the byte[] that getFile() returns comes back through
     * these.  Offsets are the standard JNINativeInterface indices x4. */
    *(uintptr_t *)(fake_env + 0x2AC) = (uintptr_t)GetArrayLength;
    *(uintptr_t *)(fake_env + 0x2E0) = (uintptr_t)GetByteArrayElements;
    *(uintptr_t *)(fake_env + 0x300) = (uintptr_t)ReleaseByteArrayElements;
    *(uintptr_t *)(fake_env + 0x320) = (uintptr_t)GetByteArrayRegion;
}

/* ══════════════════════════════════════════════════════════════════════════
 * LCS boot: resolve the engine's exported JNI natives and drive them in the
 * order GTAActivityBase/GTAGLview/ActivityWrapper use on Android.
 * ══════════════════════════════════════════════════════════════════════════ */

#define JNI_PFX "Java_com_rockstargames_gtalcs_GTAJNIlib_"
#define HAL_PFX "Java_com_rockstargames_hal_ActivityWrapper_"

/* Every JNI native takes (JNIEnv*, jclass) first. */
typedef void  (*fn_v)      (void *, void *);
typedef void  (*fn_i)      (void *, void *, int);
typedef void  (*fn_ii)     (void *, void *, int, int);
typedef void  (*fn_p)      (void *, void *, void *);
typedef void  (*fn_pp)     (void *, void *, void *, void *);
typedef void  (*fn_ipss)   (void *, void *, int, void *, void *, void *);
/* Soft-float: the float argument travels in a GP register. */
typedef SOFTFP void (*fn_f)   (void *, void *, float);
typedef SOFTFP void (*fn_iif) (void *, void *, int, int, float);

/* ── Touch: call AND_TouchEvent directly, as the CTW port does ─────────────
 *
 * The JNI wrappers (onTouchStart/Move/End) take NORMALISED floats and merely
 * multiply by the screen size before tail-calling
 * `AND_TouchEvent(action, pointerId, x_px, y_px)`.  Going straight to
 * AND_TouchEvent skips a soft-float boundary for no loss — and it is the route
 * gtacw-port proved on this exact device class.
 *
 * The action codes are NOT the same as Chinatown Wars'.  Read off the tail
 * calls in each wrapper:
 *      onTouchStart -> AND_TouchEvent(2, ...)      DOWN
 *      onTouchEnd   -> AND_TouchEvent(1, ...)      UP
 *      onTouchMove  -> AND_TouchEvent(3, ...)      MOVE
 * (CTW used 0/1/2 — copying its constants would silently send the wrong
 * action for every event.) */
#define LCS_TOUCH_UP    1
#define LCS_TOUCH_DOWN  2
#define LCS_TOUCH_MOVE  3

typedef int (*fn_and_touch)(int action, int pointer, int x, int y);
static fn_and_touch g_AND_TouchEvent;

/* Kept as a fallback only if the internal symbol ever goes missing. */
typedef void (*fn_touch)(void *, void *, int, uint32_t, uint32_t);
static uint32_t f2bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static fn_touch g_onTouchStart, g_onTouchMove, g_onTouchEnd;

/* Screen size the engine was told about; touch coordinates are in these
 * pixels, so both the touchscreen and the virtual cursor scale into it. */
static int g_screen_w = SCREEN_W, g_screen_h = SCREEN_H;

static void touch_raw(int action, int pointer, int x, int y) {
    if (x < 0) x = 0; else if (x >= g_screen_w) x = g_screen_w - 1;
    if (y < 0) y = 0; else if (y >= g_screen_h) y = g_screen_h - 1;

    if (g_AND_TouchEvent) { g_AND_TouchEvent(action, pointer, x, y); return; }

    /* Fallback through the JNI wrappers, which want 0..1 floats. */
    float nx = (float)x / (float)g_screen_w;
    float ny = (float)y / (float)g_screen_h;
    fn_touch fn = (action == LCS_TOUCH_DOWN) ? g_onTouchStart
                : (action == LCS_TOUCH_UP)   ? g_onTouchEnd
                                             : g_onTouchMove;
    if (fn) fn(fake_env, NULL, pointer, f2bits(nx), f2bits(ny));
}

/* Virtual pointer for the touch frontend, driven by the pad: stick/d-pad
 * moves it, A taps.  Pointer id 0 is shared with the real touchscreen, which
 * is fine — only one of them is in use at a time in practice. */
static float g_cursor_x = 0.5f, g_cursor_y = 0.5f;   /* normalised */
static int   g_touch_down;

static void touch_send_norm(int action, float nx, float ny) {
    touch_raw(action, 0, (int)(nx * (float)g_screen_w),
                          (int)(ny * (float)g_screen_h));
}


/* ── Gamepad button numbering ──────────────────────────────────────────────
 *
 * The engine's index is NOT SDL's enum order, and assuming it was is why the
 * pad felt scrambled: SDL's A/B/X/Y happen to line up at 0-3 and then
 * everything diverges (SDL BACK=4 landed on the engine's START, SDL START=6
 * on L1, and the whole d-pad shifted by three).
 *
 * The authoritative table is in the APK, not the .so:
 * `GTAGLview.getJoypadButtonFromKeyCode(int)` is a sparse-switch from Android
 * keycodes to the index handed to onJoyButtonDown/Up.  Decoded from
 * classes.dex:
 *
 *   BUTTON_A 96 -> 0    BUTTON_L1 102 -> 6     DPAD_LEFT  21 -> 10
 *   BUTTON_B 97 -> 1    BUTTON_R1 103 -> 7     DPAD_RIGHT 22 -> 11
 *   BUTTON_X 99 -> 2    DPAD_UP    19 -> 8     THUMBL 106 -> 12
 *   BUTTON_Y 100-> 3    DPAD_DOWN  20 -> 9     THUMBR 107 -> 13
 *   START 108 -> 4  (MODE 110 also 4)          BACK     4 -> 14
 *                                              MENU    82 -> 15
 *
 * Index 5 is unused by that mapping.  onJoyButtonUp clamps with `cmp r3,#15`
 * and writes joypadButtons[index], so anything above 15 is silently dropped.
 * Triggers are axes, not buttons — they never appear here. */
/* A/B: POSITIONAL by default (SDL's naming — A = south, B = east), which is
 * what the porter wants on this device.
 *
 * The alternative reading is label-based: these handhelds are often silkscreened
 * Nintendo-style (A = east, B = south) while the engine's index 0 is Android's
 * BUTTON_A, so a label-matching mapping needs the two exchanged. That was the
 * default here for a while; it is now opt-in.
 *
 * GTALCS_SWAP_AB=1 exchanges them (conf/swap_ab.txt in the launcher). Note this
 * affects only what the ENGINE receives — the menu pointer's tap is bound to the
 * physical SDL "A" separately, so menus behave the same either way. */
static int swap_ab(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("GTALCS_SWAP_AB");
        v = (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
    }
    return v;
}

/* Shoulders/triggers exchanged: the player's L2 drives what the engine calls
 * L1, and the player's L1 drives the engine's brake — same on the right.
 *
 * This is not a table edit, because the two live on different sides of the
 * engine's input API: L1/R1 are BUTTONS (indices 6 and 7, from the dex table
 * above) while L2/R2 are AXES (4 = LTRIGGER/BRAKE, 5 = RTRIGGER/GAS). So the
 * swap is done at the source — the button bitmask takes its L1/R1 bits from the
 * trigger axes, and axes 4/5 take their value from the shoulder buttons.
 *
 * GTALCS_SWAP_SHOULDERS=0 turns it off (conf/swap_shoulders.txt). */
static int swap_shoulders(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("GTALCS_SWAP_SHOULDERS");
        v = (e && *e && strcmp(e, "0") == 0) ? 0 : 1;
    }
    return v;
}

/* Which physical button taps the virtual menu pointer.
 *
 * This must NOT be a button the player holds during gameplay.  It was hard-wired
 * to SDL_CONTROLLER_BUTTON_A, which on a Nintendo-labelled handheld is the
 * physical B button — so as soon as A/B were mapped positionally, "accelerate"
 * and "tap" landed on the same button and holding it to drive also held a touch
 * down on the frontend.  That looked exactly like accelerate not working, and it
 * was not a mapping bug at all.
 *
 * Default is SDL_CONTROLLER_BUTTON_B, i.e. the button silkscreened "A" on these
 * devices — the natural confirm button in menus, and not the accelerator.
 * GTALCS_TAP_BUTTON takes an SDL button name ("a", "b", "rightstick", "back", …)
 * for a pad that is labelled the other way round. */
static int tap_button(void) {
    static int v = -1;
    if (v < 0) {
        v = SDL_CONTROLLER_BUTTON_B;
        const char *e = getenv("GTALCS_TAP_BUTTON");
        if (e && *e) {
            SDL_GameControllerButton b = SDL_GameControllerGetButtonFromString(e);
            if (b != SDL_CONTROLLER_BUTTON_INVALID) v = (int)b;
            else fprintf(stderr, "[input] GTALCS_TAP_BUTTON='%s' is not an SDL "
                                 "button name — using 'b'\n", e);
        }
        fprintf(stderr, "[input] menu tap button = SDL %s\n",
                SDL_GameControllerGetStringForButton((SDL_GameControllerButton)v));
    }
    return v;
}

/* A trigger counts as "pressed" past a quarter travel. Handhelds whose L2/R2
 * are microswitches report only 0 or full scale, so the exact value matters
 * only on pads with real analogue triggers. */
#define TRIGGER_PRESS 0.25f

static int sdl_button_to_engine(int b) {
    switch (b) {
    case SDL_CONTROLLER_BUTTON_A:             return swap_ab() ? 1 : 0;
    case SDL_CONTROLLER_BUTTON_B:             return swap_ab() ? 0 : 1;
    case SDL_CONTROLLER_BUTTON_X:             return 2;
    case SDL_CONTROLLER_BUTTON_Y:             return 3;
    case SDL_CONTROLLER_BUTTON_START:         return 4;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  return 6;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return 7;
    case SDL_CONTROLLER_BUTTON_DPAD_UP:       return 8;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:     return 9;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:     return 10;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:    return 11;
    case SDL_CONTROLLER_BUTTON_LEFTSTICK:     return 12;
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK:    return 13;
    case SDL_CONTROLLER_BUTTON_BACK:          return 14;   /* Android BACK */
    case SDL_CONTROLLER_BUTTON_GUIDE:         return 15;   /* Android MENU */
    default:                                  return -1;
    }
}

/* ── Real touchscreen (evdev multitouch type B) ────────────────────────────
 *
 * The RG353V has a "Hynitron cst3xx Touchscreen" on /dev/input/event1, and the
 * whole LCS frontend is the Android touch frontend — so without this the panel
 * simply does nothing.  Ported from gtacw-port's reader, with two changes:
 * the device is discovered by capability rather than hardcoded, and the axis
 * ranges come from EVIOCGABS instead of being assumed equal to the screen.
 *
 * GTALCS_TOUCH_DEV overrides the device path. */
#define MAX_TOUCH_SLOTS 5

typedef struct { int tracking_id, x, y, prev_active, dirty; } touch_slot_t;
static touch_slot_t g_slots[MAX_TOUCH_SLOTS];
static int g_cur_slot;
static int g_touch_fd = -1;
static int g_tx_min, g_tx_max, g_ty_min, g_ty_max;

static int touch_try_open(const char *path) {
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return -1;

    unsigned long absbits[(ABS_MAX + 8 * sizeof(long)) / (8 * sizeof(long))];
    memset(absbits, 0, sizeof(absbits));
    if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits) < 0 ||
        !(absbits[ABS_MT_POSITION_X / (8 * sizeof(long))] &
          (1UL << (ABS_MT_POSITION_X % (8 * sizeof(long)))))) {
        close(fd);
        return -1;                       /* not a multitouch device */
    }

    struct input_absinfo ai;
    g_tx_min = 0; g_tx_max = SCREEN_W - 1;
    g_ty_min = 0; g_ty_max = SCREEN_H - 1;
    if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &ai) == 0 && ai.maximum > ai.minimum)
        { g_tx_min = ai.minimum; g_tx_max = ai.maximum; }
    if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &ai) == 0 && ai.maximum > ai.minimum)
        { g_ty_min = ai.minimum; g_ty_max = ai.maximum; }

    char name[128] = "?";
    ioctl(fd, EVIOCGNAME(sizeof(name)), name);
    fprintf(stderr, "[touch] %s: \"%s\" x=%d..%d y=%d..%d -> %dx%d\n",
            path, name, g_tx_min, g_tx_max, g_ty_min, g_ty_max,
            g_screen_w, g_screen_h);
    return fd;
}

static void touchscreen_init(void) {
    for (int i = 0; i < MAX_TOUCH_SLOTS; i++) g_slots[i].tracking_id = -1;

    const char *env = getenv("GTALCS_TOUCH_DEV");
    if (env && *env) {
        g_touch_fd = touch_try_open(env);
        if (g_touch_fd < 0)
            fprintf(stderr, "[touch] GTALCS_TOUCH_DEV=%s unusable: %s\n",
                    env, strerror(errno));
        return;
    }
    for (int i = 0; i < 32 && g_touch_fd < 0; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        g_touch_fd = touch_try_open(path);
    }
    if (g_touch_fd < 0)
        fprintf(stderr, "[touch] no multitouch device found "
                        "(menus still work via the pad cursor)\n");
}

static int touch_scale(int v, int lo, int hi, int out) {
    if (hi <= lo) return v;
    long r = (long)(v - lo) * (out - 1) / (hi - lo);
    return (int)(r < 0 ? 0 : (r >= out ? out - 1 : r));
}

static void touchscreen_poll(void) {
    if (g_touch_fd < 0) return;
    struct input_event ev;
    while (read(g_touch_fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
        if (ev.type == EV_ABS) {
            switch (ev.code) {
            case ABS_MT_SLOT:
                if ((unsigned)ev.value < MAX_TOUCH_SLOTS) g_cur_slot = ev.value;
                break;
            case ABS_MT_TRACKING_ID:
                g_slots[g_cur_slot].tracking_id = ev.value;
                break;
            case ABS_MT_POSITION_X:
                g_slots[g_cur_slot].x = touch_scale(ev.value, g_tx_min, g_tx_max, g_screen_w);
                g_slots[g_cur_slot].dirty = 1;
                break;
            case ABS_MT_POSITION_Y:
                g_slots[g_cur_slot].y = touch_scale(ev.value, g_ty_min, g_ty_max, g_screen_h);
                g_slots[g_cur_slot].dirty = 1;
                break;
            }
        } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
            for (int i = 0; i < MAX_TOUCH_SLOTS; i++) {
                int active = (g_slots[i].tracking_id != -1);
                if (active && !g_slots[i].prev_active)
                    touch_raw(LCS_TOUCH_DOWN, i, g_slots[i].x, g_slots[i].y);
                else if (!active && g_slots[i].prev_active)
                    touch_raw(LCS_TOUCH_UP,   i, g_slots[i].x, g_slots[i].y);
                else if (active && g_slots[i].dirty)
                    touch_raw(LCS_TOUCH_MOVE, i, g_slots[i].x, g_slots[i].y);
                g_slots[i].prev_active = active;
                g_slots[i].dirty       = 0;
            }
        }
    }
}

static void *sym(const char *name) {
    void *p = (void *)so_symbol(&gtalcs_mod, name);
    if (!p) fprintf(stderr, "[jni] MISSING native: %s\n", name);
    return p;
}

/* Runs Java_com_rockstargames_hal_ActivityWrapper_main, which never returns. */
static void *ui_thread_main(void *arg) {
    fn_v main_fn = (fn_v)arg;
    fprintf(stderr, "[jni] UI thread: entering ActivityWrapper_main\n"); fflush(stderr);
    main_fn(fake_env, NULL);
    fprintf(stderr, "[jni] UI thread: ActivityWrapper_main RETURNED\n"); fflush(stderr);
    return NULL;
}

static void poll_input(fn_iif setJoyAxis, fn_ii onJoyDown, fn_ii onJoyUp) {
    static int prev_buttons = 0;
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) { SDL_Quit(); exit(0); }
    }
    if (!g_gamepad) return;

    /* Axes, normalised to -1..1 with a deadzone. */
    static const SDL_GameControllerAxis axmap[6] = {
        SDL_CONTROLLER_AXIS_LEFTX,  SDL_CONTROLLER_AXIS_LEFTY,
        SDL_CONTROLLER_AXIS_RIGHTX, SDL_CONTROLLER_AXIS_RIGHTY,
        SDL_CONTROLLER_AXIS_TRIGGERLEFT, SDL_CONTROLLER_AXIS_TRIGGERRIGHT,
    };
    /* GTALCS_AUTOWALK=<ms>: from <ms> after start, hold the left stick fully
     * forward.  There is no way to press a button on the device from here, so
     * this is how a headless run proves the in-game control path actually
     * reaches the engine — if the character walks, the framebuffer changes. */
    static int autowalk_ms = -1;
    if (autowalk_ms < 0) {
        const char *e = getenv("GTALCS_AUTOWALK");
        autowalk_ms = (e && *e) ? atoi(e) : 0;
        if (autowalk_ms > 0)
            fprintf(stderr, "[input] AUTOWALK: left stick forward from %d ms\n",
                    autowalk_ms);
    }
    int autowalk = autowalk_ms > 0 && (int)SDL_GetTicks() >= autowalk_ms;

    /* One-time report of what this pad actually exposes. If the triggers are
     * unbound, SWAP_SHOULDERS leaves the engine's L1/R1 dead, and that is
     * something to see in the log rather than to guess at from the couch. */
    static int bind_logged;
    if (!bind_logged) {
        bind_logged = 1;
        SDL_GameControllerButtonBind lt =
            SDL_GameControllerGetBindForAxis(g_gamepad, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
        SDL_GameControllerButtonBind rt =
            SDL_GameControllerGetBindForAxis(g_gamepad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
        fprintf(stderr, "[input] swap_ab=%d swap_shoulders=%d; "
                        "trigger binds L2=%d R2=%d (0 = NOT REPORTED by this pad)\n",
                swap_ab(), swap_shoulders(), lt.bindType, rt.bindType);
        if (swap_shoulders() &&
            (lt.bindType == SDL_CONTROLLER_BINDTYPE_NONE ||
             rt.bindType == SDL_CONTROLLER_BINDTYPE_NONE))
            fprintf(stderr, "[input] WARNING: L2/R2 are not reported as axes, so "
                            "the engine's L1/R1 will not fire. Set "
                            "GTALCS_SWAP_SHOULDERS=0 (conf/swap_shoulders.txt).\n");
        fflush(stderr);
    }

    const int swapsh = swap_shoulders();

    for (int i = 0; i < 6; i++) {
        float v;
        if (swapsh && i == 4) {
            /* engine brake <- the player's L1 */
            v = SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)
                ? 1.0f : 0.0f;
        } else if (swapsh && i == 5) {
            /* engine throttle <- the player's R1 */
            v = SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)
                ? 1.0f : 0.0f;
        } else {
            v = SDL_GameControllerGetAxis(g_gamepad, axmap[i]) / 32767.0f;
            if (v > -STICK_DEADZONE && v < STICK_DEADZONE) v = 0.0f;
        }
        if (autowalk && i == 1) v = -1.0f;      /* LY: negative is forward */
        g_gamepad_axis[i] = v;
        if (setJoyAxis) setJoyAxis(fake_env, NULL, 0, i, v);
    }

    /* Buttons, as a bitmask in SDL_GameControllerButton order.  The engine's
     * own button numbering is not yet confirmed — see the notes doc; this
     * feeds both the bitmask path (WarGamepad) and the event path. */
    int buttons = 0;
    for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++)
        if (SDL_GameControllerGetButton(g_gamepad, b)) buttons |= (1 << b);

    if (swapsh) {
        /* Re-source the shoulder bits from the triggers, leaving
         * sdl_button_to_engine's table (LEFTSHOULDER -> 6, RIGHTSHOULDER -> 7)
         * untouched — the mapping is the engine's and should not be edited to
         * express a player preference. Read the raw axes, not g_gamepad_axis,
         * which now holds the shoulder buttons for indices 4 and 5. */
        buttons &= ~((1 << SDL_CONTROLLER_BUTTON_LEFTSHOULDER) |
                     (1 << SDL_CONTROLLER_BUTTON_RIGHTSHOULDER));
        float lt = SDL_GameControllerGetAxis(g_gamepad,
                       SDL_CONTROLLER_AXIS_TRIGGERLEFT)  / 32767.0f;
        float rt = SDL_GameControllerGetAxis(g_gamepad,
                       SDL_CONTROLLER_AXIS_TRIGGERRIGHT) / 32767.0f;
        if (lt > TRIGGER_PRESS) buttons |= (1 << SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
        if (rt > TRIGGER_PRESS) buttons |= (1 << SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    }

    g_gamepad_buttons = buttons;

    int changed = buttons ^ prev_buttons;
    for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++) {
        if (!(changed & (1 << b))) continue;
        int eb = sdl_button_to_engine(b);

        /* GTALCS_BTN_TRACE=1: log the first N edges as SDL's name for the button
         * the player pressed and the engine index it turns into.  This is what
         * found the A/B problem — the pad reports its physical A as SDL "b", and
         * one line of this said so after two rounds of reasoning had not.  Off by
         * default now that the layout is settled; keep it, it is cheap and it is
         * the right first move for any future "button X does the wrong thing". */
        static int traced, trace_on = -1;
        if (trace_on < 0) {
            const char *e = getenv("GTALCS_BTN_TRACE");
            trace_on = (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
        }
        if (trace_on && traced < 40) {
            traced++;
            const char *nm = SDL_GameControllerGetStringForButton(b);
            fprintf(stderr, "[btn] SDL %-14s (%d) %-4s -> engine %d\n",
                    nm ? nm : "?", b,
                    (buttons & (1 << b)) ? "DOWN" : "UP", eb);
            fflush(stderr);
        }

        if (eb < 0) continue;                 /* nothing the engine listens for */
        if (buttons & (1 << b)) { if (onJoyDown) onJoyDown(fake_env, NULL, 0, eb); }
        else                    { if (onJoyUp)   onJoyUp  (fake_env, NULL, 0, eb); }
    }

    /* ── virtual pointer ────────────────────────────────────────────────── */
    static Uint32 last_ms;
    Uint32 now_ms = SDL_GetTicks();
    float  secs   = last_ms ? (float)(now_ms - last_ms) / 1000.0f : 0.0f;
    last_ms = now_ms;
    if (secs > 0.1f) secs = 0.1f;          /* a stall must not teleport it */

    float dx = g_gamepad_axis[0], dy = g_gamepad_axis[1];
    if (buttons & (1 << SDL_CONTROLLER_BUTTON_DPAD_LEFT))  dx -= 1.0f;
    if (buttons & (1 << SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) dx += 1.0f;
    if (buttons & (1 << SDL_CONTROLLER_BUTTON_DPAD_UP))    dy -= 1.0f;
    if (buttons & (1 << SDL_CONTROLLER_BUTTON_DPAD_DOWN))  dy += 1.0f;

    if (dx != 0.0f || dy != 0.0f) {
        g_cursor_x += dx * CURSOR_SPEED * secs;
        g_cursor_y += dy * CURSOR_SPEED * secs;
        if (g_cursor_x < 0.0f) g_cursor_x = 0.0f;
        if (g_cursor_x > 1.0f) g_cursor_x = 1.0f;
        if (g_cursor_y < 0.0f) g_cursor_y = 0.0f;
        if (g_cursor_y > 1.0f) g_cursor_y = 1.0f;
        if (g_touch_down) touch_send_norm(LCS_TOUCH_MOVE, g_cursor_x, g_cursor_y);
    }

    int tap = (buttons & (1 << tap_button())) != 0;
    if (tap && !g_touch_down) {
        g_touch_down = 1;
        touch_send_norm(LCS_TOUCH_DOWN, g_cursor_x, g_cursor_y);
    } else if (!tap && g_touch_down) {
        g_touch_down = 0;
        touch_send_norm(LCS_TOUCH_UP, g_cursor_x, g_cursor_y);
    }

    prev_buttons = buttons;
}

/* Headless bring-up.  With no hands on the device, a run has to drive its own
 * frontend, so two env vars synthesise taps:
 *
 *   GTALCS_AUTOTAP=<ms>            tap the centre every <ms> ms (a "keep
 *                                  pressing continue" heartbeat)
 *   GTALCS_TAPS="t:x,y t:x,y ..."  a script — tap normalised (x,y) at t ms
 *                                  after the frame loop starts
 *
 * Coordinates are the same normalised 0..1 the engine's own onTouch* natives
 * take, so a point read off a screenshot as (px/w, py/h) can be replayed
 * verbatim. */
#define MAX_TAP_SCRIPT 32
static struct { Uint32 t; float x, y; } g_tap_script[MAX_TAP_SCRIPT];
static int g_tap_count, g_tap_next;

/* GTALCS_BUTTONS="t:index t:index …" — press engine button <index> at <t> ms
 * and release it 120 ms later.  Indices are the ENGINE numbering decoded from
 * getJoypadButtonFromKeyCode (0=A 1=B 2=X 3=Y 4=START 6=L1 7=R1 8..11=dpad
 * 12=L3 13=R3 14=BACK 15=MENU), not SDL's.  This exists so a headless run can
 * reproduce things that need a button — skipping the opening cutscene, for
 * one — without hands on the device. */
#define MAX_BTN_SCRIPT 32
static struct { Uint32 t; int btn; int state; } g_btn_script[MAX_BTN_SCRIPT];
static int g_btn_count, g_btn_next;

static void btn_script_parse(void) {
    const char *e = getenv("GTALCS_BUTTONS");
    if (!e || !*e) return;
    const char *p = e;
    while (*p && g_btn_count + 2 <= MAX_BTN_SCRIPT) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        unsigned t; int b; int used = 0;
        if (sscanf(p, "%u:%d%n", &t, &b, &used) == 2 && used > 0) {
            g_btn_script[g_btn_count].t = (Uint32)t;
            g_btn_script[g_btn_count].btn = b;
            g_btn_script[g_btn_count].state = 1;  g_btn_count++;
            g_btn_script[g_btn_count].t = (Uint32)t + 120;
            g_btn_script[g_btn_count].btn = b;
            g_btn_script[g_btn_count].state = 0;  g_btn_count++;
            p += used;
        } else {
            fprintf(stderr, "[input] GTALCS_BUTTONS: cannot parse from '%s'\n", p);
            break;
        }
    }
    for (int i = 0; i < g_btn_count; i += 2)
        fprintf(stderr, "[input] button script: t=%ums press engine button %d\n",
                g_btn_script[i].t, g_btn_script[i].btn);
}

static void btn_script_tick(Uint32 since_start, fn_ii down, fn_ii up) {
    while (g_btn_next < g_btn_count && since_start >= g_btn_script[g_btn_next].t) {
        int b = g_btn_script[g_btn_next].btn;
        int st = g_btn_script[g_btn_next].state;
        g_btn_next++;
        fprintf(stderr, "[input] scripted button %d %s\n", b, st ? "DOWN" : "UP");
        fflush(stderr);
        if (st) { if (down) down(fake_env, NULL, 0, b); }
        else    { if (up)   up  (fake_env, NULL, 0, b); }
    }
}

static void tap_script_parse(void) {
    const char *e = getenv("GTALCS_TAPS");
    if (!e || !*e) return;
    const char *p = e;
    while (*p && g_tap_count < MAX_TAP_SCRIPT) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        unsigned t; float x, y;
        int used = 0;
        if (sscanf(p, "%u:%f,%f%n", &t, &x, &y, &used) == 3 && used > 0) {
            g_tap_script[g_tap_count].t = (Uint32)t;
            g_tap_script[g_tap_count].x = x;
            g_tap_script[g_tap_count].y = y;
            g_tap_count++;
            p += used;
        } else {
            fprintf(stderr, "[input] GTALCS_TAPS: cannot parse from '%s'\n", p);
            break;
        }
    }
    for (int i = 0; i < g_tap_count; i++)
        fprintf(stderr, "[input] tap script %d: t=%ums (%.3f,%.3f)\n",
                i, g_tap_script[i].t, g_tap_script[i].x, g_tap_script[i].y);
}

static void autotap_tick(Uint32 now) {
    static int    inited;
    static Uint32 base;
    static int    period;
    static Uint32 next_auto;
    static int    down;
    static float  dx = 0.5f, dy = 0.5f;

    if (!inited) {
        inited = 1;
        base   = now;
        const char *e = getenv("GTALCS_AUTOTAP");
        period = (e && *e) ? atoi(e) : 0;
        if (period > 0) {
            fprintf(stderr, "[input] AUTOTAP every %d ms at centre screen\n", period);
            next_auto = now + (Uint32)period;
        }
        tap_script_parse();
        btn_script_parse();
    }

    /* Release first: a tap in progress always completes before the next one
     * starts, otherwise the engine sees two downs with no up between them. */
    if (down) {
        if (now >= next_auto) {
            down = 0;
            touch_send_norm(LCS_TOUCH_UP, dx, dy);
            fprintf(stderr, "[input] tap UP (%.3f,%.3f)\n", dx, dy);
            next_auto = period > 0 ? now + (Uint32)period : 0xffffffffu;
        }
        return;
    }

    /* Scripted taps take priority over the heartbeat. */
    if (g_tap_next < g_tap_count && now - base >= g_tap_script[g_tap_next].t) {
        dx = g_tap_script[g_tap_next].x;
        dy = g_tap_script[g_tap_next].y;
        g_tap_next++;
        down = 1;
        touch_send_norm(LCS_TOUCH_DOWN, dx, dy);
        fprintf(stderr, "[input] scripted tap DOWN (%.3f,%.3f) at t=%ums\n",
                dx, dy, (unsigned)(now - base));
        next_auto = now + 80;
        return;
    }

    if (period > 0 && now >= next_auto) {
        dx = 0.5f; dy = 0.5f;
        down = 1;
        touch_send_norm(LCS_TOUCH_DOWN, dx, dy);
        fprintf(stderr, "[input] autotap DOWN (%.3f,%.3f)\n", dx, dy);
        next_auto = now + 80;
    }
}

/* ── streaming-state probe ────────────────────────────────────────────────
 *
 * The remaining crash is CPed::SetModelIndex being handed a model index whose
 * clump never streamed in (see docs/REVERSE-ENGINEERING-NOTES.md for the full
 * call chain).  DumpStreamer::dump_CStreaming() exists in the .so but is an
 * empty stub in the release build, so read the exported globals directly —
 * read-only, no hooking, no trampolines.
 *
 * ms_channelError is the one that matters: if the streamer's read channel has
 * faulted, every model behind it silently stays unloaded, which is exactly the
 * state that produces a NULL RslElementGroup.
 *
 * Enabled with GTALCS_DUMP_MS=<ms>. */
void streaming_dump(void) {
    static int    inited;
    static int   *chan_err, *cur_ped, *num_req, *num_prio, *num_infos;
    static int   *big, *disable, *last_img;
    if (!inited) {
        inited = 1;
        chan_err  = (int *)so_symbol(&gtalcs_mod, "_ZN10CStreaming15ms_channelErrorE");
        last_img  = (int *)so_symbol(&gtalcs_mod, "_ZN10CStreaming16ms_lastImageReadE");
        big       = (int *)so_symbol(&gtalcs_mod, "_ZN10CStreaming19ms_bLoadingBigModelE");
        disable   = (int *)so_symbol(&gtalcs_mod, "_ZN10CStreaming19ms_disableStreamingE");
        cur_ped   = (int *)so_symbol(&gtalcs_mod, "_ZN10CStreaming20ms_currentPedLoadingE");
        num_req   = (int *)so_symbol(&gtalcs_mod, "_ZN10CStreaming21ms_numModelsRequestedE");
        num_prio  = (int *)so_symbol(&gtalcs_mod, "_ZN10CStreaming22ms_numPriorityRequestsE");
        num_infos = (int *)so_symbol(&gtalcs_mod, "_ZN10CModelInfo15msNumModelInfosE");
        fprintf(stderr, "[stream] probe: chanErr=%p curPed=%p numReq=%p infos=%p\n",
                (void *)chan_err, (void *)cur_ped, (void *)num_req, (void *)num_infos);
    }
    extern int g_cd_entries;
    fprintf(stderr, "[stream] cdEntries=%d\n", g_cd_entries);
    fprintf(stderr, "[stream] chanErr=%d lastImg=%d big=%d disabled=%d "
                    "curPedLoading=%d numRequested=%d numPriority=%d modelInfos=%d\n",
            chan_err  ? *chan_err  : -1,
            last_img  ? *last_img  : -1,
            big       ? *big       : -1,
            disable   ? *disable   : -1,
            cur_ped   ? *cur_ped   : -1,
            num_req   ? *num_req   : -1,
            num_prio  ? *num_prio  : -1,
            num_infos ? *num_infos : -1);
    fflush(stderr);
}

void jni_boot_lcs(void) {
    /* ── Hand the engine its JavaVM first ───────────────────────────────────
     * libGTALcs.so keeps a global `gJavaVM` (BSS) that JNI_OnLoad fills in, and
     * AndroidUtil_JNIInit derives every JNIEnv from it.  Skipping JNI_OnLoad —
     * which seemed reasonable since LCS boots through named entry points rather
     * than RegisterNatives — left gJavaVM NULL, so the engine's own
     * _JNIEnv::CallStaticObjectMethod wrapper faulted on a null vtable the
     * moment anything called up into Java (GetDeviceLanguage, during
     * viewOnInit).  Android's loader always calls this; so must we. */
    {
        int (*JNI_OnLoad)(void *vm, void *reserved) =
            (void *)so_symbol(&gtalcs_mod, "JNI_OnLoad");
        if (JNI_OnLoad) {
            fprintf(stderr, "[jni] JNI_OnLoad...\n"); fflush(stderr);
            int ver = JNI_OnLoad(fake_vm, NULL);
            fprintf(stderr, "[jni] JNI_OnLoad returned 0x%x\n", ver);
        } else {
            fprintf(stderr, "[jni] WARNING: JNI_OnLoad not found\n");
        }
        /* Belt and braces: if the engine did not populate it, set gJavaVM
         * directly.  It is an exported symbol, so this is cheap insurance. */
        void **gJavaVM = (void **)so_symbol(&gtalcs_mod, "gJavaVM");
        if (gJavaVM) {
            if (!*gJavaVM) {
                *gJavaVM = fake_vm;
                fprintf(stderr, "[jni] gJavaVM was NULL — set to fake_vm\n");
            } else {
                fprintf(stderr, "[jni] gJavaVM = %p (set by JNI_OnLoad)\n", *gJavaVM);
            }
        }
        fflush(stderr);
    }

    /* ── Resolve the entry points ───────────────────────────────────────── */
    fn_p    setAssetManager   = (fn_p)   sym(JNI_PFX "setAssetManager");
    fn_i    setIsTVDevice     = (fn_i)   sym(JNI_PFX "setIsTVDevice");
    fn_i    setHasVibrator    = (fn_i)   sym(JNI_PFX "setHasVibrator");
    fn_p    setPrivateFilesDir= (fn_p)   sym(JNI_PFX "setPrivateFilesDir");
    fn_p    setGameFilesDir   = (fn_p)   sym(JNI_PFX "setGameFilesDir");
    fn_p    setFileInstalled  = (fn_p)   sym(JNI_PFX "setFileInstalled");
    fn_v    viewOnInit        = (fn_v)   sym(JNI_PFX "viewOnInit");
    fn_f    viewOnDrawFrame   = (fn_f)   sym(JNI_PFX "viewOnDrawFrame");
    fn_i    setOSVersion      = (fn_i)   sym(JNI_PFX "setOSVersion");
    fn_ipss setDeviceInfo     = (fn_ipss)sym(JNI_PFX "setDeviceInfo");
    fn_ii   setDisplaySize    = (fn_ii)  sym(JNI_PFX "setDisplaySize");
    fn_i    setNoJoysticks    = (fn_i)   sym(JNI_PFX "setNoJoysticks");
    fn_ii   onJoyButtonDown   = (fn_ii)  sym(JNI_PFX "onJoyButtonDown");
    fn_ii   onJoyButtonUp     = (fn_ii)  sym(JNI_PFX "onJoyButtonUp");
    fn_iif  setJoyAxis        = (fn_iif) sym(JNI_PFX "setJoyAxis");
    g_onTouchStart            = (fn_touch)sym(JNI_PFX "onTouchStart");
    g_onTouchMove             = (fn_touch)sym(JNI_PFX "onTouchMove");
    g_onTouchEnd              = (fn_touch)sym(JNI_PFX "onTouchEnd");
    /* Preferred path: the internal handler the JNI wrappers tail-call into. */
    g_AND_TouchEvent = (fn_and_touch)so_symbol(&gtalcs_mod, "_Z14AND_TouchEventiiii");
    fprintf(stderr, "[touch] AND_TouchEvent = %p%s\n", (void *)g_AND_TouchEvent,
            g_AND_TouchEvent ? "" : "  (falling back to the JNI wrappers)");

    fn_p    setUserAgent      = (fn_p)   sym(HAL_PFX "setUserAgent");
    fn_p    setVersionNumber  = (fn_p)   sym(HAL_PFX "setVersionNumber");
    fn_pp   setLanguage       = (fn_pp)  sym(HAL_PFX "setLanguage");
    fn_ii   setScreenSize     = (fn_ii)  sym(HAL_PFX "setCurrentScreenSize");
    fn_v    wrapper_main      = (fn_v)   sym(HAL_PFX "main");
    fn_v    onStartApp        = (fn_v)   sym(HAL_PFX "onStartApp");

    /* setDisplaySize/setCurrentScreenSize take (long edge, short edge). */
    const int lw = SCREEN_W > SCREEN_H ? SCREEN_W : SCREEN_H;
    const int lh = SCREEN_W > SCREEN_H ? SCREEN_H : SCREEN_W;

    /* Touch coordinates are in the same pixel space we hand the engine here,
     * so record it before opening the touchscreen (which scales into it). */
    g_screen_w = lw;
    g_screen_h = lh;
    touchscreen_init();

    /* setGameFilesDir is NOT a directory despite the name — the engine fopen()s
     * the string verbatim.  Confirmed on device: passing "/roms/ports/gtalcs"
     * logged `[fopen] ok: /roms/ports/gtalcs`, i.e. it opened the directory
     * itself, got a handle it could not read, and every asset then fell back to
     * a relative path (`TEXT/ENGLISH.GXT`, `texture_meta_data_*.csv`) that does
     * not exist on disk.  It wants the OBB archive. */
    char save_dir[600], game_dir[700], obb_main[700];
    snprintf(save_dir, sizeof(save_dir), "%s/save/", g_data_path);
    snprintf(obb_main, sizeof(obb_main), "%s%s", g_data_path, OBB_MAIN_RELPATH);
    snprintf(game_dir, sizeof(game_dir), "%s", g_data_path);

    /* Two supported data layouts, decided here by what is actually on disk:
     *
     *   OBB present  — setGameFilesDir hands the archive to the engine, which
     *                  mounts it in _LogicalFS_Init and serves everything from
     *                  it.  This is the layout the port was first brought up on.
     *   OBB absent   — the archive is unpacked under GAMEDATA_PATH instead.
     *                  _LogicalFS_Init still adds the bundle root, so every
     *                  logical path resolves as a plain relative open and
     *                  gamedata_redirect (main.c) maps it into the unpacked
     *                  tree.  Nothing about this path needs a WAD.
     *
     * Registering a WAD that is not there is worse than registering none: the
     * mount fails, and the engine carries a dead mount point it will consult
     * on every single open.  So the probe decides, rather than always calling
     * setGameFilesDir with a path that may not exist.
     *
     * GTALCS_WAD overrides the choice with an explicit archive path. */
    int have_obb = 0;
    {
        const char *forced = getenv("GTALCS_WAD");
        if (forced && *forced)
            snprintf(obb_main, sizeof(obb_main), "%s", forced);

        FILE *probe = fopen(obb_main, "rb");
        if (probe) { have_obb = 1; fclose(probe); }
    }
    if (have_obb) {
        fprintf(stderr, "[jni] data: WAD archive %s\n", obb_main);
    } else {
        struct stat gd;
        int has_gamedata = (stat(GAMEDATA_PATH, &gd) == 0 && S_ISDIR(gd.st_mode));
        fprintf(stderr, "[jni] data: no OBB at %s -> loose files under %s%s\n",
                obb_main, GAMEDATA_PATH,
                has_gamedata ? "" : "  *** WHICH DOES NOT EXIST ***");
        if (!has_gamedata)
            fprintf(stderr, "[jni] ERROR: neither the OBB nor an unpacked "
                            "%s exists — no game data at all\n", GAMEDATA_PATH);
    }

    /* ── Phase 1: what GTAActivityBase.onCreate / GTAActivity.a() do ────── */
    fprintf(stderr, "[jni] phase 1: activity setup\n");
    if (setAssetManager)    setAssetManager(fake_env, NULL, (void *)0x41414141);
    if (setIsTVDevice)      setIsTVDevice(fake_env, NULL, 0);
    if (setHasVibrator)     setHasVibrator(fake_env, NULL, 0);
    if (setPrivateFilesDir) setPrivateFilesDir(fake_env, NULL, save_dir);
    /* Only register the archive when there is one.  setGameFilesDir appends to
     * the engine's g_WADFilePath[2][512] and bumps g_totalWadFiles; with the
     * count left at 0, _LogicalFS_Init skips the mount entirely and the bundle
     * root is the only mount point. */
    if (have_obb) {
        if (setGameFilesDir)  setGameFilesDir(fake_env, NULL, obb_main);
        if (setFileInstalled) setFileInstalled(fake_env, NULL, obb_main);
    }

    /* ── Phase 2: ActivityWrapper.runMain's sequence ────────────────────── */
    fprintf(stderr, "[jni] phase 2: ActivityWrapper\n");
    if (setUserAgent)     setUserAgent(fake_env, NULL, (void *)"GTALCS/2.4 (Linux; ARM)");
    if (setVersionNumber) setVersionNumber(fake_env, NULL, (void *)"2.4");
    if (setLanguage)      setLanguage(fake_env, NULL, (void *)"en", (void *)"US");
    if (setScreenSize)    setScreenSize(fake_env, NULL, lw, lh);
    if (onStartApp)       onStartApp(fake_env, NULL);

    /* ActivityWrapper_main does NOT return — confirmed on device 2026-09-07:
     * it blocks driving the HAL/Social Club UI, exactly as it does on Android's
     * UI thread, and phase 3 was never reached when we called it inline.
     *
     * So mirror Android's split: main() runs on its own thread, while THIS
     * thread keeps the GL context and drives the renderer callbacks.  The
     * context must stay with the thread that made it current — egl_patch
     * captured it here — so the UI thread is the one that gets moved, not the
     * render loop. */
    if (wrapper_main) {
        static fn_v s_wrapper_main;
        s_wrapper_main = wrapper_main;
        pthread_t ui;
        if (pthread_create(&ui, NULL, ui_thread_main, (void *)(uintptr_t)s_wrapper_main) == 0) {
            pthread_setname_np(ui, "lcs-ui");
            fprintf(stderr, "[jni] ActivityWrapper_main running on its own thread\n");
        } else {
            fprintf(stderr, "[jni] WARNING: could not spawn UI thread\n");
        }
        fflush(stderr);
        /* Give the engine a moment to get through its own start-up before the
         * renderer starts calling in. */
        SDL_Delay(500);
    }

    /* ── Phase 3: the GLSurfaceView renderer callbacks ──────────────────── */
    /* Re-assert the context on THIS thread first.  The UI thread we just
     * spawned runs engine code that may call eglMakeCurrent, and an EGL context
     * can only be current on one thread at a time — if it moved, every
     * glGetString here returns NULL and the engine's extension probes see an
     * empty string. */
    if (SDL_GL_MakeCurrent(g_window, g_gl_ctx) != 0)
        fprintf(stderr, "[jni] WARNING: SDL_GL_MakeCurrent failed: %s\n", SDL_GetError());
    {
        const unsigned char *rend = glGetString(0x1F01 /* GL_RENDERER */);
        fprintf(stderr, "[jni] GL_RENDERER = %s\n", rend ? (const char *)rend : "(NULL)");
    }
    /* Re-capture here: this is the point where the context is provably usable,
     * so it is the most reliable moment to record what the engine will share. */
    egl_patch_capture();

    fprintf(stderr, "[jni] phase 3: surface init\n");
    if (viewOnInit)     viewOnInit(fake_env, NULL);
    if (setOSVersion)   setOSVersion(fake_env, NULL, DEVICE_OS_VERSION);
    /* Report the machine's REAL memory.  The engine feeds this to
     * SetDevicePerfIndex, which sizes the streaming budget among other things,
     * and the hardcoded 512 understated a 2 GB device badly: the streamer
     * thrashed, draw calls DECAYED in-game (333k -> 37k as the world unloaded
     * and never came back), and a script CREATE_CHAR eventually got a model
     * whose clump had been evicted -> CPed::SetModelIndex with a NULL clump ->
     * the GetFirstElement(NULL) crash.  Reporting 2 GB reverses the trend
     * (335k -> 649k -> 689k, world streaming IN) and the crash goes away.
     * Detect it rather than hardcoding, so this is right on other devices. */
    int mem_mb = DEVICE_MEMORY_MB;
    {
        FILE *mi = fopen("/proc/meminfo", "r");
        if (mi) {
            char line[128];
            while (fgets(line, sizeof(line), mi)) {
                unsigned long kb;
                if (sscanf(line, "MemTotal: %lu kB", &kb) == 1) {
                    if (kb > 64u * 1024u) mem_mb = (int)(kb / 1024u);
                    break;
                }
            }
            fclose(mi);
        }
        fprintf(stderr, "[jni] setDeviceInfo memory = %d MB%s\n", mem_mb,
                mem_mb == DEVICE_MEMORY_MB ? " (fallback)" : " (from /proc/meminfo)");
    }
    if (setDeviceInfo)  setDeviceInfo(fake_env, NULL, mem_mb,
                                      (void *)DEVICE_MANUFACTURER,
                                      (void *)DEVICE_MANUFACTURER,
                                      (void *)DEVICE_HARDWARE);
    if (setDisplaySize) setDisplaySize(fake_env, NULL, lw, lh);

    /* ── Phase 4: the frame loop ────────────────────────────────────────── */
    fprintf(stderr, "[jni] entering frame loop\n"); fflush(stderr);
    Uint32 prev = SDL_GetTicks();
    int      frames = 0;
    /* Framebuffer dumps are a bring-up tool and are OFF unless asked for:
     * each one is a 900 KB PPM written next to the game, so a shipping build
     * that dumped every 5 s would quietly fill the card.  Set
     * GTALCS_SHOT_MS=<ms> to enable (first dump at 2 s, then every <ms>). */
    Uint32 shot_ms   = 0;
    Uint32 next_shot = 0xffffffffu;
    {
        const char *e = getenv("GTALCS_SHOT_MS");
        if (e && *e) {
            int v = atoi(e);
            if (v > 0) { shot_ms = (Uint32)v; next_shot = 2000; }
        }
    }
    extern volatile unsigned long g_gl_draw_calls;
    unsigned long draws_prev = g_gl_draw_calls;
    for (;;) {
        Uint32 now = SDL_GetTicks();
        float dt = (float)(now - prev);
        prev = now;

        poll_input(setJoyAxis, onJoyButtonDown, onJoyButtonUp);
        touchscreen_poll();
        autotap_tick(now);
        {   static Uint32 t0; if (!t0) t0 = now;
            btn_script_tick(now - t0, onJoyButtonDown, onJoyButtonUp); }

        /* One-shot: are the model's texlist slot and the archive's the same slot?
         * Answered as soon as the CD directory is parsed — see txd_boot_check. */
        { extern void txd_boot_check(void); txd_boot_check(); }

        /* GTALCS_DUMP_MS=<ms>: periodic streaming-state line (off by default). */
        {
            static int    dump_ms = -1;
            static Uint32 next_dump;
            if (dump_ms < 0) {
                const char *e = getenv("GTALCS_DUMP_MS");
                dump_ms = (e && *e) ? atoi(e) : 0;
                next_dump = now;
            }
            if (dump_ms > 0 && now >= next_dump) {
                streaming_dump();
                next_dump = now + (Uint32)dump_ms;
            }
        }

        /* The engine re-reads the pad count every frame on Android. */
        if (setNoJoysticks)  setNoJoysticks(fake_env, NULL, g_gamepad ? 1 : 0);
        if (viewOnDrawFrame) viewOnDrawFrame(fake_env, NULL, dt);

        /* Bring-up screenshot: read the framebuffer back BEFORE the swap —
         * after it the back buffer is undefined, which is why the first
         * capture pass saw uniform black at every interval. */
        frames++;
        if (now >= next_shot) {
            /* Who owns this thread's context right now?  The engine creates a
             * shared context on a pbuffer; if it left that current, both the
             * readback below and the swap further down address the wrong
             * surface.  One line per interval, not per frame. */
            egl_patch_report_current("frameloop");
            int w = 0, h = 0;
            SDL_GL_GetDrawableSize(g_window, &w, &h);
            /* ES2 guarantees exactly ONE format for the default framebuffer:
             * GL_RGBA/GL_UNSIGNED_BYTE.  GL_RGB is an implementation-defined
             * extra and Mali rejects it — every readback so far returned
             * GL_INVALID_OPERATION (0x0502) and left the buffer untouched,
             * which is why the "screenshots" were first uniform black (fresh
             * pages) and later RGB noise (dirty heap).  Read RGBA, write RGB. */
            unsigned char *px = malloc((size_t)w * h * 4);
            if (px) {
                /* default framebuffer (framebuffer object 0 is not bindable on
                 * ES2; glReadPixels on the default reads the back buffer). */
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                /* Poison first: a silently-failing glReadPixels leaves the
                 * buffer holding whatever malloc handed us, and dirty heap
                 * memory reads back as convincing "content" (it did — a
                 * min=0/max=255/mean=37 frame that was pure RGB noise).
                 * A known fill plus the GL error makes that unambiguous. */
                memset(px, 0x7f, (size_t)w * h * 4);
                while (glGetError() != GL_NO_ERROR) { }
                glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
                GLenum rp_err = glGetError();
                char name[64];
                snprintf(name, sizeof(name), "frame%d.ppm", frames);
                FILE *f = fopen(name, "wb");
                if (f) {
                    fprintf(f, "P6\n%d %d\n255\n", w, h);
                    /* GL origin is bottom-left; flip rows so the PPM is upright,
                     * and drop the alpha channel on the way out. */
                    unsigned char *row = malloc((size_t)w * 3);
                    for (int y = h - 1; y >= 0 && row; y--) {
                        const unsigned char *src = px + (size_t)y * w * 4;
                        for (int x = 0; x < w; x++) {
                            row[x * 3 + 0] = src[x * 4 + 0];
                            row[x * 3 + 1] = src[x * 4 + 1];
                            row[x * 3 + 2] = src[x * 4 + 2];
                        }
                        fwrite(row, 1, (size_t)w * 3, f);
                    }
                    free(row);
                    fclose(f);
                    /* quick content summary so a run says black/static/detail
                     * without fetching the pixels */
                    unsigned long mn = 255, mx = 0, sum = 0;
                    for (size_t i = 0; i < (size_t)w * h * 4; i++) {
                        if (px[i] < mn) mn = px[i];
                        if (px[i] > mx) mx = px[i];
                        sum += px[i];
                    }
                    fprintf(stderr, "[shot] %s %dx%d min=%lu max=%lu mean=%.1f "
                                    "draws=%lu readpixels_err=0x%04x\n",
                            name, w, h, mn, mx, (double)sum / ((double)w * h * 4),
                            g_gl_draw_calls - draws_prev, (unsigned)rp_err);
                }
                free(px);
            }
            draws_prev = g_gl_draw_calls;
            next_shot = now + shot_ms;
        }

        /* THE PRESENT.  libGTALcs.so's 408 imports do NOT include
         * eglSwapBuffers: on Android, GLSurfaceView swaps for the renderer
         * once onDrawFrame() returns.  We are the GLSurfaceView, so nothing
         * reaches the panel unless we do this — which is exactly why every
         * capture so far showed an unchanged screen. */
        SDL_GL_SwapWindow(g_window);
    }
}
