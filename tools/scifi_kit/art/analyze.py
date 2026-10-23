"""Automatic stock analysis: `run.py analyze <asset>` writes assets/<asset>.analysis.json.

Reads ONLY the decoded stock mesh and texture (round2/stock/<id>.json) for measurement.
The agent confirms or corrects the draft; it does not derive it. Pure Python + numpy/PIL/scipy,
never imported by Blender.
"""
import json
import math
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

HERE = Path(__file__).resolve().parent
STOCK = Path('G:/mc2-assets/out/scifi-kit/round2/stock')
ASSETS = HERE / 'assets'
INVENTORY = Path('E:/mc2-ref/scifi-kit/mapping/inventory.csv')
SCHEMA = 'mc2-kit-analysis/1'


# ------------------------------------------------------------------ stock io
def stock_models(name):
    """The asset's own mapped stock models from the catalog: intact LOD0 stem, destroyed stem, intact/destroyed stems list."""
    import counts
    try:
        obj = counts.catalog()[name]
    except Exception:
        return dict(intact=name, destroyed=None, source='no catalog entry; file name = id')
    stem = lambda m: Path(m['tgl']).stem.lower()  # noqa: E731
    intact = [stem(m) for m in obj['models'] if m['role'] == 'intact' and m.get('lod') == 0]
    dest = [stem(m) for m in obj['models'] if m['role'] == 'destroyed']
    return dict(intact=intact[0] if intact else name, destroyed=dest[0] if dest else None, source='catalog.json models[role=intact,lod=0]')


def load_stock(name):
    stem = stock_models(name)['intact']
    d = json.loads((STOCK / f'{stem}.json').read_text())
    V = np.array(d['vertices_m'], float).reshape(-1, 3, 3)
    UV = np.array(d['uv'], float).reshape(-1, 3, 2)
    mat = np.array(d['materials'], int)
    parts, at = [], 0
    for n in d.get('nodes', []):
        parts.append((n['name'], at, at + n['triangles']))
        at += n['triangles']
    if at != len(V):
        parts = [('all', 0, len(V))]
    return dict(raw=d, V=V, UV=UV, mat=mat, parts=parts, name=name, stem=stem)


def origin_offset(d, size):
    """Stock pivot relative to the authored (bbox-centred, z=0) frame (counts.ORIGINS)."""
    b = d['raw']['bounds']
    return [round((b['min'][0] + b['max'][0]) / 8, 6), round(-(b['min'][2] + b['max'][2]) / 8, 6), round(b['min'][1] / 4, 6)]


def _tris_normal(V):
    n = np.cross(V[:, 1] - V[:, 0], V[:, 2] - V[:, 0])
    ln = np.linalg.norm(n, axis=1)
    return n / np.maximum(ln, 1e-12)[:, None], ln / 2


def _texture(path):
    return np.array(Image.open(path).convert('RGBA')) if path and Path(path).is_file() else None


