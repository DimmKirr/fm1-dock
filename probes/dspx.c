/* SPDX-License-Identifier: GPL-3.0-only */
/* DSPX: audio kernels on the FM-1 (and fm1-emu), 256 samples each from RAM: cycles a sample (TIMER4
 * ticks x 10: 240 MHz) and a checksum of the output (the same on both, or a semantics bug). Biquad
 * (naive 32-bit, 64-bit MAC as C gives it, Q15 halfword MAC with saturation, float on the FPU),
 * state-variable filter (int, float), oscillators (saw, polynomial sine, wavetable from RAM, from a
 * 128 KB span of flash, the same with a prefetch a sample ahead), clamp (branches, qasl), a 4-voice
 * saturating mix. argument: [skip mask] (bit k: skip kernel k; the emulator lacks the halfword MAC) */
/* hbmod-cflags: -mcpu=r3 -mfprev1 */
#include "probe.h"

typedef long long i64;
#define N 256u
static int16_t in16[N], out16[N];
static int32_t in32[N], out32[N];
static float inf[N], outf[N];
static int16_t tab_ram[1025];

static uint32_t sum16(const int16_t *p) { uint32_t s = 0, i; for (i = 0; i < N; i++) s = s * 31u + (uint16_t)p[i]; return s; }
static uint32_t sum32(const int32_t *p) { uint32_t s = 0, i; for (i = 0; i < N; i++) s = s * 31u + (uint32_t)p[i]; return s; }
static uint32_t sumf(const float *p) { uint32_t s = 0, i; for (i = 0; i < N; i++) s = s * 31u + (uint32_t)(int32_t)(p[i] * 1024.0f); return s; }

/* biquad, low-pass ~1 kHz at 44.1 kHz: b0 b1 b2 a1 a2 */
static void bq_c32(void) {                   /* Q14 coefficients, 32-bit products */
    static const int32_t b0 = 81, b1 = 162, b2 = 81, a1 = -31233, a2 = 14787;   /* (Q14) */
    int32_t x1 = 0, x2 = 0, y1 = 0, y2 = 0, i;
    for (i = 0; i < (int32_t)N; i++) {
        int32_t x = in32[i], y = (b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2) >> 14;
        x2 = x1; x1 = x; y2 = y1; y1 = y; out32[i] = y;
    }
}
static void bq_mac64(void) {                 /* Q30 coefficients, 64-bit MAC */
    static const int32_t b0 = 5313232, b1 = 10626464, b2 = 5313232, a1 = -2046980096, a2 = 969078784;
    int32_t x1 = 0, x2 = 0, y1 = 0, y2 = 0, i;
    for (i = 0; i < (int32_t)N; i++) {
        int32_t x = in32[i];
        i64 acc = (i64)b0 * x + (i64)b1 * x1 + (i64)b2 * x2 - (i64)a1 * y1 - (i64)a2 * y2;
        int32_t y = (int32_t)(acc >> 30);
        x2 = x1; x1 = x; y2 = y1; y1 = y; out32[i] = y;
    }
}
static void bq_hmac(void) {                  /* Q15 halfwords, a 32-bit accumulator: x2 and saturation */
    /* coefficients halved (Q14 in Q15 form, a1 = -1.906 does not fit Q15): acc in Q30, then << 1 */
    int32_t cb = (81 << 16) | (162 & 0xFFFF), cb2 = 81, ca = ((15617) << 16) | ((-7393) & 0xFFFF);   /* -a1/2, -a2/2 */
    int32_t xs = 0, ys = 0, x2 = 0, i;       /* xs = x1:x (hi:lo), ys = y2:y1 */
    for (i = 0; i < (int32_t)N; i++) {
        int32_t acc = 0, x = in16[i], y;
        xs = (xs << 16) | (x & 0xFFFF);      /* hi: x1, lo: x */
        __asm__ volatile("%0 += %1.l * %2.l (ssat,x2)" : "+r"(acc) : "r"(cb), "r"(xs));    /* b1 * x  */
        __asm__ volatile("%0 += %1.h * %2.h (ssat,x2)" : "+r"(acc) : "r"(cb), "r"(xs));    /* b0 * x1 */
        __asm__ volatile("%0 += %1.l * %2.l (ssat,x2)" : "+r"(acc) : "r"(cb2), "r"(x2));   /* b2 * x2 */
        __asm__ volatile("%0 += %1.h * %2.l (ssat,x2)" : "+r"(acc) : "r"(ca), "r"(ys));    /* -a1/2 * y1 */
        __asm__ volatile("%0 += %1.l * %2.h (ssat,x2)" : "+r"(acc) : "r"(ca), "r"(ys));    /* -a2/2 * y2 */
        y = __builtin_pi32v2_qasl(acc, 2) >> 16;   /* (x2 x2: Q15, saturated) */
        x2 = xs >> 16; ys = (ys << 16) | (y & 0xFFFF);
        out16[i] = (int16_t)y;
    }
}
static void bq_float(void) {
    const float b0 = 0.00494f, b1 = 0.00989f, b2 = 0.00494f, a1 = -1.9064f, a2 = 0.9025f;
    float x1 = 0, x2 = 0, y1 = 0, y2 = 0; uint32_t i;
    for (i = 0; i < N; i++) {
        float x = inf[i], y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y; outf[i] = y;
    }
}
static void svf_int(void) {                  /* Chamberlin SVF, Q15 f and q, low-pass out */
    int32_t lp = 0, bp = 0, f = 4660, q = 23170, i;
    for (i = 0; i < (int32_t)N; i++) {
        int32_t hp = in32[i] - lp - ((q * bp) >> 15);
        bp += (f * hp) >> 15; lp += (f * bp) >> 15; out32[i] = lp;
    }
}
static void svf_float(void) {
    float lp = 0, bp = 0, f = 0.1422f, q = 0.7071f; uint32_t i;
    for (i = 0; i < N; i++) { float hp = inf[i] - lp - q * bp; bp += f * hp; lp += f * bp; outf[i] = lp; }
}
static void osc_saw(void) { uint32_t ph = 0, i; for (i = 0; i < N; i++) { ph += 42949673u; out16[i] = (int16_t)(ph >> 16); } }
static void osc_sine_poly(void) {            /* parabolic sine, Q15 */
    uint32_t ph = 0, i;
    for (i = 0; i < N; i++) {
        int32_t x, y;
        ph += 42949673u; x = (int32_t)ph >> 16;                  /* -32768..32767 */
        y = (x * (32768 - (x < 0 ? -x : x))) >> 13;
        out16[i] = (int16_t)__builtin_pi32v2_qasl(y, 16 - 16);
    }
}
static void osc_tab_ram(void) {              /* 1024-entry table, linear interpolation */
    uint32_t ph = 0, i;
    for (i = 0; i < N; i++) {
        uint32_t k = ph >> 22, fr = (ph >> 7) & 0x7FFF;
        int32_t a = tab_ram[k], b = tab_ram[k + 1];
        ph += 42949673u * 7u; out16[i] = (int16_t)(a + (((b - a) * (int32_t)fr) >> 15));
    }
}
static void osc_tab_flash(const int16_t *t, int pf) {   /* a 64 K-entry (128 KB) table in flash: misses */
    uint32_t ph = 0x12345678u, i;
    for (i = 0; i < N; i++) {
        uint32_t k = ph >> 16, fr = ph & 0x7FFF, nk = (ph + 2654435761u) >> 16;
        int32_t a, b;
        if (pf) __builtin_pi32v2_pfetch((int)&t[nk]);
        a = t[k]; b = t[k + 1];
        ph += 2654435761u; out16[i] = (int16_t)(a + (((b - a) * (int32_t)fr) >> 15));
    }
}
static void clamp_branch(void) { uint32_t i; for (i = 0; i < N; i++) { int32_t x = in32[i] * 3; out16[i] = (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x); } }
static void clamp_qasl(void) { uint32_t i; for (i = 0; i < N; i++) { int32_t x = in32[i] * 3; out16[i] = (int16_t)(__builtin_pi32v2_qasl(x, 16) >> 16); } }
static void mix4_sat(void) {
    uint32_t i;
    for (i = 0; i < N; i++) {
        int32_t a = in32[i] << 15, s = a;
        __asm__ volatile("%0 = %0 + %1 (ssat)" : "+r"(s) : "r"(a)); __asm__ volatile("%0 = %0 + %1 (ssat)" : "+r"(s) : "r"(a));
        __asm__ volatile("%0 = %0 + %1 (ssat)" : "+r"(s) : "r"(a));
        out32[i] = s;
    }
}

