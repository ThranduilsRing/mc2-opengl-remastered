#!/usr/bin/env python3
"""Redraw the red in-world meeting marker (data/tgl/128/a_meetingxxxmarker.tga) at 8x texel density (deterministic).

    py -3 tools/gen_world_marker.py            # write the TGA
    py -3 tools/gen_world_marker.py --check    # fail when the checked-in file differs

Same design as the stock 64x64 art: a flat-top hexagon ring with three 120-degree-spaced open triangles whose tips touch
the hexagon vertices, a brighter dot on every vertex. Colour stays the stock red (192,0,0) on every texel (so bilinear
edges never pull another colour in); only alpha carries the shape. Geometry is in stock texel units, drawn at 4x
supersampling and box-filtered to 8 texels per stock unit.
"""
import argparse
import math
import sys
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "data/tgl/128/a_meetingxxxmarker.tga"
STOCK, SCALE, SS = 64, 8, 4
CX, CY, HEX_R = 27.5, 32.0, 12.7
LINE_W, DOT_R, LINE_A = 3.4, 1.35, 165
TIP = (18.0, 0.0)                      # triangle tip, offset from the hexagon centre (stock units)
BASE = ((31.5, -6.6), (31.5, 7.4))     # the other two triangle corners


def rot(p, deg):
    a = math.radians(deg)
    return (CX + p[0] * math.cos(a) - p[1] * math.sin(a), CY + p[0] * math.sin(a) + p[1] * math.cos(a))


def render():
    k = SCALE * SS
    size = STOCK * k
    line = Image.new("L", (size, size), 0)
    dots = Image.new("L", (size, size), 0)
    dl, dd = ImageDraw.Draw(line), ImageDraw.Draw(dots)
    pt = lambda p: (p[0] * k, p[1] * k)
    hexv = [(CX + HEX_R * math.cos(math.radians(60 * i)), CY + HEX_R * math.sin(math.radians(60 * i))) for i in range(6)]
    shapes = [hexv + [hexv[0]]]
    for deg in (0, 120, 240):
        t = [rot(TIP, deg), rot(BASE[0], deg), rot(BASE[1], deg)]
        shapes.append(t + [t[0]])
    corners = hexv + [rot(q, d) for d in (0, 120, 240) for q in (TIP,) + BASE]
    for poly in shapes:
        pts = [pt(p) for p in poly]
        dl.line(pts, fill=LINE_A, width=round(LINE_W * k), joint="curve")
        for p in pts:
            r = LINE_W * k / 2
            dl.ellipse((p[0] - r, p[1] - r, p[0] + r, p[1] + r), fill=LINE_A)
    for p in corners:
        x, y = pt(p)
        r = DOT_R * k
        dd.ellipse((x - r, y - r, x + r, y + r), fill=255)
    alpha = Image.composite(dots, line, dots.point(lambda v: 255 if v else 0)).resize((STOCK * SCALE,) * 2, Image.BOX)
    img = Image.new("RGBA", alpha.size, (192, 0, 0, 0))
    img.putalpha(alpha)
    return img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()
    img = render()
    if args.check:
        same = OUT.exists() and Image.open(OUT).convert("RGBA").tobytes() == img.tobytes()
        print("OK" if same else f"differs: {OUT}")
        return 0 if same else 1
    img.save(OUT, format="TGA", rle=False)
    print(f"wrote {OUT} {img.size}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
