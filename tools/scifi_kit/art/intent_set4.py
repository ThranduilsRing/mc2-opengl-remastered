"""Intent pass, industrial round 2: depots, warehouses and yard structures.

grainstorage, civstoragea/b/c, gantry, crane. Same family language and access rules.
"""
import math

import geo
import ind_parts as ip
import intent as I
import intent_set3 as S

count = I.count


# ------------------------------------------------------------------ grainstorage
def grainstorage(k):
    x0, x1, y0, y1 = -10.5, 10.5, -5.2, 5.2
    yf = -2.8                                     # facade line behind the silo row
    zf, zb_ = 15.5, 18.9
    top = lambda x, y: zf + (zb_ - zf) * (y - y0) / (y1 - y0)  # noqa: E731
    k.box((0, 0, .15), (21.2, 10.6, .3), 'ferro', 'mass', ch=.1, walk=True)
    zb = .3
    S.shed(k, x0, yf, x1, 4.4, 19.0, sides='wne', zb=zb, top=top, openings={
        'n': [('rollup', -4.5, zb, 4.2, 4.8), ('rollup', 4.5, zb, 4.2, 4.8)],
        'w': [('door', 1.0, zb, 1.2, 2.3), ('window', 1.0, 9.5, 1.6, .9)], 'e': [('door', -1.0, zb, 1.2, 2.3), ('window', -1.0, 9.5, 1.6, .9)]})
    S.shed(k, x0, yf, x1, 4.4, 19.0, sides='s', zb=13.7, top=top)
    k.prism([(y0 - .3, zf - .05), (y1 + .3, zb_ - .05), (y1 + .3, zb_ + .2), (y0 - .3, zf + .2)], x0 - .25, x1 + .25, 'paint.roof', 'roof', axis='x')
    for x in ip.frange(x0, x1, .7):
        k.beam((x, y0 - .3, zf + .22), (x, y1 + .3, zb_ + .22), .04, 'paint.roof', 'detail', h=.04)
    # four steel silos built into the front, conveyor gallery across their heads
    for cx in (-7.8, -2.6, 2.6, 7.8):
        c = (cx, yf)
        ip.lathe(k, [(2.55, .3), (2.55, .7), (0, .7)], 'ferro', 'mass', n=48, c=c)
        ip.vessel(k, c, 2.4, 1.6, 12.0, top='flat', bottom='cone', courses=5, mat='corr.second', head_mat='paint.second', n=48)
        for a in (math.radians(200), math.radians(270), math.radians(340)):
            p = ip.polar(c, 2.1, a, 0)
            ip.hbeam(k, (p[0], p[1], .7), (p[0], p[1], 1.9), .2, .2)
        k.cyl((cx, yf - 1.8, 1.1), .25, .9, 'paint.second', 'frame', sides=12)
        count(k, 'tanks')
    k.box((0, (y0 + yf) / 2 - .05, 13.7), (x1 - x0, abs(yf - y0) + .2, 3.4), 'paint.second', 'mass', ch=.08)
    for x in ip.frange(x0 + .6, x1 - .6, .9):
        k.box((x, y0 - .07, 12.85), (.5, .06, 1.0), 'grate', 'frame')
    k.box((0, y0 - .1, 13.75), (x1 - x0 + .2, .3, .12), 'paint.trim', 'frame')
    S.roof_pipes(k, [(-9.5, -4.0, 15.1), (9.5, -4.0, 15.1)], r=.2, z=13.7)