static void timed(hb_probe_ctx_t *c, uint32_t skip, uint32_t k, const char *name, void (*f)(void), uint32_t (*sum)(void))
{
    uint32_t t, r[2];
    if (skip >> k & 1u) return;
    f();                                       /* (warm) */
    p_irq_off(); t = p_ticks(); f(); t = p_ticks() - t; p_irq_on();
    r[0] = t * 10u * 100u / N;                 /* (cycles x 100 a sample) */
    r[1] = sum();
    p_words(c, name, r, 2);
    c->wdt_feed();
}
static uint32_t s16(void) { return sum16(out16); }
static uint32_t s32(void) { return sum32(out32); }
static uint32_t sf(void) { return sumf(outf); }
static void flash0(void) { osc_tab_flash((const int16_t *)0x02020000u, 0); }
static void flash1(void) { osc_tab_flash((const int16_t *)0x02020000u, 1); }

static int32_t run(hb_probe_ctx_t *c)
{
    uint32_t skip = p_arg32(c, 0, 0), i, s = 1;
    for (i = 0; i < N; i++) {                  /* a reproducible input: noise with a square under it */
        s = s * 1103515245u + 12345u;
        in16[i] = (int16_t)(((s >> 16) & 0x1FFF) - 0x1000 + ((i & 64u) ? 8000 : -8000));
        in32[i] = in16[i]; inf[i] = (float)in16[i];
    }
    for (i = 0; i <= 1024u; i++) {             /* one sine cycle, parabolic, Q15 */
        int32_t x = (int32_t)(i * 64u) - 32768; tab_ram[i] = (int16_t)((x * (32768 - (x < 0 ? -x : x))) >> 15);
    }
    timed(c, skip, 0, "bq_c32", bq_c32, s32);
    timed(c, skip, 1, "bq_mac64", bq_mac64, s32);
    timed(c, skip, 2, "bq_hmac", bq_hmac, s16);
    timed(c, skip, 3, "bq_float", bq_float, sf);
    timed(c, skip, 4, "svf_int", svf_int, s32);
    timed(c, skip, 5, "svf_float", svf_float, sf);
    timed(c, skip, 6, "osc_saw", osc_saw, s16);
    timed(c, skip, 7, "osc_sine_poly", osc_sine_poly, s16);
    timed(c, skip, 8, "osc_tab_ram", osc_tab_ram, s16);
    timed(c, skip, 9, "osc_tab_flash", flash0, s16);
    timed(c, skip, 10, "osc_tab_flash_pfetch", flash1, s16);
    timed(c, skip, 11, "clamp_branch", clamp_branch, s16);
    timed(c, skip, 12, "clamp_qasl", clamp_qasl, s16);
    timed(c, skip, 13, "mix4_ssat", mix4_sat, s32);
    return 0;
}

HB_PROBE = {HB_PROBE_MAGIC, "DSPX", run};
