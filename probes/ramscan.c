/* SPDX-License-Identifier: GPL-3.0-only */
/* RAMSCAN: is there RAM at a base, how much, is it a mirror, how fast. Each 4 KB page: a word written
 * and read back (the first page that does not keep it ends the RAM); a progress record every 64 KB (a
 * bus fault resets harbor: the last record says where); then each page a signature, read back (a page
 * showing an earlier page's signature: a mirror); load and store timing against main RAM. Only RAM is
 * written, never flash. argument: [base, max KB] (default 0x04000000, 1024) */
#include "probe.h"

static uint32_t rs_main[1024];

static uint32_t rs_time(volatile uint32_t *p, int store)
{
    uint32_t t, i, s = 0;
    p_irq_off();
    t = p_ticks();
    if (store) for (i = 0; i < 1024u; i++) p[i] = i;
    else for (i = 0; i < 1024u; i++) s += p[i];
    t = p_ticks() - t;
    p_irq_on();
    __asm__ volatile("" :: "r"(s));
    return t * 1000u / 1024u;                  /* (ticks x 1000 an access) */
}

static int32_t run(hb_probe_ctx_t *c)
{
    uint32_t base = p_arg32(c, 0, 0x04000000u), max = p_arg32(c, 1, 1024) * 1024u, a, pages, k, mirror = 0, w[4];
    volatile uint32_t *p;
    p_hex(c, "base", base);
    p_words(c, "first_words_before", (const uint32_t *)base, 4);
    for (a = 0; a < max; a += 4096u) {
        p = (volatile uint32_t *)(base + a);
        if (!(a & 0xFFFFu)) p_hex(c, "reached", base + a);
        p[0] = 0x5A5A0000u | (a >> 12);
        p[1] = ~(0x5A5A0000u | (a >> 12));
        if (p[0] != (0x5A5A0000u | (a >> 12)) || p[1] != ~(0x5A5A0000u | (a >> 12)))
            break;
        c->wdt_feed();
    }
    pages = a / 4096u;
    p_u32(c, "kb_writable", pages * 4u);
    for (k = 0; k < pages; k++) ((volatile uint32_t *)(base + k * 4096u))[2] = 0xC0DE0000u | k;
    for (k = 0; k < pages; k++)
        if (((volatile uint32_t *)(base + k * 4096u))[2] != (0xC0DE0000u | k)) { mirror = k; break; }
    p_u32(c, "mirror_at_page", mirror);        /* 0: no mirror within kb_writable */
    if (pages) {
        w[0] = rs_time((volatile uint32_t *)base, 0); w[1] = rs_time((volatile uint32_t *)base, 1);
        w[2] = rs_time(rs_main, 0); w[3] = rs_time(rs_main, 1);
        p_words(c, "mticks_load_store_here_vs_main", w, 4);
    }
    return (int32_t)(pages * 4u);
}

HB_PROBE = {HB_PROBE_MAGIC, "RAMSCAN", run};
