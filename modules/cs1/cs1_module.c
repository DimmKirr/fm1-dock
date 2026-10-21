/* SPDX-License-Identifier: GPL-3.0-only */
/* CS-1: twochannels' CS-80-style synthesizer (github.com/twochannels/CS-1, 0.3.2, 4e0ebe3) as a
 * harbor module: its own app, cs1/cs1.c, unchanged but for one line (its platform include, now
 * cs1_platform.h), with its voice, effects, sounds, pages, user presets and MIDI. The host runs
 * CS-1's app_init() at init and its app_loop() from draw(); the voice renders in the host's
 * audio interrupt. What a module cannot have: cs1_platform.h (the keys' sensing, the second
 * core). HOME held 1.5 s alone opens harbor's module list (a short HOME is CS-1's). */
#include "cs1_platform.h"
#include "cs1/cs1.c"

static void render(int32_t *out, uint32_t n)
{
    if (n == HALF_FRAMES)
        app_audio(out, n);
}

static void ui(uint32_t ev, int32_t arg) { hbf_event(ev, arg); }
static void midi(uint32_t st, uint32_t d1, uint32_t d2) { hbf_midi(st, d1, d2); }
static void draw(uint32_t full)
{
    if (full) page_drawn = ~0u;                 /* (back from the module list: the page again) */
    app_loop();
}

static void note(uint32_t ch, uint32_t key, uint32_t vel)
{
    (void)ch;
    if (vel) note_on(key, vel);
    else note_off(key);
}
static void cc(uint32_t ch, uint32_t num, uint32_t val) { hbf_midi(0xB0u | (ch & 15u), num, val); }

static int init(const hb_host_t *h)
{
    H = h;
    app_init();
    H->log("cs1: CS-1 " APP_VER);
    return 0;
}

static void release(void)
{
    uint32_t v;
    for (v = 0; v < NV; v++)
        voices[v].gate = 0;
}

HB_DESC = {HB_ABI, "CS-1", init, release, note, cc, render, ui, draw, 0, 0, HB_F_SCREEN | HB_F_PANEL, midi};
