#include "/home/mafradon/gtalcs-port/src/guard_alloc.c"
#include <assert.h>
int main(void) {
    setenv("GTALCS_CANARY", "1", 1);
    setenv("GTALCS_CANARY_SWEEP", "4", 1);
    ga_init();
    if (!g_can) { fprintf(stderr, "FAIL: canary did not enable\n"); return 1; }

    /* 1. ordinary round trip over a range of sizes, incl. non-multiples of 8 */
    for (size_t n = 1; n <= 300; n += 7) {
        char *p = ga_malloc(n);
        assert(p);
        memset(p, 0xAB, n);                       /* exactly n bytes is legal */
        assert(((uintptr_t)p % 8) == 0);
        ga_free(p);
    }
    printf("ok: round trip, alignment, exact-size writes\n");

    /* 2. realloc preserves contents and stays canaried */
    char *q = ga_malloc(16); memset(q, 'A', 16);
    q = ga_realloc(q, 64);
    for (int i = 0; i < 16; i++) assert(q[i] == 'A');
    ga_free(q);
    printf("ok: realloc preserves data\n");

    /* 3. calloc zeroes */
    char *z = ga_calloc(10, 10);
    for (int i = 0; i < 100; i++) assert(z[i] == 0);
    ga_free(z);
    printf("ok: calloc zeroes\n");

    /* 4. THE POINT: a one-byte overflow must be reported at free */
    unsigned before = g_can_reports;
    char *v = ga_malloc(24);
    v[24] = 0x7F;              /* ALIGN_UP(24,8)==24, so this IS the footer */
    ga_free(v);
    if (g_can_reports <= before) { fprintf(stderr, "FAIL: overflow not caught\n"); return 1; }
    printf("ok: footer smash detected at free\n");

    /* 5. the sweep must catch it while the block is still live */
    before = g_can_reports;
    char *w = ga_malloc(24);
    w[24] = 0x7F;
    for (int i = 0; i < 8; i++) { void *t = ga_malloc(8); ga_free(t); }
    if (g_can_reports <= before) { fprintf(stderr, "FAIL: sweep missed it\n"); return 1; }
    printf("ok: sweep detects a smashed live block\n");

    /* 6. a foreign (glibc) pointer must be recognised, not corrupted */
    before = g_can_reports;
    char *host = malloc(32);
    ga_free(host); free(host); /* reported + leaked by design; test frees it */
    if (g_can_reports <= before) { fprintf(stderr, "FAIL: host block not flagged\n"); return 1; }
    printf("ok: foreign pointer handled\n");

    /* 7. interior pointer must be identified, not silently passed to glibc */
    before = g_can_reports;
    char *big = ga_malloc(128);
    ga_free(big + 16);
    if (g_can_reports <= before) { fprintf(stderr, "FAIL: interior ptr not caught\n"); return 1; }
    ga_free(big);
    printf("ok: interior-pointer free identified\n");

    /* 8. double free must be caught and NOT forwarded to glibc */
    before = g_can_reports;
    char *d = ga_malloc(32);
    ga_free(d);
    ga_free(d);
    if (g_can_reports <= before) { fprintf(stderr, "FAIL: double free not caught\n"); return 1; }
    printf("ok: double free caught\n");

    printf("\nALL CANARY SELF-TESTS PASSED\n");
    return 0;
}
