"""Stock remakes, Sonnet runner (building / turret / wall categories).

Original construction; the decoded stock mesh owns silhouette, proportions and
counts (measured per asset, hardcoded here so the generators need no external
data). Same conventions as batch2/batch3: metres, Z up, front -Y, frame first,
skin as separate removable panels.
"""
import math
import random

import counts
import geo
from geo import rect_poly

SIZES = {
    'lrmturret': (5.75, 6.92292, 10.6861),
    'mediumautocannonturret': (5.75, 14.6186, 8.53358),
    'genericmilitary': (27.67255, 20.82053, 9.95408),
    'geodesicdome': (18.27, 22.4158, 12.537),
    'liquidstorage': (23.91525, 25.43175, 15.71625),
    'largefueltank': (26.7264, 23.37787, 22.87107),
    'ammodump': (20.35583, 17.6615, 7.33495),
}
ORIGINS = {
    'd_heavywall': (-0.00050, 3.58770, -0.01630),
    'lrmturret': (0.0, 0.58646, 0.0),
    'mediumautocannonturret': (0.0, 2.73667, 0.0),
    'genericmilitary': (0.48577, 5.32946, -0.02967),
    'geodesicdome': (-5.44440, 2.52000, 0.0),
    'liquidstorage': (0.0, 0.14288, 0.0),
    'largefueltank': (-2.61180, 0.09491, 0.0),
    'ammodump': (-5.34374, 5.34135, -0.00875),
}
BUILDINGS = {'genericmilitary', 'geodesicdome', 'liquidstorage', 'largefueltank', 'ammodump'}


# ------------------------------------------------------------ helpers
def C(x):
    return kit.C(x)


def taper(k, p0, z0, p1, z1, mat, role='mass', **fl):
    """Solid between two same-length CCW polygons at z0 and z1."""
    n = len(p0)
    vv = [(x, y, z0) for x, y in p0] + [(x, y, z1) for x, y in p1]
    ff = [tuple(reversed(range(n))), tuple(range(n, 2 * n))] + [(i, (i + 1) % n, (i + 1) % n + n, i + n) for i in range(n)]
    return k.add(vv, ff, mat, role, **fl)


def tri_plate(k, a, b, c, t, mat, role='skin', shrink=.93, **fl):
    """Thin raised triangular plate over the triangle abc (outward normal by winding)."""
    cen = tuple((a[i] + b[i] + c[i]) / 3 for i in range(3))
    sh = [tuple(cen[i] + (p[i] - cen[i]) * shrink for i in range(3)) for p in (a, b, c)]
    u = [b[i] - a[i] for i in range(3)]
    v = [c[i] - a[i] for i in range(3)]
    n = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
    ln = math.sqrt(sum(x * x for x in n)) or 1
    n = tuple(x / ln for x in n)
    out = [tuple(p[i] + n[i] * t for i in range(3)) for p in sh]
    inn = [tuple(p[i] - n[i] * .02 for i in range(3)) for p in sh]
    vv = inn + out
    ff = [(2, 1, 0), (3, 4, 5), (0, 1, 4, 3), (1, 2, 5, 4), (2, 0, 3, 5)]
    return k.add(vv, ff, mat, role, **fl)


def face_frame(x0, x1, y0, y1, i):
    return [((0, y0, 0), 0.0), ((x1, 0, 0), math.pi / 2), ((0, y1, 0), math.pi), ((x0, 0, 0), -math.pi / 2)][i]


def face(k, x0, x1, y0, y1, i, mat, panels=(), windows=(), ribs=(), rib_z=(.6, 4.0), world_x=True, wz=(0, 0)):
    """One wall face. u is the face-local x (front: +X, right: +Y, back: -X, left: -Y)."""
    o, yaw = face_frame(x0, x1, y0, y1, i)
    sign = -1 if i in (2, 3) else 1
    def U(a, b):
        return (a, b) if sign > 0 else (-b, -a)
    with k.at(o, yaw=yaw):
        for (a, b, z0, z1) in panels:
            u0, u1 = U(a, b)
            k.slab(u0 + .05, u1 - .05, z0, z1, -.07, .03, mat, 'skin', front=C(.03), removable=True)
            kit.bolts(k, (u0 + .22, -.08, z0 + .3), (u0 + .22, -.08, z1 - .3), pitch=.7)
            kit.bolts(k, (u1 - .22, -.08, z0 + .3), (u1 - .22, -.08, z1 - .3), pitch=.7)
        for u in ribs:
            uu = u if sign > 0 else -u
            k.slab(uu - .13, uu + .13, rib_z[0], rib_z[1], -.11, .02, 'paint.trim', 'frame')
        for (a, b, z0, z1) in windows:
            u0, u1 = U(a, b)
            kit.batch2.window(k, u0, u1, z0, z1, -.01, 0)


def deck(k, x0, x1, y0, y1, z, nx=3, ny=2, mat='gravel'):
    sx, sy = (x1 - x0) / nx, (y1 - y0) / ny
    for i in range(nx):
        for j in range(ny):
            k.box((x0 + (i + .5) * sx, y0 + (j + .5) * sy, z - .1), (sx - .02, sy - .02, .2), mat, 'roof', removable=True)


def coping(k, x0, x1, y0, y1, z, t=.45, h=.35, mat='paint.trim'):
    for (a, b, c, d) in ((x0, y0 - t / 2, x1, y0 + t / 2), (x0, y1 - t / 2, x1, y1 + t / 2),
                         (x0 - t / 2, y0, x0 + t / 2, y1), (x1 - t / 2, y0, x1 + t / 2, y1)):
        k.box(((a + c) / 2, (b + d) / 2, z + h / 2), (c - a, d - b, h), mat, 'frame', ch=C(.04), top=C(.05))


