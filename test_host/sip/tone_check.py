#!/usr/bin/env python3
"""Check that a raw s16le recording holds a clean sine.

    tone_check.py <file> <rate> <hz> [min_amplitude]

Analyses the middle of the file (skips 0.5 s at both ends): at least 90 % of
the energy must sit at <hz> and the amplitude must exceed min_amplitude
(default 2000). Used by integration.sh on what Asterisk recorded from the
caller, i.e. after Asterisk's own G.722 decoder.
"""
import math
import struct
import sys


def main():
    path, rate, hz = sys.argv[1], int(sys.argv[2]), float(sys.argv[3])
    min_amp = float(sys.argv[4]) if len(sys.argv) > 4 else 2000.0
    with open(path, "rb") as f:
        data = f.read()
    x = struct.unpack("<%dh" % (len(data) // 2), data[: len(data) // 2 * 2])
    x = x[rate // 2: len(x) - rate // 2]
    if len(x) < rate // 2:
        print("TONE-CHECK %s: only %d samples" % (path, len(x)))
        return 1
    w = 2 * math.pi * hz / rate
    c = 2 * math.cos(w)
    s1 = s2 = 0.0
    for v in x:
        s1, s2 = v + c * s1 - s2, s1
    n = len(x)
    p = (s1 * s1 + s2 * s2 - c * s1 * s2) / (n * n)
    ms = sum(v * v for v in x) / n
    purity = 2 * p / ms if ms else 0.0
    amp = 2 * math.sqrt(p)
    print("TONE-CHECK %s: %.2f s, %.0f Hz amplitude %.0f, %.1f %% of energy"
          % (path, n / rate, hz, amp, 100 * purity))
    return 0 if purity >= 0.9 and amp >= min_amp else 1


if __name__ == "__main__":
    sys.exit(main())
