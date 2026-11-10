#!/usr/bin/env python3
"""Stock tree atlases re-generated at 1024 (stock-tree-upscale lane, 2026-11-10).

Owner: stock-placed trees keep their stock cards and look, at 1024 with leaves and branches, in the mc2_01 palette
(data/vegetation/palettes/mc2_01.json). Each stock 128x128 atlas is the layout guide; nothing from other games is used.

Recipe per atlas (seeded, deterministic; numpy + Pillow + scipy only):
  1. stock colour dilated into transparent texels, x8 Lanczos of colour and alpha (the stock picture as a base);
  2. silhouette: the upsampled alpha field perturbed by leaf-cluster domes (foliage) or left smooth (bare branches),
     re-thresholded with a bisected offset so the covered fraction equals stock (the cards read as closed as stock);
  3. colour: stock luminance rank mapped onto a ramp built from the mc2_01 palette swatches (lifted to natural foliage
     values, which is the night fix on the content side), stock hue kept in part, per-leaf-cluster value/hue jitter and
     fine leaf or bark grain. No baked light direction or AO (the prop shader applies both);
  4. colour dilated into the cut-out texels so far mips carry no dark fringe;
  5. cooked with tools/content_pipeline/texcook.py (role base_color, BC7) to data/tgl/128/<stem>.ktx2, the same stem as the
     stock TGA: the texture loader prefers that loose sidecar (mclib/texture_assets.cpp load()); removing it renders stock.

  py -3 tools/tree_retexture/gen_stock_trees.py [--stock-dir DIR] [--out data/tgl/128] [--png-dir DIR] [--only STEM]
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image
from scipy import ndimage

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))
from content_pipeline import texcook  # noqa: E402

STOCK_DIR = Path("A:/Games/mc2-opengl/mc2-win64-v0.4c/data/tgl/128")   # golden install, read only
PALETTE = REPO / "data/vegetation/palettes/mc2_01.json"
PINS = REPO / "data/kit/stock-textures.json"
SIZE = 1024

# Ramps: (palette swatch name, value lift). Lift raises the very dark stock values (30-45/255) toward natural foliage.
RAMPS = {
    "crown": [("treeline canopy", 1.6), ("palm frond green", 1.35), ("treeline lit", 1.45), ("palm frond khaki", 1.0)],
    "palm":  [("palm frond green", 1.25), ("palm frond green", 1.7), ("palm frond khaki", 1.0), ("palm frond khaki", 1.3)],
    "bark":  [("bark", 0.6), ("bark", 1.0), ("bark", 1.35), ("bark", 1.75)],
}

# stem -> recipe. kind: leaf (clustered foliage silhouette), frond (elongated leaflets), branch (bare wood, smooth edges).
# hue: share of the ramp colour over the stock hue; lift-only atlases (multi-species L1 sheets with white birch bark) keep
# stock hue and scale value.
ATLASES = {
    # mc2_01 LOD0
    "a_treeclump1":       dict(kind="leaf",   ramp="crown", hue=0.8, cell=5.0, seed=11),
    "a_palm":             dict(kind="frond",  ramp="palm",  hue=0.5, cell=6.0, seed=12, bark_u=(0.70, 0.80)),
    "a_mapledead":        dict(kind="branch", ramp="bark",  hue=0.7, cell=5.0, seed=13),
    "a_oak1dead":         dict(kind="branch", ramp="bark",  hue=0.7, cell=5.0, seed=14),
    # mc2_01 LOD1 (shared with other maps)
    "a_treeclump1l1":     dict(kind="leaf",   ramp="crown", hue=0.8, cell=5.0, seed=21),
    # Multi-species L1 sheets (white birch bark, green oak crowns beside dead ones): stock hue, value lift, smooth outline.
    "a_pine&palm&birch":  dict(kind="branch", ramp=None,    lift=1.35, cell=5.0, seed=22, blur=0.6, grow=1.15),
}
# Not regenerated (stock 128 stays): a_deadtreesl1 and a_oak&mapledeadl1. Their far-LOD twigs are one texel at alpha
# below the cut, so any threshold re-gen beads them into dots or smears them into blobs (tried 2026-11-10, blur 0.35-0.6);
# they need a traced twig skeleton redrawn as strokes.


def hexrgb(h):
    h = h.lstrip("#")
    return np.array([int(h[i:i + 2], 16) for i in (0, 2, 4)], np.float32)


def palette_swatches():
    pal = json.loads(PALETTE.read_text())
    sw = {s["name"]: hexrgb(s["hex"]) for s in pal["swatches"]}
    sw["bark"] = hexrgb(pal["bark"])
    return sw


def ramp_colors(name):
    sw = palette_swatches()
    stops = [np.clip(sw[n] * k, 0, 255) for n, k in RAMPS[name]]
    return np.stack(stops)


def luma(rgb):
    return rgb[..., 0] * 0.299 + rgb[..., 1] * 0.587 + rgb[..., 2] * 0.114


def dilate_rgb(rgb, mask):
    """Nearest opaque colour into every transparent texel (EDT)."""
    if mask.all() or not mask.any():
        return rgb
    _, (iy, ix) = ndimage.distance_transform_edt(~mask, return_indices=True)
    return rgb[iy, ix]


def value_noise(shape, cell, rng, octaves=3):
    h, w = shape
    out = np.zeros(shape, np.float32)
    amp, tot = 1.0, 0.0
    for o in range(octaves):
        c = max(2, int(round(cell / (2 ** o))))
        g = rng.random((h // c + 2, w // c + 2)).astype(np.float32)
        up = np.asarray(Image.fromarray(g).resize((g.shape[1] * c, g.shape[0] * c), Image.BICUBIC))[:h, :w]
        out += amp * up
        tot += amp
        amp *= 0.5
    return out / tot


def leaf_cells(shape, cell, rng, aniso=None):
    """Voronoi leaf clusters: (dome 0..1 per texel, cell id). aniso = (angle field, stretch) for fronds."""
    h, w = shape
    n = int(h * w / (cell * cell))
    pts = rng.random((n, 2)).astype(np.float32) * np.array([h, w], np.float32)
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    from scipy.spatial import cKDTree
    if aniso is None:
        tree = cKDTree(pts)
        d, idx = tree.query(np.stack([yy.ravel(), xx.ravel()], 1), k=1, workers=1)
    else:
        ang, stretch = aniso
        # Warp space along the local frond direction: leaflets elongate across the frond axis.
        c, s = np.cos(ang), np.sin(ang)
        u = (xx * c + yy * s)
        v = (-xx * s + yy * c) / stretch
        q = np.stack([v.ravel(), u.ravel()], 1)
        pc = np.stack([np.cos(ang[pts[:, 0].astype(int), pts[:, 1].astype(int)]),
                       np.sin(ang[pts[:, 0].astype(int), pts[:, 1].astype(int)])], 1)
        pu = pts[:, 1] * pc[:, 0] + pts[:, 0] * pc[:, 1]
        pv = (-pts[:, 1] * pc[:, 1] + pts[:, 0] * pc[:, 0]) / stretch
        tree = cKDTree(np.stack([pv, pu], 1))
        d, idx = tree.query(q, k=1, workers=1)
    d = d.reshape(h, w).astype(np.float32)
    dome = np.clip(1.0 - d / (cell * 0.75), 0.0, 1.0)
    return dome, idx.reshape(h, w)


def coverage_threshold(field, target):
    lo, hi = -1.0, 2.0
    for _ in range(40):
        mid = (lo + hi) / 2
        if (field > mid).mean() > target:
            lo = mid
        else:
            hi = mid
    return (lo + hi) / 2


def generate(stem, recipe, stock_dir=STOCK_DIR, size=SIZE):
    src = Image.open(stock_dir / f"{stem}.tga").convert("RGBA")
    s = np.asarray(src).astype(np.float32)
    rgb0, a0 = s[..., :3], s[..., 3] / 255.0
    cov = float((a0 >= 0.5).mean())
    rgb0 = dilate_rgb(rgb0, a0 >= 0.5)
    rng = np.random.default_rng(recipe["seed"])
    up = lambda arr: np.asarray(Image.fromarray(arr.astype(np.float32)).resize((size, size), Image.LANCZOS))
    rgb = np.stack([up(rgb0[..., c]) for c in range(3)], -1)
    a = np.clip(up(a0), 0, 1)
    scale = size / src.size[0]
    cell = recipe["cell"] * scale / 8.0 * 1.0
    kind = recipe["kind"]

    # Silhouette. Foliage: blur the stock alpha by about one stock texel so its 1-2 texel pin holes close (crowns read
    # closed from above, owner 2026-11-09), then break only the outline into leaf clusters. Bare branches keep stock coverage.
    if kind in ("leaf", "frond"):
        field = ndimage.gaussian_filter(a, scale * (0.8 if kind == "leaf" else 0.45))
        if kind == "frond":
            gy, gx = np.gradient(ndimage.gaussian_filter(a, scale * 1.5))
            ang = np.arctan2(gy, gx).astype(np.float32)       # across the frond edge: leaflets run across the axis
            dome, ids = leaf_cells(a.shape, cell, rng, aniso=(ang, 3.0))
        else:
            dome, ids = leaf_cells(a.shape, cell, rng)
        big, big_ids = leaf_cells(a.shape, cell * 3.0, rng)
        edge = np.clip(1.0 - np.abs(field - 0.5) * 2.5, 0, 1)      # perturb only near the outline
        field = field + edge * (dome - 0.45) * 0.45 + edge * (big - 0.5) * 0.25
        mask = field > (coverage_threshold(field, cov) if kind == "frond" else 0.45)
        holes = ndimage.binary_fill_holes(mask) & ~mask
        lab, n = ndimage.label(holes)
        if n:
            area = ndimage.sum(holes, lab, index=np.arange(1, n + 1))
            small = np.concatenate([[False], area < (scale * 5.0) ** 2])
            mask |= small[lab]
    else:
        # blur (stock texels) and grow (coverage gain): the one-texel twigs of the far sheets bead into dots at a
        # stock-coverage cut, so those keep a little more coverage; too much blur turns twigs into blobs.
        field = ndimage.gaussian_filter(a, scale * recipe.get("blur", 0.35))
        ids = big_ids = dome = None
        mask = field > coverage_threshold(field, min(1.0, cov * recipe.get("grow", 1.0)))

    # Colour.
    L = luma(rgb)
    if recipe.get("ramp"):
        stops = ramp_colors(recipe["ramp"])
        inside = L[mask] if mask.any() else L.ravel()
        lo, hi = np.percentile(inside, 3), np.percentile(inside, 97)
        t = np.clip((L - lo) / max(hi - lo, 1e-3), 0, 1) * (len(stops) - 1)
        i0 = np.clip(np.floor(t).astype(int), 0, len(stops) - 2)
        f = (t - i0)[..., None]
        ramp = stops[i0] * (1 - f) + stops[i0 + 1] * f
        chroma = rgb / np.maximum(L, 1.0)[..., None]
        stock_hued = luma(ramp)[..., None] * chroma
        col = ramp * recipe["hue"] + stock_hued * (1 - recipe["hue"])
    else:
        col = rgb * recipe["lift"]
    # Detail: per-cluster jitter (value and a little hue), fine grain. Grain is leaf speckle or bark fibre.
    if ids is not None:
        jit = rng.random(int(big_ids.max()) + 1).astype(np.float32)
        hj = rng.random(int(big_ids.max()) + 1).astype(np.float32)
        lj = rng.random(int(ids.max()) + 1).astype(np.float32)
        v = (0.88 + 0.24 * jit[big_ids]) * (0.90 + 0.20 * lj[ids])   # cluster value, then single leaves
        col = col * v[..., None]
        col[..., 0] *= 0.95 + 0.10 * hj[big_ids]
        col[..., 2] *= 1.05 - 0.10 * hj[big_ids]
        gaps = np.clip((0.10 - dome) / 0.10, 0, 1)                   # thin darker seams between leaves
        col = col * (1 - 0.18 * gaps)[..., None]
        grain = value_noise(a.shape, max(2.0, cell / 3), rng, octaves=2)
        col = col * (0.92 + 0.16 * grain)[..., None]
    else:
        fib = value_noise((a.shape[0], a.shape[1] // 4), max(2.0, cell / 2), rng, octaves=2)
        fib = np.asarray(Image.fromarray(fib).resize((a.shape[1], a.shape[0]), Image.BICUBIC))
        col = col * (0.85 + 0.30 * fib)[..., None]
    if recipe.get("bark_u"):                                         # palm trunk strip: ring bands across the strip
        u0, u1 = (int(x * size) for x in recipe["bark_u"])
        bark = ramp_colors("bark")
        yy = np.arange(size, dtype=np.float32)
        rings = 0.5 + 0.5 * np.sin(yy / (scale * 1.6) * np.pi + value_noise((size, 1), 16, rng)[:, 0] * 3.0)
        fib = value_noise((size, u1 - u0), 3.0, rng, octaves=2)
        base = (bark[1] * (1 - rings[:, None, None]) + bark[2] * rings[:, None, None]) * (0.85 + 0.3 * fib)[..., None]
        strip = mask[:, u0:u1]
        col[:, u0:u1][strip] = base[strip] * 1.15
    col = np.clip(col, 0, 255)
    col = dilate_rgb(col, mask)
    out = np.dstack([col, mask.astype(np.float32) * 255.0]).round().astype(np.uint8)
    return out, cov


def sha256(b):
    return hashlib.sha256(b).hexdigest()


def build_one(stem, stock_dir, out, png_dir, png_only):
    src = stock_dir / f"{stem}.tga"
    src_sha = sha256(src.read_bytes())
    pin = json.loads(PINS.read_text())["sha256"].get(f"data/tgl/128/{stem}.tga")
    if pin and src_sha != pin:
        raise SystemExit(f"{src}: not the stock TGA (sha {src_sha[:12]} != pin {pin[:12]})")
    img, cov = generate(stem, ATLASES[stem], stock_dir)
    if png_dir:
        png_dir.mkdir(parents=True, exist_ok=True)
        Image.fromarray(img, "RGBA").save(png_dir / f"{stem}.png")
    row = {"source_sha256": src_sha, "size": SIZE, "stock_coverage": round(cov, 4),
           "coverage": round(float((img[..., 3] >= 128).mean()), 4)}
    if not png_only:
        ktx = texcook.cook_ktx2(img, "base_color", alpha_cutoff=0.5, cache_dir=None)
        (out / f"{stem}.ktx2").write_bytes(ktx)
        row.update(ktx2_sha256=sha256(ktx), bytes=len(ktx))
    print(f"[tree_retexture] {stem}: {SIZE}px cov {cov:.3f}->{row['coverage']:.3f} {row.get('bytes', 0)} B", flush=True)
    return stem, row


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--stock-dir", type=Path, default=STOCK_DIR)
    ap.add_argument("--out", type=Path, default=REPO / "data/tgl/128")
    ap.add_argument("--png-dir", type=Path, help="also write the level-0 RGBA as PNG (review)")
    ap.add_argument("--png-only", action="store_true", help="review PNGs only, no BC7 cook")
    ap.add_argument("--only", action="append")
    ap.add_argument("--jobs", type=int, default=4, help="atlases cooked in parallel (texconv is single threaded per call)")
    ap.add_argument("--manifest", type=Path, help="write {stem: {source_sha256, size, coverage, ktx2_sha256, bytes}}")
    a = ap.parse_args(argv)
    a.out.mkdir(parents=True, exist_ok=True)
    stems = [s for s in ATLASES if not a.only or s in a.only]
    from concurrent.futures import ProcessPoolExecutor
    with ProcessPoolExecutor(max_workers=max(1, a.jobs)) as pool:
        rows = dict(pool.map(build_one, stems, *zip(*[(a.stock_dir, a.out, a.png_dir, a.png_only)] * len(stems))))
    if a.manifest:
        a.manifest.write_text(json.dumps(rows, indent=1, sort_keys=True) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
