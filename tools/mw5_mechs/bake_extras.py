#!/usr/bin/env python3
"""Bake MW5 weathering that the cooked material graph hides: grime/rust streaks along world-down, curvature edge wear,
macro colour variation and projected decals from the shared mech decal atlas. Works in the pack GLB's UV space
(rest pose, Y up, +X forward) on the images bake_material.py wrote.

  edge_wear(glb)            -> float [0,1] convex-edge mask in UV space (feeds the bake's scratch channel)
  weather(glb, alb, orm)    -> streaks (dark grime + rust) and macro variation applied to uint8 alb/orm arrays
  decals(glb, alb, orm, d)  -> projected decals from an atlas, d = list of placements (see DECALS)
"""
import argparse
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image
from scipy import ndimage

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_glb import read_accessor  # noqa: E402
from sockets import load_glb  # noqa: E402

SIZE = 2048


def body_triangles(glb, weapons=False):
    """(pos, normal, uv, tris) of every hull part that samples the atlas 1:1 (not weapons, not the glass tile)."""
    doc, buf = load_glb(glb)
    out = []
    for m in doc["meshes"]:
        if m["name"].startswith("wp") != weapons or m["name"] == "cockpit_glass":
            continue
        for pr in m["primitives"]:
            a = pr["attributes"]
            out.append(tuple(read_accessor(doc, buf, a[k]).astype(np.float64) for k in ("POSITION", "NORMAL", "TEXCOORD_0"))
                       + (read_accessor(doc, buf, pr["indices"]).astype(np.int64).reshape(-1, 3),))
    return out


def rasterize(parts, size=SIZE):
    """Per-texel world position, normal, coverage and the UV-space downhill direction (+ steepness)."""
    pos = np.zeros((size, size, 3), np.float32)
    nrm = np.zeros((size, size, 3), np.float32)
    cov = np.zeros((size, size), bool)
    down = np.zeros((size, size, 3), np.float32)   # (du, dv) unit, steepness
    for p, n, uv, tris in parts:
        for t in tris:
            q = uv[t] * size
            lo = np.maximum(np.floor(q.min(0)).astype(int), 0)
            hi = np.minimum(np.ceil(q.max(0)).astype(int) + 1, size)
            if hi[0] <= lo[0] or hi[1] <= lo[1] or hi[0] - lo[0] > 600 or hi[1] - lo[1] > 600:
                continue
            e1, e2 = q[1] - q[0], q[2] - q[0]
            den = e1[0] * e2[1] - e1[1] * e2[0]
            if abs(den) < 1e-9:
                continue
            xs, ys = np.meshgrid(np.arange(lo[0], hi[0]) + .5, np.arange(lo[1], hi[1]) + .5)
            d0, d1 = xs - q[0, 0], ys - q[0, 1]
            b1 = (d0 * e2[1] - d1 * e2[0]) / den
            b2 = (e1[0] * d1 - e1[1] * d0) / den
            ins = (b1 >= -.02) & (b2 >= -.02) & (b1 + b2 <= 1.02)
            if not ins.any():
                continue
            w = np.stack((1 - b1 - b2, b1, b2), -1)[ins]
            sl = (slice(lo[1], hi[1]), slice(lo[0], hi[0]))
            pos[sl][ins] = w @ p[t]
            nn = w @ n[t]
            nrm[sl][ins] = nn / np.maximum(np.linalg.norm(nn, axis=-1, keepdims=True), 1e-8)
            cov[sl] |= ins
            # d(height)/d(texel) from the vertex heights; downhill is the negative gradient
            dh = p[t][1:, 1] - p[t][0, 1]
            m = np.array([[e1[0], e1[1]], [e2[0], e2[1]]])
            g = np.linalg.solve(m, dh)
            mag = np.linalg.norm(g)
            if mag > 1e-12:
                down[sl][ins] = (-g[0] / mag, -g[1] / mag, 1.0)
    return pos, nrm, cov, down


def _noise(seed, size=SIZE, sigma=1.0):
    return ndimage.gaussian_filter(np.random.default_rng(seed).random((size, size)).astype(np.float32), sigma, mode="wrap")