# ------------------------------------------------------------------ rasteriser (silhouettes, height fields)
def raster(tris2d, W, H, box, vals=None, alpha=None):
    """Orthographic raster of 2D triangles (n,3,2) into a WxH grid covering box=(u0,u1,v0,v1).
    Row 0 is the top (largest v). Edges are drawn too so edge-on sheets and wires still read.
    vals: (n,3) per-vertex scalar -> returns max-buffer too. alpha: (tex_rgba, uv(n,3,2)) alpha test."""
    u0, u1, v0, v1 = box
    sx, sy = W / (u1 - u0), H / (v1 - v0)
    mask = np.zeros((H, W), bool)
    zb = np.full((H, W), -1e9) if vals is not None else None
    P = np.empty_like(tris2d)
    P[..., 0] = (tris2d[..., 0] - u0) * sx
    P[..., 1] = (v1 - tris2d[..., 1]) * sy
    for i, t in enumerate(P):
        x0, x1 = int(max(0, math.floor(t[:, 0].min()))), int(min(W - 1, math.ceil(t[:, 0].max())))
        y0, y1 = int(max(0, math.floor(t[:, 1].min()))), int(min(H - 1, math.ceil(t[:, 1].max())))
        if x1 < 0 or y1 < 0 or x0 >= W or y0 >= H:
            continue
        (ax, ay), (bx, by), (cx, cy) = t
        det = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy)
        if abs(det) > 1e-9:
            gx, gy = np.meshgrid(np.arange(x0, x1 + 1) + .5, np.arange(y0, y1 + 1) + .5)
            l1 = ((by - cy) * (gx - cx) + (cx - bx) * (gy - cy)) / det
            l2 = ((cy - ay) * (gx - cx) + (ax - cx) * (gy - cy)) / det
            l3 = 1 - l1 - l2
            ins = (l1 >= -1e-6) & (l2 >= -1e-6) & (l3 >= -1e-6)
            if alpha is not None and ins.any():
                tex, uv = alpha
                th, tw = tex.shape[:2]
                uu = (l1 * uv[i, 0, 0] + l2 * uv[i, 1, 0] + l3 * uv[i, 2, 0]) % 1
                vv = (l1 * uv[i, 0, 1] + l2 * uv[i, 1, 1] + l3 * uv[i, 2, 1]) % 1
                ins &= tex[(vv * th).astype(int) % th, (uu * tw).astype(int) % tw, 3] > 128
            mask[y0:y1 + 1, x0:x1 + 1] |= ins
            if vals is not None:
                z = l1 * vals[i, 0] + l2 * vals[i, 1] + l3 * vals[i, 2]
                sub = zb[y0:y1 + 1, x0:x1 + 1]
                np.maximum(sub, np.where(ins, z, -1e9), out=sub)
        if alpha is None:
            for a, b in ((0, 1), (1, 2), (2, 0)):
                n = int(max(abs(t[b, 0] - t[a, 0]), abs(t[b, 1] - t[a, 1])) * 1.5) + 2
                xs = np.clip((t[a, 0] + (t[b, 0] - t[a, 0]) * np.linspace(0, 1, n)).astype(int), 0, W - 1)
                ys = np.clip((t[a, 1] + (t[b, 1] - t[a, 1]) * np.linspace(0, 1, n)).astype(int), 0, H - 1)
                mask[ys, xs] = True
    return (mask, zb) if vals is not None else mask


# camera table shared with blend.assembly_job / stock_views: (name, horizontal axis, sign, vertical axis)
VIEWS = (('front', 0, 1, 2), ('left', 1, -1, 2), ('rear', 0, -1, 2), ('right', 1, 1, 2), ('top', 0, 1, 1))


def project(V, view):
    """(n,3,3) -> (n,3,2) for one of the 5 views. front looks +Y (x right); left looks +X (-y right)..."""
    _, h, sign, v = view
    return np.stack([V[..., h] * sign, V[..., v]], -1)


def view_box(size, view, margin=.04):
    w, d, hgt = size
    _, h, sign, v = view
    ext = {0: w, 1: d, 2: hgt}
    he, ve = ext[h], ext[v]
    lo_v = 0.0 if v == 2 else -ve / 2
    hi_v = ve if v == 2 else ve / 2
    m = max(he, ve) * margin
    return (-he / 2 - m, he / 2 + m, lo_v - m, hi_v + m)


def grid_for(box, long_px=192):
    du, dv = box[1] - box[0], box[3] - box[2]
    s = long_px / max(du, dv)
    return max(8, int(round(du * s))), max(8, int(round(dv * s)))


def stock_masks(st, size, long_px=192):
    """Five silhouette masks of the stock mesh, alpha-tested through its own texture."""
    d = st['raw']
    tex = [_texture(p) for p in d['texture_paths']]
    V = st['V'] - np.array([(st['V'][..., i].min() + st['V'][..., i].max()) / 2 if i < 2 else st['V'][..., 2].min() for i in range(3)])
    out = {}
    for view in VIEWS:
        box = view_box(size, view)
        W, H = grid_for(box, long_px)
        m = np.zeros((H, W), bool)
        for mi in sorted(set(st['mat'].tolist())):
            sel = st['mat'] == mi
            usealpha = mi < len(d['texture_alpha']) and d['texture_alpha'][mi] and tex[mi] is not None
            m |= raster(project(V[sel], view), W, H, box, alpha=(tex[mi], st['UV'][sel]) if usealpha else None)
        out[view[0]] = (m, box)
    return out


