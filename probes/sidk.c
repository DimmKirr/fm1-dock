/* SPDX-License-Identifier: GPL-3.0-only */
/* SIDK: FM-1 SID's own SID model (probes/fm1sid: a copy of fm1-sid's src/sid.c) on the FM-1 and in
 * fm1-emu: oscillators and the filter in five set-ups, 512 samples each at 44.1 kHz after a warm-up,
 * cycles (240 MHz) a sample and a checksum of the output, to check the emulator gives the device's
 * result and its cost. argument: none */
/* hbmod-cflags: -mcpu=r3 -mfprev1 -Iprobes/fm1sid */
#include "probe.h"
#include "fm1sid/sid.c"

static sid_t sk_sid;
static int32_t sk_out[512];

static void sk_regs(const uint8_t *r, uint32_t n) { uint32_t i; for (i = 0; i + 1 < n; i += 2) sid_write(&sk_sid, r[i], r[i + 1]); }

static void sk_case(hb_probe_ctx_t *c, const char *name, sid_model_t model, const uint8_t *r, uint32_t n)
{
    uint32_t t, i, sum = 0, w[2];
    sid_init(&sk_sid, model, SID_CLOCK_PAL, 44100);
    sk_regs(r, n);
    sid_render(&sk_sid, sk_out, 512);          /* (warm: the envelopes up) */
    p_irq_off();
    t = p_ticks();
    for (i = 0; i < 4u; i++) sid_render(&sk_sid, sk_out + 128u * i, 128);   /* (as the firmware: 128 a call) */
    t = p_ticks() - t;
    p_irq_on();
    for (i = 0; i < 512u; i++) sum = sum * 31u + (uint32_t)sk_out[i];
    w[0] = t * 10u * 100u / 512u;              /* (cycles x 100 a sample) */
    w[1] = sum;
    p_words(c, name, w, 2);
    c->wdt_feed();
}

/* register, value pairs: voice 1 at 0..6, voice 2 at 7..13, voice 3 at 14..20, filter 21..24 */
static const uint8_t SAW[] = {0, 0xD6, 1, 0x1C, 5, 0x00, 6, 0xF0, 24, 0x0F, 4, 0x21};
static const uint8_t PULSE_LP[] = {0, 0xD6, 1, 0x0E, 2, 0x00, 3, 0x08, 5, 0x00, 6, 0xF0, 21, 0x00, 22, 0x30,
                                   23, 0xF1, 24, 0x1F, 4, 0x41};
static const uint8_t NOISE_HP[] = {0, 0x00, 1, 0x40, 5, 0x00, 6, 0xF0, 21, 0x00, 22, 0x60, 23, 0x41, 24, 0x4F, 4, 0x81};
static const uint8_t THREE_BP[] = {0, 0xD6, 1, 0x1C, 5, 0x00, 6, 0xF0, 7, 0x61, 8, 0x15, 9, 0x00, 10, 0x06,
                                   12, 0x00, 13, 0xF0, 14, 0x6B, 15, 0x0E, 19, 0x00, 20, 0xF0, 21, 0x00,
                                   22, 0x50, 23, 0x87, 24, 0x2F, 4, 0x21, 11, 0x41, 18, 0x11};
static const uint8_t COMBINED[] = {0, 0xD6, 1, 0x1C, 5, 0x00, 6, 0xF0, 24, 0x0F, 4, 0x31};

static int32_t run(hb_probe_ctx_t *c)
{
    sk_case(c, "saw_6581", SID_6581, SAW, sizeof SAW);
    sk_case(c, "pulse_lp_res_6581", SID_6581, PULSE_LP, sizeof PULSE_LP);
    sk_case(c, "pulse_lp_res_8580", SID_8580, PULSE_LP, sizeof PULSE_LP);
    sk_case(c, "noise_hp_6581", SID_6581, NOISE_HP, sizeof NOISE_HP);
    sk_case(c, "three_voices_bp_6581", SID_6581, THREE_BP, sizeof THREE_BP);
    sk_case(c, "saw_tri_combined_6581", SID_6581, COMBINED, sizeof COMBINED);
    return 0;
}

HB_PROBE = {HB_PROBE_MAGIC, "SIDK", run};
