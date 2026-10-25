/* SPDX-License-Identifier: GPL-3.0-only */
/* DSPX2: how much faster the FM-1's DSP features make inner loops. Hardware loops (rep N rR { }: no
 * branch at the end of an iteration; the compiler never emits them) against C loops, and halfword
 * multiply-accumulates with saturation (inline assembly) against what C gives: loop overhead, a 64-tap
 * FIR on 16-bit samples, a block gain with a clamp. Cycles (240 MHz) per iteration / tap / sample, and
 * each result, to check the fast versions compute the same. argument: none */
/* hbmod-cflags: -mcpu=r3 -mfprev1 */
#include "probe.h"

typedef long long i64;
#define TAPS 64u
#define OUTS 64u
static int16_t x16[TAPS + OUTS] __attribute__((aligned(4)));
static int16_t h16[TAPS] __attribute__((aligned(4)));
static int32_t buf[256];
static int16_t g_in[256] __attribute__((aligned(4))), g_out[256] __attribute__((aligned(4)));

static __attribute__((noinline)) int32_t sum_c(const int32_t *p, uint32_t n)
{
    int32_t s = 0; uint32_t i;
    for (i = 0; i < n; i++) s += p[i];
    return s;
}
/* (a hardware loop's count register is good for 0..31 on the FM-1: probes/repcount.c; so 16 at a time) */
static __attribute__((noinline)) int32_t sum_rep(const int32_t *p, uint32_t n)
{
    int32_t s = 0, a; uint32_t k, i;
    for (i = 0; i < n; i += 16u) {
        k = 16;                                /* (the loop counts it down to 0) */
        __asm__ volatile("rep %[k] {\n\t%[a] = [%[p]++=4]\n\t%[s] += %[a]\n\t}"
                         : [s] "+r"(s), [a] "=&r"(a), [p] "+r"(p), [k] "+r"(k) :: "memory");
    }
    return s;
}
/* one FIR output: sum of x[i] * h[i], Q15 x Q15 */
static __attribute__((noinline)) int32_t fir_c(const int16_t *x, const int16_t *h)
{
    int32_t acc = 0; uint32_t i;
    for (i = 0; i < TAPS; i++) acc += x[i] * h[i];
    return acc << 1;                           /* (Q30 -> Q31, as x2 does) */
}
static __attribute__((noinline)) int32_t fir_c64(const int16_t *x, const int16_t *h)
{
    i64 acc = 0; uint32_t i;
    for (i = 0; i < TAPS; i++) acc += (i64)x[i] * h[i];
    return (int32_t)(acc << 1);
}
static __attribute__((noinline)) int32_t fir_rep2(const int16_t *x, const int16_t *h)
{
    int32_t acc = 0, a, b; uint32_t n, i;
    for (i = 0; i < TAPS / 2u; i += 16u)
    if ((n = 16)) __asm__ volatile("rep %[n] {\n\t%[a] = [%[x]++=4]\n\t%[b] = [%[h]++=4]\n\t"
                     "%[acc] += %[a].l * %[b].l (ssat,x2)\n\t%[acc] += %[a].h * %[b].h (ssat,x2)\n\t}"
                     : [acc] "+r"(acc), [a] "=&r"(a), [b] "=&r"(b), [x] "+r"(x), [h] "+r"(h), [n] "+r"(n) :: "memory");
    return acc;
}
static __attribute__((noinline)) int32_t fir_rep4(const int16_t *x, const int16_t *h)
{
    int32_t acc = 0, a, b, c, d; uint32_t n = TAPS / 4u;
    __asm__ volatile("rep %[n] {\n\t%[a] = [%[x]++=4]\n\t%[b] = [%[h]++=4]\n\t%[c] = [%[x]++=4]\n\t%[d] = [%[h]++=4]\n\t"
                     "%[acc] += %[a].l * %[b].l (ssat,x2)\n\t%[acc] += %[a].h * %[b].h (ssat,x2)\n\t"
                     "%[acc] += %[c].l * %[d].l (ssat,x2)\n\t%[acc] += %[c].h * %[d].h (ssat,x2)\n\t}"
                     : [acc] "+r"(acc), [a] "=&r"(a), [b] "=&r"(b), [c] "=&r"(c), [d] "=&r"(d), [x] "+r"(x), [h] "+r"(h), [n] "+r"(n)
                     :: "memory");
    return acc;
}
static __attribute__((noinline)) void gain_c(const int16_t *in, int16_t *out, int32_t g, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) { int32_t v = (in[i] * g) >> 14; out[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
}
static __attribute__((noinline)) void gain_rep(const int16_t *in, int16_t *out, int32_t g, uint32_t n0)
{
    int32_t a, v; uint32_t n, i;               /* v = in * g (x2, saturated), << 1 saturated, >> 16 */
    for (i = 0; i < n0; i += 16u)
    if ((n = 16)) __asm__ volatile("rep %[n] {\n\t%[a] = h[%[i]++=2] (s)\n\t%[v] = 0\n\t%[v] += %[a].l * %[g].l (ssat,x2)\n\t"
                     "%[v] = %[v] <<< 1\n\t%[v] = %[v] >>> 16\n\th[%[o]++=2] = %[v]\n\t}"
                     : [a] "=&r"(a), [v] "=&r"(v), [i] "+r"(in), [o] "+r"(out), [n] "+r"(n) : [g] "r"(g) : "memory");
}

static uint32_t cyc(uint32_t t, uint32_t per) { return t * 10u * 100u / per; }   /* (cycles x 100 per unit) */

static int32_t run(hb_probe_ctx_t *c)
{
    uint32_t i, t, s = 7, r[3];
    int32_t chk;
    for (i = 0; i < TAPS + OUTS; i++) { s = s * 1103515245u + 12345u; x16[i] = (int16_t)((s >> 16) & 0x3FFF) - 0x2000; }
    for (i = 0; i < TAPS; i++) h16[i] = (int16_t)((i < TAPS / 2u ? i : TAPS - i) * 300);
    for (i = 0; i < 256u; i++) { buf[i] = (int32_t)i; g_in[i] = x16[i % (TAPS + OUTS)]; }

#define TIME(label, per, body) do { body; p_irq_off(); t = p_ticks(); body; t = p_ticks() - t; p_irq_on(); \
        r[0] = cyc(t, per); r[1] = (uint32_t)chk; p_words(c, label, r, 2); c->wdt_feed(); } while (0)
    TIME("loop_c_per_iter", 256u, chk = sum_c(buf, 256));
    TIME("loop_rep_per_iter", 256u, chk = sum_rep(buf, 256));
    TIME("fir_c_per_tap", TAPS * OUTS, { chk = 0; for (i = 0; i < OUTS; i++) chk += fir_c(&x16[i & ~1u], h16) >> 8; });
    TIME("fir_c64_per_tap", TAPS * OUTS, { chk = 0; for (i = 0; i < OUTS; i++) chk += fir_c64(&x16[i & ~1u], h16) >> 8; });
    TIME("fir_hmac_rep2_per_tap", TAPS * OUTS, { chk = 0; for (i = 0; i < OUTS; i++) chk += fir_rep2(&x16[i & ~1u], h16) >> 8; });
    TIME("fir_hmac_rep4_per_tap", TAPS * OUTS, { chk = 0; for (i = 0; i < OUTS; i++) chk += fir_rep4(&x16[i & ~1u], h16) >> 8; });
    TIME("gain_c_per_sample", 256u, { gain_c(g_in, g_out, 20000, 256); chk = g_out[3] + g_out[200]; });
    for (i = 0; i < 256u; i++) g_out[i] = 0;
    TIME("gain_rep_per_sample", 256u, { gain_rep(g_in, g_out, 20000, 256); chk = g_out[3] + g_out[200]; });
    return 0;
}

HB_PROBE = {HB_PROBE_MAGIC, "DSPX2", run};
