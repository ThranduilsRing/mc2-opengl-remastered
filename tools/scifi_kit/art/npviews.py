"""Numpy textured rasteriser for the decoded STOCK mesh: the 5 count views without Blender (about a second per asset),
and the painted-opening evidence (dark rectangles per side) the validator holds the remake to.

Pure Python + numpy/scipy/PIL; never imported by Blender.
"""
import math
from pathlib import Path

import numpy as np
from PIL import Image

import analyze

# depth axis and the sign of "nearer to the camera" per view (front looks +Y: the camera sits at -Y, so smaller y is nearer)
DEPTH = {'front': (1, -1), 'left': (0, -1), 'rear': (1, 1), 'right': (0, 1), 'top': (2, 1)}
BG = (150, 160, 175)


def render_view(st, size, view, long_px=420):
    """-> (rgb uint8 HxWx3, covered bool HxW, box). Unlit stock texture colour, z-buffered, alpha tested."""
    d = st['raw']
    tex = [analyze._texture(p) for p in d['texture_paths']]
    V0 = st['V']
    V = V0 - np.array([(V0[..., i].min() + V0[..., i].max()) / 2 if i < 2 else V0[..., 2].min() for i in range(3)])
    box = analyze.view_box(size, view)
    W, H = analyze.grid_for(box, long_px)
    u0, u1, v0, v1 = box
    sx, sy = W / (u1 - u0), H / (v1 - v0)
    P2 = analyze.project(V, view)
    ax, sg = DEPTH[view[0]]
    depth = V[..., ax] * -sg
    img = np.zeros((H, W, 3), np.uint8)
    img[:] = BG
    zb = np.full((H, W), -1e9)
    cov = np.zeros((H, W), bool)
    for i in range(len(V)):
        t = np.empty((3, 2))
        t[:, 0] = (P2[i, :, 0] - u0) * sx
        t[:, 1] = (v1 - P2[i, :, 1]) * sy
        x0, x1 = int(max(0, math.floor(t[:, 0].min()))), int(min(W - 1, math.ceil(t[:, 0].max())))
        y0, y1 = int(max(0, math.floor(t[:, 1].min()))), int(min(H - 1, math.ceil(t[:, 1].max())))
        if x1 < 0 or y1 < 0 or x0 >= W or y0 >= H:
            continue
        (ax_, ay_), (bx, by), (cx, cy) = t
        det = (by - cy) * (ax_ - cx) + (cx - bx) * (ay_ - cy)
        if abs(det) < 1e-9:
            continue
        gx, gy = np.meshgrid(np.arange(x0, x1 + 1) + .5, np.arange(y0, y1 + 1) + .5)
        l1 = ((by - cy) * (gx - cx) + (cx - bx) * (gy - cy)) / det
        l2 = ((cy - ay_) * (gx - cx) + (ax_ - cx) * (gy - cy)) / det
        l3 = 1 - l1 - l2
        ins = (l1 >= -1e-6) & (l2 >= -1e-6) & (l3 >= -1e-6)
        mi = int(st['mat'][i])
        tx = tex[mi] if mi < len(tex) else None
        uv = st['UV'][i]
        uu = (l1 * uv[0, 0] + l2 * uv[1, 0] + l3 * uv[2, 0]) % 1
        vv = (l1 * uv[0, 1] + l2 * uv[1, 1] + l3 * uv[2, 1]) % 1
        if tx is not None:
            th, tw = tx.shape[:2]
            px = tx[(vv * th).astype(int) % th, (uu * tw).astype(int) % tw]
            col = px[..., :3]
            if mi < len(d['texture_alpha']) and d['texture_alpha'][mi]:
                ins &= px[..., 3] > 128
        else:
            col = np.full(ins.shape + (3,), 128, np.uint8)
        z = l1 * depth[i, 0] + l2 * depth[i, 1] + l3 * depth[i, 2]
        sub = zb[y0:y1 + 1, x0:x1 + 1]
        win = ins & (z > sub)
        sub[win] = z[win]
        img[y0:y1 + 1, x0:x1 + 1][win] = col[win]
        cov[y0:y1 + 1, x0:x1 + 1] |= win
    return img, cov, box


def save_stock_views(name, out_dir, long_px=420):
    """The five `<name>_stock_<view>.png` sheets the review reads (replaces the third Blender process)."""
    st = analyze.load_stock(name)
    V = st['V']
    size = (V.reshape(-1, 3).max(0) - V.reshape(-1, 3).min(0)).tolist()
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    for view in analyze.VIEWS:
        img, _, _ = render_view(st, size, view, long_px)
        Image.fromarray(img).save(out / f'{name}_stock_{view[0]}.png')


