#!/usr/bin/env python3
"""TERRAIN-REGEN-1: classify a stock mission's terrain by what it mimics.

Inputs: sim heights + per-vertex overlay handle (mission .pak), terrain objects, the shipped
baked colormap.  Works on a --size x --size raster (default 1024, bicubic height upsample, so
slope is smoother than the 128 wu grid but the SIM data is untouched).  Output (--out):
  <m>_class.png  colour class map     <m>_class_id.png  uint8 ids     <m>_legend.png/.json (areas)
Classes are heuristics (slope/height/overlay/colormap colour); see docs/terrain-regen-mc01.md.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw
from scipy.ndimage import binary_dilation, zoom, uniform_filter

sys.path.insert(0, str(Path(__file__).resolve().parent))
import export_mission as E  # noqa: E402

WU = E.WU
CLASSES = [  # id, name, rgb, mimics
    (0, "water", (30, 70, 160), "open water"),
    (1, "beach / water edge", (240, 220, 140), "sand / shoreline band"),
    (2, "cliff / rock face", (120, 40, 40), "vertical rock: replace with real cliff geometry"),
    (3, "steep rocky slope", (200, 110, 60), "scree / broken rock slope"),
    (4, "road / concrete", (150, 150, 150), "paved road, pads, runway (overlay tiles)"),
    (5, "structure footprint", (255, 0, 255), "building / prop footprint"),
    (6, "dirt / bare ground", (160, 120, 70), "dry earth, trails"),
    (7, "grass", (140, 200, 80), "open grass"),
    (8, "jungle / forest floor", (20, 100, 40), "dark dense vegetation floor"),
    (9, "other ground", (200, 200, 200), "unclassified flat ground"),
]


def classify(m: dict, cmap: Image.Image, size: int, p: dict) -> np.ndarray:
    side, elev = m["side"], m["elev"]
    ext = side * WU
    # vertex (i,j) sits at world x=-ext/2+j*WU: raster pixel centre -> same frame, cell-aligned to ext
    h = zoom(elev, size / side, order=3, mode="nearest")
    pix = ext / size
    gy, gx = np.gradient(h, pix)
    slope = np.degrees(np.arctan(np.hypot(gx, gy)))
    slope_s = uniform_filter(slope, 3)
    rgb = np.asarray(cmap.convert("RGB").resize((size, size), Image.BILINEAR), np.float32) / 255
    lum = rgb.mean(-1)
    greenness = rgb[..., 1] - 0.5 * (rgb[..., 0] + rgb[..., 2])
    # overlay: nearest-neighbour from the vertex grid
    ov_hi = (m["texdata"] >> 16) & 0xFFFF
    ov = (ov_hi != 0) & (ov_hi != 0xFFFF)
    ov = np.asarray(Image.fromarray(ov.astype(np.uint8) * 255).resize((size, size), Image.NEAREST)) > 0
    foot = np.zeros((size, size), bool)
    for ot, x, y, z, rot in m["objects"]:
        c = int((x + ext / 2) / pix)
        r = int((ext / 2 - y) / pix)
        rad = max(2, int(p["foot_wu"] / pix))
        foot[max(0, r - rad):r + rad + 1, max(0, c - rad):c + rad + 1] = True

    water = h <= m["water_elev"]
    edge = (~water) & binary_dilation(water, iterations=int(p["edge_wu"] / pix)) & (slope_s < p["steep"])
    beach = edge | ((~water) & (h < m["water_elev"] + p["beach_h"]) & (slope_s < p["steep"]))
    cls = np.full((size, size), 9, np.uint8)
    cls[(greenness > p["green"])] = 7
    cls[(greenness > p["green"]) & (lum < p["jungle_lum"])] = 8
    cls[(greenness <= p["green"]) & (rgb[..., 0] > rgb[..., 2] + 0.03)] = 6
    cls[slope_s >= p["steep"]] = 3
    cls[slope_s >= p["cliff"]] = 2
    cls[beach] = 1
    cls[ov & (slope_s < p["steep"])] = 4
    cls[foot & ~np.isin(cls, (1, 2, 3))] = 5  # prop boxes never replace shore / cliff / steep
    cls[water] = 0
    return cls, slope_s, rgb


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("mission")
    ap.add_argument("--out", required=True)
    ap.add_argument("--install", help="game install dir (colormap); else $MC2_INSTALL")
    ap.add_argument("--size", type=int, default=1024)
    ap.add_argument("--cliff", type=float, default=38.0, help="deg, cliff / rock face")
    ap.add_argument("--steep", type=float, default=24.0, help="deg, steep rocky slope")
    a = ap.parse_args()
    res = E.resolve_missions_dir(None, [a.mission])
    m = E.load_mission(a.mission, res.dir)
    cm = E.find_colormap(a.mission, E.install_dir(a.install))
    cmap = Image.open(cm).convert("RGB").resize((a.size, a.size), Image.LANCZOS)
    p = {"cliff": a.cliff, "steep": a.steep, "edge_wu": 256.0, "beach_h": 30.0, "green": 0.02,
         "jungle_lum": 0.28, "foot_wu": 96.0}
    cls, slope, rgb = classify(m, cmap, a.size, p)
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    pal = np.zeros((256, 3), np.uint8)
    for i, n, c, d in CLASSES:
        pal[i] = c
    Image.fromarray(cls).save(out / f"{a.mission}_class_id.png")
    img = Image.fromarray(pal[cls])
    # legend strip
    leg = Image.new("RGB", (a.size, 24 * len(CLASSES) + 8), (24, 24, 24))
    dr = ImageDraw.Draw(leg)
    land = max(int((cls != 0).sum()), 1)
    stats_map = {}
    stats = {}
    for k, (i, n, c, d) in enumerate(CLASSES):
        cnt = int((cls == i).sum())
        pct = 100.0 * cnt / land  # percent of LAND (water class 0 reports its share of the whole map)
        if i == 0:
            pct = 100.0 * cnt / cls.size
        stats[n] = round(pct, 2)
        stats_map[n] = round(100.0 * cnt / cls.size, 2)
        dr.rectangle([6, 8 + 24 * k, 26, 24 + 24 * k], fill=c)
        dr.text((34, 10 + 24 * k), f"{i} {n}  {pct:.1f}%  - {d}", fill=(235, 235, 235))
    sheet = Image.new("RGB", (a.size, a.size + leg.height))
    sheet.paste(img, (0, 0))
    sheet.paste(leg, (0, a.size))
    sheet.save(out / f"{a.mission}_class.png")
    (out / f"{a.mission}_legend.json").write_text(json.dumps(
        {"pct_basis": "land (class 0 water: whole map)", "land_fraction": round(land / cls.size, 4), "params": p, "classes": [{"id": i, "name": n, "rgb": c, "mimics": d, "pct_land": stats[n], "pct_map": stats_map[n]}
                                  for i, n, c, d in CLASSES]}, indent=2))
    print(json.dumps(stats, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
