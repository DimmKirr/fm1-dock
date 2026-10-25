// modules/sid/resid_glue.cc - reSID 0.16 (Dag Lem, GPL-2.0-or-later, resid/) for the SID module:
// FM-1 SID's firmware/resid_glue.cc (fm1-emu 7856393), one chip in static storage, C entry points.
// fm1-dock: no allocator (fast sampling allocates nothing: the resampling path, never taken here,
// would get no memory), and reSID's sid.h by its path (modules/sid/sid.h is FM-1 SID's own model's).
// SPDX-License-Identifier: GPL-3.0-only
#include "resid/sid.h"

void *operator new(__SIZE_TYPE__) { return 0; }
void *operator new[](__SIZE_TYPE__) { return 0; }
void operator delete(void *) {}
void operator delete[](void *) {}
inline void *operator new(__SIZE_TYPE__, void *p) { return p; }
extern "C" void __cxa_pure_virtual() { for (;;) {} }

static SID *chip;
static unsigned char store[sizeof(SID)] __attribute__((aligned(8)));

extern "C" void rs_init(int model, double clock, double fs)
{
    chip = new (store) SID();
    chip->set_chip_model(model ? MOS8580 : MOS6581);
    chip->enable_filter(true);
    chip->enable_external_filter(true);
    chip->set_sampling_parameters(clock, SAMPLE_FAST, fs);
}

extern "C" void rs_write(int reg, int val) { chip->write(reg, val); }

// clock up to *cycles, producing at most n samples; *cycles is what is left
extern "C" int rs_clock(int *cycles, short *buf, int n)
{
    cycle_count dt = *cycles;
    int got = chip->clock(dt, buf, n);
    *cycles = dt;
    return got;
}

// the voices' envelope levels (0..255), for the display
extern "C" void rs_env(unsigned char env[3])
{
    SID::State st = chip->read_state();
    env[0] = st.envelope_counter[0];
    env[1] = st.envelope_counter[1];
    env[2] = st.envelope_counter[2];
}