# ------------------------------------------------------------------ painted openings (dark rectangles per side)
def painted_openings(st, size, long_px=420):
    """Doors / windows / bays the stock TEXTURE paints on each wall: dark filled rectangles in the 4 side views.
    -> {'doors': [...], 'windows': [...], 'openings_bays': [...]} items dict(side, at_m=[u, z centre], size_m=[w, h], conf, mask_px)."""
    from scipy import ndimage
    feats = {'doors': [], 'windows': [], 'openings_bays': []}
    for view in analyze.VIEWS[:4]:
        img, cov, box = render_view(st, size, view, long_px)
        if cov.sum() < 400:
            continue
        lum = (img.astype(float) / 255) @ np.array([.2126, .7152, .0722])
        inner = ndimage.binary_erosion(cov, iterations=2)
        med = float(np.median(lum[inner])) if inner.any() else 0.0
        if med < .1:
            continue
        dark = inner & (lum < .62 * med)
        dark = ndimage.binary_opening(dark, iterations=1)
        H, W = lum.shape
        mpp = (box[1] - box[0]) / W
        lab, n = ndimage.label(dark)
        for j, sl in enumerate(ndimage.find_objects(lab), 1):
            ys, xs = sl
            bh, bw = ys.stop - ys.start, xs.stop - xs.start
            area = int((lab[sl] == j).sum())
            fill = area / max(bh * bw, 1)
            ww, hh = bw * mpp, bh * mpp
            if fill < .62 or ww < .4 or hh < .3 or ww / hh > 9 or hh / ww > 8:
                continue
            ztop = box[3] - ys.start * mpp
            zb = ztop - hh
            uc = box[0] + (xs.start + xs.stop) / 2 * mpp
            kind = None
            if zb < .7 and 1.6 <= hh <= 5.5 and .8 <= ww <= 7:
                kind = 'openings_bays' if (ww > 3.2 and hh > 3.0) else 'doors'
            elif zb >= .5 and .4 <= ww <= 6 and .3 <= hh <= 3.2:
                kind = 'windows'
            if kind:
                feats[kind].append(dict(side=view[0], at_m=[round(float(uc), 1), round(float(zb + hh / 2), 1)], size_m=[round(float(ww), 1), round(float(hh), 1)],
                                        conf=round(float(.45 + .4 * min(1, fill)), 2), source='stock texture: dark rectangle in the textured side view'))
    _ground_panels(st, size, long_px, feats)
    return feats


def _ground_panels(st, size, long_px, feats):
    """Doors and roll-up bays are painted as a panel of a different colour (not always dark) standing on the ground line:
    a colour-outlier region, 1.6-5.5 m tall, touching the base. Windows detected inside one become part of it."""
    from scipy import ndimage
    for view, thr in [(v, t) for v in analyze.VIEWS[:4] for t in (.17, .12)]:
        img, cov, box = render_view(st, size, view, long_px)
        inner = ndimage.binary_erosion(cov, iterations=2)
        if inner.sum() < 400:
            continue
        rgb = img.astype(float) / 255
        med = np.median(rgb[inner], axis=0)
        dist = np.linalg.norm(rgb - med, axis=2)
        mask = inner & (dist > thr)
        mask = ndimage.binary_closing(ndimage.binary_opening(mask, iterations=1), iterations=2)
        H, W = dist.shape
        mpp = (box[1] - box[0]) / W
        lab, n = ndimage.label(mask)
        for j, sl in enumerate(ndimage.find_objects(lab), 1):
            ys, xs = sl
            bh, bw = ys.stop - ys.start, xs.stop - xs.start
            fill = int((lab[sl] == j).sum()) / max(bh * bw, 1)
            ww, hh = bw * mpp, bh * mpp
            zb = box[3] - ys.stop * mpp
            if fill < .7 or zb > .7 or not (1.6 <= hh <= 5.5) or not (.8 <= ww <= 7):
                continue
            uc = box[0] + (xs.start + xs.stop) / 2 * mpp
            zc = zb + hh / 2
            if any(f['side'] == view[0] and abs(f['at_m'][0] - uc) < (f['size_m'][0] + ww) / 3 for f in feats['doors'] + feats['openings_bays']):
                continue        # the same panel already found at the other threshold
            for k in ('windows',):      # a painted window inside the door panel is part of the door
                feats[k] = [f for f in feats[k] if not (f['side'] == view[0] and abs(f['at_m'][0] - uc) < ww / 2 + .2 and zb - .2 <= f['at_m'][1] <= zb + hh + .2)]
            kind = 'openings_bays' if (ww > 3.2 and hh > 3.0) else 'doors'
            feats['doors' if kind == 'doors' else 'openings_bays'].append(dict(side=view[0], at_m=[round(float(uc), 1), round(float(zc), 1)], size_m=[round(float(ww), 1), round(float(hh), 1)],
                                                                          conf=round(float(.45 + .4 * min(1, fill)), 2), source='stock texture: ground-standing colour panel in the textured side view'))
