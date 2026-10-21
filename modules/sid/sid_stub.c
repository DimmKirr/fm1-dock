/* SPDX-License-Identifier: GPL-3.0-only */
/* What sidsynth.c wants of FM-1 SID's own SID model (sid.c), which this module does not have:
 * its notes reach reSID through sidsyn_t.write / env, so the own chips only keep their registers. */
#include "sid.h"

void sid_init(sid_t *s, sid_model_t model, uint32_t clock_hz, uint32_t fs)
{
    uint32_t i;
    uint8_t *p = (uint8_t *)s;
    (void)clock_hz;
    for (i = 0; i < sizeof *s; i++) p[i] = 0;
    s->model = (uint8_t)model;
    s->fs = fs;
}
void sid_write(sid_t *s, uint32_t reg, uint32_t val) { if (reg < 32u) s->reg[reg] = (uint8_t)val; }
void sid_render(sid_t *s, int32_t *out, uint32_t n) { (void)s; while (n--) *out++ = 0; }
void sid_render_add(sid_t *s, int32_t *out, uint32_t n) { (void)s; (void)out; (void)n; }
