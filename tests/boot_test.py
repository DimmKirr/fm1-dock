#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""fm1-dock's boot slot in fm1-emu (fm1-live --fwsc, --flash; its SFC model decrypts what fm1-dock writes
below the plain window): a whole firmware deployed over DIN, then power-ons from that flash.
  tools/build.py && tests/boot_test.py BOOT.dock     (tools/hbboot.py: e.g. Felucca relinked)
  - install: the boot firmware into the DIN jack: written encrypted, read back through the SFC, in the table;
  - boot: at power-on fm1-dock hands over (the start counted), the firmware runs and plays a key;
  - home: HOME held at power-on: fm1-dock stays, the firmware in its list;
  - damaged: a bit flipped in the firmware's flash: fm1-dock stays (the CRC through the SFC);
  - no_modules: a module deploy is refused while a boot firmware is in (its data is where they go);
  - delete: DELETE takes it out of the table: the next power-on is fm1-dock's."""
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

SRC = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SRC / "tools"))
import dock  # noqa: E402
import hbdeploy as hd  # noqa: E402
import hbmod  # noqa: E402

FWSC = SRC / "build" / "fm1-dock.fwsc"
failures = []


def check(case, ok, why):
    print(f"  {'ok  ' if ok else 'FAIL'} {case}: {why}")
    if not ok:
        failures.append(case)


def live(ms, keys, flash, wd):
    (wd / "work").mkdir(exist_ok=True)
    (wd / "keys.txt").write_text(",".join(keys))
    ram = wd / "ram.bin"
    env = dict(os.environ, FM1_BENCH_KEYS="@" + str(wd / "keys.txt"), FM1_DUMPRAM=str(ram))
    r = subprocess.run([str(dock.live()), "--fwsc", str(FWSC), "--shot", str(ms), "--flash", str(flash)],
                       cwd=wd, capture_output=True, text=True, env=env)
    if r.returncode:
        raise SystemExit(f"fm1-live: {r.stdout[-1500:]}{r.stderr[-1500:]}")
    out = r.stdout + r.stderr
    peak = int(out.split("audio peak over the last 0.5 s: ")[1].split()[0]) if "audio peak" in out else 0
    ram = ram.read_bytes()
    return dock.read_log(ram), struct.unpack_from("<2I", ram, 0x7FD40), peak


def main():
    blob = Path(sys.argv[1]).read_bytes()
    name = hbmod.parse(blob)[0]["name"]
    wd = Path(tempfile.mkdtemp(prefix="hb-boot-"))
    try:
        base = wd / "installed.bin"
        keys, done = hd.din_script(blob, 600)
        log, _, _ = live(int(done) + 1500, keys, base, wd)
        check("install", f"deploy: boot firmware at {0x02010F00:08x}" in log and "boot slot ok" in log and name in log,
              f"{name} ({len(blob) // 1024} KiB) over the DIN jack in {done / 1000:.0f} s, read back through the SFC")

        def run(case, keys, ms=4000, edit=None):
            f = wd / f"{case}.bin"
            shutil.copy(base, f)
            if edit:
                edit(f)
            return live(ms, keys, f, wd) + (f,)

        log, go, peak, _ = run("boot", ["2500:21", "3500:21^"])
        check("boot", go[1] == 1 and log.startswith("no fm1-dock state") and peak > 500,
              f"handed over (start {go[1]}), {name} runs (fm1-dock's NOINIT is its now) and plays (peak {peak})")

        log, go, peak, _ = run("home", ["0:8", "1500:8^", "2500:21", "3500:21^"])
        check("home", go[1] == 0 and "boot firmware not started 00000001" in log and name in log and peak > 500,
              "HOME held at power-on: fm1-dock stays, the sine plays, the boot firmware is in its list")

        def flip(f):
            b = bytearray(f.read_bytes())
            b[0x40000] ^= 1
            f.write_bytes(b)
        log, go, peak, _ = run("damaged", ["2500:21", "3500:21^"], edit=flip)
        check("damaged", go[1] == 0 and "boot slot refused 00000001" in log and peak > 500,
              "a bit flipped in its flash: BAD CRC through the SFC, fm1-dock stays")

        sine = (SRC / "build" / "modules" / "sine.dock").read_bytes()
        k2, d2 = hd.din_script(sine, 700)
        log, go, peak, f = run("no_modules", ["0:8", "600:8^", *k2], ms=int(d2) + 1500)
        check("no_modules", "deploy failed 00000004" in log, "a module deploy: NO SPACE while a boot firmware is in")

        delete = hd.frame(hd.DELETE, 1, hd.name24(name))
        log, go, peak, f = run("delete", ["0:8", "600:8^", f"900:m{delete.hex()}"], ms=1800)
        log2, go2, peak2 = live(4000, ["2500:21", "3500:21^"], f, wd)
        check("delete", "deploy: deleted" in log and go2[1] == 0 and name not in log2 and peak2 > 500,
              "DELETE: out of the table; the next power-on is fm1-dock's, the sine plays")
    finally:
        shutil.rmtree(wd, ignore_errors=True)
    print("all passed" if not failures else f"FAILED: {', '.join(failures)}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
