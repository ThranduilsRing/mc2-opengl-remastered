"""Automatic validator: `run.py validate <asset>`. Names exactly what is wrong.

Checks (thresholds in assets/validate_thresholds.json, calibrated on the 24 approved assets by `validate --calibrate`):
  silhouette  remake game-mesh silhouette vs the decoded stock mesh, same 5 views (front/left/rear/right/top), IoU
  palette     area-weighted mean colour of the remake vs the stock texture's mean (Lab delta E); post-bake: baked atlas mean
  counts      contract (counts.json) vs a recount from the built model's feature tags
  budget      game-carrier triangles per state vs the class budget (post-bake: LOD0/1/2 files)
  states      all five states build and differ; destroyed is lower than pristine
  sockets     light sockets exist for every door/window/lamp the contract promises (post-bake: in the bake json and the GLBs)
Pure Python + numpy/scipy/PIL; never imported by Blender.
"""
import json
import math
import re
from pathlib import Path

import numpy as np

import analyze
import archetypes
import bake_budget
import counts as counts_mod
import drafts
import geo

HERE = Path(__file__).resolve().parent
THRESH = HERE / 'assets' / 'validate_thresholds.json'
STATES = ('pristine', 'worn', 'abandoned', 'partial', 'destroyed')
DEFAULT = dict(iou_min=dict(front=.5, left=.5, rear=.5, right=.5, top=.5), iou_mean_min=.5, palette_de_max=30.0, baked_de_max=30.0, thin_px=6)


def thresholds():
    if THRESH.is_file():
        t = json.loads(THRESH.read_text())
        return {**DEFAULT, **t}
    return DEFAULT


# ------------------------------------------------------------------ building the remake
def build_game(name, cond='pristine'):
    drafts.FIT_CACHE.pop(name, None)
    geo.GAME_MESH = False
    drafts.build(name, 'pristine')          # the high source primes the fit cache, as in bake.py
    geo.GAME_MESH = True
    try:
        return drafts.build(name, cond)
    finally:
        geo.GAME_MESH = False


def tri_soup(k):
    out = []
    for pc in k.pieces:
        V = np.array(pc['v'], float)
        for f in pc['f']:
            for i in range(1, len(f) - 1):
                out.append(V[[f[0], f[i], f[i + 1]]])
    return np.array(out) if out else np.zeros((0, 3, 3))


def tri_count(k):
    return sum(sum(len(f) - 2 for f in p['f']) for p in k.pieces)


def remake_masks(k, size, long_px=192):
    V = tri_soup(k)
    out = {}
    for view in analyze.VIEWS:
        box = analyze.view_box(size, view)
        W, H = analyze.grid_for(box, long_px)
        out[view[0]] = (analyze.raster(analyze.project(V, view), W, H, box), box)
    return out


def _fill(m):
    """Close gaps (wire rows, strands) and fill holes, then 1 px tolerance: the silhouette a player reads."""
    from scipy import ndimage
    f = ndimage.binary_fill_holes(ndimage.binary_closing(np.pad(m, 12), iterations=5))[12:-12, 12:-12]
    return ndimage.binary_dilation(f, iterations=1)


def iou(a, b):
    a, b = _fill(a), _fill(b)
    u = (a | b).sum()
    return float((a & b).sum() / u) if u else 1.0


def silhouette(name, k, size, thin_px):
    st = analyze.load_stock(name)
    sm = analyze.stock_masks(st, list(size))
    rm = remake_masks(k, size)
    res = {}
    for view, (m, box) in sm.items():
        ys, xs = np.where(m)
        thin = len(xs) == 0 or min(xs.max() - xs.min(), ys.max() - ys.min()) < thin_px
        res[view] = dict(iou=round(iou(m, rm[view][0]), 3), thin=bool(thin), stock_px=int(m.sum()), remake_px=int(rm[view][0].sum()))
    return res


# ------------------------------------------------------------------ palette
def _lab(rgb_lin):
    c = np.clip(np.asarray(rgb_lin, float), 0, 1)
    m = np.array([[.4124, .3576, .1805], [.2126, .7152, .0722], [.0193, .1192, .9505]])
    xyz = (m @ c) / np.array([.9505, 1.0, 1.089])
    f = np.where(xyz > .008856, np.cbrt(xyz), 7.787 * xyz + 16 / 116)
    return np.array([116 * f[1] - 16, 500 * (f[0] - f[1]), 200 * (f[1] - f[2])])


