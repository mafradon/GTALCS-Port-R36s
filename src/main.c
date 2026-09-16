/* main.c -- GTA: Liberty City Stories loader for R36S-class handhelds
 *
 * Loads the Android armeabi-v7a libGTALcs.so with our own ELF loader and gives
 * it everything it expects from bionic, the NDK and the Java runtime.
 *
 * Adapted from the GTA: Chinatown Wars port (~/gtacw-port).  The bionic ABI
 * shims, ctype/stdio compatibility, crash handler and libc time patching are
 * that port's, proven on device; the EGL, asset-manager, OpenAL and JNI layers
 * are LCS-specific because the two games use completely different frameworks.
 *
 * THE STANDING HAZARD: libGTALcs.so is soft-float ABI, this binary is
 * hard-float.  Every hook passing a float or double BY VALUE — argument or
 * return — needs SOFTFP, or the callee reads a stale VFP register.  When you
 * add a hook, check its signature first.  See docs/REVERSE-ENGINEERING-NOTES.md.
 */

#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <fenv.h>
#include <setjmp.h>
#include <ctype.h>
#include <wchar.h>
#include <wctype.h>
#include <locale.h>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <semaphore.h>
#include <signal.h>
#include <ucontext.h>
#include <execinfo.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <strings.h>   /* strcasecmp — loose-file path folding */
#include <zlib.h>
#include <linux/input.h>

#include <SDL2/SDL.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>

#include "config.h"
#include "so_util.h"
#include "jni_patch.h"
#include "asset_manager.h"

/* Bionic-layout setjmp/longjmp — src/setjmp_fix.c.  libpng/zlib inside the
 * .so embed jmp_bufs sized for bionic; glibc's would overrun them. */
int  my_setjmp(int *buf);
void my_longjmp(int *buf, int val);

/* Guard-page debug allocator — src/guard_alloc.c.  Pass-through to glibc
 * unless GTALCS_GUARD=1 in the environment. */
void  ga_init(void);
void *ga_malloc(size_t n);
void  ga_free(void *p);
void *ga_realloc(void *p, size_t n);
void *ga_calloc(size_t a, size_t b);
int   guard_annotate(unsigned long addr);   /* crash-handler block attribution */
#include "egl_patch.h"
#include "openal_patch.h"
#include "opengl_patch.h"

/* ── Globals shared with other modules ──────────────────────────────────── */

so_module           gtalcs_mod;
SDL_Window         *g_window  = NULL;
SDL_GLContext       g_gl_ctx  = NULL;
SDL_GameController *g_gamepad = NULL;
int                 g_gamepad_buttons = 0;
float               g_gamepad_axis[6] = { 0 };
char                g_data_path[512]  = DATA_PATH;

/* ── C++ ABI symbols ─────────────────────────────────────────────────────── */
extern int  __cxa_atexit(void (*)(void *), void *, void *) __attribute__((weak));
extern void __cxa_finalize(void *)                         __attribute__((weak));

/* __cxa_guard: replace bionic libc++ implementation with a simple GCC-compatible one.
 * Guard object layout: byte 0 = initialized flag.
 * Returns 1 (proceed) if not yet initialized, 0 if done. */
static int  __cxa_guard_acquire_impl(long *g) { return (*(char*)g == 0); }
static void __cxa_guard_release_impl(long *g) { *(char*)g = 1; }
static void __cxa_guard_abort_impl  (long *g) { (void)g; }

/* ── glibc gettid wrapper (kernel syscall) ───────────────────────────────── */
static pid_t _gettid(void) { return (pid_t)syscall(SYS_gettid); }

/* clock_gettime, __clock_gettime64, and clock_gettime64_safe are defined in
 * clock_fix.c (separate TU that avoids <time.h>'s __asm__ alias which would
 * cause duplicate symbols when both are defined in the same translation unit) */
extern int clock_gettime(clockid_t, struct timespec *);
extern int __clock_gettime64(clockid_t, void *);
extern int clock_gettime64_safe(clockid_t, void *);
extern int gettimeofday64_safe(void *, void *);
extern int gettimeofday_safe(void *, void *);

/* Bionic-layout time calls — see the long comment in clock_fix.c.  The game's
 * struct timespec/timeval are 8 bytes; this glibc's are 16.  Handing the
 * game's buffers to glibc smashes its stack frame. */
extern int  bionic_clock_gettime(int, void *);
extern int  bionic_gettimeofday(void *, void *);
extern int  bionic_nanosleep(const void *, void *);
extern long bionic_time(long *);

/* ── isfinite / signbit: glibc provides these as macros, expose functions ── */
static int _isfinite(double d) { return isfinite(d); }
static int _signbit(double d)  { return signbit(d); }

/* ── Stub helpers ────────────────────────────────────────────────────────── */

static int  ret0(void)  { return 0; }
static int  ret1(void)  { return 1; }

static volatile int g_malloc_count = 0;
static void *malloc_debug(size_t n) {
    return malloc(n);
}

/* ── ARM EABI memory helpers: argument order differs from glibc ──────────── */
/* __aeabi_memset(dst, n, c) — note: n and c are SWAPPED vs memset(dst,c,n) */
static void __aeabi_memset_impl(void *dst, size_t n, int c)  { memset(dst, c, n); }
/* __aeabi_memclr(dst, n) — zero n bytes */
static void __aeabi_memclr_impl(void *dst, size_t n)         { memset(dst, 0, n); }

/* ── Android log → stderr ────────────────────────────────────────────────── */

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
#ifdef DEBUG
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[%s] ", tag);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
#else
    (void)prio; (void)tag; (void)fmt;
#endif
    return 0;
}

/* ── Bionic pthread/semaphore ABI shims ──────────────────────────────────────
 * On Android bionic (32-bit): mutex=4 bytes, cond=4 bytes, sem=4 bytes.
 * On glibc (32-bit):          mutex=24 bytes, cond=48 bytes, sem=16 bytes.
 * pthread_attr_t: bionic=24 bytes, glibc=36 bytes.
 *
 * Strategy for mutex/cond/sem: store a heap pointer to a real glibc object in
 * the game's 4-byte bionic slot.  Lazy-init handles zero-initialised globals.
 * Strategy for attr: use our own 24-byte layout, ignore glibc's larger struct.
 */

/* ── mutex attributes: a bionic attr must NEVER reach glibc ────────────────
 *
 * bionic's pthread_mutexattr_t is a 4-byte int with the type in bits 0-3
 * (0 normal, 1 recursive, 2 errorcheck) and the process-shared flag at 0x10.
 * glibc's is also 4 bytes, but the encoding is entirely different — the kind
 * shares the word with ROBUST (0x40000000), PSHARED (0x80000000) and the
 * priority-protocol bits.
 *
 * pthread_mutexattr_init and _settype were both bound to `ret0`, so the
 * game's attr object was never written at all — it held whatever was on the
 * caller's stack — and pthread_mutex_init_fake then handed that garbage
 * straight to glibc's pthread_mutex_init as a glibc attr.  Depending on the
 * bits, that yields a mutex of an arbitrary kind, or a robust/pshared one, or
 * a failed init.  A game that asked for RECURSIVE and got NORMAL deadlocks on
 * its second lock; one that got a garbage kind behaves unpredictably — which
 * is exactly the shape of a nondeterministic multithreaded failure.
 *
 * Fix: implement the bionic side honestly, and translate at the boundary. */
#define BIONIC_MUTEXATTR_TYPE_MASK   0x000f

static int pthread_mutexattr_init_fake(int *a) {
    if (a) *a = 0;                       /* PTHREAD_MUTEX_NORMAL */
    return 0;
}
static int pthread_mutexattr_destroy_fake(int *a) { (void)a; return 0; }
static int pthread_mutexattr_settype_fake(int *a, int type) {
    if (!a) return 22 /* EINVAL */;
    *a = (*a & ~BIONIC_MUTEXATTR_TYPE_MASK) | (type & BIONIC_MUTEXATTR_TYPE_MASK);
    return 0;
}
static int pthread_mutexattr_gettype_fake(const int *a, int *type) {
    if (!a || !type) return 22;
    *type = *a & BIONIC_MUTEXATTR_TYPE_MASK;
    return 0;
}

static int pthread_mutex_init_fake(pthread_mutex_t **m, const int *bionic_attr) {
    pthread_mutex_t *real = calloc(1, sizeof(pthread_mutex_t));
    if (!real) return 12 /* ENOMEM */;

    if (bionic_attr) {
        int type = *bionic_attr & BIONIC_MUTEXATTR_TYPE_MASK;
        pthread_mutexattr_t ga;
        pthread_mutexattr_init(&ga);
        switch (type) {
        case 1:  pthread_mutexattr_settype(&ga, PTHREAD_MUTEX_RECURSIVE);  break;
        case 2:  pthread_mutexattr_settype(&ga, PTHREAD_MUTEX_ERRORCHECK); break;
        default: pthread_mutexattr_settype(&ga, PTHREAD_MUTEX_NORMAL);     break;
        }
        pthread_mutex_init(real, &ga);
        pthread_mutexattr_destroy(&ga);
    } else {
        pthread_mutex_init(real, NULL);
    }
    *m = real;
    return 0;
}
static int pthread_mutex_destroy_fake(pthread_mutex_t **m) {
    if (*m) { pthread_mutex_destroy(*m); free(*m); *m = NULL; }
    return 0;
}
static int pthread_mutex_lock_fake(pthread_mutex_t **m) {
    if (!*m) pthread_mutex_init_fake(m, NULL);
    return pthread_mutex_lock(*m);
}
static int pthread_mutex_unlock_fake(pthread_mutex_t **m) {
    if (!*m) return 0;
    return pthread_mutex_unlock(*m);
}
static int pthread_mutex_trylock_fake(pthread_mutex_t **m) {
    if (!*m) pthread_mutex_init_fake(m, NULL);
    return pthread_mutex_trylock(*m);
}

static int pthread_cond_init_fake(pthread_cond_t **c,
                                   const pthread_condattr_t *a) {
    pthread_cond_t *real = calloc(1, sizeof(pthread_cond_t));
    pthread_cond_init(real, a);
    *c = real;
    return 0;
}
static int pthread_cond_destroy_fake(pthread_cond_t **c) {
    if (*c) { pthread_cond_destroy(*c); free(*c); *c = NULL; }
    return 0;
}
static int pthread_cond_wait_fake(pthread_cond_t **c, pthread_mutex_t **m) {
    if (!*c) pthread_cond_init_fake(c, NULL);
    if (!*m) pthread_mutex_init_fake(m, NULL);
    return pthread_cond_wait(*c, *m);
}
static int pthread_cond_timedwait_fake(pthread_cond_t **c, pthread_mutex_t **m,
                                        const struct timespec *t) {
    if (!*c) pthread_cond_init_fake(c, NULL);
    if (!*m) pthread_mutex_init_fake(m, NULL);
    return pthread_cond_timedwait(*c, *m, t);
}
static int pthread_cond_signal_fake(pthread_cond_t **c) {
    if (*c) return pthread_cond_signal(*c);
    return 0;
}
static int pthread_cond_broadcast_fake(pthread_cond_t **c) {
    if (*c) return pthread_cond_broadcast(*c);
    return 0;
}

static int sem_init_fake(sem_t **s, int pshared, unsigned int value) {
    sem_t *real = calloc(1, sizeof(sem_t));
    sem_init(real, pshared, value);
    *s = real;
    return 0;
}
static int sem_destroy_fake(sem_t **s) {
    if (*s) { sem_destroy(*s); free(*s); *s = NULL; }
    return 0;
}
/* Instrumented sem_wait.
 *
 * The streamer syncs on Platform::Semaphore::Down() -> sem_wait, and
 * CdStreamSync has a textbook lost-wakeup shape: it reads ch->nSectorsToRead,
 * sees work pending, THEN sets ch->bLocked and waits — while the stream thread
 * clears nSectorsToRead, reads bLocked, and only posts if it was set.  If the
 * thread runs between those two steps the post never happens and the waiter
 * blocks forever.  A stalled streamer would explain everything seen in-game:
 * draw calls decaying (333k -> 37k) as the world unloads and never reloads,
 * and then a model index whose clump is NULL.
 *
 * So: wait in bounded slices and report a stall instead of hanging silently.
 * This only observes — it still waits forever — but it names the caller, which
 * is what decides whether the deadlock theory is right. */
static int sem_wait_fake(sem_t **s) {
    if (!*s) sem_init_fake(s, 0, 0);

    if (sem_trywait(*s) == 0) return 0;          /* fast path, no syscall churn */

    void *caller = __builtin_return_address(0);
    for (int slice = 0; ; slice++) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 2;
        if (sem_timedwait(*s, &ts) == 0) {
            if (slice > 0)
                fprintf(stderr, "[sem] recovered after %d s (caller=%p)\n",
                        slice * 2, caller);
            return 0;
        }
        if (errno != ETIMEDOUT) return -1;
        /* StreamThread::ThreadMain+0x30 is the worker's IDLE wait for the next
         * queued request.  A long wait there is normal (nothing to stream),
         * not a deadlock — it was the first thing this detector reported and
         * it is noise.  Everything else is worth seeing. */
        if ((uintptr_t)caller - gtalcs_mod.load_bias == 0x231494) continue;
        static int reported;
        if (reported < 40) {
            reported++;
            fprintf(stderr, "[sem] STALL: waiting %d s on %p caller=%p thread=%p\n",
                    (slice + 1) * 2, (void *)*s, caller,
                    (void *)(uintptr_t)pthread_self());
            fflush(stderr);
        }
    }
}
static int sem_post_fake(sem_t **s) {
    if (!*s) sem_init_fake(s, 0, 0);
    return sem_post(*s);
}
static int sem_trywait_fake(sem_t **s) {
    if (!*s) return EAGAIN;
    return sem_trywait(*s);
}
static int sem_getvalue_fake(sem_t **s, int *val) {
    if (!*s) { if (val) *val = 0; return 0; }
    return sem_getvalue(*s, val);
}

/* pthread_attr_t: bionic layout (24 bytes) — store only what we need */
typedef struct {
    uint32_t flags;
    void    *stack_base;
    size_t   stack_size;
    size_t   guard_size;
    int32_t  sched_policy;
    int32_t  sched_priority;
} bionic_attr_t;

static int pthread_attr_init_fake(bionic_attr_t *a) {
    memset(a, 0, sizeof(*a));
    a->guard_size = 4096;
    return 0;
}
static int pthread_attr_destroy_fake(bionic_attr_t *a)          { (void)a; return 0; }
static int pthread_attr_setstacksize_fake(bionic_attr_t *a, size_t s) {
    a->stack_size = s; return 0;
}
static int pthread_attr_getstacksize_fake(bionic_attr_t *a, size_t *s) {
    *s = a->stack_size; return 0;
}
static int pthread_attr_getstack_fake(bionic_attr_t *a, void **base, size_t *s) {
    *base = a->stack_base; *s = a->stack_size; return 0;
}
static int pthread_attr_setschedparam_fake(bionic_attr_t *a,
                                            const struct sched_param *p) {
    a->sched_priority = p->sched_priority; return 0;
}
static int pthread_attr_getschedparam_fake(bionic_attr_t *a, struct sched_param *p) {
    p->sched_priority = a->sched_priority; return 0;
}

/* ── Real pthread_create (RTLD_NEXT avoids recursion when we define the symbol) */
static int (*real_pthread_create)(pthread_t *, const pthread_attr_t *,
                                   void *(*)(void *), void *) = NULL;
static void init_real_pthread_create(void) {
    if (!real_pthread_create)
        real_pthread_create = dlsym(RTLD_NEXT, "pthread_create");
}

/* Global pthread_create — intercepts thread creation from ALL loaded shared libs.
 * The executable's strong definition preempts libpthread's for every .so loaded
 * with RTLD_GLOBAL (which includes system libopenal.so). Lets us catch any
 * library that tries to spawn a thread with a NULL start_routine. */
int pthread_create(pthread_t *tidp, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg) {
    init_real_pthread_create();
    /* Use volatile to prevent -O2 from eliminating the NULL check based on
     * the nonnull prototype attribute. */
    void *(*volatile sr)(void *) = start_routine;
    pthread_t *volatile tp = tidp;
    if (!sr) {
        if (tp) *tp = 0;
        return 0;
    }
    return real_pthread_create(tidp, attr, sr, arg);
}

/* ── Bionic TSD stubs ────────────────────────────────────────────────────────
 * The PSVita port stubs all TSD operations as no-ops and that works fine.
 * Our real bionic→glibc key mapping caused pthread_kill to be called with
 * a corrupted TID when keys or their values got out of sync with glibc
 * internals.  Match the PSVita approach: key_create/delete/get/set are all
 * ret0 — NVThreadGetCurrentJNIEnv() is already hooked directly so the game's
 * JNI env lookup never needs TSD. */

/* Global pthread_cancel interceptor — the game does not import pthread_cancel,
 * but SDL2/OpenAL may call it to stop their internal threads.  Log it so we
 * can identify which thread is being cancelled and what its pthread_t is. */
int pthread_cancel(pthread_t thread) {
    static int (*real_pc)(pthread_t) = NULL;
    if (!real_pc)
        real_pc = dlsym(RTLD_NEXT, "pthread_cancel");
    return real_pc(thread);
}

/* pthread_create_fake: called from libGTALcs.so's GOT (bionic ABI, pointer-redirect
 * attr).  Uses a trampoline to log thread start/stop and handles NULL guards. */
typedef struct { void *(*func)(void *); void *arg; } pt_tramp_t;

static void *pthread_tramp(void *p) {
    pt_tramp_t *t = p;
    void *(*f)(void *) = t->func;
    void *a = t->arg;
    free(t);
    return f(a);
}

static int pthread_create_fake(pthread_t *tidp, bionic_attr_t *attr,
                                void *func, void *arg) {
    (void)attr;
    if (!func) {
        if (tidp) *tidp = 0;
        return 0;
    }
    init_real_pthread_create();
    pt_tramp_t *t = malloc(sizeof(*t));
    t->func = (void *(*)(void *))func;
    t->arg  = arg;
    pthread_t tid;
    int r = real_pthread_create(&tid, NULL, pthread_tramp, t);
    if (tidp) *tidp = tid;
    return r;
}