# ------------------------------------------------------------ genericmilitary
def genericmilitary(k):
    x0, x1, y0, y1 = -13.9, 9.8, -6.9, 8.3
    zb, zt = .55, 6.0
    tx0, tx1, ty0, ty1, th = 7.6, 13.8, 4.3, 10.4, 9.95
    # footing, then block core and tower core (frame / mass first)
    k.prism(rect_poly(x0 - .25, y0 - .25, x1 + .25, y1 + .25, C(.2)), 0, zb, 'ferro', 'mass', top=C(.08))
    k.prism(rect_poly(x0, y0, x1, y1), zb, zt, 'paint.second', 'mass')
    k.prism(rect_poly(tx0 - .2, ty0 - .2, tx1 + .2, ty1 + .2, C(.2)), 0, zb, 'ferro', 'mass', top=C(.08))
    k.prism(rect_poly(tx0, ty0, tx1, ty1), zb, th - .5, 'paint.second', 'mass')
    # corner posts (stock: pale vertical corner trim)
    for (cx, cy) in ((x0, y0), (x1, y0), (x0, y1), (tx0, ty1), (tx1, ty0), (tx1, ty1)):
        h = (th - .5) if cx >= tx0 and cy >= ty0 else zt
        k.box((cx, cy, (zb + h) / 2), (.34, .34, h - zb), 'paint.trim', 'frame', ch=C(.04))
    band_z = (4.3, 5.3)
    wcx = lambda n, a, b: [(a + (i + .5) * (b - a) / n) for i in range(n)]
    # FRONT (khaki): two panels either side of the door bay, 6 windows
    face(k, x0, x1, y0, y1, 0, 'paint.main',
         panels=[(-13.4, -6.5, .75, 3.95), (1.8, 9.4, .75, 3.95)],
         windows=[(c - 1.4, c + 1.4, *band_z) for c in wcx(6, x0, x1)])
    # RIGHT / east end (dark): 2 panels, 3 windows (4th hidden behind the tower)
    face(k, x0, x1, y0, y1, 1, 'paint.second',
         panels=[(-6.7, -.3, .75, 3.95), (-.1, 4.1, .75, 3.95)],
         windows=[(c - 1.4, c + 1.4, *band_z) for c in wcx(4, y0, y1)[:3]])
    # BACK (dark): 3 panels, 6 windows
    face(k, x0, x1, y0, y1, 2, 'paint.second',
         panels=[(-13.4, -6.0, .75, 3.95), (-5.8, 1.8, .75, 3.95), (2.0, 9.5, .75, 3.95)],
         windows=[(c - 1.4, c + 1.4, *band_z) for c in wcx(6, x0, x1)])
    # LEFT / west end (khaki): 2 panels, 4 windows
    face(k, x0, x1, y0, y1, 3, 'paint.main',
         panels=[(-6.7, .7, .75, 3.95), (.9, 8.1, .75, 3.95)],
         windows=[(c - 1.4, c + 1.4, *band_z) for c in wcx(4, y0, y1)])
    # fascia band above the windows, belt below them
    for (a, b, c, d) in ((x0 - .1, y0 - .1, x1 + .1, y0 + .1), (x0 - .1, y1 - .1, x1 + .1, y1 + .1)):
        k.box(((a + c) / 2, (b + d) / 2, 5.65), (c - a, d - b + .1, .7), 'paint.trim', 'frame', ch=C(.03))
        k.box(((a + c) / 2, (b + d) / 2, 4.1), (c - a, d - b + .1, .22), 'paint.trim', 'frame')
    for (a, b, c, d) in ((x0 - .1, y0, x0 + .1, y1), (x1 - .1, y0, x1 + .1, y1)):
        k.box(((a + c) / 2, (b + d) / 2, 5.65), (c - a + .1, d - b, .7), 'paint.trim', 'frame', ch=C(.03))
        k.box(((a + c) / 2, (b + d) / 2, 4.1), (c - a + .1, d - b, .22), 'paint.trim', 'frame')
    # front door bay: recessed teal pilaster fields, double door, canopy on two posts
    with k.at((0, y0, 0)):
        k.slab(-6.3, 1.6, zb, 3.6, -.02, .05, 'paint.teal', 'skin', removable=True)
        for x in (-6.3, 1.6):
            k.slab(x - .15, x + .15, zb, 3.7, -.12, .02, 'paint.trim', 'frame')
        k.slab(-4.1, -.3, zb, 3.15, -.14, -.02, 'paint.trim', 'frame', front=C(.03))
        for dx in (-3.95, -2.15):
            k.slab(dx, dx + 1.7, zb + .05, 3.05, -.16, -.05, 'metal', 'skin', front=C(.03), removable=True)
            k.slab(dx + .2, dx + 1.5, 1.7, 2.8, -.18, -.15, 'glass', 'glass', pane=True)
        k.lamp_head(-2.2, 3.35, -1.2, w=.5)
    k.box((-2.2, y0 - 1.75, .18), (7.8, 3.5, .36), 'ferro', 'mass', ch=C(.05), top=C(.04))
    k.box((-2.2, y0 - 1.75, 3.62), (7.8, 3.5, .26), 'paint.trim', 'frame', ch=C(.04))
    for px in (-5.9, 1.5):
        k.box((px, y0 - 3.3, 1.85), (.22, .22, 3.5), 'metal', 'frame')
    k.box((-2.2, y0 - 3.45, 3.62), (7.8, .12, .3), 'hazard', 'detail')
    # tower faces: one riveted panel per face, two windows each
    tower = [(0, 'paint.main', tx0, tx1, ty0, ty1), (1, 'paint.second', tx0, tx1, ty0, ty1),
             (2, 'paint.second', tx0, tx1, ty0, ty1), (3, 'paint.main', tx0, tx1, ty0, ty1)]
    for i, mat, a0, a1, b0, b1 in tower:
        length = (a1 - a0) if i in (0, 2) else (b1 - b0)
        centre = (a0 + a1) / 2 if i in (0, 2) else (b0 + b1) / 2
        u0, u1 = centre - length / 2, centre + length / 2
        if i in (2, 3):
            u0, u1 = -(centre + length / 2), -(centre - length / 2)
            cs = [-(centre + length * .22), -(centre - length * .22)]
        else:
            cs = [centre - length * .22, centre + length * .22]
        o, yaw = face_frame(a0, a1, b0, b1, i)
        with k.at(o, yaw=yaw):
            k.slab(u0 + .3, u1 - .3, 1.0, 7.0, -.07, .03, mat, 'skin', front=C(.03), removable=True)
            for c in cs:
                kit.batch2.window(k, c - 1.0, c + 1.0, 7.9, 8.7, -.01, 0)
            kit.bolts(k, (u0 + .2, -.09, 1.0), (u0 + .2, -.09, 7.0), pitch=.7)
            kit.bolts(k, (u1 - .2, -.09, 1.0), (u1 - .2, -.09, 7.0), pitch=.7)
    # roofs: gravel deck in lapped slabs, coping, rooftop plates
    deck(k, x0 + .3, x1 - .3, y0 + .3, y1 - .3, zt + .02, 3, 2)
    coping(k, x0, x1, y0, y1, zt)
    deck(k, tx0 + .3, tx1 - .3, ty0 + .3, ty1 - .3, th - .5 + .02, 1, 1)
    coping(k, tx0, tx1, ty0, ty1, th - .5)
    k.box((-5.7, -2.9, zt + .12), (3.3, 3.0, .16), 'paint.trim', 'frame', ch=C(.03))
    k.box((10.7, 7.3, th - .38), (1.5, 1.5, .14), 'paint.trim', 'frame', ch=C(.03))
    k.moss_sites.append(((x0, y0 - .4, 0), (x1, y0 - .4, 0)))
    k.hits = [dict(p=(x0, 1.0, 2.6), d=(1, 0, 0), r=3.2, depth=0, sag=.2),
              dict(p=(1.0, 2.0, zt), d=(0, 0, -1), r=3.4, depth=0, sag=.2)]


