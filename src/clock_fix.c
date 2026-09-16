/* clock_fix.c -- bypass glibc's broken __clock_gettime64 vDSO dispatch.
 *
 * On this device (RK3566/dArkOS ARM32), glibc's __clock_gettime64 has a NULL
 * vDSO function pointer and crashes.  libopenal.so (glibc 2.34+) imports
 * __clock_gettime64@GLIBC_2.34 directly by the internal symbol name.
 *
 * We define both __clock_gettime64 and clock_gettime as global functions so
 * our executable's symbols win over libc's for all DSOs loaded later.
 *
 * IMPORTANT: do NOT include <time.h> here.  glibc's <time.h> has an __asm__
 * alias that redirects clock_gettime -> __clock_gettime64 at the object level,
 * which would produce duplicate symbol errors when both are defined.
 */

/* Only include headers that do NOT touch clock_gettime.
 * pthread.h must NOT be included here: it pulls in time.h which has an
 * __asm__ alias redirecting clock_gettime -> __clock_gettime64 at object
 * level, causing duplicate-symbol errors when both are defined in this TU. */
#define _GNU_SOURCE
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>

/* Declare pthread_exit without including pthread.h (see above). */
extern void pthread_exit(void *) __attribute__((__noreturn__));

typedef int clockid_t;

/* Raw ARM32 inline-asm syscall: two arguments.
 * glibc's syscall() wrapper goes through the vDSO for some syscalls (including
 * clock_gettime64/403), and on this device the vDSO pointer is NULL → PC=0.
 * We bypass glibc entirely with a direct swi #0.
 * r7 is the frame pointer under -fno-omit-frame-pointer, so we cannot use it
 * as a named asm constraint — push/pop it manually around the swi.          */
static long raw_syscall2(long nr, long a0, long a1) {
    register long _r0 asm("r0") = a0;
    register long _r1 asm("r1") = a1;
    asm volatile(
        "push {r7}\n\t"
        "mov  r7, %[nr]\n\t"
        "swi  #0\n\t"
        "pop  {r7}"
        : "+r"(_r0)
        : "r"(_r1), [nr]"r"(nr)
        : "memory", "cc", "r2", "r3", "ip", "lr"
    );
    return _r0;
}

/* ARM32 Linux syscall numbers */
#define NR_clock_gettime    263
#define NR_clock_gettime64  403   /* Linux 5.1+ */

/* glibc's internal struct __timespec64 on ARM32 little-endian:
 *   tv_sec  (8 bytes, int64)
 *   tv_nsec (4 bytes, int32)
 *   __pad   (4 bytes)                                               */
struct ts64 { long long tv_sec; int tv_nsec; int __pad; };

/* glibc's public struct timespec on ARM32 (32-bit time_t):
 *   tv_sec  (4 bytes, long)
 *   tv_nsec (4 bytes, long)                                         */
struct ts32 { long tv_sec; long tv_nsec; };

/* clock_gettime64_safe: installed as a Thumb2 trampoline replacement for
 * glibc's __clock_gettime64 by patch_libc_clock64() in main.c.
 *
 * Uses raw swi #0 (not glibc's syscall()) to avoid the broken vDSO path.
 *
 * Handles two failure modes:
 *   1. tp = 0x3ff00000 (high word of IEEE 754 1.0 — not a valid pointer):
 *      skip the write; call succeeds vacuously.
 *   2. LR = 0 at the call site (caller used BX/tail-call with LR=0):
 *      __builtin_return_address(0) is 0; exit the thread via pthread_exit. */
int clock_gettime64_safe(clockid_t clk_id, struct ts64 *tp) {
    void *ra = __builtin_return_address(0);

    if ((uintptr_t)(void *)tp > 0x10000u) {
        long r = raw_syscall2(NR_clock_gettime64, (long)clk_id, (long)tp);
        if (r != 0) {
            struct ts32 ts;
            r = raw_syscall2(NR_clock_gettime, (long)clk_id, (long)&ts);
            if (r == 0) {
                tp->tv_sec  = ts.tv_sec;
                tp->tv_nsec = (int)ts.tv_nsec;
                tp->__pad   = 0;
            }
        }
    }

    if (!ra) {
        static int warned = 0;
        if (!warned) {
            warned = 1;
            const char msg[] = "clock_gettime64_safe: LR=0, exiting thread\n";
            (void)!write(2, msg, sizeof(msg) - 1);
        }
        pthread_exit(NULL);
        __builtin_unreachable();
    }
    return 0;
}

int __clock_gettime64(clockid_t clk_id, struct ts64 *tp) {
    long r = raw_syscall2(NR_clock_gettime64, (long)clk_id, (long)tp);
    if (r == 0) return 0;
    if (r != -38) /* ENOSYS */ return (int)r;
    struct ts32 ts;
    r = raw_syscall2(NR_clock_gettime, (long)clk_id, (long)&ts);
    if (r == 0) {
        tp->tv_sec  = ts.tv_sec;
        tp->tv_nsec = (int)ts.tv_nsec;
        tp->__pad   = 0;
    }
    return (int)r;
}

