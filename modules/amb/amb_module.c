/* SPDX-License-Identifier: GPL-3.0-only */
/* AMB-1: Charles Vestal's generative ambient machine (github.com/charlesvestal/fm1-amb, 7797f4e; MPL-2.0, its files
 * in amb/ unchanged, see take.py) as a fm1-dock module: breathing pads, bells on tape loops, a gliding bass and soft
 * percussion that play themselves, through a tape echo, a tape loop and a plate reverb. Its app on fm1-dock's host
 * (amb_platform.inc: fm1-base's platform calls); its engine renders in the host's audio interrupt, as on fm1-base (one
 * core), with the FPU as fm1-base builds it (module.cfg). HOME held 1.5 s alone opens fm1-dock's module list. */
#include "amb_platform.inc"
#include "app.c"

static int32_t amb_buf[2u * APP_BLOCK];
static void amb_render(int32_t *out, uint32_t n)
{
    uint32_t t0 = H->ticks(), i;
    if (n != APP_BLOCK)
        return;
    app_render(amb_buf, n, 4096u);          /* (the host applies MASTER) */
    for (i = 0; i < 2u * n; i++) {          /* fm1-base's 24 bits into the host's Q15, added */
        int32_t v = out[i] + (amb_buf[i] >> 8);
        out[i] = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
    }
    amb_load = (H->ticks() - t0) * 100u / (5805u * 24u);
}

static void amb_ui(uint32_t ev, int32_t arg) { hbf_event(ev, arg); }
static void amb_midi(uint32_t st, uint32_t d1, uint32_t d2) { hbf_midi(st, d1, d2); }
static void amb_note(uint32_t ch, uint32_t key, uint32_t vel) { hbf_midi(0x90u | (ch & 15u), key, vel); }
static void amb_cc(uint32_t ch, uint32_t num, uint32_t val) { hbf_midi(0xB0u | (ch & 15u), num, val); }

static uint32_t amb_last_frame;
static void amb_draw(uint32_t full)
{
    if (full) {                             /* (back from the module list: the whole screen and the lights again) */
        app_ui_init();
        amb_lights(1);
    }
    if (H->ms() - amb_last_frame >= 16u) {  /* ~60 frames a second at most, input in between (fm1-base's loop) */
        amb_last_frame = H->ms();
        app_frame();
    } else {
        app_input_only();
    }
}

static int amb_init(const hb_host_t *h)
{
    H = h;
    app_init();
    app_ui_init();
    amb_lights(1);
    H->log("amb: AMB-1 (charlesvestal/fm1-amb 7797f4e)");
    return 0;
}

static void amb_release(void) { app_panic(); }

HB_DESC = {HB_ABI, "AMB-1", amb_init, amb_release, amb_note, amb_cc, amb_render, amb_ui, amb_draw, 0, 0,
           HB_F_SCREEN | HB_F_PANEL, amb_midi};
