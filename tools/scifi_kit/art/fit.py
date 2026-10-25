"""Building fit step (owner 2026-10-24: "We can also just scale them down"). After assembly and before the bake, a
building whose plan sticks out more than 0.5 m past the stock outline is scaled uniformly in plan (X and Y, one
factor, about the stock pivot) until it fits; Z takes the same factor unless that would put the height more than
10 % under the stock (then Z stays 1.0). Factors of 0.85 or more are applied; below that the overhang is an offset
mass or a design problem and the asset is flagged for a manual fix. Walls, gates, fences, posts and pipes keep the
strict exact-geometry rule and are never scaled.

  py -3 tools/scifi_kit/art/fit.py            sweep all buildings, write assets/fit_scales.json and a report
"""
import json
import math
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
TABLE = HERE / 'assets' / 'fit_scales.json'
LIMIT = .5
MIN_SCALE = .85
_T = None


def table():
    global _T
    if _T is None:
        _T = json.loads(TABLE.read_text()) if TABLE.exists() else {}
    return _T


def eligible(name):
    import footprint
    return footprint.category(name) == 'building' and 'pipe' not in name


def apply(k, name):
    """Scale the assembled kit in place when the table holds an applied factor for it (called by intent.build)."""
    e = table().get(name)
    if not e or not e.get('applied'):
        return
    s, sz = e['scale'], e['scale_z']
    px, py = e['pivot']
    for pc in k.pieces:
        pc['v'] = [(px + (v[0] - px) * s, py + (v[1] - py) * s, v[2] * sz) for v in pc['v']]
    for a in k.meta.get('anim_nodes', []):
        x, y, z = a['pivot']
        a['pivot'] = (px + (x - px) * s, py + (y - py) * s, z * sz)
    for so in k.meta.get('sockets', []):
        x, y, z = so['position_m']
        so['position_m'] = (px + (x - px) * s, py + (y - py) * s, z * sz)
    k.meta['fit'] = dict(scale=s, scale_z=sz)


def needed(name):
    """-> (scale, protrusion_before, pivot) for the unscaled build: the largest uniform plan factor about the stock
    pivot that brings every part above 0.3 m within 0.5 m of the stock outline."""
    from scipy import ndimage
    import counts
    import footprint
    import topdown
    S = topdown.stock_tris(name)
    R = topdown.remake_tris(name, raw=True)
    o = counts.ORIGINS.get(name, (0, 0, 0))
    pivot = (-o[0], -o[1])
    sv, rv = S.reshape(-1, 3), R.reshape(-1, 3)
    allv = np.r_[sv, rv]
    box = (allv[:, 0].min() - 2, allv[:, 1].min() - 2, allv[:, 0].max() + 2, allv[:, 1].max() + 2)
    ms = footprint._plan(S, box)
    d = ndimage.distance_transform_edt(~ms) / footprint.PX

    def prot(s):
        T = R.copy()
        T[..., 0] = pivot[0] + (T[..., 0] - pivot[0]) * s
        T[..., 1] = pivot[1] + (T[..., 1] - pivot[1]) * s
        mr = footprint._plan(T, box)
        return float(d[mr].max()) if mr.any() else 0.0
    p0 = prot(1.0)
    if p0 <= LIMIT:
        return 1.0, p0, pivot
    lo, hi = .5, 1.0
    if prot(lo) > LIMIT:
        return None, p0, pivot
    for _ in range(14):
        mid = (lo + hi) / 2
        if prot(mid) <= LIMIT:
            lo = mid
        else:
            hi = mid
    return math.floor(lo * 1000) / 1000, p0, pivot


def sweep():
    import intent
    import analyze
    out = {}
    names = [n for n in intent.ASSETS if eligible(n)]
    for n in names:
        s, p0, pivot = needed(n)
        H = float(analyze.load_stock(n)['V'].reshape(-1, 3)[:, 2].max() - analyze.load_stock(n)['V'].reshape(-1, 3)[:, 2].min())
        k = intent.build(n, raw=True)
        h = k.meta['bounds_m'][1][2] - k.meta['bounds_m'][0][2]
        if s is None:
            out[n] = dict(applied=False, scale=None, protrusion_before=round(p0, 2), reason='needs more than 0.5x: manual fix')
            continue
        sz = s if h * s >= .9 * H else 1.0
        e = dict(applied=s < 1.0 and s >= MIN_SCALE, scale=s, scale_z=sz if s < 1.0 else 1.0, pivot=[round(pivot[0], 4), round(pivot[1], 4)],
                 protrusion_before=round(p0, 2), height_remake=round(h, 2), height_stock=round(H, 2))
        if s < MIN_SCALE:
            e['reason'] = 'scale below 0.85: offset mass or design problem, manual fix'
        if sz == 1.0 and s < 1.0:
            e['note'] = 'Z kept at 1.0: scaling Z would put the height more than 10 % under the stock'
        # doors below 2.0 m clear after scaling
        low = []
        for pc in k.pieces:
            if pc.get('door') or pc.get('feature') in ('door', 'rollup'):
                v = np.asarray(pc['v'], float)
                hh = (v[:, 2].max() - v[:, 2].min()) * e['scale_z']
                if 1.2 < (v[:, 2].max() - v[:, 2].min()) and hh < 2.0:
                    low.append(round(hh, 2))
        if low and e['applied']:
            e['doors_below_2m'] = sorted(set(low))
        out[n] = e
        print(n, e)
    TABLE.write_text(json.dumps(out, indent=1))
    return out


if __name__ == '__main__':
    sweep()