def streaks(nrm, cov, down, seed, steps=200, step_px=1.6, decay=.99, density=.035):
    """Thin runs that start on up-facing ledges and flow along world-down inside each UV island."""
    rng = np.random.default_rng(seed)
    ledge = np.clip((nrm[..., 1] - .25) * 2, 0, 1) * cov
    seeds = (rng.random(cov.shape) < density) * (ledge + .12 * cov) * (.5 + .5 * rng.random(cov.shape))
    seeds = seeds.astype(np.float32)
    dx, dy = down[..., 0] * step_px, down[..., 1] * step_px
    gy, gx = np.mgrid[0:cov.shape[0], 0:cov.shape[1]].astype(np.float32)
    flowing = (down[..., 2] > 0) & (nrm[..., 1] < .85)
    cur = seeds
    for _ in range(steps):
        prev = ndimage.map_coordinates(cur, [gy - dy, gx - dx], order=1, mode="nearest")
        cur = np.where(flowing, np.maximum(seeds, prev * decay), seeds) * cov
    return np.clip(ndimage.gaussian_filter(cur, .7) * 1.6, 0, 1)


def edge_wear(glb, size=SIZE):
    """Convex-edge mask: mean curvature of welded vertices, rasterised over UV."""
    parts = body_triangles(glb)
    allp = np.concatenate([p for p, _, _, _ in parts])
    alln = np.concatenate([n for _, n, _, _ in parts])
    key = np.round(allp * 400).astype(np.int64)
    _, inv = np.unique(key, axis=0, return_inverse=True)
    nw = np.zeros((inv.max() + 1, 3))
    np.add.at(nw, inv, alln)
    nw /= np.maximum(np.linalg.norm(nw, axis=1, keepdims=True), 1e-8)
    pw = np.zeros_like(nw)
    pw[inv] = allp
    curv = np.zeros(len(nw))
    cnt = np.zeros(len(nw))
    base = 0
    for p, _, _, tris in parts:
        ids = inv[base:base + len(p)][tris]
        base += len(p)
        for a, b in ((0, 1), (1, 2), (2, 0)):
            i, j = ids[:, a], ids[:, b]
            d = pw[j] - pw[i]
            L2 = np.maximum((d * d).sum(1), 1e-6)
            c = ((nw[j] - nw[i]) * d).sum(1) / L2
            for u, s in ((i, c), (j, c)):
                np.add.at(curv, u, s)
                np.add.at(cnt, u, 1)
    curv /= np.maximum(cnt, 1)
    vert = curv[inv]
    img = np.zeros((size, size), np.float32)
    cov = np.zeros((size, size), bool)
    base = 0
    for p, _, uv, tris in parts:
        for t in tris:
            q = uv[t] * size
            lo = np.maximum(np.floor(q.min(0)).astype(int), 0)
            hi = np.minimum(np.ceil(q.max(0)).astype(int) + 1, size)
            if hi[0] <= lo[0] or hi[1] <= lo[1] or hi[0] - lo[0] > 600 or hi[1] - lo[1] > 600:
                continue
            e1, e2 = q[1] - q[0], q[2] - q[0]
            den = e1[0] * e2[1] - e1[1] * e2[0]
            if abs(den) < 1e-9:
                continue
            xs, ys = np.meshgrid(np.arange(lo[0], hi[0]) + .5, np.arange(lo[1], hi[1]) + .5)
            d0, d1 = xs - q[0, 0], ys - q[0, 1]
            b1 = (d0 * e2[1] - d1 * e2[0]) / den
            b2 = (e1[0] * d1 - e1[1] * d0) / den
            ins = (b1 >= -.02) & (b2 >= -.02) & (b1 + b2 <= 1.02)
            w = np.stack((1 - b1 - b2, b1, b2), -1)[ins]
            img[lo[1]:hi[1], lo[0]:hi[0]][ins] = w @ vert[base + t]
            cov[lo[1]:hi[1], lo[0]:hi[0]] |= ins
        base += len(p)
    edge = np.clip((img - 1.2) / 2.5, 0, 1) * cov          # convex only, above a ridge threshold
    chip = np.clip((_noise(7, size, 1.2) - .42) * 6, 0, 1)  # broken, not a ruler line
    return np.clip(ndimage.gaussian_filter(edge, .8) * 2, 0, 1) * chip


