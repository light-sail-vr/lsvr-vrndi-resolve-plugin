#!/usr/bin/env python3
"""Generate an STMap EXR for testing the Equirect (STMap) warp at real sizes.

The STMap fixtures in tests/fixtures/ are 32x16, so the warp's output (the
map's size) is too small to measure or look at. This writes a map of any size
in the convention src/STMap.h reads: an uncompressed float32 scanline EXR,
rows stored top-down, R = source U and G = source V, both normalized with a
bottom-left origin (Fusion/Nuke).

Maps
----
* identity: every output pixel samples its own position. On a timeline the
  map's size, the stream must equal Passthrough, so
  `ndi_verify --patches` on the patch chart checks the warp end to end.
* mirror: U flipped. Labels on the resolution chart read backwards, which
  shows at a glance that the warp, not the plain downscale, is running.

Usage
-----
  scripts/gen_stmap_exr.py --width 1920 --height 1080 --out identity_1920x1080.exr
  scripts/gen_stmap_exr.py --width 4096 --height 4096 --map mirror --out mirror_4096.exr

Standard library only, like the chart generators.
"""

import argparse
import struct
import sys
from array import array


def attr(name, type_name, payload):
    return name.encode() + b"\0" + type_name.encode() + b"\0" + struct.pack("<i", len(payload)) + payload


def header(width, height):
    channels = b""
    for name in ("G", "R"):  # EXR stores channels sorted by name
        channels += name.encode() + b"\0" + struct.pack("<iB3xii", 2, 0, 1, 1)  # 2 = FLOAT
    channels += b"\0"
    box = struct.pack("<iiii", 0, 0, width - 1, height - 1)
    return (b"\x76\x2f\x31\x01" + struct.pack("<i", 2)
            + attr("channels", "chlist", channels)
            + attr("compression", "compression", b"\0")  # NO_COMPRESSION
            + attr("dataWindow", "box2i", box)
            + attr("displayWindow", "box2i", box)
            + attr("lineOrder", "lineOrder", b"\0")  # INCREASING_Y
            + attr("pixelAspectRatio", "float", struct.pack("<f", 1.0))
            + attr("screenWindowCenter", "v2f", struct.pack("<ff", 0.0, 0.0))
            + attr("screenWindowWidth", "float", struct.pack("<f", 1.0))
            + b"\0")


def write_map(path, width, height, mirror):
    us = array("f", ((x + 0.5) / width for x in range(width)))
    if mirror:
        us.reverse()
    if sys.byteorder != "little":
        us.byteswap()
    u_bytes = us.tobytes()
    head = header(width, height)
    line_bytes = 8 + 2 * 4 * width
    first = len(head) + 8 * height
    with open(path, "wb") as f:
        f.write(head)
        f.write(b"".join(struct.pack("<Q", first + y * line_bytes) for y in range(height)))
        for y in range(height):  # row 0 = top
            v = (height - 1 - y + 0.5) / height
            f.write(struct.pack("<ii", y, 2 * 4 * width))
            f.write(struct.pack("<f", v) * width)  # G, then R
            f.write(u_bytes)


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--width", type=int, required=True)
    p.add_argument("--height", type=int, required=True)
    p.add_argument("--map", choices=("identity", "mirror"), default="identity")
    p.add_argument("--out", required=True)
    a = p.parse_args()
    if a.width < 1 or a.height < 1:
        p.error("width and height must be positive")
    write_map(a.out, a.width, a.height, a.map == "mirror")
    print(f"wrote {a.out}: {a.width}x{a.height} {a.map} STMap")


if __name__ == "__main__":
    main()
