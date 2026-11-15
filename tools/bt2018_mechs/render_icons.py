#!/usr/bin/env python3
"""P6: offline mech icons rendered from the pack model (no engine, no GPU).

Products per chassis (written into the pack's data/art/):
  <stem>_hud_<w>x<h>.tga   HUD force-group strip, one cell wide, two rows: row 0 = front view,
                           row 1 = rear view. Every pixel is either 0 (transparent), the
                           outline grey 0xff505050, or an EXACT region palette value: one of
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


# ----------------------------------------------------------------------------- raster


def render(parts, w, h, rear: bool, mode: str):
    """Orthographic z-buffered raster. mode 'hud' -> exact palette; 'lit' -> shaded RGBA."""
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
    if mode == "hud":
        filled = img[:, :, 3] > 0
        edge = np.zeros_like(filled)
        for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            edge |= np.roll(filled, (dy, dx), axis=(0, 1)) & ~filled
        img[edge] = (*OUTLINE, 255)
    return img


def hud_strip(parts, w, h):
    front = render(parts, w, h, rear=False, mode="hud")
    back = render(parts, w, h, rear=True, mode="hud")
    return np.concatenate([front, back], axis=0)


def palette_ok(img: np.ndarray, rear_row_from: int | None = None) -> list[str]:
    """Every opaque pixel must be the outline or an exact region palette value."""
    allowed = {OUTLINE}
    for r in range(8):
        for s in range(3):
            allowed.add(region_color(r, s))
    bad = []
    ys, xs = np.nonzero(img[:, :, 3])
    for y, x in zip(ys, xs):
        c = tuple(int(v) for v in img[y, x, :3])
        if c not in allowed:
            bad.append(f"{x},{y}={c}")
        elif rear_row_from is not None and y >= rear_row_from and c != OUTLINE:
            region = c[0] - DAMAGE0[[d[1] for d in DAMAGE0].index(c[1])][0]
            if region not in REAR_REGIONS:
                bad.append(f"rear {x},{y} region {region}")
    return bad


def save_tga(img: np.ndarray, path: Path):
    path.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(img, "RGBA").save(path)


def build_icons(glb: Path, stem: str, out_art: Path, bay_size=(128, 128)):
    parts = load_parts(glb)
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
    bay = render(parts, *bay_size, rear=False, mode="lit")
    p = out_art / f"{stem}_bay.tga"
    save_tga(bay, p)
    written[p.name] = bay
    # Logistics resolves individual icons by CSV stem, independently of the legacy sheet index.
    small = render(parts, 25, 30, rear=False, mode="hud")
    for green, value in ((229, 255), (150, 160), (83, 96), (80, 32)):
        mask = (small[:, :, 1] == green) & (small[:, :, 3] != 0)
        small[mask, :3] = value
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