# ------------------------------------------------------------------ civstoragea
def civstoragea(k):
    x0, x1, y0, y1 = -4.8, 4.8, -7.4, 7.4
    eave, ridge = 4.3, 5.3
    xs = [x0, x0 + 3.2, x0 + 6.4, x1]

    def top(x, y):
        for a, b in zip(xs, xs[1:]):
            if a - 1e-6 <= x <= b + 1e-6:
                m = (a + b) / 2
                return ridge - (ridge - eave) * abs(x - m) / ((b - a) / 2)
        return eave
    k.box((0, 0, .1), (9.8, 16.6, .2), 'ferro', 'mass', ch=.06, walk=True)
    zb = .2
    S.shed(k, x0, y0, x1, y1, ridge, zb=zb, top=top, bay=2.7, mat='corr.main', openings={
        's': [('rollup', 0.0, zb, 3.0, 3.0)], 'n': [('rollup', 0.0, zb, 3.0, 3.0)],
        'e': [('window', -3.5, 2.2, 3.0, .7), ('window', 3.5, 2.2, 3.0, .7)], 'w': [('window', -3.5, 2.2, 3.0, .7), ('window', 3.5, 2.2, 3.0, .7)]})
    for a, b in zip(xs, xs[1:]):
        m = (a + b) / 2
        for sgn in (-1, 1):
            e = a if sgn < 0 else b
            k.prism([(e - sgn * .25 if sgn < 0 else m, eave - .08 if sgn < 0 else ridge - .05), (m if sgn < 0 else e + .25, ridge - .05 if sgn < 0 else eave - .08),
                     (m if sgn < 0 else e + .25, ridge + .12 if sgn < 0 else eave + .09), (e - .25 if sgn < 0 else m, eave + .09 if sgn < 0 else ridge + .12)],
                    y0 - .3, y1 + .3, 'paint.roof', 'roof', axis='y')
        k.box((m, 0, ridge + .14), (.3, y1 - y0 + .6, .12), 'paint.trim', 'frame')
    for x in (x0 + 3.2, x0 + 6.4):
        k.box((x, 0, eave - .05), (.4, y1 - y0 + .6, .2), 'paint.second', 'frame')       # valley gutters
    for x, y, f in ((x1, -6.5, 'e'), (x1, 6.5, 'e'), (x0, -6.5, 'w'), (x0, 6.5, 'w')):
        ip.lamp(k, x, y, 3.6, I.YAW[f], k.meta['sockets'], 'wall')
    for x, y, f in ((x0 + .3, y0, 's'), (x1 - .3, y1, 'n')):
        ip.downpipe(k, x, y, .2, eave - .1, I.YAW[f])


# ------------------------------------------------------------------ civstorageb
def civstorageb(k):
    x0, x1, y0, y1 = -3.6, 3.0, -9.2, 9.2
    lo, hi = 5.4, 6.7
    bays = [(-9.2, -3.07), (-3.07, 3.07), (3.07, 9.2)]

    def top(x, y):
        for a, b in bays:
            if a - 1e-6 <= y <= b + 1e-6:
                return hi - (hi - lo) * (y - a) / (b - a)
        return lo
    k.box((-.3, 0, .1), (6.8, 18.6, .2), 'ferro', 'mass', ch=.06, walk=True)
    zb = .2
    dock = 1.1
    k.box((3.75, 0, dock / 2), (1.5, 18.4, dock), 'ferro', 'mass', ch=.06, top=.04, walk=True)        # loading dock along the east face
    k.box((3.75, 0, dock + .03), (1.5, 18.4, .06), 'hazard', 'skin')
    S.shed(k, x0, y0, x1, y1, hi, sides='wns', zb=zb, top=top, bay=3.07, mat='corr.rust', openings={
        's': [('window', 0.0, 4.7, 6.0, .9)], 'w': [('door', 0.0, zb, 1.2, 2.3)]})
    S.shed(k, x0, y0, x1, y1, hi, sides='e', zb=dock, top=top, bay=3.07, mat='corr.rust', openings={
        'e': [('rollup', -6.13, dock, 3.2, 3.2), ('rollup', 0.0, dock, 3.2, 3.2), ('rollup', 6.13, dock, 3.2, 3.2)]})
    for a, b in bays:
        k.prism([(a - .15, hi - .02), (b + .15, lo - .1), (b + .15, lo + .12), (a - .15, hi + .2)], x0 - .25, x1 + .25, 'corr.rust', 'roof', axis='x')
        if a > -9.0:      # clerestory glazing in each saw's vertical face
            with k.at((0, a, 0)):
                k.slab(x0 + .4, x1 - .4, lo + .15, hi - .05, -.04, .02, 'glass', 'glass', pane=True)
    for yy, d in ((-9.2, -1), (9.2, 1)):         # dock steps at both ends
        for i in range(3):
            k.box((3.75, yy + d * (.85 - i * .3), .18 + i * .36), (1.2, .3, .36 + i * .0), 'ferro', 'mass', walk=True)
    k.box((4.6, 7.6, 1.9), (.4, 1.0, 1.4), 'paint.second', 'frame', ch=.04)       # switch cabinet
    for x, y, f in ((x0, -7.0, 'w'), (x0, 7.0, 'w')):
        ip.lamp(k, x, y, 3.4, I.YAW[f], k.meta['sockets'], 'wall')


