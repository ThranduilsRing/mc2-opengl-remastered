"""Tiny numpy software rasteriser for prop thumbnails and review sheets (offline, no GPU).

Orthographic, y-up, z-buffered, lambert shaded, nearest texture sampling. Meant for 100-200 px
thumbnails so catalogs and review sheets render without launching the engine.
"""
from __future__ import annotations

import numpy as np

SKY = (36, 40, 48)
GROUND = (70, 74, 70)


def view_matrix(yaw_deg, pitch_deg):
    y, p = np.radians(yaw_deg), np.radians(pitch_deg)
    ry = np.array([[np.cos(y), 0, np.sin(y)], [0, 1, 0], [-np.sin(y), 0, np.cos(y)]])
    rx = np.array([[1, 0, 0], [0, np.cos(p), -np.sin(p)], [0, np.sin(p), np.cos(p)]])
    return rx @ ry


def render(prims, size=128, yaw=35.0, pitch=22.0, px_per_unit=None, fit=None, ground=True, max_tris=24000):
    """prims: [{"pos": (N,3), "idx": (M,3), "uv": (N,2)|None, "tex": HxWx3|None, "color": (r,g,b)}].

    px_per_unit fixes the scale (sheets share one scale per row); otherwise the bbox fits `fit` of the frame.
    Returns (rgb uint8 HxWx3, coverage bool HxW).
    """
    rot = view_matrix(yaw, pitch)
    allp = np.concatenate([p["pos"] for p in prims]) @ rot.T
    lo, hi = allp.min(0), allp.max(0)
    span = max(hi[0] - lo[0], hi[1] - lo[1], 1e-6)
    scale = px_per_unit if px_per_unit else (fit or 0.82) * size / span
    cx = (lo[0] + hi[0]) / 2
    base = lo[1]
    img = np.empty((size, size, 3), np.uint8)
    img[:] = SKY
    if ground:
        img[int(size * 0.86):] = GROUND
    zbuf = np.full((size, size), -1e18, np.float32)
    cover = np.zeros((size, size), bool)
    light = np.array([0.4, 0.8, 0.45])
    light /= np.linalg.norm(light)
    tris = []
    for pi, p in enumerate(prims):
        v = p["pos"] @ rot.T
        sx = (v[:, 0] - cx) * scale + size / 2
        sy = size * 0.86 - (v[:, 1] - base) * scale
        tri = p["idx"]
        a, b, c = v[tri[:, 0]], v[tri[:, 1]], v[tri[:, 2]]
        n = np.cross(b - a, c - a)
        nl = np.linalg.norm(n, axis=1) + 1e-12
        n = n / nl[:, None]
        n_world = n @ rot  # back to model space for lighting
        shade = 0.35 + 0.65 * np.clip(np.abs(n_world @ light), 0, 1)
        area = 0.5 * np.abs((sx[tri[:, 1]] - sx[tri[:, 0]]) * (sy[tri[:, 2]] - sy[tri[:, 0]])
                            - (sx[tri[:, 2]] - sx[tri[:, 0]]) * (sy[tri[:, 1]] - sy[tri[:, 0]]))
        for k in range(len(tri)):
            tris.append((area[k], pi, k))
        p["_sx"], p["_sy"], p["_z"], p["_shade"] = sx, sy, v[:, 2], shade
    tris.sort(key=lambda t: -t[0])
    for _, pi, k in tris[:max_tris]:
        p = prims[pi]
        i0, i1, i2 = p["idx"][k]
        xs = p["_sx"][[i0, i1, i2]]
        ys = p["_sy"][[i0, i1, i2]]
        x0, x1 = int(max(np.floor(xs.min()), 0)), int(min(np.ceil(xs.max()), size - 1))
        y0, y1 = int(max(np.floor(ys.min()), 0)), int(min(np.ceil(ys.max()), size - 1))
        if x1 < x0 or y1 < y0:
            continue
        shade = p["_shade"][k]
        col = np.array(p.get("color", (150, 150, 150)), np.float32)
        gx, gy = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
        den = (ys[1] - ys[2]) * (xs[0] - xs[2]) + (xs[2] - xs[1]) * (ys[0] - ys[2])
        if abs(den) < 1e-9:
            continue
        w0 = ((ys[1] - ys[2]) * (gx - xs[2]) + (xs[2] - xs[1]) * (gy - ys[2])) / den
        w1 = ((ys[2] - ys[0]) * (gx - xs[2]) + (xs[0] - xs[2]) * (gy - ys[2])) / den
        w2 = 1 - w0 - w1
        inside = (w0 >= -0.02) & (w1 >= -0.02) & (w2 >= -0.02)
        if not inside.any():
            continue
        z = w0 * p["_z"][i0] + w1 * p["_z"][i1] + w2 * p["_z"][i2]
        zb = zbuf[y0:y1 + 1, x0:x1 + 1]
        win = inside & (z > zb)
        if not win.any():
            continue
        if p.get("tex") is not None and p.get("uv") is not None:
            uv = w0[..., None] * p["uv"][i0] + w1[..., None] * p["uv"][i1] + w2[..., None] * p["uv"][i2]
            th, tw = p["tex"].shape[:2]
            tx = np.clip((uv[..., 0] % 1.0) * tw, 0, tw - 1).astype(int)
            ty = np.clip((uv[..., 1] % 1.0) * th, 0, th - 1).astype(int)
            pix = p["tex"][ty, tx][..., :3].astype(np.float32)
        else:
            pix = np.broadcast_to(col, (*gx.shape, 3))
        out = np.clip(pix * shade, 0, 255).astype(np.uint8)
        region = img[y0:y1 + 1, x0:x1 + 1]
        region[win] = out[win]
        zb[win] = z[win]
        cover[y0:y1 + 1, x0:x1 + 1] |= win
    return img, cover


def silhouette(prims, grid=12):
    """Aspect-normalised occupancy of front and side views (2*grid*grid bool), from triangle samples."""
    bits = []
    pts = []
    for p in prims:
        t = p["idx"]
        pos = p["pos"]
        pts.append(np.concatenate([pos, pos[t].mean(1), (pos[t[:, 0]] + pos[t[:, 1]]) / 2]))
    pts = np.concatenate(pts)
    lo, hi = pts.min(0), pts.max(0)
    span = np.maximum(hi - lo, 1e-6)
    q = (pts - lo) / span
    for ax in (0, 2):
        h, _, _ = np.histogram2d(q[:, ax], q[:, 1], bins=grid, range=[[0, 1], [0, 1]])
        bits.append(h > 0)
    return np.concatenate([b.reshape(-1) for b in bits])


def contact_sheet(tiles, labels, cols, tile=128, label_h=26, font=None):
    from PIL import Image, ImageDraw
    rows = (len(tiles) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * tile, rows * (tile + label_h)), (20, 22, 26))
    d = ImageDraw.Draw(sheet)
    for i, (t, lab) in enumerate(zip(tiles, labels)):
        x, y = (i % cols) * tile, (i // cols) * (tile + label_h)
        sheet.paste(Image.fromarray(t) if isinstance(t, np.ndarray) else t, (x, y))
        d.text((x + 3, y + tile + 2), lab[:30], fill=(220, 220, 220), font=font)
        if len(lab) > 30:
            d.text((x + 3, y + tile + 13), lab[30:60], fill=(220, 220, 220), font=font)
    return sheet
