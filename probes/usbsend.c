/* SPDX-License-Identifier: GPL-3.0-only */
/* USBSEND: N note-ons (note 100 + k) and their note-offs through fm1-dock's own MIDI out (midi_send:
 * its queue, usb.c's ep1_tx from the 2 kHz poll), ms apart: tools/probe.py lists what arrives. The
 * baseline to probes/usbalign.c, which arms EP1 by itself. argument: [n, ms] */
#include "probe.h"

static int32_t run(hb_probe_ctx_t *c)
{
    uint32_t k, i, n = p_arg32(c, 0, 8), ms = p_arg32(c, 1, 30);
    for (k = 0; k < n && k < 16u; k++) {
        c->host->midi_send(0x90, 100u + k, 1);
        for (i = p_ticks(); p_ticks() - i < 24000u * ms;)
            c->wdt_feed();
        c->host->midi_send(0x80, 100u + k, 0);
    }
    return 0;
}

HB_PROBE = {HB_PROBE_MAGIC, "USBSEND", run};
