#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Compare two WAVs from their first non-silent sample: bit-identical or not.
  tests/same_note.py A.wav B.wav [seconds]"""
import sys
import wave


def frames(path):
    w = wave.open(path)
    d = w.readframes(w.getnframes())
    s = [int.from_bytes(d[i:i + 2], "little", signed=True) for i in range(0, len(d), 2)]
    first = next((i for i, x in enumerate(s) if x), len(s))
    return s[first - first % 2:]


a, b = frames(sys.argv[1]), frames(sys.argv[2])
n = min(len(a), len(b), int(float(sys.argv[3]) * 88200) if len(sys.argv) > 3 else 1 << 60)
diff = next((i for i in range(n) if a[i] != b[i]), None)
if n == 0:
    sys.exit("silent")
if diff is None:
    print(f"identical: {n // 2} frames from the onset")
else:
    sys.exit(f"differ at frame {diff // 2} of {n // 2}: {a[diff]} / {b[diff]}")
