/* SPDX-License-Identifier: GPL-3.0-only */
/* RADIOCAL: the radio's factory trims on this unit, read only (PLAN.md, "BLE-MIDI", step 4). Stock's
 * SDK VM keeps them at flash 0xE8000 (Optimist's docs/BLE-HW-FACTS.md §14); 0xE7000 is a candidate
 * second area, 0xE9000 BTIF (the pairing area). For each of the three sectors: whether it starts with
 * the VM's magic 55 AA AA 55, how many bytes are not erased, its CRC-32 and first 16 bytes; then the
 * whole sector the argument names (0, 1 or 2; default 1, 0xE8000), for a backup:
 *   tools/probe.py run probes/radiocal.c --on fm1 --arg 01000000
 * Reads through XIP: fm1-dock's plain window shows the flash from 0x93000 up as stored. */
#include "probe.h"

#define SECT(i) ((const uint8_t *)(0x02000000u + 0xE7000u - 0x4000u + 0x1000u * (i)))

static uint32_t crc32(const uint8_t *p, uint32_t n)
{
    uint32_t c = ~0u, k;
    while (n--) {
        c ^= *p++;
        for (k = 0; k < 8u; k++)
            c = c & 1u ? (c >> 1) ^ 0xEDB88320u : c >> 1;
    }
    return ~c;
}

static int32_t run(hb_probe_ctx_t *c)
{
    static const char *const at[3] = {"e7000", "e8000", "e9000"};
    char key[16];
    uint32_t i, j, used, pick = p_arg32(c, 0, 1);
    for (i = 0; i < 3u; i++) {
        const uint8_t *s = SECT(i);
        for (j = used = 0; j < 0x1000u; j++)
            used += s[j] != 0xFFu;
        for (j = 0; j < 5u; j++) key[j] = at[i][j];
        key[5] = '_';
        key[6] = 'v'; key[7] = 'm'; key[8] = 0;
        p_u32(c, key, s[0] == 0x55u && s[1] == 0xAAu && s[2] == 0xAAu && s[3] == 0x55u);
        key[6] = 'u'; key[7] = 's'; key[8] = 'e'; key[9] = 'd'; key[10] = 0;
        p_u32(c, key, used);
        key[6] = 'c'; key[7] = 'r'; key[8] = 'c'; key[9] = 0;
        p_hex(c, key, crc32(s, 0x1000u));
        key[6] = 'h'; key[7] = 'e'; key[8] = 'a'; key[9] = 'd'; key[10] = 0;
        p_bytes(c, key, s, 16u);
    }
    if (pick < 3u) {
        p_hex(c, "sector", 0xE7000u + 0x1000u * pick);
        p_bytes(c, "data", SECT(pick), 0x1000u);
    }
    return 0;
}

HB_PROBE = {HB_PROBE_MAGIC, "RADIOCAL", run};