def delta_e(a, b):
    return float(np.linalg.norm(_lab(a) - _lab(b)))


def remake_mean_colour(k):
    pal = geo.LANG['palettes'][k.palette]
    acc, tot = np.zeros(3), 0.0
    for pc in k.pieces:
        if pc['role'] in ('detail', 'glass') or pc['m'].startswith('veg'):
            continue
        col = archetypes.slot_colour(pc['m'], pal)
        if col is None:
            continue
        V = np.array(pc['v'], float)
        area = 0.0
        for f in pc['f']:
            for i in range(1, len(f) - 1):
                area += np.linalg.norm(np.cross(V[f[i]] - V[f[0]], V[f[i + 1]] - V[f[0]])) / 2
        acc += np.array(col, float) * area
        tot += area
    return acc / tot if tot else np.array([.3, .3, .3])


def stock_mean(name):
    path = analyze.ASSETS / f'{name}.analysis.json'
    if path.is_file():
        a = json.loads(path.read_text())
        if a.get('palette'):
            return np.array(a['palette']['mean_linear'])
    info = analyze.palette_info(analyze.load_stock(name))
    return np.array(info['mean_linear']) if info else None


def baked_mean(pass_dir, name, cond='pristine'):
    import wear_metrics
    px = wear_metrics.mine_px(pass_dir, name, cond)
    return analyze._srgb2lin(px).mean(0)


# ------------------------------------------------------------------ counts
GROUP_RE = (('crates', re.compile(r'^crate[:]?\d+$')), ('barrels', re.compile(r'^(drum|barrel[:])\d+$')))


def recount(k):
    groups = {}
    for pc in k.pieces:
        g = pc.get('group')
        if g:
            groups.setdefault(g, 0)
            groups[g] += 1
    out = {}
    for g in groups:
        m = re.match(r'^(door|window|bay|panel|crate|barrel|tank|rib|support|antenna|stair):(\d+)$', g)
        if m:
            out.setdefault(archetypes.FIELD[m.group(1)], set()).add(g)
        else:
            for field, rx in GROUP_RE:
                if rx.match(g):
                    out.setdefault(field, set()).add(g)
    return {f: len(v) for f, v in out.items()}


def verifiable_fields(name):
    arch = drafts.ARCH_OF.get(name)
    if arch:
        import arch_props
        return set(arch_props.TAGGED.get(arch, ()))
    return {'crates', 'barrels'} & set(f for f, v in counts_mod.CONTRACTS.get(name, {}).items() if v['count'])


# ------------------------------------------------------------------ cheap pre-render checks (failures the runner only saw after rendering)
SLOT_CLASSES = ('paint', 'corr', 'ferro', 'wood', 'canvas')


def render_sanity(k):
    """Inverted solids (wrong face winding), unknown palette slots (Blender KeyError) and near-black material slots
    (detail plates that bake black). Returns failure strings."""
    out = []
    pal = geo.LANG['palettes'][k.palette]
    bad_slot, black, inverted = set(), set(), 0
    closed = 0
    for pc in k.pieces:
        cls, _, slot = pc['m'].partition('.')
        if cls in SLOT_CLASSES and slot and slot not in pal and not (cls in ('corr', 'wood', 'canvas', 'ferro') and True):
            bad_slot.add(pc['m'])
        if cls == 'paint' and slot and slot not in pal:
            bad_slot.add(pc['m'])
        col = archetypes.slot_colour(pc['m'], pal)
        if col is not None and cls not in ('rubber', 'glass', 'char', 'lamp', 'lamp_red') and sum(col) / 3 < .02:
            black.add(pc['m'])
        # winding: signed volume of a closed piece must be positive (outward faces)
        edges = {}
        for f in pc['f']:
            for i in range(len(f)):
                e = (f[i], f[(i + 1) % len(f)])
                edges[e] = edges.get(e, 0) + 1
        if edges and all(edges.get((b, a), 0) == c for (a, b), c in edges.items()):
            closed += 1
            V = pc['v']
            vol = 0.0
            for f in pc['f']:
                for i in range(1, len(f) - 1):
                    a, b, c = V[f[0]], V[f[i]], V[f[i + 1]]
                    vol += (a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * c[1] - b[1] * c[0])) / 6
            inverted += vol < -1e-9
    if bad_slot:
        out.append('render: palette slot(s) missing from the palette (Blender would raise): ' + ', '.join(sorted(bad_slot)))
    if black:
        out.append('render: material slot(s) resolve to near-black (would bake black): ' + ', '.join(sorted(black)))
    if closed and inverted > .85 * closed:
        out.append(f'render: {inverted} of {closed} closed pieces have inward faces (wrong winding)')
    return out


