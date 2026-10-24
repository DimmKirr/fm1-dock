/* SPDX-License-Identifier: GPL-3.0-only */
/* HELLO: the probe path itself (tools/probe.py, tests/probe_test.py): the argument back, a value
 * of each kind, .data and .bss where the host put them, a pointer the host relocated, and how
 * many TIMER4 ticks one of the host's milliseconds has. */
#include "probe.h"

static uint32_t counter = 7;                    /* .data */
static volatile uint32_t zeros[16];             /* .bss */
static const char *const greet = "hello from RAM";   /* a relocated pointer to .rodata */

static int32_t run(hb_probe_ctx_t *c)
{
    uint32_t i, z = 0, ms0, t0, t1;
    p_bytes(c, "arg", c->arg, c->arg_n);
    p_text(c, "greet", greet);
    for (i = 0; i < 16u; i++) z |= zeros[i];
    p_u32(c, "data", counter++);
    p_u32(c, "bss_zero", z == 0);
    p_hex(c, "run_at", (uint32_t)&run);
    p_hex(c, "data_at", (uint32_t)&counter);
    p_i32(c, "minus", -5);
    ms0 = c->host->ms();
    while (c->host->ms() == ms0) ;
    t0 = p_ticks();
    while (c->host->ms() - ms0 < 11u) ;
    t1 = p_ticks();
    p_u32(c, "ticks_per_10ms", t1 - t0);
    c->host->log("hello: done");
    return 42;
}

HB_PROBE = {HB_PROBE_MAGIC, "HELLO", run};
