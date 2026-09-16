#include <stdio.h>
#include <string.h>
#include "ctype_tbl.inc"
/* the engine's view: _ctype_ points at the table BASE and code reads ptr[c+1] */
#define F(c) ((unsigned char)android_ctype_table[(c)+1])
#define _U 0x01
#define _L 0x02
#define _N 0x04
#define _S 0x08
#define _P 0x10
#define _C 0x20
static int fails;
static void chk(const char *what, int got, int want) {
    if (!!got != !!want) { printf("  FAIL %-22s got=%d want=%d\n", what, !!got, !!want); fails++; }
}
int main(void) {
    printf("table entries = %zu (must be 257)\n", sizeof android_ctype_table);
    if (sizeof android_ctype_table != 257) return 1;
    chk("isspace(' ')",  F(' ')&_S, 1);
    chk("isspace('\\t')", F('\t')&_S, 1);
    chk("isspace('\\n')", F('\n')&_S, 1);
    chk("isspace('A')",  F('A')&_S, 0);
    chk("isspace(NUL)",  F(0)&_S,   0);
    chk("iscntrl(NUL)",  F(0)&_C,   1);
    chk("iscntrl(0x1F)", F(0x1F)&_C,1);
    chk("iscntrl(' ')",  F(' ')&_C, 0);
    chk("iscntrl(DEL)",  F(0x7F)&_C,1);
    chk("islower('a')",  F('a')&_L, 1);
    chk("islower('z')",  F('z')&_L, 1);
    chk("islower('A')",  F('A')&_L, 0);
    chk("isupper('A')",  F('A')&_U, 1);
    chk("isupper('Z')",  F('Z')&_U, 1);
    chk("isdigit('0')",  F('0')&_N, 1);
    chk("isdigit('9')",  F('9')&_N, 1);
    chk("isdigit('a')",  F('a')&_N, 0);
    chk("ispunct('.')",  F('.')&_P, 1);
    /* the fold the engine actually performs, for the whole alphabet */
    for (int c='a'; c<='z'; c++)
        if (!(F(c)&_L)) { printf("  FAIL '%c' does not fold\n", c); fails++; }
    for (int c='A'; c<='Z'; c++)
        if (F(c)&_L) { printf("  FAIL '%c' wrongly folds\n", c); fails++; }
    printf(fails ? "\n%d FAILURES\n" : "\nALL CTYPE CHECKS PASSED\n", fails);
    return fails != 0;
}