# ------------------------------------------------------------ geodesicdome
def _ico_cap():
    s5 = math.sqrt(5)
    V = [(0, 0, 1)]
    for i in range(5):
        a = i * math.tau / 5
        V.append((2 / s5 * math.cos(a), 2 / s5 * math.sin(a), 1 / s5))
    for i in range(5):
        a = math.pi / 5 + i * math.tau / 5
        V.append((2 / s5 * math.cos(a), 2 / s5 * math.sin(a), -1 / s5))
    F = [(0, 1 + i, 1 + (i + 1) % 5) for i in range(5)]
    for i in range(5):
        F.append((1 + i, 6 + i, 1 + (i + 1) % 5))
        F.append((6 + i, 6 + (i + 1) % 5, 1 + (i + 1) % 5))

    def mid(a, b):
        m = tuple((a[j] + b[j]) / 2 for j in range(3))
        n = math.sqrt(sum(c * c for c in m))
        return tuple(c / n for c in m)
    tris = []
    for a, b, c in F:
        A, B, Cc = V[a], V[b], V[c]
        ab, bc, ca = mid(A, B), mid(B, Cc), mid(Cc, A)
        for t in ((A, ab, ca), (ab, B, bc), (ca, bc, Cc), (ab, bc, ca)):
            if min(p[2] for p in t) > -1e-6:
                tris.append(t)
    return tris


def geodesicdome(k):
    R, H, zb = 9.135, 8.85, .55
    tris = _ico_cap()
    P = lambda p: (p[0] * R, p[1] * R, zb + max(0.0, p[2]) * H)
    # base skirt: dark decagon curb under the shell
    ring = [(R * math.cos(math.pi / 10 + i * math.tau / 10) * 1.04, R * math.sin(math.pi / 10 + i * math.tau / 10) * 1.04) for i in range(10)]
    k.prism(ring, 0, zb, 'ferro', 'mass', top=C(.06))
    # structure first: struts along every geodesic edge, hub at the apex
    edges = set()
    for t in tris:
        for a, b in ((0, 1), (1, 2), (2, 0)):
            key = tuple(sorted((tuple(round(c, 4) for c in t[a]), tuple(round(c, 4) for c in t[b]))))
            edges.add(key)
    for a, b in edges:
        pa, pb = P(a), P(b)
        k.beam(pa, pb, .34, 'paint.trim', 'frame', h=.30)
    # cladding: one raised plate per facet, separate removable skin
    for i, t in enumerate(tris):
        pts = [P(p) for p in t]
        a = pts[0]
        tri_plate(k, pts[0], pts[1], pts[2], .14, 'paint.main' if i % 3 else 'paint.second', 'skin', shrink=.90, removable=True)
        c = tuple(sum(p[j] for p in pts) / 3 for j in range(3))
        kit.bolts(k, a, c, pitch=1.6, s=.05)
    # six crown slit windows around the ring
    for j in range(6):
        phi = math.radians(30 + 60 * j)
        al = .78
        cx, cy = R * math.sin(al) * math.cos(phi), R * math.sin(al) * math.sin(phi)
        cz = zb + H * math.cos(al)
        nh = math.hypot(cx / R ** 2, cy / R ** 2)
        tilt = -math.atan2((cz - zb) / H ** 2, nh)
        with k.at((cx * .985, cy * .985, cz * .985), yaw=phi + math.pi / 2, pitch=tilt):
            k.slab(-1.3, 1.3, -.3, .3, -.28, -.15, 'paint.trim', 'frame', front=C(.03))
            k.slab(-1.15, 1.15, -.22, .22, -.17, -.12, 'glass', 'glass', pane=True)
    # two entry doors on the west / east flanks, each with a green status lamp
    for ang in (math.radians(192), math.radians(-12)):
        px, py = (R - .12) * math.cos(ang), (R - .12) * math.sin(ang)
        with k.at((px, py, zb), yaw=ang + math.pi / 2):
            k.slab(-.75, .75, 0, 2.3, -.32, -.05, 'paint.trim', 'frame', front=C(.03))
            k.slab(-.6, .6, .05, 2.2, -.40, -.30, 'paint.brown', 'skin', front=C(.02), removable=True)
            k.slab(-.4, .4, 1.35, 1.95, -.42, -.40, 'glass', 'glass', pane=True)
            k.box((1.15, -.3, 1.7), (.14, .1, .14), 'lamp', 'detail')
    # front tower on the dome front (-Y): tapered block, vents, access panel, cap
    ty0, ty1 = -13.285, -8.985
    k.box((0, (ty0 + ty1) / 2 + .2, 1.8), (2.3, 3.4, 3.6), 'paint.second', 'mass', ch=C(.05))  # neck link block
    taper(k, rect_poly(-2.65, ty0, 2.65, ty1, C(.25)), 0, rect_poly(-1.95, ty0 + .35, 1.95, ty1 - .25, C(.2)), 6.7, 'paint.main', 'mass')
    k.box((0, (ty0 + ty1) / 2, 6.95), (4.4, 4.0, .6), 'paint.trim', 'frame', ch=C(.15), top=C(.06))
    k.box((0, (ty0 + ty1) / 2, 7.4), (1.6, 1.6, .5), 'paint.second', 'detail', ch=C(.1))
    with k.at((0, ty0, 0)):
        for z in (4.3, 4.95, 5.6):
            k.slab(-1.05, 1.05, z, z + .32, -.2, .05, 'glass', 'glass', pane=True)
            k.slab(-1.15, 1.15, z - .06, z + .38, -.12, -.02, 'paint.trim', 'frame')
        k.slab(-1.25, 1.25, 1.4, 2.6, -.14, .03, 'paint.trim', 'skin', front=C(.04), removable=True)
        k.service_panel(x=0, z=.8, yf=0.0, w=.8, h=.5)
    # antenna mast (striped) and stack pipe on the crown
    top = zb + H
    k.cyl((0, 0, top + 1.55), .13, 3.1, 'paint.trim', 'frame', sides=8)
    for j in range(5):
        k.cyl((0, 0, top + .4 + j * .6), .145, .3, 'paint.red', 'detail', sides=8)
    k.tube([(.75, -.6, top - .3), (.75, -.6, top + .8), (.2, -.6, top + 1.15)], .16, 'metal', 'frame', sides=8)
    k.moss_sites.append(((-R, -2, 0), (-R, 4, 0)))
    k.hits = [dict(p=(R * .55, -R * .45, zb + H * .6), d=(-.5, .5, -.5), r=4.2, depth=0, sag=0)]


# ------------------------------------------------------------ liquidstorage
def _seg():
    """Round tanks are truly round: the stock decagon was a 1990s budget limit, not a design."""
    return 28 if geo.GAME_MESH else 56


def _decagon(r, phase=math.pi / 2):
    n = _seg()
    return [(r * math.cos(phase + i * math.tau / n), r * math.sin(phase + i * math.tau / n)) for i in range(n)]


