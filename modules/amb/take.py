#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""modules/amb/amb/ from AMB-1's own tree (github.com/charlesvestal/fm1-amb, with its base/ submodule, fm1-base): its
app, UI, pieces and engine unchanged (MPL-2.0, file by file), and from fm1-base the platform interface (plat.h), its
maths (fastmath.h), its drawing (gfx.c) and its fonts (felucca_font.h, made by fm1-base's tools/gen_font.py: Pillow).
One change for a module's RAM: the .pool section (fm1-base's second RAM region) is gone, a module's RAM is one region
the host clears. The platform under it is fm1-dock's: amb_platform.inc. Run it again on a new AMB-1:
    modules/amb/take.py ~/src/fm1-amb [FONT_H]        (AMB-1 7797f4e, fm1-base 934e40d)
gen_font.py needs Pillow with libraqm (Debian's python3-pil and libraqm0 have it; macOS's wheels may not): FONT_H is
its output made elsewhere, e.g.
    docker run --rm -v "$PWD:$PWD" -w ~/src/fm1-amb debian:bookworm-slim sh -c \
      'apt-get update && apt-get install -y python3-pil libraqm0 && python3 base/tools/gen_font.py /tmp/f.h && cp /tmp/f.h .'"""
import shutil
import subprocess
import sys
from pathlib import Path

src = Path(sys.argv[1])
base = src / "base"
out = Path(__file__).resolve().parent / "amb"
out.mkdir(exist_ok=True)
for f in ["app/app.c", "app/app.h", "app/project.c", "app/ui.c", "app/dsp/amb.c", "app/dsp/amb.h"]:
    shutil.copy(src / f, out / Path(f).name)
for f in ["include/plat.h", "include/fastmath.h", "device/gfx.c"]:
    shutil.copy(base / f, out / Path(f).name)
for f, to in [("LICENSE", "LICENSE"), ("LICENSING.md", "LICENSING.md"),
              ("base/assets/fonts/Barlow-OFL.txt", "Barlow-OFL.txt"),
              ("base/assets/fonts/Terminus-LICENSE.txt", "Terminus-LICENSE.txt")]:
    shutil.copy(src / f, out / to)
if len(sys.argv) > 2:
    shutil.copy(sys.argv[2], out / "felucca_font.h")
else:
    subprocess.run([sys.executable, str(base / "tools/gen_font.py"), str(out / "felucca_font.h")], check=True)


def sub(name, old, new, count=1):
    t = (out / name).read_text()
    assert t.count(old) == count, (name, old, t.count(old))
    (out / name).write_text(t.replace(old, new))


POOL = '__attribute__((section(".pool")))'
sub("plat.h", "#define PLAT_POOL " + POOL, "#define PLAT_POOL                      /* fm1-dock: one RAM region (take.py) */")
sub("amb.c", "#define EN_POOL " + POOL, "#define EN_POOL                         /* fm1-dock: one RAM region (take.py) */")
sub("gfx.c", "static uint16_t cv_px[CV_MAX] " + POOL + ";", "static uint16_t cv_px[CV_MAX];   /* fm1-dock: one RAM region (take.py) */")
# two things for a module's 272 KiB of RAM (AMB-1 on fm1-base has more), both settable and off in AMB-1's own build:
# the tape loop's length TAPE_N (its echo times already keep to it: the longest loop shorter), and the plate's delay
# lines in 16 bits (PL_Q: a float's 4 bytes to 2, as the tape keeps its samples)
sub("amb.c", "#define TAPE_N 62000                             /* 5.6 s at 11.025 kHz */",
    "#ifndef TAPE_N                                   /* fm1-dock: settable (take.py) */\n"
    "#define TAPE_N 62000                             /* 5.6 s at 11.025 kHz */\n#endif")
sub("amb.c", """static float pl_buf[DL_TOTAL] EN_POOL;
typedef struct {
    float *b;
    int n, i;
} dl_t;""", """#ifdef PL_Q                                       /* fm1-dock: the delay lines in 16 bits, PL_Q a unit (take.py) */
typedef int16_t pl_t;
static inline float pl_ld(pl_t v) { return (float)v * (1.0f / (float)PL_Q); }
static inline pl_t pl_st(float x)
{
    x *= (float)PL_Q;
    return (pl_t)(x > 32767.0f ? 32767.0f : x < -32767.0f ? -32767.0f : x + (x < 0.0f ? -0.5f : 0.5f));
}
#else
typedef float pl_t;
#define pl_ld(v) (v)
#define pl_st(x) (x)
#endif
static pl_t pl_buf[DL_TOTAL] EN_POOL;
typedef struct {
    pl_t *b;
    int n, i;
} dl_t;""")
sub("amb.c", """    return d->b[j < 0 ? j + d->n : j];
}""", """    return pl_ld(d->b[j < 0 ? j + d->n : j]);
}""")
sub("amb.c", """    d->b[d->i] = x;
    if (++d->i >= d->n)""", """    d->b[d->i] = pl_st(x);
    if (++d->i >= d->n)""")
sub("amb.c", """    float *p = pl_buf;
    int k, i;""", """    pl_t *p = pl_buf;
    int k, i;""")
sub("amb.c", """        pl_buf[i] = 0.0f;""", """        pl_buf[i] = 0;""")
for f in out.glob("*.[ch]"):
    assert POOL not in f.read_text(), f
print("amb/: taken from", src)
