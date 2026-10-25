#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""BLE-MIDI from the computer, straight over Bluetooth LE (bleak), without the OS's MIDI setup:
fm1-dock's BLUETOOTH (PLAN.md, "BLE-MIDI") tested from a Mac.

  uv run --with bleak tools/blemidi.py scan [--secs 6]
      the devices advertising the BLE-MIDI service, with their signal strength
  uv run --with bleak tools/blemidi.py test [--name FM1-DOCK] [--secs 15]
      connects, turns the MIDI notifications on, plays C E G (a note each 0.4 s) into the FM-1, then
      prints what the FM-1 sends for the rest of the time: play its keys

macOS asks once for the terminal's Bluetooth permission. Leave the FM-1 unconnected in Audio MIDI
Setup: a connected peripheral no longer advertises, and the scan would not find it."""
import argparse
import asyncio
import sys
import time

MIDI_SVC = "03b80e5a-ede8-4b33-a751-6ce34ec4c700"
MIDI_IO = "7772e5db-3868-4112-a1a9-f2669d106bf3"
LEN = {0x80: 2, 0x90: 2, 0xA0: 2, 0xB0: 2, 0xC0: 1, 0xD0: 1, 0xE0: 2}


def packet(msgs, t0):
    """one BLE-MIDI packet: the header (timestamp's high 6 bits), then per message its low 7 bits and the bytes"""
    ts = int((time.monotonic() - t0) * 1000) & 0x1FFF
    out = bytearray([0x80 | (ts >> 7)])
    for m in msgs:
        out += bytes([0x80 | (ts & 0x7F)]) + bytes(m)
    return bytes(out)


def decode(p):
    """[(timestamp ms (13 bits), message bytes)] from a BLE-MIDI packet (running status, SysEx not handled)"""
    out, i, hi, st, ts, lo = [], 1, p[0] & 0x3F, None, 0, -1
    while i < len(p):
        if p[i] & 0x80:                         # a timestamp byte (its low 7 bits wrap: the high part goes on)
            if (p[i] & 0x7F) < lo:
                hi = (hi + 1) & 0x3F
            lo = p[i] & 0x7F
            ts = hi << 7 | lo
            i += 1
            if i < len(p) and p[i] & 0x80:
                st = p[i]
                i += 1
        if st is None:
            break
        n = LEN.get(st & 0xF0, 0)
        out.append((ts, bytes([st]) + bytes(p[i:i + n])))
        i += n
    return out


def describe(m):
    s, ch = m[0] & 0xF0, (m[0] & 15) + 1
    name = {0x80: "note off", 0x90: "note on", 0xA0: "poly pressure", 0xB0: "CC", 0xC0: "program",
            0xD0: "pressure", 0xE0: "pitch bend"}.get(s, f"{m[0]:02X}")
    return f"ch {ch} {name} " + " ".join(str(b) for b in m[1:])


async def find(name, secs):
    from bleak import BleakScanner
    found = await BleakScanner.discover(timeout=secs, return_adv=True, service_uuids=[MIDI_SVC])
    return [(d, a) for d, a in found.values() if not name or name.lower() in (a.local_name or d.name or "").lower()]


async def scan(a):
    devs = await find(None, a.secs)
    if not devs:
        print("no BLE-MIDI device advertising (BLUETOOTH ON on the FM-1? not connected in Audio MIDI Setup?)")
    for d, adv in devs:
        print(f"{adv.local_name or d.name or '?':24} {adv.rssi:4} dBm  {d.address}")
    return 0 if devs else 1


async def test(a):
    from bleak import BleakClient
    devs = await find(a.name, a.secs if a.secs < 8 else 8)
    if not devs:
        print(f"no BLE-MIDI device named like {a.name!r}")
        return 1
    d, adv = devs[0]
    print(f"connecting to {adv.local_name or d.name} ({adv.rssi} dBm) ...")
    t0 = time.monotonic()
    got = []

    def rx(_, data):
        for ts, m in decode(bytes(data)):
            got.append(m)
            print(f"  from the FM-1  t={ts:5} ms  {describe(m)}   ({bytes(data).hex(' ')})")

    async with BleakClient(d) as c:
        print(f"connected in {time.monotonic() - t0:.1f} s, MTU {c.mtu_size}")
        await c.start_notify(MIDI_IO, rx)
        print("notifications on; playing C E G into the FM-1")
        for n in (60, 64, 67):
            await c.write_gatt_char(MIDI_IO, packet([(0x90, n, 100)], t0), response=False)
            await asyncio.sleep(0.3)
            await c.write_gatt_char(MIDI_IO, packet([(0x80, n, 0)], t0), response=False)
            await asyncio.sleep(0.1)
        print(f"now play the FM-1's keys: listening {a.secs} s")
        await asyncio.sleep(a.secs)
        await c.stop_notify(MIDI_IO)
    print(f"done: {len(got)} messages from the FM-1")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("scan")
    s.add_argument("--secs", type=float, default=6)
    t = sub.add_parser("test")
    t.add_argument("--name", default="FM1-DOCK")
    t.add_argument("--secs", type=float, default=15)
    a = ap.parse_args()
    return asyncio.run(scan(a) if a.cmd == "scan" else test(a))


if __name__ == "__main__":
    sys.exit(main())
