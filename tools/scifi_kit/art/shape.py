"""Gross shape of a triangle soup, measured the same way for the stock mesh and for a remake.

shape_info(V, size) -> dict(footprint, roof, wall_profile, features):
  footprint     plan outline from the top-view height field: rect | rectilinear (L/T/U..., cells + boundary edges) |
                polygon (convex n-gon: hex, oct...) | circle | irregular
  roof          the best of three height-field models, by inlier fraction: flat (one height), extrude (a cross-section
                profile swept along the ridge axis: gable, mono, sawtooth, stepped/clerestory, A-frame) or inset (height as a
                function of the distance to the plan outline: hip, pyramid, dome). Knots are what the builder sweeps.
  wall_profile  plan scale factor per axis vs height below the eave (batter, skirts, overhangs), 1.0 = the footprint extent
  features      residual blobs above the roof model (vents, fans, domes, tanks, towers) and outside the plan outline
                (pipes, annexes), with plan position, size, base, shoulder and peak heights
The builder (shell.py) consumes this dict; the validator re-measures the remake with the same function and compares.
Pure numpy/scipy/skimage; never imported by Blender.
"""
import math

import numpy as np
from scipy import ndimage

SCHEMA = 'mc2-kit-shape/1'
GRID = 96


# ------------------------------------------------------------------ height field
def heightfield(V, size, n=GRID):
    """Max-z height field of the soup over the plan bounds. -> (mask, H, cell, (nx, ny)); row 0 = +y edge."""
    import analyze
    w, d, h = size
    cell = max(w, d) / n
    nx, ny = max(8, int(round(w / cell))), max(8, int(round(d / cell)))
    box = (-w / 2, w / 2, -d / 2, d / 2)
    mask, zb = analyze.raster(V[..., :2], nx, ny, box, vals=V[..., 2])
    valid = zb > -1e8
    if not valid.any():
        return mask, np.zeros_like(zb), (w / nx, d / ny), (nx, ny)
    # edge-only cells (vertical walls seen edge-on) take the nearest surface height
    idx = ndimage.distance_transform_edt(~valid, return_distances=False, return_indices=True)
    H = zb[idx[0], idx[1]]
    mask = ndimage.binary_fill_holes(ndimage.binary_closing(np.pad(mask | valid, 2), iterations=1))[2:-2, 2:-2]
    H = np.where(mask, np.maximum(H, 0.0), 0.0)
    return mask, H, (w / nx, d / ny), (nx, ny)


def _xy(r, c, cell, size):
    w, d, _ = size
    return -w / 2 + (np.asarray(c) + .5) * cell[0], d / 2 - (np.asarray(r) + .5) * cell[1]


# ------------------------------------------------------------------ footprint
def _poly_mask(poly, cell, size, shape):
    from skimage.draw import polygon as skpoly
    w, d, _ = size
    m = np.zeros(shape, bool)
    xs = [(x + w / 2) / cell[0] - .5 for x, _ in poly]
    ys = [(d / 2 - y) / cell[1] - .5 for _, y in poly]
    rr, cc = skpoly(ys, xs, shape)
    m[rr, cc] = True
    return m


def _breaks(occ, min_gap):
    """Indices where consecutive occupancy columns differ (plan steps), merged closer than min_gap."""
    n = occ.shape[1]
    score = np.array([0.0] + [float(np.mean(occ[:, j] != occ[:, j - 1])) for j in range(1, n)])
    cand = [j for j in range(1, n) if score[j] > .06]
    out = []
    for j in sorted(cand, key=lambda j: -score[j]):
        if all(abs(j - q) >= min_gap for q in out) and min_gap <= j <= n - min_gap:
            out.append(j)
    return [0] + sorted(out) + [n]


