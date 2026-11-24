#!/usr/bin/env python3
"""P6: offline mech icons rendered from the pack model (no engine, no GPU).

Products per chassis (written into the pack's data/art/):
  <stem>_hud_<w>x<h>.tga   HUD force-group strip, one cell wide, two rows: row 0 = front view,
                           row 1 = rear view. Opaque like stock: every pixel is black (background
                           and panel gaps) or an EXACT region palette value: one of
                           ForceGroupIcon::damageColors[0][shade] with the body-region index
                           added to the red channel (code/mechicon.cpp). The HUD recolours by
                           exact value, so no filtering, blending or antialiasing is allowed.
                           Sizes: 25x30, 32x38, 40x48 (mechicon.cpp setIconVariables).
  <stem>_bay.tga           lit mech-lab/bay portrait (RGBA, alpha cut-out).

Region index i (mechicon.cpp MechIcon::update): 0 LeftLeg, 1 RightLeg, 2 LeftArm, 3 RightArm,
4 RightTorso, 5 LeftTorso, 6 CenterTorso, 7 Head. The rear row only draws regions 4-7.

Geometry: the converted GLB's intact parts (no *_dmg / *_explode / ui meshes) at the rest
pose (joint world x inverse bind), part -> region from the BT part name
(<tag>_<left|right>_<arm|leg|torso>..., centre_torso*, head*).
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image

DAMAGE0 = [(194, 229, 255), (92, 150, 194), (0, 83, 146)]   # damageColors[0][0..2] (R,G,B)
OUTLINE = (0x50, 0x50, 0x50)
HUD_SIZES = [(25, 30), (32, 38), (40, 48)]
REGION = {"left_leg": 0, "right_leg": 1, "left_arm": 2, "right_arm": 3,
          "right_torso": 4, "left_torso": 5, "centre_torso": 6, "center_torso": 6, "head": 7}
REAR_REGIONS = {4, 5, 6, 7}


def region_of(part_name: str):
    n = part_name.lower()
    if "_dmg" in n or "explode" in n or n.startswith("uix"):
        return None
    for key in ("left_leg", "right_leg", "left_arm", "right_arm", "right_torso", "left_torso",
                "centre_torso", "center_torso", "head"):
        if key in n:
            return REGION[key]
    return None


def region_color(region: int, shade: int):
    r, g, b = DAMAGE0[shade]
    return (r + region, g, b)


# ----------------------------------------------------------------------------- GLB


def _accessor(g, blob, idx):
    acc = g.accessors[idx]
    bv = g.bufferViews[acc.bufferView]
    comps = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}[acc.type]
    dtype = {5126: np.float32, 5123: np.uint16, 5125: np.uint32, 5121: np.uint8}[acc.componentType]
    start = (bv.byteOffset or 0) + (acc.byteOffset or 0)
    item = comps * np.dtype(dtype).itemsize
    stride = bv.byteStride or item
    raw = np.frombuffer(blob, dtype=np.uint8, count=stride * (acc.count - 1) + item, offset=start)
    rows = np.lib.stride_tricks.as_strided(raw, shape=(acc.count, item), strides=(stride, 1))
    return np.ascontiguousarray(rows).view(dtype).reshape(acc.count, comps)


def _local(node):
    if node.matrix:
        return np.array(node.matrix, dtype=np.float64).reshape(4, 4).T
    t = np.array(node.translation or [0, 0, 0], dtype=np.float64)
    x, y, z, w = node.rotation or [0, 0, 0, 1]
    s = np.array(node.scale or [1, 1, 1], dtype=np.float64)
    r = np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                  [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                  [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])
    m = np.eye(4)
    m[:3, :3] = r * s
    m[:3, 3] = t
    return m


def load_parts(glb: Path):
    """[(region, world_positions[N,3], triangles[M,3])] at the rest pose."""
    from pygltflib import GLTF2
    g = GLTF2().load(str(glb))
    blob = g.binary_blob()
    parent = {}
    for i, n in enumerate(g.nodes):
        for c in n.children or []:
            parent[c] = i
    world = {}

    def wm(i):
        if i not in world:
            world[i] = (wm(parent[i]) if i in parent else np.eye(4)) @ _local(g.nodes[i])
        return world[i]

    parts = []
    for ni, node in enumerate(g.nodes):
        if node.mesh is None:
            continue
        region = region_of(node.name or g.meshes[node.mesh].name or "")
        if region is None:
            continue
        skin = g.skins[node.skin] if node.skin is not None else None
        ibm = _accessor(g, blob, skin.inverseBindMatrices).reshape(-1, 4, 4).transpose(0, 2, 1) if skin and skin.inverseBindMatrices is not None else None
        for prim in g.meshes[node.mesh].primitives:
            pos = _accessor(g, blob, prim.attributes.POSITION).astype(np.float64)
            idx = _accessor(g, blob, prim.indices).reshape(-1, 3) if prim.indices is not None else np.arange(len(pos)).reshape(-1, 3)
            if skin is not None and prim.attributes.JOINTS_0 is not None:
                j0 = _accessor(g, blob, prim.attributes.JOINTS_0)[:, 0].astype(int)   # rigid BT parts: one joint
                mats = np.stack([wm(skin.joints[j]) @ ibm[j] for j in range(len(skin.joints))])
                ph = np.c_[pos, np.ones(len(pos))]
                out = np.einsum("nij,nj->ni", mats[j0], ph)[:, :3]
            else:
                out = (np.c_[pos, np.ones(len(pos))] @ wm(ni).T)[:, :3]
            parts.append((region, out, idx))
    return parts


def region_of_tgl(node: str):
    """MC2/MCO shape node (ra_luarm, ra_llleg, ra_lfoot, joint_lbtoe, ra_torso2, ra_hip...) -> HUD region."""
    n = node.lower().split("_", 1)[-1]
    side = n[:1] if n[:1] in "lr" and len(n) > 1 else ""
    if any(k in n for k in ("head", "cockpit")):
        return 7
    if side and any(k in n[1:] for k in ("arm", "shoulder", "hand", "gun", "weapon")):
        return 2 if side == "l" else 3
    if side and any(k in n[1:] for k in ("leg", "foot", "toe", "thigh", "calf", "ankle", "knee", "shin")):
        return 0 if side == "l" else 1
    if side and "torso" in n[1:]:
        return 5 if side == "l" else 4
    return 6


def load_parts_tgl(tgl: Path):
    """[(region, positions, triangles, part)] of a legacy MC2/MCO .tgl shape, turned to face +Z like the GLBs."""
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
    from tools.assets.common import parse_tgl
    mesh = parse_tgl(Path(tgl).read_bytes())
    parts, at = [], 0
    for index, node in enumerate(mesh.nodes):
        if not node["triangles"] or node["helper"]:
            continue
        tri = mesh.vertices[at:at + 3 * node["triangles"]].astype(np.float64) * [-1, 1, -1]   # MC2 shapes face -Z
        at += 3 * node["triangles"]
        parts.append((region_of_tgl(node["name"]), tri, np.arange(len(tri)).reshape(-1, 3), index))
    if not any(region in (0, 1) for region, *_ in parts):            # no legs: a vehicle, drawn from above, nose up
        parts = [(region, p[:, [0, 2, 1]] * [1, 1, -1], tris, index) for region, p, tris, index in parts]
    return parts


# ----------------------------------------------------------------------------- raster


SS = 8                                   # render large, then reduce each SSxSS block to one cell pixel
LIGHT_FRAC, MID_FRAC = 0.25, 0.30        # stock tone mix (atlas/catapult/bushwacker/jenner): 25% light, 30% mid, 45% dark
DEPTH_STEP = 0.15                        # a jump of this share of the figure's depth inside one panel is a gap too


def _hud_render(parts, w, h, rear: bool):
    """Stock-style HUD cell (measured on stock atlas/catapult/bushwacker/jenner at 40x48).

    Stock cells are opaque black with the mech as plated panels: a 1 px black gap between panels
    (armour zones, and any separate part or depth step inside one), light on each panel's
    top-left, dark on its bottom-right, about 25/30/45 light/mid/dark, the figure ~47 px of 48."""
    lam, reg, part, z = _raster(parts, w * SS, h * SS, rear)
    blocks = lambda a: a.reshape(h, SS, w, SS).transpose(0, 2, 1, 3).reshape(h, w, SS * SS)
    rb, pb, lb, zb = blocks(reg), blocks(part), blocks(lam), blocks(z)
    filled = rb >= 0
    solid = filled.sum(2) >= SS * SS // 2
    sp = np.pad(solid, 1)                                # pinholes in a plate are fill, not gaps
    solid |= (filled.sum(2) > 0) & sp[:-2, 1:-1] & sp[2:, 1:-1] & sp[1:-1, :-2] & sp[1:-1, 2:]
    img = np.zeros((h, w, 4), dtype=np.uint8)
    img[:, :, 3] = 255
    if not solid.any():
        return img
    count = lambda a, k: (a == k).sum(2)
    region = np.argmax(np.stack([count(rb, r) for r in range(8)]), 0)
    ids = np.unique(pb[pb >= 0])
    panel = ids[np.argmax(np.stack([count(pb, i) for i in ids]), 0)]
    # Bolts, vents and other parts smaller than a few cell pixels read as surface detail, not plates.
    sizes = {int(i): int((solid & (panel == i)).sum()) for i in ids}
    panel = np.where(np.vectorize(lambda i: sizes.get(int(i), 0) >= 10)(panel), panel, -1 - region)
    # Interpenetrating parts flicker between owners pixel to pixel: each pixel takes its 3x3 majority owner.
    pp = np.pad(np.where(solid, panel, -99), 1, constant_values=-99)
    for y, x in zip(*np.nonzero(solid)):
        vals, counts = np.unique(pp[y:y + 3, x:x + 3], return_counts=True)
        counts[vals == -99] = 0
        panel[y, x] = vals[np.argmax(counts)]
    # High-poly faces shade noisily: each pixel takes the mean light of its 3x3 within its own panel.
    raw = np.where(filled, lb, 0).sum(2) / np.maximum(filled.sum(2), 1)
    rp, pq = np.pad(raw, 1), np.pad(np.where(solid, panel, -99), 1, constant_values=-99)
    smooth = raw.copy()
    for y, x in zip(*np.nonzero(solid)):
        same = pq[y:y + 3, x:x + 3] == panel[y, x]
        smooth[y, x] = rp[y:y + 3, x:x + 3][same].mean()
    lit = smooth
    depth = np.where(filled, zb, 0).sum(2) / np.maximum(filled.sum(2), 1)
    step = DEPTH_STEP * max(float(z[reg >= 0].max() - z[reg >= 0].min()), 1e-6)
    # Gap: where two panels (or a depth step) meet, the farther pixel turns black.
    gap = np.zeros_like(solid)
    for dy, dx in ((1, 0), (0, 1)):
        a = (slice(0, h - dy), slice(0, w - dx))
        b = (slice(dy, h), slice(dx, w))
        meet = solid[a] & solid[b] & ((panel[a] != panel[b]) | (region[a] != region[b]) | (abs(depth[a] - depth[b]) > step))
        far_a = depth[a] < depth[b]
        gap[a] |= meet & far_a
        gap[b] |= meet & ~far_a
    for r in range(8):                                   # never erase a region entirely
        if (solid & (region == r)).any() and not (solid & ~gap & (region == r)).any():
            gap[region == r] = False
    body = solid & ~gap
    # Bevel: a panel's top/left edge catches the light, its bottom/right edge falls into shadow.
    pad = np.pad(body, 1)
    above, left, below, right = pad[:-2, 1:-1], pad[1:-1, :-2], pad[2:, 1:-1], pad[1:-1, 2:]
    score = lit + 0.6 * ((~above).astype(float) + (~left)) - 0.6 * ((~below).astype(float) + (~right))
    vals = score[body]
    hi_cut = np.quantile(vals, 1 - LIGHT_FRAC)
    mid_cut = np.quantile(vals, 1 - LIGHT_FRAC - MID_FRAC)
    shade = np.where(score >= hi_cut, 0, np.where(score >= mid_cut, 1, 2))
    # Ordered, not noisy: a lone pixel whose four body neighbours agree takes their shade.
    sp = np.pad(np.where(body, shade, -1), 1, constant_values=-1)
    nb = np.stack([sp[:-2, 1:-1], sp[2:, 1:-1], sp[1:-1, :-2], sp[1:-1, 2:]])
    lone = body & (nb[0] >= 0) & (nb == nb[0]).all(0) & (nb[0] != shade)
    shade = np.where(lone, nb[0], shade)
    for y, x in zip(*np.nonzero(body)):
        img[y, x, :3] = region_color(int(region[y, x]), int(shade[y, x]))
    return img


def _raster(parts, w, h, rear: bool):
    """Per-subsample lit value (n.L, light from the top-left), region and part (-1 empty), depth."""
    shown = [(i, part) for i, part in enumerate(parts) if not (rear and part[0] not in REAR_REGIONS)] or list(enumerate(parts))
    allp = np.concatenate([p for _, (_, p, *_) in shown])
    lo, hi = allp.min(0), allp.max(0)
    sx = -1.0 if not rear else 1.0
    m = SS // 2                                          # half a cell pixel of margin; the figure stands ~h-1 tall like stock
    scale = min((w - 2 * m) / max(hi[0] - lo[0], 1e-6), (h - 2 * m) / max(hi[1] - lo[1], 1e-6))
    cx, cy = (hi[0] + lo[0]) / 2, (hi[1] + lo[1]) / 2
    zbuf = np.full((h, w), -np.inf)
    lam = np.zeros((h, w))
    reg = np.full((h, w), -1, dtype=int)
    pid = np.full((h, w), -1, dtype=int)
    view = np.array([0, 0, -1.0 if rear else 1.0])
    light = np.array([-0.5 * sx, 0.6, 0.6 * view[2]])
    light /= np.linalg.norm(light)
    for index, (region, p, tris, *ident) in shown:
        part_id = ident[0] if ident else index
        X = (p[:, 0] - cx) * sx * scale + w / 2
        Y = h / 2 - (p[:, 1] - cy) * scale
        Z = p[:, 2] if not rear else -p[:, 2]
        for a, b, c in tris:
            n = np.cross(p[b] - p[a], p[c] - p[a])
            nl = np.linalg.norm(n)
            if nl < 1e-12:
                continue
            n /= nl
            if n @ view < 0:
                n = -n
            val = float(n @ light)
            xs, ys = (X[a], X[b], X[c]), (Y[a], Y[b], Y[c])
            x0, x1 = max(int(np.floor(min(xs))), 0), min(int(np.ceil(max(xs))), w - 1)
            y0, y1 = max(int(np.floor(min(ys))), 0), min(int(np.ceil(max(ys))), h - 1)
            if x0 > x1 or y0 > y1:
                continue
            gx, gy = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
            d = (ys[1] - ys[2]) * (xs[0] - xs[2]) + (xs[2] - xs[1]) * (ys[0] - ys[2])
            if abs(d) < 1e-12:
                continue
            l0 = ((ys[1] - ys[2]) * (gx - xs[2]) + (xs[2] - xs[1]) * (gy - ys[2])) / d
            l1 = ((ys[2] - ys[0]) * (gx - xs[2]) + (xs[0] - xs[2]) * (gy - ys[2])) / d
            inside = (l0 >= 0) & (l1 >= 0) & (1 - l0 - l1 >= 0)
            if not inside.any():
                continue
            z = l0 * Z[a] + l1 * Z[b] + (1 - l0 - l1) * Z[c]
            win = inside & (z > zbuf[y0:y1 + 1, x0:x1 + 1])
            zbuf[y0:y1 + 1, x0:x1 + 1][win] = z[win]
            lam[y0:y1 + 1, x0:x1 + 1][win] = val
            reg[y0:y1 + 1, x0:x1 + 1][win] = region
            pid[y0:y1 + 1, x0:x1 + 1][win] = part_id
    zbuf[reg < 0] = 0
    return lam, reg, pid, zbuf


def render(parts, w, h, rear: bool, mode: str):
    """Orthographic z-buffered raster. mode 'hud' -> exact palette; 'lit' -> shaded RGBA."""
    if mode == "hud":
        return _hud_render(parts, w, h, rear)
    allp = np.concatenate([p for _, p, _ in parts])
    lo, hi = allp.min(0), allp.max(0)
    up = 1                                               # glTF Y-up
    # Front view looks down -Z (the model faces +Z); x flips so the mech's left is image right.
    sx = -1.0 if not rear else 1.0
    ext_x, ext_y = hi[0] - lo[0], hi[up] - lo[up]
    scale = min((w - 2) / max(ext_x, 1e-6), (h - 2) / max(ext_y, 1e-6))
    cx, cy = (hi[0] + lo[0]) / 2, (hi[up] + lo[up]) / 2
    zbuf = np.full((h, w), -np.inf)
    img = np.zeros((h, w, 4), dtype=np.uint8)
    light = np.array([0.3, 0.6, 0.75 if not rear else -0.75])
    light /= np.linalg.norm(light)
    for region, p, tris in parts:
        if mode == "hud" and rear and region not in REAR_REGIONS:
            continue
        X = (p[:, 0] - cx) * sx * scale + w / 2
        Y = h / 2 - (p[:, up] - cy) * scale
        Z = p[:, 2] if not rear else -p[:, 2]
        for a, b, c in tris:
            n = np.cross(p[b] - p[a], p[c] - p[a])
            nl = np.linalg.norm(n)
            if nl < 1e-12:
                continue
            n /= nl
            lam = abs(float(n @ light))
            shade = 0 if lam > 0.66 else (1 if lam > 0.33 else 2)
            xs, ys = (X[a], X[b], X[c]), (Y[a], Y[b], Y[c])
            x0, x1 = max(int(np.floor(min(xs))), 0), min(int(np.ceil(max(xs))), w - 1)
            y0, y1 = max(int(np.floor(min(ys))), 0), min(int(np.ceil(max(ys))), h - 1)
            if x0 > x1 or y0 > y1:
                continue
            gx, gy = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
            d = (ys[1] - ys[2]) * (xs[0] - xs[2]) + (xs[2] - xs[1]) * (ys[0] - ys[2])
            if abs(d) < 1e-12:
                continue
            l0 = ((ys[1] - ys[2]) * (gx - xs[2]) + (xs[2] - xs[1]) * (gy - ys[2])) / d
            l1 = ((ys[2] - ys[0]) * (gx - xs[2]) + (xs[0] - xs[2]) * (gy - ys[2])) / d
            l2 = 1 - l0 - l1
            inside = (l0 >= 0) & (l1 >= 0) & (l2 >= 0)
            if not inside.any():
                continue
            z = l0 * Z[a] + l1 * Z[b] + l2 * Z[c]
            sub = zbuf[y0:y1 + 1, x0:x1 + 1]
            win = inside & (z > sub)
            sub[win] = z[win]
            if mode == "hud":
                col = region_color(region, shade)
            else:
                v = int(70 + 170 * lam)
                col = (v, int(v * 0.95), int(v * 0.88))
            img[y0:y1 + 1, x0:x1 + 1][win] = (*col, 255)
    return img


def hud_strip(parts, w, h):
    front = render(parts, w, h, rear=False, mode="hud")
    back = render(parts, w, h, rear=True, mode="hud")
    return np.concatenate([front, back], axis=0)


def palette_ok(img: np.ndarray, rear_row_from: int | None = None) -> list[str]:
    """Every opaque pixel must be black, the legacy outline or an exact region palette value."""
    allowed = {OUTLINE, (0, 0, 0)}
    for r in range(8):
        for s in range(3):
            allowed.add(region_color(r, s))
    bad = []
    ys, xs = np.nonzero(img[:, :, 3])
    for y, x in zip(ys, xs):
        c = tuple(int(v) for v in img[y, x, :3])
        if c not in allowed:
            bad.append(f"{x},{y}={c}")
        elif rear_row_from is not None and y >= rear_row_from and c not in (OUTLINE, (0, 0, 0)):
            region = c[0] - DAMAGE0[[d[1] for d in DAMAGE0].index(c[1])][0]
            if region not in REAR_REGIONS:
                bad.append(f"rear {x},{y} region {region}")
    return bad


def save_tga(img: np.ndarray, path: Path):
    path.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(img, "RGBA").save(path)


def build_icons(glb: Path, stem: str, out_art: Path, bay_size=(128, 128), hud_only=False):
    parts = load_parts_tgl(glb) if Path(glb).suffix.lower() == ".tgl" else load_parts(glb)
    if not parts:
        raise SystemExit(f"{glb}: no intact parts mapped to regions")
    written = {}
    for w, h in HUD_SIZES:
        strip = hud_strip(parts, w, h)
        bad = palette_ok(strip, rear_row_from=h)
        if bad:
            raise SystemExit(f"{stem} {w}x{h}: non-palette pixels {bad[:5]}")
        p = out_art / f"{stem}_hud_{w}x{h}.tga"
        save_tga(strip, p)
        written[p.name] = strip
    if hud_only:
        return written
    bay = render(parts, *bay_size, rear=False, mode="lit")
    p = out_art / f"{stem}_bay.tga"
    save_tga(bay, p)
    written[p.name] = bay
    # Logistics resolves individual icons by CSV stem, independently of the legacy sheet index.
    small = render(parts, 25, 30, rear=False, mode="hud")
    for green, value in ((229, 255), (150, 160), (83, 96), (80, 32)):
        mask = (small[:, :, 1] == green) & (small[:, :, 3] != 0)
        small[mask, :3] = value
    small[(small[:, :, :3] == 0).all(2), 3] = 0                 # the stock cell's black ground is the card's own
    p = out_art / "mechicons" / f"{stem}.png"
    p.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(small, "RGBA").save(p)
    written[p.relative_to(out_art).as_posix()] = small
    return written


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("glb")
    ap.add_argument("--stem", required=True)
    ap.add_argument("--out", required=True, help="output art dir (e.g. <pack>/data/art)")
    ap.add_argument("--preview", default="", help="also write an enlarged preview PNG of every product")
    a = ap.parse_args()
    out = build_icons(Path(a.glb), a.stem, Path(a.out))
    for name, img in out.items():
        print(f"{name}: {img.shape[1]}x{img.shape[0]}")
        if a.preview:
            Path(a.preview).mkdir(parents=True, exist_ok=True)
            Image.fromarray(img, "RGBA").resize((img.shape[1] * 6, img.shape[0] * 6), Image.NEAREST).save(
                Path(a.preview) / (Path(name).stem + "_x6.png"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