# ------------------------------------------------------------------ civstoragec
def civstoragec(k):
    cx0, cx1, cy0, cy1 = -3.3, 3.3, -.6, 4.7
    zf = 3.1
    hi, lo = 7.4, 6.1
    top = lambda x, y: hi - (hi - lo) * (x - cx0) / (cx1 - cx0)  # noqa: E731
    # timber stilts, X-braced, on concrete pads; joists and plank floor
    for x in (cx0 + .25, 0.0, cx1 - .25):
        for y in (cy0 + .25, cy1 - .25):
            k.box((x, y, .1), (.5, .5, .2), 'ferro', 'mass', ch=.05, walk=True)
            k.box((x, y, (zf + .2) / 2), (.25, .25, zf - .2), 'wood', 'frame')
    for y in (cy0 + .25, cy1 - .25):
        for xa, xb in ((cx0 + .25, 0.0), (0.0, cx1 - .25)):
            k.beam((xa, y, .4), (xb, y, zf - .3), .12, 'wood', 'frame', h=.08)
            k.beam((xb, y, .4), (xa, y, zf - .3), .12, 'wood', 'frame', h=.08)
    for x in (cx0 + .25, cx1 - .25):
        k.beam((x, cy0 + .25, .4), (x, cy1 - .25, zf - .3), .12, 'wood', 'frame', h=.08)
    k.box((0, (cy0 + cy1) / 2, zf - .1), (6.7, 5.4, .2), 'wood', 'mass', walk=True)
    # cabin: vertical boards, mono-pitch tin roof, door to the landing, window east
    F = I.faces_of(cx0, cy0, cx1, cy1, .12)
    for side in 'nesw':
        origin, yaw, L, _ = F[side]
        tf = S._local_top(side, F, top)
        with k.at(origin, yaw=yaw):
            holes = [(-.5, zf, .5, zf + 2.1)] if side == 's' else ([(-.6, zf + 1.2, .6, zf + 2.0)] if side == 'e' else [])
            n = max(2, int(L / .22))
            for i in range(n):
                xa, xb = -L / 2 + L * i / n + .005, -L / 2 + L * (i + 1) / n - .005
                for r in geo.subtract_rect((xa, zf, xb, min(tf(xa), tf(xb))), holes):
                    k.slab(r[0], r[2], r[1], r[3], -.02 * (i % 2), .12, 'wood', 'skin')
            if side == 's':
                k.slab(-.48, .48, zf + .02, zf + 2.08, .03, .08, 'paint.door', 'skin', door=True)
                k.slab(-.6, .6, zf + 2.1, zf + 2.25, -.06, .12, 'wood', 'frame')
                count(k, 'doors')
            if side == 'e':
                ip.steel_window(k, -.6, .6, zf + 1.2, zf + 2.0, 0.0, cols=2, rows=1)
    k.prism([(cx0 - .4, hi - .02), (cx1 + .4, lo - .1), (cx1 + .4, lo + .06), (cx0 - .4, hi + .14)], cy0 - .4, cy1 + .4, 'corr.roof', 'roof', axis='y')
    # landing at the door and a timber stair down to grade
    ip.rect_platform(k, -1.0, cy0 - 1.2, 1.0, cy0, zf, rail_sides='ew', legs=True, deck='wood', name='door landing')
    ip.stair(k, (0.0, -4.6, 0.0), (0.0, cy0 - 1.2, zf), w=.85, mat='wood', name='cabin stair')
    count(k, 'stairs_ladders')


