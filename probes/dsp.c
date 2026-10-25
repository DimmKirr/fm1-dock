/* SPDX-License-Identifier: GPL-3.0-only */
/* DSP: what the FM-1's DSP-style instructions cost (8 to a round, interrupts off, the empty loop
 * taken off) and what they give: the 64-bit multiply and multiply-accumulate C compiles to, the
 * 64-bit shift, abs, min / max, clz, the saturating shift (__builtin_pi32v2_qasl: a clamp in one), a
 * predicated ifs block (clamp16), saturating add and halfword MACs (inline assembly), and whether a
 * prefetch (pfetch) hides a flash miss. argument: [rounds] */
#include "probe.h"

typedef long long i64;
#define X8(s) s; s; s; s; s; s; s; s
static volatile int dp_va = 12345, dp_vb = -678;
static __attribute__((noinline)) int dp_clamp16(int x) { return x > 32767 ? 32767 : x < -32768 ? -32768 : x; }

static int32_t run(hb_probe_ctx_t *c)
{
    uint32_t n = p_arg32(c, 0, 2000), i, t, miss, pf;
    int a = dp_va, b = dp_vb, d = 0, s = 3;
    i64 acc = 1;
    const volatile uint32_t *x = (const volatile uint32_t *)0x02040000u;
    P_TIME(c, "mul64_shr_chain_x8", n, X8(a = (int)(((i64)a * b) >> 15) | 1));
    P_TIME(c, "mac64_x8", n, X8(__asm__ volatile("" : "+r"(a)); acc += (i64)a * b));
    P_TIME(c, "mac64_shr_x8", n, X8(__asm__ volatile("" : "+r"(a)); acc += (i64)a * b; d += (int)(acc >> 15)));
    P_TIME(c, "abs_x8", n, X8(__asm__ volatile("%0 = abs(%0)" : "+r"(a))));
    P_TIME(c, "smax_x8", n, X8(__asm__ volatile("%0 = smax(%0, %1)" : "+r"(a) : "r"(b))));
    P_TIME(c, "clz_x8", n, X8(__asm__ volatile("%0 = clz(%0)" : "+r"(a))));
    P_TIME(c, "qasl_x8", n, X8(a = __builtin_pi32v2_qasl(a, s)));
    P_TIME(c, "ssat_add_x8", n, X8(__asm__ volatile("%0 = %0 + %1 (ssat)" : "+r"(a) : "r"(b))));
    P_TIME(c, "mac_hh_ssat_x8", n, X8(__asm__ volatile("%0 += %1.h * %2.h (ssat)" : "+r"(d) : "r"(a), "r"(b))));
    P_TIME(c, "mac_ll_ssat_x2_x8", n, X8(__asm__ volatile("%0 += %1.l * %2.l (ssat,x2)" : "+r"(d) : "r"(a), "r"(b))));
    P_TIME(c, "clamp16_call_x8", n / 8u + 32u, X8(__asm__ volatile("" : "+r"(a)); d += dp_clamp16(a)));
    /* results, for the emulator's semantics */
    p_i32(c, "r_qasl_big", __builtin_pi32v2_qasl(0x40000000, 2));
    p_i32(c, "r_qasl_neg", __builtin_pi32v2_qasl(-0x40000000, 2));
    { int r = 0x7FFFFFF0, q = 0x100; __asm__ volatile("%0 = %0 + %1 (ssat)" : "+r"(r) : "r"(q)); p_hex(c, "r_ssat_add_over", (uint32_t)r); }
    { int r = 0x7FFF0000, q = 0x7FFF7FFF, w = 0x7FFF7FFF; __asm__ volatile("%0 += %1.h * %2.h (ssat)" : "+r"(r) : "r"(q), "r"(w)); p_hex(c, "r_mac_hh_ssat", (uint32_t)r); }
    { int r = 0, q = 0x00008000, w = 0x00008000; __asm__ volatile("%0 += %1.l * %2.l (ssat,x2)" : "+r"(r) : "r"(q), "r"(w)); p_hex(c, "r_mac_ll_ssat_x2_min", (uint32_t)r); }
    p_i32(c, "r_clz_1", __builtin_pi32v2_clz(1));
    p_i32(c, "r_cls_m1", __builtin_pi32v2_cls(-1));
    /* pfetch: a load from a flash line not in the cache (the value used at once: the time includes
     * the fill), one prefetched ~2 us before, one already in the cache. Lines 64 KB apart: none of
     * them in the cache from before */
    {
        uint32_t v, hit;
        const volatile uint32_t *ev = (const volatile uint32_t *)0x02080000u;
        p_irq_off();
        for (i = 0; i < 65536u / 64u; i++)      /* (64 KB of other lines: the 32 KB cache emptied) */
            d += (int)ev[i * 16u];
        d += (int)x[0x8000];                    /* (the line of the hit case, in) */
        t = p_ticks(); v = x[0]; __asm__ volatile("%0 = %0 + %0" : "+r"(v)); miss = p_ticks() - t; d += (int)v;
        __builtin_pi32v2_pfetch((int)&x[0x4000]);
        for (i = 0; i < 400u; i++) __asm__ volatile("nop");
        t = p_ticks(); v = x[0x4000]; __asm__ volatile("%0 = %0 + %0" : "+r"(v)); pf = p_ticks() - t; d += (int)v;
        t = p_ticks(); v = x[0x8000]; __asm__ volatile("%0 = %0 + %0" : "+r"(v)); hit = p_ticks() - t; d += (int)v;
        p_irq_on();
        p_u32(c, "hit_ticks", hit);
    }
    p_u32(c, "miss_ticks", miss);
    p_u32(c, "after_pfetch_ticks", pf);
    dp_va = a + d + (int)acc;
    return 0;
}

HB_PROBE = {HB_PROBE_MAGIC, "DSP", run};
