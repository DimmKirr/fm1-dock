#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Scintilla (joakimeriksson/scintilla-fm1) into this module: its engine as it is (engine/), its app
(firmware/firmware/src/scintilla.c) with the edits below, each checked (app/scintilla.c).
  modules/scintilla/take.py ~/work/scintilla-fm1
The edits: the platform is sc_platform.h (fm1-dock's, as CS-1's), not Dinghy's platform.c; the DIN jack
is the host's (its MIDI comes through midi()); app_main is split into app_init and app_loop (the
host calls the loop body from draw()), its locals made file statics; the first sound is loaded
straight in app_init (the audio is muted while a module starts: load() would wait for a block)."""
import re
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
src = Path(sys.argv[1])
(HERE / "engine").mkdir(exist_ok=True)
for f in (src / "engine").glob("sc_*.[ch]"):
    shutil.copy(f, HERE / "engine" / f.name)
s = (src / "firmware/firmware/src/scintilla.c").read_text()


def rep(old, new):
    global s
    if s.count(old) != 1:
        raise SystemExit(f"take.py: scintilla.c has changed here ({s.count(old)} x): {old[:70]!r}")
    s = s.replace(old, new)


rep('#define APP_EARLY_INIT din_init                 /* the DIN jack\'s UART, before platform.c starts the key scan */\n'
    'static void din_init(void);\n#include "platform.c"\n', '#include "sc_platform.h"                /* (fm1-dock: Dinghy\'s platform calls) */\n')
rep('#ifndef DINGHY_HOST\n#include "fm1_uart.h"', '#if 0                                   /* (fm1-dock: the DIN jack is the host\'s) */\n#include "fm1_uart.h"')
rep('#ifdef DINGHY_HOST                             /* (a Mach-O section needs', '#if 1                                   /* (fm1-dock: the module RAM, all .bss) (a Mach-O section needs')
m = re.search(r"static void app_main\(void\)\n\{\n(.*?)\n\n    settings_load", s, re.S)
if not m:
    raise SystemExit("take.py: app_main's locals have changed")
decls = "\n".join("static " + l.strip() for l in m.group(1).split("\n"))
rep("static void app_main(void)\n{\n" + m.group(1) + "\n\n    settings_load",
    decls + "\n\nstatic void app_init(void)\n{\n    settings_load")
rep("    sc_perf_default(&perf);\n    load(set.preset);\n",
    "    sc_perf_default(&perf);\n    staged = SC_PRESETS[set.preset % SC_N_PRESETS];   /* (fm1-dock: muted while it starts) */\n"
    "    sc_init(&syn, &staged);\n    syn.perf = perf;\n    sc_post(&syn, SC_EV_PATCH, 0, 0);\n    ready = 1;\n")
rep("    lcd_fill(0, 0, 240, 240, C_BLACK);\n\n    for (;;) {\n",
    "    lcd_fill(0, 0, 240, 240, C_BLACK);\n}\n\nstatic void app_loop(void)\n{\n    {\n")
(HERE / "app").mkdir(exist_ok=True)
(HERE / "app" / "scintilla.c").write_text(s)
print("modules/scintilla: engine/ and app/scintilla.c taken from", src)