# ------------------------------------------------------------------ gantry
def gantry(k):
    k.box((-.5, 0, .55), (21.5, 25.4, 1.1), 'ferro', 'mass', ch=.15, top=.08, walk=True)
    zp = 1.1
    tc, tw = (1.5, -5.5), 7.0
    tx0, tx1, ty0, ty1 = tc[0] - tw / 2, tc[0] + tw / 2, tc[1] - tw / 2, tc[1] + tw / 2
    zm, zt, ze = 17.6, 29.0, 26.0
    ip.lattice(k, tc, tw, zp, zt, panel=3.0, legs=.4)
    # equipment house in the tower base (khaki cladding, louvres, door) and intermediate grating floors
    k.box((tc[0], tc[1], zp + 3.0), (tw - .9, tw - .9, 6.0), 'paint.main', 'mass', ch=.1, top=.06)
    k.box((tc[0], tc[1], zp + 6.05), (tw - .7, tw - .7, .15), 'paint.trim', 'frame')
    with k.at((tc[0] + 1.6, ty0 + .45, zp)):
        ip.personnel_door(k, 0, 1.2, 2.3, 0.0, canopy=False, apron=False)
    count(k, 'doors')
    for j in range(6):
        k.box((tx0 + .43, tc[1] - 1.5 + j * .5, zp + 4.0), (.06, .4, .1), 'grate', 'frame')
    for zf in (12.3, 23.3):
        k.box((tc[0], tc[1], zf - .04), (tw - .9, tw - .9, .08), 'grate', 'frame', walk=True)
    ip.tube(k, [(tx0 + .5, ty1 - .5, zp + 6.1), (tx0 + .5, ty1 - .5, zt - .2)], .12, 'paint.accent', 'frame', sides=8)
    ip.tube(k, [(tx0 + .8, ty1 - .5, zp + 6.1), (tx0 + .8, ty1 - .5, zt - .2)], .09, 'paint.pipe', 'frame', sides=8)
    # cabin-level deck inside the bracing (no rails needed inside the lattice), east balcony outside it
    k.box((tc[0], tc[1], zm - .04), (tw - .9, tw - .9, .08), 'grate', 'frame', walk=True)
    ip.rect_platform(k, tx1 + .35, ty0 + .6, tx1 + 1.9, ty1 - .6, zm, rail_sides='nes', legs=False, name='east balcony')
    k.box((tx1 + .1, tc[1], zm - .04), (.6, tw - 1.4, .08), 'grate', 'frame', walk=True)
    ip.rect_platform(k, tx0 - .3, ty0 - .3, tx1 + .3, ty1 + .3, zt, rail_sides='esw', legs=False,
                     gaps=[(tx1 + .3, tc[1] - 1.6), (tx1 + .3, tc[1] + 1.6)], name='top deck')
    ip.rect_platform(k, tx1 + .4, tc[1] + .8, 11.3, tc[1] + 4.6, ze, rail_sides='nes', legs=False, name='east service platform')
    for y in (tc[1] + 1.1, tc[1] + 4.3):
        k.beam((tx1, y, ze - 2.5), (11.0, y, ze - .2), .2, ip.STEEL, 'frame', h=.2)
    # north service arm: truss carrying a railed walkway off the top deck
    ay0, ay1 = ty1 + .3, 12.5
    k.box((tc[0], (ay0 + ay1) / 2, zt - .04), (4.0, ay1 - ay0, .08), 'grate', 'frame', walk=True)
    ip.railing(k, [(tc[0] - 2.0, ay0), (tc[0] - 2.0, ay1), (tc[0] + 2.0, ay1), (tc[0] + 2.0, ay0)], zt)
    for dx in (-2.0, 2.0):
        k.beam((tc[0] + dx, ay0, zt - .2), (tc[0] + dx, ay1, zt - .2), .18, 'paint.accent', 'frame', h=.18)
        k.beam((tc[0] + dx, ty1 - .4, zt - 3.0), (tc[0] + dx, ay1 - 1.0, zt - .3), .16, 'paint.accent', 'frame', h=.16)
    for y in ip.frange(ay0 + 1.0, ay1, 1.8):
        k.beam((tc[0] - 2.0, y, zt - .25), (tc[0] + 2.0, y, zt - .25), .1, 'paint.accent', 'frame', h=.1)
    # ladders: plinth -> cabin deck (south face), balcony -> top deck, east platform -> top deck
    ip.ladder(k, tc[0] - 1.6, ty0, -math.pi / 2, zp, zm, off=.35, reach=.4, standoff=False, name='gantry ladder 1')
    ip.ladder(k, tx1 + .3, tc[1] - 1.6, 0.0, zm, zt, off=.25, reach=.3, standoff=False, name='gantry ladder 2')
    ip.ladder(k, tx1 + .3, tc[1] + 1.6, 0.0, ze, zt, off=.25, reach=.3, standoff=False, name='east platform ladder')
    count(k, 'stairs_ladders', 3)
    # control cabin cantilevered west of the tower on knee braces, door onto the cabin deck
    cc = (-5.9, -8.0)
    k.box((cc[0], cc[1], 19.6), (6.4, 5.6, 4.0), 'paint.main', 'mass', ch=.15, top=.1)
    k.box((cc[0], cc[1], 21.7), (6.8, 6.0, .2), 'paint.trim', 'frame', ch=.06)
    k.box((cc[0], cc[1], 17.5), (6.6, 5.8, .2), ip.STEEL, 'frame')
    for dy in (-2.2, 2.2):
        k.beam((tx0, cc[1] + dy, 12.0), (cc[0] - 2.8, cc[1] + dy, 17.4), .25, ip.STEEL, 'frame', h=.25)
    with k.at((cc[0], cc[1] - 2.8, 0)):
        k.slab(-2.8, 2.8, 19.0, 20.7, -.03, .02, 'glass', 'glass', pane=True)
    with k.at((cc[0] - 3.2, cc[1], 0), yaw=-math.pi / 2):
        k.slab(-2.4, 2.4, 19.0, 20.7, -.03, .02, 'glass', 'glass', pane=True)
    k.box(((cc[0] + 3.2 + tx0) / 2, cc[1] + 1.0, zm - .04), (tx0 - cc[0] - 3.2 + .4, 1.2, .08), 'grate', 'frame', walk=True)
    with k.at((cc[0] + 3.2, cc[1] + 1.0, 0), yaw=math.pi / 2):
        k.slab(-.5, .5, zm + .05, zm + 2.1, -.06, .02, 'paint.door', 'skin', door=True)
    count(k, 'doors')
    # twin antenna masts with red beacons on the tower head
    for ay in (tc[1] - .8, tc[1] - 3.0):
        ip.tube(k, [(tx1 - .6, ay, zt), (tx1 - .6, ay, 46.1)], .12, 'metal', 'frame', sides=8)
        for z in ip.frange(32.0, 45.0, 3.0):
            k.beam((tx1 - 1.0, ay, z), (tx1 - .2, ay, z), .05, 'metal', 'frame', h=.05)
        k.cyl((tx1 - .6, ay, 46.15), .12, .1, 'lamp_red', 'frame', sides=8)
    count(k, 'antennas_dishes', 2)