def weather(fields, alb, orm, rust_tex, macro_tex, streak_strength=1.0, macro_strength=.35):
    """Apply grime/rust streaks and macro variation. alb/orm are uint8 RGBA arrays; returns new copies."""
    pos, nrm, cov, down = fields
    alb, orm = alb.astype(np.float32) / 255, orm.astype(np.float32) / 255
    grime = streaks(nrm, cov, down, 11)
    rust = streaks(nrm, cov, down, 23, density=.015)
    low = np.clip((_noise(5, SIZE, 40) - .5) * 14 + .5, 0, 1)          # patchy: rust only in some bands
    rust = rust * low
    t = lambda im: np.asarray(Image.open(im).convert("RGB").resize((SIZE, SIZE)), np.float32) / 255  # noqa: E731
    rt = np.tile(t(rust_tex)[::2, ::2], (2, 2, 1))
    mt = np.tile(t(macro_tex)[::2, ::2], (2, 2, 1))
    lum = mt.mean(-1, keepdims=True)
    k = ((lum - lum.mean()) * macro_strength + 1)
    rgb = alb[..., :3] * np.where(cov[..., None], k, 1)
    g = (grime * streak_strength)[..., None]
    rgb = rgb * (1 - .55 * g)
    r = (rust * streak_strength)[..., None]
    rgb = rgb * (1 - .75 * r) + rt * rt * .55 * .75 * r   # rust texture is sRGB; squared to darken toward the paint
    alb[..., :3] = np.clip(rgb, 0, 1)
    orm[..., 1] = np.clip(orm[..., 1] + (grime * .3 + rust * .5) * streak_strength, 0, 1)
    orm[..., 2] = orm[..., 2] * (1 - np.clip(rust * streak_strength * 1.3, 0, 1))
    u8 = lambda a: np.uint8(np.clip(a, 0, 1) * 255 + .5)  # noqa: E731
    return u8(alb), u8(orm)


# Projected decals. axis = world direction the projector looks along (the surface normal must oppose it);
# at = world centre; size = metres (width along 'right', height along +Y); depth = accepted band along axis.
# cell = pixel rect (x0, y0, x1, y1) in the 2048 MechDecals_ACL atlas, trimmed to alpha when applied.
DECALS = [
    {"name": "fangs_front", "cell": (820, 1150, 1010, 1260), "axis": (-1, 0, 0), "at": (3.4, 13.3, 0), "size": (2.0, 1.0), "depth": 1.2},
    {"name": "chev_arm_l", "cell": (45, 1570, 470, 1650), "axis": (0, 0, 1), "at": (-1.5, 9.8, -5.8), "size": (1.5, .45), "depth": 1.0},
    {"name": "chev_arm_r", "cell": (45, 1570, 470, 1650), "axis": (0, 0, -1), "at": (-1.5, 9.8, 5.8), "size": (1.5, .45), "depth": 1.0},
    {"name": "warn_torso", "cell": (530, 1330, 720, 1410), "axis": (-1, 0, 0), "at": (3.4, 11.4, 0), "size": (1.6, .7), "depth": 1.2},
    {"name": "star_back", "cell": (1240, 450, 1400, 600), "axis": (1, 0, 0), "at": (-3.0, 12.2, 0), "size": (1.5, 1.5), "depth": 1.2},
    {"name": "nostep_calf_l", "cell": (1500, 1650, 1880, 1740), "axis": (0, 0, 1), "at": (-1.5, 4.0, -4.3), "size": (1.5, .45), "depth": 1.0},
    {"name": "nostep_calf_r", "cell": (1500, 1650, 1880, 1740), "axis": (0, 0, -1), "at": (-1.5, 4.0, 4.3), "size": (1.5, .45), "depth": 1.0},
    {"name": "caution_thigh_l", "cell": (1050, 1440, 1180, 1500), "axis": (0, 0, 1), "at": (-1.0, 7.5, -3.9), "size": (.9, .45), "depth": 1.0},
    {"name": "caution_thigh_r", "cell": (1050, 1440, 1180, 1500), "axis": (0, 0, -1), "at": (-1.0, 7.5, 3.9), "size": (.9, .45), "depth": 1.0},
]


