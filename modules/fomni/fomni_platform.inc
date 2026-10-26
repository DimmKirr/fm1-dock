/* SPDX-License-Identifier: GPL-3.0-only */
/* What FoMni's platform (its app/plat.h, plat_fm1.c) gives FoMni, on the fm1-dock host: the panel, the time and MIDI
 * (modules/lib/hb_fm1.h, the panel in FoMni's own order: B_* and EN_*), the lights (lit, and the glow of the unlit
 * buttons), the screen (FoMni's gfx.c onto the host's fill and blit), the store (the project in the module's first
 * bank) and the audio load (the render timed here). What a module has not: the screen reader over USB (its ann_*
 * calls do nothing). As modules/amb/amb_platform.inc, which grew from FoMni's as AMB-1 did. */
#include <stdint.h>
#include "hb_fm1.h"
#include "app.h"                                /* (plat.h: the panel, the store objects) */

void *memcpy(void *d, const void *s, unsigned n);   /* (modules/lib/hb_lib.c) */
void *memset(void *d, int c, unsigned n);

/* ---- the panel: fm1-base's B_FX .. B_OCTUP and EN_SELECT .. EN_K4, from the host's ids */
static const uint8_t OM_BTN[NB] = {HB_BTN_FX, HB_BTN_SEL, HB_BTN_ENV, HB_BTN_LFO, HB_BTN_EDIT, HB_BTN_GLO, HB_BTN_HOME,
                                    HB_BTN_SAVE, HB_BTN_ARP, HB_BTN_SEQ, HB_BTN_PLAY, HB_BTN_REC, HB_BTN_OCT_DOWN,
                                    HB_BTN_OCT_UP};
static const uint8_t OM_ENC[NE] = {HB_ENC_SELECT, HB_ENC_ALGORITHM, HB_ENC_PRESETS, HB_ENC_KNOB1, HB_ENC_KNOB2,
                                    HB_ENC_KNOB3, HB_ENC_KNOB4};

uint32_t plat_ms(void) { return H->ms(); }
/* held, as fm1-base reads the panel; a press that came and went since the last read (the host hands the module events,
 * FoMni polls) counts as held for that one read, so its release is seen at the next */
uint32_t plat_buttons(void)
{
    uint32_t b = 0, i, held = fm1_in.buttons | fm1_input_edges(0);
    for (i = 0; i < NB; i++)
        if (held & 1u << OM_BTN[i])
            b |= 1u << i;
    return b;
}
uint32_t plat_keys(void) { return fm1_in.notes | fm1_input_note_edges(); }
int32_t plat_enc(int role) { return role >= 0 && role < NE ? fm1_enc_take(OM_ENC[role]) : 0; }

/* ---- the lights: what changed goes to the host (lit 1, the glow 2) */
static uint32_t om_lit_b, om_lit_k, om_glow, om_sent_b = ~0u, om_sent_k = ~0u, om_sent_glow = ~0u;
static void om_lights(int all)
{
    uint32_t i;
    for (i = 0; i < NB; i++) {
        uint32_t m = 1u << i, v = (om_lit_b & m) ? 1u : om_glow ? 2u : 0u;
        uint32_t was = (om_sent_b & m) ? 1u : om_sent_glow ? 2u : 0u;
        if (all || v != was)
            H->led(OM_BTN[i], v);
    }
    for (i = 0; i < NKEYS; i++) {
        uint32_t m = 1u << i;
        if (all || ((om_lit_k ^ om_sent_k) & m))
            H->led(HB_LED_KEY(i), (om_lit_k & m) ? 1u : 0u);
    }
    om_sent_b = om_lit_b;
    om_sent_k = om_lit_k;
    om_sent_glow = om_glow;
}
void plat_leds(uint32_t buttons, uint32_t keys)
{
    om_lit_b = buttons & ((1u << NB) - 1u);
    om_lit_k = keys;
    om_lights(0);
}
void plat_glow(int on)
{
    om_glow = on != 0;
    om_lights(0);
}

/* ---- MIDI: USB-MIDI packets (byte 0 the code index, then status, data 1, data 2) */
int plat_midi_in(uint32_t *pkt)
{
    uint8_t st, d1, d2;
    if (!midi_get(&st, &d1, &d2))
        return 0;
    *pkt = (uint32_t)(st >> 4) | (uint32_t)st << 8 | (uint32_t)d1 << 16 | (uint32_t)d2 << 24;
    return 1;
}
void plat_midi_out(uint32_t pkt) { H->midi_send(pkt >> 8 & 0xFFu, pkt >> 16 & 0xFFu, pkt >> 24 & 0xFFu); }

/* ---- the store: the project (OBJ_PROJ) in the module's first bank (8 KiB); the panel calibration is the host's */
static int om_banks(void) { return H->size >= (uint32_t)((const char *)&H->bank_free - (const char *)H) + sizeof H->bank_free; }
int plat_store_load(uint32_t obj, void *dst, uint32_t max)
{
    if (obj != OBJ_PROJ || !om_banks())
        return -1;
    return H->bank_load_n(1u, dst, max);
}
int plat_store_save(uint32_t obj, const void *src, uint32_t len)
{
    if (obj != OBJ_PROJ || !om_banks() || len > PLAT_STORE_MAX)
        return -1;
    return H->bank_save_n(1u, src, len) ? -1 : 0;
}

/* ---- the audio load: the render's own time against its block's (256 frames at 44.1 kHz: 5805 us) */
static volatile uint32_t om_load;
uint32_t plat_cpu_pct(void) { return om_load; }

void om_midi_out(uint32_t st, uint32_t d1, uint32_t d2) { H->midi_send(st, d1, d2); }   /* (plat_fm1.c's) */

/* ---- the screen: gfx.c's canvas onto the host's (the colours as RGB565 to fill, the canvas already as the panel
 * takes it to blit) */
static void lcd_sync(void) {}
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c) { H->fill((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, c); }
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px) { H->blit((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, px); }
#include "gfx.c"

/* ---- the screen reader (fm1-base's announce.c, over USB): not in a module */
#define ANN_MAX 240u
#define ANN_NOW 1u
#define ANN_KEEP 2u
static int ann_on(void) { return 0; }
static void ann_say(const char *a, const char *b, uint32_t flags) { (void)a; (void)b; (void)flags; }
static void ann_later(const char *s, uint32_t flags, uint32_t quiet_ms, uint32_t now) { (void)s; (void)flags; (void)quiet_ms; (void)now; }
static int ann_where(void) { return 0; }
static void ann_tick(uint32_t now) { (void)now; }
