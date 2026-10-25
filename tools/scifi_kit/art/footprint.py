"""Hard footprint check against the stock (owner 2026-10-24: "gates/fences/walls have to have the same footprint as
stock otherwise they can't be a replacement"). Missions place these assets on the stock grid; runs of walls, fences
and posts meet end to end, and pathing/blocking come from the stock footprint.

Both meshes are compared in the authored frame (the stock bbox-centred frame the remakes are built in, z = 0 at the
base; the bake restores the stock pivot from that frame, so matching the frame keeps the pivot identical).

strict (walls, gates, fences, posts, diagonal pieces on their own run axis):
  - run length along the run axis within max(1 %, 2 cm) of the stock;
  - both end faces at the stock end positions within the same tolerance;
  - thickness within 5 % (stock billboard planes thinner than 0.3 m: within 0.3 m, flagged);
  - plan IoU >= 0.9 (plan cells covered above 0.3 m; thin runs use the run-axis occupancy instead);
  - joint test: stock, remake, remake, stock in a row along the run axis must be continuous, no gap or overlap.
building:
  - plan IoU >= 0.8;
  - nothing above 0.3 m sticks out more than 0.5 m past the stock plan outline.
prop: reported only.
"""
import csv
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

import topdown

INV = Path('E:/mc2-ref/scifi-kit/mapping/inventory.csv')
PX = 10.0
_CAT = None


def category(name):
    global _CAT
    if _CAT is None:
        with open(INV, encoding='utf-8-sig') as f:
            _CAT = {r['id']: r['category'] for r in csv.DictReader(f)}
    c = _CAT.get(name, '')
    if 'gatecontrol' in name:
        return 'building'
    if c in ('wall', 'gate', 'fence') or any(w in name for w in ('wall', 'gate', 'fence', 'post')):
        return 'strict'
    if c in ('building', 'turret', 'bridge'):
        return 'building'
    return 'prop'


def _run_occ(T, ax, lo, hi, z=.3, px=20.0):
    """1-D occupancy along the run axis from the triangles' own extents (works for zero-thickness card planes)."""
    n = int((hi - lo) * px) + 2
    occ = np.zeros(n, bool)
    for t in T:
        if t[:, 2].max() <= z:
            continue
        a, b = int((t[:, ax].min() - lo) * px), int((t[:, ax].max() - lo) * px)
        occ[max(0, a):max(0, min(n, b + 1))] = True
    return occ


def _plan(T, box, z=.3, px=PX, fill=False):
    """Plan mask above z. fill (strict pieces only): enclosed holes filled, so an open-topped stock box (side walls, no
    cap: the MCO tunnel jambs) occupies its whole outline like a capped solid; buildings keep open yards open."""
    Z = topdown.raster(T, box, px) > z
    if fill:
        from scipy.ndimage import binary_fill_holes
        from PIL import Image, ImageDraw
        # (near-)vertical faces rasterise to nothing: draw every face's plan edges so a box of side walls closes
        x0, _, _, y1 = box
        im = Image.fromarray(Z.astype(np.uint8) * 255)
        dr = ImageDraw.Draw(im)
        for t in np.asarray(T):
            if t[:, 2].max() > z:
                P = [((v[0] - x0) * px, (y1 - v[1]) * px) for v in t]
                dr.line(P + [P[0]], fill=255, width=1)
        Z = binary_fill_holes(np.asarray(im) > 0)
    return Z


