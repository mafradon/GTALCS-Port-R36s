/* guard_alloc.c — Electric-Fence-style debug allocator for libGTALcs.so
 *
 * WHY THIS EXISTS
 *
 * The engine aborts in malloc_consolidate() ("unaligned fastbin chunk") some
 * tens of seconds into a run: a chunk header was corrupted earlier by a wild
 * or stale write and glibc only notices when linking free lists.  Indirect
 * probes (MALLOC_CHECK_, MALLOC_PERTURB_, wider crash dumps) moved detection
 * around without naming the corruptor — by the time glibc notices, the frame
 * that caused it has returned.
 *
 * So make the corruption fault where it happens:
 *   - every game allocation is tail-placed so its LAST byte abuts a PROT_NONE
 *     guard page → a one-byte overflow faults immediately;
 *   - free() makes the block's pages PROT_NONE → any later read or write
 *     faults at the exact instruction, with LR pointing at the corruptor;
 *   - double-free / free-of-foreign is caught directly via the live table.
 *
 * Only the game's four allocator imports (malloc/free/realloc/calloc) route
 * here, and only when GTALCS_GUARD=1; otherwise every function passes straight
 * to glibc, so the shipping build is untouched.  When guard regions are
 * exhausted we fall back to plain malloc (those pointers are never in the
 * table and not inside a guard region, so free() tells them apart).
 *
 * Block layout for n bytes: a region [B, B+align_up(n)+PAGE), user data
 * tail-placed at ptr = B+align_up(n)-n, guard page [B+align_up(n),
 * B+align_up(n)+PAGE).  The tail is therefore always page-aligned, which is
 * all free() needs to recover the layout from (ptr, n) alone.
 *
 * The allocator is called from several game threads at once, so all state is
 * under one spinlock.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>

static size_t g_page;
static int    g_on;
static int    g_lock;                        /* spinlock */

static void lock(void)   { while (__atomic_exchange_n(&g_lock, 1, __ATOMIC_ACQUIRE)) ; }
static void unlock(void) { __atomic_store_n(&g_lock, 0, __ATOMIC_RELEASE); }

#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

/* ── regions: PROT_NONE reserves we RW-on-demand ─────────────────────────── */
#define NREGION 8
static uintptr_t g_rbase[NREGION], g_rcur[NREGION], g_rend[NREGION];
static int       g_nregions;