def liquidstorage(k):
    R0, R1, R2 = 12.57, 11.43, 10.47      # base, shoulder foot, shoulder
    z1, z2, z3 = 12.29, 13.24, 14.19
    # foundation ring, then the decagonal drum (battered), shoulder, low crown
    k.prism(_decagon(R0 + .2), 0, .38, 'ferro', 'mass', top=C(.08))
    k.prism(_decagon(R0), .38, 1.4, 'paint.trim', 'mass')
    taper(k, _decagon(R0), 1.4, _decagon(R1), z1, 'paint.second', 'mass')
    taper(k, _decagon(R1), z1, _decagon(R2), z2, 'paint.main', 'roof')
    taper(k, _decagon(R2), z2, _decagon(1.2), z3, 'paint.main', 'roof')
    # cladding: two tiers x ten faces of battered plates, dark belt and vertical straps at the corners
    def rad(z):
        return R0 + (R1 - R0) * (z - 1.4) / (z1 - 1.4)
    belt = 6.55
    tiers = ((1.55, belt - .25), (belt + .25, z1 - .15))
    for i in range(10):
        a0 = math.pi / 2 + i * math.tau / 10
        a1 = a0 + math.tau / 10
        am = (a0 + a1) / 2
        dark = math.cos(am) > -.2 or math.sin(am) > .2
        for ti, (zl, zh) in enumerate(tiers):
            rl, rh = rad(zl), rad(zh)
            f = math.cos(math.pi / 10)
            p = lambda r, a, z: (r * math.cos(a), r * math.sin(a), z)
            q = [p(rl, a0, zl), p(rl, a1, zl), p(rh, a1, zh), p(rh, a0, zh)]
            # inset toward the face centre so adjacent plates leave a corner joint
            cx, cy = (sum(v[0] for v in q) / 4, sum(v[1] for v in q) / 4)
            q = [(cx + (v[0] - cx) * .93, cy + (v[1] - cy) * .93, v[2]) for v in q]
            nx, ny = math.cos(am), math.sin(am)
            back = [(v[0] - nx * .14, v[1] - ny * .14, v[2]) for v in q]
            front = [(v[0] + nx * .05, v[1] + ny * .05, v[2]) for v in q]
            kit.hexa(k, [front[0], front[1], front[2], front[3]], [back[0], back[1], back[2], back[3]],
                     ('paint.cool2' if ti else 'paint.cool') if dark else ('paint.main' if ti else 'paint.second'), 'skin', removable=True)
        # vertical corner straps
        zs = (1.5, z1 - .1)
        cxy = lambda r, z: (r * math.cos(a0), r * math.sin(a0), z)
        k.beam(cxy(rad(zs[0]) + .02, zs[0]), cxy(rad(zs[1]) + .02, zs[1]), .16, 'paint.trim', 'frame', h=.12)
    # belt ring girder
    taper(k, _decagon(rad(belt) + .12), belt - .2, _decagon(rad(belt) + .12), belt + .2, 'paint.trim', 'frame')
    # roof hut on the front half: sloped lean-to hood over the access stair, with stair column down the front vertex
    k.box((0, -3.9, 14.1), (4.76, 12.7, 3.2), 'paint.second', 'mass', ch=C(.1))
    verts = [(-2.38, -12.72, 12.38), (2.38, -12.72, 12.38), (2.38, -9.86, 15.72), (-2.38, -9.86, 15.72)]
    k.add(verts + [(x, y, z - .5) for x, y, z in verts], [(0, 1, 2, 3), (7, 6, 5, 4), (4, 5, 1, 0), (5, 6, 2, 1), (6, 7, 3, 2), (7, 4, 0, 3)], 'paint.main', 'roof')
    k.box((0, 1.2, 15.8), (4.9, 2.7, .3), 'paint.trim', 'frame', ch=C(.06))
    k.box((0, -6.0, 15.76), (4.0, 5.0, .08), 'paint.red', 'frame')
    # stair column down the front vertex (ladder bay) and base control door
    k.box((0, -R0 + .55, 6.4), (2.0, 1.9, 12.0), 'ferro.main', 'frame', ch=C(.05))
    for z in range(2, 12, 1):
        kit.bolts(k, (-.9, -R0 - .5, z), (.9, -R0 - .5, z), pitch=.45)
    k.slab(-1.15, 1.15, .6, 2.4, -R0 - .5, -R0 - .35, 'paint.second', 'skin', front=C(.03), removable=True)
    k.slab(-.8, .8, 1.0, 1.9, -R0 - .6, -R0 - .5, 'glass', 'glass', pane=True)
    k.cyl((-1.8, 2.0, 15.2), .9, .12, 'paint.trim', 'detail', sides=12)   # roof vent
    z2 = 13.24 + .28
    k.cyl((4.9, -2.2, z2), 1.0, .5, 'paint.trim', 'detail', sides=8)
    k.moss_sites.append(((-R0, -2, 0), (R0, -2, 0)))
    k.hits = [dict(p=(R0 * .78, R0 * .55, 6.5), d=(-.6, -.4, 0), r=4.3, depth=0, sag=0),
              dict(p=(-3, 3, z2), d=(0, 0, -1), r=3.2, depth=0, sag=0)]


# ------------------------------------------------------------ largefueltank
def _poly(r, n, phase=0.0, cx=0.0, cy=0.0):
    return [(cx + r * math.cos(phase + i * math.tau / n), cy + r * math.sin(phase + i * math.tau / n)) for i in range(n)]


