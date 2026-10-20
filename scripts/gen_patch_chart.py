#!/usr/bin/env python3
"""Generate the levels/colour patch chart for the NDI signal-flow check.

Companion to gen_resolution_chart.py (issue #57): where that chart proves how
many pixels reach the viewer, this one proves which CODE VALUES do. Drop the
PNG on a timeline at the chart's size, stream it through the plugin, and read
it back with

  ./build/ndi_verify --source "<name>" --patches --color bgra

which samples the centre of every patch, prints received vs. source values,
and fails when any channel is more than 2 codes off (SpeedHQ is lossy; the
encoder itself is exact). `--color bgra` makes NDI's own UYVY->RGB decoder do
the conversion, i.e. what a receiver displays; the default (UYVY) uses
ndi_verify's BT.709 limited-range decode instead. For a packed stereo stream
add `--patch-region left` (or right/top/bottom) to read one eye.

Layout (keep in sync with kPatches in tools/ndi_verify.cpp): a 6x4 grid of
equal patches filling the whole frame, row-major from the top-left.

  row 0  levels: 0, 16, 32, 128, 235, 255 grey — the old full-range encoder
         crushed 16 to 0 and clipped 235 to 255, and tinted 128 green
  row 1  100% primaries and secondaries: R, G, B, C, M, Y
  row 2  grey ramp 0 .. 116 (12-step ramp, first half)
  row 3  grey ramp 139 .. 255 (second half)

Each patch carries a small label in its top-left corner, outside the central
50% that ndi_verify averages. Standard library only, like the resolution
chart.

Usage
-----
  scripts/gen_patch_chart.py --width 1920 --height 1080 --out patches.png
  scripts/gen_patch_chart.py --size 3600 --eye L
"""

import argparse
import struct
import sys
import zlib

from gen_resolution_chart import FONT

COLS, ROWS = 6, 4

_RAMP = [round(i * 255 / 11) for i in range(12)]

# (label, (r, g, b)) in 8-bit full range, row-major. tools/ndi_verify.cpp
# carries the same table.
PATCHES = [
    ("0", (0, 0, 0)), ("16", (16, 16, 16)), ("32", (32, 32, 32)),
    ("128", (128, 128, 128)), ("235", (235, 235, 235)), ("255", (255, 255, 255)),
    ("R", (255, 0, 0)), ("G", (0, 255, 0)), ("B", (0, 0, 255)),
    ("C", (0, 255, 255)), ("M", (255, 0, 255)), ("Y", (255, 255, 0)),
] + [(str(v), (v, v, v)) for v in _RAMP]


def patch_rect(index, width, height):
    """Pixel rectangle (x0, y0, x1, y1) of patch `index`; matches
    ndi_verify's patchRect()."""
    col, row = index % COLS, index // COLS
    return (col * width // COLS, row * height // ROWS,
            (col + 1) * width // COLS, (row + 1) * height // ROWS)


def build_chart(width, height, eye):
    rows = [bytearray(width * 3) for _ in range(height)]

    def fill(x0, y0, x1, y1, rgb):
        seg = bytes(rgb) * (x1 - x0)
        for y in range(y0, y1):
            rows[y][x0 * 3:x1 * 3] = seg

    def text(x, y, s, scale, rgb):
        for ch in s.upper():
            glyph = FONT.get(ch, FONT[" "])
            for r, bits in enumerate(glyph):
                for c in range(5):
                    if bits & (0x10 >> c):
                        fill(x + c * scale, y + r * scale,
                             x + (c + 1) * scale, y + (r + 1) * scale, rgb)
            x += 6 * scale

    scale = max(1, min(width // COLS, height // ROWS) // 60)
    for i, (label, rgb) in enumerate(PATCHES):
        x0, y0, x1, y1 = patch_rect(i, width, height)
        fill(x0, y0, x1, y1, rgb)
        luma = 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2]
        ink = (0, 0, 0) if luma > 100 else (255, 255, 255)
        pad = 2 * scale
        tag = label if i else "%s %s" % (eye, label)  # eye name on the first patch
        text(x0 + pad, y0 + pad, tag, scale, ink)
    return width, height, rows


def write_png(path, width, height, rows):
    raw = bytearray()
    for row in rows:
        raw.append(0)  # filter type 0 (None)
        raw += row
    data = zlib.compress(bytes(raw), 6)

    def chunk(tag, body):
        c = tag + body
        return struct.pack(">I", len(body)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)  # 8-bit RGB
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", ihdr))
        f.write(chunk(b"IDAT", data))
        f.write(chunk(b"IEND", b""))


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--size", type=int, help="square chart size (sets both width and height)")
    ap.add_argument("--width", type=int, help="chart width in px")
    ap.add_argument("--height", type=int, help="chart height in px")
    ap.add_argument("--eye", default="MONO", choices=["L", "R", "MONO", "l", "r", "mono"],
                    help="eye label drawn on the first patch (L/R for a stereo clip pair)")
    ap.add_argument("--out", help="output PNG path (default patches_<W>x<H>_<EYE>.png)")
    a = ap.parse_args(argv)

    width = a.width or a.size
    height = a.height or a.size
    if not width or not height:
        ap.error("give --size N or both --width and --height")
    if width % 2 or height % 2:
        ap.error("width and height must be even (4:2:2 streams carry pixel pairs)")
    if width < COLS * 16 or height < ROWS * 16:
        ap.error("chart too small: at least %dx%d" % (COLS * 16, ROWS * 16))
    eye = a.eye.upper()
    out = a.out or "patches_%dx%d_%s.png" % (width, height, eye)

    write_png(out, *build_chart(width, height, eye))
    print("wrote %s (%dx%d, 8-bit RGB, %d patches)" % (out, width, height, len(PATCHES)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