static int region_grow_unlocked(void) {
    if (g_nregions >= NREGION) return 0;
    size_t sz = 128u << 20;
    void *m = mmap(NULL, sz, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (m == MAP_FAILED) return 0;
    g_rbase[g_nregions] = (uintptr_t)m;
    g_rcur[g_nregions]  = (uintptr_t)m;
    g_rend[g_nregions]  = (uintptr_t)m + sz;
    g_nregions++;
    fprintf(stderr, "[guard] region %d: %08lx + %zu MB\n",
            g_nregions, (unsigned long)(uintptr_t)m, sz >> 20);
    return 1;
}

/* ── hole list (sorted by start, coalesced) ──────────────────────────────── */
typedef struct { uintptr_t s, e; } hole_t;
static hole_t *g_holes;
static int     g_nholes, g_holes_cap;

static void hole_add(uintptr_t s, uintptr_t e) {
    if (g_nholes == g_holes_cap) {
        g_holes_cap = g_holes_cap ? g_holes_cap * 2 : 1024;
        g_holes = realloc(g_holes, (size_t)g_holes_cap * sizeof(hole_t));
    }
    int i = 0;
    while (i < g_nholes && g_holes[i].s < s) i++;
    memmove(&g_holes[i + 1], &g_holes[i], (size_t)(g_nholes - i) * sizeof(hole_t));
    g_holes[i] = (hole_t){ s, e };
    g_nholes++;
    if (i + 1 < g_nholes && g_holes[i].e == g_holes[i + 1].s) {   /* merge next */
        g_holes[i].e = g_holes[i + 1].e;
        memmove(&g_holes[i + 1], &g_holes[i + 2],
                (size_t)(--g_nholes - i - 1) * sizeof(hole_t));
    }
    if (i > 0 && g_holes[i - 1].e == g_holes[i].s) {              /* merge prev */
        g_holes[i - 1].e = g_holes[i].e;
        memmove(&g_holes[i], &g_holes[i + 1],
                (size_t)(--g_nholes - i) * sizeof(hole_t));
    }
}

static uintptr_t hole_take(size_t need, uintptr_t *B_out) {
    for (int i = 0; i < g_nholes; i++) {
        if (g_holes[i].e - g_holes[i].s < need) continue;
        uintptr_t B = g_holes[i].e - need;        /* tail-fit keeps alignment */
        g_holes[i].e = B;
        if (g_holes[i].s == g_holes[i].e)
            memmove(&g_holes[i], &g_holes[i + 1],
                    (size_t)(--g_nholes - i) * sizeof(hole_t));
        *B_out = B;
        return 1;
    }
    return 0;
}

/* ── live table: open-addressed ptr→size hash ────────────────────────────── */
/* 2^20 entries (12 MB of tables).  16 bits was not enough: GTA's world load
 * holds more than 65536 live allocations, and once the table fills every
 * further block is untracked — which also makes core_free report spurious
 * DOUBLE FREEs for blocks it simply never recorded.
 *
 * Be aware this instrument does not scale all the way here regardless: the
 * page-guard design costs a minimum of two pages (8 KB) of address space per
 * allocation, so ~200k live blocks would need ~1.6 GB in a 3 GB user space.
 * For the world-load corruption a redzone/canary allocator (magic pattern
 * before and after each block, validated on free) is the right next tool —
 * it catches the overflow WRITES that corrupt glibc's chunk metadata without
 * the address-space blowup. */
#define HT_SIZE_BITS 20
#define HT_SIZE (1u << HT_SIZE_BITS)
static uintptr_t g_ht_p[HT_SIZE];             /* 0 = empty, 1 = tombstone */
static size_t    g_ht_n[HT_SIZE];
static uintptr_t g_ht_lr[HT_SIZE];            /* malloc caller */

/* ── freed ring: last FREE_RING frees, for fault-time attribution ────────── */
#define FREE_RING 2048
typedef struct {
    uintptr_t ptr;
    size_t    size;
    uintptr_t free_lr, alloc_lr;
} free_rec;
static free_rec g_ring[FREE_RING];
static unsigned g_ring_n;                     /* monotonic; & (FREE_RING-1) */

static void ring_push(uintptr_t p, size_t n, uintptr_t flr, uintptr_t alr) {
    g_ring[(g_ring_n++) & (FREE_RING - 1)] =
        (free_rec){ p, n, flr, alr };
}

/* Called from the crash handler: if addr sits in (or adjacent to) a recently
 * freed block, name both ends of its lifetime.  Returns 1 if we said
 * something.  Uses raw (void)!write() + manual hex formatting: async-signal-safe. */
static void wr(const char *s) { (void)!write(2, s, strlen(s)); }
static void wr_hex(unsigned long v) {
    char b[11] = "0x";
    int  i;
    for (i = 0; i < 8; i++)
        b[2 + i] = "0123456789abcdef"[(v >> ((7 - i) * 4)) & 0xF];
    b[10] = 0; wr(b);
}
static void wr_dec(unsigned long v) {
    char b[12]; int i = 0;
    if (!v) { wr("0"); return; }
    while (v) { b[i++] = '0' + (v % 10); v /= 10; }
    char r[12]; int j = 0;
    while (i) r[j++] = b[--i];
    r[j] = 0; wr(r);
}
int guard_annotate(unsigned long addr) {
    if (!g_on) return 0;
    unsigned start = g_ring_n > FREE_RING ? g_ring_n - FREE_RING : 0;
    for (unsigned k = start; k < g_ring_n; k++) {
        free_rec *f = &g_ring[k & (FREE_RING - 1)];
        uintptr_t lo = f->ptr, hi = f->ptr + f->size;
        /* include a page either side: wild writes land near the block */
        if (addr + g_page >= lo && addr < hi + g_page) {
            wr("  [guard] addr "); wr_hex(addr);
            wr(" is inside/adjacent to a block freed ");
            wr_dec(g_ring_n - 1 - k); wr(" frees ago: block=");
            wr_hex(f->ptr); wr(" size="); wr_dec(f->size);
            wr(" freed_by="); wr_hex(f->free_lr);
            wr(" alloc_by="); wr_hex(f->alloc_lr); wr("\n");
            return 1;
        }
    }
    /* check liveness too */
    for (unsigned h = 0; h < HT_SIZE; h++) {
        if (g_ht_p[h] > 1 && addr >= g_ht_p[h] && addr < g_ht_p[h] + g_ht_n[h]) {
            wr("  [guard] addr "); wr_hex(addr);
            wr(" is LIVE inside block "); wr_hex(g_ht_p[h]);
            wr(" offset="); wr_dec(addr - g_ht_p[h]);
            wr(" size="); wr_dec(g_ht_n[h]);
            wr(" alloc_by="); wr_hex(g_ht_lr[h]); wr("\n");
            return 1;
        }
    }
    return 0;
}

static void ht_put(uintptr_t p, size_t n, uintptr_t lr) {
    unsigned h = (unsigned)((p >> 12) & (HT_SIZE - 1));
    for (unsigned i = 0; i < HT_SIZE; i++, h = (h + 1) & (HT_SIZE - 1))
        if (g_ht_p[h] <= 1) {
            g_ht_p[h] = p; g_ht_n[h] = n; g_ht_lr[h] = lr; return;
        }
    fprintf(stderr, "[guard] live table full — allocation untracked\n");
}
static int ht_del(uintptr_t p, size_t *n_out, uintptr_t *lr_out) {
    unsigned h = (unsigned)((p >> 12) & (HT_SIZE - 1));
    for (unsigned i = 0; i < HT_SIZE; i++, h = (h + 1) & (HT_SIZE - 1)) {
        if (g_ht_p[h] == 0) return 0;
        if (g_ht_p[h] == p) {
            *n_out = g_ht_n[h]; *lr_out = g_ht_lr[h];
            g_ht_p[h] = 1; return 1;
        }
    }
    return 0;
}
static int ht_get(uintptr_t p, size_t *n_out) {
    unsigned h = (unsigned)((p >> 12) & (HT_SIZE - 1));
    for (unsigned i = 0; i < HT_SIZE; i++, h = (h + 1) & (HT_SIZE - 1)) {
        if (g_ht_p[h] == 0) return 0;
        if (g_ht_p[h] == p) { *n_out = g_ht_n[h]; return 1; }
    }
    return 0;
}

/* ── core ────────────────────────────────────────────────────────────────── */

static size_t ga_slack(void) {
    static size_t v; static int done;
    if (!done) {
        done = 1;
        const char *e = getenv("GTALCS_GUARD_SLACK");
        v = (e && *e) ? (size_t)atoi(e) : 16;
    }
    return v;
}

static void *core_malloc(size_t n, void *caller) {
    if (n == 0) n = 1;
    /* The slack is part of the block, so it must be inside `body`.  Sizing
     * body from n alone and then placing the pointer at tail-(n+slack) puts
     * the user data BEFORE the block start, in the previous block's guard
     * page — which is exactly how the first slack build failed: core_malloc
     * handed back protected memory and our own ga_calloc memset faulted on
     * it at offset 0.  A guard allocator that returns unmapped memory is
     * worse than none. */
    size_t body = ALIGN_UP(n + ga_slack(), g_page);
    size_t need = body + g_page;

    lock();
    uintptr_t B = 0;
    while (!B) {
        if (hole_take(need, &B)) break;
        for (int r = 0; r < g_nregions && !B; r++)
            if (g_rcur[r] + need <= g_rend[r]) { B = g_rcur[r]; g_rcur[r] += need; }
        if (B) break;
        /* nothing free enough: try a fresh region (huge single blocks just
         * fall through to glibc) */
        if (need > (64u << 20) || !region_grow_unlocked()) { unlock(); return malloc(n); }
    }
    uintptr_t tail = B + body;
    if (mprotect((void *)B, body, PROT_READ | PROT_WRITE) != 0) {
        unlock();
        return malloc(n);
    }
    /* malloc contract: returned memory must be max_align_t-aligned.  Tail
     * placement alone misaligns whenever n%8!=0 — an early guard run proved
     * that: the game's std::string ldm of a 4-byte-aligned header faulted
     * with SIGBUS on a pointer that was correct-but-misaligned.
     *
     * Rounding the POINTER down to 8 (the first attempt at this) fixes the
     * alignment and breaks everything else: core_free recovers the block from
     * p + n, so a rounded-down p makes that sum fall short of the page-aligned
     * tail by up to 7 bytes.  mprotect then rejects the misaligned base, the
     * block is never re-protected, and the hole recorded for it is off by the
     * same amount — so a later allocation can be handed memory that overlaps a
     * live one.  A corrupting allocator hunting a corruption bug.
     *
     * Round the SIZE up instead: ptr = tail - align_up(n,8) is 8-aligned
     * because tail is page-aligned, and storing the padded size keeps
     * p + stored_n == tail exactly.  Cost: the overflow guard trips up to 7
     * bytes late. */
    /* GTALCS_GUARD_SLACK=<bytes>: shift the block down so that this many bytes
     * of *mapped* memory sit between the end of the user data and the guard
     * page.  Real allocators always have something mapped there, and engine
     * code relies on it — the first guard run died at ReadTextureMetaData+0x438
     * on `ldrb r3,[r9,#1]`, a CSV parser peeking one byte past its buffer,
     * before world load was ever reached.  With slack, benign over-READS land
     * in mapped memory while anything reaching past the slack still faults, so
     * the guard survives long enough to see the code that matters.  Rounded up
     * to 8 to preserve alignment; 0 = strict (the original behaviour). */
    size_t    na  = ((n + ga_slack()) + 7u) & ~(size_t)7u;
    uintptr_t ptr = tail - na;
    if (ptr & 7u) {                      /* cannot happen; assert it anyway */
        fprintf(stderr, "[guard] FATAL: misaligned block %p (n=%zu na=%zu "
                        "tail=%p caller=%p)\n",
                (void *)ptr, n, na, (void *)tail, caller);
        abort();
    }
    ht_put(ptr, na, (uintptr_t)caller);
    unlock();
    return (void *)ptr;
}

/* returns 0 if p was live and is now released; 1 if not live but guard-owned
 * (double free); 2 if not guard-owned at all (host block). */
static int core_free(void *p, void *caller) {
    lock();
    size_t n = 0;
    uintptr_t altr = 0;
    if (!ht_del((uintptr_t)p, &n, &altr)) {
        int in_region = 0;
        for (int i = 0; i < g_nregions; i++)
            if ((uintptr_t)p >= g_rbase[i] && (uintptr_t)p < g_rend[i]) { in_region = 1; break; }
        unlock();
        return in_region ? 1 : 2;
    }
    size_t body = ALIGN_UP(n, g_page);
    uintptr_t tail = ((uintptr_t)p) + n;           /* page-aligned by placement */
    uintptr_t B = tail - body;
    mprotect((void *)B, body, PROT_NONE);          /* UAF faults from here on */
    hole_add(B, tail + g_page);
    ring_push((uintptr_t)p, n, (uintptr_t)caller, altr);
    unlock();
    return 0;
}


/* ── canary (redzone) allocator ──────────────────────────────────────────────
 *
 * The page-guard mode above is the right instrument for a low-volume path and
 * unusable for a streaming game: two pages of address space per block, and
 * GTA's world load holds tens of thousands of live blocks inside a 32-bit
 * address space.  For heap-METADATA corruption — "malloc_consolidate():
 * unaligned fastbin chunk", "munmap_chunk(): invalid pointer" — the cheap
 * instrument is a redzone:
 *
 *   [ hdr 32B ][ user n bytes ][ pad to 8 ][ ftr 8B ]
 *
 * with a magic keyed to the block's own address (so a foreign pointer cannot
 * collide), the allocation's call site recorded, and both canaries validated
 * on free.  A neighbour that overflows its buffer smashes THIS block's header,
 * and the report names the victim's size and allocation site — which is what
 * actually locates the corruptor.
 *
 * GTALCS_CANARY=1 turns it on.  GTALCS_CANARY_SWEEP=<n> additionally walks the
 * whole live list every n allocations, so corruption is caught near the write
 * rather than whenever the victim happens to be freed.
 *
 * Invariants are asserted and reported loudly — a silent, broken allocator is
 * worse than none, which this port has already paid for once. */

#define CAN_HDR_MAGIC 0x4C435348u        /* "LCSH" */
#define CAN_FTR_MAGIC 0x4C435346u        /* "LCSF" */
#define CAN_FTR_SIZE  8u
/* Header size is sizeof(can_hdr), NOT a hardcoded constant: it holds two
 * pointers, so it is 32 bytes on the 32-bit target and 48 on a 64-bit host.
 * Writing 32 here made the footer land outside the block on the host — caught
 * by the placement invariant below on the very first self-test allocation. */

typedef struct can_hdr {
    uint32_t        magic;               /* CAN_HDR_MAGIC ^ (uintptr_t)user  */
    uint32_t        size;                /* n as requested                    */
    uint32_t        lr;                  /* allocation call site              */
    uint32_t        seq;                 /* allocation ordinal                */
    struct can_hdr *prev, *next;         /* live list — no fixed-size table   */
    uint32_t        pad[2];
} can_hdr;

static int       g_can;                  /* canary mode on */
static can_hdr  *g_can_head;
static uint32_t  g_can_seq;
static unsigned  g_can_sweep;            /* sweep every n allocations, 0 = off */
static unsigned  g_can_reports;

static uint32_t can_key(const void *user) {
    return CAN_HDR_MAGIC ^ (uint32_t)(uintptr_t)user;
}
static uint32_t *can_ftr(can_hdr *h) {
    return (uint32_t *)((char *)(h + 1) + ALIGN_UP(h->size, 8));
}

static int g_can_selftest;               /* suppress reports we caused ourselves */

static void can_report(const char *what, can_hdr *h, void *user, void *caller) {
    g_can_reports++;
    /* The init self-test deliberately smashes a footer.  Printing that looks
     * exactly like a real finding in the log, which is how a good instrument
     * starts costing debugging time. */
    if (g_can_selftest || g_can_reports > 40) return;
    fprintf(stderr,
            "[canary] *** %s *** block=%p size=%u allocated-by=0x%08x seq=%u\n"
            "[canary]     header magic=0x%08x (expected 0x%08x)  footer=0x%08x "
            "(expected 0x%08x)\n"
            "[canary]     detected by=%p\n",
            what, user, h ? h->size : 0, h ? h->lr : 0, h ? h->seq : 0,
            h ? h->magic : 0, (unsigned)can_key(user),
            (h && h->size < (1u << 28)) ? *can_ftr(h) : 0,
            (unsigned)CAN_FTR_MAGIC, caller);
    fflush(stderr);
}

/* Validate one block.  Returns 0 when both canaries are intact. */
static int can_check(can_hdr *h, void *user, void *caller, const char *what) {
    if (h->magic != can_key(user)) { can_report(what, h, user, caller); return 1; }
    /* Only trust size once the header magic has verified. */
    if (*can_ftr(h) != CAN_FTR_MAGIC) {
        can_report("FOOTER SMASHED (overflow past the end)", h, user, caller);
        return 1;
    }
    return 0;
}

/* Walk every live block.  Costly, so it runs on a stride, not per allocation. */
static void can_sweep(void *caller) {
    unsigned n = 0, bad = 0;
    for (can_hdr *h = g_can_head; h; h = h->next) {
        n++;
        void *user = (void *)(h + 1);
        if (h->magic != can_key(user) || *can_ftr(h) != CAN_FTR_MAGIC) {
            bad++;
            can_report("SWEEP found a smashed block", h, user, caller);
            if (bad >= 4) break;          /* one report storm is enough */
        }
    }
    fprintf(stderr, "[canary] sweep: %u live blocks, %u smashed\n", n, bad);
    fflush(stderr);
}

static void *can_malloc(size_t n, void *caller) {
    if (n > 0xF0000000u) return NULL;
    size_t total = sizeof(can_hdr) + ALIGN_UP(n, 8) + CAN_FTR_SIZE;
    can_hdr *h = (can_hdr *)malloc(total);
    if (!h) return NULL;
    void *user = (void *)(h + 1);

    h->magic = can_key(user);
    h->size  = (uint32_t)n;
    h->lr    = (uint32_t)(uintptr_t)caller;

    lock();
    h->seq  = ++g_can_seq;
    h->prev = NULL;
    h->next = g_can_head;
    if (g_can_head) g_can_head->prev = h;
    g_can_head = h;
    unsigned sweep_now = (g_can_sweep && (g_can_seq % g_can_sweep) == 0);
    unlock();

    *can_ftr(h) = CAN_FTR_MAGIC;

    /* The instrument's own invariant: the footer must sit inside the block we
     * just asked glibc for.  If this ever trips the allocator is the bug. */
    if ((char *)can_ftr(h) + CAN_FTR_SIZE != (char *)h + total) {
        /* Never flip g_can off here: blocks already handed out carry our
         * header, and switching modes would send them to glibc's free() with
         * an interior pointer.  Report and keep going — ga_init's self-test is
         * what decides whether this mode is usable at all. */
        static int once;
        if (!once++) {
            fprintf(stderr, "[canary] INTERNAL: footer placement wrong for "
                            "n=%zu — reports from here are UNTRUSTWORTHY\n", n);
            fflush(stderr);
        }
    }
    if (sweep_now) can_sweep(caller);
    return user;
}

/* Classify a pointer that has no valid header of its own.  Walking the live
 * list is O(n) and only ever happens on the error path, and it is what turns
 * "invalid pointer" into a name: an INTERIOR pointer means somebody freed
 * base+k, which is exactly what produces glibc's "munmap_chunk(): invalid
 * pointer", and the block it points into names the owner. */
static void can_classify(void *user, void *caller) {
    can_hdr *found = NULL; size_t off = 0;
    lock();
    for (can_hdr *h = g_can_head; h; h = h->next) {
        char *b = (char *)(h + 1);
        if ((char *)user > b && (char *)user < b + h->size) {
            found = h; off = (size_t)((char *)user - b); break;
        }
    }
    unlock();
    if (found)
        fprintf(stderr,
                "[canary] *** FREE OF AN INTERIOR POINTER *** %p is %zu bytes "
                "into a LIVE %u-byte block at %p allocated by 0x%08x (seq=%u); "
                "freed by %p\n",
                user, off, found->size, (void *)(found + 1), found->lr,
                found->seq, caller);
    else
        fprintf(stderr,
                "[canary] free(%p) by %p: no header and not inside any live "
                "block — a foreign/host block, or a DOUBLE FREE (not "
                "forwarded to glibc; leaked instead)\n",
                user, caller);
    fflush(stderr);
}

/* 0 = ours and released; 1 = not ours (host block, free it normally). */
static int can_free(void *user, void *caller) {
    can_hdr *h = ((can_hdr *)user) - 1;
    if (h->magic != can_key(user)) {
        /* Either a block allocated before the game's imports were bound (ours
         * from C code), a smashed header, or an interior pointer.  Say which. */
        /* NOTE: the 0xDEADBEEF marker written at free time is NOT reliable —
         * glibc's tcache stores its next/key words in the freed chunk's user
         * area, which is exactly where our header sits.  So a double free
         * arrives here looking like any other unknown pointer.  Do not try to
         * distinguish it by reading freed memory. */
        if (!g_can_selftest) { g_can_reports++; can_classify(user, caller); }
        /* Deliberately DO NOT forward this to glibc.  If the pointer really is
         * bad, free() is precisely the call that aborts the process — and an
         * instrument that triggers the fault it exists to diagnose is worse
         * than none.  Leaking a few host blocks for the length of a debug run
         * is the cheap side of that trade. */
        return 0;
    }
    if (*can_ftr(h) != CAN_FTR_MAGIC)
        can_report("FOOTER SMASHED, caught at free (overflow past the end)",
                   h, user, caller);

    lock();
    if (h->prev) h->prev->next = h->next; else g_can_head = h->next;
    if (h->next) h->next->prev = h->prev;
    unlock();

    h->magic = 0xDEADBEEFu;               /* so a double free is recognisable */
    free(h);
    return 0;
}

/* ── public entry points ─────────────────────────────────────────────────── */

void ga_init(void) {
    g_page = (size_t)sysconf(_SC_PAGESIZE);
    {   /* GTALCS_GUARD=0 must mean off, not "the variable exists". */
        const char *e = getenv("GTALCS_GUARD");
        g_on = e && *e && strcmp(e, "0") != 0;
    }
    {   const char *e = getenv("GTALCS_CANARY");
        g_can = e && *e && strcmp(e, "0") != 0;
        e = getenv("GTALCS_CANARY_SWEEP");
        g_can_sweep = (e && *e) ? (unsigned)atoi(e) : 0;
        if (g_can && g_on) {
            fprintf(stderr, "[canary] GUARD and CANARY are mutually exclusive "
                            "— using GUARD\n");
            g_can = 0;
        }
        /* Self-test before trusting it with the game's heap: allocate, write
         * exactly n bytes, confirm both canaries survive, then confirm a
         * one-byte overflow is actually detected.  A silently broken allocator
         * produces confident wrong crash reports — this port has paid for that
         * once already. */
        if (g_can) {
            unsigned r0 = g_can_reports;
            g_can_selftest = 1;
            char *t = (char *)can_malloc(13, (void *)0);
            int bad = !t;
            if (t) {
                memset(t, 0xA5, 13);
                can_hdr *th = ((can_hdr *)t) - 1;
                if (th->magic != can_key(t) || *can_ftr(th) != CAN_FTR_MAGIC) bad = 1;
                *((char *)t + ALIGN_UP(13, 8)) = 1;          /* smash the footer */
                if (can_free(t, (void *)0) != 0) bad = 1;
                if (g_can_reports == r0) bad = 1;            /* must have reported */
            }
            g_can_selftest = 0;
            g_can_reports = r0;
            if (bad) {
                g_can = 0;
                fprintf(stderr, "[canary] SELF-TEST FAILED — canary mode refused "
                                "(header=%zuB); running with plain glibc malloc\n",
                        sizeof(can_hdr));
            } else {
                fprintf(stderr, "[canary] allocator ON — %zuB header + %uB footer "
                                "per block, sweep every %u allocs (0 = never); "
                                "self-test passed\n",
                        sizeof(can_hdr), CAN_FTR_SIZE, g_can_sweep);
            }
            fflush(stderr);
        }
    }
    if (g_on)
        fprintf(stderr, "[guard] allocator ON — page=%zu, overflow faults at "
                        "the write, UAF faults at the read\n", g_page);
}

/* ── allocation sanity reporting (active with the guard OFF too) ──────────
 *
 * World load fails nondeterministically with base::cMemoryManager::Allocate
 * calling memset(NULL, 0, n) — i.e. malloc returned NULL on a machine with
 * 1.5 GB free and only 386 MB of address space mapped.  That means the SIZE is
 * garbage, not the memory.  Naming the size and the caller turns "something is
 * corrupt" into a specific arithmetic bug.
 *
 * GTALCS_ALLOC_WARN sets the threshold in MB (default 64). */
static size_t ga_warn_bytes(void) {
    static size_t v;
    if (!v) {
        const char *e = getenv("GTALCS_ALLOC_WARN");
        size_t mb = (e && *e) ? (size_t)atoi(e) : 64;
        v = mb ? mb << 20 : (size_t)-1;
    }
    return v;
}

static void *ga_check(void *p, size_t n, const char *what, void *caller) {
    static int logged;
    if (logged >= 64) return p;
    if (n >= ga_warn_bytes()) {
        logged++;
        fprintf(stderr, "[alloc] HUGE %s(%zu = 0x%zx, %.1f MB) caller=%p -> %p\n",
                what, n, n, (double)n / 1048576.0, caller, p);
        fflush(stderr);
    } else if (!p && n) {
        logged++;
        fprintf(stderr, "[alloc] FAILED %s(%zu = 0x%zx) caller=%p\n",
                what, n, n, caller);
        fflush(stderr);
    }
    return p;
}

void *ga_malloc(size_t n) {
    void *caller = __builtin_return_address(0);
    void *p = g_on  ? core_malloc(n, caller)
            : g_can ? can_malloc(n, caller)
                    : malloc(n);
    return ga_check(p, n, "malloc", caller);
}

void ga_free(void *p) {
    if (!p) return;
    if (g_can) { if (can_free(p, __builtin_return_address(0))) free(p); return; }
    if (!g_on) { free(p); return; }
    int r = core_free(p, __builtin_return_address(0));
    if (r == 2) free(p);
    else if (r == 1)
        fprintf(stderr, "[guard] DOUBLE FREE %p caller=%p — ignored\n",
                p, __builtin_return_address(0));
}

void *ga_realloc(void *p, size_t n) {
    if (g_can) {
        void *caller = __builtin_return_address(0);
        if (!p) return ga_check(can_malloc(n, caller), n, "realloc", caller);
        if (n == 0) { if (can_free(p, caller)) free(p); return NULL; }
        can_hdr *h = ((can_hdr *)p) - 1;
        if (h->magic != can_key(p))          /* host block — leave it to glibc */
            return ga_check(realloc(p, n), n, "realloc", caller);
        can_check(h, p, caller, "realloc of a smashed block");
        size_t on = h->size;
        void *np = can_malloc(n, caller);
        if (!np) return NULL;
        memcpy(np, p, n < on ? n : on);
        if (can_free(p, caller)) free(p);
        return ga_check(np, n, "realloc", caller);
    }
    if (!g_on) return ga_check(realloc(p, n), n, "realloc",
                               __builtin_return_address(0));
    if (!p) return ga_malloc(n);
    if (n == 0) { ga_free(p); return NULL; }
    size_t on = 0;
    if (!ht_get((uintptr_t)p, &on)) return realloc(p, n);   /* host block */
    void *np = core_malloc(n, __builtin_return_address(0));
    if (!np) return NULL;
    memcpy(np, p, n < on ? n : on);
    ga_free(p);
    return np;
}

void *ga_calloc(size_t a, size_t b) {
    void  *caller = __builtin_return_address(0);
    size_t n = a * b;
    void *p = g_on  ? core_malloc(n, caller)
            : g_can ? can_malloc(n, caller)
                    : calloc(a, b);
    if (p && (g_on || g_can)) memset(p, 0, n);
    return ga_check(p, n, "calloc", caller);
}
