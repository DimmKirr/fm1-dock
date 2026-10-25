/* SPDX-License-Identifier: GPL-3.0-only */
/* SLOW: a test module (tests/deploy_test.py) whose render takes one and a half blocks once a key is
 * down: slower than real time, which the host must stop (dock.c, overload) and play the sine. */
#include "hb_module.h"

static const hb_host_t *H;
static volatile uint32_t held;

static void render(int32_t *out, uint32_t n)
{
    uint32_t t0 = H->ticks();
    (void)out;
    (void)n;
    if (held)
        while (H->ticks() - t0 < 3u * 139296u / 2u)   /* 1.5 blocks of TIMER4 (24 MHz) */
            ;
}

static void note(uint32_t ch, uint32_t key, uint32_t vel) { (void)ch; (void)key; held = vel != 0; }
static void cc(uint32_t ch, uint32_t num, uint32_t val) { (void)ch; (void)num; (void)val; }
static void ui(uint32_t ev, int32_t arg) { (void)ev; (void)arg; }
static void draw(uint32_t full) { (void)full; }
static void release(void) { held = 0; }
static int init(const hb_host_t *h)
{
    H = h;
    held = 0;
    H->log("slow: ready");
    return 0;
}

HB_DESC = {HB_ABI, "SLOW", init, release, note, cc, render, ui, draw, 0, 0, 0, 0};