def largefueltank(k):
    Rb = 10.745
    # hex pedestal (vertices on +-Y), flared neck, decagon shell (vertices on +-X), shoulders, crown
    hexa_ = lambda r: _poly(r, 6, math.pi / 2)
    taper(k, hexa_(11.69), 0, hexa_(8.53), 4.69, 'paint.ped', 'mass')
    k.prism(hexa_(8.7), 4.69, 5.0, 'paint.ped', 'frame')
    taper(k, _poly(7.3, _seg()), 4.72, _poly(Rb, _seg()), 6.2, 'paint.trim', 'mass')
    # stacked shell bands: lower collar, hazard band, belt panels, upper plated course
    bands = [(6.2, 7.8, 'paint.trim'), (7.8, 11.6, 'hazard'), (11.6, 14.8, 'paint.second'), (14.8, 20.18, 'paint.main')]
    for (za, zb, m) in bands:
        k.prism(_poly(Rb, _seg()), za, zb, m, 'mass')
    # separate plates over the drum: each band x 10 faces (frame first: ring girders + corner straps)
    for z in (7.8, 11.6, 14.8, 20.0):
        k.prism(_poly(Rb + .08, _seg()), z - .15, z + .15, 'paint.trim', 'frame')
    for i in range(10):
        a0 = i * math.tau / 10
        a1 = a0 + math.tau / 10
        am = (a0 + a1) / 2
        nx, ny = math.cos(am), math.sin(am)
        for (za, zb, m) in ((8.0, 11.4, 'hazard'), (11.8, 14.6, 'paint.second'), (15.0, 19.9, 'paint.main')):
            q = [(Rb * math.cos(a0), Rb * math.sin(a0), za), (Rb * math.cos(a1), Rb * math.sin(a1), za),
                 (Rb * math.cos(a1), Rb * math.sin(a1), zb), (Rb * math.cos(a0), Rb * math.sin(a0), zb)]
            cx, cy = sum(v[0] for v in q) / 4, sum(v[1] for v in q) / 4
            q = [(cx + (v[0] - cx) * .94, cy + (v[1] - cy) * .94, v[2]) for v in q]
            kit.hexa(k, [(v[0] + nx * .08, v[1] + ny * .08, v[2]) for v in q], [(v[0] - nx * .12, v[1] - ny * .12, v[2]) for v in q],
                     m, 'skin', removable=True)
        kit.bolts(k, (Rb * math.cos(a0) * 1.01, Rb * math.sin(a0) * 1.01, 20.0), (Rb * math.cos(a0) * 1.01, Rb * math.sin(a0) * 1.01, 14.9), pitch=.9)
    taper(k, _poly(Rb, _seg()), 20.18, _poly(7.3, _seg()), 21.66, 'paint.trim', 'roof')
    taper(k, _poly(7.3, _seg()), 21.66, _poly(3.3, _seg()), 22.57, 'paint.main', 'roof')
    k.prism(_poly(3.2, _seg()), 22.57, 22.87, 'hazard', 'roof')
    # pedestal fascia: three recessed control panels on the front faces, round access hatch on the front hex face
    for j, ang in enumerate((math.radians(-150), math.radians(-90), math.radians(-30))):
        cx, cy = 7.9 * math.cos(ang), 7.9 * math.sin(ang)
        with k.at((cx, cy, 0), yaw=ang + math.pi / 2):
            k.slab(-2.6, 2.6, 1.3, 3.5, -.45, .05, 'paint.ped', 'skin', front=C(.03), removable=True)
            k.slab(-1.9, -.7, 1.8, 3.0, -.5, -.44, 'paint.red', 'detail')
            k.slab(.5, 1.9, 1.8, 3.0, -.5, -.44, 'paint.trim', 'detail')
    with k.at((0, -7.9, 0), yaw=0):
        k.cyl((0, -.75, 1.9), .8, .3, 'rubber', 'detail', sides=12)
    # outlet duct to the west (-X) with end flange
    k.box((-11.2, 0, 1.7), (9.6, 2.0, 2.1), 'paint.ped', 'frame', ch=C(.1))
    k.box((-16.0, 0, 1.7), (1.0, 2.2, 3.4), 'paint.trim', 'frame', ch=C(.06))
    k.moss_sites.append(((-11, -11, 0), (11, -11, 0)))
    k.hits = [dict(p=(7.5, -6.8, 13.0), d=(-.6, .6, 0), r=5.0, depth=0, sag=0),
              dict(p=(0, -8, 3.0), d=(0, 1, 0), r=3.0, depth=0, sag=.2)]


# ------------------------------------------------------------ ammodump
def ammodump(k):
    bx, by = 17.6, 14.4          # base footprint of the battered body
    ti = 1.85                    # horizontal inset of the top edge
    zp, zw, zt = .7, 6.65, 7.33
    hx, hy = bx / 2, by / 2
    k.prism(rect_poly(-hx - .2, -hy - .2, hx + .2, hy + .2, C(.12)), 0, zp, 'ferro', 'mass', top=C(.05))
    # battered core + steel frame ribs at the face joints
    taper(k, rect_poly(-hx, -hy, hx, hy), zp, rect_poly(-hx + ti, -hy + ti, hx - ti, hy - ti), zw, 'paint.main', 'mass')
    k.prism(rect_poly(-hx + ti, -hy + ti, hx - ti, hy - ti), zw, zt - .3, 'paint.main', 'mass')
    coping(k, -hx + ti + .4, hx - ti - .4, -hy + ti + .4, hy - ti - .4, zt - .3, t=.8, h=.63)
    # roof: recessed gravel field and four red corner marks, central hatch
    deck(k, -hx + ti + .8, hx - ti - .8, -hy + ti + .8, hy - ti - .8, zt - .17, 2, 2)
    for sx in (-1, 1):
        for sy in (-1, 1):
            cx, cy = sx * (bx / 2 - ti - .9), sy * (by / 2 - ti - .9)
            tri = [(cx, cy), (cx - sx * 1.8, cy), (cx, cy - sy * 1.8)]
            if sx * sy < 0:
                tri = tri[::-1]
            k.prism(tri, zt - .17, zt - .1, 'paint.red', 'detail')
    k.box((0, 0, zt - .05), (2.2, 1.0, .26), 'paint.trim', 'frame', ch=C(.04))
    # four faces: tan side panels, green centre bay, hazard band, sign above door
    slope = ti / (zw - zp)
    for i, (o, yaw, length, door) in enumerate([((0, -hy, 0), 0.0, bx, 'wide'), ((hx, 0, 0), math.pi / 2, by, 'narrow'),
                                               ((0, hy, 0), math.pi, bx, 'wide'), ((-hx, 0, 0), -math.pi / 2, by, 'narrow')]):
        tilt = math.atan(slope)
        with k.at((o[0], o[1], zp), yaw=yaw, pitch=-tilt):
            sl = (zw - zp) / math.cos(tilt)
            half = length / 2 - ti * .5
            bay = 3.2 if door == 'wide' else 2.6
            for sgn in (-1, 1):
                a, b = (bay + .25, half - .15) if sgn > 0 else (-half + .15, -bay - .25)
                k.slab(a, b, .35, sl - .45, -.07, .04, 'paint.main', 'skin', front=C(.03), removable=True)
                k.slab(a, b, 2.1, 3.1, -.10, -.06, 'hazard', 'detail')
            k.slab(-bay, bay, .35, sl - .45, -.02, .05, 'paint.second', 'skin', removable=True)
            for sgn in (-1, 1):
                k.slab(sgn * bay - .12, sgn * bay + .12, .3, sl - .4, -.14, .0, 'paint.trim', 'frame')
            # door in the green bay
            dw = 1.5 if door == 'wide' else .8
            k.slab(-dw, dw, .4, 3.0, -.16, -.04, 'metal', 'skin', front=C(.03), removable=True)
            if door == 'narrow':
                k.slab(-.5, .5, 2.0, 2.7, -.18, -.15, 'glass', 'glass', pane=True)
            k.slab(-.9, .9, 3.6, 4.5, -.14, -.06, 'paint.red', 'detail')
    # protruding door porches on +-Y (floor vent grille), stair landings with four treads on +-X
    for sy in (-1, 1):
        k.box((0, sy * (hy + .8), 1.05), (6.2, 1.6, 2.1), 'paint.trim', 'mass', ch=C(.06), top=C(.05))
        k.box((0, sy * (hy + 1.62), .45), (3.6, .1, .5), 'grate', 'detail')
    for sx in (-1, 1):
        for t in range(4):
            k.box((sx * (hx + .35 + t * .45), 0, .3 - t * .07), (.5, 1.6, .5 - t * .1), 'paint.trim', 'frame')
    k.moss_sites.append(((-hx, -hy - .3, 0), (hx, -hy - .3, 0)))
    k.hits = [dict(p=(hx - .5, 1.5, 2.5), d=(-1, 0, 0), r=3.0, depth=0, sag=0),
              dict(p=(1.0, 1.0, zt), d=(0, 0, -1), r=3.0, depth=0, sag=0)]


