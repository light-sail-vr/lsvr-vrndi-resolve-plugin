#!/usr/bin/env python3
"""Check the NDI Output presets end to end against an independent oracle.

Companion to scripts/gen_patch_chart.py and `ndi_verify --patches` (issue
#57). ndi_verify compares the patch chart against its SOURCE values, which
only proves Passthrough. This script proves the display presets: it reads the
live stream with ndi_verify, takes the output space from each frame's
lsvr_color tag, and compares every patch against values computed here from
the published definitions — CIE xy primaries -> RGB->XYZ matrices in double
precision, pure-power Gamma 2.2/2.4/2.6, piecewise sRGB. None of the
plugin's code is involved, so a pass means the plugin matches the standards,
not that it agrees with itself.

Setup (Tier 2): the patch chart on a timeline at its size, the NDI Output
node alone on the Color page, Timeline Color resolving to --timeline
(default Rec.709 Gamma 2.4 — an unmanaged DaVinci YRGB project). Then

  tools/ndi_color_check.py --source "VR.NDI Sender" --loop 300

and cycle NDI Output through the presets, holding each ~5 s; Ctrl-C (or the
--loop time running out) prints one PASS/FAIL line per preset seen.

Frames under MIN_WIDTH are skipped: Resolve renders filmstrip and node
thumbnails through the node (184x92 and 46x22 seen), and outside Mono packing
or an active stereo pair the plugin streams them — a 46x22 chart would fail
every patch. LUT File isn't covered — its tag names what the LUT produces,
not a transform this script can compute; compare it against Resolve applying
the same .cube on a node instead (BUILD.md).

Standard library only, like the chart generators.
"""
import argparse
import re
import subprocess
import sys
import time

# CIE 1931 xy of the R, G, B primaries; every space here is D65.
PRIMARIES = {
    "bt709": ((0.640, 0.330), (0.300, 0.600), (0.150, 0.060)),
    "p3d65": ((0.680, 0.320), (0.265, 0.690), (0.150, 0.060)),
    "bt2020": ((0.708, 0.292), (0.170, 0.797), (0.131, 0.046)),
}
D65 = (0.3127, 0.3290)
MIN_WIDTH = 320  # narrower frames are Resolve thumbnails, not the program

# Keyed by ndi_color::displaySpaceName (src/OutputColor.h) — the names
# ndi_verify prints for the lsvr_color tag.
SPACES = {
    "Rec.709 Gamma 2.4": ("bt709", 2.4),
    "Rec.709 Gamma 2.2": ("bt709", 2.2),
    "sRGB": ("bt709", "srgb"),
    "P3-D65 Gamma 2.2": ("p3d65", 2.2),
    "P3-D65 Gamma 2.6": ("p3d65", 2.6),
    "Display P3": ("p3d65", "srgb"),
    "Rec.2020 Gamma 2.4": ("bt2020", 2.4),
}

# Keep in sync with PATCHES in scripts/gen_patch_chart.py and kPatches in
# tools/ndi_verify.cpp (row-major from the top-left).
PATCHES = [
    ("0", (0, 0, 0)), ("16", (16, 16, 16)), ("32", (32, 32, 32)),
    ("128", (128, 128, 128)), ("235", (235, 235, 235)), ("255", (255, 255, 255)),
    ("R", (255, 0, 0)), ("G", (0, 255, 0)), ("B", (0, 0, 255)),
    ("C", (0, 255, 255)), ("M", (255, 0, 255)), ("Y", (255, 255, 0)),
    ("0", (0, 0, 0)), ("23", (23, 23, 23)), ("46", (46, 46, 46)),
    ("70", (70, 70, 70)), ("93", (93, 93, 93)), ("116", (116, 116, 116)),
    ("139", (139, 139, 139)), ("162", (162, 162, 162)), ("185", (185, 185, 185)),
    ("209", (209, 209, 209)), ("232", (232, 232, 232)), ("255", (255, 255, 255)),
]