int clock_gettime(clockid_t clk_id, struct ts32 *tp) {
    long r = raw_syscall2(NR_clock_gettime, (long)clk_id, (long)tp);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

/* ── gettimeofday / __gettimeofday64 safe replacements ──────────────────────
 * glibc's __gettimeofday64 has the same broken vDSO dispatch as
 * __clock_gettime64: it loads a NULL vDSO function pointer and does
 * blx r3 → PC=0.  We provide:
 *   gettimeofday64_safe  — trampoline target for patching __gettimeofday64
 *   gettimeofday         — global override for PLT callers (SDL2, OpenAL, …)
 * ARM32 gettimeofday syscall: NR=78; args r0=timeval*, r1=timezone*          */

#define NR_gettimeofday 78

/* glibc's internal struct __timeval64 on ARM32:
 *   tv_sec  (8 bytes, int64)
 *   tv_usec (8 bytes, int64)                                                  */
struct tv64 { long long tv_sec; long long tv_usec; };

/* Public 32-bit struct timeval:
 *   tv_sec  (4 bytes, long)
 *   tv_usec (4 bytes, long)                                                   */
struct tv32 { long tv_sec; long tv_usec; };

/* gettimeofday64_safe: installed as a Thumb2 trampoline replacement for
 * glibc's __gettimeofday64 by patch_libc_gettimeofday64() in main.c.       */
int gettimeofday64_safe(struct tv64 *tv, void *tz) {
    void *ra = __builtin_return_address(0);

    if ((uintptr_t)(void *)tv > 0x10000u) {
        struct tv32 tv2;
        long r = raw_syscall2(NR_gettimeofday, (long)&tv2, (long)tz);
        if (r == 0) {
            tv->tv_sec  = tv2.tv_sec;
            tv->tv_usec = tv2.tv_usec;
        }
    }

    if (!ra) {
        static int warned2 = 0;
        if (!warned2) {
            warned2 = 1;
            const char msg[] = "gettimeofday64_safe: LR=0, exiting thread\n";
            (void)!write(2, msg, sizeof(msg) - 1);
        }
        pthread_exit(NULL);
        __builtin_unreachable();
    }
    return 0;
}

/* gettimeofday_safe: trampoline target for patching libc's gettimeofday by
 * patch_libc_gettimeofday() in main.c.  Also used directly by so_resolve via
 * the dynlib table ("gettimeofday" → gettimeofday_safe) to fill the game's
 * NULL GOT entry. */
int gettimeofday_safe(struct tv32 *tv, void *tz) {
    long r = raw_syscall2(NR_gettimeofday, (long)tv, (long)tz);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

/* Global gettimeofday override: intercepts PLT callers in ALL DSOs loaded
 * after our executable (SDL2, OpenAL, …).  Delegates to gettimeofday_safe
 * so the direct-syscall path is shared with the trampoline target.
 * No <sys/time.h> here — the __asm__ alias would cause duplicate symbols.  */
int gettimeofday(struct tv32 *tv, void *tz) {
    return gettimeofday_safe(tv, tz);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Bionic-layout time calls
 *
 * THE BUG THIS FIXES (found 2026-09-07 on RG353V, glibc 2.41):
 *
 * Bionic's 32-bit struct timespec is 8 bytes — a 32-bit time_t plus a 32-bit
 * tv_nsec.  libGTALcs.so was compiled against that, and Platform::GetSystemTicks
 * allocates exactly 8 bytes of stack for one:
 *
 *     sub sp, sp, #8  /  bl clock_gettime  /  ldr [fp,#-12], ldr [fp,#-8]
 *
 * Modern glibc on ARM32 uses a 64-bit time_t, so its struct timespec is 16
 * bytes.  Handing the game's 8-byte buffer to glibc's clock_gettime overruns
 * it by 8 bytes and lands squarely on the saved fp and lr — the function then
 * returns through `pop {fp, pc}` into whatever the overwrite left behind,
 * which is why the crash shows PC=0 with a stale LR pointing at unrelated
 * code.  Nothing about the symptom points at the real cause.
 *
 * So every time-related call the game imports must use the 32-bit layout and
 * bypass glibc entirely.  The kernel's legacy syscalls still write exactly
 * that layout, so we go straight to them.
 *
 * These are y2038-limited by construction.  That is correct here: it is the
 * ABI the game was built against, and matching it is the whole point.
 * ══════════════════════════════════════════════════════════════════════════ */

/* struct tv32 is already defined above */

#define NR_gettimeofday 78
#define NR_nanosleep    162

int bionic_clock_gettime(clockid_t clk_id, struct ts32 *tp) {
    return (int)raw_syscall2(NR_clock_gettime, (long)clk_id, (long)tp);
}

int bionic_gettimeofday(struct tv32 *tv, void *tz) {
    (void)tz;
    return (int)raw_syscall2(NR_gettimeofday, (long)tv, 0);
}

int bionic_nanosleep(const struct ts32 *req, struct ts32 *rem) {
    return (int)raw_syscall2(NR_nanosleep, (long)req, (long)rem);
}

/* time(2) returns a 32-bit time_t to the game; derive it from the same call
 * rather than glibc's, whose time_t is 64-bit here. */
long bionic_time(long *t) {
    struct tv32 tv = { 0, 0 };
    raw_syscall2(NR_gettimeofday, (long)&tv, 0);
    if (t) *t = tv.tv_sec;
    return tv.tv_sec;
}
