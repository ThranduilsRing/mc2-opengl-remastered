#!/usr/bin/env python3
"""Bake MW5 paint from an external JSON of named Unreal material parameters.

JSON: colorSpace="linear", parameters={MetalColorMultiplier: [r,g,b],
PaintColorMultiplier: [r,g,b], PaintColorPrimary/Secondary/Tertiary: [r,g,b]},
maskChannels={R: "PaintColorPrimary", G: "PaintColorSecondary",
B: "PaintColorTertiary", A: "opacity"}. Keep source names/provenance beside it.
Unreal FLinearColor numbers and mask weights are linear. Blend in linear space,
then encode RGB to sRGB ONCE. Alpha is linear opacity, not a fourth paint colour.
The BT cook stores these encoded bytes in BC7 UNORM; mechSurface decodes RGB.
"""
import argparse
import json

import numpy as np
from PIL import Image

def bake(mask_path, size, paint):
    if paint["colorSpace"] != "linear" or paint["maskChannels"]["A"] != "opacity":
        raise ValueError("expected linear Unreal parameters and linear mask alpha")
    params = {k: np.asarray(v, dtype=np.float32) for k, v in paint["parameters"].items()}
    if any(v.shape != (3,) or not np.isfinite(v).all() for v in params.values()):
        raise ValueError("material parameters must be finite RGB triples")
    # Resize data channels independently: RGBA filtering otherwise premultiplies
    # RGB by alpha and destroys colour-mask values under transparent pixels.
    channels = Image.open(mask_path).convert("RGBA").split()
    m = np.stack([np.asarray(c.resize((size, size), Image.Resampling.BILINEAR)) for c in channels], axis=-1).astype(np.float32) / 255
    out = np.empty(m.shape[:2] + (3,), np.float32)
    out[:] = params["MetalColorMultiplier"]
    for ch, channel in enumerate("RGB"):
        w = m[..., ch:ch + 1]
        col = params[paint["maskChannels"][channel]] * params["PaintColorMultiplier"]
        out = out * (1 - w) + col * w
    out = np.clip(out, 0, 1)
    srgb = np.where(out <= 0.0031308, out * 12.92, 1.055 * out ** (1 / 2.4) - 0.055)
    rgba = np.concatenate((srgb, m[..., 3:4]), axis=2)
    return Image.fromarray((rgba * 255 + 0.5).astype(np.uint8))


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--mask", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--params", required=True, help="external JSON with named linear paint parameters")
    ap.add_argument("--size", type=int, default=2048)
    a = ap.parse_args()
    with open(a.params, encoding="utf-8") as f:
        bake(a.mask, a.size, json.load(f)).save(a.out)