def seat(fields, d, min_hits=1500):
    """Re-seat a decal that bounding-box scaling left off the surface: centre it on the outermost matching surface near its place."""
    pos, nrm, cov, _ = fields
    axis = np.asarray(d["axis"], np.float32)
    right = np.cross([0, 1, 0], axis)
    right = right / np.linalg.norm(right)
    rel = pos - np.asarray(d["at"], np.float32)
    facing = cov & ((nrm * axis).sum(-1) < -.55)
    across, up = (rel * right).sum(-1), rel[..., 1]
    if (facing & (np.abs((rel * axis).sum(-1)) < d["depth"]) & (np.abs(across) < d["size"][0] / 2) & (np.abs(up) < d["size"][1] / 2)).sum() >= min_hits:
        return d
    near = facing & (np.abs(across) < d["size"][0] * 1.6) & (np.abs(up) < d["size"][1] * 2.5)
    if not near.any():
        return d
    p = pos[near]
    along = (p * axis).sum(-1)
    outer = p[along < along.min() + .35]
    if len(outer) < 40:
        return d
    at = np.asarray(d["at"], np.float32)
    c = np.median(outer, axis=0)
    at = at + axis * float(((c - at) * axis).sum()) + right * float(((c - at) * right).sum()) + np.array([0, 1, 0], np.float32) * float(c[1] - at[1])
    return {**d, "at": tuple(float(v) for v in at), "depth": max(d["depth"], .6)}


def decals(fields, alb, orm, atlas, placements=DECALS):
    pos, nrm, cov, down = fields
    atlas = np.asarray(Image.open(atlas).convert("RGBA"), np.float32) / 255
    alb, orm = alb.astype(np.float32) / 255, orm.astype(np.float32) / 255
    srgb_to_lin = lambda c: np.where(c <= .04045, c / 12.92, ((c + .055) / 1.055) ** 2.4)  # noqa: E731
    for d in placements:
        d = seat(fields, d)
        x0, y0, x1, y1 = d["cell"]
        cell = atlas[y0:y1, x0:x1]
        ys, xs = np.nonzero(cell[..., 3] > .05)
        if len(ys) == 0:
            continue
        cell = cell[ys.min():ys.max() + 1, xs.min():xs.max() + 1]
        axis = np.asarray(d["axis"], np.float32)
        right = np.cross([0, 1, 0], axis)
        right = right / np.linalg.norm(right)
        rel = pos - np.asarray(d["at"], np.float32)
        a = (rel * right).sum(-1) / d["size"][0] + .5
        b = .5 - rel[..., 1] / d["size"][1]
        depth = np.abs((rel * axis).sum(-1))
        facing = (nrm * axis).sum(-1) < -.55
        hit = cov & facing & (depth < d["depth"]) & (a >= 0) & (a < 1) & (b >= 0) & (b < 1)
        if not hit.any():
            print("decal missed", d["name"])
            continue
        px = np.clip((a[hit] * cell.shape[1]).astype(int), 0, cell.shape[1] - 1)
        py = np.clip((b[hit] * cell.shape[0]).astype(int), 0, cell.shape[0] - 1)
        c = cell[py, px]
        al = c[:, 3:4] * .92
        cur = alb[hit][:, :3]
        alb[hit, :3] = cur * (1 - al) + c[:, :3] * al
        orm[hit, 1] = orm[hit, 1] * (1 - al[:, 0]) + .62 * al[:, 0]
        orm[hit, 2] = orm[hit, 2] * (1 - al[:, 0])
        print("decal", d["name"], int(hit.sum()), "texels")
    u8 = lambda a_: np.uint8(np.clip(a_, 0, 1) * 255 + .5)  # noqa: E731
    return u8(alb), u8(orm)


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--glb", required=True)
    ap.add_argument("--bake", required=True, help="dir with alb.png orm.png (bake_material.py output)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--rust", required=True)
    ap.add_argument("--macro", required=True)
    ap.add_argument("--atlas")
    a = ap.parse_args()
    f = rasterize(body_triangles(a.glb))
    alb = np.asarray(Image.open(Path(a.bake) / "alb.png").convert("RGBA"))
    orm = np.asarray(Image.open(Path(a.bake) / "orm.png").convert("RGBA"))
    alb, orm = weather(f, alb, orm, a.rust, a.macro)
    if a.atlas:
        alb, orm = decals(f, alb, orm, a.atlas)
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    Image.fromarray(alb).save(out / "alb.png")
    Image.fromarray(orm).save(out / "orm.png")
    Image.open(Path(a.bake) / "nrm.png").save(out / "nrm.png")
