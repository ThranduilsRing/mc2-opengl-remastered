"""Archetype builders for props and structures (tent, container, crate, barrel, tank, turret, vehicle, pylon,
tower, junk, bridge, pipe, fence) plus the shared damage kinds. See archetypes.py for the params schema.

Every builder: frame first, skin as separate removable panels, hit zones, feature tags (group 'kind:i') so the
validator can recount, light sockets where the stock has emitters, supports/footings flagged part='footing'.
"""
import math
import random

import geo
from archetypes import add_socket, kit, C
from geo import rect_poly, subtract_rect


def D(p, key, default=None):
    return p.get('dims', {}).get(key, default)


def n_of(p, field, default=0):
    return int(p.get('counts', {}).get(field, default))


def scale_pieces(k, i0, cx, cy, sxy, sz):
    """Scale pieces added since index i0 about (cx, cy, 0): lets a legacy fixed-size builder serve any size."""
    for pc in k.pieces[i0:]:
        pc['v'] = [((x - cx) * sxy + cx, (y - cy) * sxy + cy, z * sz) for x, y, z in pc['v']]


def cloth(k, a, b, c, d, nu, nv, sag, mat, role='skin', thick=.02, sagdir=(0, 0, -1), **fl):
    """Bilinear canvas patch over corners a,b,c,d (a-b and d-c are the nu direction) with a bulge along sagdir;
    a closed two-layer solid so it bakes."""
    if geo.GAME_MESH:
        nu, nv = min(nu, 2), min(nv, 2)
    lay = []
    for off in (0.0, thick):
        pts = []
        for j in range(nv + 1):
            v = j / nv
            for i in range(nu + 1):
                u = i / nu
                base = [(1 - u) * (1 - v) * a[m] + u * (1 - v) * b[m] + u * v * c[m] + (1 - u) * v * d[m] for m in range(3)]
                s = (sag * math.sin(math.pi * u) * math.sin(math.pi * v) + off) if not geo.GAME_MESH else off + sag * .5 * (1 if (0 < i < nu and 0 < j < nv) else 0)
                pts.append(tuple(base[m] + sagdir[m] * s for m in range(3)))
        lay.append(pts)
    stride = nu + 1
    n = stride * (nv + 1)
    verts = lay[0] + lay[1]
    faces = []
    for j in range(nv):
        for i in range(nu):
            q = j * stride + i
            faces.append((q, q + 1, q + stride + 1, q + stride))
            faces.append((n + q, n + q + stride, n + q + stride + 1, n + q + 1))
    for j in range(nv):
        for i in (0, nu):
            q, r = j * stride + i, (j + 1) * stride + i
            f = (q, r, n + r, n + q)
            faces.append(f if i == 0 else tuple(reversed(f)))
    for j in (0, nv):
        for i in range(nu):
            q = j * stride + i
            f = (q, n + q, n + q + 1, q + 1)
            faces.append(f if j == 0 else tuple(reversed(f)))
    return k.add(verts, faces, mat, role, **fl)


def arc_plate(k, R, th0, th1, z0, z1, t, mat, cx=0.0, cy=0.0, role='skin', **fl):
    """Vertical cylindrical shell plate (outer radius R, thickness t) between angles th0..th1."""
    n = max(1, int(round((th1 - th0) / (math.pi / (8 if geo.GAME_MESH else 24)))))
    vv, ff = [], []
    for rr in (R, R - t):
        for z in (z0, z1):
            for i in range(n + 1):
                a = th0 + (th1 - th0) * i / n
                vv.append((cx + rr * math.cos(a), cy + rr * math.sin(a), z))
    s = n + 1
    o = lambda ring, zi, i: (ring * 2 + zi) * s + i  # noqa: E731
    for i in range(n):
        ff.append((o(0, 0, i), o(0, 0, i + 1), o(0, 1, i + 1), o(0, 1, i)))
        ff.append((o(1, 0, i), o(1, 1, i), o(1, 1, i + 1), o(1, 0, i + 1)))
        ff.append((o(0, 1, i), o(0, 1, i + 1), o(1, 1, i + 1), o(1, 1, i)))
        ff.append((o(0, 0, i), o(1, 0, i), o(1, 0, i + 1), o(0, 0, i + 1)))
    for i in (0, n):
        f = (o(0, 0, i), o(0, 1, i), o(1, 1, i), o(1, 0, i))
        ff.append(f if i == 0 else tuple(reversed(f)))
    return k.add(vv, ff, mat, role, **fl)


def footing_box(k, c, s, mat='ferro', ch=.06):
    return k.box(c, s, mat, 'mass', ch=C(ch), top=C(ch * .7), part='footing')


# ------------------------------------------------------------------ tent
def tent(k, p):
    """Poles and ridge -> eave rails -> canvas panels per bay (roof + valance) -> gable ends with the door frame -> guys."""
    w, d, h = p['size']
    eave = D(p, 'eave_h', h * .38)
    ridge_x = w >= d
    span, run = (d, w) if ridge_x else (w, d)
    bays = int(D(p, 'bays') or max(1, round(run / 4.5)))
    ys = [-run / 2 + .35 + (run - .7) * i / bays for i in range(bays + 1)]
    doors = n_of(p, 'doors', 1)
    ov = .25
    with k.at((0, 0, 0), yaw=math.pi / 2 if ridge_x else 0.0):
        for y in ys:
            k.beam((0, y, 0), (0, y, h - .02), .12, 'metal', 'frame')
        k.beam((0, ys[0] - .3, h), (0, ys[-1] + .3, h), .14, 'metal', 'frame', h=.12)
        for sx in (-1, 1):
            for y in ys:
                k.beam((sx * (span / 2 - .25), y, 0), (sx * (span / 2 - .25), y, eave), .08, 'metal', 'frame')
            k.beam((sx * (span / 2 - .25), ys[0], eave), (sx * (span / 2 - .25), ys[-1], eave), .06, 'metal', 'frame')
        for sx in (-1, 1):
            for i in range(bays):
                a, b = ys[i] - (.05 if i else .3), ys[i + 1] + (.05 if i < bays - 1 else .3)
                cloth(k, (0, a, h), (0, b, h), (sx * (span / 2 + ov), b, eave - .12), (sx * (span / 2 + ov), a, eave - .12), 4, 3, .14, 'canvas.main', removable=True, tent=(sx, i))
                cloth(k, (sx * (span / 2 + ov), a, eave - .12), (sx * (span / 2 + ov), b, eave - .12), (sx * (span / 2 + ov * .6), b, .25), (sx * (span / 2 + ov * .6), a, .25),
                      3, 2, .07, 'canvas.second', sagdir=(sx, 0, 0), removable=True, tent=(sx, i, 'v'))
        for e, yy in enumerate((ys[0] - .3, ys[-1] + .3)):
            outward = -1 if e == 0 else 1
            hw = span / 2 + ov * .3
            if e < doors:
                dw, dh = D(p, 'door_w', 1.3), min(D(p, 'door_h', 2.2), eave + (h - eave) * .5)
                poly = [(-hw, .2), (-dw / 2, .2), (-dw / 2, dh), (dw / 2, dh), (dw / 2, .2), (hw, .2), (hw, eave), (0, h - .05), (-hw, eave)]
                with k.tagged(f'door:{e}'):
                    for sx in (-1, 1):
                        k.beam((sx * (dw / 2 + .05), yy + outward * .05, .2), (sx * (dw / 2 + .05), yy + outward * .05, dh), .1, 'wood.second', 'frame')
                    k.beam((-dw / 2 - .05, yy + outward * .05, dh + .05), (dw / 2 + .05, yy + outward * .05, dh + .05), .1, 'wood.second', 'frame')
                    k.box((0, yy - outward * .15, dh / 2 + .1), (dw - .1, .06, dh - .2), 'rubber', 'skin')
            else:
                poly = [(-hw, .2), (hw, .2), (hw, eave), (0, h - .05), (-hw, eave)]
            if outward > 0:
                poly = [(x, z) for x, z in poly]
            k.prism(poly, yy - .03, yy + .03, 'canvas.main', 'skin', axis='y', removable=True, tent=('end', e))
        for sx in (-1, 1) if D(p, 'guys', False) else ():       # guy ropes are not in the stock bounds (COUNT RULE): off unless the stock shows them
            for y in (ys[0] - .3, ys[-1] + .3):
                kit.wire(k, [(sx * (span / 2 + ov), y, eave - .1), (sx * (span / 2 + 2.0), y + (-.6 if y < 0 else .6), .05)], .014, 'metal', 'detail', sides=4)
                k.box((sx * (span / 2 + 2.0), y + (-.6 if y < 0 else .6), .1), (.08, .08, .22), 'metal', 'detail')
    k.moss_sites.append(((-w / 2, -d / 2 - .1, 0.05), (w / 2, -d / 2 - .1, 0.05)))
    k.hits.append(dict(p=(0, 0, h * .8), d=(0, 0, -1), r=max(1.6, span * .3), depth=0, sag=.25))
    k.hits.append(dict(p=(span * .3 if not ridge_x else 0, 0 if not ridge_x else span * .3, eave), d=(1, 0, 0), r=max(1.4, run * .15), depth=0, sag=.1))


