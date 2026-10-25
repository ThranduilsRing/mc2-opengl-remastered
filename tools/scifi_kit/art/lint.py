"""Offline geometry lint for intent-pass assets, run before any preview (no Blender).

1. overhang: a mass/roof piece above the ground whose lowest corners have nothing under them within
   0.25 m (e.g. a rectangular roof slab sticking out past a chamfered pod's plan);
2. hidden opening: a door leaf or glazing whose face is covered by a mass piece on BOTH sides within
   1.2 m (e.g. strip windows hidden behind a glacis skirt).
Findings fail intent validation; deliberate eaves and cantilevers can use lint_skip.
"""
import numpy as np

import clearance
import ind_parts as ip

STRUCT = {ip.STEEL, ip.GALV, ip.RAIL, 'grate'}


def _normal(v, f):
    n = np.zeros(3)
    for i in range(len(f)):
        a, b = v[f[i]], v[f[(i + 1) % len(f)]]
        n += np.cross(a, b)
    ln = np.linalg.norm(n)
    return n / ln if ln > 1e-9 else None, ln


def _hull(pts):
    pts = sorted(set(map(tuple, np.round(pts, 3))))
    if len(pts) < 3:
        return np.array(pts)

    def half(seq):
        h = []
        for p in seq:
            while len(h) >= 2 and (h[-1][0] - h[-2][0]) * (p[1] - h[-2][1]) - (h[-1][1] - h[-2][1]) * (p[0] - h[-2][0]) <= 0:
                h.pop()
            h.append(p)
        return h
    lower, upper = half(pts), half(pts[::-1])
    return np.array(lower[:-1] + upper[:-1])


def _outside(H, p):
    """Distance of p outside the CCW convex polygon H (0 inside)."""
    d = 0.0
    for a, b in zip(H, np.roll(H, -1, 0)):
        e = b - a
        n = np.array([e[1], -e[0]]) / (np.linalg.norm(e) or 1)
        d = max(d, float(np.dot(p - a, n)))
    return d


def check(k):
    sc = clearance.Scene(k)
    out = []
    # 1. a roof/deck piece must sit within the plan of what carries it (hull of supporting verts at its base)
    P = [(np.asarray(pc['v'], float), pc) for pc in k.pieces if pc['role'] != 'detail' and pc['v']]
    for v, pc in P:
        if pc['role'] != 'roof' or pc.get('lint_skip') or v[:, 2].min() < .3 or np.ptp(v[:, 2]) > 1.0:
            continue
        zb = v[:, 2].min()
        lo, hi = v[:, :2].min(0) - .5, v[:, :2].max(0) + .5
        sup = []
        for w, qc in P:
            if qc is pc or qc['role'] not in ('mass', 'skin', 'roof'):      # a neighbouring roof deck carries a shared edge
                continue
            if w[:, 2].min() > zb + .6 or w[:, 2].max() < zb - .6:
                continue
            # a piece passing through the slab's height (a wall it abuts) carries it along its whole plan
            m = np.all(w[:, :2] >= lo, 1) & np.all(w[:, :2] <= hi, 1)
            sup += list(w[m, :2])
        if len(sup) < 3:
            out.append(f'overhang: {pc["role"]}:{pc["m"]} at z={zb:.2f} has nothing under it')
            continue
        H = _hull(np.array(sup))
        d = max(_outside(H, p) for p in v[:, :2])
        if d > .4:
            out.append(f'overhang: {pc["role"]}:{pc["m"]} at z={zb:.2f} sticks out {d:.2f} m past its support '
                       f'near ({v[:, 0].mean():.1f}, {v[:, 1].mean():.1f})')
    # 1b. a mounted part (window frame, visor, plate, rib) must sit on a block: most of its corners within
    #     0.6 m of some mass/skin/roof piece (catches frames hanging past a wall end or a pod's chamfer)
    # glazing carries its own mullions and head ledge (slanted control-cab glass)
    blocks = [(w.min(0), w.max(0)) for w, qc in P if qc['role'] in ('mass', 'skin', 'roof', 'glass')]
    if blocks:
        blo = np.array([b[0] for b in blocks])
        bhi = np.array([b[1] for b in blocks])
        for v, pc in P:
            if pc['role'] != 'frame' or pc.get('access') or pc.get('platform') or v[:, 2].min() < .3 or len(v) < 4 \
                    or pc['m'] in STRUCT:      # steelwork (platforms, legs, braces) is checked by clearance.py
                continue
            gap = np.maximum(np.maximum(blo[None] - v[:, None], v[:, None] - bhi[None]), 0)
            gap[:, :, 2] = np.maximum(blo[None, :, 2] - v[:, None, 2], 0)      # standing above a block's plan is supported
            d = np.linalg.norm(gap, axis=2).min(1)
            far = (d > .6).mean()
            if far > .25:
                out.append(f'overhang: frame:{pc["m"]} {far:.0%} of it hangs clear of any block near '
                           f'({v[:, 0].mean():.1f}, {v[:, 1].mean():.1f}, {v[:, 2].mean():.1f})')
    mass = np.array([sc.k.pieces[o]['role'] == 'mass' for o in sc.own]) if len(sc.own) else np.array([])
    for i, pc in enumerate(k.pieces):
        if not (pc.get('door') or pc.get('pane') or pc['m'] == 'glass') or not pc['f']:
            continue
        v = np.asarray(pc['v'], float)
        best = max(pc['f'], key=lambda f: _normal(v, f)[1])
        n, _ = _normal(v, best)
        if n is None:
            continue
        c = v.mean(0)
        blocked = 0
        for s in (1, -1):
            hs = [h for h in sc.hits(c + n * s * .03, c + n * s * 1.2) if sc.own[h[0]] != i and mass[h[0]]]
            blocked += bool(hs)
        if blocked == 2:
            out.append(f'hidden opening: {pc["m"]} at ({c[0]:.1f}, {c[1]:.1f}, {c[2]:.1f}) is covered on both sides')
    return out
