/* setjmp_fix.c — bionic-layout setjmp/longjmp for libGTALcs.so
 *
 * WHY THIS EXISTS (the same trap as clock_fix.c, one struct further along)
 *
 * libGTALcs.so links libpng and zlib statically.  Both embed a jmp_buf INSIDE
 * their own structs (png_struct, png_struct_def::jmpbuf_buffer; and zlib's
 * inflate/deflate state when DEBUG unwind is used), sized for **bionic's**
 * jmp_buf as compiled against the 2016 NDK.  The calls themselves are imports,
 * so they land in glibc's setjmp — whose ARM32 jmp_buf layout is different and
 * larger (sigcount + fp state + padding).  glibc's setjmp writes past the
 * space libpng allocated, corrupting the png_struct fields that follow the
 * jmpbuf.  The symptom lands far away: a chunk handler returning through a
 * clobbered slot → PC=0 with LR parked at libGTALcs.so+0x5c51b0, immediately
 * after a benign "iCCP: known incorrect sRGB profile" warning.
 *
 * Fix: implement both functions here with a minimal, self-consistent layout:
 *
 *      [0  .. 36]  r4-r11, r13(sp), r14(lr)      (10 words)
 *      [40 .. 104] d8-d15                       (16 words, VFP callee-saved)
 *      = 104 bytes, <= bionic's jmp_buf for hard-float ABI armeabi-v7a.
 *
 * Nothing outside this file ever reads these buffers — every user (libpng,
 * zlib, the engine) reaches them through these same two functions — so
 * self-consistency is all that matters, plus fitting the allocation.  Signal
 * masks are deliberately not saved: bionic's setjmp() doesn't either (that is
 * sigsetjmp's job, and nothing here imports it).
 *
 * my_setjmp is NAKED ASM ON PURPOSE.  A C wrapper around an asm helper saves
 * the WRAPPER's sp/lr, so a longjmp lands inside the wrapper and its epilogue
 * pops callee-saved registers off a long-dead frame.  See the comment on the
 * asm block; this cost a full debugging cycle and looked like a bug on
 * libpng's success path.
 *
 * Logging is capped (the engine setjmps in loops) and happens only after the
 * buffer is committed.
 */

#include <stdio.h>
#include <stdint.h>

__asm__(
    "       .text\n"
    "       .syntax unified\n"
    "       .arm\n"
    /* my_setjmp MUST be naked.
     *
     * It was a C wrapper around an asm helper, and that quietly broke the
     * whole mechanism: the helper stored ITS OWN caller's sp and lr — i.e.
     * the wrapper's frame — so a later longjmp returned into the middle of
     * my_setjmp, whose epilogue then popped r4-r8/fp/pc off a stack frame
     * that had been dead for thousands of calls.  The restored registers were
     * whatever libpng had since left on that stack.  The failure landed in
     * prepareForUploadPNG+0xc0 as `ldr r1,[r4,#24]` with r4=0x3b40 — on the
     * SUCCESS path out of png_image_begin_read_from_memory, which is what
     * makes it so confusing: the crash is in code that never saw an error.
     *
     * Writing the buffer before any call means sp and lr are still the real
     * caller's, so the logging call below is harmless — by then the jmp_buf
     * is already correct and the extra frame is transient. */
    "       .globl my_setjmp\n"
    "       .type  my_setjmp,%function\n"
    "my_setjmp:\n"
    "       stmia  r0, {r4-r11}\n"          /* 0..31   callee-saved core     */
    "       str    sp, [r0, #32]\n"         /* 32      caller's sp           */
    "       str    lr, [r0, #36]\n"         /* 36      caller's return addr  */
    "       add    r2, r0, #40\n"
    "       vstmia r2, {d8-d15}\n"          /* 40..103 callee-saved VFP      */
    "       push   {r0, lr}\n"
    "       bl     setjmp_log\n"
    "       pop    {r0, lr}\n"
    "       mov    r0, #0\n"
    "       bx     lr\n"
    "       .size my_setjmp, .-my_setjmp\n"
    /* longjmp may stay a C wrapper: it never returns, and _longjmp_asm
     * reinstates sp/lr from the buffer, so its own frame is irrelevant. */
    "       .globl _longjmp_asm\n"
    "       .type  _longjmp_asm,%function\n"
    "_longjmp_asm:\n"
    "       mov   r3, r1\n"                  /* return value, survives restores */
    "       add   r2, r0, #40\n"
    "       vldmia r2, {d8-d15}\n"
    "       ldr   lr, [r0, #36]\n"           /* setjmp's caller's return address */
    "       ldr   r2, [r0, #32]\n"           /* saved sp */
    "       ldmia r0, {r4-r11}\n"
    "       mov   r0, r3\n"
    "       mov   sp, r2\n"
    "       bx    lr\n"
    "       .size _longjmp_asm, .-_longjmp_asm\n"
);

extern int  my_setjmp(int *buf);
extern void _longjmp_asm(int *buf, int val);

#define LOG_LIMIT 24
static int s_setjmp_calls, s_longjmp_calls;

/* Called from my_setjmp AFTER the buffer is written; must not touch it. */
void setjmp_log(int *buf) {
    if (s_setjmp_calls < LOG_LIMIT)
        fprintf(stderr, "[setjmp] #%d buf=%p sp=%p lr=%p\n",
                ++s_setjmp_calls, buf, (void *)(uintptr_t)buf[8],
                (void *)(uintptr_t)buf[9]);
}

void my_longjmp(int *buf, int val) {
    if (val == 0) val = 1;   /* C standard: setjmp returns 1 for longjmp(buf,0) */
    if (s_longjmp_calls < LOG_LIMIT)
        fprintf(stderr, "[longjmp] #%d buf=%p val=%d from %p\n",
                ++s_longjmp_calls, buf, val, __builtin_return_address(0));
    _longjmp_asm(buf, val);
    __builtin_unreachable();
}