# ------------------------------------------------------------------ container (dumpster, steel cases, lockers)
def container(k, p):
    """Skids -> corner posts and rails -> ribbed sheet panels per face (separate, removable) -> hinged lid -> handles."""
    w, d, h = p['size']
    n = max(1, n_of(p, 'crates', 1))
    long_x = w >= d
    ul = (w if long_x else d) / n
    gap = min(.12, ul * .03)
    lid = D(p, 'lid', 0.0)        # opening angle in degrees; 0 = closed flat lid
    hl = D(p, 'lid_h', .16)
    doors = n_of(p, 'doors', 0)
    ribs = int(D(p, 'ribs', max(3, round(ul / .45))))
    for i in range(n):
        c0 = -(w if long_x else d) / 2 + ul * (i + .5)
        cx, cy = (c0, 0.0) if long_x else (0.0, c0)
        uw, ud = ((ul - gap), d) if long_x else (w, (ul - gap))
        bh = h - (hl + .06 + (ud - .12) * math.sin(math.radians(lid)) if lid else 0)      # the open lid's far edge must still end at the stock height
        with k.tagged(f'crate:{i}'):
            with k.at((cx, cy, 0)):
                for sy in (-1, 1):
                    k.box((0, sy * (ud / 2 - .35), .1), (uw - .1, .16, .2), 'metal', 'frame', ch=C(.02), part='footing')
                for sx in (-1, 1):
                    for sy in (-1, 1):
                        k.box((sx * (uw / 2 - .06), sy * (ud / 2 - .06), (.2 + bh) / 2), (.12, .12, bh - .2), 'paint.trim', 'frame', ch=C(.02))
                for z in (.28, bh - .08):
                    for sy in (-1, 1):
                        k.box((0, sy * (ud / 2 - .05), z), (uw - .1, .1, .12), 'paint.trim', 'frame')
                    for sx in (-1, 1):
                        k.box((sx * (uw / 2 - .05), 0, z), (.1, ud - .1, .12), 'paint.trim', 'frame')
                # skin: separate sheet panels per face (two per long face), removable
                for sy in (-1, 1):
                    for r in range(2):
                        x0 = -uw / 2 + .12 + (uw - .24) * r / 2
                        x1 = -uw / 2 + .12 + (uw - .24) * (r + 1) / 2 + (.05 if r == 0 else 0)
                        k.slab(x0, x1, .26, bh - .06, sy * (ud / 2 - .05) - .03, sy * (ud / 2 - .05) + .02, 'paint.main', 'skin', front=C(.012), removable=True)
                for sx in (-1, 1):
                    k.box((sx * (uw / 2 - .03), 0, (bh + .26) / 2), (.05, ud - .24, bh - .32), 'paint.main', 'skin', removable=True)
                for q in range(ribs):
                    x = -uw / 2 + .25 + (uw - .5) * (q + .5) / ribs
                    for sy in (-1, 1):
                        k.box((x, sy * (ud / 2 - .01), (bh + .26) / 2), (.07, .035, bh - .4), 'paint.second', 'detail')
                # lid
                if lid:
                    ang = math.radians(lid)
                    with k.at((0, ud / 2 - .06, bh), pitch=-ang if long_x else -ang):
                        k.box((0, -(ud - .12) / 2, hl / 2), (uw - .08, ud - .12, hl), 'paint.second', 'skin', ch=C(.02), removable=True, lid=True)
                else:
                    k.box((0, 0, bh + hl / 2), (uw - .06, ud - .06, hl), 'paint.second', 'skin', ch=C(.02), top=C(.02), removable=True, lid=True)
                if i < doors:
                    with k.tagged(f'door:{i}'):
                        for sx in (-1, 1):
                            k.slab(sx * .02 + (-uw / 2 + .2 if sx < 0 else .02), sx * .02 + (-.02 if sx < 0 else uw / 2 - .2), .5, bh - .4, -ud / 2 - .04, -ud / 2 + .02, 'paint.trim', 'frame')
                k.box((uw * .3, -ud / 2 - .03, bh * .55), (.3, .04, .08), 'metal', 'detail')
        k.moss_sites.append(((cx - uw / 2, cy - ud / 2 - .06, .02), (cx + uw / 2, cy - ud / 2 - .06, .02)))
    k.hits.append(dict(p=(0, 0, h * .5), d=(0, 1, 0), r=max(.9, min(w, d) * .4), depth=0, sag=.1))


# ------------------------------------------------------------------ crate stack (legacy wooden case + tarp builders)
def crates(k, p):
    w, d, h = p['size']
    n = max(1, n_of(p, 'crates', 2))
    tarp = bool(D(p, 'tarp', False))
    unit = (w / n) * .96
    L, W, H = unit, d * .95, h * .967
    xs = tuple(-w / 2 + w * (i + .5) / n for i in range(n))
    with _overrides((kit.CR, dict(L=L, W=W, H=H, xs=xs)), (kit.TARP, dict(hem_ends=H * .45, hem_sides=H * .58))):
        for idx, cx in enumerate(xs):
            kit.crate(k, cx, idx, end_midrail=bool(D(p, 'end_midrail', True)))
            if tarp:
                kit.tarp(k, cx, idx)
    k.hits.append(dict(p=(xs[0], -W / 2, H * .35), d=(0, 1, 0), r=max(1.0, H * .5), depth=0, sag=0))


class _overrides:
    def __init__(self, *pairs):
        self.pairs = pairs

    def __enter__(self):
        self.old = [(t, {kk: t.get(kk) for kk in v}) for t, v in self.pairs]
        for t, v in self.pairs:
            t.update(v)

    def __exit__(self, *a):
        for t, old in self.old:
            t.update(old)


