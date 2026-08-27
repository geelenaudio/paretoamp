#!/usr/bin/env python3
"""TAS5825M signed Q1.31 reference encoder.

Used by `test/test_native/test_tas5825m_bq.cpp` as the host-side oracle.
Range is [-1.0, +1.0); values outside saturate. NaN / Inf become 0.
"""

from __future__ import annotations

import math
import struct
import sys


def float_to_q1_31(v: float) -> int:
    if math.isnan(v) or math.isinf(v):
        return 0
    if v <= -1.0:
        return -2147483648
    if v >= 1.0:
        return 2147483647
    x = v * 2147483648.0
    if x >= 2147483647.0:
        return 2147483647
    if x <= -2147483648.0:
        return -2147483648
    return int(x + 0.5) if x >= 0 else int(x - 0.5)


def q1_31_to_be(v: int) -> bytes:
    return struct.pack(">i", v)


def main() -> int:
    samples = [0.0, 1.0, -1.0, 0.5, -0.5, 0.25, math.pi / 4]
    for s in samples:
        q = float_to_q1_31(s)
        print(f"{s: .8f} -> 0x{q & 0xFFFFFFFF:08X} ({q})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