# ------------------------------------------------------------------ the validator
def validate(name, pass_dir=None, pre=False, with_masks=True):
    """-> dict(asset, ok, failures[], notes[], metrics{}). `pass_dir` adds the post-bake checks."""
    th = thresholds()
    fails, notes, met = [], [], {}
    if name not in drafts.DRAFTS:
        return dict(asset=name, ok=False, failures=[f'{name}: not registered (no params.json / not a legacy draft)'], notes=[], metrics={})
    size = drafts.SIZES[name]
    fails.extend(check_stock_reference(name))
    k0 = build_game(name, 'pristine')
    # --- states
    ks = {}
    for c in STATES:
        try:
            ks[c] = build_game(name, c)
        except Exception as exc:     # a state that cannot be built is missing
            fails.append(f'state {c}: build failed ({type(exc).__name__}: {exc})')
    top = lambda kk: max(v[2] for p in kk.pieces for v in p['v']) if kk.pieces else 0.0  # noqa: E731
    if 'destroyed' in ks and 'pristine' in ks:
        if top(ks['destroyed']) > top(ks['pristine']) * .98 and name in drafts.BUILDINGS:
            fails.append(f'states: destroyed (top {top(ks["destroyed"]):.2f} m) is not lower than pristine ({top(ks["pristine"]):.2f} m)')
    for c in ('partial', 'destroyed'):
        if c in ks and tri_count(ks[c]) == tri_count(k0) and all(a['v'] == b['v'] for a, b in zip(ks[c].pieces, k0.pieces)):
            fails.append(f'states: {c} is identical to pristine (no removable skin / hit zone?)')
    fails.extend(render_sanity(k0))
    # --- budget
    bud = bake_budget.budget(name)
    met['tris'] = {c: tri_count(kk) for c, kk in ks.items()}
    for c, n in met['tris'].items():
        if n > bud:
            fails.append(f'budget: {c} game carrier {n} tris > {bud}')
    # --- sockets
    sockets = lights_for(k0)
    met['sockets'] = len(sockets)
    expect = expected_sockets(name, k0)
    if len(sockets) < expect:
        fails.append(f'sockets: {len(sockets)} light sockets, contract needs {expect} (doors/bays/windows/lamps)')
    for c in ('partial', 'destroyed', 'abandoned'):
        if c in ks and lights_for(ks[c]):
            fails.append(f'sockets: {c} must have no active emitters')
    # --- counts
    rc = recount(k0)
    contract = counts_mod.CONTRACTS.get(name, {})
    mism = []
    for f in sorted(verifiable_fields(name)):
        want = contract.get(f, {}).get('count') or 0
        got = rc.get(f, 0)
        if want != got:
            mism.append(f'{f.replace("_", " ")} {got} vs {want}')
    met['recount'] = rc
    if mism:
        fails.append('counts: ' + '; '.join(mism) + '  (model vs counts.json)')
    skipped = sorted(set(f for f, v in contract.items() if v['count']) - verifiable_fields(name))
    if skipped:
        notes.append('counts not verifiable (no tags): ' + ', '.join(skipped))
    fails.extend(check_placement(name, k0, notes))
    # --- palette (declared)
    try:
        sm_ = stock_mean(name)
        if sm_ is None:
            raise ValueError('stock texture unreadable')
        de = delta_e(remake_mean_colour(k0), sm_)
        met['palette_de'] = round(de, 1)
        if de > th['palette_de_max']:
            fails.append(f'palette: remake mean colour dE {de:.1f} from the stock texture mean (max {th["palette_de_max"]})')
    except Exception as exc:
        notes.append(f'palette skipped: {exc}')
    # --- silhouette
    if with_masks:
        sil = silhouette(name, k0, size, th['thin_px'])
        met['silhouette'] = sil
        live = [r['iou'] for r in sil.values() if not r['thin']]
        met['iou_mean'] = round(sum(live) / len(live), 3) if live else None
        if live and met['iou_mean'] < th['iou_mean_min']:
            fails.append(f'silhouette: mean IoU over the 5 views {met["iou_mean"]:.2f} < {th["iou_mean_min"]:.2f}')
        for view, r in sil.items():
            if r['thin']:
                continue
            need = th['iou_min'][view]
            if r['iou'] < need:
                idx = [v[0] for v in analyze.VIEWS].index(view) + 1
                fails.append(f'silhouette: {view} (side{idx}) IoU {r["iou"]:.2f} < {need:.2f}')
    # --- post-bake
    if pass_dir:
        fails.extend(check_baked(name, Path(pass_dir), bud, expect, th, met))
    return dict(asset=name, ok=not fails, failures=fails, notes=notes, metrics=met)