def _ccw_edges(cells_in, xs, ys):
    """Boundary edges (CCW, outward on the right) of a union of grid cells; collinear runs merged."""
    nyc, nxc = cells_in.shape
    occ = lambda i, j: 0 <= i < nyc and 0 <= j < nxc and cells_in[i, j]  # noqa: E731
    segs = []
    for i in range(nyc):          # row i spans ys[i] (low) .. ys[i+1]
        for j in range(nxc):
            if not cells_in[i, j]:
                continue
            x0, x1, y0, y1 = xs[j], xs[j + 1], ys[i], ys[i + 1]
            if not occ(i - 1, j):
                segs.append(((x0, y0), (x1, y0)))
            if not occ(i, j + 1):
                segs.append(((x1, y0), (x1, y1)))
            if not occ(i + 1, j):
                segs.append(((x1, y1), (x0, y1)))
            if not occ(i, j - 1):
                segs.append(((x0, y1), (x0, y0)))
    merged = True
    while merged:
        merged = False
        for a in range(len(segs)):
            for b in range(len(segs)):
                if a != b and segs[a][1] == segs[b][0]:
                    da = np.subtract(segs[a][1], segs[a][0])
                    db = np.subtract(segs[b][1], segs[b][0])
                    if abs(da[0] * db[1] - da[1] * db[0]) < 1e-9 and np.dot(da, db) > 0:
                        segs[a] = (segs[a][0], segs[b][1])
                        segs.pop(b)
                        merged = True
                        break
            if merged:
                break
    return [[round(float(a[0]), 3), round(float(a[1]), 3), round(float(b[0]), 3), round(float(b[1]), 3)] for a, b in segs]


def _rects(cells_in, xs, ys):
    """Greedy merge of grid cells into rectangles [x0, y0, x1, y1]."""
    left = cells_in.copy()
    out = []
    nyc, nxc = left.shape
    for i in range(nyc):
        j = 0
        while j < nxc:
            if not left[i, j]:
                j += 1
                continue
            j1 = j
            while j1 + 1 < nxc and left[i, j1 + 1]:
                j1 += 1
            i1 = i
            while i1 + 1 < nyc and left[i1 + 1, j:j1 + 1].all():
                i1 += 1
            left[i:i1 + 1, j:j1 + 1] = False
            out.append([round(float(xs[j]), 3), round(float(ys[i]), 3), round(float(xs[j1 + 1]), 3), round(float(ys[i1 + 1]), 3)])
            j = j1 + 1
    return out


def _ngon(n, cx, cy, ax, ay, phase):
    return [(cx + ax * math.cos(phase + math.tau * i / n), cy + ay * math.sin(phase + math.tau * i / n)) for i in range(n)]


def _convex_fit(mask, cell, size):
    """Best axis-symmetric n-gon (n = 5..8, either phase) or ellipse for the plan mask, by IoU over a small parameter grid.
    Protrusions (an attached tower, a pipe) stay outside the fitted outline and become side features. -> dict or None."""
    w, d, _ = size
    best = {}
    for n in (5, 6, 7, 8, 32):
        for phase in ((0.0, math.pi / n) if n < 32 else (0.0,)):
            unit = _ngon(n, 0, 0, 1, 1, phase)
            ex, ey = max(abs(x) for x, _ in unit), max(abs(y) for _, y in unit)
            for fx in (.84, .92, 1.0):
                for fy in (.84, .92, 1.0):
                    for ox in (-.04, 0.0, .04):
                        for oy in (-.04, 0.0, .04):
                            cx, cy = ox * w, oy * d
                            ax, ay = (w / 2) * fx / ex, (d / 2) * fy / ey
                            poly = _ngon(n, cx, cy, ax, ay, phase)
                            pm = _poly_mask(poly, cell, size, mask.shape)
                            iou = float((pm & mask).sum() / max((pm | mask).sum(), 1))
                            if iou > best.get(n, (0,))[0]:
                                best[n] = (iou, poly, (cx, cy, ax, ay))
    if not best:
        return None
    circ = best.pop(32)
    n_best = max(best, key=lambda n: (best[n][0], -n))
    piou, poly, _ = best[n_best]
    if max(piou, circ[0]) < .9:
        return None
    if circ[0] >= piou - .01 or n_best >= 5:
        # owner rule (2026-10-23): a 5+ sided stock outline is the era's low-poly circle, rebuilt round (not a hex/oct prism)
        cx, cy, ax, ay = circ[2]
        return dict(kind='circle', n=0, facets=0 if circ[0] >= piou - .01 else n_best, center=[round(cx, 3), round(cy, 3)], R=round((ax + ay) / 2, 3), radii=[round(ax, 3), round(ay, 3)], iou=round(circ[0], 3),
                    poly=[[round(x, 3), round(y, 3)] for x, y in circ[1]], edges=[])
    poly = [[round(x, 3), round(y, 3)] for x, y in poly]
    edges = [[poly[i][0], poly[i][1], poly[(i + 1) % len(poly)][0], poly[(i + 1) % len(poly)][1]] for i in range(len(poly))]
    return dict(kind='polygon', n=n_best, poly=poly, edges=edges, iou=round(piou, 3))


