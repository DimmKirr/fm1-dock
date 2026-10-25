/* SPDX-License-Identifier: GPL-3.0-only */
/* LCD: what the screen's transfers take on the FM-1 and in fm1-emu: host->blit of 240 x n rows (it
 * returns when the panel has them), the best of 4, in TIMER4 ticks, and the SPI clock that gives
 * (kHz). fm1-emu models SPI1 as 60 MHz / (BAUD + 1): 12 MHz at Dinghy's BAUD 4, a whole frame ~77 ms.
 * argument: none */
#include "probe.h"

static uint16_t lc_px[240 * 16];

static void lc_one(hb_probe_ctx_t *c, uint32_t rows, const char *kt, const char *kk)
{
    uint32_t best = 0xFFFFFFFFu, i, t, bits = 240u * rows * 16u;
    for (i = 0; i < 4u; i++) {
        t = p_ticks();
        c->host->blit(0, 112, 240, (int32_t)rows, lc_px);
        t = p_ticks() - t;
        if (t < best) best = t;
    }
    p_u32(c, kt, best);
    p_u32(c, kk, best ? bits * 24000u / best : 0);
}

static int32_t run(hb_probe_ctx_t *c)
{
    uint32_t i;
    for (i = 0; i < 240u * 16u; i++)
        lc_px[i] = (uint16_t)(i & 32u ? 0x1F00u : 0x00F8u);   /* (bars: a test pattern, high byte first) */
    lc_one(c, 1, "blit_240x1_ticks", "spi_khz_240x1");
    lc_one(c, 4, "blit_240x4_ticks", "spi_khz_240x4");
    lc_one(c, 16, "blit_240x16_ticks", "spi_khz_240x16");
    return 0;
}

HB_PROBE = {HB_PROBE_MAGIC, "LCD", run};