# ------------------------------------------------------------------ barrels (legacy drum scaled to the stock radius/height)
def barrels(k, p):
    w, d, h = p['size']
    n = max(1, n_of(p, 'barrels', 1))
    R = min(w / 2, d / (2 * n) * (1 if n > 1 else 1)) * (1.0 if n == 1 else .95)
    R = min(R, w / 2)
    centres = [((-1) ** i * (w / 2 - R) * .35 if n > 1 else 0.0, -d / 2 + d * (i + .5) / n) for i in range(n)]
    for i, (x, y) in enumerate(centres):
        i0 = len(k.pieces)
        with k.at((x, y, 0)):
            with k.tagged(f'barrel:{i}'):
                kit.drum(k, i)
        for pc in k.pieces[i0:]:
            pc['group'] = f'barrel:{i}'
        scale_pieces(k, i0, x, y, R / .58, h / 1.68)
        k.moss_sites.append(((x - R, y - R, 0), (x + R, y - R, 0)))
    if D(p, 'flame'):
        with k.at((centres[0][0], centres[0][1], h)):
            for q, (r0, hh) in enumerate(((R * .55, .5), (R * .38, .8), (R * .2, 1.0))):
                k.cyl((0, 0, hh * .5), r0, hh, 'lamp.marker', 'detail', sides=8, top=r0 * .5)
        add_socket(k, 'fire_01', 'point', (centres[0][0], centres[0][1], h + .6), (0, 0, 1), 'window')
        k.meta['sockets'][-1]['colour_linear'] = (1.0, .45, .1)
    k.hits.append(dict(p=(0, centres[0][1], h * .5), d=(0, 1, 0), r=max(.8, R * 2), depth=0, sag=0))


# ------------------------------------------------------------------ tank
def tank(k, p):
    """Footing ring -> legs or skirt -> stiffener rings -> shell plates by course -> roof cap -> hazard band, ladder, catwalk."""
    w, d, h = p['size']
    n = max(1, n_of(p, 'tanks', 1))
    import shell as shellmod
    S = shellmod.load(p['asset'])
    if S is not None and S['footprint']['kind'] != 'circle':
        why = shellmod.unsupported(S, 'tank') or (None if n == 1 else f'tank: {n} tanks on a {S["footprint"]["kind"]} plan')
        if why:
            raise ValueError(f'needs_builder: {why}')
        with k.tagged('tank:0'):
            shellmod.build(k, p, S)
        return
    body = S['footprint'] if (S is not None and n == 1) else None      # the measured round body (may sit off the bbox centre)
    along_y = d >= w
    legs = int(D(p, 'legs', 0))
    leg_h = D(p, 'leg_h', h * .35 if legs else .5)
    cap_h = D(p, 'cap_h', min(h * .12, 2.5))
    R = min((w if along_y else d) / 2, ((d if along_y else w) / n) / 2) * .96
    R_foot = R_waist = None
    if body:            # body, waist and footing radii from the measured wall profile (the stock skirt narrows under the shell)
        wp = S['wall_profile']
        ip = lambda kn, t: next((v0 + (v1 - v0) * (t - t0) / max(t1 - t0, 1e-9) for (t0, v0), (t1, v1) in zip(kn, kn[1:]) if t <= t1), kn[-1][1])  # noqa: E731
        rad = lambda t: (ip(wp['x'], t) * w / 2 + ip(wp['y'], t) * d / 2) / 2  # noqa: E731
        R = rad(1.0)
        R_foot = max(min(body['radii']), R)
        R_waist = min(R, min(rad(t / 20) for t in range(1, 8)))
    shell = h - leg_h - cap_h
    cone = bool(body and S['roof']['model'] == 'inset')
    if cone:            # the measured body: walls to the stock eave, a conical roof to the stock roof top (not the bbox top)
        cap_h = max(.3, S['roof']['top'] - S['roof']['eave'])
        shell = max(1.0, S['roof']['eave'] - leg_h)
    N = 28 if geo.GAME_MESH else 56          # truly round: no era faceting
    for i in range(n):
        c = -(d if along_y else w) / 2 + ((d if along_y else w) / n) * (i + .5)
        cx, cy = (0.0, c) if along_y else (c, 0.0)
        if body:
            cx, cy = body['center']
        with k.tagged(f'tank:{i}'):
            k.prism(geo.reg_poly(R_foot or R + .25, N, cx=cx, cy=cy), 0, .3, 'ferro', 'mass', top=C(.08), part='footing')
            if legs:
                for q in range(legs):
                    a = q * math.tau / legs + math.pi / legs
                    x, y = cx + (R * .82) * math.cos(a), cy + (R * .82) * math.sin(a)
                    kit.ibeam(k, (x, y, .3), (x, y, leg_h), d=.34, w=.2)
                for q in range(legs):
                    a0, a1 = q * math.tau / legs + math.pi / legs, (q + 1) * math.tau / legs + math.pi / legs
                    p0 = (cx + R * .82 * math.cos(a0), cy + R * .82 * math.sin(a0))
                    p1 = (cx + R * .82 * math.cos(a1), cy + R * .82 * math.sin(a1))
                    kit.ibeam(k, (p0[0], p0[1], .4), (p1[0], p1[1], leg_h * .62), d=.12, w=.08)
                    kit.ibeam(k, (p1[0], p1[1], .4), (p0[0], p0[1], leg_h * .62), d=.12, w=.08)
            else:
                k.prism(geo.reg_poly(R_waist or R * .96, N, cx=cx, cy=cy), .3, leg_h, 'ferro', 'mass', top=C(.05))
            k.prism(geo.reg_poly(R + .06, N, cx=cx, cy=cy), leg_h - .12, leg_h + .12, 'paint.trim', 'frame')
            courses = max(2, round(shell / 2.6))
            ch = shell / courses
            for c_ in range(courses):
                z0 = leg_h + c_ * ch
                per = 6          # removable plates: identical on the high source and the game carrier (uid lockstep)
                for s_ in range(per):
                    th0 = s_ * math.tau / per + (c_ % 2) * math.pi / per
                    arc_plate(k, R, th0 - .01, th0 + math.tau / per + .01, z0 + .01, z0 + ch - .01, .05 + .012 * ((s_ + c_) % 2), 'paint.main', cx, cy, removable=True)
                k.prism(geo.reg_poly(R + .05, N, cx=cx, cy=cy), z0 + ch - .05, z0 + ch + .05, 'paint.trim', 'frame')
            if D(p, 'band', True):
                with k.at((cx, cy, 0)):
                    kit.wrapped_band(k, R + .02, leg_h + shell * .42, leg_h + shell * .62, 0.0, 1.6)
            if cone:
                k.prism(geo.reg_poly(R * 1.01, N, cx=cx, cy=cy), leg_h + shell, leg_h + shell + cap_h, 'paint.roof', 'roof', top=R * .9, top_h=cap_h, removable=True)
            else:
                k.cyl((cx, cy, leg_h + shell + cap_h * .25), R * .98, cap_h * .5, 'paint.second', 'skin', sides=N, top=C(R * .15), removable=True)
                k.cyl((cx, cy, leg_h + shell + cap_h * .75), R * .55, cap_h * .5, 'paint.second', 'skin', sides=N, top=C(R * .1), removable=True)
            # access: ladder up the shell and a catwalk ring (detail)
            for z in range(1, int(shell / .35)):
                k.box((cx + R + .05, cy, leg_h + z * .35), (.05, .4, .05), 'metal', 'detail')
            k.box((cx + R + .12, cy - .2, leg_h + shell / 2), (.05, .05, shell), 'metal', 'detail')
            k.box((cx + R + .12, cy + .2, leg_h + shell / 2), (.05, .05, shell), 'metal', 'detail')
            k.moss_sites.append(((cx - R, cy - R - .3, .3), (cx + R, cy - R - .3, .3)))
    k.hits.append(dict(p=(0, 0, leg_h + shell * .45), d=(0, -1, 0), r=max(2.0, R * .9), depth=0, sag=.3))
    if body:            # measured side/rooftop features (the stock's oval side cylinder, vents) at their stock positions
        shellmod._features(k, S, h)


