#!/usr/bin/env python3
"""Bake a base-colour texture for an MW5 mech: MW5 ships no albedo, only an RGB paint-zone mask plus
material parameters. albedo = bare metal where the mask is black, a flat zone colour where it is lit.

  py -3 tools/mw5_mechs/bake_albedo.py --mask Timberwolf_Body_Default_MSK.png --out mw5_madcat_alb.tga [--size 2048]
"""
import argparse

import numpy as np
from PIL import Image

METAL = (0.36, 0.35, 0.33)   # BaseMech_MTI MetalColorMultiplier (0.63, 0.62, 0.57) darkened for an albedo
ZONES = ((0.27, 0.30, 0.21), (0.52, 0.46, 0.30), (0.42, 0.13, 0.10))   # R, G, B zones: olive, tan, dark red


def bake(mask_path, size):
    m = np.asarray(Image.open(mask_path).convert("RGBA").resize((size, size), Image.BILINEAR), dtype=np.float32) / 255.0
    out = np.empty(m.shape[:2] + (3,), np.float32)
    out[:] = METAL
    for ch, col in zip((0, 1, 2), ZONES):
        w = m[..., ch:ch + 1]
        out = out * (1 - w) + np.array(col, np.float32) * w
    srgb = np.clip(out, 0, 1) ** (1 / 2.2)
    return Image.fromarray((srgb * 255 + 0.5).astype(np.uint8), "RGB")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--mask", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--size", type=int, default=2048)
    a = ap.parse_args()
    bake(a.mask, a.size).save(a.out)