def footprint(mask, cell, size):
    w, d, _ = size
    ny, nx = mask.shape
    area = mask.sum()
    fill = float(area / mask.size)
    rect = [[-w / 2, -d / 2], [w / 2, -d / 2], [w / 2, d / 2], [-w / 2, d / 2]]
    edges_of = lambda poly: [[round(poly[i][0], 3), round(poly[i][1], 3), round(poly[(i + 1) % len(poly)][0], 3), round(poly[(i + 1) % len(poly)][1], 3)] for i in range(len(poly))]  # noqa: E731
    if fill >= .92:
        return dict(kind='rect', n=4, poly=rect, edges=edges_of(rect), cells=[[-w / 2, -d / 2, w / 2, d / 2]], fill=round(fill, 3))
    fit = _convex_fit(mask, cell, size)
    if fit:
        return dict(fit, fill=round(fill, 3))
    # rectilinear: plan steps on both axes, cells kept where the mask fills them
    mg = max(3, int(.06 * max(nx, ny)))
    bx = _breaks(mask, mg)
    by = _breaks(mask.T, mg)
    xs = [-w / 2 + b * cell[0] for b in bx]
    ys = [d / 2 - b * cell[1] for b in by][::-1]           # ascending y
    rows = list(zip(by[:-1], by[1:]))[::-1]
    cin = np.array([[mask[r0:r1, c0:c1].mean() > .5 for c0, c1 in zip(bx[:-1], bx[1:])] for r0, r1 in rows], bool)
    cm = np.zeros_like(mask)
    for i, (r0, r1) in enumerate(rows):
        for j, (c0, c1) in enumerate(zip(bx[:-1], bx[1:])):
            cm[r0:r1, c0:c1] = cin[i, j]
    iou = float((cm & mask).sum() / max((cm | mask).sum(), 1))
    edges = _ccw_edges(cin, xs, ys)
    if iou >= .88 and cin.sum() <= 16:
        return dict(kind='rectilinear', n=len(edges), edges=edges, cells=_rects(cin, xs, ys), fill=round(fill, 3), iou=round(iou, 3))
    return dict(kind='irregular', n=0, edges=[], fill=round(fill, 3), iou=round(iou, 3))


def plan_mask(fp, cell, size, shape):
    """The footprint's own raster (outline only, without the features outside it)."""
    if fp['kind'] in ('rect', 'rectilinear'):
        m = np.zeros(shape, bool)
        for x0, y0, x1, y1 in fp['cells']:
            m |= _poly_mask([(x0, y0), (x1, y0), (x1, y1), (x0, y1)], cell, size, shape)
        return m
    if fp['kind'] in ('polygon', 'circle'):
        return _poly_mask(fp['poly'], cell, size, shape)
    return None


# ------------------------------------------------------------------ roof models
def _dp(pts, tol):
    if len(pts) < 3:
        return list(pts)
    a, b = np.array(pts[0]), np.array(pts[-1])
    ab = b - a
    L = np.linalg.norm(ab) or 1e-9
    dist = [abs(ab[0] * (p[1] - a[1]) - ab[1] * (p[0] - a[0])) / L for p in pts[1:-1]]   # 2-D cross (numpy 2.5 dropped 2-D np.cross)
    i = int(np.argmax(dist)) + 1
    if dist[i - 1] <= tol:
        return [pts[0], pts[-1]]
    return _dp(pts[:i + 1], tol)[:-1] + _dp(pts[i:], tol)


def _peaks(kn, prom):
    from scipy.signal import find_peaks
    c = np.linspace(kn[0][0], kn[-1][0], 240)
    z = np.interp(c, [a for a, _ in kn], [b for _, b in kn])
    z = np.concatenate([[z.min() - 10 * prom], z, [z.min() - 10 * prom]])
    return int(len(find_peaks(z, prominence=prom, plateau_size=(1, None))[0]))