# ------------------------------------------------------------------ turret
def turret(k, p):
    """Footing -> pedestal and ring -> turntable -> housing frame + separate panels -> gun barrels and sensor."""
    w, d, h = p['size']
    bh = D(p, 'base_h', h * (.55 if D(p, 'popup') else .3))
    br = D(p, 'base_r', min(w, d) * .42)
    hw, hd, hh = D(p, 'house', (w * .62, d * .45, h * .3))
    nb = int(D(p, 'barrels', 1))
    bl = D(p, 'barrel_len', d * .42)
    N = 24 if geo.GAME_MESH else 48
    k.prism(geo.reg_poly(br * 1.1, N), 0, .35, 'ferro', 'mass', top=C(.1), part='footing')
    k.prism(geo.reg_poly(br, N), .35, bh, 'ferro', 'mass', top=C(.08), bot=C(.06))
    k.prism(geo.reg_poly(br * 1.05, N), bh - .12, bh + .1, 'paint.trim', 'frame')
    z0 = bh + .1
    with k.tagged('support:0'):
        k.box((0, 0, z0 + .1), (hw * .8, hd * .8, .2), 'metal', 'frame')
    for sx in (-1, 1):
        for sy in (-1, 1):
            k.box((sx * (hw / 2 - .12), sy * (hd / 2 - .12), z0 + hh / 2), (.22, .22, hh), 'paint.trim', 'frame', ch=C(.03))
    for z in (z0 + .15, z0 + hh - .1):
        for sy in (-1, 1):
            k.box((0, sy * (hd / 2 - .12), z), (hw - .2, .16, .16), 'paint.trim', 'frame')
        for sx in (-1, 1):
            k.box((sx * (hw / 2 - .12), 0, z), (.16, hd - .2, .16), 'paint.trim', 'frame')
    for sy in (-1, 1):
        k.slab(-hw / 2 + .12, hw / 2 - .12, z0 + .22, z0 + hh - .22, sy * (hd / 2 - .04) - .05, sy * (hd / 2 - .04) + .04, 'paint.main', 'skin', front=C(.02), removable=True)
    for sx in (-1, 1):
        k.box((sx * (hw / 2 - .04), 0, z0 + hh / 2), (.09, hd - .24, hh - .44), 'paint.main', 'skin', removable=True)
    k.box((0, 0, z0 + hh + .06), (hw - .1, hd - .1, .14), 'paint.second', 'skin', ch=C(.03), removable=True)
    for i in range(nb):
        x = (i - (nb - 1) / 2) * min(hw * .5, .9)
        k.box((x, -hd / 2 - bl / 2 + .05, z0 + hh * .55), (.28, bl, .28), 'metal', 'frame', ch=C(.03))
        k.box((x, -hd / 2 - bl + .05, z0 + hh * .55), (.38, .35, .38), 'paint.trim', 'frame')
    k.cyl((hw * .25, hd * .15, z0 + hh + .3), .28, .35, 'paint.trim', 'detail', sides=10)
    k.hits.append(dict(p=(0, -hd / 2, z0 + hh * .5), d=(0, 1, 0), r=max(1.2, hw * .45), depth=0, sag=.2))


# ------------------------------------------------------------------ vehicle
def vehicle(k, p):
    """Chassis rails and axles -> wheels -> cab frame + panels (windscreen, headlamps) -> cargo body -> details."""
    w, d, h = p['size']
    along_y = d >= w
    L, Wd = (d, w) if along_y else (w, d)
    cargo = D(p, 'cargo', 'box')
    axles = int(D(p, 'axles', 3 if L > 10 else 2))
    wr = D(p, 'wheel_r', min(h * .17, .75))
    cab_l = L * D(p, 'cab_frac', .26 if cargo != 'none' else .6)
    ch = D(p, 'cab_h', h * .78)
    zc = wr * 2 * .55
    with k.at((0, 0, 0), yaw=0.0 if along_y else -math.pi / 2):
        # chassis
        for sx in (-1, 1):
            k.box((sx * Wd * .3, 0, zc), (.18, L * .96, .26), 'metal', 'frame')
        ay = [-L / 2 + cab_l * .55 + i * ((L - cab_l * .55 - L * .08) / max(1, axles)) * (1 if axles > 1 else 0) for i in range(axles)] if axles > 1 else [-L * .2, L * .3]
        if axles == 1:
            ay = [0]
        wheel = 0
        for y in ay:
            k.box((0, y, wr), (Wd * .9, .1, .1), 'metal', 'frame')
            for sx in (-1, 1):
                nw_ = 12 if not geo.GAME_MESH else 8
                with k.tagged(f'support:{wheel}'):       # one tag per wheel: legs_supports = wheels
                    k.prism([(wr * math.cos(a) + y, wr * math.sin(a) + wr) for a in [q * math.tau / nw_ for q in range(nw_)]],
                            sx * Wd * .45 - .17, sx * Wd * .45 + .17, 'rubber', 'mass', axis='x')
                wheel += 1
        # cab: frame then panels
        cy0, cy1 = -L / 2, -L / 2 + cab_l
        for sx in (-1, 1):
            for yy in (cy0 + .1, cy1 - .1):
                k.box((sx * (Wd * .5 - .1), yy, zc + ch / 2), (.14, .14, ch), 'paint.trim', 'frame')
        k.box((0, (cy0 + cy1) / 2, zc + ch), (Wd - .1, cab_l, .12), 'paint.main', 'skin', ch=C(.03), removable=True)
        k.slab(-Wd / 2 + .1, Wd / 2 - .1, zc + .35, zc + ch * .55, cy0 - .03, cy0 + .04, 'paint.main', 'skin', front=C(.04), removable=True)
        k.slab(-Wd / 2 + .15, Wd / 2 - .15, zc + ch * .56, zc + ch - .1, cy0 + .02, cy0 + .06, 'glass', 'glass', pane=True)
        for sx in (-1, 1):
            k.box((sx * (Wd / 2 - .06), (cy0 + cy1) / 2, zc + ch * .5), (.06, cab_l - .1, ch - .5), 'paint.main', 'skin', removable=True)
            k.box((sx * Wd * .3, cy0 - .06, zc + .45), (.35, .1, .22), 'lamp', 'detail')
            add_socket(k, f'head_{"l" if sx < 0 else "r"}', 'spot', (sx * Wd * .3, cy0 - .12, zc + .45), (0, -1, -.1), 'door')
        # cargo body
        if cargo in ('box', 'ambulance'):
            bl = L - cab_l - .15
            by = cy1 + .1 + bl / 2
            bh = h - zc - .15
            for sx in (-1, 1):
                for yy in (by - bl / 2 + .1, by + bl / 2 - .1):
                    k.box((sx * (Wd / 2 - .08), yy, zc + bh / 2), (.12, .12, bh), 'paint.trim', 'frame')
            for sx in (-1, 1):
                k.box((sx * (Wd / 2 - .04), by, zc + bh / 2), (.06, bl - .2, bh - .2), 'paint.second', 'skin', removable=True)
            k.box((0, by, zc + bh), (Wd - .1, bl, .1), 'paint.second', 'skin', ch=C(.02), removable=True)
            k.box((0, by + bl / 2 - .03, zc + bh / 2), (Wd - .2, .06, bh - .2), 'paint.second', 'skin', removable=True)
            k.box((0, by - bl / 2 + .03, zc + bh / 2), (Wd - .2, .06, bh - .2), 'paint.second', 'skin', removable=True)
        elif cargo == 'tank':
            r = min(Wd * .42, (h - zc) / 2)
            bl = L - cab_l - .3
            with k.at((0, cy1 + .2 + bl / 2, zc + r + .1), pitch=math.pi / 2):
                k.cyl((0, 0, 0), r, bl, 'paint.second', 'skin', sides=14, removable=True)
        elif cargo == 'flat':
            k.box((0, cy1 + (L - cab_l) / 2, zc + .12), (Wd - .1, L - cab_l - .2, .14), 'paint.second', 'skin', removable=True)
    k.hits.append(dict(p=(0, 0, h * .5), d=(1, 0, 0), r=max(1.2, min(w, d) * .5), depth=0, sag=.1))