def inverse3(m):
    a, b, c = m[0]
    d, e, f = m[1]
    g, h, i = m[2]
    det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)
    return [
        [(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det],
        [(f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det],
        [(d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det],
    ]


def multiply(m, n):
    return [[sum(m[r][k] * n[k][c] for k in range(3)) for c in range(3)] for r in range(3)]


def apply(m, v):
    return [sum(m[r][k] * v[k] for k in range(3)) for r in range(3)]


def rgb_to_xyz(primaries):
    """RGB->XYZ for these primaries, scaled so RGB 1,1,1 is D65 at Y = 1."""
    xyz = [[x / y, 1.0, (1 - x - y) / y] for x, y in primaries]
    p = [[xyz[c][r] for c in range(3)] for r in range(3)]
    s = apply(inverse3(p), [D65[0] / D65[1], 1.0, (1 - D65[0] - D65[1]) / D65[1]])
    return [[p[r][c] * s[c] for c in range(3)] for r in range(3)]


def decode(curve, v):
    if curve == "srgb":
        return v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4
    return v ** curve


def encode(curve, linear):
    if curve == "srgb":
        return 12.92 * linear if linear <= 0.0031308 else 1.055 * linear ** (1 / 2.4) - 0.055
    return linear ** (1 / curve)


def expected_patches(timeline, output):
    """8-bit R'G'B' each chart patch should arrive as: decode the timeline
    space, convert primaries, clip to the display gamut, encode the output."""
    in_prim, in_curve = SPACES[timeline]
    out_prim, out_curve = SPACES[output]
    m = multiply(inverse3(rgb_to_xyz(PRIMARIES[out_prim])), rgb_to_xyz(PRIMARIES[in_prim]))
    result = []
    for _, rgb in PATCHES:
        linear = apply(m, [decode(in_curve, c / 255.0) for c in rgb])
        result.append(tuple(round(255 * encode(out_curve, min(1.0, max(0.0, l)))) for l in linear))
    return result


def read_stream(ndi_verify, source, seconds):
    """One ndi_verify read: (tag or None, (w, h) or None, received patches)."""
    proc = subprocess.run(
        [ndi_verify, "--source", source, "--duration", str(seconds), "--patches", "--color", "bgra",
         "--patch-tolerance", "255", "--quiet"],
        capture_output=True, text=True, timeout=seconds + 60)
    text = proc.stdout + proc.stderr
    tag = re.search(r"color tag: (.+?) on (\d+)/(\d+) frames", text)
    size = re.search(r"last frame: (\d+)x(\d+)", text)
    received = [tuple(int(v) for v in g.split(","))
                for g in re.findall(r"^\s+\S+\s+\d+,\d+,\d+\s+(\d+,\d+,\d+)\s+\d+", text, re.M)]
    return (tag.group(1) if tag and tag.group(2) == tag.group(3) else None,
            (int(size.group(1)), int(size.group(2))) if size else None,
            received)


def compare(timeline, output, received, tolerance):
    worst, rows = 0, []
    for (label, src), want, got in zip(PATCHES, expected_patches(timeline, output), received):
        diff = max(abs(a - b) for a, b in zip(want, got))
        worst = max(worst, diff)
        rows.append(f"  {label:<4} source {src[0]:>3},{src[1]:>3},{src[2]:>3}"
                    f"  expected {want[0]:>3},{want[1]:>3},{want[2]:>3}"
                    f"  received {got[0]:>3},{got[1]:>3},{got[2]:>3}  diff {diff}"
                    f"{'  <- OFF' if diff > tolerance else ''}")
    return worst, rows


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--source", required=True, help="part of the NDI source name")
    parser.add_argument("--ndi-verify", default="./build/ndi_verify", help="path to ndi_verify (make ndi-verify)")
    parser.add_argument("--timeline", default="Rec.709 Gamma 2.4", choices=list(SPACES),
                        help="the timeline colour space the node sees")
    parser.add_argument("--tolerance", type=int, default=2, help="max codes off per channel (SpeedHQ is lossy)")
    parser.add_argument("--loop", type=float, default=0, metavar="SECONDS",
                        help="keep reading for this long (0 = one read)")
    args = parser.parse_args()

    # The identity case must reproduce the chart exactly, or the oracle is wrong.
    if expected_patches(args.timeline, args.timeline) != [rgb for _, rgb in PATCHES]:
        sys.exit("internal error: identity transform does not reproduce the chart")

    worst_by_tag = {}
    end = time.time() + args.loop
    try:
        while True:
            tag, size, received = read_stream(args.ndi_verify, args.source, 1.5)
            stamp = time.strftime("%H:%M:%S")
            if size and size[0] < MIN_WIDTH:
                print(f"{stamp} skipped ({size[0]}x{size[1]} frame: a Resolve thumbnail)", flush=True)
            elif tag not in SPACES or len(received) != len(PATCHES):
                print(f"{stamp} skipped (tag {tag!r}, {len(received)} patches read)", flush=True)
            else:
                worst, rows = compare(args.timeline, tag, received, args.tolerance)
                print(f"{stamp} {tag}: worst {worst} code(s) -> {'PASS' if worst <= args.tolerance else 'FAIL'}",
                      flush=True)
                if tag not in worst_by_tag or worst > worst_by_tag[tag]:
                    worst_by_tag[tag] = worst
                    print("\n".join(rows), flush=True)
            if time.time() >= end:
                break
    except KeyboardInterrupt:
        pass

    print(f"summary (timeline {args.timeline}, tolerance +/-{args.tolerance}):")
    for tag, worst in worst_by_tag.items():
        print(f"  {tag:<20} worst {worst} code(s)  {'PASS' if worst <= args.tolerance else 'FAIL'}")
    if not worst_by_tag:
        print("  no readable frames")
        sys.exit(2)
    sys.exit(0 if all(w <= args.tolerance for w in worst_by_tag.values()) else 1)


if __name__ == "__main__":
    main()