def roof(mask, H, cell, size, fp, force=None):
    w, d, h = size
    m = plan_mask(fp, cell, size, mask.shape) if fp['kind'] != 'irregular' else mask
    m = m & mask if m is not None else mask
    if m.sum() < 8:
        m = mask
    tol = max(.25, .03 * h)
    vals = H[m]
    cands = {}
    c = float(np.median(vals))
    cands['flat'] = (float(np.mean(np.abs(vals - c) < tol)), None)
    for ax in ('y', 'x'):           # extrusion (ridge) axis; the profile runs across it
        Hm = np.where(m, H, np.nan)
        with np.errstate(all='ignore'), __import__('warnings').catch_warnings():
            __import__('warnings').simplefilter('ignore')
            prof = np.nanmedian(Hm, axis=0) if ax == 'y' else np.nanmedian(Hm, axis=1)
        if np.isnan(prof).all():
            continue
        prof = np.where(np.isnan(prof), np.nanmin(prof), prof)
        model = np.broadcast_to(prof[None, :], H.shape) if ax == 'y' else np.broadcast_to(prof[:, None], H.shape)
        cands['extrude_' + ax] = (float(np.mean(np.abs(H[m] - model[m]) < tol)), prof)
    dist = np.maximum(ndimage.distance_transform_edt(np.pad(m, 1))[1:-1, 1:-1] - .5, 0) * min(cell)
    dmax = float(dist[m].max()) if m.any() else 1.0
    nb = max(4, int(dmax / min(cell)))
    bins = np.minimum((dist / max(dmax, 1e-6) * nb).astype(int), nb - 1)
    f = np.array([np.median(H[m & (bins == b)]) if (m & (bins == b)).any() else np.nan for b in range(nb)])
    ok = ~np.isnan(f)
    f = np.interp(np.arange(nb), np.where(ok)[0], f[ok]) if ok.any() else np.zeros(nb)
    cands['inset'] = (float(np.mean(np.abs(H[m] - f[bins[m]]) < tol)), f)
    best = max(v[0] for v in cands.values())
    if force in cands:            # re-measuring a remake: the stock's model family, so residual features are comparable
        kind = force
    elif cands['flat'][0] >= best - .04:
        kind = 'flat'
    else:
        ext = max((k for k in cands if k.startswith('extrude')), key=lambda k: cands[k][0], default=None)
        kind = ext if ext and cands[ext][0] >= cands['inset'][0] - .02 else 'inset'
    out = dict(inliers={k: round(v[0], 3) for k, v in cands.items()}, tol=round(tol, 3), fit=kind)
    if kind == 'flat':
        ring = m & (dist < 2.5 * min(cell))
        rim = float(np.percentile(H[ring], 75)) if ring.any() else c
        out.update(model='flat', form='flat', ridges=0, eave=round(c, 3), top=round(c, 3), rim=round(max(rim, c), 3), profile=[])
        model = np.full(H.shape, c)
    elif kind.startswith('extrude'):
        ax = kind[-1]
        prof = cands[kind][1]
        span = w if ax == 'y' else d
        n = len(prof)
        cs = [-span / 2 + span * (i + .5) / n for i in range(n)] if ax == 'y' else [span / 2 - span * (i + .5) / n for i in range(n)]
        pts = sorted(zip(cs, prof.tolist()))
        pts = [(-span / 2, pts[0][1])] + pts + [(span / 2, pts[-1][1])]
        kn = _dp(pts, max(.12, .025 * h))
        zs = [z for _, z in kn]
        ridges = _peaks(kn, max(tol, .08 * (max(zs) - min(zs))))
        imax = int(np.argmax(zs))
        steps = sum(1 for (c0, z0), (c1, z1) in zip(kn, kn[1:]) if abs(z1 - z0) > 4 * abs(c1 - c0) and abs(z1 - z0) > tol)
        if max(zs) - min(zs) < tol:
            form = 'flat'
        elif steps:
            form = 'stepped'
        elif ridges >= 2:
            form = 'sawtooth'
        elif imax in (0, len(zs) - 1) or zs[0] > max(zs) - tol or zs[-1] > max(zs) - tol:
            form = 'mono'
        else:
            form = 'gable'
        out.update(model='extrude', axis=ax, form=form, ridges=ridges, eave=round(min(zs[0], zs[-1]), 3), top=round(max(zs), 3),
                   profile=[[round(float(a), 3), round(float(b), 3)] for a, b in kn])
        # end walls standing above the swept profile (a recessed roof behind parapet ends): the max over the outer 8% of the run
        Hm = np.where(m, H, np.nan)
        nrun = Hm.shape[0] if ax == 'y' else Hm.shape[1]
        q = max(2, int(.08 * nrun))
        with np.errstate(all='ignore'), __import__('warnings').catch_warnings():
            __import__('warnings').simplefilter('ignore')
            ends = [Hm[:q], Hm[-q:]] if ax == 'y' else [Hm[:, :q], Hm[:, -q:]]
            endp = np.nanmin([np.nanmax(e, axis=0 if ax == 'y' else 1) for e in ends], axis=0)
        endp = np.where(np.isnan(endp), prof, endp)
        if np.any(endp - prof > tol) and np.mean(endp - prof > tol) > .1:
            pe = sorted(zip(cs, endp.tolist()))
            pe = [(-span / 2, pe[0][1])] + pe + [(span / 2, pe[-1][1])]
            out['end_profile'] = [[round(float(a), 3), round(float(b), 3)] for a, b in _dp(pe, max(.12, .025 * h))]
        model = cands[kind][1][None, :] if ax == 'y' else cands[kind][1][:, None]
        model = np.broadcast_to(model, H.shape)
    else:
        f = cands['inset'][1]
        pts = [(0.0, float(f[0]))] + [((b + .5) / nb * dmax, float(f[b])) for b in range(nb)]
        kn = _dp(pts, max(.12, .025 * h))
        # leading run steeper than 45 degrees near the outline = skirt and (battered) walls seen from above; the roof starts after it
        e = 0
        for i in range(len(kn) - 1):
            run_, rise_ = kn[i + 1][0] - kn[i][0], kn[i + 1][1] - kn[i][1]
            if kn[i][0] < .45 * dmax and rise_ > max(run_, 1e-6):
                e = i + 1
            elif kn[i][0] < .2 * dmax and abs(rise_) < tol:
                continue
            else:
                break
        kn = kn[e:] if e < len(kn) - 1 else kn[-2:]
        zs = [z for _, z in kn]
        form = dict(rect='hip', rectilinear='hip', polygon='pyramid', circle='dome').get(fp['kind'], 'hip')
        if max(zs) - zs[0] < tol:          # battered walls under a flat roof
            out.update(model='flat', form='flat', ridges=0, eave=round(float(np.median(zs)), 3), top=round(max(zs), 3), rim=round(max(zs), 3), profile=[],
                       batter_s=round(kn[0][0] / dmax, 3))
            return out, np.where(m, f[bins], 0.0), m
        out.update(model='inset', form=form, ridges=1, eave=round(zs[0], 3), top=round(max(zs), 3), inradius=round(dmax, 3),
                   profile=[[round(float(a) / dmax, 3), round(float(b), 3)] for a, b in kn])
        model = f[bins]
    return out, np.where(m, model, 0.0), m