# ------------------------------------------------------------------ pylon (lamp post, marker, sign, flag, mast, alarm)
def pylon(k, p):
    w, d, h = p['size']
    kind = D(p, 'kind', 'lamp')
    pw = D(p, 'pole_w', max(.12, min(w, d) * .28))
    if kind == 'marker':
        base = [(-w * .5, 0), (w * .5, -d * .5), (w * .5, d * .5)] if D(p, 'wedge', True) else geo.reg_poly(min(w, d) / 2, 3)
        k.prism(base, 0, h * .1, 'metal', 'frame', part='footing')
        vv = [(x, y, h * .1) for x, y in base] + [(x * .45, y * .45, h) for x, y in base]
        k.add(vv, [(2, 1, 0), (3, 4, 5), (0, 1, 4, 3), (1, 2, 5, 4), (2, 0, 3, 5)], D(p, 'lamp_mat', 'lamp.marker'), 'skin')
        add_socket(k, 'marker_01', 'point', (0, 0, h * .65), (0, 0, 1), 'window')
        return
    footing_box(k, (0, 0, .15), (max(.5, pw * 4), max(.5, pw * 4), .3))
    top = h - (D(p, 'head_h', .0) if kind != 'flag' else 0)
    with k.tagged('support:0'):
        if kind == 'mast':
            for sx in (-1, 1):
                for sy in (-1, 1):
                    kit.ibeam(k, (sx * w * .4, sy * d * .4, .3), (sx * w * .12, sy * d * .12, h * .92), d=.16, w=.1)
            for z0, z1 in ((0.3, h * .3), (h * .3, h * .6), (h * .6, h * .9)):
                f0, f1 = 1 - z0 / h * .7, 1 - z1 / h * .7
                for sx in (-1, 1):
                    k.beam((sx * w * .4 * f0, -d * .4 * f0, z0), (sx * w * .4 * f1, d * .4 * f1, z1), .05, 'metal', 'frame')
        else:
            kit.loft(k, (-pw / 2, -pw / 2, pw / 2, pw / 2), .3, (-pw * .38, -pw * .38, pw * .38, pw * .38), top, 'metal' if kind != 'sign' else 'paint.trim', 'frame', ch0=.01)
    if kind == 'lamp':
        for z in (top * .3, top * .78):
            k.box((0, 0, z), (pw * 1.15, pw * 1.15, .18), 'paint.trim', 'frame')
        hw, hd2 = max(w * .5, .5), max(d * .45, .4)
        with k.at((0, 0, top)):
            k.box((0, -hd2 * .3, .12), (hw, hd2, .22), 'paint.main', 'frame', ch=C(.03))
            k.box((0, -hd2 * .3, .0), (hw * .85, hd2 * .8, .05), 'lamp', 'glass')
        add_socket(k, 'head_01', 'spot', (0, -hd2 * .3, top - .1), (0, -.45, -1), 'door')
        k.meta['sockets'][-1]['range_m'] = 10
    elif kind == 'sign':
        pw2 = D(p, 'plate_w', min(w, d) * .9 or .9)
        k.box((0, -pw * .6, top - pw2 / 2), (pw2, .05, pw2), D(p, 'plate_mat', 'paint.accent'), 'skin', ch=C(.04), removable=True)
        k.box((0, -pw * .6 - .03, top - pw2 / 2), (pw2 * .8, .02, pw2 * .8), 'hazard', 'detail')
    elif kind == 'flag':
        fh, fw = h * .3, max(w, d) * .9
        cloth(k, (pw / 2, 0, h - .3), (pw / 2 + fw, 0, h - .5), (pw / 2 + fw, 0, h - .3 - fh), (pw / 2, 0, h - .3 - fh), 6, 4, .2, 'canvas.main', sagdir=(0, -1, 0), removable=True)
    elif kind == 'mast':
        with k.tagged('antenna:0'):
            k.cyl((0, 0, h * .96), min(w, d) * .3, .08, 'paint.trim', 'frame', sides=12)
            k.beam((0, 0, h * .9), (0, -d * .3, h * .99), .06, 'metal', 'frame')
    elif kind == 'alarm':
        k.box((0, -.1, top + .1), (pw * 2.5, pw * 2, pw * 2), 'lamp_red', 'detail', ch=.03)
        add_socket(k, 'alarm_01', 'point', (0, -.1, top + .1), (0, -1, 0), 'window')
        k.meta['sockets'][-1]['colour_linear'] = (1.0, .04, .02)
    k.moss_sites.append(((-.4, -.4, .02), (.4, -.4, .02)))
    k.hits.append(dict(p=(0, 0, h * .4), d=(1, 0, 0), r=max(1.0, h * .3), depth=0, sag=0))