/* ── stat/fstat: bionic's struct stat, filled COMPLETELY ──────────────────
 *
 * Fourth member of this port's ABI-mismatch family, after time_t, jmp_buf and
 * the soft-float boundary — and the nastiest, because the old hooks looked
 * deliberate.  They wrote exactly one field:
 *
 *     *(int *)((char *)statbuf + 0x50) = (int)st.st_mtime;   // and nothing else
 *
 * That is the right offset for bionic's st_mtime, so it read as a considered
 * translation.  But every OTHER field in the caller's struct was left holding
 * whatever was on its stack — including **st_size at offset 48**.
 *
 * The engine fstat()s the 1.9 GB OBB to learn the archive size before seeking
 * in it.  Handed a garbage size, WadArchive computes garbage offsets and block
 * lengths; the failure lands far away, in SerialiseEncryptDecryptBuffer
 * writing 0x2000 bytes through a pointer that is nowhere near a real buffer.
 *
 * Layouts (ARM32):
 *
 *   bionic                        glibc (armhf, 32-bit off_t)
 *   ------------------------      -------------------------------
 *    0 st_dev      u64             0 st_dev      u64
 *    8 __pad0[4]                   8 __pad1[2]+pad
 *   12 __st_ino    u32            12 st_ino      u32
 *   16 st_mode     u32            16 st_mode     u32
 *   20 st_nlink    u32            20 st_nlink    u32
 *   24 st_uid      u32            24 st_uid      u32
 *   28 st_gid      u32            28 st_gid      u32
 *   32 st_rdev     u64            32 st_rdev     u64
 *   40 __pad3[4] + pad            40 __pad2[2]+pad
 *   48 st_size     s64  <-- !!    44 st_size     s32   <-- !!
 *   56 st_blksize  u32            48 st_blksize  u32
 *   64 st_blocks   u64            52 st_blocks   u32
 *   72 st_atime, 80 st_mtime, 88 st_ctime (each u32 + u32 nsec)
 *   96 st_ino      u64
 *   = 104 bytes
 *
 * So bionic's 8-byte st_size overlaps glibc's st_blksize+st_blocks — reading
 * it would yield something like 0x…1000, which is exactly the flavour of
 * bogus length seen in the crash.  Nothing about that is fixable by writing
 * one field; the whole struct has to be translated.
 *
 * fstat64/stat64 are used as the source so a >2 GB archive is still correct. */

struct bionic_stat {
    uint64_t st_dev;          /*  0 */
    uint32_t __pad0;          /*  8 */
    uint32_t __st_ino;        /* 12 */
    uint32_t st_mode;         /* 16 */
    uint32_t st_nlink;        /* 20 */
    uint32_t st_uid;          /* 24 */
    uint32_t st_gid;          /* 28 */
    uint64_t st_rdev;         /* 32 */
    uint32_t __pad3;          /* 40 */
    uint32_t __pad4;          /* 44 */
    int64_t  st_size;         /* 48 */
    uint32_t st_blksize;      /* 56 */
    uint32_t __pad5;          /* 60 */
    uint64_t st_blocks;       /* 64 */
    uint32_t st_atime_;       /* 72 */
    uint32_t st_atime_nsec;   /* 76 */
    uint32_t st_mtime_;       /* 80 = 0x50 — what the old hooks wrote */
    uint32_t st_mtime_nsec;   /* 84 */
    uint32_t st_ctime_;       /* 88 */
    uint32_t st_ctime_nsec;   /* 92 */
    uint64_t st_ino;          /* 96 */
};                            /* 104 */
_Static_assert(sizeof(struct bionic_stat) == 104, "bionic struct stat is 104 bytes");
_Static_assert(__builtin_offsetof(struct bionic_stat, st_size)  == 48, "st_size at 48");
_Static_assert(__builtin_offsetof(struct bionic_stat, st_mtime_) == 0x50, "st_mtime at 0x50");

static void stat64_to_bionic(const struct stat64 *g, struct bionic_stat *b) {
    memset(b, 0, sizeof(*b));
    b->st_dev         = (uint64_t)g->st_dev;
    b->__st_ino       = (uint32_t)g->st_ino;
    b->st_ino         = (uint64_t)g->st_ino;
    b->st_mode        = (uint32_t)g->st_mode;
    b->st_nlink       = (uint32_t)g->st_nlink;
    b->st_uid         = (uint32_t)g->st_uid;
    b->st_gid         = (uint32_t)g->st_gid;
    b->st_rdev        = (uint64_t)g->st_rdev;
    b->st_size        = (int64_t) g->st_size;
    b->st_blksize     = (uint32_t)g->st_blksize;
    b->st_blocks      = (uint64_t)g->st_blocks;
    b->st_atime_      = (uint32_t)g->st_atim.tv_sec;
    b->st_atime_nsec  = (uint32_t)g->st_atim.tv_nsec;
    b->st_mtime_      = (uint32_t)g->st_mtim.tv_sec;
    b->st_mtime_nsec  = (uint32_t)g->st_mtim.tv_nsec;
    b->st_ctime_      = (uint32_t)g->st_ctim.tv_sec;
    b->st_ctime_nsec  = (uint32_t)g->st_ctim.tv_nsec;
}

static int stat_hook(const char *path, void *statbuf) {
    struct stat64 st;
    int r = stat64(path, &st);
    if (r == 0) stat64_to_bionic(&st, (struct bionic_stat *)statbuf);
    return r;
}

/* ── ctype / stdio ABI compatibility ─────────────────────────────────────── */

/* Android libc exposes these as pointers; glibc does too but at different
 * symbol names.  We provide matching data so the game finds valid tables. */

static const short C_tolower_tab[257] = {
    -1,
    0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
    0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
    0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
    0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f,
    0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27,
    0x28,0x29,0x2a,0x2b,0x2c,0x2d,0x2e,0x2f,
    0x30,0x31,0x32,0x33,0x34,0x35,0x36,0x37,
    0x38,0x39,0x3a,0x3b,0x3c,0x3d,0x3e,0x3f,
    0x40,'a','b','c','d','e','f','g',
    'h','i','j','k','l','m','n','o',
    'p','q','r','s','t','u','v','w',
    'x','y','z',0x5b,0x5c,0x5d,0x5e,0x5f,
    0x60,0x61,0x62,0x63,0x64,0x65,0x66,0x67,
    0x68,0x69,0x6a,0x6b,0x6c,0x6d,0x6e,0x6f,
    0x70,0x71,0x72,0x73,0x74,0x75,0x76,0x77,
    0x78,0x79,0x7a,0x7b,0x7c,0x7d,0x7e,0x7f,
    0x80,0x81,0x82,0x83,0x84,0x85,0x86,0x87,
    0x88,0x89,0x8a,0x8b,0x8c,0x8d,0x8e,0x8f,
    0x90,0x91,0x92,0x93,0x94,0x95,0x96,0x97,
    0x98,0x99,0x9a,0x9b,0x9c,0x9d,0x9e,0x9f,
    0xa0,0xa1,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,
    0xa8,0xa9,0xaa,0xab,0xac,0xad,0xae,0xaf,
    0xb0,0xb1,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,
    0xb8,0xb9,0xba,0xbb,0xbc,0xbd,0xbe,0xbf,
    0xc0,0xc1,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7,
    0xc8,0xc9,0xca,0xcb,0xcc,0xcd,0xce,0xcf,
    0xd0,0xd1,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,
    0xd8,0xd9,0xda,0xdb,0xdc,0xdd,0xde,0xdf,
    0xe0,0xe1,0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,
    0xe8,0xe9,0xea,0xeb,0xec,0xed,0xee,0xef,
    0xf0,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,
    0xf8,0xf9,0xfa,0xfb,0xfc,0xfd,0xfe,0xff,
};
static const short *tolower_tab_ptr = C_tolower_tab;

/* Android __sF is an array of embedded bionic FILE structs (~84 bytes each).
 * Allocate enough space so __sF[1] (stdout) lands inside our buffer.
 * resolve_stream() maps bionic-fake addresses back to real glibc streams. */
#define BIONIC_FILE_SIZE 84
static char  sF_fake[3 * BIONIC_FILE_SIZE];
static FILE *stderr_fake;
static int   stack_chk_guard_fake = 0x42424242;

/* ctype_ pointer: provided via android_ctype_table below */

/* ── __assert2 (Android assertion handler) ───────────────────────────────── */
static void __assert2_impl(const char *file, int line,
                            const char *func, const char *expr) {
    fprintf(stderr, "ASSERT FAIL: %s:%d %s(): %s\n", file, line, func, expr);
    fflush(stderr);
    /* Don't call real abort() — that raises SIGABRT through glibc internals
     * bypassing our raise_hook. Just hang so we can see the log. */
    for (;;) usleep(1000000);
}

static void abort_hook(void) {
    (void)!write(2, "ABORT_HOOK\n", 11);  /* confirm hook fires */
    void *bt[32];
    int n = backtrace(bt, 32);
    fprintf(stderr, "abort() intercepted from game (caller=%p):\n",
            __builtin_return_address(0));
    char **syms = backtrace_symbols(bt, n);
    for (int i = 0; i < n; i++)
        fprintf(stderr, "  bt[%d] %s\n", i, syms ? syms[i] : "?");
    free(syms);
    fflush(stderr);
    /* Swallow — loop so execution doesn't continue off the end */
    for (;;) usleep(1000000);
}

/* Intercept raise() from libGTALcs.so: log and swallow.
 * bionic's abort() calls raise(SIGABRT); some internal crash paths call raise(SIGSEGV).
 * Swallowing lets us observe what happens next instead of dying immediately. */
static int raise_hook(int sig) {
    (void)!write(2, "RAISE_HOOK\n", 11);  /* async-signal-safe confirm */
    void *bt[32];
    int n = backtrace(bt, 32);
    fprintf(stderr, "raise(%d) intercepted from game (caller=%p):\n",
            sig, __builtin_return_address(0));
    char **syms = backtrace_symbols(bt, n);
    for (int i = 0; i < n; i++)
        fprintf(stderr, "  bt[%d] %s\n", i, syms ? syms[i] : "?");
    free(syms);
    fflush(stderr);
    return 0;  /* swallow — do not deliver signal */
}

/* ── __gnu_Unwind_Find_exidx (ARM EHABI — no C++ exceptions needed) ──────── */
static void *__gnu_Unwind_Find_exidx_stub(void *pc, int *pcount) {
    (void)pc;
    if (pcount) *pcount = 0;
    return NULL;
}

/* ── Bionic-compatible _ctype_ table ─────────────────────────────────────── */
/* BSD/bionic bit flags: _U=0x01 _L=0x02 _N=0x04 _S=0x08 _P=0x10 _C=0x20
 *                                              ^^^^^^^^        ^^^^^^^^
 * This comment used to have _S and _C the other way round, and the table was
 * built to match it, so `isspace(' ')` was false and control characters read as
 * space.  Pinned by the binary itself: the 31 case-folding sites test 0x02
 * (_L), which identifies the header as the BSD-style one, and the only other
 * flag the engine ever tests is 0x08 — in std::__convert_to_v, where it is
 * isspace.  Blast radius of the correction is exactly those two sites.        */
static const char android_ctype_table[257] = {
    0,                                                /* [0]   EOF */
    0x20,                                             /* [1]   0x00 NUL   ctrl */
    0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,         /* [2-9] 0x01-0x08  ctrl */
    0x28,0x28,0x28,0x28,0x28,                         /* [10-14] 0x09-0x0D HT/LF/VT/FF/CR space+ctrl */
    0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,         /* [15-22] 0x0E-0x15 ctrl */
    0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20, /* [23-32] 0x16-0x1F ctrl */
    0x88,                                             /* [33]  0x20 SP    space+blank */
    0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10, /* ! " # ... / */
    0x44,0x44,0x44,0x44,0x44,0x44,0x44,0x44,0x44,0x44, /* 0-9 digit+hex */
    0x10,0x10,0x10,0x10,0x10,0x10,0x10,              /* : ; < = > ? @ */
    0x41,0x41,0x41,0x41,0x41,0x41,                   /* A-F upper+hex */
    0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01, /* G-Z upper */
    0x10,0x10,0x10,0x10,0x10,0x10,                   /* [ \ ] ^ _ ` */
    0x42,0x42,0x42,0x42,0x42,0x42,                   /* a-f lower+hex */
    0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02, /* g-z lower */
    0x10,0x10,0x10,0x10,0x20,                        /* { | } ~ DEL */
    /* 0x80-0xFF: non-ASCII */
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
};
/* _ctype_ points at the BASE of the table, not at [1].
 *
 * This was the vinc_01 crash.  Bionic's ctype macros — and the code libGTALcs.so
 * has them inlined into — index the table as `ptr[c + 1]`, with [0] reserved for
 * EOF.  Pointing _ctype_ at `table + 1` so that `ptr[c]` looked right made every
 * lookup in the engine read the flags of character **c+1**.
 *
 * The only lowercase letter that then fails to fold is 'z' ('z'+1 == '{', which
 * is punctuation, not lower), so CTexListStore::FindTexListSlot's case-insensitive
 * compare — @0x48b828, `flags = ip[c+1]; if (flags & _L) c -= 32;` — matched every
 * name in the game except one containing a 'z'.
 *
 * DEFAULT.IDE declares vehicle 182 as `toyz` (lowercase) and the archive holds
 * `TOYZ.TXD` (uppercase).  So CBaseModelInfo::SetTexList added slot "toyz" and
 * LoadCdDirectory failed to find it, added a SECOND slot "TOYZ", and positioned
 * the archive data there.  The model pointed at the empty slot, its dictionary
 * stayed NULL, ConvertBufferToObject returned 0 — and a 0 return abandons the
 * whole request queue, starving vinc_01 behind it and handing CPed::SetModelIndex
 * a model with no clump.
 *
 * GTALCS_CTYPE_OLD=1 restores the off-by-one so the failure can be reproduced on
 * demand — a fix is only believable against a harness that shows the bug first. */
static const char *ctype_ptr_val = android_ctype_table;

/* ── ImmVibe stubs (haptics — stub as no-ops) ────────────────────────────── */

static int ImmVibeInitialize2(void *p)                               { (void)p; return 0; }
static int ImmVibeOpenDevice(int d, int *h)                          { (void)d; if(h)*h=0; return 0; }
static int ImmVibeCloseDevice(int h)                                 { (void)h; return 0; }
static int ImmVibeTerminate(void)                                     { return 0; }
static int ImmVibePlayUHLEffect(int h, int e, int i, int *p)         { (void)h;(void)e;(void)i;(void)p; return 0; }
static int ImmVibeStopPlayingEffect(int h, int e)                    { (void)h;(void)e; return 0; }
static int ImmVibeGetEffectState(int h, int e, int *s)               { (void)h;(void)e; if(s)*s=0; return 0; }
static int ImmVibeGetIVTEffectIndexFromName(void *d, void *n, int *i){ (void)d;(void)n;(void)i; return 0; }

/* resolve_stream: bionic __sF[n] lands inside sF_fake[] — map back to glibc streams. */
static FILE *resolve_stream(FILE *s) {
    ptrdiff_t off = (char *)s - sF_fake;
    if (off >= 0 && off < (ptrdiff_t)sizeof(sF_fake)) {
        int idx = (int)(off / BIONIC_FILE_SIZE);
        if (idx == 0) return stdin;
        if (idx == 1) return stdout;
        if (idx == 2) return stderr;
    }
    return s;
}

static int    fclose_fake(FILE *s)                               { return fclose(resolve_stream(s)); }
static int    feof_fake(FILE *s)                                 { return feof(resolve_stream(s)); }
static int    fflush_fake(FILE *s)                               { return fflush(resolve_stream(s)); }
static int    fgetc_fake(FILE *s)                                { return fgetc(resolve_stream(s)); }
static char  *fgets_fake(char *b, int n, FILE *s)                { return fgets(b, n, resolve_stream(s)); }
static int    fprintf_fake(FILE *s, const char *fmt, ...)        { va_list ap; va_start(ap, fmt); int r = vfprintf(resolve_stream(s), fmt, ap); va_end(ap); return r; }
static int    fputc_fake(int c, FILE *s)                         { return fputc(c, resolve_stream(s)); }
static int    fputs_fake(const char *b, FILE *s)                 { return fputs(b, resolve_stream(s)); }
static wint_t fputwc_fake(wchar_t c, FILE *s)                   { return fputwc(c, resolve_stream(s)); }
static size_t fread_fake(void *p, size_t sz, size_t n, FILE *s)  { return fread(p, sz, n, resolve_stream(s)); }
static int    fseek_fake(FILE *s, long o, int w)                 { return fseek(resolve_stream(s), o, w); }
static long   ftell_fake(FILE *s)                                { return ftell(resolve_stream(s)); }

/* fwrite_safe: glibc's _IO_fwrite uses NEON/ldm which faults on misaligned src.
 * Copy misaligned source buffers to the heap before passing to fwrite. */
static size_t fwrite_safe(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    stream = resolve_stream(stream);
    /* ptr in loader binary range is always wrong — game heap is at 0xec000000+ */
    if ((uintptr_t)ptr < 0x10000000) {
        fprintf(stderr, "fwrite_safe: suspicious ptr=%p size=%zu nmemb=%zu stream=%p LR=%p\n",
                ptr, size, nmemb, (void *)stream,
                __builtin_return_address(0));
        fflush(stderr);
        return 0;
    }
    if ((uintptr_t)ptr & 7) {
        size_t total = size * nmemb;
        void *buf = malloc(total);
        if (buf) {
            memcpy(buf, ptr, total);
            size_t ret = fwrite(buf, size, nmemb, stream);
            free(buf);
            return ret;
        }
        return 0;  /* malloc failed, can't safely write unaligned buf */
    }
    return fwrite(ptr, size, nmemb, stream);
}

static FILE *fopen_fake(const char *path, const char *mode);
static int   pthread_kill_fake(pthread_t thread, int sig);
static int   open_fake(const char *path, int flags, ...);

/* ── Softfp ABI thunks for math functions ────────────────────────────────── *
 * libGTALcs.so (Android armeabi-v7a) uses soft-float calling convention:        *
 * scalars in integer registers r0-r3.  System libm uses hard-float (VFP).    *
 * These thunks (pcs("aapcs")) receive args from int registers and return      *
 * results in int registers, bridging to/from the hard-float system functions. */
static SOFTFP float  acosf_abi(float x)                  { return acosf(x);       }
static SOFTFP float  asinf_abi(float x)                  { return asinf(x);       }
static SOFTFP double atan_abi(double x)                   { return atan(x);        }
static SOFTFP float  atan2f_abi(float y, float x)         { return atan2f(y, x);   }
static SOFTFP float  atanf_abi(float x)                   { return atanf(x);       }
static SOFTFP double atof_abi(const char *s)              { return atof(s);        }
static SOFTFP double cos_abi(double x)                    { return cos(x);         }
static SOFTFP float  cosf_abi(float x)                    { return cosf(x);        }
static SOFTFP double exp_abi(double x)                    { return exp(x);         }
static SOFTFP double exp2_abi(double x)                   { return exp2(x);        }
static SOFTFP float  expf_abi(float x)                    { return expf(x);        }
static SOFTFP double floor_abi(double x)                  { return floor(x);       }
static SOFTFP float  floorf_abi(float x)                  { return floorf(x);      }
static SOFTFP float  log10f_abi(float x)                  { return log10f(x);      }
static SOFTFP float  logf_abi(float x)                    { return logf(x);        }
static SOFTFP double pow_abi(double x, double y)          { return pow(x, y);      }
static SOFTFP float  powf_abi(float x, float y)           { return powf(x, y);     }
static SOFTFP double sin_abi(double x)                    { return sin(x);         }
static SOFTFP float  sinf_abi(float x)                    { return sinf(x);        }
static SOFTFP double tan_abi(double x)                    { return tan(x);         }
static SOFTFP float  strtof_abi(const char *s, char **e)  { return strtof(s, e);   }

/* ══════════════════════════════════════════════════════════════════════════
 * LCS-specific shims — symbols Chinatown Wars never imported
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── Android logging (the three beyond __android_log_print) ──────────────── */