# ------------------------------------------------------------------ features (residual blobs)
def _blob(sel, H, base, cell, size, where):
    b0 = float(np.median(base[sel])) if where == 'roof' else 0.0
    top0 = float(H[sel].max())
    core = sel & (H >= b0 + .5 * (top0 - b0))
    if core.sum() >= 4:
        sel = core
    r, c = np.where(sel)
    xs, ys = _xy(r, c, cell, size)
    x0, x1 = float(xs.min() - cell[0] / 2), float(xs.max() + cell[0] / 2)
    y0, y1 = float(ys.min() - cell[1] / 2), float(ys.max() + cell[1] / 2)
    sx, sy = x1 - x0, y1 - y0
    fill = float(sel.sum() * cell[0] * cell[1] / max(sx * sy, 1e-9))
    top = float(H[sel].max())
    shoulder = float(np.percentile(H[sel], 50))
    b = float(np.median(base[sel])) if where == 'roof' else 0.0
    rr, cc = r - r.min(), c - c.min()
    nr, nc = rr.max() + 1, cc.max() + 1
    q = max(1, int(round(.18 * min(nr, nc))))
    cfill = float((((rr < q) | (rr >= nr - q)) & ((cc < q) | (cc >= nc - q))).sum() / max(4 * q * q, 1))
    rnd = bool(cfill < .45 and .7 < sx / max(sy, 1e-6) < 1.43 and min(sx, sy) > 3 * max(cell))
    return dict(where=where, shape='round' if rnd else 'box', at=[round((x0 + x1) / 2, 3), round((y0 + y1) / 2, 3)], size=[round(sx, 3), round(sy, 3)],
                base=round(b, 3), shoulder=round(max(shoulder, b), 3), top=round(top, 3), fill=round(fill, 3))


