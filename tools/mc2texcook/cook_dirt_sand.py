#!/usr/bin/env python3
"""cook_dirt_sand.py -- cook the sand/gravel ground detail normal for the terrain dirt layer (TERRAIN-NORMAL-UNIFORM).

Owner 2026-11-24 (Desert Fox): "Some areas have strong normal map details and others do not", "I want high res ground".
The stock dirt layer (mat2_normal.tga) is almost flat (rms slope 0.065, fine-detail rms 0.03 against 0.27 for grass and rock),
so sand showed no relief beside the rocky band. This cooks a replacement with the same 2048 RGBA8 layout as the matN_normal.ktx2
sidecars (detail-normal array layer 2, MAT_LAYER_DIRT):

  variants Poly Haven (CC0) sand_01 (rippled sand), aerial_beach_01 (packed sand), ganges_river_pebbles (pebbly), dry_ground_01
           (cracked earth), blended by tileable 40..130 wu patch masks so neighbouring patches differ
  stones   tileable small domes of 2..5 world units, sparse
  ripples  tileable band noise of 25..70 world units, gentle undulation that survives the far mips

Deterministic (fixed seed). RGB = GL tangent normal, A = height.

Usage: py -3 tools/mc2texcook/cook_dirt_sand.py --src G:/mc2-assets/pbr/dl-2026-10-23/ground --out data/terrain_layers
"""
import argparse
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cook_grass_turf as turf  # noqa: E402  (tileable noise and slope helpers)
import cook_terrain_layers as ctl  # noqa: E402

SIZE = 2048
OUT_NAME = "dirt_sand_normal.ktx2"
# Four ground variants blended by tileable patch masks: neighbouring patches of the same class look different.
# (path under the Poly Haven ground dir, or "@name" under EXTRA_SRC for the owner-pack extractions, gain, repeats per tile)
EXTRA_SRC = "E:/mc2-ref/terrain-normals-2026-11-24"  # drive-path: ok PC-only cook source (SOURCES.txt there holds pack and licence)
VARIANTS = (
    ("sand_01/textures/sand_01_nor_gl_2k.jpg", 0.9, 1),                  # rippled granular sand
    ("@aerial_beach_01_2k.png", 1.3, 1),                                 # packed beach sand (fine)
    ("@ganges_river_pebbles_2k.png", 0.8, 4),                         # pebbly sand
    ("dry_ground_01/textures/dry_ground_01_nor_gl_2k.jpg", 1.0, 1),      # cracked dry earth
)
SEED = 20261124
# One dirt tile is 384 wu at the shipped tiling: 5.33 texels per wu.
STONES = ((9.0, 0.55), (16.0, 0.45))   # dome radii in texels and their share of the count
STONE_COUNT = 5000
RIPPLE_BAND = (130.0, 380.0)
PATCH_BAND = (220.0, 700.0)            # variant patch wavelengths in texels (40..130 wu)
STONE_SLOPE = 2.5
RIPPLE_SLOPE = 5.0


def sand_texels(variants, seed=SEED, size=SIZE):
    rng = np.random.default_rng(seed)
    acc = np.zeros((size, size))
    y, x = np.mgrid[0:size, 0:size]
    dy = np.minimum(y, size - y).astype(np.float64)
    dx = np.minimum(x, size - x).astype(np.float64)
    d2 = dx * dx + dy * dy
    for radius, share in STONES:
        imp = np.zeros((size, size))
        n = int(STONE_COUNT * share)
        imp[rng.integers(0, size, n), rng.integers(0, size, n)] = rng.uniform(0.5, 1.0, n)
        kernel = np.clip(1.0 - d2 / (radius * radius), 0.0, None) ** 1.5
        acc += np.real(np.fft.ifft2(np.fft.fft2(imp) * np.fft.fft2(kernel)))
    stones = 1.0 - np.exp(-1.4 * acc)
    ripple = turf._band_noise(rng, size, *RIPPLE_BAND) * 0.5
    height = stones + 0.5 * ripple
    sx, sy = turf._slopes(stones, STONE_SLOPE)
    rx, ry = turf._slopes(ripple, RIPPLE_SLOPE)
    # Softmax of one noise field per variant: weights sum to 1, patches blend over a few wu.
    fields = np.stack([turf._band_noise(rng, size, *PATCH_BAND) * 2.2 for _ in variants])
    weights = np.exp(fields - fields.max(0)); weights /= weights.sum(0)
    nx = sx + rx
    ny = sy + ry
    for (rgb, gain, rep), w in zip(variants, weights):
        vx, vy = turf._normal_slopes(rgb, size // rep)
        if rep > 1: vx, vy = np.tile(vx, (rep, rep)), np.tile(vy, (rep, rep))
        nx = nx + vx * gain * w
        ny = ny + vy * gain * w
    inv = 1.0 / np.sqrt(nx * nx + ny * ny + 1.0)
    n = np.stack([nx * inv, ny * inv, inv], -1)
    out = np.empty((size, size, 4), np.uint8)
    out[..., :3] = np.clip(np.rint((n + 1.0) * 127.5), 0, 255)
    h = (height - height.min()) / max(np.ptp(height), 1e-9)
    out[..., 3] = np.clip(np.rint(h * 255.0), 0, 255)
    return out


def load_sources(src_dir):
    return tuple((np.asarray(Image.open(os.path.join(EXTRA_SRC if p[0] == "@" else src_dir, p.lstrip("@"))).convert("RGB")), g, r)
                 for p, g, r in VARIANTS)


def cook(src_dir, out_dir, preview=None):
    texels = sand_texels(load_sources(src_dir))
    os.makedirs(out_dir, exist_ok=True)
    dst = os.path.join(out_dir, OUT_NAME)
    staged = dst + ".tmp"
    ctl.write_rgba8_ktx2(staged, ctl.box_mips(texels))
    os.replace(staged, dst)
    if preview:
        Image.fromarray(texels[..., :3]).save(preview)
    return dst


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--src", required=True, help="dir holding sand_01/ and dry_ground_01/ (Poly Haven downloads)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--preview", help="optional PNG of the cooked normal (RGB)")
    a = ap.parse_args()
    print(cook(a.src, a.out, a.preview))


if __name__ == "__main__":
    main()
