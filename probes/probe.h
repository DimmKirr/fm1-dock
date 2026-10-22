/* SPDX-License-Identifier: GPL-3.0-only */
/* What a probe (firmware/harbor/hb_module.h, "Probes"; tools/probe.py) is written with.
 *
 *   #include "probe.h"
 *   static int32_t run(hb_probe_ctx_t *c)
 *   {
 *       p_u32(c, "answer", 42);
 *       P_TIME(c, "nop", 1000, __asm__ volatile("nop"));
 *       return 0;
 *   }
 *   HB_PROBE = {HB_PROBE_MAGIC, "MY PROBE", run};
 *
 * The output is a list of records, [type u8][key length u8][key][value length u16][value], which
 * tools/probe.py prints as JSON ({key: value}, a key written again becomes a list):
 *   p_u32 / p_i32   a number          p_hex   a number shown in hex
 *   p_bytes         bytes (hex)       p_text  a string
 *   p_words         u32[] (a list)
 * A record is complete before out_n counts it, so a probe that crashes leaves the ones before.
 *
 * Time: TIMER4, 24 MHz (c->host->ticks()). P_TIME(c, key, n, stmt) runs stmt n times with the
 * interrupts off, subtracts an empty loop of n, and writes [ticks, n]; tools/probe.py adds the
 * nanoseconds per run. fm1-emu runs one instruction per 240 MHz cycle: 10 per tick. */
#ifndef PROBE_H
#define PROBE_H
#include "hb_module.h"

enum { P_U32 = 1, P_I32 = 2, P_HEX = 3, P_BYTES = 4, P_TEXT = 5, P_WORDS = 6, P_TIME_T = 7 };

static inline uint32_t p_strlen(const char *s) { uint32_t n = 0; while (s[n]) n++; return n; }

static void p_rec(hb_probe_ctx_t *c, uint32_t type, const char *key, const void *v, uint32_t n)
{
    uint32_t k = p_strlen(key), at = c->out_n, i;
    const uint8_t *p = (const uint8_t *)v;
    if (k > 255u) k = 255u;
    if (n > 0xFFFFu || at + 4u + k + n > HB_PROBE_OUT)
        return;                                 /* (full: the rest is dropped) */
    c->out[at] = (uint8_t)type;
    c->out[at + 1u] = (uint8_t)k;
    for (i = 0; i < k; i++) c->out[at + 2u + i] = (uint8_t)key[i];
    c->out[at + 2u + k] = (uint8_t)n;
    c->out[at + 3u + k] = (uint8_t)(n >> 8);
    for (i = 0; i < n; i++) c->out[at + 4u + k + i] = p[i];
    c->out_n = at + 4u + k + n;                 /* last: a crash leaves only whole records */
}
static void p_u32(hb_probe_ctx_t *c, const char *key, uint32_t v) { p_rec(c, P_U32, key, &v, 4); }
static void p_i32(hb_probe_ctx_t *c, const char *key, int32_t v) { p_rec(c, P_I32, key, &v, 4); }
static void p_hex(hb_probe_ctx_t *c, const char *key, uint32_t v) { p_rec(c, P_HEX, key, &v, 4); }
static void p_bytes(hb_probe_ctx_t *c, const char *key, const void *p, uint32_t n) { p_rec(c, P_BYTES, key, p, n); }
static void p_text(hb_probe_ctx_t *c, const char *key, const char *s) { p_rec(c, P_TEXT, key, s, p_strlen(s)); }
static void p_words(hb_probe_ctx_t *c, const char *key, const uint32_t *w, uint32_t n) { p_rec(c, P_WORDS, key, w, 4u * n); }

/* the interrupts off and on (as Dinghy's fm1_irq_off / fm1_irq_on) */
static inline void p_irq_off(void) { __asm__ volatile("cli" ::: "memory"); }
static inline void p_irq_on(void) { __asm__ volatile("csync\n\tsti" ::: "memory"); }

/* after writing code to RAM, before running it */
static inline void p_csync(void) { __asm__ volatile("csync" ::: "memory"); }

/* TIMER4 directly (the host's ticks() is a call away: this is two instructions) */
#define P_T4 (*(volatile uint32_t *)0x10804u)
static inline uint32_t p_ticks(void) { return P_T4; }

/* stmt n times, the interrupts off, an empty loop of n taken off; [ticks, n] under key */
#define P_TIME(c, key, n, stmt) do {                                            \
        uint32_t p_n_ = (n), p_i_, p_t0_, p_t1_, p_t2_, p_r_[2];               \
        p_irq_off();                                                            \
        p_t0_ = p_ticks();                                                      \
        for (p_i_ = 0; p_i_ < p_n_; p_i_++) __asm__ volatile("" ::: "memory");  \
        p_t1_ = p_ticks();                                                      \
        for (p_i_ = 0; p_i_ < p_n_; p_i_++) { stmt; __asm__ volatile("" ::: "memory"); } \
        p_t2_ = p_ticks();                                                      \
        p_irq_on();                                                             \
        p_r_[0] = (p_t2_ - p_t1_) - (p_t1_ - p_t0_);                            \
        p_r_[1] = p_n_;                                                         \
        p_rec((c), P_TIME_T, (key), p_r_, 8);                                   \
    } while (0)

/* the argument (RUN's payload) as little-endian words */
static inline uint32_t p_arg32(const hb_probe_ctx_t *c, uint32_t i, uint32_t def)
{
    const uint8_t *a = c->arg + 4u * i;
    return 4u * i + 4u <= c->arg_n ? a[0] | (uint32_t)a[1] << 8 | (uint32_t)a[2] << 16 | (uint32_t)a[3] << 24 : def;
}
#endif