# ------------------------------------------------------------ turrets
def _base(k, top=4.45):
    taper(k, rect_poly(-2.875, -2.875, 2.875, 2.875, C(.1)), 0, rect_poly(-top / 2, -top / 2, top / 2, top / 2, C(.1)), 5.0, 'paint.main', 'mass')
    k.prism(rect_poly(-top / 2 - .1, -top / 2 - .1, top / 2 + .1, top / 2 + .1, C(.1)), 4.75, 5.05, 'paint.trim', 'frame')
    k.moss_sites.append(((-2.9, -2.9, 0), (2.9, -2.9, 0)))


def lrmturret(k):
    _base(k)
    with k.tagged('turret'):
        # neck pillar and trunnion block, then the two launcher pods on each side
        taper(k, rect_poly(-.98, -.9, .98, .98), 5.0, rect_poly(-.8, -.55, .8, .6), 8.69, 'paint.second', 'frame')
        k.box((0, 1.0, 7.99), (2.07, .87, .76), 'paint.trim', 'frame', ch=C(.05))
        for s in (-1, 1):
            xc = s * 1.83
            k.box((xc, -.2, 8.0), (1.56, 6.52, 5.4), 'paint.main', 'mass', ch=C(.1), top=C(.08))
            k.box((xc, -3.52, 8.0), (1.68, .2, 5.5), 'paint.trim', 'frame')
            for r_ in range(3):
                for c_ in range(2):
                    with k.at((xc - .38 + c_ * .76, -3.65, 6.2 + r_ * 1.45)):
                        k.cyl((0, 0, 0), .3, .2, 'rubber', 'detail', sides=10)
            k.slab(xc - .5, xc + .5, 5.35, 5.6, -3.2, 2.9, 'paint.second', 'frame')
        k.box((0, -.2, 10.6), (.5, 6.2, .18), 'paint.accent', 'detail')
    k.hits = [dict(p=(1.8, -1.0, 8.0), d=(-1, 0, 0), r=2.6, depth=0, sag=0)]


def mediumautocannonturret(k):
    _base(k)
    with k.tagged('turret'):
        taper(k, rect_poly(-1.0, 1.6, 1.0, 3.9), 5.03, rect_poly(-.8, 1.8, .8, 3.7), 6.4, 'paint.second', 'frame')
        k.box((0, 1.73, 7.4), (3.25, 3.74, 2.26), 'paint.main', 'mass', ch=C(.14), top=C(.1))
        k.extrude([(3.5, 6.27), (7.3, 6.27), (7.3, 6.6), (3.5, 7.9)], -.65, .65, 'paint.main', 'mass', axis='x')
        for s in (-1, 1):
            xc = s * .78
            taper(k, rect_poly(xc - .7, -7.3, xc + .7, .97), 6.85, rect_poly(xc - .6, -7.2, xc + .6, .9), 8.18, 'paint.main', 'mass', barrel=True)
            k.box((xc, -7.3, 7.5), (1.3, .2, 1.2), 'paint.trim', 'frame')
            k.cyl((xc, -7.5, 7.5), .26, .2, 'rubber', 'detail', sides=10)
        k.slab(-1.0, 1.0, 7.9, 8.5, -.6, .4, 'glass', 'glass', pane=True)
    k.hits = [dict(p=(0, -1.0, 7.4), d=(0, 1, -.3), r=3.2, depth=0, sag=0)]


def turret_damage(k, condition):
    """Partial: the head leans and the barrels droop. Destroyed: head thrown off its ring, base stub kept."""
    rng = random.Random(k.seed * 31 + 11)
    head = [p for p in k.pieces if p.get('group') == 'turret']
    base = [p for p in k.pieces if p.get('group') != 'turret']
    pivot = (0, 0, 5.0)
    if condition == 'partial':
        for p in head:
            p['v'] = [geo.rotate_about(v, pivot, (1, 0, 0), -.28) for v in p['v']]
            if p.get('barrel'):
                p['v'] = [geo.rotate_about(v, (v[0], 0, 6.5), (1, 0, 0), .22) for v in p['v']]
    else:
        for p in base:
            p['v'] = [(x, y, min(z, 2.4 + .0 * x)) if z > 2.4 else (x, y, z) for x, y, z in p['v']]
        for i, p in enumerate(head):
            ang = rng.uniform(-.6, .6)
            p['v'] = [geo.rotate_about(v, pivot, (0, 0, 1), ang) for v in p['v']]
            p['v'] = [geo.rotate_about(v, pivot, (1, 0, 0), -1.1 + rng.uniform(-.1, .1)) for v in p['v']]
            p['v'] = [(x + 3.5, y - 1.0, z - 3.4) for x, y, z in p['v']]
        w, d, h = k.meta['size']
        for i in range(10):
            geo_chunk = rng.uniform(.5, 1.2)
            kit.rock(k, (rng.uniform(-3, 3), rng.uniform(-3, 3), .2), geo_chunk, 'ferro', rng)
    k.hits = [dict(p=(0, 0, .5), d=(0, 0, -1), r=3, depth=0, sag=0)]


# ------------------------------------------------------------ rubble for the buildings
def _remnant_corner(k, rng, w, d, h, fh, mat='paint.main'):
    # one standing broken corner of the shell (identity remnant)
    x, y = -w * .36, -d * .34
    k.box((x, y, fh / 2), (.5, 5.0, fh), mat, 'frame', ch=C(.06))
    k.box((x + 2.0, y - 2.4, fh * .35), (4.0, .5, fh * .7), mat, 'frame', ch=C(.06))


RUBBLE = {
    'genericmilitary': dict(core='ferro', heap=.24, slab='paint.main', n=14, remnant='tower', fh=3.6),
    'geodesicdome': dict(core='ferro', heap=.2, slab='paint.main', n=12, remnant='ribs', fh=4.0),
    'liquidstorage': dict(core='ferro', heap=.18, slab='paint.main', n=10, remnant='ring', fh=3.0),
    'largefueltank': dict(core='ferro', heap=.16, slab='paint.main', n=10, remnant='pedestal', fh=4.7),
    'ammodump': dict(core='ferro', heap=.34, slab='paint.main', n=12, remnant='bay', fh=2.6),
}


