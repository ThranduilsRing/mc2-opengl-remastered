#!/usr/bin/env python3
"""Cook the campaign mission marker widgets at 4x texel density (deterministic).

    py -3 tools/gen_campaign_marker.py            # write data/art/ui/mcl_cm_5/*.png
    py -3 tools/gen_campaign_marker.py --check    # fail when the checked-in files differ

data/art/ui/manifest.json lists the sheet "mcl_cm_5" with "scale": 4: rects, pad and the FIT uv stay in the
stock 51x59 cell units (the draw rect on screen is unchanged), the widget files carry 4x the texels.
The marker art is the targeting reticle (bracket corners, ring and centre pip; the pressed/selected cell adds
crosshair ticks), drawn as vector shapes at 16 samples per cell unit and box-filtered down to 4 texels per unit.
The art is white with a dark neutral halo, so the stock FIT colour 0xffff0000 multiplies to a crisp red.
Transparent texels keep the halo colour, so bilinear sampling never pulls black into the edges.
The three layout-only widgets of the sheet (cmstatic33..35) are nearest-neighbour 4x copies of the stock art.
"""
import argparse
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "data/art/ui/mcl_cm_5"
SCALE = 4          # texels per cell unit in the widget files
SS = 4             # extra supersampling per texel when drawing
U = SCALE * SS     # drawing samples per cell unit
WHITE = (255, 255, 255)
HALO = (16, 16, 16)  # neutral dark outline; never pure black (pure black is the engine's key colour)
CELL_W, CELL_H = 51, 59
# widget name -> (widget file extent in cell units: x0, y0, w, h in sheet units, cell origin x, kind)
WIDGETS = {
    "mcl_cm_op1_1.button0.disabled": ((50, 0, 53, 60), 51, "normal"),    # uv 51,0: normal / hover / disabled
    "mcl_cm_op1_1.button0.pressed": ((0, 0, 52, 60), 0, "selected"),     # uv 0,0: pressed / selected
}
NEAREST_ONLY = ["mcl_cm_layout.cmstatic33", "mcl_cm_layout.cmstatic34", "mcl_cm_layout.cmstatic35"]


def line(d, pts, w, close=False):
    if close:
        pts = pts + [pts[0]]
    d.line([(x * U, y * U) for x, y in pts], fill=255, width=round(w * U), joint="curve")
    r = w * U / 2
    for x, y in pts:
        d.ellipse([x * U - r, y * U - r, x * U + r, y * U + r], fill=255)


def ring(d, cx, cy, r, w):
    d.ellipse([(cx - r) * U, (cy - r) * U, (cx + r) * U, (cy + r) * U], outline=255, width=round(w * U))


def disc(d, cx, cy, r):
    d.ellipse([(cx - r) * U, (cy - r) * U, (cx + r) * U, (cy + r) * U], fill=255)


def brackets(d, cx, cy, half, arm, w):
    for sx in (-1, 1):
        for sy in (-1, 1):
            x, y = cx + sx * half, cy + sy * half
            line(d, [(x - sx * arm, y), (x, y), (x, y - sy * arm)], w)


def reticle(d, cx, cy, kind):
    if kind == "normal":
        brackets(d, cx, cy, 14, 6, 2.0)
        ring(d, cx, cy, 5, 1.4)
        disc(d, cx, cy, 1.6)
    else:
        brackets(d, cx, cy, 19, 9, 2.6)
        ring(d, cx, cy, 8, 1.6)
        disc(d, cx, cy, 3.2)
        for sg in (-1, 1):
            line(d, [(cx, cy + sg * 8), (cx, cy + sg * 13)], 1.6)
            line(d, [(cx + sg * 8, cy), (cx + sg * 13, cy)], 1.6)


def render(extent, cell_x, kind):
    x0, y0, w, h = extent
    mask = Image.new("L", (w * U, h * U), 0)
    d = ImageDraw.Draw(mask)
    # centre of the 51x59 cell, in widget-local cell units
    reticle(d, cell_x - x0 + CELL_W / 2, CELL_H / 2, kind)
    mask = mask.resize((w * SCALE, h * SCALE), Image.BOX)
    halo = mask.filter(ImageFilter.MaxFilter(2 * SCALE + 1)).filter(ImageFilter.GaussianBlur(SCALE * 0.5))
    out = Image.new("RGBA", mask.size, HALO + (0,))
    mp, hp, op = mask.load(), halo.load(), out.load()
    for y in range(mask.size[1]):
        for x in range(mask.size[0]):
            m = mp[x, y]
            ha = min(255, int(hp[x, y] * 0.85))
            a = m + ha * (255 - m) // 255
            if a == 0:
                continue
            c = [(WHITE[i] * m + HALO[i] * ha * (255 - m) // 255) // a for i in range(3)]
            op[x, y] = (c[0], c[1], c[2], a)
    return out


STOCK_SIZE = {"mcl_cm_layout.cmstatic33": (2, 2), "mcl_cm_layout.cmstatic34": (53, 65),
              "mcl_cm_layout.cmstatic35": (52, 75)}   # the 1x padded widget sizes


def build():
    files = {}
    for name, (extent, cell_x, kind) in WIDGETS.items():
        files[name] = render(extent, cell_x, kind)
    for name in NEAREST_ONLY:
        with Image.open(OUT / (name + ".png")) as im:
            im = im.convert("RGBA")
        if im.size == STOCK_SIZE[name]:   # still 1x: upscale once; a 4x file is kept as is
            im = im.resize((im.size[0] * SCALE, im.size[1] * SCALE), Image.NEAREST)
        files[name] = im
    return files


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    files = build()
    bad = 0
    for name, im in files.items():
        path = OUT / (name + ".png")
        if a.check:
            with Image.open(path) as cur:
                if cur.convert("RGBA").tobytes() != im.tobytes() or cur.size != im.size:
                    print("differs:", path)
                    bad += 1
        else:
            im.save(path, optimize=True)
            print("wrote", path, im.size)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
