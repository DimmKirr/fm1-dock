/* SPDX-License-Identifier: GPL-3.0-only */
/* USBSIE: the SIE mailbox (USB CON1: a command written, bit 15 "done", the byte back in bits 0..7)
 * as fast -O2 code sees it. usb.c's ep1_tx sequence worked from a probe compiled -Os and failed
 * compiled -O2 (probes/usbarm.c, usbarmos.c, 2026-10-24); FM-1 SID's usb.c is -O2 -mcpu=r3. For 32
 * reads of FRAME1 (the frame counter: read-only, no endpoint touched), right after the command is
 * written: how many polls bit 15 still reads 1 (stale: the previous command's), then how many
 * until it is 1 again; the bytes read with the plain wait and with a wait for 0 first.
 * Interrupts off throughout (fm1-dock's USB poll uses the mailbox too). argument: none */
#include "probe.h"

#define UA_CON1 (*(volatile uint32_t *)0x11804u)
enum { S_FRAME1 = 12 };

static int32_t run(hb_probe_ctx_t *c)
{
    uint32_t i, n, stale[32], wait[32], plain[8], careful[8], first[4], cmd = ((S_FRAME1 << 8) & 0xFFBF00u) | 0x4000u;
    p_irq_off();
    for (i = 0; i < 32u; i++) {
        uint32_t s = 0, w = 0;
        UA_CON1 = cmd;
        __asm__ volatile("csync" ::: "memory");
        while ((UA_CON1 & 0x8000u) && s < 1000u) s++;          /* still the old done */
        while (!(UA_CON1 & 0x8000u) && w < 100000u) w++;       /* the new one */
        stale[i] = s; wait[i] = w;
    }
    for (i = 0; i < 4u; i++) {                 /* the raw register, right after a write */
        UA_CON1 = cmd;
        __asm__ volatile("csync" ::: "memory");
        first[i] = UA_CON1;
        for (n = 0; n < 100000u && !(UA_CON1 & 0x8000u); n++) ;
    }
    for (i = 0; i < 8u; i++) {                 /* the frame counter as usb.c reads it, back to back */
        UA_CON1 = cmd;
        __asm__ volatile("csync" ::: "memory");
        for (n = 0; n < 20000u && !(UA_CON1 & 0x8000u); n++) ;
        plain[i] = UA_CON1 & 0xFFu;
    }
    for (i = 0; i < 8u; i++) {
        UA_CON1 = cmd;
        __asm__ volatile("csync" ::: "memory");
        for (n = 0; n < 1000u && (UA_CON1 & 0x8000u); n++) ;
        for (n = 0; n < 20000u && !(UA_CON1 & 0x8000u); n++) ;
        careful[i] = UA_CON1 & 0xFFu;
    }
    p_irq_on();
    p_words(c, "stale_polls", stale, 32);
    p_words(c, "polls_to_done", wait, 32);
    p_words(c, "con1_right_after_write", first, 4);
    p_words(c, "frame1_plain", plain, 8);
    p_words(c, "frame1_wait_for_0_first", careful, 8);
    return 0;
}

HB_PROBE = {HB_PROBE_MAGIC, "USBSIE", run};
