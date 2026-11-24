#!/usr/bin/env python3
"""cook_forest_floor.py -- cook the forest-floor detail normal (detail-normal array layer 6, the under-forest ground).

Owner 2026-11-24: "I don't think we have the under forest ground in yet." Source: Poly Haven forest_ground_04 4k nor_gl (CC0),
extracted from the owner's GameAsset download to E:/mc2-ref/terrain-normals-2026-11-24 (SOURCES.txt there holds pack and licence).
The array needs one layer size, so the 4096 source is box-reduced to 2048 (one forest tile is 384 wu at the shipped tiling).
Output data/terrain_layers/forest_floor_normal.ktx2: 2048x2048 R8G8B8A8_UNORM + mip chain, RGB = GL tangent normal, A = 128.
Usage: py -3 tools/mc2texcook/cook_forest_floor.py --out data/terrain_layers [--src DIR]
"""
import argparse
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cook_terrain_layers as ctl  # noqa: E402

SIZE = 2048
OUT_NAME = "forest_floor_normal.ktx2"
SOURCE_DIR = "E:/mc2-ref/terrain-normals-2026-11-24"  # drive-path: ok PC-only cook source
SOURCE = "forest_ground_04_4k.png"


def forest_texels(rgb):
    img = Image.fromarray(rgb).resize((SIZE, SIZE), Image.BOX) if rgb.shape[0] != SIZE else Image.fromarray(rgb)
    n = np.asarray(img).astype(np.float64) / 127.5 - 1.0
    n /= np.maximum(np.linalg.norm(n, axis=-1, keepdims=True), 1e-6)
    out = np.empty((SIZE, SIZE, 4), np.uint8)
    out[..., :3] = np.clip(np.rint((n + 1.0) * 127.5), 0, 255)
    out[..., 3] = 128
    return out


def cook(out_dir, src_dir=SOURCE_DIR):
    # deploy_payload passes the shared ground-pack dir to every cooker; the forest source lives in SOURCE_DIR instead.
    path = os.path.join(src_dir, SOURCE)
    if not os.path.isfile(path):
        path = os.path.join(SOURCE_DIR, SOURCE)
    rgb = np.asarray(Image.open(path).convert("RGB"))
    os.makedirs(out_dir, exist_ok=True)
    dst = os.path.join(out_dir, OUT_NAME)
    ctl.write_rgba8_ktx2(dst + ".tmp", ctl.box_mips(forest_texels(rgb)))
    os.replace(dst + ".tmp", dst)
    return dst


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True)
    ap.add_argument("--src", default=SOURCE_DIR)
    a = ap.parse_args()
    print(cook(a.out, a.src))


if __name__ == "__main__":
    main()
