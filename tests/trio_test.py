#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""FM-1 SID (a whole firmware, its tunes in its pack) in fm1-dock's boot slot, and CS-1 and Scintilla
deployed as modules beside it (a boot firmware that keeps no flash data: HB_F_BOOT_NODATA), in
fm1-emu, both ways round:
  tests/trio_test.py SID.dock      (tools/hbboot.py make ... --no-data)
  - each deploy over the DIN jack goes in (HOME held at power-on once the boot firmware is in);
  - power-on: fm1-dock hands over to FM-1 SID, which plays;
  - HOME held at power-on: fm1-dock, its list has all three; SELECT CS-1 / SCINTILLA: each plays a key."""
import shutil
import sys
import tempfile
from pathlib import Path

SRC = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SRC / "tools"))
sys.path.insert(0, str(SRC / "tests"))
import boot_test as bt  # noqa: E402
import hbdeploy as hd  # noqa: E402
import hbmod  # noqa: E402

HOME = ["0:8", "600:8^"]


def deploy_keys(blobs, t0):
    keys, t = [], t0
    for b in blobs:
        k, t = hd.din_script(b, t)
        keys += k
        t += 1500
    return keys, t


def main():
    sid = Path(sys.argv[1]).read_bytes()
    sid_name = hbmod.parse(sid)[0]["name"]
    mods = [(SRC / "build" / "modules" / f).read_bytes() for f in ("cs1.dock", "scintilla.dock")]
    wd = Path(tempfile.mkdtemp(prefix="hb-trio-"))
    try:
        for order in ("boot first", "modules first"):
            f = wd / "flash.bin"
            f.unlink(missing_ok=True)
            if order == "boot first":
                k, t = deploy_keys([sid], 600)
                log, _, _ = bt.live(int(t), k, f, wd)
                bt.check(f"{order}: boot", "boot slot ok" in log and sid_name in log, f"{sid_name} in ({len(sid) // 1024} KiB)")
                k, t = deploy_keys(mods, 1200)
                log, _, _ = bt.live(int(t), HOME + k, f, wd)
                bt.check(f"{order}: modules", log.count("deploy: written at") == 2 and "deploy failed" not in log,
                         "CS-1 and SCINTILLA deployed beside it")
            else:
                k, t = deploy_keys(mods, 600)
                log, _, _ = bt.live(int(t), k, f, wd)
                bt.check(f"{order}: modules", log.count("deploy: written at") == 2 and "deploy failed" not in log,
                         "CS-1 and SCINTILLA deployed")
                k, t = deploy_keys([sid], 1200)
                log, _, _ = bt.live(int(t), HOME + k, f, wd)
                bt.check(f"{order}: boot", "deploy: boot firmware at" in log and "deploy failed" not in log,
                         f"{sid_name} in beside them ({len(sid) // 1024} KiB)")
            if order == "boot first":
                # the module deployed last is the one picked: power-on is fm1-dock's, with it; fm1-dock's
                # list (HOME held 1.5 s), PRESETS to the boot firmware, SEL: picked, stored, a reboot
                log, go, peak = bt.live(4000, ["2500:21", "3500:21^"], f, wd)
                ran = log.split("started")[-1].split("\n")[1].split(" ", 1)[-1] if "started" in log else "?"
                bt.check(f"{order}: last pick", go[1] == 0 and ran in ("CS-1", "SCINTILLA") and peak > 500,
                         f"power-on after the deploys: fm1-dock stays, with {ran} (peak {peak})")
                log, go, peak = bt.live(5500, HOME + ["1500:8", "3300:8^", "3600:e6:-10", "3900:e6:1", "4300:3", "4400:3^"], f, wd)
                bt.check(f"{order}: pick {sid_name}", "settings saved" in log, "fm1-dock's list, PRESETS to it, SEL: the pick stored, a reboot")
            log, go, peak = bt.live(4000, ["2500:21", "3500:21^"], f, wd)
            bt.check(f"{order}: power-on", go[1] == 1 and peak > 500, f"{sid_name} runs (start {go[1]}) and plays (peak {peak})")
            for name in ("CS-1", "SCINTILLA"):
                sel = hd.frame(hd.SELECT, 1, hd.name24(name))
                log, go, peak = bt.live(4000, HOME + [f"1200:m{sel.hex()}", "2000:21", "3900:21^"], f, wd)
                ok = go[1] == 0 and "started" in log and name in log and peak > 500 and sid_name in log
                bt.check(f"{order}: {name}", ok, f"HOME at power-on, SELECT {name}: plays a key (peak {peak})")
    finally:
        shutil.rmtree(wd, ignore_errors=True)
    print("all passed" if not bt.failures else f"FAILED: {', '.join(bt.failures)}")
    return 1 if bt.failures else 0


if __name__ == "__main__":
    sys.exit(main())