# ------------------------------------------------------------------ crane
def _square_frustum(k, r0, z0, r1, z1, mat, role='mass'):
    """Square shell rotated 45 degrees (vertices on the axes, the stock diamond), half-diagonal r0 -> r1."""
    v = [(r0 * math.cos(i * math.pi / 2), r0 * math.sin(i * math.pi / 2), z0) for i in range(4)] + \
        [(r1 * math.cos(i * math.pi / 2), r1 * math.sin(i * math.pi / 2), z1) for i in range(4)]
    f = [(0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7), (3, 2, 1, 0), (4, 5, 6, 7)]
    return k.add(v, f, mat, role)


def crane(k):
    zd = 5.4
    for x in (-1.5, 1.5):
        for y in (-1.5, 1.5):
            k.box((x, y, .15), (.7, .7, .3), 'ferro', 'mass', ch=.06, walk=True)
            ip.hbeam(k, (x, y, .3), (x, y, zd - .2), .3, .3)
    for (ax, ay), (bx, by) in (((-1.5, -1.5), (1.5, -1.5)), ((1.5, -1.5), (1.5, 1.5)), ((1.5, 1.5), (-1.5, 1.5)), ((-1.5, 1.5), (-1.5, -1.5))):
        for z0, z1 in ((.6, 2.7), (2.7, zd - .4)):
            ip.angle_brace(k, (ax, ay, z0), (bx, by, z1), s=.09)
            ip.angle_brace(k, (bx, by, z0), (ax, ay, z1), s=.09)
        ip.hbeam(k, (ax, ay, 2.7), (bx, by, 2.7), .2, .16)
    # flared diamond cab (the stock silhouette): vertical lower band, raked glazed walls, brim roof
    _square_frustum(k, 2.7, zd - .15, 2.7, 6.3, 'paint.main')
    _square_frustum(k, 2.7, 6.3, 4.3, 8.0, 'paint.main')
    for i in range(4):
        a0, a1 = i * math.pi / 2, (i + 1) * math.pi / 2
        q = lambda r, a, z: (r * math.cos(a), r * math.sin(a), z)  # noqa: E731
        f0, f1 = .22, .78
        lo = [q(2.7 + .06, a0, 6.5), q(2.7 + .06, a1, 6.5)]
        hi = [q(4.15 + .06, a0, 7.75), q(4.15 + .06, a1, 7.75)]
        lerp = lambda a, b, t: tuple(a[j] + (b[j] - a[j]) * t for j in range(3))  # noqa: E731
        quad = [lerp(lo[0], lo[1], f0), lerp(lo[0], lo[1], f1), lerp(hi[0], hi[1], f1), lerp(hi[0], hi[1], f0)]
        k.add(quad, [(0, 1, 2, 3)], 'glass', 'glass', pane=True)
    _square_frustum(k, 4.35, 8.0, 4.25, 8.18, 'paint.second', 'roof')
    k.box((0, 0, zd - .04), (6.0, 6.0, .08), 'grate', 'frame', walk=True)
    ip.railing(k, [(-3.0, -3.0), (3.0, -3.0), (3.0, 3.0), (-3.0, 3.0)], zd, closed=True, gaps=[(1.4, -3.0)])
    door_c = (1.35 * .98, -1.35 * .98)
    with k.at((door_c[0], door_c[1], 0), yaw=math.pi / 4):
        k.slab(-.45, .45, zd + .05, zd + 2.0, -.08, .02, 'paint.door', 'skin', door=True)
    count(k, 'doors')
    ip.lamp(k, 0.0, -2.75, 6.1, -math.pi / 2, k.meta['sockets'])
    ip.ladder(k, 1.4, -3.0, -math.pi / 2, 0.0, zd, off=.18, reach=.3, standoff=False, name='cab ladder')
    count(k, 'stairs_ladders')