def features(mask, H, model, m, cell, size, fp, roof_):
    w, d, h = size
    thr = max(.4, .06 * h)
    out = []
    min_area = max(.35, 3 * cell[0] * cell[1])
    res = np.where(m, H - model, 0.0)
    edge = ndimage.distance_transform_edt(np.pad(mask, 1))[1:-1, 1:-1]
    outside = ndimage.binary_opening(mask & ~m, iterations=1) & (H > thr)
    lab, n = ndimage.label((m & (res > thr)) | outside, structure=np.ones((3, 3)))
    for i in range(1, n + 1):
        sel = lab == i
        if sel.sum() * cell[0] * cell[1] < min_area:
            continue
        rr, cc = np.where(sel)
        if min(np.ptp(rr) + 1, np.ptp(cc) + 1) < 3:          # a one-cell sliver along a roof step is a rasterisation seam
            continue
        side = bool((sel & outside).sum() > .25 * sel.sum())
        b = _blob(sel, H, model, cell, size, 'side' if side else 'roof')
        if b['fill'] < .35:
            continue                    # a ragged ring sliver (round outline vs faceted walls), not a mass
        if not side and b['top'] - b['base'] > .25 * h and float(edge[sel].min()) <= 2.5:
            b.update(where='side', base=0.0)        # a tall mass on the outline (stair tower, attached silo) stands on the ground
        out.append(b)
    for b in out:
        tall = (b['top'] - b['base']) > .25 * h
        b['kind'] = {('round', True): 'tank', ('round', False): 'fan', ('box', True): 'tower', ('box', False): 'vent'}[(b['shape'], tall)]
        if b['where'] == 'side' and min(b['size']) < 1.2:
            b['kind'] = 'pipe'
    out.sort(key=lambda b: -(b['size'][0] * b['size'][1]))
    return out[:16]


