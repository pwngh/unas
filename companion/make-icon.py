#!/usr/bin/env python3
"""Generate app-icon.png — a 1024x1024 source icon, no third-party deps.

Run `python3 make-icon.py` then `npm run icon` to produce every platform
format under src-tauri/icons/. Replace app-icon.png with your own art any
time; this is just a clean placeholder (a blue disk stack)."""
import struct
import sys
import zlib

S = 1024
BLUE = (0x37, 0x8A, 0xDD)
WHITE = (0xFF, 0xFF, 0xFF)
PAD = 96          # transparent margin -> rounded-square icon
RAD = 190         # outer corner radius


def in_round_rect(x, y, x0, y0, x1, y1, r):
    if x < x0 or y < y0 or x >= x1 or y >= y1:
        return False
    cx = min(max(x, x0 + r), x1 - r)
    cy = min(max(y, y0 + r), y1 - r)
    return (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def pixel(x, y):
    if not in_round_rect(x, y, PAD, PAD, S - PAD, S - PAD, RAD):
        return (0, 0, 0, 0)
    # three stacked "platters" in white -> a storage glyph
    cx, w = S / 2, 250
    for cy in (S * 0.38, S * 0.50, S * 0.62):
        if in_round_rect(x, y, cx - w, cy - 34, cx + w, cy + 34, 34):
            return (*WHITE, 255)
    return (*BLUE, 255)


def main(path):
    raw = bytearray()
    for y in range(S):
        raw.append(0)                       # filter byte: none
        for x in range(S):
            raw += bytes(pixel(x, y))

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", S, S, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)
    print(f"wrote {path} ({S}x{S})")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "app-icon.png")
