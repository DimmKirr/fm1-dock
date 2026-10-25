/* SPDX-License-Identifier: GPL-3.0-only */
/* TINY: the smallest probe (its code ends well before 0x1A0): does code from RAM run at all, and
 * what the RAM holds where harbor put the probe (the first 0x200 bytes, read as data: compare with
 * the .hrbr's code; a crash at 0x01C301A0..BF in hello and timing on an FM-1, 2026-10-24). */
#include "probe.h"

static int32_t run(hb_probe_ctx_t *c)
{
    p_hex(c, "run_at", (uint32_t)&run);
    p_words(c, "ram_01c30000", (const uint32_t *)0x01C30000u, 128);
    return 7;
}

HB_PROBE = {HB_PROBE_MAGIC, "TINY", run};