def painted_roof(st, size, H, cell):
    """Fans and vent housings the stock TEXTURE paints on the roof (the decoded top view): dark rings or dark squares with a lighter
    centre, 1.5 m+ and at most 40% of the short side, found at three darkness thresholds (squares first). -> feature dicts."""
    import analyze
    import npviews
    w, d, h = size
    img, cov, box = npviews.render_view(st, size, analyze.VIEWS[4], 420)
    roof_colour = [round(float(x), 4) for x in analyze._srgb2lin(img[cov]).mean(0)] if cov.any() else None
    lum = (img.astype(float) / 255) @ np.array([.2126, .7152, .0722])
    mpp = (box[1] - box[0]) / lum.shape[1]
    inner = ndimage.binary_erosion(cov, iterations=3)
    q = 6
    win = max(5, int(8.0 / mpp / q)) | 1
    small = lum[::q, ::q].astype(np.float32)
    pad = np.pad(small, win // 2, mode='edge')        # local median (numpy; scipy median_filter crashes on this interpreter)
    loc = np.median(np.lib.stride_tricks.sliding_window_view(pad, (win, win)), axis=(-2, -1))
    loc = np.kron(loc, np.ones((q, q)))[:lum.shape[0], :lum.shape[1]]
    out = []
    for thr in (.8, .72, .6):
        raw = ndimage.binary_closing(inner & (lum < thr * loc), iterations=2)
        m = ndimage.binary_opening(ndimage.binary_fill_holes(raw), iterations=3)
        lab, _ = ndimage.label(m)
        for j, sl in enumerate(ndimage.find_objects(lab), 1):
            ys, xs = sl
            reg = lab[sl] == j
            sx, sy = (xs.stop - xs.start) * mpp, (ys.stop - ys.start) * mpp
            fill = float(reg.mean())
            hole = float(reg.sum() / max((raw[sl] & reg).sum(), 1))
            if not (.7 <= sx / sy <= 1.43 and min(sx, sy) >= max(1.5, .05 * min(w, d)) and max(sx, sy) <= .4 * min(w, d) and fill >= .6 and hole >= 1.2):
                continue
            x, y = box[0] + (xs.start + xs.stop) / 2 * mpp, box[3] - (ys.start + ys.stop) / 2 * mpp
            if any(abs(x - f['at'][0]) < (sx + f['size'][0]) / 4 and abs(y - f['at'][1]) < (sy + f['size'][1]) / 4 for f in out):
                continue
            r = int(np.clip((d / 2 - y) / cell[1], 0, H.shape[0] - 1))
            c = int(np.clip((x + w / 2) / cell[0], 0, H.shape[1] - 1))
            z = float(H[r, c])
            out.append(dict(where='painted', shape='round' if fill <= .84 else 'box', kind='fan' if fill <= .84 else 'vent', at=[round(x, 3), round(y, 3)],
                            size=[round(sx, 3), round(sy, 3)], base=round(z, 3), shoulder=round(z, 3), top=round(z, 3), fill=round(fill, 3)))
    return sorted(out, key=lambda f: (round(f['at'][0], 1), round(f['at'][1], 1))), roof_colour


# ------------------------------------------------------------------ wall profile (batter below the eave)
def wall_profile(V, size, eave):
    import analyze
    w, d, _ = size
    kx, ky = [], []
    if eave < 1.0:                 # the roof reaches the ground (A-frame, low skirt): no wall to batter
        return dict(x=[[0.0, 1.0], [1.0, 1.0]], y=[[0.0, 1.0], [1.0, 1.0]])
    top = eave
    for t in np.linspace(.02, .98, 17):
        p = analyze._slice_extent(V, t * top)
        if len(p):
            kx.append((float(t), float((p[:, 0].max() - p[:, 0].min()) / w)))
            ky.append((float(t), float((p[:, 1].max() - p[:, 1].min()) / d)))
    simp = lambda kn: [[round(a, 3), round(min(1.0, b), 3)] for a, b in _dp(kn, .02)] if kn else [[0, 1.0], [1, 1.0]]  # noqa: E731
    sx, sy = simp(kx), simp(ky)
    sx[0][0] = sy[0][0] = 0.0
    sx[-1][0] = sy[-1][0] = 1.0
    return dict(x=sx, y=sy)


# ------------------------------------------------------------------ top level
def shape_info(V, size, st=None, like=None):
    """V: (n,3,3) centred soup (x, y about the bbox centre, z from 0), size = (w, d, h) of the stock bounds.
    st: the decoded stock (analyze.load_stock) adds the texture-painted roof features; the remake is measured without it."""
    V = np.asarray(V, float)
    size = tuple(float(x) for x in size)
    mask, H, cell, _ = heightfield(V, size)
    fp = footprint(mask, cell, size)
    rf, model, m = roof(mask, H, cell, size, fp, force=(like or {}).get('roof', {}).get('fit'))
    feats = features(mask, H, model, m, cell, size, fp, rf)
    if st is not None:
        near = lambda f, g: abs(f['at'][0] - g['at'][0]) < (f['size'][0] + g['size'][0]) / 2 and abs(f['at'][1] - g['at'][1]) < (f['size'][1] + g['size'][1]) / 2  # noqa: E731
        painted, roof_colour = painted_roof(st, size, H, cell)
        feats += [f for f in painted if not any(near(f, g) for g in feats)]
    else:
        roof_colour = None
    out = dict(schema=SCHEMA, size_m=[round(x, 3) for x in size], footprint=fp, roof=rf, wall_profile=wall_profile(V, size, rf['eave']), features=feats)
    if roof_colour:
        out['roof_colour'] = roof_colour
    return out