int __android_log_write(int prio, const char *tag, const char *text) {
    (void)prio;
    fprintf(stderr, "[%s] %s\n", tag ? tag : "?", text ? text : "");
    return 0;
}

int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list ap) {
    (void)prio;
    fprintf(stderr, "[%s] ", tag ? tag : "?");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    return 0;
}

/* Bionic's assert handler: logs and aborts.  Keep the abort — a failed engine
 * assertion means state is already wrong, and continuing hides the cause. */
void __android_log_assert(const char *cond, const char *tag,
                          const char *fmt, ...) {
    fprintf(stderr, "[%s] ASSERT FAILED: %s\n", tag ? tag : "?",
            cond ? cond : "(null)");
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        fputc('\n', stderr);
        va_end(ap);
    }
    fflush(stderr);
    abort();
}

/* ── Soft-float bridges for the libm/stdlib entry points LCS adds ────────── */

static SOFTFP double acos_abi(double x)                    { return acos(x);       }
static SOFTFP double ceil_abi(double x)                    { return ceil(x);       }
static SOFTFP float  ceilf_abi(float x)                    { return ceilf(x);      }
static SOFTFP float  fmodf_abi(float x, float y)           { return fmodf(x, y);   }
static SOFTFP double frexp_abi(double x, int *e)           { return frexp(x, e);   }
static SOFTFP double modf_abi(double x, double *ip)        { return modf(x, ip);   }
static SOFTFP double sqrt_abi(double x)                    { return sqrt(x);       }
static SOFTFP float  tanf_abi(float x)                     { return tanf(x);       }
static SOFTFP double strtod_abi(const char *s, char **e)   { return strtod(s, e);  }

/* glBlendColor is the one GLES2 entry point LCS adds that takes floats. */
static SOFTFP void glBlendColor_abi(float r, float g, float b, float a) {
    glBlendColor(r, g, b, a);
}

/* ── NULL-tolerant string helpers ────────────────────────────────────────────
 * The engine does the usual GL capability probe —
 *     strstr(glGetString(GL_EXTENSIONS), "GL_OES_...")
 * — with no null check.  If the driver hands back NULL (no context current on
 * the calling thread, or a restricted/offscreen context) glibc's strstr walks
 * a null pointer and the process dies inside libc, far from the real cause.
 *
 * Bionic's strstr is equally undefined for NULL, so the game never had a
 * guarantee here; it simply never hit the case on Android.  Tolerating it
 * costs one branch and converts a hard crash into a clean "extension not
 * found", which is the answer the probe actually wants. */
static char *strstr_safe(const char *h, const char *n) {
    if (!h || !n) return NULL;
    return strstr(h, n);
}
int g_cd_entries;   /* CD directory entries parsed (see below) */
static char *strchr_safe(const char *h, int c) {
    if (!h) return NULL;
    char *r = strchr(h, c);
    /* CStreaming::LoadCdDirectory does strchr(entry_name, '.') and then writes
     * a NUL through the result with NO null check — so an extension-less entry
     * is an immediate store to address 0.  Log the haystack when that is about
     * to happen; the name is what says whether the directory data is real,
     * empty, or garbage. */
    /* Only the LoadCdDirectory call site matters: every other caller of
     * strchr(x,'.') in the engine checks the result.  Its return address is
     * libGTALcs.so+0x478350 (the instruction after `bl strchr@plt` at
     * +0x47834c), and the very next instruction stores through it. */
    /* CStreaming::LoadCdDirectory calls strchr(entry_name, '.') once per CD
     * directory entry, so this call site is a free entry counter — and the
     * count is the question that matters.  If the directory is only partly
     * parsed, every model beyond the cut is "not in the CD image",
     * RequestModel silently does nothing, the world cannot stream (draw calls
     * decay, the stream thread sits idle) and a script CREATE_CHAR eventually
     * hands CPed::SetModelIndex a model whose clump was never loaded. */
    {
        uintptr_t ra0 = (uintptr_t)__builtin_return_address(0);
        if (c == '.' && ra0 - gtalcs_mod.load_bias == 0x478350) {
            g_cd_entries++;
            if (g_cd_entries <= 3 || (g_cd_entries % 2000) == 0)
                fprintf(stderr, "[cd] entry %d: \"%s\"\n", g_cd_entries, h);
            /* GTALCS_CD_TRACE=<substr>: show every directory entry whose name
             * contains this, case-insensitively.  Used to answer whether
             * LoadCdDirectory ever SEES an entry — TOYZ.TXD is in gta3.dir at
             * 0xd9e7 yet its TXD slot 5054 ends up with no cd position at all,
             * and "never parsed" and "parsed but not matched to a slot" are
             * different bugs. */
            static const char *trace;
            static int trace_init;
            if (!trace_init) { trace_init = 1; trace = getenv("GTALCS_CD_TRACE"); }
            if (trace && *trace && strcasestr(h, trace))
                fprintf(stderr, "[cd] TRACE entry %d: \"%s\"\n", g_cd_entries, h);
        }
    }
    if (!r && c == '.') {
        uintptr_t ra  = (uintptr_t)__builtin_return_address(0);
        uintptr_t off = ra - gtalcs_mod.load_bias;
        static int logged;
        if (off == 0x478350 && logged < 40) {
            logged++;
            char esc[80]; int n = 0;
            for (const unsigned char *p = (const unsigned char *)h;
                 *p && n < (int)sizeof(esc) - 5; p++) {
                if (*p >= 0x20 && *p < 0x7f) esc[n++] = (char)*p;
                else n += snprintf(esc + n, sizeof(esc) - n, "\\x%02x", *p);
            }
            esc[n] = 0;
            fprintf(stderr, "[strchr] LoadCdDirectory: no '.' in \"%s\" (len=%zu)"
                            " — first 32 bytes: ", esc, strlen(h));
            for (int i = 0; i < 32; i++)
                fprintf(stderr, "%02x ", ((const unsigned char *)h)[i]);
            fprintf(stderr, "\n");
        }
    }
    return r;
}
static int strcmp_safe(const char *a, const char *b) {
    if (!a || !b) return a == b ? 0 : (a ? 1 : -1);
    return strcmp(a, b);
}

/* glGetString must never return NULL to the game, for the same reason. */
static const unsigned char *glGetString_safe(unsigned int name) {
    const unsigned char *r = glGetString(name);
    if (!r) {
        fprintf(stderr, "[gl] glGetString(0x%04x) returned NULL -> \"\" "
                        "(no current context on this thread?)\n", name);
        return (const unsigned char *)"";
    }
    return r;
}

/* ── FILE*-taking calls: must go through resolve_stream ──────────────────────
 * A bionic __sF address must never reach glibc.  Every one of these takes a
 * FILE*, so each needs the same mapping the CTW port established. */

static int     getc_fake(FILE *s)                        { return getc(resolve_stream(s)); }
static int     putc_fake(int c, FILE *s)                 { return putc(c, resolve_stream(s)); }
static void    rewind_fake(FILE *s)                      { rewind(resolve_stream(s)); }
static int     setvbuf_fake(FILE *s, char *b, int m, size_t n) { return setvbuf(resolve_stream(s), b, m, n); }
static int     ungetc_fake(int c, FILE *s)               { return ungetc(c, resolve_stream(s)); }
static wint_t  getwc_fake(FILE *s)                       { return getwc(resolve_stream(s)); }
static wint_t  putwc_fake(wchar_t c, FILE *s)            { return putwc(c, resolve_stream(s)); }
static wint_t  ungetwc_fake(wint_t c, FILE *s)           { return ungetwc(c, resolve_stream(s)); }
/* fdopen returns a fresh glibc stream, so it needs no mapping on the way in. */
static FILE   *fdopen_fake(int fd, const char *mode)     { return fdopen(fd, mode); }

/* ── fstat: see stat_hook above; this is the one the archive reader uses ──── */
static int fstat_hook(int fd, void *statbuf) {
    struct stat64 st;
    int r = fstat64(fd, &st);
    if (r == 0) {
        stat64_to_bionic(&st, (struct bionic_stat *)statbuf);
        static int logged;
        if (logged < 8) {
            logged++;
            fprintf(stderr, "[fstat] fd=%d size=%lld mode=%o\n",
                    fd, (long long)st.st_size, (unsigned)st.st_mode);
        }
    }
    return r;
}

/* ── dlsym: the .so imports dlsym but never dlopen ───────────────────────────
 * So every call is effectively dlsym(RTLD_DEFAULT, name).  Search our own
 * resolver table first, then the host.  A silent NULL here becomes a call
 * through a null pointer much later and is miserable to trace, so log misses. */
static so_default_dynlib *g_dynlib_all      = NULL;
static int                g_dynlib_all_count = 0;

static void *dlsym_hook(void *handle, const char *name) {
    (void)handle;
    if (!name) return NULL;
    for (int i = 0; i < g_dynlib_all_count; i++)
        if (strcmp(g_dynlib_all[i].symbol, name) == 0)
            return (void *)g_dynlib_all[i].func;

    void *host = dlsym(RTLD_DEFAULT, name);
    if (!host)
        fprintf(stderr, "[dlsym] MISS: '%s' -> NULL\n", name);
    return host;
}

/* ── Named semaphores: same 4-byte bionic slot indirection as sem_init ────── */
static sem_t *sem_open_fake(const char *name, int oflag, ...) {
    va_list ap;
    va_start(ap, oflag);
    mode_t mode  = (mode_t)va_arg(ap, int);
    unsigned val = va_arg(ap, unsigned);
    va_end(ap);
    return sem_open(name, oflag, mode, val);
}
static int sem_close_fake(sem_t **s)  { return (s && *s) ? sem_close(*s) : 0; }
static int sem_unlink_fake(const char *name) { return sem_unlink(name); }

/* ── pthread_attr_setdetachstate on our own bionic-sized attr ─────────────── */
static int pthread_attr_setdetachstate_fake(bionic_attr_t *a, int state) {
    if (a) a->flags = (uint32_t)state;
    return 0;
}

/* ── Bionic data exports ─────────────────────────────────────────────────── */

/* bionic exposes the page size as an int global, not sysconf(). */
static int bionic_page_size = 4096;

static const short C_toupper_tab[257] = {
    -1,
    0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
    0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
    0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
    0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f,
    0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27,
    0x28,0x29,0x2a,0x2b,0x2c,0x2d,0x2e,0x2f,
    0x30,0x31,0x32,0x33,0x34,0x35,0x36,0x37,
    0x38,0x39,0x3a,0x3b,0x3c,0x3d,0x3e,0x3f,
    0x40,0x41,0x42,0x43,0x44,0x45,0x46,0x47,
    0x48,0x49,0x4a,0x4b,0x4c,0x4d,0x4e,0x4f,
    0x50,0x51,0x52,0x53,0x54,0x55,0x56,0x57,
    0x58,0x59,0x5a,0x5b,0x5c,0x5d,0x5e,0x5f,
    0x60,'A','B','C','D','E','F','G',
    'H','I','J','K','L','M','N','O',
    'P','Q','R','S','T','U','V','W',
    'X','Y','Z',0x7b,0x7c,0x7d,0x7e,0x7f,
    0x80,0x81,0x82,0x83,0x84,0x85,0x86,0x87,
    0x88,0x89,0x8a,0x8b,0x8c,0x8d,0x8e,0x8f,
    0x90,0x91,0x92,0x93,0x94,0x95,0x96,0x97,
    0x98,0x99,0x9a,0x9b,0x9c,0x9d,0x9e,0x9f,
    0xa0,0xa1,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,
    0xa8,0xa9,0xaa,0xab,0xac,0xad,0xae,0xaf,
    0xb0,0xb1,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,
    0xb8,0xb9,0xba,0xbb,0xbc,0xbd,0xbe,0xbf,
    0xc0,0xc1,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7,
    0xc8,0xc9,0xca,0xcb,0xcc,0xcd,0xce,0xcf,
    0xd0,0xd1,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,
    0xd8,0xd9,0xda,0xdb,0xdc,0xdd,0xde,0xdf,
    0xe0,0xe1,0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,
    0xe8,0xe9,0xea,0xeb,0xec,0xed,0xee,0xef,
    0xf0,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,
    0xf8,0xf9,0xfa,0xfb,0xfc,0xfd,0xfe,0xff,
};
static const short *toupper_tab_ptr = C_toupper_tab;

/* ── ARM EHABI / libgcc ──────────────────────────────────────────────────────
 * The game was built with exceptions enabled and imports the unwinder.  libgcc
 * already provides every one of these; we only need their addresses, and they
 * have no public header, so declare them opaquely. */
extern void _Unwind_Complete(void);
extern void _Unwind_DeleteException(void);
extern void _Unwind_GetDataRelBase(void);
extern void _Unwind_GetLanguageSpecificData(void);
extern void _Unwind_GetRegionStart(void);
extern void _Unwind_GetTextRelBase(void);
extern void _Unwind_RaiseException(void);
extern void _Unwind_Resume(void);
extern void _Unwind_Resume_or_Rethrow(void);
extern void _Unwind_VRS_Get(void);
extern void _Unwind_VRS_Set(void);
extern void __gnu_unwind_frame(void);
extern void __aeabi_unwind_cpp_pr0(void);
extern void __aeabi_unwind_cpp_pr1(void);

/* Integer/float helper routines, likewise from libgcc. */
extern void __aeabi_idiv(void);
extern void __aeabi_idivmod(void);
extern void __aeabi_uidiv(void);
extern void __aeabi_uidivmod(void);
extern void __aeabi_uldivmod(void);
extern void __aeabi_l2f(void);
extern void __aeabi_ul2d(void);
extern void __aeabi_ul2f(void);
extern void __aeabi_d2ulz(void);


/* ── Symbol table ────────────────────────────────────────────────────────────
 * Every undefined symbol in libGTALcs.so (408 of them) must appear here or
 * in one of the module tables, or the game calls through a null pointer at the
 * first use.  `make check-syms` prints the authoritative list from the binary.
 *
 * 242 entries carry over from the Chinatown Wars port unchanged, 131 are new
 * here, and 35 (EGL and OpenAL) are resolved by egl_patch.c and
 * openal_patch.c in later so_resolve passes.
 */

