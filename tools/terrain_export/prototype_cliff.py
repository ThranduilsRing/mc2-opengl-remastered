#!/usr/bin/env python3
"""TERRAIN-REGEN-4: visual-only cliff prototype on one mc2_01 area (exploration; nothing is wired in).

Builds <out>/mc2_01_fresh.glb = the exported terrain with a cell-aligned patch of the sim grid cut out and
replaced by a hi-res (default 24 verts per sim cell) mesh whose heights are  smooth base + cliff relief.
The patch boundary is pinned to the original triangle surface, and the relief is weighted by a slope-derived
cliff mask that is 0 on the border, so the silhouette and the sim grid are untouched.  Relief = ridged fbm +
strata terraces (a heightfield cannot make overhangs; real cliff meshes are the production path, see the doc).
The patch gets its own baked albedo (colormap crop blended with a procedural rock colour on the cliff mask).
  py -3 tools/terrain_export/prototype_cliff.py mc2_01 --out DIR [--c0 41 --r0 38 --n 14 --amp 55]
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image
from scipy.ndimage import gaussian_filter, map_coordinates, zoom

sys.path.insert(0, str(Path(__file__).resolve().parent))
import export_mission as E  # noqa: E402

WU = E.WU


def fbm(shape, seed, octaves=6, base=4, ridged=False):
    rng = np.random.default_rng(seed)
    out = np.zeros(shape)
    amp = 1.0
    tot = 0.0
    for o in range(octaves):
        n = base * 2 ** o
        g = rng.random((n + 1, n + 1))
        z = zoom(g, shape[0] / n, order=3, mode="nearest")[:shape[0], :shape[1]]
        if z.shape != shape:
            z = np.pad(z, ((0, shape[0] - z.shape[0]), (0, shape[1] - z.shape[1])), mode="edge")
        if ridged:
            z = 1.0 - np.abs(2 * z - 1)
        out += amp * z
        tot += amp
        amp *= 0.5
    return out / tot


def tri_height(elev, fr, fc):
    """height of the exported triangle surface at fractional (row, col) grid coords (diagonal b-cc)."""
    r0 = np.clip(np.floor(fr).astype(int), 0, elev.shape[0] - 2)
    c0 = np.clip(np.floor(fc).astype(int), 0, elev.shape[1] - 2)
    v, u = fr - r0, fc - c0
    h00, h01, h10, h11 = elev[r0, c0], elev[r0, c0 + 1], elev[r0 + 1, c0], elev[r0 + 1, c0 + 1]
    lo = h00 + u * (h01 - h00) + v * (h10 - h00)
    hi = h11 + (1 - u) * (h10 - h11) + (1 - v) * (h01 - h11)
    return np.where(u + v <= 1, lo, hi)


def smooth(x, a, b):
    t = np.clip((x - a) / (b - a), 0, 1)
    return t * t * (3 - 2 * t)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("mission")
    ap.add_argument("--out", required=True)
    ap.add_argument("--c0", type=int, default=41)
    ap.add_argument("--r0", type=int, default=38)
    ap.add_argument("--n", type=int, default=14, help="patch size in sim cells")
    ap.add_argument("--per", type=int, default=24, help="patch verts per sim cell")
    ap.add_argument("--amp", type=float, default=55.0, help="relief amplitude, wu")
    ap.add_argument("--scale", type=float, default=0.1)
    ap.add_argument("--tex", type=int, default=2048)
    a = ap.parse_args()
    res = E.resolve_missions_dir(None, [a.mission])
    m = E.load_mission(a.mission, res.dir)
    elev, side = m["elev"], m["side"]
    S = a.scale
    n, per = a.n, a.per
    N = n * per + 1
    fr = a.r0 + np.linspace(0, n, N)[:, None] * np.ones((1, N))
    fc = a.c0 + np.ones((N, 1)) * np.linspace(0, n, N)[None, :]
    base_tri = tri_height(elev, fr, fc)
    smooth_b = map_coordinates(elev, [fr, fc], order=3, mode="nearest")  # bicubic
    # border taper (0 at the patch edge) keeps the silhouette and the seam identical to the original
    d = np.minimum.reduce([fr - a.r0, a.r0 + n - fr, fc - a.c0, a.c0 + n - fc]) / 1.5  # cells
    taper = smooth(d, 0.0, 1.0)
    base = base_tri * (1 - taper) + smooth_b * taper
    pix = WU / per
    gy, gx = np.gradient(base, pix)
    slope = np.degrees(np.arctan(np.hypot(gx, gy)))
    sl = gaussian_filter(slope, per * 0.6)
    w = smooth(sl, 14.0, 34.0) * taper
    w = gaussian_filter(w, per * 0.4)
    rid = fbm((N, N), 7, 6, 6, True)
    n2 = fbm((N, N), 11, 5, 10)
    # strata: terraces along the height, warped by noise
    hh = base + 40 * (n2 - 0.5)
    terr = (hh / 22.0)
    terr = (terr - np.floor(terr))
    terr = smooth(terr, 0.0, 0.18) - 0.5 * smooth(terr, 0.8, 1.0)
    relief = (rid - 0.45) * 1.6 * a.amp + terr * 0.12 * a.amp
    h = base + w * relief
    # --- patch mesh (world: x east, y north); glTF (x, h, -y)
    wx = -side * WU / 2 + fc * WU
    wy = side * WU / 2 - fr * WU
    pos = np.stack([wx, h, -wy], -1).reshape(-1, 3).astype("<f4") * S
    gy2, gx2 = np.gradient(h, pix)
    nrm = np.stack([-gx2, np.ones_like(h), gy2], -1)
    nrm = (nrm / np.linalg.norm(nrm, axis=-1, keepdims=True)).reshape(-1, 3).astype("<f4")
    uv = np.stack([(fc - a.c0) / n, (fr - a.r0) / n], -1).reshape(-1, 2).astype("<f4")
    ii = np.arange(N * N).reshape(N, N)
    A_, B_, C_, D_ = ii[:-1, :-1], ii[:-1, 1:], ii[1:, :-1], ii[1:, 1:]
    idx = np.stack([A_, C_, B_, B_, C_, D_], -1).reshape(-1).astype("<u4")
    # --- patch albedo: colormap crop blended with procedural rock on the cliff mask
    cm = Image.open(E.find_colormap(a.mission)).convert("RGB")
    ext = side * WU
    cw = cm.width
    box = (int(a.c0 / side * cw), int(a.r0 / side * cw), int((a.c0 + n) / side * cw), int((a.r0 + n) / side * cw))
    T = a.tex
    crop = np.asarray(cm.crop(box).resize((T, T), Image.LANCZOS), np.float32) / 255
    wt = np.asarray(Image.fromarray((w * 255).astype(np.uint8)).resize((T, T), Image.BICUBIC), np.float32) / 255
    ht = np.asarray(Image.fromarray(((h - h.min()) / (np.ptp(h) + 1e-6) * 255).astype(np.uint8)).resize((T, T), Image.BICUBIC), np.float32) / 255
    nt = fbm((T, T), 5, 7, 8)
    band = 0.5 + 0.5 * np.sin((ht * 40.0 + nt * 6.0) * 3.1416)
    rock = np.stack([0.42 + 0.20 * nt, 0.37 + 0.17 * nt, 0.31 + 0.14 * nt], -1) * (0.75 + 0.25 * band[..., None])
    wb = smooth(wt, 0.15, 0.6)[..., None]
    alb = crop * (1 - wb) + rock * wb
    alb_img = Image.fromarray((np.clip(alb, 0, 1) * 255).astype(np.uint8))
    # --- assemble: original terrain minus the patch cells + patch
    g = E.Glb()
    cmap = cm.resize((4096, 4096), Image.LANCZOS)
    g.d["materials"].append({"name": "terrain_colormap", "doubleSided": True, "pbrMetallicRoughness": {
        "metallicFactor": 0.0, "roughnessFactor": 0.95, "baseColorTexture": {"index": g.image(cmap, "JPEG")}}})
    p0, n0, uv0, idx0 = E.terrain_arrays(elev, S)
    tri = idx0.reshape(-1, 6)
    cell = np.arange(tri.shape[0])
    cr, cc = cell // (side - 1), cell % (side - 1)
    inside = (cr >= a.r0) & (cr < a.r0 + n) & (cc >= a.c0) & (cc < a.c0 + n)
    g.node("terrain", g.mesh("terrain", p0, tri[~inside].reshape(-1), n0, uv0, 0))
    g.d["materials"].append({"name": "cliff_patch", "doubleSided": True, "pbrMetallicRoughness": {
        "metallicFactor": 0.0, "roughnessFactor": 0.9, "baseColorTexture": {"index": g.image(alb_img, "JPEG")}}})
    g.node("cliff_patch", g.mesh("cliff_patch", pos, idx, nrm, uv, 1))
    g.d["materials"].append({"name": "water", "alphaMode": "BLEND", "doubleSided": True, "pbrMetallicRoughness": {
        "baseColorFactor": [0.05, 0.22, 0.30, 0.65], "metallicFactor": 0.0, "roughnessFactor": 0.05}})
    hw = ext / 2 * S
    g.node("water", g.mesh("water", np.array([[-hw, 0, hw], [hw, 0, hw], [hw, 0, -hw], [-hw, 0, -hw]], "<f4"),
                            np.array([0, 1, 2, 0, 2, 3], "<u4"), np.tile(np.array([0, 1, 0], "<f4"), (4, 1)), None, 2),
           t=(0, m["water_elev"] * S, 0))
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    g.write(out / f"{a.mission}_fresh.glb")
    cx = -side * WU / 2 + (a.c0 + n / 2) * WU
    cy = side * WU / 2 - (a.r0 + n / 2) * WU
    print(f"patch centre world ({cx:.0f},{cy:.0f}) wu; verts {N*N}; cliff-mask coverage {100*(w>0.3).mean():.1f}%; "
          f"max |relief| {np.abs(h-base).max():.1f} wu")
    return 0


if __name__ == "__main__":
    sys.exit(main())
