/* SPDX-License-Identifier: GPL-3.0-only */
/* IDLE: the CPU's idle instruction (sleep to the next interrupt) and USB. N MIDI notes go out over
 * USB, ~10 ms apart; between them a busy wait (mode 0) or idle, again and again, until the time is
 * up (mode 1). tools/probe.py counts the notes that arrive (midi_in). FM-1 SID's builds that slept
 * in their main loop never answered an installer on a real FM-1 (2026-10-24). Also how long an
 * idle sleeps (TIMER5 ticks at 10 kHz: ~100 us at most). argument: [mode, notes] */
#include "probe.h"

static int32_t run(hb_probe_ctx_t *c)
{
    uint32_t mode = p_arg32(c, 0, 1), n = p_arg32(c, 1, 40), i, t0, idles = 0, worst = 0;
    uint32_t t_all = p_ticks(), ms0 = c->host->ms();
    for (i = 0; i < n; i++) {
        c->host->midi_send(0x90, 36u + i % 48u, 64);
        t0 = p_ticks();
        while (p_ticks() - t0 < 10u * 1000u * c->ticks_per_us) {
            if (mode) {
                uint32_t a = p_ticks();
                __asm__ volatile("idle" ::: "memory");
                a = p_ticks() - a;
                if (a > worst) worst = a;
                idles++;
            }
        }
        c->host->midi_send(0x80, 36u + i % 48u, 0);
        c->wdt_feed();
    }
    p_u32(c, "mode", mode);
    p_u32(c, "notes_sent", n);
    p_u32(c, "idles", idles);
    p_u32(c, "idle_worst_ticks", worst);
    p_u32(c, "ticks", p_ticks() - t_all);
    p_u32(c, "host_ms", c->host->ms() - ms0);
    return (int32_t)n;
}

HB_PROBE = {HB_PROBE_MAGIC, "IDLE", run};
