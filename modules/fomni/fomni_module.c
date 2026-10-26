/* SPDX-License-Identifier: GPL-3.0-only */
/* FOMNI: Charles Vestal's FoMni (github.com/charlesvestal/fm1-fomni, 7e7547f; its files in fomni/ unchanged, see
 * take.py), a chord harp after the Omnichord, as a fm1-dock module: chords on the black keys, strummed on the white
 * ones, a chord section, a bass and a rhythm box, through a plate. Its app on fm1-dock's host (fomni_platform.inc: its
 * platform calls); its engine renders in the host's audio interrupt, as on its own firmware (one core), with the FPU
 * as FoMni builds it (module.cfg). HOME held 1.5 s alone opens fm1-dock's module list. */
#include "fomni_platform.inc"
#include "project.c"
#include "ui.c"

#define OM_BLOCK 256u
static int32_t om_buf[2u * OM_BLOCK];
static void fomni_render(int32_t *out, uint32_t n)
{
    uint32_t t0 = H->ticks(), i;
    if (n != OM_BLOCK)
        return;
    om_render(om_buf, n, 4096u);            /* (the host applies MASTER) */
    for (i = 0; i < 2u * n; i++) {          /* FoMni's 24 bits into the host's Q15, added */
        int32_t v = out[i] + (om_buf[i] >> 8);
        out[i] = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
    }
    om_load = (H->ticks() - t0) * 100u / (5805u * 24u);
}

static void fomni_ui(uint32_t ev, int32_t arg) { hbf_event(ev, arg); }
static void fomni_midi(uint32_t st, uint32_t d1, uint32_t d2) { hbf_midi(st, d1, d2); }
static void fomni_note(uint32_t ch, uint32_t key, uint32_t vel) { hbf_midi(0x90u | (ch & 15u), key, vel); }
static void fomni_cc(uint32_t ch, uint32_t num, uint32_t val) { hbf_midi(0xB0u | (ch & 15u), num, val); }

static uint32_t om_last_frame;
static void fomni_draw(uint32_t full)
{
    if (full) {                             /* (back from the module list: the whole screen and the lights again) */
        ui_init();
        om_lights(1);
    }
    if (H->ms() - om_last_frame >= 16u) {   /* ~60 frames a second at most, input in between (its main loop) */
        om_last_frame = H->ms();
        ui_frame();
    } else {
        ui_input_only();
    }
}

static int fomni_init(const hb_host_t *h)
{
    H = h;
    om_init();
    project_load();                         /* defaults when nothing (or another format) is saved */
    project_apply();
    ui_init();
    om_lights(1);
    H->log("fomni: FoMni (charlesvestal/fm1-fomni 7e7547f)");
    return 0;
}

static void fomni_release(void)
{
    om_play(0);
    om_panic();
}

HB_DESC = {HB_ABI, "FOMNI", fomni_init, fomni_release, fomni_note, fomni_cc, fomni_render, fomni_ui, fomni_draw, 0, 0,
           HB_F_SCREEN | HB_F_PANEL, fomni_midi};
