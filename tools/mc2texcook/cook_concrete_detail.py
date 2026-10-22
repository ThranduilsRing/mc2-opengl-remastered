#!/usr/bin/env python3
"""cook_concrete_detail.py -- cook the concrete/road PBR DETAIL maps (detail only, never colour).

Each output is a BC7 linear KTX2, RGBA = (normal.x, normal.y, roughness, ao), 1024x1024, full mip chain:
  concrete_detail_pad.ktx2   <- Poly Haven concrete_layers_02 (CC0): nor_gl + rough; AO = cavity derived from disp
  concrete_detail_road.ktx2  <- Poly Haven clean_asphalt (CC0): nor_gl + ARM (AO, rough)
Normal z is rebuilt in the shader. The albedo of these sets is never read: MC2 keeps its own colour.

Usage: py -3 tools/mc2texcook/cook_concrete_detail.py --src G:/mc2-assets/pbr/concrete-road --out data/terrain_layers
"""
import argparse
import os
import sys
import tempfile

os.environ.setdefault("OPENCV_IO_ENABLE_OPENEXR", "1")
import cv2  # noqa: E402
import numpy as np  # noqa: E402
from PIL import Image  # noqa: E402

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cook_pbr_maps as pbr  # noqa: E402

SIZE = 1024
KTX = "A:/Games/mc2-tools/ktx/ktx.exe"


def load(path):
    img = cv2.imread(path, cv2.IMREAD_UNCHANGED)
    if img is None:
        raise RuntimeError("cannot read " + path)
    if img.dtype == np.uint8:
        img = img.astype(np.float32) / 255.0
    elif img.dtype == np.uint16:
        img = img.astype(np.float32) / 65535.0
    else:
        img = img.astype(np.float32)
    if img.ndim == 3 and img.shape[2] >= 3:
        img = img[:, :, [2, 1, 0]]          # BGR -> RGB
    return np.clip(img, 0.0, 1.0)


def rs(a):
    return cv2.resize(a, (SIZE, SIZE), interpolation=cv2.INTER_AREA)


def gray(a):
    return a if a.ndim == 2 else a[:, :, 0]


def pad_maps(src):
    d = os.path.join(src, "concrete_layers_02", "textures")
    n = rs(load(os.path.join(d, "concrete_layers_02_nor_gl_2k.exr")))
    r = rs(gray(load(os.path.join(d, "concrete_layers_02_rough_2k.exr"))))
    h = rs(gray(load(os.path.join(d, "concrete_layers_02_disp_2k.png"))))
    # no AO channel in this set: cavity = how far the height sits below its local mean (only ever darkens)
    blur = cv2.GaussianBlur(h, (0, 0), 24)
    ao = np.clip(1.0 + (h - blur) * 4.0, 0.0, 1.0)
    return n[:, :, 0], n[:, :, 1], r, ao


def road_maps(src):
    d = os.path.join(src, "clean_asphalt", "textures")
    n = rs(load(os.path.join(d, "clean_asphalt_nor_gl_2k.jpg")))
    arm = rs(load(os.path.join(d, "clean_asphalt_arm_2k.jpg")))
    ao = np.clip(arm[:, :, 0] / max(float(np.percentile(arm[:, :, 0], 90)), 1e-3), 0.0, 1.0)
    return n[:, :, 0], n[:, :, 1], arm[:, :, 1], ao


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--ktx-exe", default=KTX)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    for name, fn in (("pad", pad_maps), ("road", road_maps)):
        nx, ny, rough, ao = fn(a.src)
        rgba = (np.stack([nx, ny, rough, ao], axis=2) * 255.0 + 0.5).astype(np.uint8)
        with tempfile.TemporaryDirectory() as td:
            png = os.path.join(td, "concrete_detail_%s.png" % name)
            Image.fromarray(rgba, "RGBA").save(png)
            out = os.path.join(a.out, "concrete_detail_%s.ktx2" % name)
            if not pbr.cook_png_to_bc7(a.ktx_exe, png, out, "R8G8B8A8_UNORM", "linear", td, mips=True):
                sys.exit("cook failed: " + name)
            print("wrote", out, os.path.getsize(out), "bytes")


if __name__ == "__main__":
    main()
