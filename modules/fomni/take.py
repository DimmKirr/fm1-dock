#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""modules/fomni/fomni/ from FoMni's own tree (github.com/charlesvestal/fm1-fomni): its app (app/: the project, the
UI, its platform interface plat.h), its engine (dsp/: the harp, the chord section, the bass, the rhythm and its drums,
the plate) and its drawing (gfx.c) unchanged, and its fonts (felucca_font.h, made by FoMni's tools/gen_font.py:
Pillow with libraqm). One change for a module's RAM: the .pool section (the firmware's second RAM region) is gone,
a module's RAM is one region the host clears. The platform under it is fm1-dock's: fomni_platform.inc. Run it again on
a new FoMni:
    modules/fomni/take.py ~/src/fm1-fomni [FONT_H]        (FoMni 7e7547f)
FONT_H: gen_font.py's output made elsewhere when this Python's Pillow has no libraqm (see modules/amb/take.py)."""
import shutil
import subprocess
import sys
from pathlib import Path

src = Path(sys.argv[1])
fw = src / "firmware/src"
out = Path(__file__).resolve().parent / "fomni"
for d in ["app", "dsp"]:
    (out / d).mkdir(parents=True, exist_ok=True)
for f in ["app/app.h", "app/plat.h", "app/project.c", "app/ui.c", "dsp/omni.c", "dsp/omni.h", "dsp/fastmath.h",
          "dsp/om_data.h", "dsp/om_drums.h", "gfx.c"]:
    shutil.copy(fw / f, out / f)
for f, to in [("LICENSE", "LICENSE"), ("LICENSING.md", "LICENSING.md"),
              ("assets/fonts/Barlow-OFL.txt", "Barlow-OFL.txt"), ("assets/fonts/Terminus-LICENSE.txt", "Terminus-LICENSE.txt")]:
    shutil.copy(src / f, out / to)
if len(sys.argv) > 2:
    shutil.copy(sys.argv[2], out / "felucca_font.h")
else:
    subprocess.run([sys.executable, str(src / "tools/gen_font.py"), str(out / "felucca_font.h")], check=True)


def sub(name, old, new):
    t = (out / name).read_text()
    assert t.count(old) == 1, (name, old, t.count(old))
    (out / name).write_text(t.replace(old, new))


POOL = '__attribute__((section(".pool")))'
sub("app/plat.h", "#define OM_POOL " + POOL, "#define OM_POOL                        /* fm1-dock: one RAM region (take.py) */")
sub("dsp/omni.c", "#define OM_POOL " + POOL, "#define OM_POOL                        /* fm1-dock: one RAM region (take.py) */")
sub("gfx.c", "static uint16_t cv_px[CV_MAX] " + POOL + ";", "static uint16_t cv_px[CV_MAX];   /* fm1-dock: one RAM region (take.py) */")
for f in out.rglob("*.[ch]"):
    assert POOL not in f.read_text(), f
print("fomni/: taken from", src)