static so_default_dynlib default_dynlib[] = {
    /* DELIBERATELY ret0 — do not "fix" this by wiring up asset_manager.c.
     *
     * It looks like a bug: src/asset_manager.c implements all seven entry
     * points over the extracted assets/ tree, and binding them here makes the
     * dead code live.  Tried on 2026-09-08; it broke a working build, crashing
     * during boot before the first frame.  Two reasons:
     *
     *   1. The engine passes ABSOLUTE paths to AAssetManager_open — the first
     *      call is the OBB itself — and the shim prefixes the assets root,
     *      producing ".../assets//roms/ports/gtalcs/main.17….obb".
     *   2. More fundamentally, a non-NULL AAssetManager_fromJava changes which
     *      loader the engine chooses.  With NULL it falls through to fopen and
     *      its own OBB reader, which is the path that works and is complete.
     *
     * The APK assets/ tree is NOT unreachable as a result: it is served
     * through the fake-JNI getFile() up-call instead — the boot log shows
     * `[getFile] .../assets/json/socialClubAssets.json (4295 bytes)`.
     * asset_manager.c is kept because it is correct and may be needed if a
     * future screen uses the NDK API with relative paths, but it stays
     * unbound until something demonstrably asks for it. */
    { "AAssetManager_fromJava",            (uintptr_t)ret0 },
    { "AAssetManager_open",                (uintptr_t)ret0 },
    { "AAsset_close",                      (uintptr_t)ret0 },
    { "AAsset_getLength",                  (uintptr_t)ret0 },
    { "AAsset_getRemainingLength",         (uintptr_t)ret0 },
    { "AAsset_read",                       (uintptr_t)ret0 },
    { "AAsset_seek",                       (uintptr_t)ret0 },
    { "ImmVibeCloseDevice",                (uintptr_t)ImmVibeCloseDevice },
    { "ImmVibeGetEffectState",             (uintptr_t)ImmVibeGetEffectState },
    { "ImmVibeInitialize2",                (uintptr_t)ImmVibeInitialize2 },
    { "ImmVibeOpenDevice",                 (uintptr_t)ImmVibeOpenDevice },
    { "ImmVibePlayUHLEffect",              (uintptr_t)ImmVibePlayUHLEffect },
    { "ImmVibeStopPlayingEffect",          (uintptr_t)ImmVibeStopPlayingEffect },
    { "ImmVibeTerminate",                  (uintptr_t)ImmVibeTerminate },
    { "_Unwind_Complete",                  (uintptr_t)&_Unwind_Complete },
    { "_Unwind_DeleteException",           (uintptr_t)&_Unwind_DeleteException },
    { "_Unwind_GetDataRelBase",            (uintptr_t)&_Unwind_GetDataRelBase },
    { "_Unwind_GetLanguageSpecificData",   (uintptr_t)&_Unwind_GetLanguageSpecificData },
    { "_Unwind_GetRegionStart",            (uintptr_t)&_Unwind_GetRegionStart },
    { "_Unwind_GetTextRelBase",            (uintptr_t)&_Unwind_GetTextRelBase },
    { "_Unwind_RaiseException",            (uintptr_t)&_Unwind_RaiseException },
    { "_Unwind_Resume",                    (uintptr_t)&_Unwind_Resume },
    { "_Unwind_Resume_or_Rethrow",         (uintptr_t)&_Unwind_Resume_or_Rethrow },
    { "_Unwind_VRS_Get",                   (uintptr_t)&_Unwind_VRS_Get },
    { "_Unwind_VRS_Set",                   (uintptr_t)&_Unwind_VRS_Set },
    { "__aeabi_d2ulz",                     (uintptr_t)&__aeabi_d2ulz },
    { "__aeabi_idiv",                      (uintptr_t)&__aeabi_idiv },
    { "__aeabi_idivmod",                   (uintptr_t)&__aeabi_idivmod },
    { "__aeabi_l2f",                       (uintptr_t)&__aeabi_l2f },
    { "__aeabi_uidiv",                     (uintptr_t)&__aeabi_uidiv },
    { "__aeabi_uidivmod",                  (uintptr_t)&__aeabi_uidivmod },
    { "__aeabi_ul2d",                      (uintptr_t)&__aeabi_ul2d },
    { "__aeabi_ul2f",                      (uintptr_t)&__aeabi_ul2f },
    { "__aeabi_uldivmod",                  (uintptr_t)&__aeabi_uldivmod },
    { "__aeabi_unwind_cpp_pr0",            (uintptr_t)&__aeabi_unwind_cpp_pr0 },
    { "__aeabi_unwind_cpp_pr1",            (uintptr_t)&__aeabi_unwind_cpp_pr1 },
    { "__android_log_assert",              (uintptr_t)&__android_log_assert },
    { "__android_log_print",               (uintptr_t)__android_log_print },
    { "__android_log_vprint",              (uintptr_t)&__android_log_vprint },
    { "__android_log_write",               (uintptr_t)&__android_log_write },
    { "__assert2",                         (uintptr_t)__assert2_impl },
    { "__cxa_atexit",                      (uintptr_t)__cxa_atexit },
    { "__cxa_finalize",                    (uintptr_t)__cxa_finalize },
    { "__errno",                           (uintptr_t)__errno_location },
    { "__gnu_unwind_frame",                (uintptr_t)&__gnu_unwind_frame },
    { "__page_size",                       (uintptr_t)&bionic_page_size },
    { "__sF",                              (uintptr_t)sF_fake },
    { "__stack_chk_fail",                  (uintptr_t)abort },
    { "__stack_chk_guard",                 (uintptr_t)&stack_chk_guard_fake },
    { "_ctype_",                           (uintptr_t)&ctype_ptr_val },
    { "_exit",                             (uintptr_t)&_exit },
    { "_tolower_tab_",                     (uintptr_t)&tolower_tab_ptr },
    { "_toupper_tab_",                     (uintptr_t)&toupper_tab_ptr },
    { "abort",                             (uintptr_t)abort_hook },
    { "accept",                            (uintptr_t)&accept },
    { "acos",                              (uintptr_t)&acos_abi },
    { "acosf",                             (uintptr_t)acosf_abi },
    { "asinf",                             (uintptr_t)asinf_abi },
    { "atan",                              (uintptr_t)atan_abi },
    { "atan2f",                            (uintptr_t)atan2f_abi },
    { "atanf",                             (uintptr_t)atanf_abi },
    { "atoi",                              (uintptr_t)atoi },
    { "bind",                              (uintptr_t)&bind },
    { "bsearch",                           (uintptr_t)&bsearch },
    { "btowc",                             (uintptr_t)&btowc },
    { "calloc",                            (uintptr_t)ga_calloc },
    { "ceil",                              (uintptr_t)&ceil_abi },
    { "ceilf",                             (uintptr_t)&ceilf_abi },
    { "clock_gettime",                     (uintptr_t)&bionic_clock_gettime },
    { "close",                             (uintptr_t)close },
    { "connect",                           (uintptr_t)&connect },
    { "cos",                               (uintptr_t)cos_abi },
    { "cosf",                              (uintptr_t)cosf_abi },
    { "dlsym",                             (uintptr_t)&dlsym_hook },
    { "expf",                              (uintptr_t)expf_abi },
    { "fclose",                            (uintptr_t)fclose_fake },
    { "fcntl",                             (uintptr_t)&fcntl },
    { "fdopen",                            (uintptr_t)&fdopen_fake },
    { "fflush",                            (uintptr_t)fflush_fake },
    { "floor",                             (uintptr_t)floor_abi },
    { "floorf",                            (uintptr_t)floorf_abi },
    { "fmodf",                             (uintptr_t)&fmodf_abi },
    { "fopen",                             (uintptr_t)fopen_fake },
    { "fprintf",                           (uintptr_t)fprintf_fake },
    { "fputc",                             (uintptr_t)fputc_fake },
    { "fputs",                             (uintptr_t)fputs_fake },
    { "fread",                             (uintptr_t)fread_fake },
    { "free",                              (uintptr_t)ga_free },
    { "frexp",                             (uintptr_t)&frexp_abi },
    { "fseek",                             (uintptr_t)fseek_fake },
    { "fstat",                             (uintptr_t)&fstat_hook },
    { "ftell",                             (uintptr_t)ftell_fake },
    { "ftruncate",                         (uintptr_t)&ftruncate },
    { "fwrite",                            (uintptr_t)fwrite_safe },
    { "getaddrinfo",                       (uintptr_t)&getaddrinfo },
    { "getc",                              (uintptr_t)&getc_fake },
    { "getpid",                            (uintptr_t)&getpid },
    { "gettimeofday",                      (uintptr_t)&bionic_gettimeofday },
    { "getwc",                             (uintptr_t)&getwc_fake },
    { "glActiveTexture",                   (uintptr_t)glActiveTexture },
    { "glAttachShader",                    (uintptr_t)glAttachShader },
    { "glBindAttribLocation",              (uintptr_t)glBindAttribLocationHook },
    { "glBindBuffer",                      (uintptr_t)glBindBuffer },
    { "glBindFramebuffer",                 (uintptr_t)glBindFramebufferHook },
    { "glBindRenderbuffer",                (uintptr_t)glBindRenderbuffer },
    { "glBindTexture",                     (uintptr_t)glBindTexture },
    { "glBlendColor",                      (uintptr_t)&glBlendColor_abi },
    { "glBlendEquation",                   (uintptr_t)glBlendEquation },
    { "glBlendEquationSeparate",           (uintptr_t)glBlendEquationSeparate },
    { "glBlendFunc",                       (uintptr_t)glBlendFuncHook },
    { "glBlendFuncSeparate",               (uintptr_t)glBlendFuncSeparate },
    { "glBufferData",                      (uintptr_t)glBufferData },
    { "glBufferSubData",                   (uintptr_t)glBufferSubData },
    { "glCheckFramebufferStatus",          (uintptr_t)glCheckFramebufferStatus },
    { "glClear",                           (uintptr_t)glClear },
    { "glClearColor",                      (uintptr_t)glClearColorHook },
    { "glClearDepthf",                     (uintptr_t)glClearDepthf_abi },
    { "glClearStencil",                    (uintptr_t)glClearStencil },
    { "glColorMask",                       (uintptr_t)glColorMask },
    { "glCompileShader",                   (uintptr_t)glCompileShaderHook },
    { "glCompressedTexImage2D",            (uintptr_t)glCompressedTexImage2DHook },
    { "glCompressedTexSubImage2D",         (uintptr_t)&glCompressedTexSubImage2D },
    { "glCopyTexImage2D",                  (uintptr_t)glCopyTexImage2D },
    { "glCopyTexSubImage2D",               (uintptr_t)glCopyTexSubImage2D },
    { "glCreateProgram",                   (uintptr_t)glCreateProgram },
    { "glCreateShader",                    (uintptr_t)glCreateShader },
    { "glCullFace",                        (uintptr_t)glCullFace },
    { "glDeleteBuffers",                   (uintptr_t)glDeleteBuffers },
    { "glDeleteFramebuffers",              (uintptr_t)glDeleteFramebuffers },
    { "glDeleteProgram",                   (uintptr_t)glDeleteProgram },
    { "glDeleteRenderbuffers",             (uintptr_t)glDeleteRenderbuffers },
    { "glDeleteShader",                    (uintptr_t)glDeleteShader },
    { "glDeleteTextures",                  (uintptr_t)glDeleteTextures },
    { "glDepthFunc",                       (uintptr_t)glDepthFunc },
    { "glDepthMask",                       (uintptr_t)glDepthMaskHook },
    { "glDepthRangef",                     (uintptr_t)glDepthRangef_abi },
    { "glDetachShader",                    (uintptr_t)glDetachShader },
    { "glDisable",                         (uintptr_t)glDisableHook },
    { "glDisableVertexAttribArray",        (uintptr_t)glDisableVertexAttribArray },
    { "glDrawArrays",                      (uintptr_t)glDrawArraysHook },
    { "glDrawElements",                    (uintptr_t)glDrawElementsHook },
    { "glEnable",                          (uintptr_t)glEnableHook },
    { "glEnableVertexAttribArray",         (uintptr_t)glEnableVertexAttribArray },
    { "glFinish",                          (uintptr_t)glFinish },
    { "glFlush",                           (uintptr_t)glFlush },
    { "glFramebufferRenderbuffer",         (uintptr_t)glFramebufferRenderbuffer },
    { "glFramebufferTexture2D",            (uintptr_t)glFramebufferTexture2DHook },
    { "glFrontFace",                       (uintptr_t)glFrontFace },
    { "glGenBuffers",                      (uintptr_t)glGenBuffers },
    { "glGenFramebuffers",                 (uintptr_t)glGenFramebuffers },
    { "glGenRenderbuffers",                (uintptr_t)glGenRenderbuffers },
    { "glGenTextures",                     (uintptr_t)glGenTextures },
    { "glGenerateMipmap",                  (uintptr_t)glGenerateMipmap },
    { "glGetActiveAttrib",                 (uintptr_t)glGetActiveAttrib },
    { "glGetActiveUniform",                (uintptr_t)glGetActiveUniform },
    { "glGetAttachedShaders",              (uintptr_t)glGetAttachedShaders },
    { "glGetAttribLocation",               (uintptr_t)glGetAttribLocation },
    { "glGetBooleanv",                     (uintptr_t)glGetBooleanv },
    { "glGetBufferParameteriv",            (uintptr_t)glGetBufferParameteriv },
    { "glGetError",                        (uintptr_t)glGetError },
    { "glGetFloatv",                       (uintptr_t)glGetFloatv },
    { "glGetFramebufferAttachmentParameteriv", (uintptr_t)glGetFramebufferAttachmentParameteriv },
    { "glGetIntegerv",                     (uintptr_t)glGetIntegerv },
    { "glGetProgramInfoLog",               (uintptr_t)glGetProgramInfoLog },
    { "glGetProgramiv",                    (uintptr_t)glGetProgramiv },
    { "glGetRenderbufferParameteriv",      (uintptr_t)glGetRenderbufferParameteriv },
    { "glGetShaderInfoLog",                (uintptr_t)glGetShaderInfoLog },
    { "glGetShaderPrecisionFormat",        (uintptr_t)&glGetShaderPrecisionFormat },
    { "glGetShaderSource",                 (uintptr_t)&glGetShaderSource },
    { "glGetShaderiv",                     (uintptr_t)glGetShaderiv },
    { "glGetString",                       (uintptr_t)&glGetString_safe },
    { "glGetTexParameterfv",               (uintptr_t)glGetTexParameterfv },
    { "glGetTexParameteriv",               (uintptr_t)glGetTexParameteriv },
    { "glGetUniformLocation",              (uintptr_t)glGetUniformLocation },
    { "glGetUniformfv",                    (uintptr_t)glGetUniformfv },
    { "glGetUniformiv",                    (uintptr_t)glGetUniformiv },
    { "glGetVertexAttribPointerv",         (uintptr_t)glGetVertexAttribPointerv },
    { "glGetVertexAttribfv",               (uintptr_t)glGetVertexAttribfv },
    { "glGetVertexAttribiv",               (uintptr_t)glGetVertexAttribiv },
    { "glHint",                            (uintptr_t)glHint },
    { "glIsBuffer",                        (uintptr_t)glIsBuffer },
    { "glIsEnabled",                       (uintptr_t)glIsEnabled },
    { "glIsFramebuffer",                   (uintptr_t)&glIsFramebuffer },
    { "glIsRenderbuffer",                  (uintptr_t)&glIsRenderbuffer },
    { "glIsShader",                        (uintptr_t)glIsShader },
    { "glIsTexture",                       (uintptr_t)glIsTexture },
    { "glLineWidth",                       (uintptr_t)glLineWidth_abi },
    { "glLinkProgram",                     (uintptr_t)glLinkProgramHook },
    { "glPixelStorei",                     (uintptr_t)glPixelStorei },
    { "glPolygonOffset",                   (uintptr_t)glPolygonOffset_abi },
    { "glReadPixels",                      (uintptr_t)glReadPixels },
    { "glReleaseShaderCompiler",           (uintptr_t)glReleaseShaderCompiler },
    { "glRenderbufferStorage",             (uintptr_t)glRenderbufferStorage },
    { "glSampleCoverage",                  (uintptr_t)glSampleCoverage_abi },
    { "glScissor",                         (uintptr_t)glScissor },
    { "glShaderBinary",                    (uintptr_t)glShaderBinary },
    { "glShaderSource",                    (uintptr_t)glShaderSourceHook },
    { "glStencilFunc",                     (uintptr_t)glStencilFunc },
    { "glStencilFuncSeparate",             (uintptr_t)&glStencilFuncSeparate },
    { "glStencilMask",                     (uintptr_t)glStencilMask },
    { "glStencilMaskSeparate",             (uintptr_t)&glStencilMaskSeparate },
    { "glStencilOp",                       (uintptr_t)glStencilOp },
    { "glStencilOpSeparate",               (uintptr_t)&glStencilOpSeparate },
    { "glTexImage2D",                      (uintptr_t)glTexImage2DHook },
    { "glTexParameterf",                   (uintptr_t)glTexParameterf_abi },
    { "glTexParameterfv",                  (uintptr_t)glTexParameterfv },
    { "glTexParameteri",                   (uintptr_t)glTexParameteri },
    { "glTexParameteriv",                  (uintptr_t)glTexParameteriv },
    { "glTexSubImage2D",                   (uintptr_t)glTexSubImage2DHook },
    { "glUniform1f",                       (uintptr_t)glUniform1f_abi },
    { "glUniform1fv",                      (uintptr_t)glUniform1fv },
    { "glUniform1i",                       (uintptr_t)glUniform1i },
    { "glUniform1iv",                      (uintptr_t)glUniform1iv },
    { "glUniform2f",                       (uintptr_t)glUniform2f_abi },
    { "glUniform2fv",                      (uintptr_t)glUniform2fv },
    { "glUniform2i",                       (uintptr_t)glUniform2i },
    { "glUniform2iv",                      (uintptr_t)glUniform2iv },
    { "glUniform3f",                       (uintptr_t)glUniform3f_abi },
    { "glUniform3fv",                      (uintptr_t)glUniform3fvHook },
    { "glUniform3i",                       (uintptr_t)&glUniform3i },
    { "glUniform3iv",                      (uintptr_t)glUniform3iv },
    { "glUniform4f",                       (uintptr_t)glUniform4fHook },
    { "glUniform4fv",                      (uintptr_t)glUniform4fvHook },
    { "glUniform4i",                       (uintptr_t)glUniform4i },
    { "glUniform4iv",                      (uintptr_t)glUniform4iv },
    { "glUniformMatrix2fv",                (uintptr_t)glUniformMatrix2fv },
    { "glUniformMatrix3fv",                (uintptr_t)glUniformMatrix3fv },
    { "glUniformMatrix4fv",                (uintptr_t)glUniformMatrix4fvHook },
    { "glUseProgram",                      (uintptr_t)glUseProgramHook },
    { "glValidateProgram",                 (uintptr_t)glValidateProgram },
    { "glVertexAttrib1f",                  (uintptr_t)glVertexAttrib1f_abi },
    { "glVertexAttrib1fv",                 (uintptr_t)&glVertexAttrib1fv },
    { "glVertexAttrib2f",                  (uintptr_t)glVertexAttrib2f_abi },
    { "glVertexAttrib2fv",                 (uintptr_t)&glVertexAttrib2fv },
    { "glVertexAttrib3f",                  (uintptr_t)glVertexAttrib3f_abi },
    { "glVertexAttrib3fv",                 (uintptr_t)&glVertexAttrib3fv },
    { "glVertexAttrib4f",                  (uintptr_t)glVertexAttrib4f_abi },
    { "glVertexAttrib4fv",                 (uintptr_t)glVertexAttrib4fv },
    { "glVertexAttribPointer",             (uintptr_t)glVertexAttribPointerHook },
    { "glViewport",                        (uintptr_t)glViewport },
    { "gmtime",                            (uintptr_t)gmtime },
    { "inet_ntop",                         (uintptr_t)&inet_ntop },
    { "inet_pton",                         (uintptr_t)&inet_pton },
    { "ioctl",                             (uintptr_t)&ioctl },
    { "isalnum",                           (uintptr_t)&isalnum },
    { "isalpha",                           (uintptr_t)&isalpha },
    { "isspace",                           (uintptr_t)isspace },
    { "iswctype",                          (uintptr_t)&iswctype },
    { "listen",                            (uintptr_t)&listen },
    { "log",                               (uintptr_t)log },
    { "logf",                              (uintptr_t)logf_abi },
    { "longjmp",                           (uintptr_t)my_longjmp },
    { "lrand48",                           (uintptr_t)&lrand48 },
    { "lseek",                             (uintptr_t)lseek },
    { "malloc",                            (uintptr_t)ga_malloc },
    { "mbrtowc",                           (uintptr_t)&mbrtowc },
    { "memchr",                            (uintptr_t)memchr },
    { "memcmp",                            (uintptr_t)memcmp },
    { "memcpy",                            (uintptr_t)&memcpy },
    { "memmove",                           (uintptr_t)&memmove },
    { "memset",                            (uintptr_t)&memset },
    { "mktime",                            (uintptr_t)&mktime },
    { "modf",                              (uintptr_t)&modf_abi },
    { "nanosleep",                         (uintptr_t)&bionic_nanosleep },
    { "open",                              (uintptr_t)open_fake },
    { "poll",                              (uintptr_t)&poll },
    { "pow",                               (uintptr_t)pow_abi },
    { "powf",                              (uintptr_t)powf_abi },
    { "printf",                            (uintptr_t)&printf },
    { "pthread_attr_destroy",              (uintptr_t)pthread_attr_destroy_fake },
    { "pthread_attr_init",                 (uintptr_t)pthread_attr_init_fake },
    { "pthread_attr_setdetachstate",       (uintptr_t)&pthread_attr_setdetachstate_fake },
    { "pthread_attr_setstacksize",         (uintptr_t)pthread_attr_setstacksize_fake },
    { "pthread_cond_broadcast",            (uintptr_t)pthread_cond_broadcast_fake },
    { "pthread_cond_destroy",              (uintptr_t)pthread_cond_destroy_fake },
    { "pthread_cond_init",                 (uintptr_t)pthread_cond_init_fake },
    { "pthread_cond_signal",               (uintptr_t)pthread_cond_signal_fake },
    { "pthread_cond_timedwait",            (uintptr_t)pthread_cond_timedwait_fake },
    { "pthread_cond_wait",                 (uintptr_t)pthread_cond_wait_fake },
    { "pthread_create",                    (uintptr_t)pthread_create_fake },
    /* Real TLS.  These were all `ret0`, which meant every pthread_setspecific
     * silently discarded its value and every pthread_getspecific returned
     * NULL — so any per-thread context the engine stashed (and the streaming
     * thread does) came back as "not set" on every single lookup.  bionic's
     * pthread_key_t and glibc's are both a 4-byte int and the signatures
     * match, so these bind straight through. */
    { "pthread_getspecific",               (uintptr_t)pthread_getspecific },
    { "pthread_join",                      (uintptr_t)pthread_join },
    { "pthread_key_create",                (uintptr_t)pthread_key_create },
    { "pthread_key_delete",                (uintptr_t)pthread_key_delete },
    { "pthread_mutex_destroy",             (uintptr_t)pthread_mutex_destroy_fake },
    { "pthread_mutex_init",                (uintptr_t)pthread_mutex_init_fake },
    { "pthread_mutex_lock",                (uintptr_t)pthread_mutex_lock_fake },
    { "pthread_mutex_unlock",              (uintptr_t)pthread_mutex_unlock_fake },
    { "pthread_mutexattr_init",            (uintptr_t)pthread_mutexattr_init_fake },
    { "pthread_mutexattr_settype",         (uintptr_t)pthread_mutexattr_settype_fake },
    { "pthread_once",                      (uintptr_t)pthread_once },
    { "pthread_self",                      (uintptr_t)pthread_self },
    { "pthread_setspecific",               (uintptr_t)pthread_setspecific },
    { "putc",                              (uintptr_t)&putc_fake },
    { "puts",                              (uintptr_t)puts },
    { "putwc",                             (uintptr_t)&putwc_fake },
    { "read",                              (uintptr_t)read },
    { "realloc",                           (uintptr_t)ga_realloc },
    { "recv",                              (uintptr_t)&recv },
    { "recvfrom",                          (uintptr_t)&recvfrom },
    { "remove",                            (uintptr_t)&remove },
    { "rename",                            (uintptr_t)&rename },
    { "rewind",                            (uintptr_t)&rewind_fake },
    { "sem_close",                         (uintptr_t)&sem_close_fake },
    { "sem_init",                          (uintptr_t)sem_init_fake },
    { "sem_open",                          (uintptr_t)&sem_open_fake },
    { "sem_post",                          (uintptr_t)sem_post_fake },
    { "sem_unlink",                        (uintptr_t)&sem_unlink_fake },
    { "sem_wait",                          (uintptr_t)sem_wait_fake },
    { "send",                              (uintptr_t)&send },
    { "sendto",                            (uintptr_t)&sendto },
    { "setjmp",                            (uintptr_t)my_setjmp },
    { "setlocale",                         (uintptr_t)&setlocale },
    { "setsockopt",                        (uintptr_t)&setsockopt },
    { "setvbuf",                           (uintptr_t)&setvbuf_fake },
    { "shutdown",                          (uintptr_t)&shutdown },
    { "sigaction",                         (uintptr_t)ret0 },
    { "sigprocmask",                       (uintptr_t)&sigprocmask },
    { "sin",                               (uintptr_t)sin_abi },
    { "sinf",                              (uintptr_t)sinf_abi },
    { "snprintf",                          (uintptr_t)&snprintf },
    { "socket",                            (uintptr_t)&socket },
    { "sprintf",                           (uintptr_t)&sprintf },
    { "sqrt",                              (uintptr_t)&sqrt_abi },
    { "srand48",                           (uintptr_t)&srand48 },
    { "sscanf",                            (uintptr_t)&sscanf },
    { "strcasecmp",                        (uintptr_t)strcasecmp },
    { "strcat",                            (uintptr_t)strcat },
    { "strchr",                            (uintptr_t)&strchr_safe },
    { "strcmp",                            (uintptr_t)&strcmp_safe },
    { "strcoll",                           (uintptr_t)&strcoll },
    { "strcpy",                            (uintptr_t)strcpy },
    { "strdup",                            (uintptr_t)&strdup },
    { "strerror",                          (uintptr_t)strerror },
    { "strftime",                          (uintptr_t)&strftime },
    { "strlen",                            (uintptr_t)strlen },
    { "strncasecmp",                       (uintptr_t)strncasecmp },
    { "strncmp",                           (uintptr_t)strncmp },
    { "strncpy",                           (uintptr_t)strncpy },
    { "strrchr",                           (uintptr_t)&strrchr },
    { "strstr",                            (uintptr_t)&strstr_safe },
    { "strtod",                            (uintptr_t)&strtod_abi },
    { "strtok",                            (uintptr_t)&strtok },
    { "strtoul",                           (uintptr_t)strtoul },
    { "strxfrm",                           (uintptr_t)&strxfrm },
    { "syscall",                           (uintptr_t)syscall },
    { "sysconf",                           (uintptr_t)sysconf },
    { "tan",                               (uintptr_t)tan_abi },
    { "tanf",                              (uintptr_t)&tanf_abi },
    { "time",                              (uintptr_t)&bionic_time },
    { "tolower",                           (uintptr_t)&tolower },
    { "towlower",                          (uintptr_t)&towlower },
    { "towupper",                          (uintptr_t)&towupper },
    { "uname",                             (uintptr_t)&uname },
    { "ungetc",                            (uintptr_t)&ungetc_fake },
    { "ungetwc",                           (uintptr_t)&ungetwc_fake },
    { "usleep",                            (uintptr_t)usleep },
    { "vsnprintf",                         (uintptr_t)&vsnprintf },
    { "vsprintf",                          (uintptr_t)&vsprintf },
    { "wcrtomb",                           (uintptr_t)&wcrtomb },
    { "wcscoll",                           (uintptr_t)&wcscoll },
    { "wcsftime",                          (uintptr_t)&wcsftime },
    { "wcslen",                            (uintptr_t)&wcslen },
    { "wcsxfrm",                           (uintptr_t)&wcsxfrm },
    { "wctob",                             (uintptr_t)&wctob },
    { "wctype",                            (uintptr_t)&wctype },
    { "wmemchr",                           (uintptr_t)&wmemchr },
    { "wmemcmp",                           (uintptr_t)&wmemcmp },
    { "wmemcpy",                           (uintptr_t)&wmemcpy },
    { "wmemmove",                          (uintptr_t)&wmemmove },
    { "wmemset",                           (uintptr_t)&wmemset },
    { "write",                             (uintptr_t)&write },
    { "writev",                            (uintptr_t)&writev },
};