# ------------------------------------------------------------------ tower (lookout / sensor / control / relay / gantry)
def tower(k, p):
    """Cast base -> tapered corner columns + hidden ring girts + core -> riveted plates (or lattice bracing) -> cabin frame + windows -> roof."""
    w, d, h = p['size']
    g = p.get('dims', {})
    zb = g.get('base_h', min(h * .22, 4.5))
    cab_h = g.get('cab_h', min(h * .16, 3.2))
    z0, z1 = zb + .3, h - cab_h
    topf = g.get('top_frac', .75)
    W0, D0 = w * .6, d * .6
    W1, D1 = W0 * topf, D0 * topf
    cw0, cw1 = max(.5, W0 * .16), max(.4, W1 * .15)
    lattice = bool(g.get('lattice'))
    nd, nw = n_of(p, 'doors', 0), n_of(p, 'windows', 0)
    kit.loft(k, (-w / 2, -d / 2, w / 2, d / 2), 0, (-W0 / 2 - .5, -D0 / 2 - .5, W0 / 2 + .5, D0 / 2 + .5), zb, 'ferro', 'mass', ch0=.3, ch1=.22, rebar=True, part='footing')
    k.box((0, 0, zb + .15), (W0 + 1.1, D0 + 1.1, .3), 'paint.trim', 'mass', ch=C(.15), top=C(.05))
    H = z1 - z0
    if not lattice:
        kit.loft(k, (-W0 / 2 + .45, -D0 / 2 + .45, W0 / 2 - .45, D0 / 2 - .45), z0, (-W1 / 2 + .45, -D1 / 2 + .45, W1 / 2 - .45, D1 / 2 - .45), z1, 'paint.trim', 'mass')
    for sx in (-1, 1):
        for sy in (-1, 1):
            def rr(W, D_, cw):
                xa, xb = sorted((sx * W / 2, sx * (W / 2 - cw)))
                ya, yb = sorted((sy * D_ / 2, sy * (D_ / 2 - cw)))
                return (xa, ya, xb, yb)
            kit.loft(k, rr(W0, D0, cw0), z0, rr(W1, D1, cw1), z1, 'paint.main', 'frame', ch0=.1)
    girts = [z0 + H * f for f in (.25, .5, .75)]
    for gz in girts:
        f = (gz - z0) / H
        W, D_ = W0 + (W1 - W0) * f - .5, D0 + (D1 - D0) * f - .5
        cs = [(-W / 2, -D_ / 2, gz), (W / 2, -D_ / 2, gz), (W / 2, D_ / 2, gz), (-W / 2, D_ / 2, gz)]
        for a, b in zip(cs, cs[1:] + cs[:1]):
            kit.cee(k, a, b, d=.16, w=.06)
    # plates (skin) between the girts on each face
    faces = (((0, -D0 / 2), 0.0, W0, W1, D0, D1), ((W0 / 2, 0), math.pi / 2, D0, D1, W0, W1), ((0, D0 / 2), math.pi, W0, W1, D0, D1), ((-W0 / 2, 0), -math.pi / 2, D0, D1, W0, W1))
    cuts = [z0] + girts + [z1]
    for (ox, oy), yaw, fw0, fw1, dd0, dd1 in faces:
        bat = (dd0 - dd1) / 2 / H
        with k.at((ox, oy, 0), yaw=yaw):
            for za, zt in zip(cuts, cuts[1:]):
                fa, ft = (za - z0) / H, (zt - z0) / H
                ha, ht = fw0 / 2 - (fw0 - fw1) / 2 * fa - cw0 * (1 - fa) - cw1 * fa, fw0 / 2 - (fw0 - fw1) / 2 * ft - cw0 * (1 - ft) - cw1 * ft
                dy = (dd0 - dd1) / 2
                if lattice:
                    kit.ibeam(k, (-ha, dy * fa, za), (ht, dy * ft, zt), d=.12, w=.08)
                    kit.ibeam(k, (ha, dy * fa, za), (-ht, dy * ft, zt), d=.12, w=.08)
                else:
                    kit.plate(k, -ha, ha, -ht, ht, za + .02, zt - .02, dy * fa + .02, dy * fa + .10, 'paint.second', 'skin', removable=True)
    for i in range(nd):
        side = faces[i % 4]
        (ox, oy), yaw = side[0], side[1]
        with k.at((ox, oy, 0), yaw=yaw):
            with k.tagged(f'door:{i}'):
                k.slab(-.9, .9, 0, 2.9, -.14, .05, 'paint.corr', 'frame', front=C(.03))
                k.slab(-.72, .72, .05, 2.8, -.02, .06, 'paint.corr', 'skin', door=True)
                k.lamp_head(0, 3.3, -.02, w=.4)
            add_socket(k, f'door_{i + 1:02}', 'spot', (ox + math.sin(yaw) * .5, oy - math.cos(yaw) * .5, 3.3), (math.sin(yaw), -math.cos(yaw), -.6), 'door')
    # cabin
    cz = z1
    k.box((0, 0, cz + .15), (W0 + .25, D0 + .25, .30), 'paint.main', 'frame', ch=C(.12), top=C(.04))
    k.box((0, 0, cz + cab_h / 2), (W0 - .6, D0 - .6, cab_h - .3), 'paint.trim', 'mass')
    for sx in (-1, 1):
        for sy in (-1, 1):
            k.box((sx * (W0 / 2 - .25), sy * (D0 / 2 - .25), cz + cab_h / 2), (.5, .5, cab_h), 'paint.main', 'frame', ch=C(.06))
    per_side = max(1, math.ceil(nw / 4)) if nw else 0
    widx = 0
    for si, (o, yaw, length) in enumerate((((0, -D0 / 2), 0.0, W0), ((W0 / 2, 0), math.pi / 2, D0), ((0, D0 / 2), math.pi, W0), ((-W0 / 2, 0), -math.pi / 2, D0))):
        with k.at((o[0], o[1], cz), yaw=yaw):
            hl = length / 2 - .5
            wz0, wz1 = cab_h * .32, cab_h * .78
            k.slab(-hl, hl, .30, wz0 - .05, -.06, .12, 'paint.main', 'skin', front=C(.03), removable=True)
            k.slab(-hl, hl, wz1 + .05, cab_h, -.06, .12, 'paint.main', 'skin', front=C(.03), removable=True)
            m = min(per_side, nw - widx) if nw else 0
            if m:
                step = (2 * hl - .5) / m
                for j in range(m):
                    x0 = -hl + .25 + j * step + .14
                    x1 = -hl + .25 + (j + 1) * step - .14
                    with k.tagged(f'window:{widx}'):
                        k.slab(x0 - .05, x1 + .05, wz0 - .05, wz1 + .05, -.10, .05, 'paint.corr', 'frame', front=C(.02))
                        k.slab(x0, x1, wz0, wz1, .10, .13, 'glass', 'glass', pane=True)
                    pos = (o[0] + (x0 + x1) / 2 * math.cos(yaw) + .05 * math.sin(yaw), o[1] + (x0 + x1) / 2 * math.sin(yaw) - .05 * math.cos(yaw), cz + (wz0 + wz1) / 2)
                    add_socket(k, f'window_{widx + 1:02}', 'point', pos, (math.sin(yaw), -math.cos(yaw), 0), 'window')
                    widx += 1
    k.box((0, 0, h - .03), (W0 + .5, D0 + .5, .40), 'paint.main', 'roof', ch=C(.15), top=C(.05))
    if g.get('mast'):
        with k.tagged('antenna:0'):
            k.beam((W0 * .2, 0, h), (W0 * .2, 0, h + min(4, h * .15)), .12, 'metal', 'frame')
    k.hits.append(dict(p=(0, -D0 / 2, h * .5), d=(0, 1, 0), r=max(2.0, W0 * .35), depth=0, sag=.15))
    k.hits.append(dict(p=(-W0 / 3, -D0 / 3, h * .9), d=(0, 1, 0), r=max(1.4, W0 * .25), depth=0, sag=.1))


# ------------------------------------------------------------------ junk pile
def junk(k, p):
    w, d, h = p['size']
    rng = random.Random(7 + int(w * 13 + d * 7))
    n = int(D(p, 'pieces', 14))
    kit.mound(k, w * .8, d * .8, h * .35, 'ferro', n=14, rings=4, power=.7)
    k.pieces[-1]['part'] = 'footing'
    for i in range(n):
        x, y = rng.uniform(-w * .38, w * .38), rng.uniform(-d * .38, d * .38)
        z = h * .3 * max(0.0, 1 - (x / (w * .5)) ** 2 - (y / (d * .5)) ** 2)
        kind = i % 3
        with k.tagged(f'panel:{i}'):       # one tag per scrap piece: panels_sections = pieces
            _junk_piece(k, rng, kind, x, y, z, w, d, h)
    k.hits.append(dict(p=(0, 0, h * .4), d=(0, 0, -1), r=max(w, d) * .3, depth=0, sag=0))


