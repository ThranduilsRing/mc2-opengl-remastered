"""Shell builder: the GROSS SHAPE of box buildings (and non-round tanks) comes only from assets/<asset>.shape.json, which shape.py
derives from the decoded stock mesh. Params refine detail (skin, bays, openings, palette), never the outline, roof or rooftop features.

  footprint  rect / rectilinear (L, T, U: walls on every boundary edge, a roof deck per cell) / convex n-gon / circle
  roof       flat deck + coping, extruded cross-section (gable, mono, sawtooth, stepped, A-frame) or inset rings (hip, pyramid, dome)
  walls      every outline edge, up to the roof line along it (gable ends follow the profile), batter from wall_profile
  features   residual blobs at their measured plan position, base, shoulder and peak; painted roof fans/vents as low housings

Construction order (ART-BAR 1): footing -> columns -> girts -> lapped removable skin -> openings -> roof frame + sheets -> trim -> features.
Pure Python (Blender imports this through drafts -> archetypes).
"""
import json
import math
from pathlib import Path

import geo
from geo import subtract_rect

HERE = Path(__file__).resolve().parent
ASSETS = HERE / 'assets'
_CACHE = {}
SIDE_N = dict(front=(0.0, -1.0), right=(1.0, 0.0), rear=(0.0, 1.0), left=(-1.0, 0.0))


def load(name):
    path = ASSETS / f'{name}.shape.json'
    if not path.is_file():
        return None
    key = (name, path.stat().st_mtime_ns)
    if key not in _CACHE:
        _CACHE[key] = json.loads(path.read_text())
    return _CACHE[key]


def unsupported(S, arch='box'):
    """The builder capability this stock shape needs and the shell builder lacks, or None."""
    if S is None:
        return 'shape: no shape.json (run `run.py analyze <asset>`)'
    fp, rf = S['footprint'], S['roof']
    if fp['kind'] == 'irregular':
        return 'footprint: irregular plan outline (not a rectangle, L/T/U cells, convex n-gon or circle)'
    if rf['model'] == 'extrude' and fp['kind'] != 'rect':
        return f'roof: {rf["form"]} cross-section swept over a {fp["kind"]} plan (extrusion is clipped to rectangles only)'
    if rf['model'] == 'inset' and fp['kind'] == 'rectilinear':
        return f'roof: {rf["form"]} roof over an L/T/U plan (inset rings need a convex plan)'
    best = max(rf.get('inliers', {}).values() or [1.0])
    if best < .75:
        return f'roof: no single roof model fits the stock (best {best:.0%} of the plan): compound roof (crossing wings, intersecting ridges)'
    return None


def _interp(knots, t):
    if not knots:
        return 1.0
    if t <= knots[0][0]:
        return knots[0][1]
    for (t0, v0), (t1, v1) in zip(knots, knots[1:]):
        if t <= t1:
            return v0 + (v1 - v0) * (t - t0) / max(t1 - t0, 1e-9)
    return knots[-1][1]


def outline(S):
    fp = S['footprint']
    if fp['kind'] == 'circle':
        cx, cy = fp['center']
        ax, ay = fp.get('radii', [fp['R'], fp['R']])
        n = 16
        return [(cx + ax * math.cos(math.tau * i / n), cy + ay * math.sin(math.tau * i / n)) for i in range(n)]
    if fp['kind'] == 'polygon':
        return [tuple(p) for p in fp['poly']]
    return None


def edges_of(S):
    fp = S['footprint']
    if fp['kind'] in ('rect', 'rectilinear', 'polygon'):
        return [((e[0], e[1]), (e[2], e[3])) for e in fp['edges']]
    pts = outline(S)
    return [(pts[i], pts[(i + 1) % len(pts)]) for i in range(len(pts))]


def overhang_t(S):
    """Flat roofs: the wall-profile height fraction where the walls stop and a wider roof slab starts (the stock overhang), else 1."""
    if S['roof']['model'] != 'flat':
        return 1.0
    wp = S['wall_profile']
    ts = []
    for kn in (wp['x'], wp['y']):
        lo = min(v for _, v in kn)
        if kn[-1][1] - lo > .03:
            ts.append(max(t for t, v in kn if v <= lo + .01))
    return min(ts) if ts else 1.0