# ------------------------------------------------------------------ geometry measurements
def _slice_extent(V, z, region=None):
    """Plan extents of the mesh cross-section at height z (triangle-edge intersections)."""
    pts = []
    for a, b in ((0, 1), (1, 2), (2, 0)):
        za, zb = V[:, a, 2], V[:, b, 2]
        cross = ((za - z) * (zb - z) <= 0) & (za != zb)
        t = (z - za[cross]) / (zb[cross] - za[cross])
        pts.append(V[cross, a, :2] + (V[cross, b, :2] - V[cross, a, :2]) * t[:, None])
    p = np.concatenate(pts) if pts else np.zeros((0, 2))
    if region is not None and len(p):
        p = p[(p[:, 1] >= region[0]) & (p[:, 1] <= region[1])]
    return p


def _dp(points, tol):
    """Douglas-Peucker on (t, v) polyline."""
    if len(points) < 3:
        return points
    a, b = np.array(points[0]), np.array(points[-1])
    best, at = 0, 0
    for i in range(1, len(points) - 1):
        p = np.array(points[i])
        t = (p[0] - a[0]) / max(b[0] - a[0], 1e-9)
        d = abs(p[1] - (a[1] + (b[1] - a[1]) * t))
        if d > best:
            best, at = d, i
    if best <= tol:
        return [points[0], points[-1]]
    return _dp(points[:at + 1], tol)[:-1] + _dp(points[at:], tol)


def profile(V, h, axis, region=None, levels=17):
    """Half-extent along `axis` (0=x,1=y) vs height fraction, simplified to knots."""
    raw = []
    for t in np.linspace(.02, .98, levels):
        p = _slice_extent(V, t * h, region)
        if len(p):
            raw.append((round(float(t), 3), float((p[:, axis].max() - p[:, axis].min()) / 2)))
    if not raw:
        return []
    top = max(v for _, v in raw)
    knots = _dp(raw, max(.03 * top, .02))
    knots[0] = (0.0, knots[0][1])
    knots[-1] = (1.0, knots[-1][1])
    return [[round(t, 3), round(v, 3)] for t, v in knots]


def round_kind(P):
    """'cylinder' / 'sphere' when the part's vertices sit on one radius (stock facets are a 1990s budget, not a design)."""
    pts = P.reshape(-1, 3)
    if len(np.unique(np.round(pts, 3), axis=0)) < 8:
        return None
    lo, hi = pts.min(0), pts.max(0)
    sz = hi - lo
    c = (lo + hi) / 2
    r = np.hypot(pts[:, 0] - c[0], pts[:, 1] - c[1])
    if sz[0] > .3 and .8 < sz[0] / max(sz[1], 1e-6) < 1.25:
        sel = r > .25 * r.max()
        if sel.sum() >= 6 and np.std(r[sel]) / max(np.mean(r[sel]), 1e-6) < .06 and sz[2] > .2 * sz[0]:
            return 'cylinder'
        r3 = np.linalg.norm(pts - c, axis=1)
        if np.std(r3) / max(np.mean(r3), 1e-6) < .08 and abs(sz[2] - sz[0]) < .3 * sz[0]:
            return 'sphere'
    return None