def rubble(k, name):
    spec = RUBBLE[name]
    rng = random.Random(k.seed * 31 + 7)
    w, d, h = SIZES[name]
    k.pieces, k.hits = [], []
    peak = h * spec['heap']
    fw, fd = w * .94, d * .94
    kit.mound(k, fw, fd, peak, spec['core'], power=.55)

    def hm(x, y):
        f = math.hypot(x / (fw / 2), y / (fd / 2))
        return peak * max(0.0, 1 - f * f) ** .5 * .85
    # kinked frame members leaning out of the heap
    for i in range(5):
        x, y = rng.uniform(-fw * .28, fw * .28), rng.uniform(-fd * .28, fd * .28)
        a = (x - 1.3, y - .8, hm(x, y) * .8)
        m = (x, y, hm(x, y) + rng.uniform(1.2, 2.6))
        b = (x + 1.6, y + .8, hm(x, y) * .9 + rng.uniform(.2, 1.0))
        kit.ibeam(k, a, m, d=.3, w=.18)
        kit.ibeam(k, m, b, d=.3, w=.18)
    # fallen skin plates, thick, resting at 0..30 degrees on the heap
    for i in range(spec['n']):
        x, y = rng.uniform(-fw * .4, fw * .4), rng.uniform(-fd * .4, fd * .4)
        if (x / (fw / 2)) ** 2 + (y / (fd / 2)) ** 2 > 1:
            continue
        with k.at((x, y, hm(x, y) + .05), yaw=rng.uniform(-math.pi, math.pi), pitch=rng.uniform(-.5, .5), roll=rng.uniform(-.4, .4)):
            k.box((0, 0, 0), (rng.uniform(1.6, 3.4), rng.uniform(1.4, 2.8), .26), spec['slab'] if rng.random() < .7 else 'paint.second', 'skin', ch=C(.04))
    for i in range(34):
        x, y = rng.uniform(-fw * .45, fw * .45), rng.uniform(-fd * .45, fd * .45)
        if (x / (fw / 2)) ** 2 + (y / (fd / 2)) ** 2 > 1:
            continue
        kit.rock(k, (x, y, hm(x, y) * .9), rng.uniform(.45, 1.3), 'ferro', rng)
    fh = spec['fh']
    r = spec['remnant']
    if r == 'tower':
        x, y = w * .38, d * .30
        k.prism(rect_poly(x - 2.8, y - 2.8, x + 2.8, y + 2.8, C(.2)), 0, fh, 'paint.second', 'frame', top=C(.15))
        k.box((x - 1.4, y - 2.9, fh * .5), (2.4, .5, fh), 'paint.second', 'frame')
        for s in (-1, 1):
            k.box((x + s * 2.5, y + 2.7, fh + .5), (.3, .3, 1.6), 'metal', 'frame')
    elif r == 'ribs':
        R = fw / 2
        for j in range(3):
            a0 = math.radians(40 + j * 30)
            pts = [((R * .95) * math.cos(a0 + t * .12), (R * .95) * math.sin(a0 + t * .12), .3 + fh * math.sin(t * math.pi / 5) * (1 - .1 * j)) for t in range(6)]
            for p0, p1 in zip(pts, pts[1:]):
                k.beam(p0, p1, .34, 'paint.trim', 'frame', h=.3)
        x, y = 0, -d * .38
        taper(k, rect_poly(x - 2.2, y - 2.0, x + 2.2, y + 2.0, C(.2)), 0, rect_poly(x - 1.7, y - 1.6, x + 1.7, y + 1.6, C(.15)), fh, 'paint.main', 'frame')
    elif r == 'ring':
        R = w * .45
        for i in range(4):
            a0 = math.radians(150 + 18 * i)
            a1 = a0 + math.radians(15)
            q = [((R + .1) * math.cos(a0), (R + .1) * math.sin(a0)), ((R + .1) * math.cos(a1), (R + .1) * math.sin(a1)),
                 ((R - .35) * math.cos(a1), (R - .35) * math.sin(a1)), ((R - .35) * math.cos(a0), (R - .35) * math.sin(a0))]
            k.prism(q, 0, fh * (.6 + .4 * rng.random()), 'paint.main', 'frame')
        k.box((0, -d * .42, fh * .5), (2.9, 1.9, fh), 'paint.trim', 'frame', ch=C(.05))
    elif r == 'pedestal':
        taper(k, _poly(w * .30, 6, math.pi / 2), 0, _poly(w * .22, 6, math.pi / 2), fh, 'paint.second', 'frame')
        k.box((-w * .36, 0, 1.7), (9.0, 2.0, 2.1), 'paint.second', 'frame')
    else:
        x, y = 0, -d * .38
        k.box((x, y, fh / 2), (6.4, 1.8, fh), 'paint.trim', 'frame', ch=C(.06))
        k.box((x - 2.6, y, fh / 2), (1.2, 1.9, fh * .8), 'paint.second', 'skin')
    for p in k.pieces:
        p['v'] = [(x, y, max(.01, z)) for x, y, z in p['v']]
    k.hits = [dict(p=(0, 0, .3), d=(0, 0, -1), r=min(w, d) * .28, depth=0, sag=0)]
    k.meta.update(collapse='full-rubble', rubble_height_ratio=spec['heap'])


# ------------------------------------------------------------ light sockets (fitted frame, metres)
def _lights_genericmilitary(size):
    out = [('door_01', (-2.2, -8.1, 3.3), (0, -1, -.6), 'spot', 'door')]
    wc = lambda n, a, b: [a + (i + .5) * (b - a) / n for i in range(n)]
    out += [(f'window_front_{i+1:02}', (c, -6.9, 4.8), (0, -1, 0), 'point', 'window') for i, c in enumerate(wc(6, -13.9, 9.8))]
    out += [(f'window_rear_{i+1:02}', (c, 8.3, 4.8), (0, 1, 0), 'point', 'window') for i, c in enumerate(wc(6, -13.9, 9.8))]
    out += [(f'window_west_{i+1:02}', (-13.9, c, 4.8), (-1, 0, 0), 'point', 'window') for i, c in enumerate(wc(4, -6.9, 8.3))]
    return out


def _lights_geodesicdome(size):
    out = [('door_west_01', (-8.9, -1.9 + 2.075, 2.4), (-1, 0, -.3), 'spot', 'door'),
           ('door_east_01', (8.9, 2.4 + 2.075 - 2.0, 2.4), (1, 0, -.3), 'spot', 'door')]
    for j in range(6):
        phi = math.radians(30 + 60 * j)
        out.append((f'window_crown_{j+1:02}', (7.2 * math.cos(phi), 7.2 * math.sin(phi) + 2.075, 7.0), (math.cos(phi), math.sin(phi), 0), 'point', 'window'))
    return out


def _lights_liquidstorage(size):
    return [('door_01', (0, -13.2, 2.8), (0, -1, -.4), 'spot', 'door'),
            ('hut_01', (0, -10.4, 14.4), (0, -1, -.3), 'point', 'window')]


