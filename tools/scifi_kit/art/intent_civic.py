"""Civilian family, group 3 (owner 2026-10-25): civic buildings and the Liao palace / Steiner government sets.

Owner style for civilian: clean and restrained, glass-heavy, stock silhouette first; no external stairs, ladders,
catwalks, railings, pipe runs or hazard bands; people enter through ground-floor doors.

Built from the stock massing (stock_massing.blocks: the stock height field as stacked plan rectangles), so the plan
and the stepped silhouette follow the stock; each mass is dressed in one of three stock families:
  n  neutral civic (hub, lobby, centerlobby): white cladding, dark plinth course, ribbon glazing on every storey,
     flat roof with a parapet and a little tidy plant;
  l  Liao palace and residences: green-grey stone base, upper storeys with red-framed window bands, tan hipped tile
     roofs on the top masses;
  s  Steiner government: grey ashlar, tall framed windows with thin mullions, slate mansard roofs on the top masses.
Entrances: a glazed double door with a canopy at the centre of the longest ground-level boundary face (and the
opposite face on long buildings).
"""
import functools
import json
import math
from pathlib import Path

import intent as I
import intent_city as C
import intent_military as M

count = I.count
STOREY = 4.0
MIN_AREA = 3.0            # plan m2: smaller massing rectangles are trim, merged into the neighbours by the plan check


@functools.lru_cache(maxsize=None)
def _raw_masses(name):
    """Stock massing at the height step that covers the most plan (sloped roofs split into one-cell strips at a fine
    step and are lost as slivers; a coarser step keeps them as masses). Cached: every build reads it."""
    import stock_massing
    if SPECS.get(name, {}).get('masses'):            # hand masses where the stock is an open frame
        return tuple((tuple(b), float(h)) for b, h in SPECS[name]['masses'])
    best = None
    for step in (.5, 1.5, 3.0, 6.0):
        r = stock_massing.blocks(name, step=step, min_h=.35)        # low terraces and podium steps count as masses
        bl = [b for b in r['blocks'] if (b['box'][2] - b['box'][0]) * (b['box'][3] - b['box'][1]) >= MIN_AREA]
        area = sum((b['box'][2] - b['box'][0]) * (b['box'][3] - b['box'][1]) for b in bl)
        if best is None or area > best[0] * 1.05:
            best = (area, bl)
    return tuple((tuple(b['box']), float(b['h'])) for b in best[1])


def _masses(name):
    return [dict(box=box, h=h) for box, h in _raw_masses(name)]


def _bounds(ms):
    return (min(m['box'][0] for m in ms), min(m['box'][1] for m in ms), max(m['box'][2] for m in ms), max(m['box'][3] for m in ms))


def _exposed(m, s, ms):
    """Is face s of mass m open (not buried against a mass at least as tall)? Tested at three points along it."""
    x0, y0, x1, y1 = m['box']
    pts = {'s': [(x, y0 - .3) for x in (x0 * .75 + x1 * .25, (x0 + x1) / 2, x0 * .25 + x1 * .75)],
           'n': [(x, y1 + .3) for x in (x0 * .75 + x1 * .25, (x0 + x1) / 2, x0 * .25 + x1 * .75)],
           'w': [(x0 - .3, y) for y in (y0 * .75 + y1 * .25, (y0 + y1) / 2, y0 * .25 + y1 * .75)],
           'e': [(x1 + .3, y) for y in (y0 * .75 + y1 * .25, (y0 + y1) / 2, y0 * .25 + y1 * .75)]}[s]
    hits = 0
    for px, py in pts:
        for o in ms:
            if o is m:
                continue
            a0, b0, a1, b1 = o['box']
            if a0 <= px <= a1 and b0 <= py <= b1 and o['h'] >= m['h'] - .5:
                hits += 1
                break
    return hits < 2


