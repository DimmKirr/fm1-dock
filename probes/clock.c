/* SPDX-License-Identifier: GPL-3.0-only */
/* CLOCK: TIMER4 against the USB host's start-of-frame (1 kHz from the computer's crystal): the
 * frame number (SIE FRAME1 / FRAME2, 11 bits) at two frame edges ~1.5 s apart and the ticks between
 * them give TIMER4's real rate (24 MHz assumed everywhere: FM1_TICKS_PER_US). And the host's
 * milliseconds over the same frames. Needs USB connected. argument: [ms] (default 1500, < 2000) */
#include "probe.h"

#define CK_CON0 (*(volatile uint32_t *)0x11800u)
#define CK_CON1 (*(volatile uint32_t *)0x11804u)

static uint32_t ck_sie_rd(uint32_t r)         /* (interrupts off: the host's USB poll uses CON1 too) */
{
    uint32_t n = 2000;
    if (!(CK_CON0 & 4u))
        return 0xFFFFu;
    CK_CON1 = ((r << 8) & 0xFFBF00u) | 0x4000u;
    __asm__ volatile("csync" ::: "memory");
    while (!(CK_CON1 & 0x8000u))
        if (!--n)
            return 0xFFFFu;
    return CK_CON1 & 0xFFu;
}
static uint32_t ck_frame(void) { return ck_sie_rd(12) | (ck_sie_rd(13) & 7u) << 8; }

/* spin (interrupts off, < 1 ms) to the next frame's edge: the frame number and the tick it came */
static uint32_t ck_edge(uint32_t *tick)
{
    uint32_t f0, f, t, n = 0;
    p_irq_off();
    f0 = ck_frame();
    if (f0 > 2047u) {                           /* (no answer: no USB, or its SIE not up) */
        p_irq_on();
        *tick = p_ticks();
        return f0;
    }
    do {
        f = ck_frame();
        t = p_ticks();
    } while (f == f0 && ++n < 100000u);
    p_irq_on();
    *tick = t;
    return f;
}

static int32_t run(hb_probe_ctx_t *c)
{
    uint32_t wait = p_arg32(c, 0, 1500), f0, f1, t0, t1, m0, m1, frames;
    if (wait > 1900u) wait = 1900u;
    f0 = ck_edge(&t0);
    m0 = c->host->ms();
    while (c->host->ms() - m0 < wait)
        c->wdt_feed();
    f1 = ck_edge(&t1);
    m1 = c->host->ms();
    if (f0 > 2047u || f1 > 2047u) {
        p_text(c, "error", "the USB frame counter did not answer (no USB host?)");
        return -1;
    }
    frames = (f1 - f0) & 2047u;
    p_u32(c, "frame0", f0);
    p_u32(c, "frame1", f1);
    p_u32(c, "frames", frames);
    p_u32(c, "ticks", t1 - t0);
    p_u32(c, "host_ms", m1 - m0);
    p_u32(c, "t4_hz", frames ? (uint32_t)((uint64_t)(t1 - t0) * 1000u / frames) : 0);
    return 0;
}

HB_PROBE = {HB_PROBE_MAGIC, "CLOCK", run};
