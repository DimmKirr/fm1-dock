/* SPDX-License-Identifier: GPL-3.0-only */
/* CYCLES: one instruction a statement (inline assembly), 8 to a loop round, interrupts off, the
 * empty loop taken off (P_TIME): what each instruction class costs on the FM-1 and in fm1-emu, for
 * fm1-emu's cycle table. A dependent chain (each one needs the last one's result) and, where it
 * matters, independent pairs (dual issue). The code runs from RAM. argument: [rounds] */
#include "probe.h"

#define X8(s) s; s; s; s; s; s; s; s
static uint32_t cy_ram[64] __attribute__((aligned(8)));
static volatile uint32_t cy_va = 12345, cy_vb = 678;
static __attribute__((noinline)) void cy_empty(void) { __asm__ volatile(""); }

static int32_t run(hb_probe_ctx_t *c)
{
    uint32_t n = p_arg32(c, 0, 2000), a = cy_va, b = cy_vb | 1u, d = 0, e = 3, i;
    uint32_t *ram = cy_ram;
    for (i = 0; i < 64u; i++)
        cy_ram[i] = (uint32_t)&cy_ram[(i + 1u) & 63u];      /* (a ring of pointers: a load chain) */
    P_TIME(c, "nop_x8", n, X8(__asm__ volatile("nop")));
    P_TIME(c, "mov_imm32_x8", n, X8(__asm__ volatile("%0 = 305419896" : "=r"(d))));
    P_TIME(c, "add_rr_dep_x8", n, X8(__asm__ volatile("%0 = %0 + %1" : "+r"(a) : "r"(b))));
    P_TIME(c, "add_rr_pair_x8", n, X8(__asm__ volatile("%0 = %0 + %2\n\t%1 = %1 + %2" : "+r"(a), "+r"(d) : "r"(b))));
    P_TIME(c, "add_imm_x8", n, X8(__asm__ volatile("%0 += 7" : "+r"(a))));
    P_TIME(c, "and_rr_x8", n, X8(__asm__ volatile("%0 = %0 & %1" : "+r"(a) : "r"(b))));
    P_TIME(c, "shl_imm_x8", n, X8(__asm__ volatile("%0 = %0 << 1" : "+r"(a))));
    P_TIME(c, "shr_reg_x8", n, X8(__asm__ volatile("%0 = %0 >> %1" : "+r"(a) : "r"(e))));
    P_TIME(c, "mul_rrr_x8", n, X8(__asm__ volatile("%0 = %0 * %1" : "+r"(a) : "r"(b))));
    P_TIME(c, "mul_pair_x8", n, X8(__asm__ volatile("%0 = %0 * %2\n\t%1 = %1 * %2" : "+r"(a), "+r"(d) : "r"(b))));
    P_TIME(c, "div_u_x8", n / 8u + 32u, X8(__asm__ volatile("%0 = %0 / %1 (u)" : "+r"(a) : "r"(b))));
    P_TIME(c, "ld_w_x8", n, X8(__asm__ volatile("%0 = [%1+0]" : "=r"(d) : "r"(ram) : "memory")));
    P_TIME(c, "ld_w_chain_x8", n, X8(__asm__ volatile("%0 = [%0+0]" : "+r"(ram) :: "memory")));
    P_TIME(c, "ld_h_x8", n, X8(__asm__ volatile("%0 = h[%1+0] (u)" : "=r"(d) : "r"(ram) : "memory")));
    P_TIME(c, "ld_b_x8", n, X8(__asm__ volatile("%0 = b[%1+0] (u)" : "=r"(d) : "r"(ram) : "memory")));
    P_TIME(c, "st_w_x8", n, X8(__asm__ volatile("[%0+0] = %1" :: "r"(&cy_ram[32]), "r"(a) : "memory")));
    P_TIME(c, "st_ld_x8", n, X8(__asm__ volatile("[%1+0] = %0\n\t%0 = [%1+0]" : "+r"(a) : "r"(&cy_ram[40]) : "memory")));
    P_TIME(c, "push_pop4_x8", n, X8(__asm__ volatile("[--sp] = {r7-r4}\n\t{r7-r4} = [sp++]" ::: "memory")));
    P_TIME(c, "goto_next_x8", n, X8(__asm__ volatile("goto 1f\n1:")));
    P_TIME(c, "if_not_taken_x8", n, X8(__asm__ volatile("if (%0 == 0) goto 1f\n\tnop\n1:" :: "r"(b))));
    P_TIME(c, "if_taken_x8", n, X8(__asm__ volatile("if (%0 != 0) goto 1f\n\tnop\n1:" :: "r"(b))));
    P_TIME(c, "call_empty_x8", n / 8u + 32u, X8(cy_empty()));
    cy_va = a + d + (uint32_t)ram;
    return 0;
}

HB_PROBE = {HB_PROBE_MAGIC, "CYCLES", run};
