/* SPDX-License-Identifier: GPL-3.0-only */
/* TIMING: what instructions and memory cost, in TIMER4 ticks (tools/probe.py prints ns and, with
 * fm1-emu's model of one instruction per 240 MHz cycle, the cycles the emulator charges). Each
 * case runs 8 times per loop round, so the loop's own cost (taken off by P_TIME) stays small.
 * argument: [rounds u32] (default 2000) */
#include "probe.h"

#define X8(s) s; s; s; s; s; s; s; s
static volatile uint32_t va = 12345, vb = 678, vc;
static volatile int64_t vl;
static uint32_t ram_buf[4096];                  /* 16 KiB in the probe's RAM */
static const uint32_t rodata_tab[1024] = {1};    /* .rodata: in RAM too (the probe runs from RAM) */
static __attribute__((noinline)) uint32_t xip_sum(const volatile uint32_t *p, uint32_t n, uint32_t stride)
{
    uint32_t s = 0, i;
    for (i = 0; i < n; i++) s += p[(i * stride) & 1023u];
    return s;
}

static int32_t run(hb_probe_ctx_t *c)
{
    uint32_t n = p_arg32(c, 0, 2000), a = va, b = vb, i;
    int64_t l = vl;
    const volatile uint32_t *xip = (const volatile uint32_t *)0x02010000u;   /* the host's code, in flash */
    P_TIME(c, "nop_x8", n, X8(__asm__ volatile("nop")));
    P_TIME(c, "add_x8", n, X8(__asm__ volatile("" : "+r"(a) : "r"(b)); a += b));
    P_TIME(c, "mul_x8", n, X8(__asm__ volatile("" : "+r"(a) : "r"(b)); a *= b));
    P_TIME(c, "mac64_x8", n, X8(__asm__ volatile("" : "+r"(l) : "r"(a)); l += (int64_t)(int32_t)a * (int32_t)b));
    P_TIME(c, "div_x8", n / 8u + 32u, X8(__asm__ volatile("" : "+r"(a) : "r"(b)); a = a / (b | 1u) + 77777u));
    P_TIME(c, "ld_ram_x8", n, X8(vc = ram_buf[a & 4095u]; a += 17u));
    P_TIME(c, "st_ram_x8", n, X8(ram_buf[a & 4095u] = a; a += 17u));
    P_TIME(c, "ld_volatile_x8", n, X8(a += va));
    P_TIME(c, "ld_xip_seq_x8", n, X8(a += xip[i = (a & 255u)]));
    P_TIME(c, "ld_xip_stride_x8", n / 8u + 32u, X8(a += xip[(a * 97u) & 16383u]));
    P_TIME(c, "ld_rodata_x8", n, X8(a += rodata_tab[a & 1023u]));
    P_TIME(c, "call_x8", n / 8u + 32u, X8(a += xip_sum(rodata_tab, 1, 1)));
    vc = a;
    vl = l;
    (void)i;
    return 0;
}

HB_PROBE = {HB_PROBE_MAGIC, "TIMING", run};
