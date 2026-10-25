/* SPDX-License-Identifier: GPL-3.0-only */
/* CACHE: the flash (XIP) cache's size and line size, for fm1-emu's model. Word loads from XIP over a
 * footprint, again and again: per load, in TIMER4 ticks (x 1000 / 24 = ns), by footprint (32-byte
 * stride: where it stops fitting the cache the cost jumps) and by stride (a 256 KB footprint: where
 * every load is a new line). Interrupts off while timing. argument: none */
#include "probe.h"

#define CA_BASE 0x02010000u                   /* XIP: harbor's code and after it (flash is 1 MB) */

static uint32_t ca_time(uint32_t foot, uint32_t stride, uint32_t loads)
{
    const volatile uint32_t *x = (const volatile uint32_t *)CA_BASE;
    uint32_t i, o = 0, s = 0, t;
    for (i = 0; i < foot / stride; i++)        /* (warm: what fits is in) */
        s += x[(i * stride) / 4u];
    p_irq_off();
    t = p_ticks();
    for (i = 0; i < loads; i++) {
        s += x[o / 4u];
        o += stride;
        if (o >= foot) o = 0;
    }
    t = p_ticks() - t;
    p_irq_on();
    __asm__ volatile("" :: "r"(s));
    return t * 1000u / loads;                  /* (ticks x 1000 a load) */
}

static int32_t run(hb_probe_ctx_t *c)
{
    static const uint32_t foot[] = {1024, 2048, 4096, 8192, 16384, 32768, 65536, 131072, 262144};
    static const uint32_t stride[] = {4, 8, 16, 32, 64, 128, 256};
    uint32_t r[9], i;
    for (i = 0; i < 9u; i++) {
        r[i] = ca_time(foot[i], 32, 4096);
        c->wdt_feed();
    }
    p_words(c, "mticks_by_foot_1k_to_256k_s32", r, 9);
    for (i = 0; i < 7u; i++) {
        r[i] = ca_time(262144, stride[i], 4096);
        c->wdt_feed();
    }
    p_words(c, "mticks_by_stride_4_to_256_f256k", r, 7);
    return 0;
}

HB_PROBE = {HB_PROBE_MAGIC, "CACHE", run};