def deck_thk(S):
    return max(.14, (1 - overhang_t(S)) * S['roof']['eave']) if S['roof']['model'] == 'flat' else 0.0


def roof_top(S, x, y):
    """Height of the roof line over plan point (x, y) for the wall tops (extrude: the profile; else the eave)."""
    rf = S['roof']
    if rf['model'] == 'extrude':
        c = x if rf['axis'] == 'y' else y
        kn = rf['profile']
        return _interp(kn, c)
    return rf['eave'] - deck_thk(S)


def _edge_tops(S, a, b, L):
    """[(u, z)] breakpoints of the roof line along an edge (u from -L/2 to L/2)."""
    rf = S['roof']
    pt = lambda u: (a[0] + (b[0] - a[0]) * (u / L + .5), a[1] + (b[1] - a[1]) * (u / L + .5))  # noqa: E731
    us = [-L / 2, L / 2]
    if rf['model'] == 'extrude':
        dx, dy = (b[0] - a[0]) / L, (b[1] - a[1]) / L
        along_c = abs(dx) > .7 if rf['axis'] == 'y' else abs(dy) > .7
        if along_c:
            ax_ = 0 if rf['axis'] == 'y' else 1
            if rf.get('end_profile'):         # gable-end walls close a recessed roof (parapet ends)
                kn_end = rf['end_profile']
                pt_ = pt
                us_ = [-L / 2, L / 2]
                dc_ = dx if ax_ == 0 else dy
                for c, _ in kn_end:
                    u = (c - a[ax_]) / dc_ - L / 2 if abs(dc_) > 1e-9 else None
                    if u is not None and -L / 2 + .02 < u < L / 2 - .02:
                        us_.append(u)
                for c, _ in rf['profile']:
                    u = (c - a[ax_]) / dc_ - L / 2 if abs(dc_) > 1e-9 else None
                    if u is not None and -L / 2 + .02 < u < L / 2 - .02:
                        us_.append(u)
                us_ = sorted(set(round(u, 4) for u in us_))
                return [(u, max(roof_top(S, *pt_(u)), _interp(kn_end, pt_(u)[ax_]))) for u in us_]
            dc = dx if ax_ == 0 else dy
            ca = a[ax_]
            for c, _ in rf['profile']:
                u = (c - ca) / dc - L / 2 if abs(dc) > 1e-9 else None
                if u is not None and -L / 2 + .02 < u < L / 2 - .02:
                    us.append(u)
    us = sorted(set(round(u, 4) for u in us))
    return [(u, roof_top(S, *pt(u))) for u in us]


def _top_at(tops, u):
    return _interp([list(t) for t in tops], u)


def _map_features(feats, edges, w, d):
    """Params features (side + u on the bbox face) -> (edge index, u along that edge)."""
    out = []
    for kind, fl in feats.items():
        for idx, f in enumerate(fl):
            n = SIDE_N[f['side']]
            u = f['at']
            p = {'front': (u, -d / 2), 'right': (w / 2, u), 'rear': (-u, d / 2), 'left': (-w / 2, -u)}[f['side']]
            best, bi, bu = 1e9, None, 0.0
            for i, (a, b) in enumerate(edges):
                L = math.dist(a, b)
                if L < .5:
                    continue
                dx, dy = (b[0] - a[0]) / L, (b[1] - a[1]) / L
                on = (dy, -dx)
                if on[0] * n[0] + on[1] * n[1] < .5:
                    continue
                t0 = (p[0] - a[0]) * dx + (p[1] - a[1]) * dy
                t = max(0.0, min(L, t0))
                perp = abs((p[0] - a[0]) * on[0] + (p[1] - a[1]) * on[1])
                dist = 10 * abs(t0 - t) + perp           # the edge whose span holds the opening first, then the outermost
                if dist < best:
                    best, bi, bu = dist, i, t - L / 2
            if bi is not None:
                out.append((kind, idx, f, bi, bu))
    return out