def _roof(k, m, fam, top):
    """Body top z for mass m and its roof: hipped tile (l) / slate mansard (s) on the top masses, else flat+parapet."""
    x0, y0, x1, y1 = m['box']
    h = m['h']
    short = min(x1 - x0, y1 - y0)
    if fam in 'ls' and top and h > 6 and short > 4:
        rh = min(short * (.32 if fam == 'l' else .22), 5.0 if fam == 'l' else 3.2)
        zb = h - rh
        ov = .25 if fam == 'l' else .05                 # Liao eaves overhang; Steiner mansard sits on the wall
        ins = short / 2 * (.8 if fam == 'l' else .45)
        M.loft(k, [M.rring(x0 - ov, y0 - ov, x1 + ov, y1 + ov, zb, .05), M.rring(x0 - ov, y0 - ov, x1 + ov, y1 + ov, zb + .22, .05),
                   M.rring(x0 + ins, y0 + ins, x1 - ins, y1 - ins, h, .05)], 'paint.roof', 'roof')
        k.box(((x0 + x1) / 2, (y0 + y1) / 2, zb + .05), (x1 - x0 + .1, y1 - y0 + .1, .1), 'paint.trim', 'frame')    # eave line
        return zb
    k.box(((x0 + x1) / 2, (y0 + y1) / 2, h - .2), (x1 - x0 + .16, y1 - y0 + .16, .4), 'paint.frame', 'frame', ch=.04)   # parapet coping
    k.box(((x0 + x1) / 2, (y0 + y1) / 2, h - .36), (x1 - x0 - .4, y1 - y0 - .4, .04), 'gravel', 'roof', walk=True)
    return h - .4