def roof_guess(V, size):
    """Flat / gable / mono / arch / hip(none) from the upward-facing height field."""
    w, d, h = size
    n, area = _tris_normal(V)
    up = np.abs(n[:, 2]) > .15      # stock winding is not reliable; the max-z buffer picks the top surface
    if not up.any() or h < .3:
        return dict(type='none', confidence=.3)
    N = 48
    box = (-w / 2, w / 2, -d / 2, d / 2)
    c = V - np.array([(V[..., 0].min() + V[..., 0].max()) / 2, (V[..., 1].min() + V[..., 1].max()) / 2, 0])
    xy = c[up][..., :2]
    zz = c[up][..., 2]
    _, zb = raster(xy, N, N, box, vals=zz)
    zb = np.where(zb < -1e8, np.nan, zb)
    res = {}
    for axis, name in ((1, 'x'), (0, 'y')):          # profile ACROSS the ridge axis
        with np.errstate(all='ignore'), __import__('warnings').catch_warnings():
            __import__('warnings').simplefilter('ignore')
            prof = np.nanmean(zb[int(N * .2):int(N * .8), :], axis=0) if axis == 1 else np.nanmean(zb[:, int(N * .2):int(N * .8)], axis=1)
        prof = prof[~np.isnan(prof)]
        if len(prof) < 8:
            continue
        s = np.linspace(-1, 1, len(prof))
        flat = float(np.std(prof))
        best = ('flat', flat)
        # gable: |s| ramp; arch: parabola/circle; mono: linear tilt
        A = np.stack([np.ones_like(s), np.abs(s)], 1)
        coef, *_ = np.linalg.lstsq(A, prof, rcond=None)
        gable = float(np.std(prof - A @ coef))
        B = np.stack([np.ones_like(s), s ** 2], 1)
        cb, *_ = np.linalg.lstsq(B, prof, rcond=None)
        arch = float(np.std(prof - B @ cb))
        C = np.stack([np.ones_like(s), s], 1)
        cc, *_ = np.linalg.lstsq(C, prof, rcond=None)
        mono = float(np.std(prof - C @ cc))
        rise = float(prof.max() - prof.min())
        cand = {'flat': flat, 'gable': gable * 1.15, 'arch': arch * 1.15, 'mono': mono * 1.15}
        kind = min(cand, key=cand.get)
        ext = (w if axis == 1 else d)
        pitch = math.degrees(math.atan2(abs(coef[1]), ext / 2)) if kind == 'gable' else math.degrees(math.atan2(abs(cc[1]), ext / 2)) if kind == 'mono' else 0.0
        res[name] = dict(type=kind, ridge_axis=name, eave_h=round(float(prof[[0, -1]].mean()), 3), ridge_h=round(float(prof.max()), 3),
                         rise_m=round(rise, 3), pitch_deg=round(pitch, 1), residual=round(cand[kind], 3), flat_residual=round(flat, 3))
    if not res:
        return dict(type='none', confidence=.3)
    pick = max(res.values(), key=lambda r: r['flat_residual'])
    flatness = pick['flat_residual']
    kind = pick['type'] if flatness > .08 * max(h, 1) else 'flat'
    conf = float(np.clip(1 - pick['residual'] / max(flatness, 1e-6), .2, .95)) if kind != 'flat' else .7
    out = dict(pick, type=kind, confidence=round(conf, 2))
    if kind == 'flat':
        out['pitch_deg'] = 0.0
    return out


# ------------------------------------------------------------------ palette
def _srgb2lin(a):
    a = np.asarray(a, float) / 255.0
    return np.where(a <= .04045, a / 12.92, ((a + .055) / 1.055) ** 2.4)


def lin2hex(c):
    s = np.where(np.asarray(c) <= .0031308, np.asarray(c) * 12.92, 1.055 * np.asarray(c) ** (1 / 2.4) - .055)
    return '#' + ''.join(f'{int(round(float(np.clip(x, 0, 1)) * 255)):02x}' for x in s)


def _used_px(st, sel):
    """RGB texels covered by the selected triangles' UVs (alpha textures: opaque texels only)."""
    d = st['raw']
    out = []
    for mi in sorted(set(st['mat'][sel].tolist())):
        tex = _texture(d['texture_paths'][mi]) if mi < len(d['texture_paths']) else None
        if tex is None:
            continue
        h, w = tex.shape[:2]
        m = Image.new('L', (w, h), 0)
        dr = ImageDraw.Draw(m)
        for uv in st['UV'][sel & (st['mat'] == mi)]:
            dr.polygon([((u % 1) * w if 0 < u < 1 else (u % 1) * w, (v % 1) * h) for u, v in uv], fill=255)
        mk = np.array(m) > 0
        px = tex[mk]
        if mi < len(d['texture_alpha']) and d['texture_alpha'][mi]:
            px = px[px[:, 3] > 128]
        px = px[~((px[:, 0] > 240) & (px[:, 2] > 240) & (px[:, 1] < 20))]
        out.append(px[:, :3])
    return np.concatenate(out) if out else np.zeros((0, 3), np.uint8)


