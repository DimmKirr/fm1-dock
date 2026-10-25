/* SPDX-License-Identifier: GPL-3.0-only */
/* ALIGN: unaligned loads and stores on the FM-1 and in fm1-emu: what each one reads, or that it
 * faults (the record "try" before each case says which one ran last). RAM, then XIP (harbor's
 * own code). argument: [first case] (0..9) */
#include "probe.h"

static uint32_t al_buf[8] = {0x03020100u, 0x07060504u, 0x0B0A0908u, 0x0F0E0D0Cu,
                             0x13121110u, 0x17161514u, 0x1B1A1918u, 0x1F1E1D1Cu};

static int32_t run(hb_probe_ctx_t *c)
{
    volatile uint8_t *b = (volatile uint8_t *)al_buf;
    const volatile uint8_t *x = (const volatile uint8_t *)0x02000200u;
    uint32_t k, v;
    p_bytes(c, "xip_bytes", (const void *)x, 8);
    for (k = p_arg32(c, 0, 0); k < 10u; k++) {
        p_u32(c, "try", k);
        switch (k) {
        case 0: v = *(volatile uint32_t *)(b + 4); p_hex(c, "ld32_aligned", v); break;
        case 1: v = *(volatile uint32_t *)(b + 5); p_hex(c, "ld32_plus1", v); break;
        case 2: v = *(volatile uint32_t *)(b + 6); p_hex(c, "ld32_plus2", v); break;
        case 3: v = *(volatile uint32_t *)(b + 7); p_hex(c, "ld32_plus3", v); break;
        case 4: v = *(volatile uint16_t *)(b + 5); p_hex(c, "ld16_plus1", v); break;
        case 5: *(volatile uint32_t *)(b + 9) = 0xA1B2C3D4u; p_words(c, "st32_plus1", al_buf, 4); break;
        case 6: *(volatile uint16_t *)(b + 13) = 0xE5F6u; p_words(c, "st16_plus1", al_buf, 4); break;
        case 7: v = *(const volatile uint32_t *)(x + 1); p_hex(c, "xip_ld32_plus1", v); break;
        case 8: v = *(const volatile uint32_t *)(x + 2); p_hex(c, "xip_ld32_plus2", v); break;
        default: v = *(const volatile uint16_t *)(x + 1); p_hex(c, "xip_ld16_plus1", v); break;
        }
    }
    return 0;
}

HB_PROBE = {HB_PROBE_MAGIC, "ALIGN", run};