def _face(k, L, zb, fam, door=False, clear=0.0):
    """Dress one exposed face (wall frame: centred, outward -Y, z from ground) of length L and body height zb."""
    if L < 2.0 or zb < 1.2:
        return
    storeys = max(1, int(zb // STOREY))
    margin = min(1.2, L * .1) + clear
    if L - 2 * margin < 2.0:
        return
    k.slab(-L / 2 + clear, L / 2 - clear, 0, .6, -.1, .02, 'paint.plinth', 'frame')    # dark plinth course
    for i in range(storeys):
        z0 = i * STOREY
        if fam == 'n':                                                             # ribbon glazing, full width
            lo, hi = z0 + (1.0 if i == 0 else .9), z0 + STOREY - .6
            if hi - lo > .8:
                if door and i == 0:
                    C.pane(k, -L / 2 + margin, -2.8, lo, hi, 1.4)
                    C.pane(k, 2.8, L / 2 - margin, lo, hi, 1.4)
                else:
                    C.pane(k, -L / 2 + margin, L / 2 - margin, lo, hi, 1.4)
            k.slab(-L / 2 + clear, L / 2 - clear, z0 + STOREY - .5, z0 + STOREY - .2, -.1, .02, 'paint.second', 'frame')   # storey band
        else:                                                                       # framed windows in bays
            n = max(1, int((L - 2 * margin) // 3.2))
            bay = (L - 2 * margin) / n
            ww = bay * (.62 if fam == 's' else .7)
            lo, hi = z0 + (1.1 if i == 0 else .9), z0 + STOREY - (.5 if fam == 's' else .9)
            for j in range(n):
                xc = -L / 2 + margin + (j + .5) * bay
                if door and i == 0 and abs(xc) < 2.8:
                    continue
                if hi - lo > .8:
                    C.pane(k, xc - ww / 2, xc + ww / 2, lo, hi, .9 if fam == 's' else 1.1, 'paint.frame')
                    count(k, 'windows')
            if fam == 'l':
                k.slab(-L / 2 + clear, L / 2 - clear, z0 + STOREY - .45, z0 + STOREY - .15, -.12, .02, 'paint.second', 'frame')
    if door:                                                                       # glazed double entrance + canopy
        C.pane(k, -2.4, 2.4, 2.6, 3.4, 1.2)
        for xl in (-1.15, 1.15):
            k.slab(xl - 1.05, xl + 1.05, .02, 2.5, -.16, -.1, 'paint.frame', 'frame')
            k.slab(xl - .95, xl + .95, .1, 2.4, -.18, -.15, 'glass', 'glass', pane=True)
        k.slab(-3.0, 3.0, 3.55, 3.7, -.3, .02, 'paint.frame', 'frame')               # slim glass-edged awning
        count(k, 'doors')


def _snap(name, ms, px=10.0, cover=.6):
    """Move every exposed face of every mass onto the stock plan outline: the outermost line (within -0.6..+1.2 m of
    the massing edge) along which the stock plan covers >= `cover` of the face length."""
    import numpy as np
    import topdown
    import footprint
    S = topdown.stock_tris(name)
    P = S.reshape(-1, 3)
    box = (P[:, 0].min() - 2, P[:, 1].min() - 2, P[:, 0].max() + 2, P[:, 1].max() + 2)
    mask = footprint._plan(S, box, z=.3, px=px)
    X0, Y0, X1, Y1 = box

    def frac_x(y, a, b):                 # coverage along x in [a, b] at height y
        r = int((Y1 - y) * px)
        c0, c1 = int((a - X0) * px), int((b - X0) * px)
        if not (0 <= r < mask.shape[0]) or c1 <= c0:
            return 0.0
        return float(mask[r, max(0, c0):min(mask.shape[1], c1)].mean())

    def frac_y(x, a, b):
        c = int((x - X0) * px)
        r0, r1 = int((Y1 - b) * px), int((Y1 - a) * px)
        if not (0 <= c < mask.shape[1]) or r1 <= r0:
            return 0.0
        return float(mask[max(0, r0):min(mask.shape[0], r1), c].mean())
    steps = np.arange(-.6, 1.21, .05)
    for m in ms:
        x0, y0, x1, y1 = m['box']
        e = {s: _exposed(m, s, ms) for s in 'snwe'}
        ix0, ix1, iy0, iy1 = x0 + .1 * (x1 - x0), x1 - .1 * (x1 - x0), y0 + .1 * (y1 - y0), y1 - .1 * (y1 - y0)
        if e['s']:
            ok = [y0 + d for d in steps if frac_x(y0 + d + .05, ix0, ix1) >= cover]
            y0 = min(ok) if ok else y0
        if e['n']:
            ok = [y1 - d for d in steps if frac_x(y1 - d - .05, ix0, ix1) >= cover]
            y1 = max(ok) if ok else y1
        if e['w']:
            ok = [x0 + d for d in steps if frac_y(x0 + d + .05, iy0, iy1) >= cover]
            x0 = min(ok) if ok else x0
        if e['e']:
            ok = [x1 - d for d in steps if frac_y(x1 - d - .05, iy0, iy1) >= cover]
            x1 = max(ok) if ok else x1
        if x1 - x0 > .5 and y1 - y0 > .5:
            m['box'] = (x0, y0, x1, y1)
        cut = []
        lim = min(x1 - x0, y1 - y0) * .35
        for cx, cy, sx, sy in ((x0, y0, 1, 1), (x1, y0, -1, 1), (x1, y1, -1, -1), (x0, y1, 1, -1)):
            d = 0.0
            while d < lim:
                px_, py_ = cx + sx * (d + .15), cy + sy * (d + .15)
                r, c = int((Y1 - py_) * px), int((px_ - X0) * px)
                if 0 <= r < mask.shape[0] and 0 <= c < mask.shape[1] and mask[r, c]:
                    break
                d += .1
            cut.append(round(2 * d, 2) if d >= .3 else 0.0)        # chamfer leg along each side
        m['cut'] = cut


CACHE = Path(__file__).resolve().parent / 'assets' / 'civic_masses.json'


def final_masses(name, rebuild=False):
    """Snapped, chamfered masses for one asset. Computed on the CPU side (stock massing and the plan snap need PIL and
    scipy, which Blender's Python lacks) and cached in assets/civic_masses.json, which the bake reads."""
    data = json.loads(CACHE.read_text()) if CACHE.exists() else {}
    if name in data and not rebuild:
        return [dict(box=tuple(m['box']), h=m['h'], cut=m.get('cut', [0, 0, 0, 0])) for m in data[name]]
    ms = _masses(name)
    _snap(name, ms)                   # massing is quantised to 0.6 m cells: exposed faces go onto the stock plan line
    data[name] = [dict(box=[round(float(v), 3) for v in m['box']], h=round(float(m['h']), 3),
                       cut=[float(c) for c in m.get('cut', [0, 0, 0, 0])]) for m in ms]
    CACHE.write_text(json.dumps(data, indent=1, sort_keys=True))
    return ms


def civic(k):
    name = k.name
    fam = SPECS[name]['fam']
    ms = final_masses(name)
    hmax = max(m['h'] for m in ms)
    B = _bounds(ms)
    # entrance: the longest ground-level face on the building boundary (and the opposite boundary face if long)
    cands = []
    for m in ms:
        x0, y0, x1, y1 = m['box']
        for s, onb, L in (('s', abs(y0 - B[1]) < 1.0, x1 - x0), ('n', abs(y1 - B[3]) < 1.0, x1 - x0),
                          ('w', abs(x0 - B[0]) < 1.0, y1 - y0), ('e', abs(x1 - B[2]) < 1.0, y1 - y0)):
            if onb and m['h'] >= 4.0 and L >= 7.0:
                cands.append((L, id(m), s))
    cands.sort(reverse=True)
    doors = set()
    if cands:
        doors.add((cands[0][1], cands[0][2]))
        opp = {'s': 'n', 'n': 's', 'w': 'e', 'e': 'w'}[cands[0][2]]
        if max(B[2] - B[0], B[3] - B[1]) > 30:
            for L, mid, s in cands:
                if s == opp:
                    doors.add((mid, s))
                    break
    for m in sorted(ms, key=lambda m: m['h']):
        x0, y0, x1, y1 = m['box']
        top = m['h'] >= hmax * .72 or not any(o is not m and o['h'] > m['h'] + 1.0 and o['box'][0] < x1 and o['box'][2] > x0 and
                                              o['box'][1] < y1 and o['box'][3] > y0 for o in ms)
        if m['h'] < 2.6:                                                             # terraces and podium steps
            k.box(((x0 + x1) / 2, (y0 + y1) / 2, m['h'] / 2), (x1 - x0, y1 - y0, m['h']), 'paint.second', 'mass', ch=.04, walk=True)
            continue
        zb = _roof(k, m, fam, top)
        cut = m.get('cut', [0, 0, 0, 0])
        if any(cut):
            c0, c1, c2, c3 = cut
            poly = [(x0 + c0, y0), (x1 - c1, y0), (x1, y0 + c1), (x1, y1 - c2), (x1 - c2, y1), (x0 + c3, y1), (x0, y1 - c3), (x0, y0 + c0)]
            dedup = [q for i, q in enumerate(poly) if abs(q[0] - poly[i - 1][0]) + abs(q[1] - poly[i - 1][1]) > 1e-6]
            k.prism(dedup, 0.0, zb, 'paint.main', 'mass')
        else:
            k.box(((x0 + x1) / 2, (y0 + y1) / 2, zb / 2), (x1 - x0, y1 - y0, zb), 'paint.main', 'mass', ch=.04)
        count(k, 'panels_sections')
        for s in 'snwe':
            if not _exposed(m, s, ms):
                continue
            o, yaw, L = M.frame_of(m['box'], s)
            with k.at((o[0], o[1], 0), yaw=yaw):
                ends = {'s': (cut[0], cut[1]), 'e': (cut[1], cut[2]), 'n': (cut[2], cut[3]), 'w': (cut[3], cut[0])}[s]
                _face(k, L, zb, fam, door=(id(m), s) in doors, clear=max(ends))
    if fam == 'n':                                                                   # tidy rooftop plant on the tallest mass
        m = max(ms, key=lambda m: m['h'])
        x0, y0, x1, y1 = m['box']
        k.box(((x0 + x1) / 2, (y0 + y1) / 2, m['h'] + .5), (min(3.0, (x1 - x0) * .4), min(2.0, (y1 - y0) * .3), 1.0), 'paint.second', 'frame', ch=.06)


# ------------------------------------------------------------------ group 4: bridge, promenades, flags, barricade
def bridge(k):
    """Road bridge tile on the stock layout (32 m along x, tiles along x): a thin asphalt deck at road level and two
    battered concrete barrier walls on the long edges (stock: 2.1 m base, 0.9 m top, 1.86 m tall), a coping on each
    and a dashed centre line whose 8 m period divides the 32 m tile."""
    k.box((0, 0, .02), (32.0, 22.08, .04), 'paint.roof', 'roof', walk=True)               # deck between the barriers
    for sy in (-1, 1):
        prof = [(11.04, 0.0), (13.14, 0.0), (12.54, 1.86), (11.64, 1.86)]
        poly = [(sy * a, z) for a, z in prof]
        if sy < 0:
            poly = poly[::-1]
        # extrude the barrier section along x: polygon in (y, z) -> use a loft between the two end sections
        ring = lambda x: [(x, y, z) for y, z in poly]        # noqa: E731
        M.loft(k, [ring(-16.0), ring(16.0)], 'ferro', 'mass')
        k.box((0, sy * 12.09, 1.9), (32.0, 1.0, .08), 'paint.frame', 'frame')                  # coping
        k.box((0, sy * 10.9, .06), (32.0, .15, .04), 'paint.frame', 'frame')                   # edge line
        count(k, 'panels_sections')
    for x in (-14.0, -6.0, 2.0, 10.0):                                                         # 4 m dashes, 8 m period
        k.box((x, 0, .05), (4.0, .15, .02), 'paint.frame', 'frame')


def _colonnade(k, x0, x1, y0, y1, h, cols):
    """Stone colonnade on the stock promenade line: plinth, round columns with bases and capitals, entablature."""
    k.box(((x0 + x1) / 2, (y0 + y1) / 2, .3), (x1 - x0, y1 - y0, .6), 'paint.second', 'mass', ch=.04, walk=True)
    for (cx, cy) in cols:
        k.box((cx, cy, .85), (1.3, 1.3, .5), 'paint.frame', 'frame', ch=.05)
        k.cyl((cx, cy, (1.1 + h - 1.6) / 2), .45, h - 1.6 - 1.1, 'paint.main', 'mass', sides=16)
        k.box((cx, cy, h - 1.45), (1.2, 1.2, .3), 'paint.frame', 'frame', ch=.05)
        count(k, 'legs_supports')
    k.box(((x0 + x1) / 2, (y0 + y1) / 2, h - .65), (x1 - x0, y1 - y0, 1.3), 'paint.main', 'mass', ch=.04)
    k.box(((x0 + x1) / 2, (y0 + y1) / 2, h - .05), (x1 - x0 + .2, y1 - y0 + .2, .1), 'paint.frame', 'frame')


def _portal(k, xc, y0, y1, h, xf):
    """Glazed portal block between columns (face at x = xf, outward -x): dark frame, glazed double doors, transom."""
    k.box((xc, (y0 + y1) / 2, h / 2), (abs(xf - xc) * 2, y1 - y0, h), 'paint.main', 'mass', ch=.04)
    yc = (y0 + y1) / 2
    with k.at((xf, yc, 0), yaw=-math.pi / 2):
        C.pane(k, -1.6, 1.6, .2, 2.6, 1.6)
        C.pane(k, -1.6, 1.6, 2.8, 3.8, 1.6)
    count(k, 'doors')


def s_promenade(k):
    """Steiner promenade piece on the stock plan: a stone gallery block on the east, a colonnade of paired round
    columns on the west carrying the entablature, and a glazed portal in the middle bay."""
    _colonnade(k, -5.29, -1.6, -15.97, 15.97, 5.17, [(-3.9, y) for y in (-14.4, -9.6, -3.6, 3.6, 9.6, 14.4)])
    k.box((1.9, 0, 3.59), (6.0, 31.94, 7.18), 'paint.main', 'mass', ch=.04)            # gallery block
    k.box((1.9, 0, 7.13), (6.2, 32.1, .1), 'paint.frame', 'frame')
    for y in (-11.0, 0.0, 11.0):                                                         # glazed bays on the gallery
        with k.at((-1.09, y, 0), yaw=-math.pi / 2):
            C.pane(k, -2.4, 2.4, 1.0, 5.6, 1.2)
            count(k, 'windows')
    _portal(k, -1.6, -1.8, 1.8, 4.6, -2.8)


def s_promenadecap(k):
    """Promenade end cap: the gallery block between two taller stone pavilions with glazed fronts, and the low
    kerb on the east edge (stock)."""
    k.box((.16, 0, 3.59), (10.2, 32.08, 7.18), 'paint.main', 'mass', ch=.04)
    for sx in (-1, 1):
        x0, x1 = (-8.54, -5.0) if sx < 0 else (5.26, 8.26)
        for ya, yb in ((-16.04, .16), (.76, 4.96), (5.56, 7.36), (7.96, 14.56)):
            k.box(((x0 + x1) / 2, (ya + yb) / 2, 4.23), (x1 - x0, yb - ya, 8.46), 'paint.main', 'mass', ch=.04)
            k.box(((x0 + x1) / 2, (ya + yb) / 2, 8.4), (x1 - x0 + .2, yb - ya + .2, .12), 'paint.frame', 'frame')
        xf = x0 if sx < 0 else x1
        with k.at((xf, -8.0, 0), yaw=-math.pi / 2 if sx < 0 else math.pi / 2):
            C.pane(k, -6.5, 6.5, 1.0, 7.4, 1.4)
            count(k, 'windows')
    k.box((8.86, .16, .31), (1.2, 32.4, .62), 'paint.second', 'mass', ch=.04, walk=True)
    _portal(k, -5.0, -1.8, 1.8, 4.6, -6.2)


def _flag(k, house):
    """Flagpole on the stock pole line (x = 1.2) with the House flag on the stock flag plane (10.5 to 12.2 m)."""
    k.cyl((1.19, 0, .1), .08, .2, 'paint.second', 'frame', sides=12)
    k.cyl((1.19, 0, 6.2), .06, 12.1, 'paint.steel', 'frame', sides=10)
    k.cyl((1.19, 0, 12.27), .08, .06, 'paint.frame', 'frame', sides=10)
    k.box((-.05, 0, 11.35), (2.36, .03, 1.7), 'paint.emblem_field', 'frame')            # flag cloth
    n0 = len(k.pieces)
    with k.at((-.05, -.016, 11.35), pitch=math.pi / 2):
        M.house_emblem(k, house, .62, depth=.02)
    for pc in k.pieces[n0:]:                                  # printed on the cloth: relief flattened to 2 cm
        pc['v'] = [(x, -.016 + (y + .016) * .1, z) for x, y, z in pc['v']]
    count(k, 'antennas_dishes')


def d_flag(k):
    _flag(k, 'd')


def s_flag(k):
    _flag(k, 's')


def smallbarricade(k):
    """Street barricade after the stock: three leaning X-board panels side by side (panel foot x 0.45, top x 0.64 at
    2.27 m), each held by a diagonal prop to the ground behind (stock x -1.24), crossed hazard boards on the panel."""
    for yc in (-2.07, 0.05, 2.12):
        for sgn in (-1, 1):
            k.beam((.46, yc - .85 * sgn, .1), (.62, yc + .85 * sgn, 2.2), .16, 'hazard', 'frame', h=.05)
        k.beam((.45, yc - .95, .02), (.64, yc - .95, 2.27), .1, 'paint.second', 'frame')      # panel stiles
        k.beam((.45, yc + .95, .02), (.64, yc + .95, 2.27), .1, 'paint.second', 'frame')
        k.box((.63, yc, 2.22), (.1, 2.0, .1), 'paint.second', 'frame')                       # top rail
        k.beam((-1.2, yc, .04), (1.2, yc, 1.9), .12, 'paint.second', 'frame')                 # diagonal prop through the panel (stock)
        k.box((-.4, yc, .04), (1.8, .12, .08), 'paint.second', 'frame')                       # foot rail
        count(k, 'panels_sections')

_DESC = {'n': 'neutral civic: white cladding, dark plinth course, ribbon glazing on every storey, flat roof with parapet',
         'l': 'Liao: green-grey stone, red-framed window bands, tan hipped tile roofs on the top masses',
         's': 'Steiner: grey ashlar, tall framed windows with thin mullions, slate mansard roofs on the top masses'}
SPECS = {}
for _n, _f, _what in (
        ('hub', 'n', 'Civic hub: a stepped square hall with a central tower'),
        ('lobby', 'n', 'Lobby: a long glazed hall stepping down to the rear'),
        ('centerlobby', 'n', 'Centre lobby: a connector hall with side wings'),
        ('l_palacetwr', 'l', 'Palace tower block'), ('l_palacewing', 'l', 'Palace wing with tall corner blocks'),
        ('l_palacecore', 'l', 'Palace core: the tall stepped centre block'), ('l_residencecore', 'l', 'Residence core'),
        ('l_residenceleft', 'l', 'Residence left wing with its tower'), ('l_residenceright', 'l', 'Residence right wing with its tower'),
        ('l_guardhouse', 'l', 'Guard house: long walled block with twin towers'),
        ('s_adminquarter', 's', 'Admin quarter'), ('s_admin_core', 's', 'Admin core: the long stepped centre block'),
        ('s_admin_entry', 's', 'Admin entry hall'), ('s_admin_wing', 's', 'Admin wing'), ('s_quetragon', 's', 'Quetragon hall'),
        ('s_main', 's', 'Government main block'), ('s_main_stairs', 's', 'Government grand terrace (stepped podium, no external stair kit)'),
        ('s_main_leftwing', 's', 'Government left wing'), ('s_main_rightwing', 's', 'Government right wing'),
        ('s_gov1', 's', 'Government building 1'), ('s_gov2', 's', 'Government building 2'), ('s_govresidence', 's', 'Governor residence')):
    SPECS[_n] = dict(fn=civic, fam=_f, palette=f'intent_civic_{_f}', cls='large', counts=dict(),
                     # stair-stepped slivers along diagonal / stepped stock faces (0.6 m massing); overhang is visual
                     # only since the stock-blocking lane landed, so the plan IoU (>= 0.89 on all) carries the rule
                     tol=dict(protrusion=1.8),
                     intent=f'{_what}, on the stock massing ({_DESC[_f]}); glazed ground-floor entrance with a canopy.')
# the stock admin wing is an open frame (thin walls, one-cell roof bars over courtyards): from above it reads as one
# long hall with a raised gabled centre, which is how it is built
for _n, _fn, _pal, _cls, _txt in (
        ('bridge', bridge, 'intent_civic_s', 'large', 'Road bridge deck tile on the exact stock slab: concrete deck, asphalt wearing course, drip bands; plain edges so it tiles.'),
        ('s_promenade', s_promenade, 'intent_civic_s', 'large', 'Steiner promenade: stone gallery block, a colonnade of round columns under an entablature, a glazed portal in the middle bay.'),
        ('s_promenadecap', s_promenadecap, 'intent_civic_s', 'large', 'Promenade end cap: gallery block between two taller stone pavilions with glazed fronts, low east kerb.'),
        ('d_flag', d_flag, 'intent_flag_d', 'small', 'Davion flag: steel pole on a base, House Davion flag with the sunburst on the stock flag plane.'),
        ('s_flag', s_flag, 'intent_flag_s', 'small', 'Steiner flag: steel pole on a base, House Steiner flag with the fist on the stock flag plane.'),
        ('smallbarricade', smallbarricade, 'intent_civic_s', 'small', 'Street barricade: three crossed hazard boards on low feet (stock X boards).')):
    SPECS[_n] = dict(fn=_fn, palette=_pal, cls=_cls, counts=dict(), intent=_txt)
SPECS['s_admin_wing']['masses'] = [((-7.12, -33.41, 7.28, 33.79), 7.96), ((-4.12, -18.41, 4.28, 16.99), 10.3)]
