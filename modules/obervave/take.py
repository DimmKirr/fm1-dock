#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""modules/obervave/obervave/ from OberVave's own tree (github.com/zvenson/obervave): its app and DSP
unchanged, but for one split: app_main's start becomes app_init(), the body of its forever loop
app_loop() (a do { } while (0), so a `continue` is still "the next pass"), its loop-wide variables
file-level statics; and two things for a module's RAM: its .pool arrays in .bss (a module's RAM is
one region, cleared by the host), the delay's length DLY_MAX settable (module.cfg). Its `#include "platform.c"` finds obervave/platform.c: fm1-dock's (obv_platform.inc
copied in, see there). Run it again on a new OberVave:
    modules/obervave/take.py ~/src/obervave        (OberVave bcddc4b)"""
import shutil
import sys
from pathlib import Path

src = Path(sys.argv[1]) / "firmware"
out = Path(__file__).resolve().parent / "obervave"
out.mkdir(exist_ok=True)
for f in ["src/obv_dsp.h", "src/obv_params.h", "src/obv_fx.h", "src/obv_engine.h", "src/obv_seq.h", "src/obv_patches.h",
          "src/obv_ui.h", "src/gfx.c", "gen/felucca_font.h", "gen/obv_tables.h", "gen/obv_splash.h"]:
    shutil.copy(src / f, out / Path(f).name)
for f in ["LICENSE", "LICENSING.md"]:
    shutil.copy(src.parent / f, out / f)
POOL = ' __attribute__((section(".pool")))'
for f in ["obv_seq.h", "obv_fx.h", "gfx.c"]:
    t = (out / f).read_text()
    assert POOL in t, f
    t = t.replace(POOL, "")
    if f == "obv_fx.h":
        old = '#define DLY_MAX 32768u                          /* a side of the delay: 743 ms */'
        assert t.count(old) == 1
        t = t.replace(old, '#ifndef DLY_MAX                                  /* fm1-dock: settable (modules/obervave/take.py) */\n' + old + '\n#endif')
    (out / f).write_text(t)

s = (src / "src/obervave.c").read_text()


def sub(old, new):
    global s
    assert s.count(old) == 1, (old[:70], s.count(old))
    s = s.replace(old, new)


head = '''static void app_main(void)
{
    uint32_t k, i, t_led = 0, t_scope = 0, t_page = 0, t_hdr = 0, leds_q = 1;
    uint32_t swap_seen = 0, leds_prev = 0xFFFFFFFFu, glow = 0, glow_prev = 0xFFFFFFFFu, t_steps = 0, ticks_seen = 0;
'''
sub(head, '''/* fm1-dock: app_main's loop-wide variables, for app_init and app_loop (modules/obervave/take.py) */
static uint32_t k, i, t_led = 0, t_scope = 0, t_page = 0, t_hdr = 0, leds_q = 1;
static uint32_t swap_seen = 0, leds_prev = 0xFFFFFFFFu, glow = 0, glow_prev = 0xFFFFFFFFu, t_steps = 0, ticks_seen = 0;
static void app_init(void)                      /* fm1-dock: app_main up to its loop */
{
''')
sub('''    ui_splash();
    scope_state = 1;

    for (;;) {
''', '''    ui_splash();
    scope_state = 1;
}

static void app_loop(void)                      /* fm1-dock: a pass of app_main's loop, from the host's draw() */
{
    do {
''')
assert s.rstrip().endswith("    }\n}"), s[-40:]
s = s.rstrip()[:-len("    }\n}")] + "    } while (0);\n}\n"
s = s.replace(' __attribute__((section(".pool")))', "")
(out / "obervave.c").write_text("/* fm1-dock: OberVave's firmware/src/obervave.c, split by modules/obervave/take.py: do not edit */\n" + s)
shutil.copy(Path(__file__).resolve().parent / "obv_platform.inc", out / "platform.c")
print("obervave/: taken from", src.parent)