def kmeans(lin, k=6, seed=3):
    rng = np.random.default_rng(seed)
    if len(lin) > 20000:
        lin = lin[rng.choice(len(lin), 20000, replace=False)]
    lum = lin @ np.array([.2126, .7152, .0722])
    cent = lin[np.argsort(lum)[np.linspace(0, len(lin) - 1, k).astype(int)]].copy()
    for _ in range(14):
        lab = np.argmin(((lin[:, None] - cent[None]) ** 2).sum(2), 1)
        for j in range(k):
            if (lab == j).any():
                cent[j] = lin[lab == j].mean(0)
    lab = np.argmin(((lin[:, None] - cent[None]) ** 2).sum(2), 1)
    frac = np.bincount(lab, minlength=k) / len(lab)
    order = np.argsort(-frac)
    return [(cent[j], float(frac[j])) for j in order if frac[j] > .005]


def palette_info(st):
    from wear_metrics import metrics  # grime metrics on UV-used texels, the same ones the bake is judged on
    n, _ = _tris_normal(st['V'])
    roofsel = (np.abs(n[:, 2]) > .5) & (st['V'][..., 2].mean(1) > .3 * st['V'][..., 2].max())
    allsel = np.ones(len(n), bool)
    px = _used_px(st, allsel)
    if not len(px):
        return None
    wall = _used_px(st, ~roofsel) if (~roofsel).any() else px
    rf = _used_px(st, roofsel) if roofsel.any() else wall
    cl = kmeans(_srgb2lin(wall))
    rcl = kmeans(_srgb2lin(rf), k=3)
    lum = lambda c: float(np.asarray(c) @ np.array([.2126, .7152, .0722]))  # noqa: E731
    sat = lambda c: float((max(c) - min(c)) / max(max(c), 1e-6))  # noqa: E731
    main = cl[0][0]
    rest = [c for c, f in cl[1:] if f > .03 and abs(lum(c) - lum(main)) > .02] or [main * .65]
    trim_c = min([c for c, f in cl if f > .02], key=lum)
    accent = max([c for c, f in cl if f > .015], key=lambda c: sat(c) * (1 + lum(c)))
    metal = max([c for c, f in cl if sat(c) < .25] or [np.array([.3, .3, .3])], key=lum)
    draft = dict(main=main, second=rest[0], trim=trim_c, roof=rcl[0][0], ferro=main * .95, accent=accent, metal=metal, corr=main, glass=np.array([.03, .05, .05]))
    mean = _srgb2lin(px).mean(0)
    return dict(
        dominant=[dict(linear=[round(float(x), 4) for x in c], hex=lin2hex(c), fraction=round(f, 3)) for c, f in cl],
        roof_dominant=[dict(linear=[round(float(x), 4) for x in c], hex=lin2hex(c), fraction=round(f, 3)) for c, f in rcl],
        mean_linear=[round(float(x), 4) for x in mean], mean_hex=lin2hex(mean),
        wear=metrics(px), texels=int(len(px)),
        draft={k2: [round(float(x), 3) for x in v] for k2, v in draft.items()})


# ------------------------------------------------------------------ draft feature counts
def _connected(V, parts):
    """Disconnected solids inside each node (union-find on welded vertices)."""
    comps = []
    for name, a, b in parts:
        tr = V[a:b]
        key = np.round(tr.reshape(-1, 3), 3)
        ids = {}
        parent = []

        def find(i):
            while parent[i] != i:
                parent[i] = parent[parent[i]]
                i = parent[i]
            return i
        vid = []
        for p in map(tuple, key):
            if p not in ids:
                ids[p] = len(parent)
                parent.append(len(parent))
            vid.append(ids[p])
        vid = np.array(vid).reshape(-1, 3)
        for t in vid:
            ra, rb, rc = find(t[0]), find(t[1]), find(t[2])
            parent[rb] = ra
            parent[find(rc)] = find(ra)
        groups = {}
        for i, t in enumerate(vid):
            groups.setdefault(find(t[0]), []).append(a + i)
        for g in groups.values():
            comps.append((name, g))
    return comps