def check(name, S=None, R=None):
    """-> dict(kind, ok, fails[], metrics{}, strip=path or None)."""
    kind = category(name)
    S = topdown.stock_tris(name) if S is None else S
    R = topdown.remake_tris(name) if R is None else R
    sv, rv = S.reshape(-1, 3), R.reshape(-1, 3)
    slo, shi, rlo, rhi = sv.min(0), sv.max(0), rv.min(0), rv.max(0)
    out = dict(kind=kind, fails=[], metrics={}, strip=None)
    allv = np.r_[sv, rv]
    box = (allv[:, 0].min() - 1, allv[:, 1].min() - 1, allv[:, 0].max() + 1, allv[:, 1].max() + 1)
    ms, mr = _plan(S, box, fill=kind == 'strict'), _plan(R, box, fill=kind == 'strict')
    iou = float((ms & mr).sum() / max(1, (ms | mr).sum()))
    out['metrics']['plan_iou'] = round(iou, 3)
    if kind == 'strict':
        ax = int(np.argmax((shi - slo)[:2]))
        cx = 1 - ax
        Ls, Lr = shi[ax] - slo[ax], rhi[ax] - rlo[ax]
        tol = max(.01 * Ls, .02)
        e0, e1 = rlo[ax] - slo[ax], rhi[ax] - shi[ax]
        Ts, Tr = shi[cx] - slo[cx], rhi[cx] - rlo[cx]
        thin = Ts < .3 or category(name) == 'strict' and ('fence' in name)   # planes and card-built wire fences
        ttol = .3 if thin else .05 * Ts
        out['metrics'].update(axis='xy'[ax], run_stock=round(Ls, 3), run_remake=round(Lr, 3), end_lo=round(e0, 3), end_hi=round(e1, 3),
                              thick_stock=round(Ts, 3), thick_remake=round(Tr, 3), thin_stock=thin)
        if abs(Lr - Ls) > tol:
            out['fails'].append(f'footprint: run length {Lr:.3f} m vs stock {Ls:.3f} m (tolerance {tol:.3f})')
        if abs(e0) > tol or abs(e1) > tol:
            out['fails'].append(f'footprint: end faces off the stock ends by {e0:+.3f} / {e1:+.3f} m (tolerance {tol:.3f})')
        if abs(Tr - Ts) > ttol:
            out['fails'].append(f'footprint: thickness {Tr:.3f} m vs stock {Ts:.3f} m (tolerance {ttol:.3f})')
        if thin:                                  # billboard plane in the stock: compare the run-axis occupancy
            ps, pr = _run_occ(S, ax, slo[ax], shi[ax]), _run_occ(R, ax, slo[ax], shi[ax])
            iou1 = float((ps & pr).sum() / max(1, (ps | pr).sum()))
            out['metrics']['run_iou'] = round(iou1, 3)
            if iou1 < .9:
                out['fails'].append(f'footprint: run occupancy IoU {iou1:.2f} < 0.90')
        elif iou < .9:
            out['fails'].append(f'footprint: plan IoU {iou:.2f} < 0.90')
        import cells
        pitch = cells.pitch_m(name)
        out['strip'], gaps = joint_strip(name, S, R, ax, pitch=pitch)
        out['metrics']['pitch_m'] = pitch
        out['metrics']['joint_gap_m'] = gaps
        if gaps > .075:                       # 1.5 raster pixels at 20 px/m: below that is sampling, not a gap
            out['fails'].append(f'footprint: joint test (stock-remake-remake-stock at the stock pitch) opens {gaps:.3f} m '
                                'of gap where the stock run is continuous')
        # gate door node: the stock door carries vertices only at its ends, so the opening cells stay free for the
        # gate's state-driven blocking; the remake's leaves sit under that node and are left out the same way
        if 'gate' in name and 'control' not in name:
            # gate: the static parts must block exactly the stock static cells; the door node may block only cells
            # inside the stock opening span (its blocking is state-driven: the engine opens those cells with the gate)
            cr = cells.check(name, topdown.remake_tris(name, skip_anim='GateDoor'), ref=topdown.stock_tris(name, 'static'),
                             rotations=(0.0, 90.0, 180.0, 270.0))
            out['metrics']['cells_rule'] = 'gate: static parts vs stock static parts'
        else:
            cr = cells.check(name, R, rotations=(0.0, 90.0, 180.0, 270.0))
        out['metrics']['cells'] = {k: v for k, v in cr.items() if k != 'mismatches'}
        diag = cells.check(name, R if 'gate' not in name else topdown.remake_tris(name, skip_anim='GateDoor'),
                           ref=None if 'gate' not in name else topdown.stock_tris(name, 'static'), rotations=(45.0, 135.0, 225.0, 315.0))
        out['metrics']['cells_diagonal_placements'] = {k: v for k, v in diag.items() if k != 'mismatches'}
        if diag.get('placements') and diag['remake_match'] < 1.0:
            out['fails'].append(f'footprint: at 45-degree placements the blocking cells differ from the stock at '
                                f'{len(diag["mismatches"])} of {diag["placements"]} (the stock blocks only the cells its few '
                                'vertices fall in; continuous geometry blocks the cells between - needs the stock collision)')
        if cr.get('placements') and cr['remake_match'] < 1.0:
            out['fails'].append(f'footprint: blocking cells differ from the stock at {len(cr["mismatches"])} of {cr["placements"]} '
                                f'sampled mission placements (e.g. {cr["mismatches"][0]})')
    elif kind == 'building':
        from scipy import ndimage
        d = ndimage.distance_transform_edt(~ms) / PX
        prot = float(d[mr].max()) if mr.any() else 0.0
        out['metrics']['protrusion_m'] = round(prot, 2)
        if iou < .8:
            out['fails'].append(f'footprint: plan IoU {iou:.2f} < 0.80')
        import intent
        lim = intent.ASSETS.get(name, {}).get('tol', {}).get('protrusion', .5)   # per-asset, with a stated reason
        if prot > lim:
            out['fails'].append(f'footprint: mass sticks out {prot:.2f} m past the stock outline (limit {lim} m)')
    out['ok'] = not out['fails']
    return out


