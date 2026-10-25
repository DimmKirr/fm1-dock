#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Probes: small programs run once from RAM inside harbor, on an FM-1 and in fm1-emu alike, to
measure timing and test instructions (firmware/harbor/hb_module.h "Probes", probes/probe.h).

  tools/probe.py run probes/timing.c [--on emu|fm1|emu,fm1] [--arg HEX | --args 1,2,3] [--ms 2000]
      builds the probe (tools/hbmod.py), runs it, prints its result as JSON; with two targets,
      side by side, with the ratio of their times
  tools/probe.py exec "r3 = r1 * r2" [--regs r1=5,r2=7] [--on ...]
      assembles the lines (`;` between them) in a wrapper that sets r0..r15 and stores them after,
      runs them with probes/exec.c and prints the registers that changed (and r0..r15 in full)
  tools/probe.py result [--port NAME]     the FM-1's last result again (after a crash: what it left)

emu: fm1-live boots build/harbor_xip.bin headless, the probe goes into the DIN jack (BEGIN, DATA,
RUN), the result is read from a RAM dump (build harbor first: tools/build.py). fm1: harbor on the
FM-1 over USB-MIDI (python-rtmidi: uv run --with python-rtmidi tools/probe.py ...); nothing is
written to its flash. A probe that crashes or hangs resets the FM-1; its output so far and the
PC are kept, and this tool asks for them once the FM-1 is back.