def _junk_piece(k, rng, kind, x, y, z, w, d, h):
    if True:
        if kind == 0:
            ln = rng.uniform(.25, .6) * max(w, d)
            yaw = rng.uniform(-math.pi, math.pi)
            a = (x - math.cos(yaw) * ln / 2, y - math.sin(yaw) * ln / 2, z + rng.uniform(.1, h * .4))
            b = (x + math.cos(yaw) * ln / 2, y + math.sin(yaw) * ln / 2, z + rng.uniform(.1, h * .5))
            kit.ibeam(k, a, b, d=.28, w=.16, mat='metal', removable=True)
        elif kind == 1:
            with k.at((x, y, z + rng.uniform(.1, h * .3)), yaw=rng.uniform(-3, 3), pitch=rng.uniform(-.5, .5), roll=rng.uniform(-.4, .4)):
                k.box((0, 0, 0), (rng.uniform(.8, 2.2), rng.uniform(.8, 2.0), .12), 'paint.second', 'skin', removable=True)
        else:
            kit.rock(k, (x, y, z), rng.uniform(.6, 1.6), 'ferro', rng)
            k.pieces[-1]['removable'] = True


# ------------------------------------------------------------------ bridge
def bridge(k, p):
    w, d, h = p['size']
    along_y = d >= w
    L, Wd = (d, w) if along_y else (w, d)
    spans = int(D(p, 'spans', max(2, round(L / 8))))
    deck_z = D(p, 'deck_z', h * .55)
    rail = h - deck_z - .15
    with k.at((0, 0, 0), yaw=0.0 if along_y else -math.pi / 2):
        for sy in (-1, 1):
            k.box((0, sy * (L / 2 - 1.2), deck_z / 2), (Wd + .6, 2.4, deck_z), 'ferro', 'mass', ch=C(.08), top=C(.05), part='footing')
        for sx in (-1, 1):
            kit.ibeam(k, (sx * Wd * .38, -L / 2 + 1.2, deck_z - .45), (sx * Wd * .38, L / 2 - 1.2, deck_z - .45), d=.9, w=.35)
        for q in range(spans + 1):
            y = -L / 2 + 1.2 + (L - 2.4) * q / spans
            with k.tagged(f'support:{q}') if 0 < q < spans else k.tagged(None):
                k.beam((-Wd * .38, y, deck_z - .45), (Wd * .38, y, deck_z - .45), .3, 'metal', 'frame', h=.5)
                if 0 < q < spans:
                    for sx in (-1, 1):
                        k.box((sx * Wd * .3, y, deck_z * .5 - .2), (.7, .7, deck_z - .5), 'ferro', 'mass', ch=C(.05), part='footing')
        for q in range(spans):
            y0 = -L / 2 + 1.2 + (L - 2.4) * q / spans
            y1 = -L / 2 + 1.2 + (L - 2.4) * (q + 1) / spans
            k.box((0, (y0 + y1) / 2, deck_z - .05), (Wd - .4, (y1 - y0) - .1, .22), 'paint.main', 'skin', ch=C(.02), removable=True, deck=q)
        for sx in (-1, 1):
            for q in range(spans * 2 + 1):
                y = -L / 2 + 1.2 + (L - 2.4) * q / (spans * 2)
                k.box((sx * (Wd / 2 - .15), y, deck_z + rail / 2), (.14, .14, rail), 'paint.trim', 'frame', ch=C(.02))
            for z in (deck_z + rail * .5, deck_z + rail - .06):
                k.beam((sx * (Wd / 2 - .15), -L / 2 + 1.2, z), (sx * (Wd / 2 - .15), L / 2 - 1.2, z), .1, 'metal', 'skin', h=.08, removable=True)
    k.moss_sites.append(((-Wd / 2, -L / 2, .05), (Wd / 2, -L / 2, .05)))
    k.hits.append(dict(p=(0, 0, deck_z), d=(0, 0, -1), r=max(3.0, Wd * .5), depth=0, sag=.5))


# ------------------------------------------------------------------ pipe runs
def pipe(k, p):
    w, d, h = p['size']
    layout = D(p, 'layout', 'straight')
    r = D(p, 'radius', max(.3, min(h * .3, min(w, d) * .3)))
    pl = D(p, 'plinth_gap', 6.0)
    zc = h - r - .1
    runs = []
    if layout == 'straight':
        runs = [((0, -d / 2 + .1), (0, d / 2 - .1))] if d >= w else [((-w / 2 + .1, 0), (w / 2 - .1, 0))]
    elif layout == 'corner':
        runs = [((-w / 2 + r, -d / 2 + .1), (-w / 2 + r, d / 2 - r)), ((-w / 2 + r, d / 2 - r), (w / 2 - .1, d / 2 - r))]
    elif layout == 'tee':
        runs = [((-w / 2 + .1, 0), (w / 2 - .1, 0)), ((0, 0), (0, d / 2 - .1))]
    elif layout == 'cross':
        runs = [((-w / 2 + .1, 0), (w / 2 - .1, 0)), ((0, -d / 2 + .1), (0, d / 2 - .1))]
    elif layout == 'end':
        runs = [((0, -d / 2 + .1), (0, d / 2 - .1))]
    ids = 0
    for (x0, y0), (x1, y1) in runs:
        ln = math.hypot(x1 - x0, y1 - y0)
        n = max(1, int(ln / pl))
        for q in range(n + 1):
            f = q / n
            x, y = x0 + (x1 - x0) * f, y0 + (y1 - y0) * f
            with k.tagged(f'support:{ids}'):
                k.box((x, y, (zc - r) / 2), (r * 2.4, r * 2.4, zc - r), 'ferro', 'mass', ch=C(.06), top=C(.04), part='footing')
                k.box((x, y, zc - r * .9), (r * 2.2 if abs(x1 - x0) < 1e-6 else .4, r * 2.2 if abs(y1 - y0) < 1e-6 else .4, .12), 'paint.trim', 'frame')
            ids += 1
        segs = n * 2
        for q in range(segs):
            a = (x0 + (x1 - x0) * q / segs, y0 + (y1 - y0) * q / segs, zc)
            b = (x0 + (x1 - x0) * (q + 1) / segs, y0 + (y1 - y0) * (q + 1) / segs, zc)
            kit.wire(k, [a, b], r, 'paint.main', 'skin', sides=10, removable=True, run=q)
            if q:
                kit.wire(k, [(a[0] - (b[0] - a[0]) * .03, a[1] - (b[1] - a[1]) * .03, zc), (a[0] + (b[0] - a[0]) * .03, a[1] + (b[1] - a[1]) * .03, zc)], r * 1.18, 'paint.trim', 'frame', sides=10)
    if layout in ('corner', 'tee', 'cross'):
        c = runs[0][1] if layout == 'corner' else (0, 0)
        k.cyl((c[0], c[1], zc), r * 1.2, r * 2.4, 'paint.trim', 'frame', sides=12)
    k.hits.append(dict(p=(0, 0, zc), d=(0, 0, -1), r=max(2.0, r * 3), depth=0, sag=.2))


