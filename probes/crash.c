/* SPDX-License-Identifier: GPL-3.0-only */
/* CRASH: the probe path's own safety, on an FM-1 (fm1-emu halts on a wild jump instead): writes a
 * record, then jumps nowhere (argument 0) or spins with the interrupts off until the watchdog
 * (argument 1). After the reset, tools/probe.py result shows the record and, for the jump, the
 * PC; fm1-dock boots as before, its boot guard untouched. */
#include "probe.h"

static int32_t run(hb_probe_ctx_t *c)
{
    p_text(c, "before", "still here");
    if (p_arg32(c, 0, 0) == 1u) {
        p_irq_off();
        for (;;) ;
    }
    ((void (*)(void))(void *)0x00000010u)();
    p_text(c, "after", "should not be here");
    return 0;
}

HB_PROBE = {HB_PROBE_MAGIC, "CRASH", run};
