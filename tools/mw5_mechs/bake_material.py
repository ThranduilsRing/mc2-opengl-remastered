#!/usr/bin/env python3
"""Bake MW5 layered mech material to flat albedo, ORM and normal images.

The engine samples one albedo, one ORM (R=AO, G=roughness, B=metal) and one normal per mech, so the
MW5 layers (paint zones, metalID metal selector, wear mask, tiling detail normals) are composed once
here. --params is an external JSON of named Unreal material parameters (kept with the source assets):
  paint:    PaintColorPrimary/Secondary/Tertiary, PaintColorMultiplier, MetalColorMultiplier (linear RGB)
  scalars:  PaintWear, DirtStrength, GrimeMultiplier, DirtRoughness, PaintRoughness, PaintRoughnessMultiplier,
            ScratchRoughnessMultiplier, GrimeRoughnessMultiplier, MetalDetailContrast, PaintDetailContrast,
            MetalTilingScale, PaintTilingScale
  vectors:  DirtColor, GrimeColor
  metals:   name -> {roughness, metallic}   (the material's per-metal scalars, used as authored, roughness clamped to 1)
  metalId:  "RGB" bits of the metalID texture (e.g. "010") -> metal name
Wear mask channels follow the material's "(R)Scratch (G)Dirt (B)Grime" texture parameter.
"""
import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image

SIZE = 2048


def load(path, size=SIZE, channels="RGBA"):
    """Area-filtered, per-channel resize to float [0,1] (no alpha premultiply of data channels)."""
    img = Image.open(path).convert(channels)
    return np.stack([np.asarray(c.resize((size, size), Image.Resampling.BOX), dtype=np.float32)
                     for c in img.split()], axis=-1) / 255


def srgb(x):
    return np.where(x <= 0.0031308, x * 12.92, 1.055 * np.power(x, 1 / 2.4) - 0.055)


def metal_maps(metal_id, p):
    """Per-pixel roughness and metallic of the metal selected by the binary-thresholded metalID RGB."""
    bits = (metal_id[..., :3] > 0.5).astype(np.int32)
    code = bits[..., 0] * 4 + bits[..., 1] * 2 + bits[..., 2]
    rough, metal = np.zeros(code.shape, np.float32), np.zeros(code.shape, np.float32)
    for key, name in p["metalId"].items():
        sel = code == int(key, 2)
        rough[sel] = min(p["metals"][name]["roughness"], 1.0)
        metal[sel] = p["metals"][name]["metallic"]
    return rough, metal


def detail_normal(base_xyz, metal_cover, nrm_metal, nrm_paint, p):
    """Add the tiling detail normals (metal detail on bare metal, paint detail on paint) to the base normal."""
    xy = base_xyz[..., :2].copy()
    s = p["scalars"]
    for tex, tiles, strength, weight in ((nrm_metal, s["MetalTilingScale"], abs(s["MetalDetailContrast"]), metal_cover),
                                         (nrm_paint, s["PaintTilingScale"], abs(s["PaintDetailContrast"]), 1 - metal_cover)):
        idx = (np.arange(SIZE) * tiles % SIZE / SIZE * tex.shape[0]).astype(int)   # repeat the texture `tiles` times over UV 0..1
        t = tex[idx][:, idx]
        xy += np.stack((t[..., 0] * 2 - 1, 1 - t[..., 1] * 2), axis=-1) * strength * weight[..., None]   # same Y flip as the base normal
    z = np.sqrt(np.clip(1 - (xy ** 2).sum(-1), 0, 1))
    n = np.concatenate((xy, z[..., None]), axis=-1)
    return n / np.maximum(np.linalg.norm(n, axis=-1, keepdims=True), 1e-8)


