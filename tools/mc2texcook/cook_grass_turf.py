#!/usr/bin/env python3
"""cook_grass_turf.py -- cook the top-down grass TURF detail normal for the terrain grass layer (GRASS-GROUND-NORMAL).

Owner 2026-11-10 (mc2_01): "We need some top down different detail normal that is a grass of some kind." Owner reference
(same day): a near-top-down meadow of dense tufted grass clumps with irregular gaps of bare cracked dirt and small pebbles,
roughly 20-30% of the area.
The old grass layer (golden mat1_normal.tga) is a fine leaf-litter normal whose features are a few texels wide: one grass
tile spans 384 world units (2048 texels), so from the gameplay camera the GPU samples mip 3 or so and the blades average
to a flat normal. The turf keeps a real blade normal for close views and adds the scales that survive the mip chain:

  blades   Poly Haven sparse_grass nor_gl (CC0), the close-range detail inside the tufts
  tussocks tileable domes of 2.5..7 world units (14..36 texels): the tufted clumps
  gaps     tileable patch mask of 20..70 world units covering GAP_SHARE of the tile: no tufts, Poly Haven dry_ground_01
           nor_gl (CC0, cracked dry earth with pebbles) instead, sitting lower in the height
  swards   tileable band noise of 40..100 world units, gentle undulation that still reads at far mips

Output: data/terrain_layers/grass_turf_normal.ktx2, the same layout as the engine's matN_normal.ktx2 sidecars so it can
replace detail-normal array layer 1 (MAT_LAYER_GRASS): 2048x2048 R8G8B8A8_UNORM, full box mip chain,
RGB = GL tangent normal, A = height (POM). Deterministic (fixed seed).

Usage: py -3 tools/mc2texcook/cook_grass_turf.py --src G:/mc2-assets/pbr/dl-2026-10-23/ground --out data/terrain_layers
"""
import argparse
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cook_terrain_layers as ctl  # noqa: E402

SIZE = 2048
OUT_NAME = "grass_turf_normal.ktx2"
BLADE_SOURCE = "sparse_grass/textures/sparse_grass_nor_gl_2k.jpg"
DIRT_SOURCE = "dry_ground_01/textures/dry_ground_01_nor_gl_2k.jpg"
BLADE_AO_SOURCE = "sparse_grass/textures/sparse_grass_arm_2k.jpg"   # glTF ARM pack: R = ambient occlusion
SEED = 20261110
# Tussock radii (texels) and their share of the clump count; one grass tile is 384 wu, 5.33 texels per wu.
TUSSOCKS = ((14.0, 0.50), (22.0, 0.32), (36.0, 0.18))
TUSSOCK_COUNT = 5200
SWARD_BAND = (200.0, 520.0)   # wavelengths in texels
GAP_BAND = (60.0, 240.0)      # bare-dirt patch wavelengths in texels
GAP_RAGGED_BAND = (24.0, 64.0)  # ragged gap edges: tufts overhang the dirt
GAP_RAGGED = 0.25
GAP_SHARE = 0.25              # owner reference: 20-30% bare dirt
GAP_EDGE = 0.5                # mask softness in noise std
# Height-to-slope gains and the mix of the scales.
TUSSOCK_SLOPE = 9.0
SWARD_SLOPE = 10.0
GAP_SLOPE = 1.5
BLADE_GAIN = 0.85
DIRT_GAIN = 0.9


def _band_noise(rng, size, lo, hi):
    """Tileable noise with wavelengths between lo and hi texels, zero mean, unit std."""
    f = np.fft.fftfreq(size)
    r = np.sqrt(f[:, None] ** 2 + f[None, :] ** 2)
    band = ((r >= 1.0 / hi) & (r <= 1.0 / lo)).astype(np.float64)
    n = np.real(np.fft.ifft2(np.fft.fft2(rng.standard_normal((size, size))) * band))
    return (n - n.mean()) / max(n.std(), 1e-9)


def _tussocks(rng, size):
    """Tileable clump height in 0..1: dome kernels convolved (circularly) with random impulses, saturated where they overlap."""
    acc = np.zeros((size, size))
    y, x = np.mgrid[0:size, 0:size]
    dy = np.minimum(y, size - y).astype(np.float64)
    dx = np.minimum(x, size - x).astype(np.float64)
    d2 = dx * dx + dy * dy
    for radius, share in TUSSOCKS:
        imp = np.zeros((size, size))
        n = int(TUSSOCK_COUNT * share)
        imp[rng.integers(0, size, n), rng.integers(0, size, n)] = rng.uniform(0.6, 1.0, n)
        kernel = np.clip(1.0 - d2 / (radius * radius), 0.0, None) ** 1.5
        acc += np.real(np.fft.ifft2(np.fft.fft2(imp) * np.fft.fft2(kernel)))
    return 1.0 - np.exp(-1.6 * acc)