Time records (P_TIME) print `ns`: per run of the statement, or per operation when the key ends in
_xN (N operations per run), and `emu_cycles`: what fm1-emu's model would charge (one instruction
per 240 MHz cycle; from the emulator's own ticks that is exact, from the FM-1's it is the number
of 240 MHz cycles the real chip took)."""
import argparse
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

SRC = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SRC / "tools"))
import hbdeploy as hd  # noqa: E402

RAM_BASE = 0x01C00000
PR_MAGIC = 0x52504248
STATE = ["none", "running", "done", "crashed", "hung"]
TYPES = {1: "u32", 2: "i32", 3: "hex", 4: "bytes", 5: "text", 6: "words", 7: "time"}
TICKS_PER_US = 24
CPU_MHZ = 240                                   # fm1-emu's model


def build(src):
    import contextlib
    import hbmod
    with contextlib.redirect_stdout(sys.stderr):   # (stdout is the JSON)
        return hbmod.build_module(src).read_bytes()


def decode(out):
    """the records (probes/probe.h) -> {key: value}; a key twice -> a list"""
    res, i = {}, 0
    while i + 4 <= len(out):
        t, k = out[i], out[i + 1]
        key = out[i + 2:i + 2 + k].decode("latin-1")
        (n,) = struct.unpack_from("<H", out, i + 2 + k)
        v = out[i + 4 + k:i + 4 + k + n]
        i += 4 + k + n
        kind = TYPES.get(t)
        if kind == "u32":
            val = struct.unpack("<I", v)[0]
        elif kind == "i32":
            val = struct.unpack("<i", v)[0]
        elif kind == "hex":
            val = f"{struct.unpack('<I', v)[0]:#010x}"
        elif kind == "bytes":
            val = v.hex()
        elif kind == "text":
            val = v.decode("latin-1")
        elif kind == "words":
            val = [f"{w:#010x}" for w in struct.unpack(f"<{len(v) // 4}I", v)]
        elif kind == "time":
            ticks, runs = struct.unpack("<iI", v)
            m = re.search(r"_x(\d+)$", key)
            per = runs * (int(m.group(1)) if m else 1)
            ns = ticks * 1000 / TICKS_PER_US / per if per else 0
            val = {"ticks": ticks, "runs": runs, "ns": round(ns, 2), "emu_cycles": round(ns * CPU_MHZ / 1000, 2)}
        else:
            val = v.hex()
        if key in res:
            res[key] = (res[key] if isinstance(res[key], list) and kind != "words" else [res[key]]) + [val]
        else:
            res[key] = val
    return res


def summary(state, run_id, rc, ticks, out_n, pc, out):
    r = {"state": STATE[state] if state < len(STATE) else state, "id": run_id, "rc": rc,
         "us": round(ticks / TICKS_PER_US, 1)}
    if state == 3:
        r["crash_pc"] = f"{pc:#010x}"
    r["result"] = decode(out[:out_n])
    return r


def pr_addr():
    m = re.search(r"^([0-9a-f]{8}) .*\shb_probe$", (SRC / "build" / "harbor.syms").read_text(), re.M)
    if not m:
        raise SystemExit("no hb_probe in build/harbor.syms: build harbor (tools/build.py)")
    return int(m.group(1), 16)


def from_ram(ram, run_id):
    o = pr_addr() - RAM_BASE
    magic, rid, state, ticks, pc, rc = struct.unpack_from("<5Ii", ram, o)
    if magic != PR_MAGIC:
        return {"state": "none", "note": "no probe ran (did it arrive? is --ms long enough?)"}
    if rid != run_id:
        return {"state": "none", "note": f"the last run was {rid}, not this one ({run_id})"}
    (out_n,) = struct.unpack_from("<I", ram, o + 48 + 16)
    out = ram[o + 76:o + 76 + min(out_n, 8192)]
    return summary(state, rid, rc, ticks, min(out_n, 8192), pc, out)


def run_emu(blob, arg, ms):
    import harbor
    run_id = int(time.time()) & 0x7FFFFFFF
    keys, done = hd.din_script(blob, 600, settle=60, last=(hd.RUN, hd.run_payload(run_id, arg)))
    with tempfile.TemporaryDirectory() as wd:
        (Path(wd) / "work").mkdir()
        ram = Path(wd) / "ram.bin"
        env = dict(os.environ, FM1_BENCH_KEYS=",".join(keys), FM1_DUMPRAM=str(ram))
        r = subprocess.run([str(harbor.live()), "--bin", str(SRC / "build" / "harbor_xip.bin"), *os.environ.get("PROBE_EMU_ARGS", "").split(), "--shot",
                            str(int(done) + ms)], cwd=wd, capture_output=True, text=True, env=env)
        if r.returncode:
            raise SystemExit(f"fm1-live: {r.stdout[-2000:]}{r.stderr[-2000:]}")
        res = from_ram(ram.read_bytes(), run_id)
        if "halt" in r.stdout + r.stderr:
            res["emulator"] = [ln for ln in (r.stdout + r.stderr).splitlines() if "halt" in ln][:3]
        res["log"] = [ln for ln in harbor.read_log(ram.read_bytes()).splitlines() if "probe" in ln or "FAULT" in ln][-6:]
        return res


def result_fm1(p):
    out, frm, head = b"", 0, None
    while True:
        d = p.ask(hd.RESULT, struct.pack("<I", frm))
        state, = d[1:2]
        run_id, rc, ticks, out_n, pc, at = struct.unpack_from("<IiIIII", d, 4)
        head = (state, run_id, rc, ticks, out_n, pc)
        chunk = d[28:]
        out += chunk
        frm = at + len(chunk)
        if not chunk or frm >= out_n:
            break
    return summary(*head, out)


def run_fm1(blob, arg, ms, port):
    run_id = int(time.time()) & 0x7FFFFFFF
    p = hd.Port(port)
    notes = [0, []]                             # MIDI note-ons that came in while it ran (probes/idle.c, usbalign.c)

    class Counting:                             # (rtmidi's get_message cannot be replaced: wrapped)
        def __init__(self, inp):
            self.inp = inp

        def get_message(self):
            m = self.inp.get_message()
            if m and m[0] and (m[0][0] & 0xF0) == 0x90 and len(m[0]) > 2 and m[0][2]:
                notes[0] += 1
                notes[1].append(m[0][1])
            return m

        def __getattr__(self, name):
            return getattr(self.inp, name)
    p.inp = Counting(p.inp)
    h = hd.hello(p)
    if h["abi"] != struct.unpack_from("<I", blob, 4)[0]:
        raise SystemExit(f"the FM-1's harbor speaks ABI {h['abi']}: rebuild the probe or update harbor")
    frames = hd.deploy_frames(blob, (hd.RUN, hd.run_payload(run_id, arg)))
    for cmd, payload in frames[:-1]:
        d = p.ask(cmd, payload, timeout=1.0, tries=4)
        if d[0]:
            p.send(hd.ABORT)
            raise SystemExit(f"probe: {hd.status(d)}")
    cmd, payload = frames[-1]
    seq = p.send(cmd, payload)
    for c, d in p.replies(seq, ms / 1000 + 2.0):
        if c == hd.RUN or c == 0x3F:
            if d[0]:
                raise SystemExit(f"probe: {hd.status(d)}")
            r = result_fm1(p)
            r["midi_in"] = {"note_ons": notes[0], "notes": notes[1][:64]}
            return r
    print("probe: no answer (it crashed or hung, the FM-1 resets); asking again in a moment", file=sys.stderr)
    print(f"probe: MIDI note-ons that came in before: {notes[1][:64]}", file=sys.stderr)
    for _ in range(20):
        time.sleep(1.0)
        try:
            return result_fm1(hd.Port(port))
        except SystemExit:
            continue
    raise SystemExit("probe: the FM-1 did not come back over USB")


WRAP_IN = ["[--sp] = {rets, r15-r4}", "sp += -8", "[sp+0] = r0",
           *[f"r{i} = [r0+{4 * i}]" for i in range(1, 16)], "r0 = [r0+0]"]
WRAP_OUT = ["[sp+4] = r0", "r0 = [sp+0]", *[f"[r0+{4 * i}] = r{i}" for i in range(1, 16)],
            "r1 = [sp+4]", "[r0+0] = r1", "sp += 8", "{pc, r15-r4} = [sp++]"]


def assemble(lines):
    """pi32v2 assembly -> the wrapped code's bytes (the toolchain's clang, as tools/build.py runs it)"""
    import build
    d = SRC / "build" / "probes" / "asm"
    d.mkdir(parents=True, exist_ok=True)
    (d / "exec.S").write_text("\t.text\n" + "".join(f"\t{ln}\n" for ln in WRAP_IN + lines + WRAP_OUT))
    build.tc("pi32v2/bin/clang", "-target", "pi32v2", "-c", d / "exec.S", "-o", d / "exec.o")
    build.tc("common/bin/objcopy", "-O", "binary", "-j", ".text", d / "exec.o", d / "exec.bin")
    return (d / "exec.bin").read_bytes()


def exec_arg(lines, regs):
    r = [0] * 16
    for kv in (x for x in regs.split(",") if x):
        k, v = kv.split("=")
        r[int(k.strip().lstrip("r"))] = int(v, 0) & 0xFFFFFFFF
    code = assemble(lines)
    if 64 + len(code) > 512:
        raise SystemExit(f"exec: {len(code)} B of code, the argument holds {512 - 64}")
    return struct.pack("<16I", *r) + code, r


def exec_show(res, r_in):
    """r0..r15 after, the changed ones marked"""
    regs = res.get("result", {}).get("regs")
    if not regs:
        return res
    out = {f"r{i}": v for i, v in enumerate(regs) if int(v, 16) != r_in[i]}
    return {"state": res["state"], "changed": out, "ticks": res["result"].get("ticks"), "regs": regs,
            **({"crash_pc": res["crash_pc"]} if "crash_pc" in res else {})}


def side_by_side(a, b):
    """two results: the time records with their ratio, the other values where they differ"""
    print(f"{'':28} {'emu':>12} {'fm1':>12}  fm1/emu")
    ra, rb = a.get("result", {}), b.get("result", {})
    for k in ra.keys() | rb.keys():
        x, y = ra.get(k), rb.get(k)
        if isinstance(x, dict) and isinstance(y, dict):
            q = y["ns"] / x["ns"] if x["ns"] else float("nan")
            print(f"{k:28} {x['ns']:>10} ns {y['ns']:>10} ns  {q:6.2f}")
        elif x != y:
            print(f"{k:28} {str(x):>12} {str(y):>12}  differ")


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("probe")
    r.add_argument("--on", default="emu")
    r.add_argument("--arg", default="", help="the argument as hex bytes")
    r.add_argument("--args", default="", help="the argument as u32s: 1,2,0x30")
    r.add_argument("--ms", type=int, default=2000, help="how long the probe may run")
    r.add_argument("--port")
    x = sub.add_parser("exec")
    x.add_argument("asm", help="pi32v2 assembly, `;` between instructions")
    x.add_argument("--regs", default="", help="r1=5,r2=0x7")
    x.add_argument("--on", default="emu")
    x.add_argument("--ms", type=int, default=500)
    x.add_argument("--port")
    g = sub.add_parser("result")
    g.add_argument("--port")
    a = ap.parse_args()
    if a.cmd == "result":
        print(json.dumps(result_fm1(hd.Port(a.port)), indent=1))
        return
    if a.cmd == "exec":
        import contextlib
        with contextlib.redirect_stdout(sys.stderr):
            arg, r_in = exec_arg([ln.strip() for ln in a.asm.split(";") if ln.strip()], a.regs)
        blob = build(SRC / "probes" / "exec.c")
        res = {on: exec_show(run_emu(blob, arg, a.ms) if on == "emu" else run_fm1(blob, arg, a.ms, a.port), r_in)
               for on in a.on.split(",")}
        print(json.dumps(res if len(res) > 1 else res[a.on], indent=1))
        if len(res) == 2:
            e, f = res.values()
            diff = [i for i in range(16) if e.get("regs", [None] * 16)[i] != f.get("regs", [None] * 16)[i]]
            print("emu and fm1 agree" if not diff else "DIFFER: " + ", ".join(
                f"r{i} emu {e['regs'][i]} fm1 {f['regs'][i]}" for i in diff))
        return
    arg = bytes.fromhex(a.arg) + b"".join(struct.pack("<I", int(x, 0) & 0xFFFFFFFF) for x in a.args.split(",") if x)
    if len(arg) > 512:
        raise SystemExit("the argument may be 512 bytes")
    p = Path(a.probe)
    blob = p.read_bytes() if p.suffix == ".hrbr" else build(p)
    res = {}
    for on in a.on.split(","):
        res[on] = run_emu(blob, arg, a.ms) if on == "emu" else run_fm1(blob, arg, a.ms, a.port)
    print(json.dumps(res if len(res) > 1 else res[a.on], indent=1))
    if len(res) == 2:
        side_by_side(*res.values())


if __name__ == "__main__":
    main()
