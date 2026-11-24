#!/usr/bin/env python3
"""Generate ORBIT's simple 512x512 orbital mark without image dependencies.

Rewritten for ORBIT on 2026-11-24.
"""
from __future__ import annotations

import struct
import sys
import zlib
import math
from pathlib import Path

W = H = 512


def chunk(kind: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)


def pixel(x: int, y: int) -> tuple[int, int, int]:
    red, green, blue = 13, 18, 28
    dx = (x - 256) / 178.0
    dy = (y - 256) / 112.0
    orbit_radius = math.sqrt((dx + dy * 0.31) ** 2 + (dy - dx * 0.24) ** 2)
    if 0.88 <= orbit_radius <= 1.02:
        red, green, blue = 36, 205, 215
    if (x - 385) ** 2 + (y - 126) ** 2 <= 23 ** 2:
        red, green, blue = 240, 244, 250
    return red, green, blue


def main(path: str) -> None:
    raw = bytearray()
    for y in range(H):
        raw.append(0)
        for x in range(W):
            raw.extend(pixel(x, y))
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    Path(path).write_bytes(png)


if __name__ == "__main__":
    main(sys.argv[1])