def gap_mask(rng, size):
    """Tileable 0..1 bare-dirt mask covering about GAP_SHARE of the tile."""
    n = _band_noise(rng, size, *GAP_BAND) + GAP_RAGGED * _band_noise(rng, size, *GAP_RAGGED_BAND)
    t = np.quantile(n, 1.0 - GAP_SHARE)
    s = np.clip((n - t) / GAP_EDGE + 0.5, 0.0, 1.0)
    return s * s * (3.0 - 2.0 * s)


def _slopes(h, gain):
    """GL tangent-space slope (x right, y up the image) of a tileable height field."""
    dx = (np.roll(h, -1, 1) - np.roll(h, 1, 1)) * 0.5
    drow = (np.roll(h, -1, 0) - np.roll(h, 1, 0)) * 0.5
    return -dx * gain, drow * gain


def _normal_slopes(rgb, size):
    if rgb.shape[:2] != (size, size):
        rgb = np.asarray(Image.fromarray(rgb).resize((size, size), Image.LANCZOS))
    n = rgb[..., :3].astype(np.float64) / 127.5 - 1.0
    z = np.clip(n[..., 2], 0.2, 1.0)
    return n[..., 0] / z, n[..., 1] / z


AO_MEAN = 0.9        # the cooked AO is mean-normalised so enabling it shifts the ground's overall tone little
GAP_AO = 0.92        # bare dirt has no AO source (dry_ground_01 ships none): near-open, the tussock edges below still shade it


def source_ao(blade_ao, tus, gap, size):
    """Tileable 0..1 occlusion: the Poly Haven sparse_grass AO (arm.R) inside the tufts, shaded further by the tussock domes' valleys."""
    if blade_ao.shape != (size, size):
        blade_ao = np.asarray(Image.fromarray(np.rint(blade_ao * 255.0).astype(np.uint8)).resize((size, size), Image.LANCZOS)) / 255.0
    ao = (1.0 - gap) * blade_ao * (0.75 + 0.25 * tus) + gap * GAP_AO * (0.85 + 0.15 * tus)
    return np.clip(ao * (AO_MEAN / max(float(ao.mean()), 1e-6)), 0.0, 1.0)


def turf_texels(blade_rgb, dirt_rgb, seed=SEED, size=SIZE, blade_ao=None):
    """HxWx4 uint8: RG = GL tangent normal x,y of tufts + dirt gaps + swards, B = source ambient occlusion (the shader rebuilds z; B = normal z
    when no AO source is given), A = turf height."""
    rng = np.random.default_rng(seed)
    gap = gap_mask(rng, size)
    grass = 1.0 - gap
    tus = _tussocks(rng, size) * grass
    sward = _band_noise(rng, size, *SWARD_BAND) * 0.5
    height = tus - 0.35 * gap + 0.35 * sward
    tx, ty = _slopes(tus, TUSSOCK_SLOPE)
    gx, gy = _slopes(gap, -GAP_SLOPE)
    sx, sy = _slopes(sward, SWARD_SLOPE)
    bx, by = _normal_slopes(blade_rgb, size)
    dx, dy = _normal_slopes(dirt_rgb, size)
    # Partial-derivative blend: slopes add, so the blade/dirt detail rides on the clump shapes.
    nx = (bx * BLADE_GAIN) * grass + (dx * DIRT_GAIN) * gap + tx + gx + sx
    ny = (by * BLADE_GAIN) * grass + (dy * DIRT_GAIN) * gap + ty + gy + sy
    inv = 1.0 / np.sqrt(nx * nx + ny * ny + 1.0)
    n = np.stack([nx * inv, ny * inv, inv], -1)
    out = np.empty((size, size, 4), np.uint8)
    out[..., :3] = np.clip(np.rint((n + 1.0) * 127.5), 0, 255)
    if blade_ao is not None:
        out[..., 2] = np.clip(np.rint(source_ao(blade_ao, tus, gap, size) * 255.0), 0, 255)
    h =(height - height.min()) / max(np.ptp(height), 1e-9)
    out[..., 3] = np.clip(np.rint(h * 255.0), 0, 255)
    return out


def load_sources(src_dir):
    return tuple(np.asarray(Image.open(os.path.join(src_dir, p)).convert("RGB")) for p in (BLADE_SOURCE, DIRT_SOURCE))


def load_blade_ao(src_dir):
    """The sparse_grass AO (float 0..1), or None when the pack has no ARM map."""
    p = os.path.join(src_dir, BLADE_AO_SOURCE)
    return np.asarray(Image.open(p).convert("RGB"))[..., 0].astype(np.float64) / 255.0 if os.path.isfile(p) else None


def cook(src_dir, out_dir, preview=None):
    texels = turf_texels(*load_sources(src_dir), blade_ao=load_blade_ao(src_dir))
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
    ap.add_argument("--src", required=True, help="dir holding sparse_grass/ and dry_ground_01/ (Poly Haven 2k glTF downloads)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--preview", help="optional PNG of the cooked normal (RGB)")
    a = ap.parse_args()
    print(cook(a.src, a.out, a.preview))


if __name__ == "__main__":
    main()