# ------------------------------------------------------------------ fence family (razor / chain-link / wood / sandbag / sheet / barricade)
def fence(k, p):
    """Footings and posts -> rails -> infill by style as separate removable sections between the posts."""
    w, d, h = p['size']
    style = D(p, 'style', 'chain')
    if style == 'razor' and D(p, 'legacy', True) and abs(d - 10.78) < .5 and abs(h - 6.84) < .5:
        return kit.fence(k)                        # the approved razorwirefence geometry, unchanged
    long_y = d >= w
    L = d if long_y else w
    T = max(w if long_y else d, .1)
    with k.at((0, 0, 0), yaw=0.0 if long_y else -math.pi / 2):
        posts = int(max(2, n_of(p, 'legs_supports', 3)))
        ys = [-L / 2 + .1 + (L - .2) * i / (posts - 1) for i in range(posts)]
        if style == 'sandbag':
            rows = max(2, int(h / .27))
            bw = L / max(4, int(L / 1.05))
            for r in range(rows):
                off = (r % 2) * bw / 2
                n = int(L / bw)
                for i in range(n):
                    y = -L / 2 + (i + .5) * bw + off - (bw if (i == n - 1 and r % 2) else 0)
                    k.box((0, max(-L / 2 + bw / 2, min(L / 2 - bw / 2, y)), .13 + r * .25), (T * (1 - .08 * r / rows), bw - .05, .26), 'canvas.second', 'mass', ch=C(.07), top=C(.08), removable=r > 0, part='footing' if r == 0 else None, sandbag=(r, i))
            with k.tagged('panel:0'):
                pass
        elif style == 'sheet':
            for i, y in enumerate(ys):
                k.box((0, y, h / 2), (.14, .14, h), 'metal', 'frame', ch=C(.02), part='footing')
            for q in range(posts - 1):
                with k.tagged(f'panel:{q}'):
                    y0, y1 = ys[q] + .05, ys[q + 1] - .05
                    for r in range(3):
                        k.slab(-.04, .04, .3 + r * (h - .5) / 3, .3 + (r + 1) * (h - .5) / 3 - .04, y0 + (.03 if r % 2 else 0), y1, 'paint.second', 'skin', removable=True, front=0)
        else:
            for i, y in enumerate(ys):
                with k.tagged(f'support:{i}'):
                    footing_box(k, (0, y, .12), (.5, .5, .24)) if style != 'wood' else k.box((0, y, .1), (.3, .3, .2), 'ferro', 'mass', part='footing')
                    k.box((0, y, .24 + (h - .24) / 2), (.14 if style == 'wood' else .2, .14 if style == 'wood' else .2, h - .24), 'wood.second' if style == 'wood' else 'paint.trim', 'frame', ch=C(.02))
            rails = (h * .25, h * .6, h * .92) if style == 'wood' else (.3, h - .1)
            for z in rails:
                kit.wire(k, [(0, ys[0], z), (0, ys[-1], z)], .04, 'wood.second' if style == 'wood' else 'paint.trim', 'frame', sides=6)
            for q in range(posts - 1):
                y0, y1 = ys[q] + .12, ys[q + 1] - .12
                with k.tagged(f'panel:{q}'):
                    if style == 'wood':
                        nb = max(4, int((y1 - y0) / .16))
                        for b in range(nb):
                            k.box((.06, y0 + (b + .5) * (y1 - y0) / nb, h * .5), (.03, (y1 - y0) / nb - .02, h * .86), 'wood.main', 'skin', removable=True, board=(q, b))
                    else:
                        step = max(.22, (h - .4) / 4)
                        n = int((y1 - y0) / step)
                        for sgn in (1, -1):
                            for b in range(-int((h - .4) / step) - 1, n + 2):
                                ya = (y0 if sgn > 0 else y1) + sgn * b * step
                                yb = ya + sgn * (h - .4)
                                lo = min((y0 - ya) / (yb - ya), (y1 - ya) / (yb - ya))
                                hi = max((y0 - ya) / (yb - ya), (y1 - ya) / (yb - ya))
                                lo, hi = max(0.0, lo), min(1.0, hi)
                                if hi - lo < .08:
                                    continue
                                pa = (0, ya + (yb - ya) * lo, .3 + (h - .4) * lo)
                                pb = (0, ya + (yb - ya) * hi, .3 + (h - .4) * hi)
                                kit.wire(k, [pa, pb], .012, 'metal', 'skin', sides=4, removable=True)
    k.moss_sites.append(((0, -L / 2, .02), (0, L / 2, .02)))
    k.hits.append(dict(p=(0, 0, h * .5), d=(1, 0, 0), r=max(1.5, L * .2), depth=0, sag=0))


def fence_damage(k, condition):
    if D(k.meta.get('params', {}), 'style') == 'sandbag' or any('sandbag' in p for p in k.pieces):
        return collapse(k, condition)
    kit.fence_damage(k, condition)


# ------------------------------------------------------------------ generic damage: collapse (partial = peel, destroyed = crushed heap)
def collapse(k, condition):
    w, d, h = k.meta['size']
    if condition == 'partial':
        return kit.peel(k, k.seed)
    rng = random.Random(k.seed * 29 + 5)
    sc = max(.12, min(1.0, min(w, d, h * 1.2) / 10))
    f = .24
    out = []
    for pc in k.pieces:
        if pc.get('pane'):
            continue
        if pc.get('removable') and rng.random() < .45 and 'uid' in pc:
            c = geo.centroid(pc)
            at = (c[0] * 1.15 + rng.uniform(-.5, .5) * sc, c[1] * 1.15 + rng.uniform(-.5, .5) * sc, .06)
            geo.lay_flat(k, pc, at, rng.uniform(-math.pi, math.pi), rng, crumple=.05)
        else:
            tilt = rng.uniform(-.25, .25)
            ph = rng.uniform(0, 6)
            pc['v'] = [(x * (1 + .05 * math.sin(z + ph)), y + z * tilt * .25, max(.02, z * f + .05 * math.sin(x * 2.1 + y * 1.7 + ph))) for x, y, z in pc['v']]
        out.append(pc)
    k.pieces = out
    kit.mound(k, w * .75, d * .75, h * .16, 'char', n=14, rings=4, power=.7)
    for i in range(int(8 + 14 * sc)):
        x, y = rng.uniform(-w * .4, w * .4), rng.uniform(-d * .4, d * .4)
        kit.rock(k, (x, y, .08), rng.uniform(.25, .8) * max(.4, sc * 1.6), 'ferro', rng, flat=.7)
    for pc in k.pieces:
        pc['v'] = [(x, y, max(.01, z)) for x, y, z in pc['v']]
    k.hits = [dict(p=(0, 0, .3), d=(0, 0, -1), r=min(w, d) * .3, depth=0, sag=0)]
    k.meta.update(collapse='crushed-heap')


BUILD = dict(tent=tent, container=container, crate=crates, barrel=barrels, tank=tank, turret=turret, vehicle=vehicle, pylon=pylon,
             tower=tower, junk=junk, bridge=bridge, pipe=pipe, fence=fence)
DAMAGE = dict(collapse=collapse, fence=fence_damage, crate=collapse, barrel=collapse)
# count fields each archetype tags in the model (the validator recounts exactly these)
TAGGED = dict(box=('doors', 'windows', 'openings_bays'), tent=('doors',), container=('crates', 'doors'), crate=('crates',), barrel=('barrels',),
              tank=('tanks',), tower=('doors', 'windows', 'antennas_dishes'), pylon=('legs_supports', 'antennas_dishes'),
              fence=('legs_supports', 'panels_sections'), wall=('legs_supports', 'panels_sections'), gate=('panels_sections',),
              pipe=('legs_supports',), bridge=('legs_supports',), turret=('legs_supports',), vehicle=('legs_supports',), junk=('panels_sections',),
              quonset=('arches_ribs',))
