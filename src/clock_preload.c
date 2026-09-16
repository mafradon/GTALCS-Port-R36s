/* clock_preload.c -- LD_PRELOAD DSO: versioned __clock_gettime64@@GLIBC_2.34
 *
 * Problem: libc.so.6's __clock_gettime64 calls a NULL vDSO pointer on this
 * device and crashes.  libstdc++.so.6 references __clock_gettime64@GLIBC_2.34
 * (a versioned symbol), so the unversioned override in our executable loses.
 *
 * Fix: compile this as a shared lib with @@GLIBC_2.34 annotation and
 * LD_PRELOAD it.  LD_PRELOAD DSOs come first in the link map, so libstdc++'s
 * PLT lookup for __clock_gettime64@GLIBC_2.34 finds us before libc.
 *
 * Uses only inline asm syscalls — no libc dependency at all.
 *
 * Build:
 *   arm-linux-gnueabihf-gcc -O1 -fPIC -shared -nostdlib \
 *     -march=armv7-a -mfpu=neon -mfloat-abi=hard \
 *     -Wl,-soname,libclock_fix.so \
 *     -o libclock_fix.so src/clock_preload.c
 */

#define NR_clock_gettime   263
#define NR_clock_gettime64 403
#define ENOSYS_NEG         (-38)

typedef int clockid_t;
typedef struct { long long tv_sec; int tv_nsec; int pad; } ts64_t;
typedef struct { long tv_sec; long tv_nsec; } ts32_t;

static long syscall2(long nr, long a0, long a1) {
    register long r0 asm("r0") = a0;
    register long r1 asm("r1") = a1;
    register long r7 asm("r7") = nr;
    asm volatile("swi #0" : "+r"(r0) : "r"(r1), "r"(r7) : "memory", "cc");
    return r0;
}

/* clock_gettime: intercept PLT calls from system libs (SDL2, libmali, OpenAL,
 * libstdc++, etc.) so their clock_gettime never reaches libc's internal
 * __clock_gettime64 → NULL vDSO crash path.
 * Version script assigns this the GLIBC_2.4 tag (@@GLIBC_2.4). */
int clock_gettime(clockid_t id, ts32_t *tp) {
    return (int)syscall2(NR_clock_gettime, (long)id, (long)tp);
}

/* Named exactly __clock_gettime64 so the version script assigns it
 * the default GLIBC_2.34 version tag (@@GLIBC_2.34). */
int __clock_gettime64(clockid_t id, ts64_t *tp) {
    long r = syscall2(NR_clock_gettime64, (long)id, (long)tp);
    if (r == 0) return 0;
    /* Fall back to 32-bit syscall on ENOSYS or any error */
    ts32_t ts;
    r = syscall2(NR_clock_gettime, (long)id, (long)&ts);
    if (r == 0) {
        tp->tv_sec  = ts.tv_sec;
        tp->tv_nsec = (int)ts.tv_nsec;
        tp->pad     = 0;
    }
    return (int)r;
}