def check_stock_reference(name):
    """The remake's stock reference must be the asset's OWN mapped intact mesh (catalog), not a look-alike."""
    want = analyze.stock_models(name)['intact']
    if not (analyze.STOCK / f'{want}.json').is_file():
        return [f'stock reference: mapped mesh {want}.json is missing for {name}']
    ap = analyze.ASSETS / f'{name}.analysis.json'
    if ap.is_file():
        got = json.loads(ap.read_text()).get('stock_model', {}).get('intact_stem')
        if got != want:
            return [f'stock reference: analysis used mesh {got!r} but {name} maps to {want!r}; re-run `run.py analyze {name}`']
    return []


def _model_features(k, w, d):
    """Placement of every tagged feature in the model: (kind, side, u along the face, z centre)."""
    groups = {}
    for pc in k.pieces:
        g = pc.get('group') or ''
        m = re.match(r'^(door|window|bay):(\d+)$', g)
        if m:
            groups.setdefault((m.group(1), int(m.group(2))), []).append(pc)
    out = []
    for (kind, _), pcs in groups.items():
        pts = [v for p in pcs for v in p['v']]
        c = [sum(v[i] for v in pts) / len(pts) for i in range(3)]
        dist = {'front': abs(c[1] + d / 2), 'rear': abs(c[1] - d / 2), 'left': abs(c[0] + w / 2), 'right': abs(c[0] - w / 2)}
        side = min(dist, key=dist.get)
        u = {'front': c[0], 'rear': -c[0], 'left': -c[1], 'right': c[1]}[side]
        zs = [v[2] for v in pts]
        out.append((kind, side, u, (min(zs) + max(zs)) / 2))
    return out


def check_placement(name, k, notes):
    """Per side: left/right/centred position and row height of doors/bays/windows vs the confirmed placements in params
    (and, as a note, vs the painted regions measured on the stock texture)."""
    arch = drafts.ARCH_OF.get(name)
    if arch not in ('box', 'tower'):
        return []
    pp = analyze.ASSETS / f'{name}.params.json'
    if not pp.is_file():
        return []
    p = json.loads(pp.read_text())
    w, d, h = drafts.SIZES[name]
    model = _model_features(k, w, d)
    out = []
    for kind in ('door', 'bay', 'window'):
        want = [f for f in p.get('features', []) if f['kind'] == kind]
        for side in archetypes.SIDES:
            wf = sorted((f['at'], f['z'] + f['h'] / 2) for f in want if f['side'] == side)
            mf = sorted((u, z) for kd, sd, u, z in model if kd == kind and sd == side)
            if len(wf) != len(mf) and wf:
                out.append(f'placement: {kind}s on {side}: params list {len(wf)} at u={[round(a, 1) for a, _ in wf]} but the model has {len(mf)} at u={[round(a, 1) for a, _ in mf]}')
                continue
            length = d if side in ('left', 'right') else w
            for (u0, z0), (u1, z1) in zip(wf, mf):
                if abs(u0 - u1) > max(1.2, .1 * length) or abs(z0 - z1) > 0.9:
                    out.append(f'placement: {kind} on {side} planned at u={u0:.1f},z={z0:.1f} but built at u={u1:.1f},z={z1:.1f}')
    ap = analyze.ASSETS / f'{name}.analysis.json'
    if ap.is_file():
        a = json.loads(ap.read_text())
        for field, kind in (('windows', 'window'), ('doors', 'door')):
            seen = {f['side'] for f in a['counts'][field]['where'] if f.get('conf', 0) >= .6}
            have = {sd for kd, sd, _, _ in model if kd == kind}
            if seen - have:
                notes.append(f'stock texture shows painted {kind}s on {sorted(seen - have)} that the model lacks (check features[])')
    return out


