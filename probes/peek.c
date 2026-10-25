/* SPDX-License-Identifier: GPL-3.0-only */
/* PEEK: n words from an address (RAM, registers, flash): e.g. the crash record a fault left in NOINIT
 * (fm1_irq.h's fm1_crash_t: magic, count, vec, pc, rets, emu, dbg, sp, psr, icfg, uptime, etm[4], early).
 * argument: [address, words] (words <= 256) */
#include "probe.h"

static int32_t run(hb_probe_ctx_t *c)
{
    uint32_t a = p_arg32(c, 0, 0x01C7C000u) & ~3u, n = p_arg32(c, 1, 16);
    if (n > 256u) n = 256u;
    p_hex(c, "at", a);
    p_words(c, "words", (const uint32_t *)a, n);
    return (int32_t)n;
}

HB_PROBE = {HB_PROBE_MAGIC, "PEEK", run};
