/* SPDX-License-Identifier: GPL-3.0-only */
/* REPCOUNT: how many times `rep N rR { }` (a hardware loop) runs its body for a count in rR: 1 added
 * per iteration, for counts 0..300. argument: none */
#include "probe.h"

static __attribute__((noinline)) uint32_t reps(uint32_t n)
{
    uint32_t s = 0;
    __asm__ volatile("rep %[n] {\n\t%[s] += 1\n\t}" : [s] "+r"(s) : [n] "r"(n));
    return s;
}

static int32_t run(hb_probe_ctx_t *c)
{
    static const uint32_t counts[] = {0, 1, 2, 3, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, 129, 255, 256, 257, 300};
    uint32_t got[20], i;
    for (i = 0; i < 20u; i++) got[i] = reps(counts[i]);
    p_words(c, "counts", counts, 20);
    p_words(c, "iterations", got, 20);
    return 0;
}

HB_PROBE = {HB_PROBE_MAGIC, "REPCOUNT", run};