def _blobs(lum, thr):
    from scipy import ndimage
    lab, n = ndimage.label(lum < thr)
    out = []
    for i, sl in enumerate(ndimage.find_objects(lab), 1):
        area = int((lab[sl] == i).sum())
        out.append((sl, area))
    return out


def face_features(st, size):
    """Doors / windows / bays from dark, rectangular texture regions on each wall plane (metric 0.2 m grid)."""
    V, UV, n, area = st['V'], st['UV'], *_tris_normal(st['V'])
    d = st['raw']
    tex = [_texture(p) for p in d['texture_paths']]
    cen = (V[..., :2].min((0, 1)) + V[..., :2].max((0, 1))) / 2
    vert = np.abs(n[:, 2]) < .35
    feats = {'doors': [], 'windows': [], 'openings_bays': []}
    keys = {}
    for i in np.where(vert)[0]:
        nn = n[i, :2] / max(np.linalg.norm(n[i, :2]), 1e-9)
        if nn @ (V[i, :, :2].mean(0) - cen) < 0 and np.linalg.norm(V[i, :, :2].mean(0) - cen) > .5:
            nn = -nn          # orient outward by position (winding is not reliable)
        if abs(nn[0]) > abs(nn[1]):
            side = 'right' if nn[0] > 0 else 'left'
            off = float(V[i, :, 0].mean())
        else:
            side = 'rear' if nn[1] > 0 else 'front'
            off = float(V[i, :, 1].mean())
        keys.setdefault((side, round(off / .5)), []).append(i)
    for (side, _), idx in keys.items():
        idx = np.array(idx)
        if area[idx].sum() < 3:
            continue
        mi = int(st['mat'][idx[0]])
        im = tex[mi] if mi < len(tex) else None
        if im is None or (mi < len(d['texture_alpha']) and d['texture_alpha'][mi]):
            continue
        ax = 0 if side in ('front', 'rear') else 1       # tangent axis
        flip = -1.0 if side in ('rear', 'left') else 1.0
        P = V[idx].reshape(-1, 3)
        T = flip * P[:, ax]
        Z = P[:, 2]
        uv = UV[idx].reshape(-1, 2)
        A = np.stack([T, Z, np.ones_like(T)], 1)
        try:
            cu, *_ = np.linalg.lstsq(A, uv[:, 0], rcond=None)
            cv, *_ = np.linalg.lstsq(A, uv[:, 1], rcond=None)
        except np.linalg.LinAlgError:
            continue
        t0, t1, z0, z1 = T.min(), T.max(), Z.min(), Z.max()
        if t1 - t0 < 1.0 or z1 - z0 < 1.0:
            continue
        cell = .2
        ts = np.arange(t0 + cell / 2, t1, cell)
        zs = np.arange(z0 + cell / 2, z1, cell)
        TT, ZZ = np.meshgrid(ts, zs)
        u = (cu[0] * TT + cu[1] * ZZ + cu[2]) % 1
        v = (cv[0] * TT + cv[1] * ZZ + cv[2]) % 1
        h, w = im.shape[:2]
        px = im[(v * h).astype(int) % h, (u * w).astype(int) % w, :3].astype(float) / 255
        lum = px @ np.array([.2126, .7152, .0722])
        med = np.median(lum)
        if med < .08:
            continue
        for sl, ar in _blobs(lum, .55 * med):
            hh, ww = (sl[0].stop - sl[0].start) * cell, (sl[1].stop - sl[1].start) * cell
            fill = ar / max((sl[0].stop - sl[0].start) * (sl[1].stop - sl[1].start), 1)
            zb = z0 + sl[0].start * cell
            tc = t0 + (sl[1].start + sl[1].stop) / 2 * cell
            if fill < .55 or hh < .6 or ww < .4:
                continue
            kind = None
            if zb - z0 < .7 and 1.6 <= hh <= 5.5 and .8 <= ww <= 7:
                kind = 'openings_bays' if (ww > 3.2 and hh > 3.0) else 'doors'
            elif zb - z0 >= .7 and .4 <= ww <= 4.5 and .4 <= hh <= 3.2:
                kind = 'windows'
            if kind:
                feats[kind].append(dict(side=side, at_m=[round(float(tc), 1), round(float(zb + hh / 2), 1)], size_m=[round(float(ww), 1), round(float(hh), 1)], conf=round(float(.45 + .4 * min(1, fill)), 2),
                                        u_frac=round(float(tc / max((t1 - t0) / 2, 1e-6)), 2), source='stock texture (painted) region on this face'))
    # the same opening seen on coplanar duplicate layers counts once
    for k, lst in feats.items():
        seen, keep = set(), []
        for f in lst:
            key = (f['side'], round(f['at_m'][0] / .8), round(f['at_m'][1] / .8))
            if key not in seen:
                seen.add(key)
                keep.append(f)
        feats[k] = keep
    return feats