def build(k, p, S):
    from assemble import infantry_door_parts, rollup
    import archetypes as A
    kit = A.kit
    C = A.C
    w, d, h = p['size']
    g, det = p.get('dims', {}), p.get('detail', {})
    fp, rf = S['footprint'], S['roof']
    corr = g.get('skin', 'plate') == 'corr'
    skin = 'corr.corr' if corr else 'paint.main'
    roofmat = 'corr.corr' if (corr or g.get('roof_mat') == 'corr') else 'paint.roof'
    eave = rf['eave']
    plinth = max(.05, min(.45, eave * .5))
    t = .24
    bay = g.get('bay_m', 4.5)
    edges = edges_of(S)
    wp = S['wall_profile']
    feats = A.plan_features(p, w, d, max(eave, 2.4))
    placed = _map_features(feats, edges, w, d)
    i_walls = len(k.pieces)
    hits_edge = None
    # 1. footing
    if fp['kind'] in ('rect', 'rectilinear'):
        for x0, y0, x1, y1 in fp['cells']:
            k.box(((x0 + x1) / 2, (y0 + y1) / 2, plinth / 2), (x1 - x0 + .2, y1 - y0 + .2, plinth), 'ferro', 'mass', ch=C(.08), top=C(.05), part='footing')
    else:
        k.prism(outline(S), 0.0, plinth, 'ferro', 'mass', top=C(.05), part='footing')
    # 2-4. walls per outline edge: columns, girts, lapped skin cut around the openings, openings, fasteners
    for ei, (a, b) in enumerate(edges):
        L = math.dist(a, b)
        if L < .3:
            continue
        yaw = math.atan2(b[1] - a[1], b[0] - a[0])
        mid = ((a[0] + b[0]) / 2, (a[1] + b[1]) / 2, 0.0)
        tops = _edge_tops(S, a, b, L)
        if max(z for _, z in tops) < plinth + .5:
            continue            # the roof reaches the ground along this edge (A-frame): no wall
        nb = max(1, round(L / bay))
        lines = [-L / 2 + L * i / nb for i in range(nb + 1)]
        mine = [(kind, idx, f, u) for kind, idx, f, e, u in placed if e == ei]
        holes = []
        for kind, idx, f, u in mine:
            fw = min(f['w'], L - 1.0)
            u = max(-L / 2 + fw / 2 + .4, min(L / 2 - fw / 2 - .4, u))
            top_here = min(_top_at(tops, u - fw / 2), _top_at(tops, u + fw / 2))
            fz = f['z'] if kind == 'window' else (0.0 if kind == 'bay' else .45)
            fh = max(.4, min(f['h'], top_here - fz - .35))
            f.update(_u=u, _w=fw, _h=fh, _z=fz)
            holes.append((u - fw / 2 - .05, fz - .05, u + fw / 2 + .05, fz + fh + .05))
        with k.at(mid, yaw=yaw):
            for i, u in enumerate(lines[:-1]):
                if any(hx0 - .2 < u < hx1 + .2 for hx0, _, hx1, _ in holes):
                    continue
                kit.ibeam(k, (u + t / 2, .2, plinth), (u + t / 2, .2, max(plinth + .3, _top_at(tops, u) - .1)), d=.26, w=.16)
            zmin = min(z for _, z in tops)
            girt_z = [plinth + (zmin - plinth) * f_ for f_ in ((1 / 3, 2 / 3) if zmin > 4 else (.5,))] if zmin > plinth + 1.2 else []
            for z in girt_z:
                free = [(-L / 2 + .2, L / 2 - .2)]
                for hx0, hz0, hx1, hz1 in holes:
                    if hz0 - .1 < z < hz1 + .1:
                        free = [seg for f0, f1 in free for seg in ((f0, min(f1, hx0)), (max(f0, hx1), f1)) if seg[1] - seg[0] > .4]
                for f0, f1 in free:
                    kit.cee(k, (f0, .12, z), (f1, .12, z), d=.18, w=.06)
                    kit.bolts(k, (f0 + .2, -.06, z), (f1 - .2, -.06, z), pitch=.6)
            # skin: strips between roof-line breakpoints and bay lines, rows of <= 3 m, gable caps above the lower corner
            br = sorted(set([round(u, 4) for u, _ in tops] + [round(u, 4) for u in lines]))
            for si, (u0, u1) in enumerate(zip(br, br[1:])):
                if u1 - u0 < .05:
                    continue
                z0t, z1t = _top_at(tops, u0), _top_at(tops, u1)
                zlo = min(z0t, z1t)
                if zlo > plinth + .2:
                    rows = max(1, round((zlo - plinth) / 3.0))
                    zr_ = max(eave, .5)
                    cuts = sorted(set([plinth + (zlo - plinth) * r / rows for r in range(rows + 1)] +
                                      [t_ * zr_ for kn_ in (wp['x'], wp['y']) for t_, _ in kn_ if plinth + .25 < t_ * zr_ < zlo - .25]))
                    for r, (za, zb) in enumerate(zip(cuts, cuts[1:])):
                        za -= 0 if r == 0 else .03
                        proud = .014 * ((si + r) % 2)
                        a0 = u0 - (.03 if si else 0)
                        for q in subtract_rect((a0, za, u1, zb), holes):
                            if corr:
                                k.corr_sheet(q[0], q[2], q[1], q[3], -.02 - proud, skin, removable=True)
                            else:
                                k.slab(q[0], q[2], q[1], q[3], -.02 - proud, .03, skin, 'skin', front=C(.012), removable=True)
                if abs(z1t - z0t) > .03:
                    zb_ = max(zlo, plinth)
                    tri = [(u0, zb_), (u1, zb_), (u1, z1t)] if z1t > z0t else [(u0, zb_), (u1, zb_), (u0, z0t)]
                    k.prism(tri, -.02, .03, skin, 'skin', axis='y', removable=True)
            for kind, idx, f, u in mine:
                k.meta.setdefault('opening_side', {})[f'{kind}:{idx}'] = f['side']
                with k.tagged(f'{kind}:{idx}'):
                    with k.at((f['_u'], 0, 0)):
                        if kind == 'door':
                            infantry_door_parts(k, f['_w'], f['_h'], apron=False)
                        elif kind == 'bay':
                            rollup(k, f['_w'], f['_h'], apron=False)
                        else:
                            k.window(-f['_w'] / 2, f['_w'] / 2, f['_z'], f['_z'] + f['_h'], 0.0, mullions=1 if f['_w'] > 1.0 else 0)
            if g.get('band') and zmin > 2.0:       # painted hazard band around the shell (tanks)
                for u0, u1 in zip(lines, lines[1:]):
                    for q in subtract_rect((u0, zmin * .42, u1, zmin * .72), holes):
                        k.slab(q[0], q[2], q[1], q[3], -.05, -.02, 'hazard', 'skin', removable=True)
            k.moss_sites.append(((-L / 2 + .3, -.2, plinth), (L / 2 - .3, -.2, plinth)))
        if hits_edge is None or L > hits_edge[2]:
            hits_edge = (a, b, L, zmin)
    # corner trim on every outline vertex
    for a, b in edges:
        z = roof_top(S, *a)
        if z > plinth + .5:
            k.box((a[0], a[1], (plinth + z) / 2), (.3, .3, z - plinth), 'paint.trim', 'frame', ch=C(.03))
    if det.get('service_panel', True) and hits_edge and hits_edge[3] > 2.6:
        a, b, L, _ = hits_edge
        with k.at(((a[0] + b[0]) / 2, (a[1] + b[1]) / 2, 0), yaw=math.atan2(b[1] - a[1], b[0] - a[0])):
            k.service_panel(-L / 4, 1.7, w=.8, h=.55)
    i_roof = len(k.pieces)
    # 5. roof
    _roof(k, p, S, roofmat, g, det, plinth)
    i_feat = len(k.pieces)
    # wall batter (stock skirts, battered walls, roof overhang): walls follow the profile; flat/inset roofs ride on the wall top,
    # an extruded roof keeps its measured cross-section (its gable ends overhang battered walls, as in the stock)
    top_t = overhang_t(S)
    sx_top, sy_top = _interp(wp['x'], 1.0), _interp(wp['y'], 1.0)
    zr = max(eave, .5)
    for pc in k.pieces[i_walls:i_roof]:
        pc['v'] = [(x * _interp(wp['x'], min(z / zr, top_t)), y * _interp(wp['y'], min(z / zr, top_t)), z) for x, y, z in pc['v']]
    if rf['model'] != 'extrude':
        for pc in k.pieces[i_roof:i_feat]:
            pc['v'] = [(x * sx_top, y * sy_top, z) for x, y, z in pc['v']]
    # 6. features at their measured positions
    ways_in = []
    for kind, idx, f, ei, u in placed:
        if kind in ('door', 'bay'):
            a, b = edges[ei]
            L = math.dist(a, b)
            ways_in.append((a[0] + (b[0] - a[0]) * (u / L + .5), a[1] + (b[1] - a[1]) * (u / L + .5)))
    _features(k, S, h, ways_in)
    # 7. sockets + hit zones
    for idx, f in enumerate(feats['door'] + feats['bay']):
        pos = A.face_world(f['side'], w, d, f['at'], .5, f.get('_z', f['z']) + f.get('_h', f['h']) + .35)
        dr = A.face_dir(f['side'])
        A.add_socket(k, f'door_{idx + 1:02}', 'spot', pos, (dr[0], dr[1], -.6), 'door')
    for idx, f in enumerate(feats['window']):
        pos = A.face_world(f['side'], w, d, f['at'], .08, f.get('_z', f['z']) + f.get('_h', f['h']) / 2)
        A.add_socket(k, f'window_{idx + 1:02}', 'point', pos, A.face_dir(f['side']), 'window')
    if hits_edge:
        a, b, L, zmin = hits_edge
        nx, ny = (b[1] - a[1]) / L, -(b[0] - a[0]) / L
        k.hits.append(dict(p=((a[0] + b[0]) / 2, (a[1] + b[1]) / 2, max(1.0, zmin * .55)), d=(-nx, -ny, 0), r=max(2.0, min(w, d) * .22), depth=0, sag=.35))
    k.hits.append(dict(p=(w * .15, d * .12, max(eave, rf.get('top', eave)) * .9), d=(0, 0, -1), r=max(1.6, min(w, d) * .16), depth=0, sag=.3))