/* ── pthread_kill interceptor ───────────────────────────────────────────────
 * The game stores bionic pthread_t values (small integers or bionic struct
 * pointers) and passes them to pthread_kill.  Glibc's pthread_kill
 * dereferences the handle as a struct pthread* — if the handle is not a real
 * glibc struct, this crashes or sends a signal to the wrong thread.
 * Two layers of interception:
 *   1. pthread_kill_fake: in the dynlib table for libGTALcs.so's GOT (if imported)
 *   2. Global pthread_kill: symbol interposition catches calls from SDL2/OpenAL */
static int pthread_kill_fake(pthread_t thread, int sig) {
    (void)thread; (void)sig;
    return 0;
}

/* Global interposition: catches pthread_kill from ALL shared libs. */
int pthread_kill(pthread_t thread, int sig) {
    static int (*real_pk)(pthread_t, int) = NULL;
    if (!real_pk) real_pk = dlsym(RTLD_NEXT, "pthread_kill");
    if (sig == SIGSEGV || sig == SIGABRT || sig == SIGILL || sig == SIGBUS)
        return 0;
    return real_pk(thread, sig);
}

/* ── fopen/open wrappers ─────────────────────────────────────────────────── */
/* Bring-up diagnostic: the engine builds game-data paths internally from what
 * setGameFilesDir() was given, and a failed open surfaces much later as a NULL
 * file handle inside RslfRead with no indication of what it wanted.  Logging
 * the misses turns that into the actual path, which is the whole answer.
 * Successes are capped so a long run cannot flood the log (hot-path logging
 * has starved streaming threads on the sibling CTW port). */
/* ── loose-file (gamedata) redirection ───────────────────────────────────────
 *
 * Every archive path is ALSO a plain relative open.  That is not a guess; it
 * is the engine's own chain, read out of libGTALcs.so:
 *
 *   base::BcfOpen(name, "r", f)                       @0x231744
 *     -> LogicalFS_OpenBundleFile(name, f)            @0x4f7370
 *          walks the mount list; _LogicalFS_Init always does
 *          LogicalFS_AddBundleRoot(Platform::GetBundleRoot()), and
 *          GetBundleRoot() returns "" — so there is a directory mount whose
 *          paths are relative to the cwd, with no WadArchive behind it
 *     -> Platform::FileOpenOSFilePath(path, READ, f)  @0x4f9604
 *          GetAssetManager() is NULL here (it tail-calls AAssetManager_fromJava,
 *          which we bind to ret0), so it does NOT take the AAssetManager_open
 *          branch and falls through to a native open of the relative path
 *
 * which is why dropping the unpacked archive under GAMEDATA_PATH works at all,
 * and why it keeps working with no OBB present: the WAD is just one more mount
 * point, and its absence removes a lookup rather than breaking one.
 *
 * The catch is case.  A WAD lookup hashes the LOWERCASED path (the FAT key is
 * ~crc32 of the lowercased name), so the archive is case-insensitive; ext4 is
 * not.  The engine asks for "MODELS/GENERIC.TXD" while the unpacked tree has
 * "Models/generic.txd", and some requests carry a trailing space — that exact
 * miss ("Models/GENERIC.TXD ") is in the boot log from a run where the OBB was
 * mounted and serving the file perfectly well.  Under the WAD those names all
 * hash the same; on disk they are four different filenames.  So resolution
 * folds case, drops trailing blanks and accepts backslashes.
 *
 * Directories are indexed lazily and cached.  With no OBB every asset read in
 * the game lands here, including the streaming thread's, and a scandir per
 * open would put it on a syscall treadmill.  Read-only opens only: a path with
 * write intent is never redirected. */
#define GD_MAX_DIRS 512
struct gd_dir { char *path; char **names; int n; };
static struct gd_dir   gd_cache[GD_MAX_DIRS];
static int             gd_ndirs;
static pthread_mutex_t gd_lock = PTHREAD_MUTEX_INITIALIZER;

/* Index one directory.  Call with gd_lock held. */
static struct gd_dir *gd_index(const char *dirpath) {
    for (int i = 0; i < gd_ndirs; i++)
        if (!strcmp(gd_cache[i].path, dirpath)) return &gd_cache[i];
    if (gd_ndirs >= GD_MAX_DIRS) return NULL;

    DIR *d = opendir(dirpath);
    if (!d) return NULL;

    struct gd_dir g = { NULL, NULL, 0 };
    int cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' &&
            (!e->d_name[1] || (e->d_name[1] == '.' && !e->d_name[2]))) continue;
        if (g.n == cap) {
            int nc = cap ? cap * 2 : 32;
            char **nn = realloc(g.names, (size_t)nc * sizeof *nn);
            if (!nn) break;
            g.names = nn; cap = nc;
        }
        if (!(g.names[g.n] = strdup(e->d_name))) break;
        g.n++;
    }
    closedir(d);

    if (!(g.path = strdup(dirpath))) {
        for (int i = 0; i < g.n; i++) free(g.names[i]);
        free(g.names);
        return NULL;
    }
    gd_cache[gd_ndirs] = g;
    return &gd_cache[gd_ndirs++];
}

static const char *gd_match(const struct gd_dir *g, const char *want) {
    for (int i = 0; i < g->n; i++)
        if (!strcasecmp(g->names[i], want)) return g->names[i];
    return NULL;
}