def part_features(st, size):
    """Posts / barrels / tanks / crates / spans from separate solids and face breaks."""
    V = st['V']
    w, d, h = size
    comps = _connected(V, st['parts'])
    out = {'legs_supports': [], 'barrels': [], 'tanks': [], 'crates': []}
    big = max(w, d)
    for name, idx in comps:
        P = V[idx].reshape(-1, 3)
        lo, hi = P.min(0), P.max(0)
        sz = hi - lo
        if len(comps) == 1 or max(sz) > .8 * max(w, d, h):
            continue
        cx = (lo + hi) / 2
        if sz[2] > 2.2 * max(sz[0], sz[1]) and max(sz[0], sz[1]) < max(1.6, .08 * big):
            out['legs_supports'].append(dict(at_m=[round(float(cx[0]), 1), round(float(cx[1]), 1)], height_m=round(float(sz[2]), 1), conf=.55))
        elif .75 < sz[0] / max(sz[1], 1e-6) < 1.33 and len(idx) >= 8:
            r = np.hypot(P[:, 0] - cx[0], P[:, 1] - cx[1])
            mid = (P[:, 2] > lo[2] + .2 * sz[2]) & (P[:, 2] < hi[2] - .2 * sz[2])
            roundish = mid.any() and float(np.std(r[mid]) / max(np.mean(r[mid]), 1e-6)) < .12
            if roundish and sz[0] < 1.6 and sz[2] < 2.6:
                out['barrels'].append(dict(at_m=[round(float(cx[0]), 1), round(float(cx[1]), 1)], conf=.5))
            elif roundish and sz[0] >= 1.6:
                out['tanks'].append(dict(at_m=[round(float(cx[0]), 1), round(float(cx[1]), 1)], conf=.5))
            elif len(idx) <= 24 and .8 < sz[0] < 7 and sz[2] > .6:
                out['crates'].append(dict(at_m=[round(float(cx[0]), 1), round(float(cx[1]), 1)], conf=.4))
        elif len(idx) <= 24 and .8 < max(sz[0], sz[1]) < 7 and 0.6 < sz[2] < 5 and min(sz[0], sz[1]) > .6:
            out['crates'].append(dict(at_m=[round(float(cx[0]), 1), round(float(cx[1]), 1)], conf=.4))
    return out


def span_count(V, size):
    """Panels/sections along the long axis: distinct breaks on the largest wall plane."""
    w, d, h = size
    ax = 1 if d >= w else 0
    n, area = _tris_normal(V)
    side = np.abs(n[:, 1 - ax]) > .8
    if not side.any():
        return 0, 0.2
    ys = np.round(V[side][..., ax].ravel() / .25) * .25
    pts = np.unique(ys)
    gaps = [b - a for a, b in zip(pts, pts[1:]) if b - a > .15 * max(w, d, 1) / 4]
    return max(1, len(gaps)), .3