def _occ_shift(S, ax, L, k_, box, px):
    T = S.copy()
    T[..., ax] += k_ * L
    m = _plan(T, box, z=.05, px=px)
    return m.any(axis=0 if ax == 0 else 1)


def joint_strip(name, S, R, ax, out_dir=None, pitch=None):
    """Stock, remake, remake, stock along the run axis at the stock pitch (survey; the bbox length when the type was
    never placed in a run); returns (png, metres of gap the remake opens where the all-stock chain is continuous)."""
    sv = S.reshape(-1, 3)
    L = pitch or (sv[:, ax].max() - sv[:, ax].min())

    def shifted(T, k_):
        T = T.copy()
        T[..., ax] += k_ * L
        return T
    parts = [shifted(S, 0), shifted(R, 1), shifted(R, 2), shifted(S, 3)]
    allv = np.concatenate([p.reshape(-1, 3) for p in parts])
    box = (allv[:, 0].min() - 1, allv[:, 1].min() - 1, allv[:, 0].max() + 1, allv[:, 1].max() + 1)
    px = 20.0
    masks = [_plan(p, box, z=.05, px=px) for p in parts]
    occ = [m.any(axis=0 if ax == 0 else 1) for m in masks]       # 1-D occupancy along the run axis
    tot = np.sum(occ, axis=0)
    ref = occ[0].astype(int) + _occ_shift(S, ax, L, 1, box, px) + _occ_shift(S, ax, L, 2, box, px) + occ[3]
    span = np.where(ref > 0)[0]
    gap = int(((tot == 0) & (ref > 0))[span[0]:span[-1] + 1].sum()) if len(span) else 0
    m = gap / px
    # image
    cols = [(150, 150, 150), (230, 140, 40), (60, 170, 230), (150, 150, 150)]
    H, W = masks[0].shape
    img = np.full((H, W, 3), 30, np.uint8)
    for mk, c in zip(masks, cols):
        img[mk] = c
    both = np.sum(masks, axis=0) > 1
    img[both] = (255, 40, 40)
    im = Image.fromarray(img)
    if ax == 1:
        im = im.rotate(90, expand=True)
    im.thumbnail((1600, 400))
    canvas = Image.new('RGB', (im.width, im.height + 26), (24, 26, 30))
    canvas.paste(im, (0, 26))
    ImageDraw.Draw(canvas).text((6, 6), f'{name} joint test at the stock pitch {L * 4:.1f} WU: stock | remake | remake | stock   '
                                f'gap opened {m:.3f} m (red = overlap, as the stock runs overlap)',
                                fill=(235, 235, 235))
    p = Path(out_dir) if out_dir else topdown.OUT / name
    p.mkdir(parents=True, exist_ok=True)
    f = p / f'{name}_joint.png'
    canvas.save(f)
    return f, round(m, 3)