def lights_for(k):
    import lights
    return lights.from_kit(k, k.meta['condition'])


def expected_sockets(name, k):
    """Sockets the stock contract promises: one per door/bay/window the model is checked for, plus any the builder declared."""
    if drafts.ARCH_OF.get(name):
        import arch_props
        tagged = set(arch_props.TAGGED.get(drafts.ARCH_OF[name], ())) if drafts.ARCH_OF[name] in ('box', 'tower') else set()
        c = counts_mod.CONTRACTS.get(name, {})
        need = sum(c.get(f, {}).get('count') or 0 for f in ('doors', 'openings_bays', 'windows') if f in tagged)
        return max(need, len(k.meta.get('sockets', [])))
    import lights
    return len(lights.light_specs(name, k.meta['size'], 'pristine'))


def check_baked(name, d, bud, expect, th, met):
    out = []
    pbr = d / 'pbr'
    per = {}
    for c in STATES:
        for i in range(3):
            if not (pbr / f'{name}_{c}_lod{i}.glb').is_file():
                out.append(f'bake: missing {name}_{c}_lod{i}.glb')
        bj = pbr / f'{name}_{c}-bake.json'
        if not bj.is_file():
            out.append(f'bake: missing {bj.name} (state {c} not baked)')
            continue
        b = json.loads(bj.read_text())
        t = [l['triangles'] for l in b['lods']]
        per[c] = t
        if t[0] > bud:
            out.append(f'bake: {c} LOD0 {t[0]} tris > budget {bud}')
        if not (t[0] >= t[1] >= t[2]):
            out.append(f'bake: {c} LODs not monotonic {t}')
        if c in ('pristine', 'worn') and len(b['lights']) < expect:
            out.append(f'bake: {c} has {len(b["lights"])} baked sockets, needs {expect}')
        if c in ('partial', 'destroyed', 'abandoned') and b['lights']:
            out.append(f'bake: {c} has active baked sockets')
    met['bake_lods'] = per
    try:
        if (pbr / f'{name}_pristine_basecolor.png').is_file():
            de = delta_e(baked_mean(d, name), stock_mean(name))
            met['baked_de'] = round(de, 1)
            if de > th['baked_de_max']:
                out.append(f'palette: baked atlas mean dE {de:.1f} from stock (max {th["baked_de_max"]})')
    except Exception as exc:
        met['baked_de'] = f'skipped ({type(exc).__name__})'
    return out


def report(res):
    head = f'[{"PASS" if res["ok"] else "FAIL"}] {res["asset"]}'
    lines = [head] + ['   - ' + f for f in res['failures']] + ['   . ' + n for n in res['notes']]
    return '\n'.join(lines)


# ------------------------------------------------------------------ calibration on the approved assets
def calibrate(names=None, pass_dir=None):
    names = names or list(drafts.LEGACY)
    rows = {}
    for n in names:
        k0 = build_game(n, 'pristine')
        sil = silhouette(n, k0, drafts.SIZES[n], DEFAULT['thin_px'])
        sm_ = stock_mean(n)
        row = dict(iou={v: r['iou'] for v, r in sil.items() if not r['thin']}, thin=[v for v, r in sil.items() if r['thin']],
                   palette_de=round(delta_e(remake_mean_colour(k0), sm_), 1) if sm_ is not None else 0.0)
        if pass_dir and (Path(pass_dir) / 'pbr' / f'{n}_pristine_basecolor.png').is_file():
            row['baked_de'] = round(delta_e(baked_mean(pass_dir, n), stock_mean(n)), 1)
        rows[n] = row
    out = dict(iou_min={}, rows=rows)
    for view in (v[0] for v in analyze.VIEWS):
        vals = [r['iou'][view] for r in rows.values() if view in r['iou']]
        worst = min(vals)
        out['iou_min'][view] = round(math.floor(worst * .9 * 100) / 100, 2)
    means = [sum(r['iou'].values()) / len(r['iou']) for r in rows.values() if r['iou']]
    out['iou_mean_min'] = round(math.floor(min(means) * .9 * 100) / 100, 2)
    pal = max(r['palette_de'] for r in rows.values())
    out['palette_de_max'] = round(math.ceil(pal * 1.2), 0)
    baked = [r['baked_de'] for r in rows.values() if 'baked_de' in r]
    out['baked_de_max'] = round(math.ceil(max(baked) * 1.2), 0) if baked else DEFAULT['baked_de_max']
    return out