def _roof(k, p, S, roofmat, g, det, plinth):
    import archetypes as A
    kit = A.kit
    C = A.C
    w, d, h = p['size']
    rf, fp = S['roof'], S['footprint']
    eave = rf['eave']
    bay = g.get('bay_m', 4.5)
    if rf['model'] == 'flat':
        top = eave
        thk = deck_thk(S)
        if fp['kind'] in ('rect', 'rectilinear'):
            for ci, (x0, y0, x1, y1) in enumerate(fp['cells']):
                nx_, ny_ = max(1, round((x1 - x0) / bay)), max(1, round((y1 - y0) / bay))
                sx_, sy_ = (x1 - x0) / nx_, (y1 - y0) / ny_
                for i in range(nx_):
                    for j in range(ny_):
                        k.box((x0 + (i + .5) * sx_, y0 + (j + .5) * sy_, top - .07), (sx_ - .01, sy_ - .01, .14), 'gravel', 'roof', removable=True, slab=(ci, i, j))
                if thk > .2:          # the stock's overhanging roof slab under the gravel deck
                    k.box(((x0 + x1) / 2, (y0 + y1) / 2, top - .14 - (thk - .14) / 2), (x1 - x0, y1 - y0, thk - .14), 'paint.trim', 'frame', ch=C(.03))
        else:
            k.prism(outline(S), top - .14, top, 'gravel', 'roof', removable=True)
            if thk > .2:
                k.prism(outline(S), top - thk, top - .14, 'paint.trim', 'frame')
        rim = max(rf.get('rim', top), top + .12)
        for a, b in edges_of(S):
            L = math.dist(a, b)
            with k.at(((a[0] + b[0]) / 2, (a[1] + b[1]) / 2, 0), yaw=math.atan2(b[1] - a[1], b[0] - a[0])):
                k.box((0, .1, (top - .14 + rim) / 2), (L + .2, .24, rim - top + .14), 'paint.trim', 'frame', ch=C(.03), top=C(.03))
        k.moss_sites.append(((-w / 4, -d / 4, top + .01), (w / 4, -d / 4, top + .01)))
        return
    if rf['model'] == 'extrude':
        ax = rf['axis']
        run = d if ax == 'y' else w
        kn = rf['profile']
        nb = max(1, round(run / bay))
        glazed = bool(det.get('glazing'))
        with k.at((0, 0, 0), yaw=0.0 if ax == 'y' else math.pi / 2):
            ys = [-run / 2 + run * j / nb for j in range(nb + 1)]
            # rafters under the sheets on every bay line, following the profile
            for y in ys:
                for (c0, z0), (c1, z1) in zip(kn, kn[1:]):
                    if abs(c1 - c0) > .3 and max(z0, z1) > plinth + .3:
                        kit.ibeam(k, (c0, y, max(z0, plinth) - .18), (c1, y, max(z1, plinth) - .18), d=.24, w=.14)
            for si, ((c0, z0), (c1, z1)) in enumerate(zip(kn, kn[1:])):
                if abs(c1 - c0) < .05:
                    continue
                vertical = abs(z1 - z0) > 4 * abs(c1 - c0)
                for j in range(nb):
                    y0, y1 = ys[j] - (.06 if j else 0), ys[j + 1]
                    if vertical:           # clerestory / roof step: a wall panel standing on the lower roof
                        zl, zh = min(z0, z1), max(z0, z1)
                        k.box(((c0 + c1) / 2, (y0 + y1) / 2, (zl + zh) / 2), (max(.06, abs(c1 - c0)), y1 - y0, zh - zl),
                              'corr.corr' if g.get('skin') == 'corr' else 'paint.main', 'skin', removable=True)
                        continue
                    tk = .035 + .014 * (j % 2)
                    fr = [(c0, y0, z0), (c1, y0, z1), (c1, y1, z1), (c0, y1, z0)]
                    bk = [(x, y, z - tk - .02) for x, y, z in fr]
                    kit.hexa(k, fr, bk, roofmat, 'roof', removable=True, sheet=(si, j))
                if glazed and not vertical:       # glazing bars over the sheets: along the slope every 1.2 m, across every bay
                    L = math.hypot(c1 - c0, z1 - z0)
                    nbar = max(1, round(run / 1.2))
                    for q in range(nbar + 1):
                        y = -run / 2 + run * q / nbar
                        k.beam((c0, y, z0 + .03), (c1, y, z1 + .03), .06, 'paint.trim', 'frame', h=.06)
                    for q in range(1, max(2, round(L / 1.6))):
                        f_ = q / max(2, round(L / 1.6))
                        c, z = c0 + (c1 - c0) * f_, z0 + (z1 - z0) * f_
                        k.beam((c, -run / 2, z + .03), (c, run / 2, z + .03), .06, 'paint.trim', 'frame', h=.06)
            # ridge and valley trim on the profile knots
            for i in range(1, len(kn) - 1):
                c, z = kn[i]
                if z >= kn[i - 1][1] and z >= kn[i + 1][1]:
                    k.box((c, 0, z + .02), (.4, run + .1, .1), 'paint.trim', 'frame')
        k.moss_sites.append(((-w / 4, -d / 2 + .3, eave + .05), (w / 4, -d / 2 + .3, eave + .05)))
        return
    # inset rings: hip / pyramid / dome over a convex plan
    kn = rf['profile']
    s0 = kn[0][0]
    if fp['kind'] == 'rect':
        ri = rf.get('inradius', min(w, d) / 2)

        def ring(s):
            dd = (s - s0) * ri
            hx, hy = max(.05, w / 2 - dd), max(.05, d / 2 - dd)
            return [(-hx, -hy), (hx, -hy), (hx, hy), (-hx, hy)]
    else:
        base = outline(S)
        cx = sum(x for x, _ in base) / len(base)
        cy = sum(y for _, y in base) / len(base)

        def ring(s):
            f = max(.02, 1 - (s - s0))
            return [(cx + (x - cx) * f, cy + (y - cy) * f) for x, y in base]
    rings = [(ring(s), z) for s, z in kn]
    for ri_, ((r0, z0), (r1, z1)) in enumerate(zip(rings, rings[1:])):
        n = len(r0)
        for j in range(n):
            a0, b0, a1, b1 = r0[j], r0[(j + 1) % n], r1[j], r1[(j + 1) % n]
            fr = [(a0[0], a0[1], z0), (b0[0], b0[1], z0), (b1[0], b1[1], z1), (a1[0], a1[1], z1)]
            bk = [(x, y, z - .05) for x, y, z in fr]
            kit.hexa(k, fr, bk, roofmat, 'roof', removable=True, sheet=(ri_, j))
    last, zl = rings[-1]
    if max(abs(x) for x, _ in last) > .1:
        k.prism(last, zl - .06, zl, 'paint.trim', 'roof', removable=True)
    k.moss_sites.append(((-w / 4, -d / 4, eave + .05), (w / 4, -d / 4, eave + .05)))


