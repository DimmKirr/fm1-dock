#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""harbor's boot slot (firmware/harbor/hb_module.h): a whole firmware that harbor starts at
power-on in its place (HOME held at power-on: harbor).

  tools/hbboot.py relink SRC        a Felucca-family checkout made to run from the slot (its app.ld,
                                    hal/fm1_guard.h, tools/build.py; again: nothing changes)
  tools/hbboot.py make APP.bin --name FELUCCA [--version 161] [-o OUT.hrbr]
                                    its app.bin (built after relink) as a boot file
  then: tools/harbor.py deploy OUT.hrbr (USB), or into fm1-emu: tools/harbor.py emu --fwsc ... --deploy

A boot firmware has the FM-1 to itself and keeps its own flash areas (Felucca's: 0x97000 up);
harbor keeps the table and its settings at 0x93000..0x96000, which Felucca leaves alone. Deployed
modules and a boot firmware exclude each other: the modules' flash is the boot firmware's data."""
import argparse
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hbmod  # noqa: E402

HB_F_BOOT = 0x200
BOOT_LO, BOOT_IMG, BOOT_HI = 0x14000, 0x15000, 0x93000
XIP = 0x02000000 + BOOT_IMG - 0x4000            # where the image runs: 0x02011000


def make(app, name, version):
    if len(app) > BOOT_HI - BOOT_IMG:
        raise SystemExit(f"{len(app)} B: the boot slot takes {BOOT_HI - BOOT_IMG} B")
    h = {"magic": hbmod.HB_MAGIC, "abi": hbmod.HB_ABI, "flags": HB_F_BOOT, "name": name, "version": version,
         "code_size": len(app), "data_size": 0, "bss_size": 0, "rt_size": 0, "worst_cycles": 0, "desc": 0,
         "base": XIP, "n_relocs": 0, "reserved": 0, "crc": 0}
    blob = hbmod.pack(h, [], app)
    assert len(blob) == 256 + len(app)
    return blob


EDITS = [   # (file, the stock text, the slot's): the Felucca-family layout (Felucca 1.6, SLOOP, ...)
    ("firmware/app.ld", re.compile(r"XIP    \(rx\)  : ORIGIN = 0x02000120, LENGTH = 0x[0-9A-Fa-f]+"),
     f"XIP    (rx)  : ORIGIN = {XIP:#010x}, LENGTH = {BOOT_HI - BOOT_IMG:#x}"),
    ("firmware/hal/fm1_guard.h", "_sstack_top[], _etext[];", "_sstack_top[], _etext[], _start[];"),
    ("firmware/hal/fm1_guard.h", "FM1_PC_LIMIT0_L = 0x02000120u;", "FM1_PC_LIMIT0_L = (uint32_t)(uintptr_t)_start;"),
    ("firmware/hal/fm1_guard.h", "re > rs ? rs : 0x02000120u;", "re > rs ? rs : (uint32_t)(uintptr_t)_start;"),
    ("tools/build.py", "APP_XIP = 0x02000120 ", f"APP_XIP = {XIP:#010x} "),
]


def relink(src):
    src = Path(src)
    for rel, old, new in EDITS:
        p = src / rel
        s = p.read_text()
        if new in s:
            continue
        s2 = old.sub(new, s, count=1) if hasattr(old, "sub") else s.replace(old, new, 1)
        if s2 == s:
            raise SystemExit(f"{p}: not the layout this knows ({old if isinstance(old, str) else old.pattern})")
        p.write_text(s2)
        print(f"  {rel}: {new}")
    left = [str(p.relative_to(src)) for p in (src / "firmware").rglob("*.[chS]") if "0x02000120" in p.read_text()]
    if left:
        print(f"  still naming 0x02000120 (check them): {', '.join(left)}")


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("relink")
    r.add_argument("src")
    m = sub.add_parser("make")
    m.add_argument("app")
    m.add_argument("--name", required=True)
    m.add_argument("--version", type=int, default=1)
    m.add_argument("-o", "--out")
    a = ap.parse_args()
    if a.cmd == "relink":
        relink(a.src)
        return
    app = Path(a.app).read_bytes()
    blob = make(app, a.name, a.version)
    out = Path(a.out or Path(a.app).with_suffix(".boot.hrbr"))
    out.write_bytes(blob)
    h, _, _ = hbmod.parse(blob)
    print(f"boot {a.name}: {out}, {len(app)} B at XIP {XIP:#010x}, crc {h['crc']:08x}; "
          f"{BOOT_HI - BOOT_IMG - len(app)} B of the slot left")


if __name__ == "__main__":
    main()