SPECS = {
    'grainstorage': dict(fn=grainstorage, palette='intent_factory', cls='large', counts=dict(tanks=4, doors=4),
        intent=('A grain store: a tall corrugated elevator house with a mono-pitch standing-seam roof; four steel silos '
                'with conical hoppers built into the front face under a louvred conveyor gallery; two roll-up loading '
                'doors at the rear, personnel doors and high windows on the sides, a roof conveyor pipe.')),
    'civstoragea': dict(fn=civstoragea, palette='intent_shed', cls='small', counts=dict(doors=2, windows=4),
        intent=('A small corrugated store with a triple-gable roof (three ridges, valley gutters), roll-up doors at both '
                'ends, window ribbons on the long sides, wall lamps and downpipes.')),
    'civstorageb': dict(fn=civstorageb, palette='intent_shed', cls='small', counts=dict(doors=4),
        intent=('A sawtooth-roof lock-up: three bays under rusty corrugated saw roofs with clerestory glazing, three roll-up '
                'doors on a hazard-edged loading dock along the east face with steps at both ends, a side door and a '
                'switch cabinet.')),
    'civstoragec': dict(fn=civstoragec, palette='intent_shed', cls='prop', counts=dict(doors=1, stairs_ladders=1),
        intent=('A raised timber store: a board-clad cabin with a mono-pitch tin roof on X-braced timber stilts, a door '
                'onto a railed landing reached by a timber stair, a window on the east side.')),
    # gantry: open lattice, the coarse height grid sees bracing gaps, so a looser mass-placement bound
    'gantry': dict(fn=gantry, palette='intent_khaki', cls='large', tol=dict(grid_mae=.3),
        counts=dict(stairs_ladders=3, antennas_dishes=2, doors=2),
        intent=('A service gantry: a square steel lattice tower on a ferrocrete plinth, a khaki equipment house in its base, intermediate grating floors, a cabin-level platform, a '
                'railed top deck and a yellow service-arm truss to the north; a khaki control cabin cantilevered on the '
                'west on knee braces; an east service platform; twin antenna masts with red beacons; caged ladders '
                'between the levels.')),
    'crane': dict(fn=crane, palette='intent_khaki', cls='small', counts=dict(doors=1, stairs_ladders=1),
        intent=('A yard control cab: an octagonal pale-khaki cab with a raked window band and a wide brim roof on a '
                'braced four-leg black steel trestle, railed cab deck reached by a caged ladder, door and lamp.')),
}
