#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Pack firmware/gen/felucca_font.h's glyphs from 4 to 1 bit per pixel (MSB first, rows of
(w + 7) / 8 bytes): its fonts are bitmap fonts (0 or 15 only), so nothing is lost, and the host
is 25 KB smaller (room for the boot slot, hb_module.h). gfx.c cv_text reads them so.
  tools/font_1bpp.py firmware/gen/felucca_font.h"""
import re
import sys
from pathlib import Path

p = Path(sys.argv[1])
s = p.read_text()
if "1 bit per pixel" in s:
    raise SystemExit(f"{p}: packed already")


def nums(body):
    return [int(x, 0) for x in re.findall(r"0x[0-9a-fA-F]+|\d+", body)]


for f in ("S", "L"):
    data = bytes(nums(re.search(rf"FONT_{f}_DATA\[\d+\] = \{{(.*?)\}};", s, re.S).group(1)))
    off = nums(re.search(rf"FONT_{f}_OFF\[\d+\] = \{{(.*?)\}};", s, re.S).group(1))
    bw = nums(re.search(rf"FONT_{f}_BW\[\d+\] = \{{(.*?)\}};", s, re.S).group(1))
    h = nums(re.search(rf"felucca_font_t FONT_{f} = \{{ *(\d+),", s).group(1))[0]
    out, new_off = bytearray(), []
    for g, w in enumerate(bw):
        src, bpr4, bpr1 = off[g], (w + 1) // 2, (w + 7) // 8
        new_off.append(len(out))
        for y in range(h):
            row = bytearray(bpr1)
            for x in range(w):
                v = data[src + y * bpr4 + x // 2]
                v = v & 15 if x & 1 else v >> 4
                if v not in (0, 15):
                    raise SystemExit(f"FONT_{f}: glyph {g} has grey ({v}): not a bitmap font")
                if v:
                    row[x // 8] |= 0x80 >> (x & 7)
            out += row
    lines = ",\n".join("    " + ", ".join(f"0x{b:02x}" for b in out[i:i + 24]) for i in range(0, len(out), 24))
    s = re.sub(rf"FONT_{f}_DATA\[\d+\] = \{{.*?\}};", f"FONT_{f}_DATA[{len(out)}] = {{\n{lines}}};", s, flags=re.S)
    s = re.sub(rf"FONT_{f}_OFF\[(\d+)\] = \{{.*?\}};", lambda m: f"FONT_{f}_OFF[{m.group(1)}] = {{{', '.join(map(str, new_off))}}};", s, flags=re.S)
    print(f"FONT_{f}: {len(data)} -> {len(out)} B")
s = s.replace("#pragma once", "/* packed to 1 bit per pixel by tools/font_1bpp.py (gfx.c cv_text) */\n#pragma once", 1)
p.write_text(s)
