/* SPDX-License-Identifier: GPL-3.0-only */
/* USBTXOFF: does the FM-1's USB transmit DMA need an 8-aligned buffer? fm1-dock's own MIDI out (the path
 * that delivers every note) sends from ep1tx + an offset for a moment (usb.c's hb_usb_tx_off, back
 * to 0 by itself after 200 ms). Per offset (0, 8, 4, 12): ep1tx's first 16 bytes filled with decoy
 * note-ons (60 + slot), then note 100 + offset sent through midi_send. What tools/probe.py lists:
 * 100 + offset = sent from the address as given; a decoy = read from somewhere else (60 + offset/4
 * rounded down to 8 = 60 or 62); nothing = the packet lost. All notes off at the end.
 * argument: [&ep1tx, &hb_usb_tx_off, &hb_usb_tx_left, &hb_usb_tx_reverts] (build/fm1-dock.syms) */
#include "probe.h"

static void wait_ms(hb_probe_ctx_t *c, uint32_t ms)
{
    uint32_t t;
    for (t = p_ticks(); p_ticks() - t < 24000u * ms;)
        c->wdt_feed();
}

static int32_t run(hb_probe_ctx_t *c)
{
    static const uint32_t offs[4] = {0, 8, 4, 12};
    uint8_t *b = (uint8_t *)(uintptr_t)p_arg32(c, 0, 0);
    volatile uint32_t *off = (volatile uint32_t *)(uintptr_t)p_arg32(c, 1, 0);
    volatile uint32_t *left = (volatile uint32_t *)(uintptr_t)p_arg32(c, 2, 0);
    volatile uint32_t *reverts = (volatile uint32_t *)(uintptr_t)p_arg32(c, 3, 0);
    uint32_t k, i, rv0, left_seen[4];
    if (((uint32_t)(uintptr_t)b & 63u) || (uint32_t)(uintptr_t)b - 0x01C00000u > 0x10000u ||
        ((uint32_t)(uintptr_t)off & 3u) || (uint32_t)(uintptr_t)off - 0x01C00000u > 0x80000u) {
        p_text(c, "error", "the addresses: ep1tx 64-aligned in low RAM, the switch in RAM");
        return -1;
    }
    rv0 = *reverts;
    wait_ms(c, 100);                           /* (fm1-dock's ack of RUN out of the way) */
    for (k = 0; k < 4u; k++) {
        p_irq_off();
        for (i = 0; i < 16u; i++) {
            b[4u * i] = 0x09; b[4u * i + 1u] = 0x90; b[4u * i + 2u] = (uint8_t)(60u + i); b[4u * i + 3u] = 1;
        }
        *left = 400;                           /* 200 ms of 2 kHz polls */
        *off = offs[k];
        p_irq_on();
        c->host->midi_send(0x90, 100u + offs[k], 1);
        wait_ms(c, 50);
        left_seen[k] = *left;                  /* (counting down: the switch is in use) */
        wait_ms(c, 250);                       /* reverted (and a stuck packet flushed) */
    }
    *off = 0;
    p_words(c, "offsets", offs, 4);
    p_words(c, "polls_left_50ms_on", left_seen, 4);
    rv0 = *reverts - rv0;
    p_words(c, "reverts", &rv0, 1);
    for (k = 0; k < 16u; k++) {
        c->host->midi_send(0x80, 60u + k, 0);
        c->host->midi_send(0x80, 100u + k, 0);
    }
    wait_ms(c, 50);
    return 0;
}

HB_PROBE = {HB_PROBE_MAGIC, "USBTXOFF", run};
