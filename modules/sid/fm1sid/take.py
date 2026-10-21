#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""modules/sid/fm1sid.c from FM-1 SID's firmware/main.c: its synth mode's code by line ranges, the
few changes for harbor checked one by one (each must match exactly once).
    modules/sid/fm1sid/take.py ~/work/fm1-emu/fm1-sid/firmware/main.c     (fm1-emu 7856393)"""
import sys
from pathlib import Path

L = Path(sys.argv[1]).read_text().split("\n")
HERE = Path(__file__).resolve().parent


def rng(a, b):                                   # 1-based, inclusive
    return "\n".join(L[a - 1:b])


def sub(text, old, new):
    assert text.count(old) == 1, (old[:70], text.count(old))
    return text.replace(old, new)


synth_state = rng(126, 167)
synth_state = sub(synth_state, "#define HALF_CYC 2860u                      /* SID cycles in a 128-frame half buffer, rounded up */",
                  "#define HALF_CYC 5720u                      /* harbor: SID cycles in a 256-frame block at 44 100 Hz, rounded up */")
on_write = rng(170, 180)
synth = rng(191, 396)
synth = sub(synth, "    if (engine)\n        return own.v[voice].env;\n", "")
apply_ = rng(400, 427)
apply_ = sub(apply_, "    if (engine)\n        sid_write(&own, e->reg, e->val);\n    else\n        rs_write(e->reg, e->val);",
             "    rs_write(e->reg, e->val);")
gfx = rng(515, 515) + "\n\n" + rng(564, 659)   # (fb; FM-1 SID's own LCD driver, 517-562, is the host's)
gfx = sub(gfx, '#include "fx.c"', '#include "fm1sid/fx.c"')
seqdraw = rng(661, 746)
draw = rng(748, 885)
draw = sub(draw, """    if (engine) {
        env[0] = own.v[0].env; env[1] = own.v[1].env; env[2] = own.v[2].env;
    } else {
        rs_env(env);
    }""", "    rs_env(env);")
draw = sub(draw, "    const psid_info_t *info = psid_info(P);\n", "")
draw = sub(draw, '    text(240 - 8 * 13 - 2, 1, engine ? "fm1-sid model" : "  reSID  0.16", C_YELLOW);',
           '    text(240 - 8 * 13 - 2, 1, "  reSID  0.16", C_YELLOW);')
i = draw.index("    } else {                               /* the tune */")
j = draw.index("    /* CPU: the audio ISR's share of its period */")
draw = draw[:i] + "    }\n" + draw[j:]                 # (the player's tune lines)
synth2 = rng(917, 1052)
synth2 = sub(synth2, """    running = 0;
    fm1_delay_ms(4);
    mode = 1;
    if (engine)
        sid_init(&own, model ? SID_8580 : SID_6581, SID_CLOCK_PAL, FS_HZ);
    else
        rs_init(model, 985248.0, 24000000.0 / 544.0);""", """    running = 0;
    block_wait();                           /* harbor: the block that may be running ends */
    mode = 1;
    rs_init(model, 985248.0, (double)FS_HZ);""")

head = (HERE / "head.c").read_text()
tail = (HERE / "tail.c").read_text()
out = "\n\n".join([head.rstrip(), synth_state, on_write, synth, apply_, gfx, seqdraw, draw, synth2, tail.rstrip()]) + "\n"
(HERE.parent / "fm1sid.c").write_text(out)
print(f"{HERE.parent / 'fm1sid.c'}: {len(out.splitlines())} lines")
