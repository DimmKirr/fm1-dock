/* SPDX-License-Identifier: GPL-3.0-only */
/* CYCLES2: the classes CYCLES left out: the FPU (add, multiply, divide, int <-> float, dependent
 * chains), the 64-bit multiply-accumulate (a chain), a loop's backward branch (taken every round but
 * the last) against a forward taken branch. 8 to a round, interrupts off, the empty loop taken off.
 * argument: [rounds] */
/* hbmod-cflags: -mcpu=r3 -mfprev1 */
#include "probe.h"

typedef long long i64;
#define X8(s) s; s; s; s; s; s; s; s
static volatile float c2_fa = 1.0001f, c2_fb = 0.9999f;
static volatile int c2_ia = 123, c2_ib = 457;

static int32_t run(hb_probe_ctx_t *c)
{
    uint32_t n = p_arg32(c, 0, 2000), t, k, w[2];
    float a = c2_fa, b = c2_fb, d = 0.0f;
    int ia = c2_ia, ib = c2_ib, id = 0;
    i64 acc = 1;
    P_TIME(c, "fadd_chain_x8", n, X8(__asm__ volatile("" : "+r"(b)); a = a + b));
    P_TIME(c, "fmul_chain_x8", n, X8(__asm__ volatile("" : "+r"(b)); a = a * b));
    P_TIME(c, "fmac_chain_x8", n, X8(__asm__ volatile("" : "+r"(b)); a = a * b + d));
    P_TIME(c, "fdiv_chain_x8", n / 8u + 32u, X8(__asm__ volatile("" : "+r"(b)); a = a / b));
    P_TIME(c, "itof_ftoi_x8", n, X8(__asm__ volatile("" : "+r"(ia)); id += (int)((float)ia * b)));
    P_TIME(c, "mac64_chain_x8", n, X8(__asm__ volatile("" : "+r"(ia)); acc += (i64)ia * ib));
    /* a loop of 1 add: 16 rounds a call; the backward branch is taken 15 times and falls through once */
    p_irq_off();
    t = p_ticks();
    for (k = 0; k < n; k++) {
        uint32_t r = 16;
        __asm__ volatile("1:\n\t%0 += 1\n\tif (--%1 != 0) goto 1b" : "+r"(id), "+r"(r));
    }
    t = p_ticks() - t;
    p_irq_on();
    w[0] = t; w[1] = n * 16u;
    p_rec(c, P_TIME_T, "loop_add_backward_per_round", w, 8);
    c2_fa = a + d; c2_ia = ia + id + (int)acc;
    return 0;
}

HB_PROBE = {HB_PROBE_MAGIC, "CYCLES2", run};
