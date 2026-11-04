#!/usr/bin/env python3
"""cook_cliff_rock.py -- cook the CC0 cliff ROCK material for steep VK terrain faces (TERRAIN-CLIFF-ROCK).

Per set two BC7 linear KTX2 layers, 1024x1024, full mip chain (they join the concrete detail array, binding 18, as layers 2 and 3):
  cliff_rock_<set>_ah.ktx2   RGB = albedo RATIO to the set's mean (linear light, half chroma, 0.5 = mean, encoded x0.5), A = height
  cliff_rock_<set>_nra.ktx2  RGBA = (normal.x, normal.y, roughness, ao), GL-convention normal
The albedo is a ratio, never a colour: the shader multiplies it onto a tone taken from MC2's own colormap, so the palette stays.
With --select <set> the chosen set is also written as cliff_rock_ah.ktx2 / cliff_rock_nra.ktx2, the names the engine loads.

Sets (all CC0, sources and licences in tools/mc2texcook/cliff_rock_provenance.json):
  marble    Poly Haven marble_cliff_01 (2k glTF: diff, nor_gl, arm) + marble_cliff_01_disp_4k.png
  riverbed  Poly Haven dry_riverbed_rock (2k: diff, nor_gl exr, rough exr, disp)
  rocks05   Poly Haven rocks_ground_05 (4k: diff, nor_gl exr, rough, disp)

Usage: py -3 tools/mc2texcook/cook_cliff_rock.py --src C:/Users/Joe/Downloads/GameAsset/Terrain --out data/terrain_layers --select marble
"""
import argparse
import os
import sys
import tempfile
import zipfile

os.environ.setdefault("OPENCV_IO_ENABLE_OPENEXR", "1")
import cv2  # noqa: E402
import numpy as np  # noqa: E402
from PIL import Image  # noqa: E402

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cook_pbr_maps as pbr  # noqa: E402

SIZE = 1024
KTX = "A:/Games/mc2-tools/ktx/ktx.exe"

SETS = {
    "marble": ("marble_cliff_01_2k.gltf.zip", {"diff": "textures/marble_cliff_01_diff_2k.jpg", "nor": "textures/marble_cliff_01_nor_gl_2k.jpg",
                                                "arm": "textures/marble_cliff_01_arm_2k.jpg", "disp@": "marble_cliff_01_disp_4k.png"}),
    "riverbed": ("dry_riverbed_rock_2k.blend.zip", {"diff": "textures/dry_riverbed_rock_diff_2k.jpg", "nor": "textures/dry_riverbed_rock_nor_gl_2k.exr",
                                                    "rough": "textures/dry_riverbed_rock_rough_2k.exr", "disp": "textures/dry_riverbed_rock_disp_2k.png"}),
    "rocks05": ("rocks_ground_05_4k.blend.zip", {"diff": "textures/rocks_ground_05_diff_4k.jpg", "nor": "textures/rocks_ground_05_nor_gl_4k.exr",
                                                 "rough": "textures/rocks_ground_05_rough_4k.jpg", "disp": "textures/rocks_ground_05_disp_4k.png"}),
}


def decode(data, name):
    img = cv2.imdecode(np.frombuffer(data, np.uint8), cv2.IMREAD_UNCHANGED)
    if img is None:
        raise RuntimeError("cannot decode " + name)
    if img.dtype == np.uint8:
        img = img.astype(np.float32) / 255.0
    elif img.dtype == np.uint16:
        img = img.astype(np.float32) / 65535.0
    else:
        img = img.astype(np.float32)
    if img.ndim == 3 and img.shape[2] >= 3:
        img = img[:, :, [2, 1, 0]]
    return cv2.resize(np.clip(img, 0.0, 1.0), (SIZE, SIZE), interpolation=cv2.INTER_AREA)


def gray(a):
    return a if a.ndim == 2 else a[:, :, 0]


def blur_wrap(a, sigma):
    n = a.shape[0]
    return cv2.GaussianBlur(np.tile(a, (3, 3)), (0, 0), sigma)[n:2 * n, n:2 * n]


def load_set(src, name):
    pack, files = SETS[name]
    m = {}
    with zipfile.ZipFile(os.path.join(src, pack)) as z:
        for k, f in files.items():
            if k.endswith("@"):
                with open(os.path.join(src, f), "rb") as fh:
                    m[k[:-1]] = decode(fh.read(), f)
            else:
                m[k] = decode(z.read(f), f)
    return m


def cook_set(m):
    srgb = m["diff"][:, :, :3]
    lin = np.where(srgb <= 0.04045, srgb / 12.92, ((srgb + 0.055) / 1.055) ** 2.4)
    ratio = lin / np.maximum(lin.reshape(-1, 3).mean(axis=0), 1e-4)
    lum = (ratio * np.array([0.2126, 0.7152, 0.0722], np.float32)).sum(axis=2, keepdims=True)
    ratio = lum + (ratio - lum) * 0.5                       # half the scan's chroma: MC2's tone carries the hue
    h = gray(m["disp"])
    h = (h - np.percentile(h, 1)) / max(float(np.percentile(h, 99) - np.percentile(h, 1)), 1e-4)
    h = np.clip(h, 0.0, 1.0)
    nx, ny = m["nor"][:, :, 0], m["nor"][:, :, 1]
    if "arm" in m:
        ao, rough = m["arm"][:, :, 0], m["arm"][:, :, 1]
    else:
        rough = gray(m["rough"])
        cav = blur_wrap(h, 12.0) - h                          # cavity: below the local mean height
        ao = np.clip(1.0 - np.maximum(cav, 0.0) * 2.5, 0.35, 1.0)
    ah = np.dstack([np.clip(ratio * 0.5, 0.0, 1.0), h])
    nra = np.dstack([nx, ny, rough, ao])
    return ah, nra


def write_ktx(ktx_exe, rgba, out):
    px = (np.clip(rgba, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)
    with tempfile.TemporaryDirectory(dir=os.environ.get("TMP")) as td:
        png = os.path.join(td, os.path.basename(out) + ".png")
        Image.fromarray(px, "RGBA").save(png)
        if not pbr.cook_png_to_bc7(ktx_exe, png, out, "R8G8B8A8_UNORM", "linear", td, mips=True):
            sys.exit("cook failed: " + out)
    print("wrote", out, os.path.getsize(out), "bytes")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--sets", default=",".join(SETS))
    ap.add_argument("--select", default="", help="also write this set under the engine's names")
    ap.add_argument("--ktx-exe", default=KTX)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    for name in [s for s in a.sets.split(",") if s]:
        ah, nra = cook_set(load_set(a.src, name))
        targets = [("cliff_rock_%s_ah.ktx2" % name, ah), ("cliff_rock_%s_nra.ktx2" % name, nra)]
        if name == a.select:
            targets += [("cliff_rock_ah.ktx2", ah), ("cliff_rock_nra.ktx2", nra)]
        for fn, img in targets:
            write_ktx(a.ktx_exe, img, os.path.join(a.out, fn))


if __name__ == "__main__":
    main()
