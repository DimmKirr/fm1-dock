#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Probes in fm1-emu (tools/probe.py): sent into the DIN jack of a running harbor, run from RAM.
  tools/build.py && tests/probe_test.py
  - HELLO: the argument back, .data / .bss where the host put them, a relocated pointer, 24 ticks / us;
  - EXEC: instruction bytes assembled on the spot, the registers in and out;
  - TIMING: every case measured, nothing negative;
  - a module file is refused by RUN, a probe file by COMMIT (nothing written)."""
import struct
import subprocess
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SRC / "tools"))
import hbdeploy as hd  # noqa: E402
import probe  # noqa: E402

failures = []


def check(case, ok, why):
    print(f"  {'ok  ' if ok else 'FAIL'} {case}: {why}")
    if not ok:
        failures.append(case)


def main():
    hello = probe.build(SRC / "probes" / "hello.c")
    r = probe.run_emu(hello, b"\x01\x02\x03", 1500)
    v = r.get("result", {})
    check("hello", r["state"] == "done" and r["rc"] == 42 and v.get("arg") == "010203" and v.get("greet") == "hello from RAM"
          and v.get("data") == 7 and v.get("bss_zero") == 1 and v.get("data_at") == "0x01c60000"
          and abs(v.get("ticks_per_10ms", 0) - 240000) < 300,
          f"ran at {v.get('run_at')}, data at {v.get('data_at')}, {v.get('ticks_per_10ms')} ticks in 10 ms, rc {r.get('rc')}")

    arg, r_in = probe.exec_arg(["r3 = r1 * r2", "r4 = r1 - r2", "r5 = r0"], "r0=0xdead,r1=5,r2=7")
    r = probe.exec_show(probe.run_emu(probe.build(SRC / "probes" / "exec.c"), arg, 500), r_in)
    check("exec", r.get("changed") == {"r3": "0x00000023", "r4": "0xfffffffe", "r5": "0x0000dead"},
          f"r3 = r1 * r2, r4 = r1 - r2, r5 = r0: {r.get('changed')}")

    r = probe.run_emu(probe.build(SRC / "probes" / "timing.c"), struct.pack("<I", 500), 2000)
    t = {k: x for k, x in r.get("result", {}).items() if isinstance(x, dict)}
    check("timing", r["state"] == "done" and len(t) >= 10 and all(x["ticks"] > 0 for x in t.values()),
          f"{len(t)} cases; nop {t.get('nop_x8', {}).get('emu_cycles')} cycles, load from RAM "
          f"{t.get('ld_ram_x8', {}).get('emu_cycles')}, from flash {t.get('ld_xip_seq_x8', {}).get('emu_cycles')}")

    sine = (SRC / "build" / "modules" / "sine.hrbr").read_bytes()
    r = probe.run_emu(sine, b"", 500)
    check("run_refuses_module", r["state"] == "none", "RUN with a module's file: nothing runs")
    keys, done = hd.din_script(hello, 600)
    with __import__("tempfile").TemporaryDirectory() as wd:
        import harbor
        import os
        (Path(wd) / "work").mkdir()
        env = dict(os.environ, FM1_BENCH_KEYS=",".join(keys), FM1_DUMPRAM=str(Path(wd) / "ram.bin"))
        subprocess.run([str(harbor.live()), "--bin", str(SRC / "build" / "harbor_xip.bin"), "--shot",
                              str(int(done) + 800)], cwd=wd, capture_output=True, text=True, env=env)
        log = harbor.read_log((Path(wd) / "ram.bin").read_bytes())
    check("commit_refuses_probe", "deploy failed 00000002" in log and "deploy: written" not in log,
          "COMMIT with a probe's file: BAD HEADER, nothing written")
    print("all passed" if not failures else f"FAILED: {', '.join(failures)}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
