/* SPDX-License-Identifier: GPL-3.0-only */
/* SID: FM-1 SID's synth (github.com/joakimeriksson/fm1-emu, fm1-sid/firmware/main.c, 7856393) as a
 * fm1-dock module, with its own screen and controls. Made by modules/sid/fm1sid/take.py: FM-1 SID's
 * code by line ranges, its changes marked "fm1-dock:". Do not edit fm1sid.c: edit head.c / tail.c
 * or take.py and run it again.
 *   - the player of C64 tunes (PSID, the 6502) is not here: this is the synth mode, always on;
 *   - the chip is reSID 0.16 only (resid/, resid_glue.cc); the own model is gone (sid_stub.c);
 *   - its loop body runs from draw(), its audio interrupt from render(), at 44 100 Hz in blocks
 *     of 256 frames (FM-1 SID: 44 118 Hz, 128): FS_HZ, HALF_FRAMES, HALF_CYC are the host's;
 *   - the panel and the time come from the host (modules/lib/hb_fm1.h), the frame goes out by blit;
 *   - HOME (FM-1 SID: the player) and EDIT (the engine) do nothing; OCT- + OCT+ held 3 s (UBOOT)
 *     is the host's business. HOME held 1.5 s opens fm1-dock's module list.
 * Its controls, as FM-1 SID: keys play, PRESETS / SELECT the patch, OCT- / OCT+ the octave,
 * KNOB1..4 cutoff, resonance, attack, release. The step sequencer: SEQ its page, PLAY run / stop,
 * REC record, SCL / ENV / LFO track 1 / 2 / 3 (PRESETS then the track's patch), ALGORITHM the
 * tempo, GLO clear the step, ARP clear the track. FX: the effects page (KNOB1 chorus, KNOB2
 * delay, KNOB3 its time, KNOB4 reverb, SELECT the feedback, PRESETS 6581 / 8580). MIDI notes in
 * play as the keys do. */
#include <stdint.h>
#include "hb_fm1.h"
#include "sid.h"
#include "sidsynth.h"
#include "fm1sid_font.h"

/* reSID (resid_glue.cc) */
void rs_init(int model, double clock, double fs);
void rs_write(int reg, int val);
int rs_clock(int *cycles, short *buf, int n);
void rs_env(unsigned char env[3]);

#define HALF_FRAMES 256u                   /* fm1-dock: the host's block */
#define FS_HZ 44100u                       /* fm1-dock: the host's rate */
#define OUT_SHIFT 0                        /* fm1-dock: the host takes Q15 */
#define PSID_PAL_FRAME 19656u              /* (player/psid.h: 312 lines x 63 cycles) */

/* ------------------------------------------------------------------------------- state --- */
typedef struct { uint32_t cyc; uint8_t reg, val; } ev_t;
#define NEV 1024u                          /* fm1-dock: 1024 (the player needed 4096) */
static ev_t evq[NEV];
static volatile uint32_t ev_head, ev_tail;  /* main loop writes head, the audio ISR tail */
static uint32_t call_base;
static volatile uint32_t play_cyc;          /* audio ISR: the SID's time */
static uint32_t cyc_frac;
static volatile uint8_t running, model;
static volatile uint8_t regs[32];           /* the registers as written, for the display */
static int16_t scope[512];
static volatile uint32_t scope_w;
static volatile uint32_t isr_ticks, isr_halves, blocks;
static void block_wait(void);
