#!/usr/bin/env python3
"""Generate a resolution test chart for the NDI signal-flow check.

Drop the PNG(s) on a timeline at the chart's size, stream through the plugin,
and read the delivered resolution off the receiver (NDI Video Monitor,
`ndi_verify --dump`, or in-headset in VR.NDI). Standard library only — no
PIL/numpy — so it runs on any Mac. A 7200x7200 chart takes ~10-20 s.

What's on the chart
-------------------
* Line-pair blocks, labelled by the delivered resolution they PROVE. Each
  block holds vertical (top half) and horizontal (bottom half) black/white
  lines at a pitch of 2, 4, 8, 16, 32 and 64 px. Lines at pitch p are only
  resolvable when the picture reaching your eye carries at least 2*W/p pixels
  across, so on a 7200-wide chart the blocks read 7200, 3600, 1800, 900, 450,
  225. Find the finest block that is still clean black/white stripes (no grey
  mush, no moire): that number is the per-eye resolution you are actually
  seeing. Expected with the plugin at Half: the "3600" block is clean, "7200"
  is uniform grey. If "3600" is moire/aliased and only "1800" is clean, a
  stage after the plugin is resampling (see LEARNINGS 2026-10-08).
* A zone plate (concentric rings of rising frequency) in the centre. Any
  nearest-neighbour or under-sampled rescale anywhere in the chain shows up as
  moire rings that are not centred on the plate.
* Corner and centre crosshairs, a 1% border, and a big eye label (L/R/MONO),
  so stereo packing and eye assignment are checked at the same time.

Usage
-----
  scripts/gen_resolution_chart.py --size 7200 --eye L --out chart_L.png
  scripts/gen_resolution_chart.py --size 7200 --eye R --out chart_R.png
  scripts/gen_resolution_chart.py --width 7200 --height 3600 --eye mono

For a stereo timeline, import chart_L.png and chart_R.png and make a stereo
clip (Media Pool > right-click > Stereo 3D Sync, or New Stereo Clip).
"""

import argparse
import math
import struct
import sys
import zlib

# 5x7 bitmap glyphs: rows top-down, 5 bits each (MSB = leftmost column).
FONT = {
    "0": [0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E],
    "1": [0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E],
    "2": [0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F],
    "3": [0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E],
    "4": [0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02],
    "5": [0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E],
    "6": [0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E],
    "7": [0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08],
    "8": [0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E],
    "9": [0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C],
    "A": [0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11],
    "B": [0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E],
    "C": [0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E],
    "D": [0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E],
    "E": [0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F],
    "F": [0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10],
    "G": [0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F],
    "H": [0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11],
    "I": [0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E],
    "J": [0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C],
    "K": [0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11],
    "L": [0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F],
    "M": [0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11],
    "N": [0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11],
    "O": [0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E],
    "P": [0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10],
    "Q": [0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D],
    "R": [0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11],
    "S": [0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E],
    "T": [0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04],
    "U": [0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E],
    "V": [0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04],
    "W": [0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A],
    "X": [0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11],
    "Y": [0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04],
    "Z": [0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F],
    " ": [0, 0, 0, 0, 0, 0, 0],
    "-": [0, 0, 0, 0x1F, 0, 0, 0],
    ":": [0, 0x04, 0, 0, 0, 0x04, 0],
    "/": [0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10],
    "=": [0, 0, 0x1F, 0, 0x1F, 0, 0],
    ">": [0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08],
}

WHITE, BLACK, GREY = 255, 0, 128


