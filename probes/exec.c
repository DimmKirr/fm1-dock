/* SPDX-License-Identifier: GPL-3.0-only */
/* EXEC: runs instruction bytes from the argument, with r0..r15 set, and returns r0..r15 after.
 * tools/probe.py exec "r3 = r1 * r2" --regs r1=5,r2=7 assembles the lines inside a wrapper that
 * loads the registers from a table and stores them back (pi32v2 has no register list with r0, so
 * the wrapper keeps the table's address on the stack), runs it here and on the other target.
 *   argument: [r0..r15 in, 16 x u32][the wrapped code]
 *   output:   regs (r0..r15 out), ticks (TIMER4 for the call, wrapper included, interrupts off) */
#include "probe.h"

static uint32_t code[(HB_PROBE_ARG - 64u) / 4u + 1u];
static uint32_t regs[16];

static int32_t run(hb_probe_ctx_t *c)
{
    uint32_t i, n = c->arg_n > 64u ? c->arg_n - 64u : 0, t0, t1;
    void (*volatile f)(uint32_t *) = (void (*)(uint32_t *))(void *)code;   /* (volatile: a call through a register, not a direct one) */
    if (!n)
        return -1;
    for (i = 0; i < 16u; i++) regs[i] = p_arg32(c, i, 0);
    for (i = 0; i < n; i++) ((uint8_t *)code)[i] = c->arg[64u + i];
    p_csync();
    p_irq_off();
    t0 = p_ticks();
    f(regs);
    t1 = p_ticks();
    p_irq_on();
    p_words(c, "regs", regs, 16);
    p_u32(c, "ticks", t1 - t0);
    return 0;
}

HB_PROBE = {HB_PROBE_MAGIC, "EXEC", run};
