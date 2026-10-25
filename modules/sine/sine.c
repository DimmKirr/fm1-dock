/* SPDX-License-Identifier: GPL-3.0-only */
/* SINE: Dinghy's 4-voice sine as a module. The host also builds this file in as its fallback
 * (firmware/src/dock.c, with HB_DESC set), so a module that fails always leaves a sound.
 *   keys / MIDI   4 voices, the velocity sets the level
 *   KNOB1         attack, KNOB2 release */
#include "hb_module.h"

static const hb_host_t *H;

#define NV 4u                                   /* voices */
static struct {
    volatile uint32_t inc;                      /* phase step per frame: f * 2^32 / 44100 */
    volatile int32_t gate;                      /* target level, Q15: 0 = released */
    int32_t amp;                                /* the level now: glides to gate, no clicks */
    uint32_t ph, age;
    uint8_t note;
} voice[NV];
static volatile int32_t att = 48, rel = 6;      /* level steps per frame */
static uint32_t ages, last_note = 255, drawn_note, drawn_env;

static int32_t sine(uint32_t ph)                /* a full cycle over 2^32 -> -32767..32767 (a refined parabola, ~0.1 %) */
{
    int32_t x = (int32_t)ph >> 16, ax = x < 0 ? -x : x, y, ay;
    y = (x * (32768 - ax)) >> 13;               /* 4x(1-|x|), Q15 */
    ay = y < 0 ? -y : y;
    y += ((((y * ay) >> 15) - y) * 7373) >> 15; /* + 0.225 (y|y| - y) */
    return y > 32767 ? 32767 : y < -32767 ? -32767 : y;
}

static void render(int32_t *out, uint32_t n)
{
    uint32_t v, i;
    int32_t a = att, r = rel;
    for (v = 0; v < NV; v++) {
        uint32_t ph = voice[v].ph, step = voice[v].inc;
        int32_t amp = voice[v].amp, g = voice[v].gate;
        if (!amp && !g)
            continue;                           /* silent: costs nothing */
        for (i = 0; i < n; i++) {
            int32_t s;
            if (amp < g) { amp += a; if (amp > g) amp = g; }
            else if (amp > g) { amp -= r; if (amp < g) amp = g; }
            ph += step;
            s = (sine(ph) * amp) >> 15;
            out[2u * i] += s;                   /* L */
            out[2u * i + 1u] += s;              /* R */
        }
        voice[v].ph = ph;
        voice[v].amp = amp;
    }
}

static const uint32_t INC_C9[12] = {            /* MIDI 120..131 (C9..B9); lower octaves shift right */
    815363807u, 863847862u, 915214929u, 969636441u, 1027294024u, 1088380105u,
    1153098554u, 1221665363u, 1294309365u, 1371273005u, 1452813141u, 1539201906u};

static void note(uint32_t ch, uint32_t key, uint32_t vel)
{
    uint32_t v, pick = 0, best = 0xFFFFFFFFu;
    (void)ch;
    if (key > 127u)
        return;
    if (!vel) {
        for (v = 0; v < NV; v++)
            if (voice[v].note == key)
                voice[v].gate = 0;
        return;
    }
    for (v = 0; v < NV; v++) {                  /* the same note, else a free voice, else the oldest */
        uint32_t score = voice[v].note == key && voice[v].gate ? 0u
                       : voice[v].gate ? 0x80000000u + voice[v].age : (uint32_t)voice[v].amp + 1u;
        if (score < best) { best = score; pick = v; }
    }
    voice[pick].note = (uint8_t)key;
    voice[pick].age = ++ages;
    voice[pick].inc = INC_C9[key % 12u] >> (10u - key / 12u);   /* the pitch before the gate */
    voice[pick].gate = (int32_t)(vel * 90u);    /* 127 -> 11430: four voices stay under full scale */
    last_note = key;
}

static void cc(uint32_t ch, uint32_t num, uint32_t val)
{
    uint32_t v;
    (void)ch; (void)val;
    if (num == 123u)                            /* all notes off */
        for (v = 0; v < NV; v++)
            voice[v].gate = 0;
}

static void ui(uint32_t ev, int32_t arg)
{
    if (HB_EV_KIND(ev) != HB_EV_ENC)
        return;
    if (HB_EV_ID(ev) == HB_ENC_KNOB1) {
        int32_t a = att - arg * 4;
        att = a < 4 ? 4 : a > 400 ? 400 : a;
    } else if (HB_EV_ID(ev) == HB_ENC_KNOB2) {
        int32_t r = rel - arg;
        rel = r < 1 ? 1 : r > 100 ? 100 : r;
    }
}

static void draw(uint32_t full)
{
    static const char *const NAME[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    char s[8] = "-";
    uint32_t env = (uint32_t)att << 16 | (uint32_t)rel;
    if (full) {
        H->fill(0, HB_TOP, 240, 240 - HB_TOP, 0);
        H->text(0, 52, 240, 0, "a sine to build on", HB_RGB(90, 90, 90), 1);
        drawn_note = drawn_env = ~0u;
    }
    if (last_note != drawn_note) {
        drawn_note = last_note;
        if (last_note < 128u) {
            const char *p = NAME[last_note % 12u];
            uint32_t i = 0;
            while (*p) s[i++] = *p++;
            if (last_note >= 12u) s[i++] = (char)('0' + last_note / 12u - 1u);
            s[i] = 0;
        }
        H->text(0, 100, 240, 1, s, 0xFFFFu, 1);
    }
    if (env != drawn_env) {
        char b[24] = "ATT 000 MS  REL 000 MS";  /* the time to full level / to silence */
        uint32_t a = (uint32_t)(11430 / att) * 1000u / 44100u, r = (uint32_t)(11430 / rel) * 1000u / 44100u;
        b[4] = (char)('0' + a / 100u % 10u); b[5] = (char)('0' + a / 10u % 10u); b[6] = (char)('0' + a % 10u);
        b[16] = (char)('0' + r / 100u % 10u); b[17] = (char)('0' + r / 10u % 10u); b[18] = (char)('0' + r % 10u);
        drawn_env = env;
        H->text(0, 200, 240, 0, b, HB_RGB(180, 140, 255), 1);
    }
}

static int init(const hb_host_t *h)
{
    H = h;
    H->log("sine: ready");
    return 0;
}

static void release(void)
{
    uint32_t v;
    for (v = 0; v < NV; v++)
        voice[v].gate = voice[v].amp = 0;
}

static const hb_param_t PARAMS[] = {{"ATTACK", 4, 400, 48}, {"RELEASE", 1, 100, 6}};

HB_DESC = {HB_ABI, "SINE", init, release, note, cc, render, ui, draw, PARAMS, 2, 0, 0};