def _features(k, S, h, ways_in=()):
    """Rooftop and side features at their measured plan position and heights; painted roof fans and vents as low housings."""
    import archetypes as A
    C = A.C
    for i, f in enumerate(S.get('features', [])):
        x, y = f['at']
        sx, sy = f['size']
        base, sh, top = f['base'], f['shoulder'], f['top']
        rnd = f['shape'] == 'round'
        if rnd:
            n = 16
            poly = lambda s, x=x, y=y, sx=sx, sy=sy: [(x + sx / 2 * s * math.cos(math.tau * q / n), y + sy / 2 * s * math.sin(math.tau * q / n)) for q in range(n)]  # noqa: E731
        else:
            poly = lambda s, x=x, y=y, sx=sx, sy=sy: [(x - sx / 2 * s, y - sy / 2 * s), (x + sx / 2 * s, y - sy / 2 * s), (x + sx / 2 * s, y + sy / 2 * s), (x - sx / 2 * s, y + sy / 2 * s)]  # noqa: E731
        with k.tagged(f'feature:{i}'):
            if f['where'] == 'painted':
                hh = .14
                k.prism(poly(1.0), base - .02, base + hh, 'paint.trim', 'frame', top=C(.02))
                k.prism(poly(.82), base + hh * .4, base + hh + .01, 'metal', 'frame')
                if rnd:
                    k.cyl((x, y, base + hh * .8), min(sx, sy) * .12, hh * .6, 'paint.second', 'frame', sides=12)
                    for q in range(6):            # fan blades (detail: baked into the atlas)
                        a = math.tau * q / 6
                        k.beam((x, y, base + hh), (x + math.cos(a) * sx * .36, y + math.sin(a) * sy * .36, base + hh), .12, 'metal', 'detail', h=.02)
                else:
                    for q in range(4):            # louvre slats (detail)
                        yy = y - sy * .3 + sy * .2 * q
                        k.box((x, yy, base + hh + .01), (sx * .7, .06, .03), 'metal', 'detail')
                continue
            if f['where'] == 'side' and any(abs(px - x) < sx / 2 + .6 and abs(py - y) < sy / 2 + .6 for px, py in ways_in):
                # a vestibule over a door: roof slab and two cheek walls, open on the door's face
                along_x = sx >= sy
                k.box((x, y, top - .15), (sx, sy, .3), 'paint.main', 'mass', ch=C(.03), top=C(.03))
                for s_ in (-1, 1):
                    if along_x:
                        k.box((x + s_ * (sx / 2 - .15), y, (base + top - .3) / 2), (.3, sy, top - .3 - base), 'paint.main', 'mass')
                    else:
                        k.box((x, y + s_ * (sy / 2 - .15), (base + top - .3) / 2), (sx, .3, top - .3 - base), 'paint.main', 'mass')
                continue
            tall = top - base > .25 * h
            mat = {'tank': 'paint.second', 'tower': 'paint.main', 'annex': 'paint.main', 'pipe': 'paint.trim'}.get(f['kind'], 'metal')
            role = 'mass' if tall or f['kind'] in ('annex', 'tower', 'tank') else 'frame'
            z0 = max(0.0, base - .1)
            if sh < top - .15:
                k.prism(poly(1.0), z0, top, mat, role, top=min(sx, sy) * .45, top_h=top - sh)
            else:
                k.prism(poly(1.0), z0, top, mat, role, top=C(min(.06, min(sx, sy) * .1)))
            if f['kind'] in ('vent', 'fan') and min(sx, sy) > .8:
                k.prism(poly(.7), top, top + .02, 'grate', 'detail')