class Canvas:
    """8-bit greyscale image as a list of bytearray rows."""

    def __init__(self, width, height, fill=GREY):
        self.w = width
        self.h = height
        self.rows = [bytearray([fill]) * width for _ in range(height)]

    def fill_rect(self, x0, y0, x1, y1, value):
        x0, x1 = max(0, x0), min(self.w, x1)
        y0, y1 = max(0, y0), min(self.h, y1)
        if x1 <= x0 or y1 <= y0:
            return
        seg = bytes([value]) * (x1 - x0)
        for y in range(y0, y1):
            self.rows[y][x0:x1] = seg

    def vlines(self, x0, y0, x1, y1, pitch):
        """Vertical black/white stripes: pitch/2 black, pitch/2 white."""
        half = pitch // 2
        unit = bytes([BLACK]) * half + bytes([WHITE]) * (pitch - half)
        width = x1 - x0
        seg = (unit * (width // pitch + 1))[:width]
        for y in range(y0, y1):
            self.rows[y][x0:x1] = seg

    def hlines(self, x0, y0, x1, y1, pitch):
        """Horizontal black/white stripes: pitch/2 black, pitch/2 white."""
        half = pitch // 2
        width = x1 - x0
        black = bytes([BLACK]) * width
        white = bytes([WHITE]) * width
        for y in range(y0, y1):
            self.rows[y][x0:x1] = black if ((y - y0) % pitch) < half else white

    def text(self, x, y, s, scale, value=WHITE, bg=None):
        """Draw 5x7 glyphs scaled by `scale`; 1 glyph-column of spacing."""
        cx = x
        for ch in s.upper():
            glyph = FONT.get(ch, FONT[" "])
            if bg is not None:
                self.fill_rect(cx, y, cx + 6 * scale, y + 7 * scale, bg)
            for r, bits in enumerate(glyph):
                for c in range(5):
                    if bits & (0x10 >> c):
                        self.fill_rect(cx + c * scale, y + r * scale,
                                       cx + (c + 1) * scale, y + (r + 1) * scale, value)
            cx += 6 * scale
        return cx

    def text_width(self, s, scale):
        return len(s) * 6 * scale

    def crosshair(self, cx, cy, arm, thick):
        self.fill_rect(cx - arm, cy - thick // 2, cx + arm, cy + thick // 2 + 1, WHITE)
        self.fill_rect(cx - thick // 2, cy - arm, cx + thick // 2 + 1, cy + arm, WHITE)
        self.fill_rect(cx - arm, cy - thick // 2 - 2, cx + arm, cy - thick // 2, BLACK)
        self.fill_rect(cx - arm, cy + thick // 2 + 1, cx + arm, cy + thick // 2 + 3, BLACK)

    def zone_plate(self, cx, cy, radius, cycles_at_edge):
        """cos(k r^2): spatial frequency rises linearly with radius and reaches
        `cycles_at_edge` cycles per radius at the rim (keep this under
        radius/2 so the finest rings are still above Nyquist for the chart)."""
        k = math.pi * cycles_at_edge / (radius * radius)
        r2max = radius * radius
        for y in range(cy - radius, cy + radius):
            if y < 0 or y >= self.h:
                continue
            dy2 = (y - cy) * (y - cy)
            row = self.rows[y]
            x0 = max(0, cx - radius)
            x1 = min(self.w, cx + radius)
            vals = bytearray(x1 - x0)
            for i, x in enumerate(range(x0, x1)):
                r2 = (x - cx) * (x - cx) + dy2
                if r2 <= r2max:
                    vals[i] = int(127.5 + 127.5 * math.cos(k * r2))
                else:
                    vals[i] = row[x]
            row[x0:x1] = vals

    def write_png(self, path):
        raw = bytearray()
        for row in self.rows:
            raw.append(0)  # filter type 0 (None)
            raw += row
        data = zlib.compress(bytes(raw), 6)

        def chunk(tag, body):
            c = tag + body
            return struct.pack(">I", len(body)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

        ihdr = struct.pack(">IIBBBBB", self.w, self.h, 8, 0, 0, 0, 0)  # 8-bit greyscale
        with open(path, "wb") as f:
            f.write(b"\x89PNG\r\n\x1a\n")
            f.write(chunk(b"IHDR", ihdr))
            f.write(chunk(b"IDAT", data))
            f.write(chunk(b"IEND", b""))


def build_chart(width, height, eye):
    c = Canvas(width, height)
    m = max(8, width // 100)  # border thickness: 1% of width
    c.fill_rect(0, 0, width, m, WHITE)
    c.fill_rect(0, height - m, width, height, WHITE)
    c.fill_rect(0, 0, m, height, WHITE)
    c.fill_rect(width - m, 0, width, height, WHITE)

    # Title row.
    title_scale = max(2, width // 400)
    title = "NDI RES CHART %dX%d %s" % (width, height, eye)
    c.text((width - c.text_width(title, title_scale)) // 2, m + title_scale * 2, title, title_scale)

    # Crosshair geometry, shared with the corner marks below so the blocks
    # start clear of the top two.
    arm = width // 40
    thick = max(3, width // 1200)
    inset_x, inset_y = width // 20, height // 20

    # Line-pair blocks across the top third: pitch 2..64.
    pitches = [2, 4, 8, 16, 32, 64]
    label_scale = max(2, width // 600)
    gap = width // 60
    block_w = (width - 2 * m - gap * (len(pitches) + 1)) // len(pitches)
    block_h = height // 4
    y_top = max(m + title_scale * 12, inset_y + arm + 2 * label_scale)
    for i, p in enumerate(pitches):
        x0 = m + gap + i * (block_w + gap)
        x1 = x0 + block_w
        proves = 2 * width // p
        label = "%d" % proves
        sub = "PITCH %d" % p
        c.text(x0, y_top, label, label_scale, WHITE)
        c.text(x0, y_top + 8 * label_scale, sub, max(1, label_scale // 2), WHITE)
        yb = y_top + 13 * label_scale
        mid = yb + block_h // 2
        c.vlines(x0, yb, x1, mid, p)
        c.hlines(x0, mid, x1, yb + block_h, p)
        # Thin separator so the two halves read as two tests.
        c.fill_rect(x0, mid - 1, x1, mid + 1, GREY)

    # Zone plate in the lower-centre, below the centre crosshair's arm.
    radius = min(width, height) // 5
    zx, zy = width // 2, height - m - radius - height // 30
    c.zone_plate(zx, zy, radius, cycles_at_edge=radius // 4)
    zl = "ZONE PLATE: MOIRE = RESAMPLING"
    c.text(zx - c.text_width(zl, label_scale) // 2, zy - radius - 9 * label_scale, zl, label_scale, WHITE)

    # Eye label, large, left-middle of the lower half (clear of the plate).
    eye_scale = max(4, width // 60)
    ex = m + gap
    ey = zy - 3 * eye_scale
    c.text(ex, ey, eye, eye_scale, WHITE, bg=BLACK)
    hint = "HALF OF %d => %d" % (width, width // 2)
    c.text(ex, ey + 9 * eye_scale, hint, max(2, label_scale // 2), WHITE)
    legend = ["CLEAN STRIPES AT N", "= N PX ACROSS", "REACH YOUR EYE"]
    for j, line in enumerate(legend):
        c.text(ex, ey + 9 * eye_scale + (j + 2) * 9 * max(2, label_scale // 2), line,
               max(2, label_scale // 2), WHITE)

    # Crosshairs: centre and the four corners (inset by 5%).
    for (cx, cy) in [(width // 2, height // 2), (inset_x, inset_y), (width - inset_x, inset_y),
                     (inset_x, height - inset_y), (width - inset_x, height - inset_y)]:
        c.crosshair(cx, cy, arm, thick)
    return c


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--size", type=int, help="square chart size (sets both width and height)")
    ap.add_argument("--width", type=int, help="chart width in px")
    ap.add_argument("--height", type=int, help="chart height in px")
    ap.add_argument("--eye", default="MONO", choices=["L", "R", "MONO", "l", "r", "mono"],
                    help="eye label drawn on the chart (L/R for a stereo clip pair)")
    ap.add_argument("--out", help="output PNG path (default chart_<W>x<H>_<EYE>.png)")
    a = ap.parse_args(argv)

    width = a.width or a.size
    height = a.height or a.size
    if not width or not height:
        ap.error("give --size N or both --width and --height")
    if width % 2 or height % 2:
        ap.error("width and height must be even (4:2:2 streams carry pixel pairs)")
    eye = a.eye.upper()
    out = a.out or "chart_%dx%d_%s.png" % (width, height, eye)

    canvas = build_chart(width, height, eye)
    canvas.write_png(out)
    print("wrote %s (%dx%d, 8-bit grey)" % (out, width, height))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