static const char *gamedata_redirect(const char *path, char *buf, size_t n) {
    if (!path || !*path || path[0] == '/')
        return NULL;

    /* backslash -> slash; the engine's own OpenBundleFile does the same fix-up */
    char rel[1024];
    size_t len = 0;
    for (const char *p = path; *p && len < sizeof rel - 1; p++)
        rel[len++] = (*p == '\\') ? '/' : *p;
    rel[len] = '\0';

    /* Exact match first: one stat, and it is the common case once the names
     * line up.  Only a miss pays for the case-folded walk. */
    snprintf(buf, n, "%s/%s", GAMEDATA_PATH, rel);
    if (access(buf, R_OK) == 0) return buf;

    char real[1024];
    int  rn = snprintf(real, sizeof real, "%s", GAMEDATA_PATH);
    if (rn < 0 || rn >= (int)sizeof real) return NULL;

    pthread_mutex_lock(&gd_lock);
    char *save = NULL;
    for (char *tok = strtok_r(rel, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
        for (size_t t = strlen(tok); t && (tok[t-1] == ' ' || tok[t-1] == '\t'); )
            tok[--t] = '\0';
        if (!*tok) continue;
        struct gd_dir *g = gd_index(real);
        const char *hit = g ? gd_match(g, tok) : NULL;
        if (!hit) { pthread_mutex_unlock(&gd_lock); return NULL; }
        int add = snprintf(real + rn, sizeof real - (size_t)rn, "/%s", hit);
        if (add < 0 || add >= (int)(sizeof real - (size_t)rn)) {
            pthread_mutex_unlock(&gd_lock);
            return NULL;
        }
        rn += add;
    }
    pthread_mutex_unlock(&gd_lock);

    snprintf(buf, n, "%s", real);
    return buf;
}

static FILE *fopen_fake(const char *path, const char *mode) {
    FILE *f = fopen(path, mode);
    if (!f) {
        char buf[1200];
        const char *redir = (mode && !strpbrk(mode, "w+a"))
                          ? gamedata_redirect(path, buf, sizeof buf) : NULL;
        if (redir) f = fopen(redir, mode);
        if (f) {
            static int rd_shown = 0;
            if (rd_shown < 10) { rd_shown++; fprintf(stderr, "[fopen] gamedata: %s\n", buf); }
        } else {
            fprintf(stderr, "[fopen] MISS: %s (%s)\n", path ? path : "(null)", mode ? mode : "?");
        }
    } else {
        static int shown = 0;
        if (shown < 40) { shown++; fprintf(stderr, "[fopen] ok: %s\n", path); }
    }
    return f;
}

static int open_fake(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    int fd = open(path, flags, mode);
    if (fd < 0) {
        char buf[1200];
        const char *redir = (flags & O_CREAT) ? NULL
                          : gamedata_redirect(path, buf, sizeof buf);
        if (redir) {
            fd = open(redir, flags, mode);
            if (fd >= 0) {
                static int rd_shown = 0;
                if (rd_shown < 10) { rd_shown++; fprintf(stderr, "[open] gamedata: %s\n", buf); }
            }
        }
    }
    if (fd < 0)
        fprintf(stderr, "[open] MISS: %s\n", path ? path : "(null)");
    return fd;
}

/* ── libpng diagnostics ──────────────────────────────────────────────────────
 * Log libpng warnings ("iCCP: known incorrect sRGB profile" etc.) without
 * changing behaviour.
 *
 * History: the viewOnInit PC=0 crash in png_chunk_unknown_handling was first
 * "fixed" here by hooking png_error AND png_longjmp to log-and-return.  That
 * was papering over the real bug — glibc setjmp writing past bionic-sized
 * jmp_bufs (see setjmp_fix.c) — and became actively wrong once that was fixed:
 * a failing PNG load must longjmp back to the caller's setjmp point, and
 * swallowing it lets libpng fall through into buffer code with uninitialized
 * data (observed: SIGSEGV writing a garbage pointer in AlignPointer from
 * WriteTextureLogFile's path, ~14 minutes into the first post-setjmp-fix run).
 * Only the warning hook stays; errors now propagate the way they do on
 * Android, through my_longjmp. */
static void png_warning_hook(void *png_ptr, const char *msg) {
    fprintf(stderr, "[png] png_warning: %s\n", msg ? msg : "(null)");
    fflush(stderr);
}

/* ── patch_game ──────────────────────────────────────────────────────────────
 * Hooks into libGTALcs.so's own code.  Unlike the Chinatown Wars port — which
 * had to replace NvEventQueue's threading and screen-size accessors — LCS
 * drives everything through documented JNI entry points, so most wiring is via
 * the symbol table.  Engine-internal hooks discovered during bring-up go here.
 */
/* ── CPed::SetModelIndex: make the ped's model resident first ─────────────
 *
 * The long-standing crash: a mission script's CREATE_CHAR reaches
 * CPed::SetModelIndex with a model whose clump is not loaded, so
 * CEntity::SetModelIndex leaves CPed::m_pClump (+0x60) NULL and the very next
 * call — RslAnimBlendElementGroupInit -> IsElementGroupSkinned ->
 * GetFirstElement(NULL) — faults on `ldr r4,[r5,#8]!`.  MALLOC_PERTURB_ showed
 * r0 = 0 rather than 0xa5a5a5a5, so the pointer is genuinely NULL: the model
 * really is absent, nothing is corrupt.
 *
 * The engine has everything needed to fix that itself, and it is all exported:
 * ask whether the model exists in the CD image, request it, and drain the
 * request queue synchronously — which is exactly what the script path is
 * supposed to have done before creating the ped.  Doing it here is a
 * belt-and-braces preload, not error suppression: if the model was already
 * resident these calls are no-ops.
 *
 * GTALCS_PRELOAD_PED=0 disables the preload and leaves only the logging, so
 * the two behaviours can be compared on device. */
static void (*orig_CPed_SetModelIndex)(void *ped, unsigned int idx);
static int  (*p_IsObjectInCdImage)(int idx);
static void (*p_RequestModel)(int idx, int flags);
static void (*p_LoadAllRequestedModels)(int priority);
static int  (*p_HasSpecialCharLoaded)(int slot);
static unsigned char **p_mspInst;   /* CStreaming::mspInst -> CStreamingInfo[] */
static int (*p_GetCdPosnAndSize)(const void *info, unsigned *posn, unsigned *size);

/* ── CStreamingInfo, read straight off CStreaming::HasSpecialCharLoaded ──
 *
 *   add  r0, r0, #109          ; slot -> model index
 *   ldr  r3, [<mspInst>]       ; mspInst is a POINTER, not an array
 *   add  r0, r0, r0, lsl #2    ; idx*5
 *   add  r0, r3, r0, lsl #2    ; base + idx*20
 *   ldrb r0, [r0, #12]         ; m_loadState
 *   ... r0 = (r0 == 1)         ; 1 == STREAMSTATE_LOADED
 *
 * so the record is 20 bytes and m_loadState is the byte at +12.  mspInst has
 * to be dereferenced at call time — it is still NULL while CStreaming::Init
 * is running, so caching the base during patch_game would read garbage.
 *
 * The other 19 bytes are dumped raw rather than decoded: the GTA3-era layout
 * (next/prev/nextOnCd, flags, loadState, cdPosn, cdSize) is 16 bytes, so this
 * build's is not identical and guessing the field offsets would be inventing
 * data.  The hex is enough to see whether a slot has a real archive position.
 */
/* RequestSpecialChar(i) is RequestSpecialModel(i + 109), and there are 21 of
 * these named cutscene/mission slots — 109..129.  Everything above that is an
 * ordinary model id (vehicles, objects, txds), NOT a special character.  Getting
 * this bound wrong once already mislabelled model 6103 as "(SPECIAL)" in a log
 * and made every loader call log unbounded. */
#define MI_SPECIAL01     109u
#define MI_SPECIAL_LAST  129u
#define STREAMINFO_STRIDE 20
/* The array does NOT start at *mspInst — it starts 4 bytes in.
 *
 * Derived, then confirmed against the archive: CStreamingInfo::GetCdPosnAndSize
 * @0x47811c reads posn at `this+12` and size at `this+16`, and returns FALSE when
 * the size word is zero.  Against a record based at *mspInst + idx*20 those two
 * offsets land on the wrong fields, so the base must be shifted.  +4 makes
 * everything line up at once:
 *
 *   real +0..3  m_prev      (HasSpecialCharLoaded's byte at *mspInst+idx*20+12
 *   real +4..7  m_next       == real+8, so its loadState read still agrees)
 *   real +8     m_loadState
 *   real +9     m_flags
 *   real +10..11 u16
 *   real +12..15 m_cdPosn   -- matches Models/gta3.dir offsets exactly
 *   real +16..19 m_cdSize
 *
 * The clincher: model 81's directory size (27 sectors, gang03.dff) is exactly the
 * word that showed up as model *82's* first field in the old dump.  Everything was
 * shifted by one record's worth of 4 bytes, which is why m_cdSize was never
 * actually being printed. */
#define STREAMINFO_BASE_FIXUP 4
static unsigned char *streaminfo(unsigned int idx) {
    unsigned char *base = p_mspInst ? *p_mspInst : NULL;
    return base ? base + STREAMINFO_BASE_FIXUP + (size_t)idx * STREAMINFO_STRIDE
                : NULL;
}
/* "load state = LOADED?" for ANY model index, via the engine's own accessor.
 * HasSpecialCharLoaded(i) reads ms_aInfoForModel[i + 109].m_loadState == 1,
 * so passing idx-109 (negative for ordinary models) makes it a general
 * predicate.  Used to cross-check the hand-rolled mspInst walk above: if the
 * two disagree, the stride/offset is wrong and the raw bytes are meaningless.
 */
static int model_is_loaded(unsigned int idx) {
    return p_HasSpecialCharLoaded ? p_HasSpecialCharLoaded((int)idx - 109) : -1;
}

/* CStreaming::LoadAllRequestedModels' re-entrancy guard.
 *
 * The function opens with
 *     ldrb r4,[r3,#8] ; cmp r4,#0 ; movne r8,#1 ; <return r8>
 *     strb #1,[r3,#8] ; bl FlushChannels ; <do the work>
 * so a non-zero guard makes every call a no-op that returns 1 without loading
 * anything.  It is a file-local static with no exported name, but its address
 * is fixed: r3 = pc+lit = 0xae1bf0, guard at +8.
 *
 * This matters because the observed failure is that NO special/cutscene model
 * ever leaves STREAMSTATE_INQUEUE, and forcing RequestModel +
 * LoadAllRequestedModels from the CPed hook did not move it either — exactly
 * what a permanently-set guard would look like.  Reading it says whether the
 * loader is being skipped or is running and failing, which are different bugs. */
#define LOADALL_GUARD_OFF 0x00ae1bf8
static volatile unsigned char *loadall_guard(void) {
    return gtalcs_mod.load_bias
         ? (volatile unsigned char *)(gtalcs_mod.load_bias + LOADALL_GUARD_OFF)
         : NULL;
}
/* Walk the list LoadAllRequestedModels actually consults.
 *
 * Its loop entry is, in full:
 *     r3 = *mspInst
 *     r2 = [r3 + 0x1e33c]        ; head->m_next
 *     r3 = r3 + 0x1e000 + 0x324  ; &sentinel
 *     cmp r2, r3 ; beq <exit>    ; empty list -> return without loading anything
 * and the node it then passes to CStreamingInfo::RemoveFromList is
 * `*mspInst + idx*20 + 4`, which is the same +4 base fixup used above — so a node
 * address converts straight back to a model index.
 *
 * The measurement this exists for: LoadAllRequestedModels(0) was called with the
 * guard clear and returned WITHOUT ever calling GetNextFileOnCd, i.e. it took that
 * `beq`.  Meanwhile ms_numModelsRequested said 6 and model 109's record had
 * non-NULL next/prev.  Both cannot be true of the same list, so print it. */
/* The terminator is 0x1e338, not the 0x1e324 the loop's entry check compares
 * against — the first dump walked head->next -> model 79 -> a node at
 * inst+0x1e338 and stopped there.  A walk that does not stop where it thinks it
 * stops may also be starting in the wrong place, so this is pinned to what the
 * device actually showed rather than to the offset read off the branch. */
#define REQLIST_HEAD_NEXT     0x1e33c   /* start sentinel's m_next */
#define REQLIST_TAIL_SENTINEL 0x1e324   /* end sentinel — model 182's m_next */
static void log_request_list(const char *tag) {
    unsigned char *inst = p_mspInst ? *p_mspInst : NULL;
    if (!inst) { fprintf(stderr, "%s <mspInst NULL>\n", tag); return; }

    /* Walk FORWARDS: m_next is at +4, NOT +0.  The first version had these the
     * other way round, so it followed m_prev from the tail, hit a sentinel after
     * one node, and produced "model 109 is not on the request list" — which was
     * wrong.  The pointers reconcile exactly the other way: model 79's +0 is the
     * start sentinel (79 is first) and model 182's +4 is the end sentinel (182 is
     * last), giving  start -> 79 -> 5024 -> ... -> 5987 -> 109 -> 182 -> end. */
    unsigned char *tail = inst + REQLIST_TAIL_SENTINEL;
    unsigned char *node = *(unsigned char **)(inst + REQLIST_HEAD_NEXT);
    fprintf(stderr, "%s head->next=%p tail=%p%s\n", tag, (void *)node,
            (void *)tail, node == tail ? "  EMPTY" : "");

    for (int n = 0; n < 64 && node && node != tail; n++) {
        long off = (long)(node - inst) - STREAMINFO_BASE_FIXUP;
        long idx = off / STREAMINFO_STRIDE;
        fprintf(stderr, "%s   [%d] node=%p -> model %ld%s loadState=%u flags=0x%x\n",
                tag, n, (void *)node, idx,
                (off % STREAMINFO_STRIDE) ? " (UNALIGNED — offsets wrong)" : "",
                node[8], node[9]);
        node = *(unsigned char **)(node + 4);          /* m_next */
    }
    fflush(stderr);
}

/* Census of everything the streamer thinks is queued.
 *
 * Reconciles a contradiction the list dump exposed: ms_numModelsRequested says 6
 * while the walked list holds exactly one node.  If six records read INQUEUE and
 * only one is on the list, the linkage is broken generally and this is not a
 * special-slot bug at all.  Bounded by CModelInfo::msNumModelInfos so it cannot
 * walk off the end of the array. */
static void log_inqueue_census(const char *tag) {
    unsigned char *inst = p_mspInst ? *p_mspInst : NULL;
    int *pn = (int *)so_symbol(&gtalcs_mod, "_ZN10CModelInfo15msNumModelInfosE");
    int n = pn ? *pn : 0;
    if (!inst || n <= 0 || n > 20000) {
        fprintf(stderr, "%s <inst=%p numModelInfos=%d — not scanning>\n",
                tag, (void *)inst, n);
        return;
    }
    int inqueue = 0, shown = 0;
    for (int i = 0; i < n; i++) {
        const unsigned char *r = inst + STREAMINFO_BASE_FIXUP
                               + (size_t)i * STREAMINFO_STRIDE;
        if (r[8] != 2) continue;                 /* STREAMSTATE_INQUEUE */
        inqueue++;
        if (shown < 16) {
            shown++;
            fprintf(stderr, "%s   model %d flags=0x%x prev=%p next=%p\n", tag, i,
                    r[9], *(void *const *)r, *(void *const *)(r + 4));
        }
    }
    fprintf(stderr, "%s total INQUEUE=%d (scanned %d)\n", tag, inqueue, n);
    fflush(stderr);
}

static void log_streaminfo(const char *tag, unsigned int idx) {
    const unsigned char *si = streaminfo(idx);
    int loaded = model_is_loaded(idx);
    if (!si) {
        fprintf(stderr, "%s model=%u loaded=%d info=<mspInst NULL>\n",
                tag, idx, loaded);
        return;
    }
    /* Ask the engine itself rather than trusting the offsets above.  This is the
     * predicate the request-list walk uses: GetCdPosnAndSize returning 0 means
     * "no size", and such a request can never be picked no matter how often the
     * loader runs — which is exactly the shape of the vinc_01 failure. */
    unsigned posn = 0, size = 0;
    int ok = -1;
    if (p_GetCdPosnAndSize) ok = p_GetCdPosnAndSize(si, &posn, &size);
    fprintf(stderr, "%s model=%u loaded=%d loadState=%u cdPosn=0x%x cdSize=%u "
                    "posnAndSize=%d info=",
            tag, idx, loaded, si[8], posn, size, ok);
    for (int i = 0; i < STREAMINFO_STRIDE; i++)
        fprintf(stderr, "%02x%s", si[i], i == STREAMINFO_STRIDE - 1 ? "" : " ");
    /* MI_SPECIAL01 == 109: RequestSpecialChar(i) is RequestSpecialModel(i+109),
     * so anything >= 109 is a named cutscene/mission slot whose contents are
     * whatever RequestSpecialModel last put there — NOT a fixed model. */
    if (idx >= MI_SPECIAL01 && idx <= MI_SPECIAL_LAST)
        fprintf(stderr, "  (SPECIAL%02u)", idx - MI_SPECIAL01 + 1);
    fprintf(stderr, "\n");
    fflush(stderr);
}

static void CPed_SetModelIndex_hook(void *ped, unsigned int idx) {
    /* Default OFF: this build is handed to the user for a diagnostic run, so
     * its behaviour must match what they already have apart from one log line.
     * GTALCS_PRELOAD_PED=1 opts into the repair attempt. */
    static int preload = -1;
    if (preload < 0) {
        const char *e = getenv("GTALCS_PRELOAD_PED");
        preload = (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
    }

    int in_cd = p_IsObjectInCdImage ? p_IsObjectInCdImage((int)idx) : -1;

    volatile unsigned char *guard = loadall_guard();
    int guard_before = guard ? *guard : -1;
    int guard_cleared = 0;

    if (preload && p_RequestModel && p_LoadAllRequestedModels && in_cd) {
        /* GTALCS_CLEAR_LOADGUARD=1: if the guard is set here it is stuck —
         * SetModelIndex is reached from cutscene/script code, not from inside
         * the loader — so clearing it lets the drain actually run.  Opt-in,
         * because if the engine ever DOES call us from within
         * LoadAllRequestedModels, clearing it re-enters the loader. */
        static int clear_guard = -1;
        if (clear_guard < 0) {
            const char *e = getenv("GTALCS_CLEAR_LOADGUARD");
            clear_guard = (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
        }
        if (clear_guard && guard && *guard) { *guard = 0; guard_cleared = 1; }

        /* Dump the list BEFORE the drain as well as after.  The "LoadAllRequested-
         * Models returned without calling GetNextFileOnCd" measurement and the
         * "list has 6 entries" measurement came from DIFFERENT runs, so they were
         * never actually in contradiction — and the loop's only pre-call exit is
         * the empty-list `beq`.  Printing the list at the instant of the call is
         * what decides whether the drain saw an empty list (correct behaviour,
         * and the request is elsewhere) or a populated one (a real anomaly). */
        if (idx >= MI_SPECIAL01 && idx <= MI_SPECIAL_LAST)
            log_request_list("[reqlist-pre]");

        /* The bool on LoadAllRequestedModels means "priority requests only"
         * (it is passed straight to GetNextFileOnCd as a filter).  Requesting
         * normally and then draining priority-only was the original no-op
         * bug; draining with 0 loads every queued request. */
        p_RequestModel((int)idx, 0);
        p_LoadAllRequestedModels(0);
    }

    /* inCdImage is the discriminator for "are the assets missing?":
     *   1 -> the model IS in the archive, so this is a streaming/eviction
     *        problem and extracting files from the OBB would change nothing
     *   0 -> the engine does not believe the model exists at all, which is
     *        what a missing loose file would look like
     * Log every call: ped creation is rare, and the last line before the
     * crash is the one that names the culprit. */
    static int logged;
    if (logged < 400) {
        logged++;
        fprintf(stderr, "[ped] SetModelIndex(%u) inCdImage=%d preload=%d "
                        "loadAllGuard=%d%s\n",
                idx, in_cd, preload, guard_before,
                guard_cleared ? " (cleared)" : "");
        /* The discriminator.  inCdImage only says the slot HAS a cd position
         * — for a special slot that position is whatever was written there
         * last, so it proves nothing on its own.  m_loadState does:
         *   1 (LOADED) + a NULL clump -> the load "succeeded" but the RSL
         *     object was never built, i.e. a conversion bug on our side
         *   anything else            -> the model was never loaded at all,
         *     and RequestSpecialModel's name lookup is the next thing to look
         *     at, not the conversion path. */
        log_streaminfo("[ped]  info:", idx);
        /* The counters, at the one instant they mean something.
         *
         * Why this is the next question: cutscene models are requested with
         * flags 0xe — which carries the PRIORITY bit — and they load.  The one
         * that does not, "vinc_01", is requested with 0x6: same slot range, no
         * priority bit.  And GetNextFileOnCd is called with prio=1 for the
         * picks that succeed.  In this engine family that argument is
         * self-cancelling — `if (priority && ms_numPriorityRequests == 0)
         * priority = false;` — so a LEAKED ms_numPriorityRequests (incremented
         * on request, never decremented on completion) would make the streamer
         * serve priority requests only, forever, and starve exactly the
         * non-priority ones.  Non-zero here with 109 stuck at INQUEUE is that
         * bug; zero means the filter is not what is skipping it. */
        if (idx >= MI_SPECIAL01 && idx <= MI_SPECIAL_LAST && model_is_loaded(idx) != 1) {
            streaming_dump();
            log_request_list("[reqlist]");
            log_inqueue_census("[queued]");
        }
        fflush(stderr);
    }

    orig_CPed_SetModelIndex(ped, idx);
}

/* ── CStreaming::RequestSpecialModel: name the character ──────────────────
 *
 * Slot 109 is MI_SPECIAL01, so the model that crashes is not a fixed ped at
 * all — it is whatever name the script last asked for.  This hook is pure
 * observation (it changes nothing) and answers the one question the crash log
 * cannot: WHICH character, and did the slot reach LOADED afterwards.
 *
 * The first two instructions of the function are `push {…}` / `add fp, sp,#32`
 * — PC-independent, so hook_arm_trampoline can relocate them verbatim.
 */
static void (*orig_RequestSpecialModel)(int idx, const char *name, int flags);

static void RequestSpecialModel_hook(int idx, const char *name, int flags) {
    static int logged;
    int show = logged < 60;
    if (show) {
        logged++;
        fprintf(stderr, "[special] RequestSpecialModel(%d, \"%s\", 0x%x)\n",
                idx, name ? name : "(null)", (unsigned)flags);
        log_streaminfo("[special] before:", (unsigned)idx);
    }

    orig_RequestSpecialModel(idx, name, flags);

    /* The call is asynchronous — this is the state it leaves behind, not the
     * final one.  What matters is whether the slot picked up a cd position at
     * all: an unchanged record means the name was never found in the CD
     * directory, and no amount of waiting will load it. */
    if (show) {
        volatile unsigned char *g = loadall_guard();
        log_streaminfo("[special] after: ", (unsigned)idx);
        fprintf(stderr, "[special] loadAllGuard=%d\n", g ? *g : -1);
    }
}

/* ── the loader itself ────────────────────────────────────────────────────
 *
 * Established so far: the request for "vinc_01" is correct (slot 109, flags
 * 0x6, cd position 0x84fd — which is exactly where vinc_01.dff sits in
 * Models/gta3.dir), the re-entrancy guard is clear, LoadAllRequestedModels
 * runs, and the slot still never leaves STREAMSTATE_INQUEUE (2).  Ordinary
 * ped models in the same run reach STREAMSTATE_LOADED (1) normally.
 *
 * So the request exists and the loader runs, but this request is never
 * serviced.  These two hooks split that in half:
 *   GetNextFileOnCd  — which request the loader picks next.  If 109 is never
 *                      returned, the request-list walk skips it and the bug is
 *                      in the linkage/ordering, not in reading or converting.
 *   ConvertBufferToObject — which model actually gets built from a buffer.  If
 *                      109 is picked but never converted, the read is failing.
 */
static int (*orig_GetNextFileOnCd)(int lastPosn, int priority);
/* RETURNS INT — declaring this void was the bug that destabilised every run with
 * the hook on.  LoadAllRequestedModels does `bl ConvertBufferToObject;
 * subs r8,r0,#0; beq 47e964` @0x47e8f0: it TESTS the return value, and 0 means
 * the conversion failed.  A void hook leaves r0 holding whatever our C function
 * happened to put there, so the engine read success/failure at random and took
 * the abort path at random — the bad free() and SIGABRT that got blamed on "hot
 * path logging" for most of a session. */
static int (*orig_ConvertBufferToObject)(unsigned char *buf, int modelId);

static int GetNextFileOnCd_hook(int lastPosn, int priority) {
    int r = orig_GetNextFileOnCd(lastPosn, priority);
    static int logged;
    /* Special slots are the ones that fail, so never drop those; ordinary
     * models are capped because this is called per request per drain. */
    int special = (r >= (int)MI_SPECIAL01 && r <= (int)MI_SPECIAL_LAST);
    if (special || logged < 4000) {
        if (!special) logged++;
        fprintf(stderr, "[cdnext] GetNextFileOnCd(lastPosn=%d, prio=%d) -> %d%s\n",
                lastPosn, priority, r, special ? "  (SPECIAL)" : "");
        fflush(stderr);
    }
    return r;
}

/* ── TXD diagnosis: ask the engine, never compute a slot number ──────────
 *
 * LoadCdDirectory @0x4783a4 maps a ".TXD" archive entry to a streaming slot as
 *
 *     FindTexListSlot(name_without_extension) + 4900
 *
 * i.e. a CTexListStore slot, NOT a model index, and the bias is 4900
 * (== msNumModelInfos).  Sibling paths in the same function: COL is
 * FindColSlot + 6100, IFP is RegisterAnimBlock + 6115.  An earlier
 * "model index + 4872" rule happened to give the same answer for vehicles
 * (their texlist slot trails their model id by a constant), which is precisely
 * why it survived two sessions unchallenged; it has been deleted.
 *
 * ConvertBufferToObject @0x47d718 reads the model's OWN txd index — a SIGNED
 * halfword at CModelInfo+0x22 — indexes CTexListStore (28-byte entries) and
 * returns 0 when that entry's dictionary pointer is NULL.  A 0 return aborts
 * the entire drain, which is what starves vinc_01.
 *
 * AddTexListSlot has two callers — CBaseModelInfo::SetTexList (IDE parse) and
 * LoadCdDirectory — so the store is filled from both ends.  If they disagree
 * about a name, ONE logical txd gets TWO slots: the model points at the empty
 * one while the archive data is positioned in the other.  Everything printed
 * here comes from an exported engine function for exactly that reason. */
#define STREAM_OFFSET_TXD 4900u
static int         (*p_FindTexListSlot)(const char *name);
static const char *(*p_GetTexListName)(int slot);
static int          *p_msNumModelInfos;
static void       ***p_modelInfoPtrs;   /* CModelInfo::ms_modelInfoPtrs */

static void txd_slot_line(const char *tag, int slot) {
    if (slot < 0) { fprintf(stderr, "[txd]   %-14s slot=%d  (NOT IN THE STORE)\n",
                            tag, slot); return; }
    unsigned idx = (unsigned)slot + STREAM_OFFSET_TXD;
    unsigned posn = 0, size = 0; int ok = -1;
    unsigned char *si = streaminfo(idx);
    if (si && p_GetCdPosnAndSize) ok = p_GetCdPosnAndSize(si, &posn, &size);
    const char *nm = p_GetTexListName ? p_GetTexListName(slot) : NULL;
    fprintf(stderr, "[txd]   %-14s slot=%d name=\"%s\" stream=%u loadState=%u "
                    "cdPosn=0x%x cdSize=%u posnAndSize=%d\n",
            tag, slot, nm ? nm : "?", idx, si ? si[8] : 255, posn, size, ok);
}

static void txd_diag(int modelId) {
    if (!p_FindTexListSlot || !p_modelInfoPtrs) {
        fprintf(stderr, "[txd] diag unavailable (symbols missing)\n");
        return;
    }
    void **arr = *p_modelInfoPtrs;
    void  *mi  = arr ? arr[modelId] : NULL;
    int    m   = mi ? (int)*(short *)((char *)mi + 0x22) : -2;
    fprintf(stderr, "[txd] model %d: info=%p m_txdIndex=%d msNumModelInfos=%d\n",
            modelId, mi, m, p_msNumModelInfos ? *p_msNumModelInfos : -1);
    txd_slot_line("m_txdIndex ->", m);

    /* The two-slot test.  Take the name the model's OWN slot carries, then ask
     * the store for it again in both cases.  If the two answers differ and both
     * are >= 0, one logical txd occupies two slots and that is the bug. */
    const char *nm = (m >= 0 && p_GetTexListName) ? p_GetTexListName(m) : NULL;
    if (!nm) { fflush(stderr); return; }

    char up[64], lo[64];
    size_t i = 0;
    for (; nm[i] && i < sizeof(up) - 1; i++) {
        up[i] = (char)toupper((unsigned char)nm[i]);
        lo[i] = (char)tolower((unsigned char)nm[i]);
    }
    up[i] = lo[i] = '\0';
    int tl = p_FindTexListSlot(lo), tu = p_FindTexListSlot(up);
    fprintf(stderr, "[txd] FindTexListSlot(\"%s\")=%d  FindTexListSlot(\"%s\")=%d%s\n",
            lo, tl, up, tu,
            (tl >= 0 && tu >= 0 && tl != tu) ? "   *** TWO SLOTS, ONE TXD ***" : "");
    txd_slot_line("lowercase ->", tl);
    txd_slot_line("uppercase ->", tu);
    fflush(stderr);
}

/* Every .TXD in the archive directory, looked up exactly the way
 * LoadCdDirectory looks it up, with the resulting stream slot checked for an
 * archive position.  ONE broken slot means a name quirk specific to toyz;
 * hundreds mean the registration pass itself is wrong — and that single number
 * decides where the next session starts.  Runs once, at the first conversion
 * failure, when the store is fully populated. */
static void txd_census(void) {
    if (!p_FindTexListSlot) return;
    const char *ov = getenv("GTALCS_DIRFILE");
    char path[700];
    if (ov && *ov) snprintf(path, sizeof path, "%s", ov);
    else           snprintf(path, sizeof path, "%s/gamedata/Models/gta3.dir", g_data_path);

    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[txd] census: cannot open %s (%s)\n",
                      path, strerror(errno)); return; }

    unsigned char e[32];
    int total = 0, unregistered = 0, noposn = 0, shown = 0;
    while (fread(e, 1, 32, f) == 32) {
        char nm[25];
        memcpy(nm, e + 8, 24); nm[24] = '\0';
        char *dot = strchr(nm, '.');
        if (!dot || strcasecmp(dot, ".txd") != 0) continue;
        *dot = '\0';                       /* what LoadCdDirectory passes */
        total++;
        int slot = p_FindTexListSlot(nm);
        if (slot < 0) {
            unregistered++;
            if (shown++ < 12) fprintf(stderr, "[txd]   UNREGISTERED \"%s\"\n", nm);
            continue;
        }
        unsigned posn = 0, size = 0;
        unsigned char *si = streaminfo((unsigned)slot + STREAM_OFFSET_TXD);
        if (si && p_GetCdPosnAndSize) p_GetCdPosnAndSize(si, &posn, &size);
        if (posn == 0 || size == 0) {
            noposn++;
            if (shown++ < 12)
                fprintf(stderr, "[txd]   NO ARCHIVE POSN \"%s\" slot=%d stream=%u\n",
                        nm, slot, (unsigned)slot + STREAM_OFFSET_TXD);
        }
    }
    fclose(f);
    fprintf(stderr, "[txd] census (%s): %d .TXD entries, %d not in the store, "
                    "%d with no archive position\n", path, total, unregistered, noposn);
    fflush(stderr);
}

/* One-shot boot check, driven from the frame loop.
 *
 * Once the archive directory has been parsed, BOTH registration paths have run
 * (CBaseModelInfo::SetTexList during the IDE parse, and LoadCdDirectory), so a
 * duplicated texlist slot is already visible — long before any gameplay.  That
 * matters a great deal for verification: the conversion failure itself only
 * happens once the city streams in, which on the device means a five-minute
 * hands-on run, and roughly a third of harness runs stall before that. */
void txd_boot_check(void) {
    static int done, enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("GTALCS_TXD_DIAG");
        enabled = (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
    }
    /* OFF by default: the census reads a 159 KB file and does 1082 store
     * lookups from the frame loop.  Bring-up instrumentation must never be
     * a suspect in the next bug's attribution. */
    if (!enabled) return;
    if (done || g_cd_entries <= 0 || !p_modelInfoPtrs || !*p_modelInfoPtrs) return;
    done = 1;
    fprintf(stderr, "[txd] boot check after %d directory entries\n", g_cd_entries);
    txd_diag(182);                        /* toyz — the one model that fails */
    txd_census();
}

/* Models whose conversion has already failed once with GTALCS_SKIP_BAD_CONVERT.
 * Without this the engine simply re-requests them — model 182 was re-converted
 * 17327 times in one 90 s run, which floods the log and starves the frame loop
 * just as effectively as the original stall did. */
static int  g_poisoned[32];
static int  g_npoisoned;
static int  poisoned(int modelId) {
    for (int i = 0; i < g_npoisoned; i++) if (g_poisoned[i] == modelId) return 1;
    return 0;
}

static int ConvertBufferToObject_hook(unsigned char *buf, int modelId) {
    static int skip = -1;
    if (skip < 0) {
        const char *e = getenv("GTALCS_SKIP_BAD_CONVERT");
        skip = (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
    }
    if (skip && poisoned(modelId)) {
        unsigned char *sip = streaminfo((unsigned)modelId);
        if (sip) sip[8] = 1;                     /* STREAMSTATE_LOADED */
        return 1;                                /* do not even attempt it again */
    }

    int r = orig_ConvertBufferToObject(buf, modelId);
    /* A zero return aborts the WHOLE drain: LoadAllRequestedModels frees the
     * buffer, clears its re-entrancy guard and returns, leaving every remaining
     * request still queued.  So one failing conversion starves everything behind
     * it in the list — which is exactly the shape of the vinc_01 stall, since 109
     * sits at position 4 of the list behind model 79.  Failures are always
     * logged; successes are capped. */
    static int ok_logged, fail_logged;
    int special = (modelId >= (int)MI_SPECIAL01 && modelId <= (int)MI_SPECIAL_LAST);
    if ((r == 0 && fail_logged++ < 12) || special || ok_logged < 80) {
        if (r != 0 && !special) ok_logged++;
        unsigned posn = 0, size = 0;
        unsigned char *si = streaminfo((unsigned)modelId);
        if (si && p_GetCdPosnAndSize) p_GetCdPosnAndSize(si, &posn, &size);
        fprintf(stderr, "[convert] model=%d cdPosn=0x%x cdSize=%u -> %s%s\n",
                modelId, posn, size,
                r ? "ok" : "FAILED (aborts the drain)",
                special ? "  (SPECIAL)" : "");
        if (r == 0) {
            /* Ask the engine which texlist slot this model actually uses, and
             * whether the archive positioned that same slot.  Runs once — the
             * failing model is re-requested hundreds of times per run. */
            static int diagnosed;
            if (!diagnosed && getenv("GTALCS_TXD_DIAG")) {
                diagnosed = 1;
                txd_diag(modelId);
                txd_census();
            }
        }
        fflush(stderr);
    }
    /* GTALCS_SKIP_BAD_CONVERT=1 — the causal-chain test, and a candidate workaround.
     *
     * A zero return aborts the entire drain, so ONE unconvertible model starves
     * every other queued request forever.  Here that model is 182 (toyz.dff),
     * whose texture dictionary (slot 5054 = TOYZ.TXD) has no cd entry at all —
     * cdPosn=0, cdSize=0, inCdImage=0 — even though TOYZ.TXD is present in
     * Models/gta3.dir at 0xd9e7.  vinc_01 sits behind it in the queue and never
     * loads, and that is the crash.
     *
     * Reporting success lets the drain continue to the next request.  The model
     * stays unbuilt, and the engine already has a path for exactly that: when
     * GetCdPosnAndSize returns 0 it marks the slot LOADED and moves on
     * (@0x47e864).  Mark it the same way so it is not retried 219 times.
     *
     * This is a diagnostic, not a fix — if the game later spawns a toyz it gets a
     * model with no clump, which is the same NULL-clump crash one step removed. */
    if (r == 0) {
        static int skip = -1;
        if (skip < 0) {
            const char *e = getenv("GTALCS_SKIP_BAD_CONVERT");
            skip = (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
        }
        if (skip) {
            unsigned char *si2 = streaminfo((unsigned)modelId);
            if (si2) si2[8] = 1;                 /* STREAMSTATE_LOADED */
            if (!poisoned(modelId) && g_npoisoned < (int)(sizeof g_poisoned / sizeof g_poisoned[0])) {
                g_poisoned[g_npoisoned++] = modelId;
                fprintf(stderr, "[convert]   -> model %d neutralised; the drain "
                                "continues past it\n", modelId);
                fflush(stderr);
            }
            return 1;
        }
    }
    return r;
}

static void patch_game(void) {
    /* libpng error/warning interception — see above. */
    struct { const char *sym; void *fn; } hooks[] = {
        { "png_warning", (void *)png_warning_hook },
    };
    for (unsigned i = 0; i < sizeof(hooks)/sizeof(hooks[0]); i++) {
        uintptr_t a = so_symbol(&gtalcs_mod, hooks[i].sym);
        if (a) { hook_addr(a, (uintptr_t)hooks[i].fn);
                 fprintf(stderr, "[patch] hooked %s @ %08x\n", hooks[i].sym, (unsigned)a); }
        else   fprintf(stderr, "[patch] %s not found (libpng not exported?)\n", hooks[i].sym);
    }

    /* Streaming helpers used by the CPed hook below — all exported. */
    p_IsObjectInCdImage      = (int (*)(int))
        so_symbol(&gtalcs_mod, "_ZN10CStreaming17IsObjectInCdImageEi");
    p_RequestModel           = (void (*)(int, int))
        so_symbol(&gtalcs_mod, "_ZN10CStreaming12RequestModelEii");
    p_LoadAllRequestedModels = (void (*)(int))
        so_symbol(&gtalcs_mod, "_ZN10CStreaming22LoadAllRequestedModelsEb");
    p_HasSpecialCharLoaded   = (int (*)(int))
        so_symbol(&gtalcs_mod, "_ZN10CStreaming20HasSpecialCharLoadedEi");
    p_mspInst                = (unsigned char **)
        so_symbol(&gtalcs_mod, "_ZN10CStreaming7mspInstE");
    p_GetCdPosnAndSize       = (int (*)(const void *, unsigned *, unsigned *))
        so_symbol(&gtalcs_mod, "_ZN14CStreamingInfo16GetCdPosnAndSizeERjS0_");
    /* TXD diagnosis — all exported, all read-only. */
    p_FindTexListSlot        = (int (*)(const char *))
        so_symbol(&gtalcs_mod, "_ZN13CTexListStore15FindTexListSlotEPKc");
    p_GetTexListName         = (const char *(*)(int))
        so_symbol(&gtalcs_mod, "_ZN13CTexListStore14GetTexListNameEi");
    p_msNumModelInfos        = (int *)
        so_symbol(&gtalcs_mod, "_ZN10CModelInfo15msNumModelInfosE");
    p_modelInfoPtrs          = (void ***)
        so_symbol(&gtalcs_mod, "_ZN10CModelInfo16ms_modelInfoPtrsE");
    fprintf(stderr, "[patch] streaming: inCd=%p req=%p loadAll=%p "
                    "hasSpecial=%p mspInst=%p\n",
            (void *)p_IsObjectInCdImage, (void *)p_RequestModel,
            (void *)p_LoadAllRequestedModels, (void *)p_HasSpecialCharLoaded,
            (void *)p_mspInst);

    /* OFF by default.  First attempt at these hooks reproducibly moved the crash
     * EARLIER — a bad free() during the first ConvertBufferToObject, identical
     * registers across two runs.  That first version had a broken log predicate
     * (`>= 109` matches almost every model id, so the 120-line cap never applied
     * and every loader call logged + fflushed); the bound is 109..129 now.
     *
     * The trampoline itself is NOT the suspect: both functions were checked and
     * are structurally safe to hook — only external `bl` sites target their
     * entries, and every `pop` matches the entry `push`.
     *
     * Whether fixing the predicate also fixes the regression is UNVERIFIED, so
     * these stay opt-in via GTALCS_LOADER_HOOKS=1.  Do not enable them in a
     * build handed to anyone without re-testing first. */
    /* Selectable, because enabling both at once destabilises the run and it is
     * not known which one is responsible:
     *   GTALCS_LOADER_HOOKS=cdnext   -> GetNextFileOnCd only
     *   GTALCS_LOADER_HOOKS=convert  -> ConvertBufferToObject only
     *   GTALCS_LOADER_HOOKS=1        -> both (the destabilising combination) */
    int want_cdnext = 0, want_convert = 0;
    {
        const char *e = getenv("GTALCS_LOADER_HOOKS");
        if (!e || !*e || !strcmp(e, "0")) goto skip_loader_hooks;
        if (!strcmp(e, "cdnext"))       want_cdnext = 1;
        else if (!strcmp(e, "convert")) want_convert = 1;
        else                            want_cdnext = want_convert = 1;
    }
    {
        struct { const char *sym; void *fn; void **orig; int want; } th[] = {
            { "_ZN10CStreaming15GetNextFileOnCdEib",
              (void *)GetNextFileOnCd_hook,       (void **)&orig_GetNextFileOnCd,
              want_cdnext },
            { "_ZN10CStreaming21ConvertBufferToObjectEPhi",
              (void *)ConvertBufferToObject_hook, (void **)&orig_ConvertBufferToObject,
              want_convert },
        };
        for (unsigned i = 0; i < sizeof th / sizeof th[0]; i++) {
            if (!th[i].want) continue;
            uintptr_t a = so_symbol(&gtalcs_mod, th[i].sym);
            if (a && hook_arm_trampoline(a, (uintptr_t)th[i].fn, th[i].orig) == 0)
                fprintf(stderr, "[patch] hooked %s @ %08x\n", th[i].sym, (unsigned)a);
            else
                fprintf(stderr, "[patch] %s NOT hooked (sym=%08x)\n",
                        th[i].sym, (unsigned)a);
        }
    }
skip_loader_hooks:

    /* GTALCS_PED_HOOKS=0 installs NO engine trampolines at all — the build then
     * behaves like the pre-instrumentation port.  Exists purely so a boot-time
     * failure can be attributed: if it reproduces with these off, the hooks are
     * not involved. */
    {
        const char *e = getenv("GTALCS_PED_HOOKS");
        if (e && !strcmp(e, "0")) {
            fprintf(stderr, "[patch] engine trampolines disabled by GTALCS_PED_HOOKS=0\n");
            return;
        }
    }
    {
        uintptr_t a = so_symbol(&gtalcs_mod, "_ZN10CStreaming19RequestSpecialModelEiPKci");
        if (a && hook_arm_trampoline(a, (uintptr_t)RequestSpecialModel_hook,
                                     (void **)&orig_RequestSpecialModel) == 0)
            fprintf(stderr, "[patch] hooked CStreaming::RequestSpecialModel @ %08x\n",
                    (unsigned)a);
        else
            fprintf(stderr, "[patch] CStreaming::RequestSpecialModel NOT hooked (sym=%08x)\n",
                    (unsigned)a);
    }

    {
        uintptr_t a = so_symbol(&gtalcs_mod, "_ZN4CPed13SetModelIndexEj");
        if (a && p_RequestModel && p_LoadAllRequestedModels &&
            hook_arm_trampoline(a, (uintptr_t)CPed_SetModelIndex_hook,
                                (void **)&orig_CPed_SetModelIndex) == 0) {
            fprintf(stderr, "[patch] hooked CPed::SetModelIndex @ %08x "
                            "(orig via trampoline %p)\n",
                    (unsigned)a, (void *)orig_CPed_SetModelIndex);
        } else {
            fprintf(stderr, "[patch] CPed::SetModelIndex NOT hooked "
                            "(sym=%08x req=%p load=%p)\n",
                    (unsigned)a, (void *)p_RequestModel,
                    (void *)p_LoadAllRequestedModels);
        }
    }
}



/* resolve addr via dladdr and write "  TAG: lib + 0xOFFSET  (sym+delta)\n".
 * Also recognises addresses inside libGTALcs.so (loaded via our own loader, so
 * dladdr can't see them) and reports their offset from load_bias — that's
 * the offset you can look up with `objdump -d libGTALcs.so`. */
static void write_addr(const char *tag, unsigned addr) {
    char buf[512];
    int n;
    /* Check libGTALcs.so range first — our loader's mmap isn't visible to dladdr */
    uintptr_t lb  = gtalcs_mod.load_bias;
    uintptr_t end = gtalcs_mod.text_base + gtalcs_mod.text_size;
    if (lb && addr >= lb && (uintptr_t)addr < end) {
        n = snprintf(buf, sizeof(buf), "  %s: %08x libGTALcs.so + 0x%x\n",
                     tag, addr, (unsigned)((uintptr_t)addr - lb));
        (void)!write(2, buf, n);
        return;
    }
    Dl_info info;
    if (dladdr((void *)(uintptr_t)addr, &info))
        n = snprintf(buf, sizeof(buf), "  %s: %s + 0x%tx  (sym %s+%td)\n",
            tag,
            info.dli_fname,
            (char *)(uintptr_t)addr - (char *)info.dli_fbase,
            info.dli_sname ? info.dli_sname : "?",
            info.dli_sname ? (char *)(uintptr_t)addr - (char *)info.dli_saddr : 0);
    else
        n = snprintf(buf, sizeof(buf), "  %s: %08x <not in any DSO>\n", tag, addr);
    (void)!write(2, buf, n);
}

/* ── SIGSEGV handler: print fault PC, LR, all regs, annotated stack ─────── */
static void segv_handler(int sig, siginfo_t *si, void *uc) {
    ucontext_t *ctx = uc;
    unsigned r0  = ctx->uc_mcontext.arm_r0;
    unsigned r1  = ctx->uc_mcontext.arm_r1;
    unsigned r2  = ctx->uc_mcontext.arm_r2;
    unsigned r3  = ctx->uc_mcontext.arm_r3;
    unsigned r4  = ctx->uc_mcontext.arm_r4;
    unsigned r5  = ctx->uc_mcontext.arm_r5;
    unsigned r6  = ctx->uc_mcontext.arm_r6;
    unsigned r7  = ctx->uc_mcontext.arm_r7;
    unsigned r8  = ctx->uc_mcontext.arm_r8;
    unsigned r9  = ctx->uc_mcontext.arm_r9;
    unsigned r10 = ctx->uc_mcontext.arm_r10;
    unsigned fp  = ctx->uc_mcontext.arm_fp;   /* r11 */
    unsigned ip  = ctx->uc_mcontext.arm_ip;   /* r12 */
    unsigned pc  = ctx->uc_mcontext.arm_pc;
    unsigned lr  = ctx->uc_mcontext.arm_lr;
    unsigned sp  = ctx->uc_mcontext.arm_sp;
    unsigned fa  = (unsigned)(uintptr_t)si->si_addr;
    char buf[512];
    int n;

    n = snprintf(buf, sizeof(buf),
        "\n=== CRASH sig=%d si_code=%d thread=%08x ===\n"
        "  PC=%08x LR=%08x SP=%08x fault=%08x\n"
        "  r0=%08x r1=%08x r2=%08x r3=%08x\n"
        "  r4=%08x r5=%08x r6=%08x r7=%08x\n"
        "  r8=%08x r9=%08x r10=%08x fp=%08x ip=%08x\n",
        sig, si->si_code, (unsigned)(uintptr_t)pthread_self(),
        pc, lr, sp, fa,
        r0, r1, r2, r3,
        r4, r5, r6, r7,
        r8, r9, r10, fp, ip);
    (void)!write(2, buf, n);

    write_addr("PC", pc);
    write_addr("LR", lr);
    /* If the fault address sits in/near a guarded allocation, say whose block
     * it was and who allocated/freed it (no-op unless GTALCS_GUARD=1). */
    guard_annotate(fa);

    /* Annotated stack: 96 words; check libGTALcs.so range and dladdr.
     * Deep enough that abort() from a libc-internal detection point still
     * shows the game frames that triggered it. */
    (void)!write(2, "  Stack (SP+0 .. SP+95):\n", 26);
    unsigned *spp = (unsigned *)(uintptr_t)sp;
    uintptr_t lb  = gtalcs_mod.load_bias;
    uintptr_t end = gtalcs_mod.text_base + gtalcs_mod.text_size;
    for (int i = 0; i < 96; i++) {
        unsigned word = spp[i];
        Dl_info info;
        if (lb && word >= lb && (uintptr_t)word < end) {
            n = snprintf(buf, sizeof(buf), "    [sp+%02d] %08x  libGTALcs.so+0x%x\n",
                         i, word, (unsigned)((uintptr_t)word - lb));
        } else if (dladdr((void *)(uintptr_t)word, &info) && info.dli_fname) {
            n = snprintf(buf, sizeof(buf), "    [sp+%02d] %08x  %s+0x%tx\n",
                i, word,
                info.dli_sname ? info.dli_sname : info.dli_fname,
                info.dli_sname
                    ? (char *)(uintptr_t)word - (char *)info.dli_saddr
                    : (char *)(uintptr_t)word - (char *)info.dli_fbase);
        } else {
            n = snprintf(buf, sizeof(buf), "    [sp+%02d] %08x\n", i, word);
        }
        (void)!write(2, buf, n);
    }

    /* /proc/self/maps for base addresses */
    (void)!write(2, "  /proc/self/maps:\n", 19);
    int mfd = open("/proc/self/maps", O_RDONLY);
    if (mfd >= 0) {
        char mbuf[4096];
        ssize_t rd;
        while ((rd = read(mfd, mbuf, sizeof(mbuf))) > 0)
            (void)!write(2, mbuf, rd);
        close(mfd);
    }
    (void)!write(2, "\n", 1);

    signal(sig, SIG_DFL);
    raise(sig);
}

/* ── Conditional libc time-function patching ───────────────────────────────
 * History: one device (RK3566/dArkOS ARM32) shipped a glibc whose
 * __clock_gettime64 / __gettimeofday64 dispatched through a NULL vDSO pointer
 * and jumped to PC=0 (SIGSEGV).  The workaround overwrote the function's
 * prologue with a trampoline into our own direct-syscall implementation.
 *
 * That workaround was UNCONDITIONAL, and on mainstream glibc (AmberELEC
 * RG351MP glibc 2.38, ROCKNIX glibc 2.41 — GitHub issues #1, #3) the native
 * function works perfectly.  Overwriting a healthy prologue then crashes with
 * SIGILL / ILL_ILLOPC at fn+0 (the trampoline bytes get decoded in the wrong
 * instruction set: our Thumb2 bytes over an ARM-mode libc function).
 *
 * Fix: probe the native function once, under a temporary SIGILL/SIGSEGV/SIGBUS
 * guard, BEFORE any threads are spawned.  Patch only if the native call
 * actually faults.  Healthy devices keep their untouched libc; the one broken
 * device still gets the trampoline.  The trampoline encoding is also made
 * ISA-aware (ARM vs Thumb2) as defence-in-depth for the fault branch. */

static sigjmp_buf            probe_jmp;
static volatile sig_atomic_t probe_faulted;

static void probe_fault_handler(int sig) {
    (void)sig;
    probe_faulted = 1;
    siglongjmp(probe_jmp, 1);
}

/* Call fn(a0, a1) under a fault guard.  Returns 1 if it faulted, else 0.
 * MUST run single-threaded (installs process-wide signal handlers). */
static int libc_time_fn_faults(void *fn, long a0, long a1) {
    struct sigaction sa, old_ill, old_segv, old_bus;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = probe_fault_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGILL,  &sa, &old_ill);
    sigaction(SIGSEGV, &sa, &old_segv);
    sigaction(SIGBUS,  &sa, &old_bus);

    probe_faulted = 0;
    if (sigsetjmp(probe_jmp, 1) == 0) {
        int (*f)(long, long) = (int (*)(long, long))fn;
        f(a0, a1);
    }

    sigaction(SIGILL,  &old_ill,  NULL);
    sigaction(SIGSEGV, &old_segv, NULL);
    sigaction(SIGBUS,  &old_bus,  NULL);
    return (int)probe_faulted;
}

/* Overwrite the prologue at `sym` with a literal-load branch to `target_fn`.
 * ISA of the branch matches the ISA of `sym` (Thumb bit in the symbol value);
 * `target_fn`'s own Thumb bit is preserved so interworking is correct.       */
static void install_time_trampoline(void *sym, void *target_fn, const char *name) {
    uint8_t  *fn     = (uint8_t *)((uintptr_t)sym & ~1u);   /* strip Thumb bit */
    int       thumb  = (uintptr_t)sym & 1u;                 /* target ISA of sym */
    uintptr_t target = (uintptr_t)target_fn;               /* keep its Thumb bit */

    uint8_t trampoline[8];
    if (thumb) {
        /* Thumb2:  ldr.w pc, [pc, #0]   (T3 literal load into PC) */
        trampoline[0] = 0xDF; trampoline[1] = 0xF8;
        trampoline[2] = 0x00; trampoline[3] = 0xF0;
    } else {
        /* ARM:     ldr pc, [pc, #-4]    (e51ff004) — loads the word that
         * immediately follows; LDR-into-PC interworks on ARMv5T+, so a
         * Thumb target address (bit0=1) switches state correctly. */
        trampoline[0] = 0x04; trampoline[1] = 0xF0;
        trampoline[2] = 0x1F; trampoline[3] = 0xE5;
    }
    trampoline[4] = (uint8_t)(target);
    trampoline[5] = (uint8_t)(target >> 8);
    trampoline[6] = (uint8_t)(target >> 16);
    trampoline[7] = (uint8_t)(target >> 24);

    uintptr_t pgsz = 4096;
    uintptr_t page = (uintptr_t)fn & ~(pgsz - 1u);
    if (mprotect((void *)page, pgsz, PROT_READ | PROT_WRITE | PROT_EXEC) < 0) {
        fprintf(stderr, "%s: mprotect failed: %s\n", name, strerror(errno));
        return;
    }
    memcpy(fn, trampoline, 8);
    __builtin___clear_cache((char *)fn, (char *)fn + 8);
    mprotect((void *)page, pgsz, PROT_READ | PROT_EXEC);
    fprintf(stderr, "%s: %s-mode trampoline @ %p -> %p\n",
            name, thumb ? "Thumb" : "ARM", (void *)fn, target_fn);
}

/* Look up `symname` in libc; probe it; patch only if the native call faults. */
static void patch_libc_time_fn(const char *symname, void *target_fn,
                               long a0, long a1, const char *name) {
    void *libc = dlopen("libc.so.6", RTLD_LAZY | RTLD_NOLOAD);
    if (!libc) {
        fprintf(stderr, "%s: libc.so.6 not found via dlopen\n", name);
        return;
    }
    void *sym = dlsym(libc, symname);
    dlclose(libc);
    if (!sym) {
        fprintf(stderr, "%s: %s not in libc\n", name, symname);
        return;
    }

    if (!libc_time_fn_faults(sym, a0, a1)) {
        fprintf(stderr, "%s: native %s works, leaving libc untouched\n",
                name, symname);
        return;
    }
    fprintf(stderr, "%s: native %s FAULTS, installing trampoline\n",
            name, symname);
    install_time_trampoline(sym, target_fn, name);
}

/* Probe scratch buffers: sized for the largest 64-bit-time struct glibc uses. */
static long probe_buf[4];

static void patch_libc_clock64(void) {
    patch_libc_time_fn("__clock_gettime64", (void *)clock_gettime64_safe,
                       (long)CLOCK_MONOTONIC, (long)probe_buf,
                       "patch_libc_clock64");
}

static void patch_libc_gettimeofday64(void) {
    patch_libc_time_fn("__gettimeofday64", (void *)gettimeofday64_safe,
                       (long)probe_buf, 0, "patch_libc_gettimeofday64");
}

static void patch_libc_gettimeofday(void) {
    patch_libc_time_fn("gettimeofday", (void *)gettimeofday_safe,
                       (long)probe_buf, 0, "patch_libc_gettimeofday");
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    /* Must precede any use of the clock: works around a vDSO SIGILL on some
     * of these SoCs by probing and, if needed, repointing libc's time calls. */
    ga_init();
    patch_libc_clock64();
    patch_libc_gettimeofday64();
    patch_libc_gettimeofday();

    const char *env_dir = getenv("GTALCS_DIR");
    if (env_dir)
        snprintf(g_data_path, sizeof(g_data_path), "%s", env_dir);

    /* Point the asset shim at <data>/assets, mirroring the APK's assets/. */
    {
        char assets[600];
        snprintf(assets, sizeof(assets), "%s/assets", g_data_path);
        asset_manager_set_root(assets);
    }

    /* The engine falls back to RELATIVE paths for some assets, so the working
     * directory has to be the game directory or those opens miss. */
    if (chdir(g_data_path) != 0)
        fprintf(stderr, "[main] WARNING: chdir(%s) failed\n", g_data_path);

    /* resolve_stream() maps bionic __sF offsets onto real glibc streams. */
    stderr_fake = stderr;

    /* ── SDL2 ───────────────────────────────────────────────────────────── */
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) < 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    /* The EGL backend is REQUIRED, not a preference: the game creates a shared
     * GL context against ours, which cannot work if SDL came up on GLX. */
    SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "1");
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE,   8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE,  8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    /* ── Multisampling ───────────────────────────────────────────────────
     * The engine renders into OUR window's default framebuffer: we are the
     * GLSurfaceView, and the only EGL surface the native side creates for
     * itself is a pbuffer used to stream resources on a background thread
     * (see egl_patch.c).  So a multisampled window config antialiases the
     * whole game, exactly as it does in the Chinatown Wars port.  Mali
     * resolves MSAA in tile memory, so 4x costs little on this GPU class.
     *
     * GTALCS_MSAA=<0|2|4|8> overrides it — the launcher exposes that as
     * conf/msaa.txt so it can be turned off on a device without a rebuild.
     * A sample count the driver cannot supply makes SDL_CreateWindow fail
     * outright, so the request is retried at 0 rather than killing the run. */
    int msaa = 4;
    {
        const char *e = getenv("GTALCS_MSAA");
        if (e && *e) msaa = atoi(e);
        if (msaa != 0 && msaa != 2 && msaa != 4 && msaa != 8) {
            fprintf(stderr, "[gl] GTALCS_MSAA=%s is not 0/2/4/8 — using 4\n", e);
            msaa = 4;
        }
    }

    for (;;) {
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, msaa ? 1 : 0);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, msaa);

        g_window = SDL_CreateWindow(
            "GTA: Liberty City Stories",
            SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
            SCREEN_W, SCREEN_H,
            SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP
        );
        if (g_window || msaa == 0) break;

        fprintf(stderr, "SDL_CreateWindow with %dx MSAA failed (%s) — "
                        "retrying without\n", msaa, SDL_GetError());
        msaa = 0;
    }
    if (!g_window) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }

    g_gl_ctx = SDL_GL_CreateContext(g_window);
    if (!g_gl_ctx) {
        fprintf(stderr, "SDL_GL_CreateContext: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_MakeCurrent(g_window, g_gl_ctx);
    SDL_GL_SetSwapInterval(1);

    /* Report what was actually granted, not what was asked for — a driver may
     * quietly hand back fewer samples, and "MSAA is on" is otherwise an
     * assumption rather than a measurement. */
    {
        int bufs = 0, samples = 0;
        SDL_GL_GetAttribute(SDL_GL_MULTISAMPLEBUFFERS, &bufs);
        SDL_GL_GetAttribute(SDL_GL_MULTISAMPLESAMPLES, &samples);
        fprintf(stderr, "[gl] MSAA: requested %dx, got buffers=%d samples=%d\n",
                msaa, bufs, samples);
    }

    /* Capture display/context/config while they are current on THIS thread —
     * everything the game's shared-context path needs comes from here. */
    egl_patch_capture();

    /* After SDL_Init so we replace SDL2's own SIGSEGV handler, which re-raises
     * via tgkill and hides the real fault PC. */
    {
        struct sigaction sa = { .sa_sigaction = segv_handler,
                                .sa_flags = SA_SIGINFO | SA_RESETHAND };
        sigemptyset(&sa.sa_mask);
        sigaction(SIGSEGV, &sa, NULL);
        sigaction(SIGABRT, &sa, NULL);
        sigaction(SIGILL,  &sa, NULL);
        sigaction(SIGBUS,  &sa, NULL);
    }

    /* ── Gamepad ────────────────────────────────────────────────────────── */
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) {
            g_gamepad = SDL_GameControllerOpen(i);
            if (g_gamepad) {
                fprintf(stderr, "Gamepad: %s\n", SDL_GameControllerName(g_gamepad));
                break;
            }
        }
    }

    /* ── Load libGTALcs.so ──────────────────────────────────────────────── */
    char so_path[560];
    snprintf(so_path, sizeof(so_path), "%s/libGTALcs.so", g_data_path);
    fprintf(stderr, "so_load: %s\n", so_path);
    if (so_load(&gtalcs_mod, so_path) < 0) {
        fprintf(stderr, "Failed to load %s\n", so_path);
        return 1;
    }
    fprintf(stderr, "so_load OK  (load_bias=%08x text=%08x+%zx)\n",
            (unsigned)gtalcs_mod.load_bias, (unsigned)gtalcs_mod.text_base,
            gtalcs_mod.text_size);

    /* Reproduce-on-demand switch for the _ctype_ off-by-one (see the table
     * above).  Must be set before so_resolve hands the engine the pointer. */
    if (getenv("GTALCS_CTYPE_OLD")) {
        ctype_ptr_val = android_ctype_table + 1;
        fprintf(stderr, "[ctype] GTALCS_CTYPE_OLD=1: _ctype_ deliberately shifted "
                        "by one — 'z' will not case-fold\n");
    }

    so_relocate(&gtalcs_mod);
    fprintf(stderr, "so_relocate OK\n");

    /* Three passes.  The first resolves everything it can, host libraries
     * included; the module tables then override their own symbols.  Passes 2
     * and 3 use only_dynlib=1 so they touch nothing outside their table. */
    so_resolve(&gtalcs_mod, default_dynlib, sizeof(default_dynlib), 0);
    so_resolve(&gtalcs_mod, (so_default_dynlib *)egl_dynlib,
               egl_dynlib_count * (int)sizeof(so_default_dynlib), 1);
    so_resolve(&gtalcs_mod, (so_default_dynlib *)openal_dynlib,
               openal_dynlib_count * (int)sizeof(so_default_dynlib), 1);
    fprintf(stderr, "so_resolve OK\n");

    /* Audit the tables for NULL implementations.
     *
     * "Every symbol is in the table" is NOT the same as "every symbol resolves".
     * An entry whose address is 0 is written into the GOT as 0, and ARM PLT
     * stubs jump via `ldr pc, [ip, ...]` WITHOUT setting LR — so the process
     * dies at PC=0 with a stale LR pointing at whatever made the last real
     * `bl`.  That is maximally misleading: the crash appears to be in an
     * unrelated leaf function.  Check it here instead of inferring it. */
    {
        int null_entries = 0;
        for (unsigned i = 0; i < sizeof(default_dynlib)/sizeof(default_dynlib[0]); i++)
            if (default_dynlib[i].func == 0) {
                fprintf(stderr, "[table] NULL IMPL: %s\n", default_dynlib[i].symbol);
                null_entries++;
            }
        for (int i = 0; i < egl_dynlib_count; i++)
            if (egl_dynlib[i].func == 0) {
                fprintf(stderr, "[table] NULL IMPL (egl): %s\n", egl_dynlib[i].symbol);
                null_entries++;
            }
        for (int i = 0; i < openal_dynlib_count; i++)
            if (openal_dynlib[i].func == 0) {
                fprintf(stderr, "[table] NULL IMPL (openal): %s\n", openal_dynlib[i].symbol);
                null_entries++;
            }
        fprintf(stderr, "[table] %d entries, %d with a NULL implementation\n",
                (int)(sizeof(default_dynlib)/sizeof(default_dynlib[0]))
                    + egl_dynlib_count + openal_dynlib_count,
                null_entries);
    }

    /* Expose the combined table to dlsym_hook, which the game uses instead of
     * dlopen+dlsym (it imports dlsym but never dlopen). */
    g_dynlib_all       = default_dynlib;
    g_dynlib_all_count = sizeof(default_dynlib) / sizeof(default_dynlib[0]);

    /* Keep OpenAL's mixer off RTKit: it is unavailable on these devices, and
     * the failed thread-priority path crashes rather than degrading. */
    {
        const char *conf_path = "/tmp/gtalcs-alsoft.conf";
        FILE *cf = fopen(conf_path, "w");
        if (cf) {
            fprintf(cf, "[general]\nrt-prio = 0\ndrivers = alsa\n\n"
                        "[alsa]\ndevice = default\ncapture = \n");
            fclose(cf);
            setenv("ALSOFT_CONF", conf_path, 1);
        }
    }

    patch_openal();
    patch_opengl();
    patch_game();

    so_flush_caches(&gtalcs_mod);
    fprintf(stderr, "so_initialize...\n");
    so_initialize(&gtalcs_mod);
    fprintf(stderr, "so_initialize OK — libGTALcs.so is live\n");

    /* ── JNI bootstrap and game loop ────────────────────────────────────── */
    jni_init();
    jni_boot_lcs();   /* runs the game loop; does not return */

    SDL_GL_DeleteContext(g_gl_ctx);
    SDL_DestroyWindow(g_window);
    SDL_Quit();
    return 0;
}