def bake(mask, metal_id, wear, normal, p, detail=None):
    """Return (albedo RGBA, orm RGBA, normal RGBA) uint8 images. detail=(metal_nrm, paint_nrm) float RGB or None."""
    s, v, w = p["scalars"], {k: np.asarray(x, np.float32) for k, x in p["vectors"].items()}, wear
    pv = {k: np.asarray(x, np.float32) for k, x in p["paint"].items()}
    scratch, dirt, grime = w[..., 0:1], w[..., 1:2], w[..., 2:3]
    # Scratches expose bare metal through paint: PaintWear is the gain, PaintWearTightness the edge sharpness.
    # The material graph is not readable (cooked), so this threshold form is an approximation.
    exposed = np.clip((scratch - (1 - 1 / s["PaintWear"])) * s["PaintWearTightness"], 0, 1)
    colour = np.broadcast_to(pv["MetalColorMultiplier"], mask.shape[:2] + (3,)).copy()
    cover = np.zeros(mask.shape[:2] + (1,), np.float32)
    for ch, zone in enumerate(("PaintColorPrimary", "PaintColorSecondary", "PaintColorTertiary")):
        wt = mask[..., ch:ch + 1] * (1 - exposed)
        colour = colour * (1 - wt) + pv[zone] * pv["PaintColorMultiplier"] * wt
        cover = 1 - (1 - cover) * (1 - wt)
    d = np.clip(dirt * s["DirtStrength"], 0, 1)
    g = np.clip(grime * abs(s["GrimeMultiplier"]), 0, 1)
    colour = colour * (1 - d) + v["DirtColor"] * d
    colour = colour * (1 - g) + v["GrimeColor"] * g
    metal_rough, metal_metal = metal_maps(metal_id, p)
    rough = (1 - cover[..., 0]) * metal_rough + cover[..., 0] * min(s["PaintRoughness"] * s["PaintRoughnessMultiplier"], 1.0)
    rough = np.where(exposed[..., 0] > 0, rough * (1 - exposed[..., 0] * (1 - s["ScratchRoughnessMultiplier"])), rough)
    rough = rough * (1 - d[..., 0]) + s["DirtRoughness"] * d[..., 0]
    rough = rough * (1 - g[..., 0]) + rough * s["GrimeRoughnessMultiplier"] * g[..., 0]
    metallic = metal_metal * (1 - cover[..., 0]) * (1 - d[..., 0])
    alb = np.concatenate((srgb(np.clip(colour, 0, 1)), mask[..., 3:4]), axis=2)
    orm = np.stack((np.ones_like(rough), np.clip(rough, 0.04, 1), np.clip(metallic, 0, 1), np.ones_like(rough)), axis=-1)
    x, y = normal[..., 0] * 2 - 1, 1 - normal[..., 1] * 2
    xyz = np.stack((x, y, np.sqrt(np.clip(1 - x * x - y * y, 0, 1))), axis=-1)
    if detail:
        xyz = detail_normal(xyz, 1 - cover[..., 0], detail[0], detail[1], p)
    nrm = np.concatenate((xyz * .5 + .5, np.ones_like(xyz[..., :1])), axis=-1)
    u8 = lambda a: Image.fromarray((np.clip(a, 0, 1) * 255 + .5).astype(np.uint8))  # noqa: E731
    return u8(alb), u8(orm), u8(nrm)


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for k in ("mask", "metal-id", "wear", "normal", "params", "out"):
        ap.add_argument("--" + k, required=True)
    ap.add_argument("--detail", nargs=2, metavar=("METAL_NRM", "PAINT_NRM"), help="tiling detail normal PNGs")
    a = ap.parse_args()
    params = json.loads(Path(a.params).read_text(encoding="utf-8"))
    det = None
    if a.detail:
        det = tuple(np.asarray(Image.open(f).convert("RGB"), np.float32) / 255 for f in a.detail)
    outs = bake(load(a.mask), load(a.metal_id), load(a.wear), load(a.normal, channels="RGB"), params, det)
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    for name, img in zip(("alb", "orm", "nrm"), outs):
        img.save(out / f"{name}.png")
        print(name, "mean RGBA", (np.asarray(img).reshape(-1, 4).mean(0) / 255).round(4).tolist())
