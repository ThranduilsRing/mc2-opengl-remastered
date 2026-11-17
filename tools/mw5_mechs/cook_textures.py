#!/usr/bin/env python3
"""Cook MW5 albedo + RG DirectX normals to 2048 BC7, in data/tgl and data/tgl/128.

--albedo is the sRGB-byte output of bake_albedo.py; --normal is the exported
Timberwolf_Body_NRM. All normal channels are linear data. Never use the BT
DXT5nm unpacker: MW5 X=R, Y=G. Output XYZ uses Y-up tangent space.
--deploy names data/tgl; --scratch must be inside the lane scratch directory.
"""
import argparse
import shutil
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.bt2018_import.cook_ktx2_textures import cook_one_png  # noqa: E402
from tools.mw5_mechs.build_glb import DEFAULT_RECIPE, load_recipe  # noqa: E402


def normal_image(src, size=2048):
    a = np.asarray(Image.open(src).convert("RGB").resize((size, size), Image.Resampling.BILINEAR), dtype=np.float32) / 255
    x, y = a[..., 0] * 2 - 1, 1 - a[..., 1] * 2
    z = np.sqrt(np.clip(1 - x * x - y * y, 0, 1))
    xyz = np.stack((x, y, z), axis=-1)
    xyz /= np.maximum(np.linalg.norm(xyz, axis=-1, keepdims=True), 1e-8)
    return Image.fromarray(np.uint8(np.clip((xyz * .5 + .5) * 255 + .5, 0, 255))).convert("RGBA")


def cook(ktx, albedo, normal, deploy, scratch, recipe=None):
    recipe = load_recipe() if recipe is None else recipe
    deploy, scratch = Path(deploy), Path(scratch)
    scratch.mkdir(parents=True, exist_ok=True)
    images = {"alb": Image.open(albedo).convert("RGBA").resize((2048, 2048), Image.Resampling.LANCZOS),
              "nrm": normal_image(normal)}
    for tag, img in images.items():
        stem = f"{recipe['mc2']['stem']}_{tag}"
        png, cooked = scratch / f"{stem}.png", scratch / f"{stem}.ktx2"
        img.save(png)
        if not cook_one_png(str(ktx), str(png), str(cooked), str(scratch / f"{stem}.uastc.ktx2")):
            raise RuntimeError(f"BC7 cook failed: {stem}")
        for dest in (deploy, deploy / "128"):
            dest.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(cooked, dest / cooked.name)
        print(stem, "mean RGBA", (np.asarray(img).mean(axis=(0, 1)) / 255).tolist())


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    for name in ("ktx", "albedo", "normal", "deploy", "scratch"):
        ap.add_argument("--" + name, required=True)
    ap.add_argument("--recipe", type=Path, default=DEFAULT_RECIPE)
    a = ap.parse_args()
    cook(a.ktx, a.albedo, a.normal, a.deploy, a.scratch, load_recipe(a.recipe))