def _lights_largefueltank(size):
    return [('hatch_01', (2.9, -9.0, 3.4), (0, -1, -.3), 'spot', 'door')] + \
           [(f'panel_{i+1:02}', (2.9 + 8.6 * math.cos(a), 8.6 * math.sin(a), 3.7), (math.cos(a), math.sin(a), 0), 'point', 'window')
            for i, a in enumerate((math.radians(-150), math.radians(-90), math.radians(-30)))]


def _lights_ammodump(size):
    return [('door_front_01', (0, -8.9, 4.2), (0, -1, -.5), 'spot', 'door'), ('door_rear_01', (0, 8.9, 4.2), (0, 1, -.5), 'spot', 'door'),
            ('door_west_01', (-9.4, 0, 3.0), (-1, 0, -.5), 'spot', 'door'), ('door_east_01', (9.4, 0, 3.0), (1, 0, -.5), 'spot', 'door')]


LIGHTS = {'genericmilitary': _lights_genericmilitary, 'geodesicdome': _lights_geodesicdome, 'liquidstorage': _lights_liquidstorage,
          'largefueltank': _lights_largefueltank, 'ammodump': _lights_ammodump}


# ------------------------------------------------------------ registration
DRAFTS = {
    'genericmilitary': (genericmilitary, 'mil_khaki'),
    'geodesicdome': (geodesicdome, 'dome_sand'),
    'liquidstorage': (liquidstorage, 'storage_tan'),
    'largefueltank': (largefueltank, 'fuel_grey'),
    'ammodump': (ammodump, 'ammo_bunker'),
    'lrmturret': (lrmturret, 'turret_olive'),
    'mediumautocannonturret': (mediumautocannonturret, 'turret_olive'),
}
DAMAGE = {'lrmturret': turret_damage, 'mediumautocannonturret': turret_damage}


def _known(**kw):
    return counts.known(**kw)


counts.CONTRACTS.update({
    'd_heavywall': _known(panels_sections=[f'sand face unit {i+1}/6' for i in range(6)] + ['lower hexagonal brace field 1/2', 'lower hexagonal brace field 2/2'],
                          legs_supports=['one +Y tapered terminal pier, four cast tiers']),
    'lrmturret': _known(panels_sections=['left launcher pod', 'right launcher pod'],
                        legs_supports=['one tapered base', 'one neck pillar with trunnion block']),
    'mediumautocannonturret': _known(panels_sections=['rear counterweight housing', 'gun mantle'],
                                     legs_supports=['one tapered base', 'one neck frustum'],
                                     antennas_dishes=['left autocannon barrel', 'right autocannon barrel']),
    'genericmilitary': _known(doors=['front double door in teal bay'],
        windows=[f'front window {i+1}/6' for i in range(6)] + [f'rear window {i+1}/6' for i in range(6)] +
                [f'west end window {i+1}/4' for i in range(4)] + [f'east end window {i+1}/3' for i in range(3)] +
                [f'tower {s} window {i+1}/2' for s in ('front', 'east', 'rear', 'west') for i in range(2)],
        panels_sections=['front left panel', 'front right panel', 'rear panel 1/3', 'rear panel 2/3', 'rear panel 3/3',
                         'west end panel 1/2', 'west end panel 2/2', 'east end panel 1/2', 'east end panel 2/2',
                         'tower front panel', 'tower east panel', 'tower rear panel', 'tower west panel'],
        legs_supports=['one rear east tower', 'two front canopy posts'],
        stairs_ladders=['one canopy landing slab']),
    'geodesicdome': _known(doors=['west flank door', 'east flank door'],
        windows=[f'crown slit {i+1}/6' for i in range(6)],
        panels_sections=[f'geodesic facet {i+1}/40' for i in range(40)] + ['front tower vent set (3 louvres)', 'front tower access panel'],
        arches_ribs=['one geodesic strut frame'], legs_supports=['one tapered front tower'],
        antennas_dishes=['one striped crown mast', 'one crown stack pipe']),
    'liquidstorage': _known(doors=['front base control door'],
        panels_sections=[f'{t} tier face {i+1}/10' for t in ('lower', 'upper') for i in range(10)],
        tanks=['one decagonal drum'], stairs_ladders=['one front stair column to roof hut'],
        antennas_dishes=['one roof vent dome', 'one roof side vent']),
    'largefueltank': _known(doors=['front round access hatch'],
        panels_sections=[f'{t} band face {i+1}/10' for t in ('hazard', 'belt', 'upper') for i in range(10)] +
                        [f'pedestal control panel {i+1}/3' for i in range(3)],
        tanks=['one decagonal tank'], legs_supports=['one hexagonal pedestal'],
        openings_bays=['one west outlet duct with end flange']),
    'ammodump': _known(doors=['front wide steel door', 'rear wide steel door', 'west narrow door', 'east narrow door'],
        panels_sections=[f'{f} face {s}' for f in ('front', 'east', 'rear', 'west') for s in ('left tan', 'green bay', 'right tan')],
        openings_bays=['front door porch', 'rear door porch'], stairs_ladders=['west four-tread landing', 'east four-tread landing']),
})
counts.NOTES.update({
    'd_heavywall': 'Six narrow olive-sand vertical panels per side above a belt; below it a chain of two hexagonal braced fields (pale); one four-tier tapered terminal pier; straight coping; no openings.',
    'lrmturret': 'Stock mesh: Base (tapered frustum 5.75 m square, 5 m tall), neck pillar with trunnion block, two tall launcher pods flanking the pillar. Stock turret texture is olive military.',
    'mediumautocannonturret': 'Stock mesh: same base; neck frustum; gun mantle; rear tapered counterweight; two long parallel barrels to the -Y front.',
    'genericmilitary': 'Five-side study: long flat-roof block, six window band on front and rear, 4 west, 3 east visible; double door in a teal bay under a canopy on the front; taller rear-east tower with two windows per face; khaki front/west and dark grey rear/east faces.',
    'geodesicdome': 'Five-side study: decagonal geodesic dome of 40 triangular facets (frequency-2 icosahedron cap), two flank doors with green lamp, six crown slit windows (best estimate from top view), front tapered tower with three vent louvres, striped mast.',
    'liquidstorage': 'Decagonal drum (vertices on +-Y), two tiers of ten faces divided by a dark belt, sloped shoulder, front stair column up to a roof hut with sloped hood, roof vent.',
    'largefueltank': 'Decagon drum (vertices on +-X) on a hex pedestal (vertices on +-Y); hazard band, belt, upper course; yellow-black octagonal crown; west duct with end flange; round hatch and three control panels on the pedestal.',
    'ammodump': 'Battered flat-topped bunker: four faces each tan/green/tan with a hazard band; wide doors with floor grille porches on +-Y, narrow doors with four-tread landings on +-X; recessed gravel roof with four red corner marks.',
})
counts.ORIGINS.update(ORIGINS)

import drafts as kit  # noqa: E402: helpers exist once drafts has defined them