def counts_draft(st, size, roof):
    feats = face_features(st, size)
    feats.update(part_features(st, size))
    spans, sconf = span_count(st['V'], size)
    res = {}
    for key in ('doors', 'windows', 'openings_bays', 'panels_sections', 'crates', 'barrels', 'tanks', 'arches_ribs', 'legs_supports', 'antennas_dishes', 'stairs_ladders'):
        items = feats.get(key, [])
        if key == 'panels_sections':
            res[key] = dict(n=spans, conf=sconf, where=[])
        elif key == 'arches_ribs':
            res[key] = dict(n=1 if roof.get('type') == 'arch' else 0, conf=.4 if roof.get('type') == 'arch' else .5, where=['barrel vault'] if roof.get('type') == 'arch' else [])
        elif key in ('antennas_dishes', 'stairs_ladders'):
            res[key] = dict(n=0, conf=.3, where=[])
        else:
            conf = round(sum(i['conf'] for i in items) / len(items), 2) if items else .5
            res[key] = dict(n=len(items), conf=conf, where=items)
    return res


# ------------------------------------------------------------------ top level
def analyze(name):
    st = load_stock(name)
    V = st['V']
    lo, hi = V.reshape(-1, 3).min(0), V.reshape(-1, 3).max(0)
    size = [round(float(x), 5) for x in (hi - lo)]
    Vc = V - np.array([(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, lo[2]])
    w, d, h = size
    res = dict(schema=SCHEMA, asset=name, source=str(STOCK / f'{name}.json'),
               bounds=dict(size_m=size, footprint_m2=round(w * d, 2), height_m=round(h, 3), tris=int(len(V)),
                           origin_offset_m=origin_offset(st, size)))
    long_ax = 1 if d >= w else 0
    near_end = [-d / 2, -d / 2 + .4 * d] if long_ax == 1 else None
    parts = []
    for pname, a, b in st['parts']:
        P = Vc[a:b]
        if not len(P):
            continue
        plo, phi = P.reshape(-1, 3).min(0), P.reshape(-1, 3).max(0)
        parts.append(dict(name=pname, tris=b - a, role='base' if 'base' in pname.lower() else 'head' if pname.lower() in ('turret', 'head', 'gun') else 'body',
                          round=round_kind(P), bbox_lo=[round(float(x), 3) for x in plo], bbox_hi=[round(float(x), 3) for x in phi],
                          profile_x=profile(P, h, 0), profile_y=profile(P, h, 1)))
    res['parts'] = parts
    sm = stock_models(name)
    res['stock_model'] = dict(asset=name, intact_stem=sm['intact'], destroyed_stem=sm['destroyed'], mesh=str(STOCK / f"{sm['intact']}.json"),
                              parts=[(q['name'], q['role']) for q in parts], resolved_from=sm['source'])
    res['round_parts'] = [q['name'] for q in parts if q['round']]
    res['profile'] = dict(x_all=profile(Vc, h, 0), y_all=profile(Vc, h, 1), x_body=profile(Vc, h, 0, near_end) if near_end else profile(Vc, h, 0),
                          long_axis='y' if long_ax == 1 else 'x', note='half-extent (m) vs height fraction; x_body = the end of the long axis away from the +Y terminal pier')
    res['roof'] = roof_guess(Vc, size)
    res['palette'] = palette_info(st)
    res['counts'] = counts_draft(dict(st, V=Vc), size, res['roof'])
    return res


def inventory_row(name):
    import csv
    for r in csv.DictReader(INVENTORY.open(encoding='utf-8-sig')):
        if r['id'] == name:
            return r
    raise KeyError(name)


def write(name):
    ASSETS.mkdir(exist_ok=True)
    a = analyze(name)
    row = inventory_row(name)
    import archetypes
    a['archetype_guess'] = archetypes.classify(row, a)
    path = ASSETS / f'{name}.analysis.json'
    path.write_text(json.dumps(a, indent=1))
    return path, a


if __name__ == '__main__':
    import sys
    for n in sys.argv[1:]:
        p, a = write(n)
        print(p, a['archetype_guess'], {k: v['n'] for k, v in a['counts'].items() if v['n']})
