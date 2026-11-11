"""Military base family (split from intent_military.py): military building parts: lofts, doors, windows, stencils, antennas, dishes, animated nodes, sandbags. Header of the original file:
Intent pass, military base family, batch 1 (owner 2026-10-24: "on to the next set").

BattleTech base language over each stock silhouette: battered ferrocrete and plate masses with a proud plinth,
chamfered heads and stepped armour ribs; blast doors in deep chamfered portals with hazard jambs and sills;
armoured slit windows under visors; unit and faction stencils; antenna masts and dishes; sandbag barriers.
Faction colours follow the stock (Steiner grey, Davion tan-yellow, Liao green, generic khaki). MW5/BT2018 are
vocabulary only; nothing is copied.
"""
import math

import numpy as np

import geo
import ind_parts as ip
import intent as I
import intent_power as PW

count = I.count


# ================================================================== military parts
def loft(k, rings, mat, role='mass', caps=True, flip=False, **fl):
    """Closed solid through rings of equal vertex count (3D points)."""
    n = len(rings[0])
    verts = [tuple(p) for r in rings for p in r]
    faces = []
    if caps:
        faces.append(tuple(reversed(range(n))))
    for i in range(len(rings) - 1):
        a, b = i * n, (i + 1) * n
        for j in range(n):
            jj = (j + 1) % n
            faces.append((a + j, a + jj, b + jj, b + j))
    if caps:
        last = (len(rings) - 1) * n
        faces.append(tuple(range(last, last + n)))
    if flip:
        faces = [tuple(reversed(f)) for f in faces]
    return k.add(verts, faces, mat, role, **fl)


def rring(x0, y0, x1, y1, z, ch=.4):
    return [(x, y, z) for x, y in geo.rect_poly(x0, y0, x1, y1, max(ch, .02))]


def frame_of(box, s):
    """(origin xy, yaw, length) of a side of a plan rectangle; local x = viewer's right, outward -Y."""
    x0, y0, x1, y1 = box
    xc, yc = (x0 + x1) / 2, (y0 + y1) / 2
    return {'s': ((xc, y0), 0.0, x1 - x0), 'e': ((x1, yc), math.pi / 2, y1 - y0),
            'n': ((xc, y1), math.pi, x1 - x0), 'w': ((x0, yc), -math.pi / 2, y1 - y0)}[s]


def batter_mass(k, box, z0, z1, bat, plinth=.5, cap=.35, ch=.5, mat='ferro.main', plinth_mat='ferro.second', proud=.18):
    """Battered armoured mass: proud plinth, walls leaning in by `bat`, chamfered head.
    Returns ins(z): inward offset of the face at height z (for parts placed on the face)."""
    x0, y0, x1, y1 = box
    zp = z0 + plinth
    loft(k, [rring(x0 - proud, y0 - proud, x1 + proud, y1 + proud, z0, ch + proud),
             rring(x0 - proud, y0 - proud, x1 + proud, y1 + proud, zp - .08, ch + proud),
             rring(x0 - proud + .08, y0 - proud + .08, x1 + proud - .08, y1 + proud - .08, zp, ch + proud - .08)], plinth_mat, 'mass')
    zt = z1 - cap
    loft(k, [rring(x0, y0, x1, y1, zp - .02, ch), rring(x0 + bat, y0 + bat, x1 - bat, y1 - bat, zt, ch),
             rring(x0 + bat + cap, y0 + bat + cap, x1 - bat - cap, y1 - bat - cap, z1, ch)], mat, 'mass')
    i_ = bat + cap + .05
    k.prism(geo.rect_poly(x0 + i_, y0 + i_, x1 - i_, y1 - i_, max(.02, ch - .05)), z1 - .07, z1 + .01, 'gravel', 'roof', walk=True)
    return lambda z: bat * min(1.0, max(0.0, (z - zp) / (zt - zp)))


def on_face(k, box, s, xl, z, ins, z0=0.0):
    """Context frame on a battered face: origin on the face at (xl, z), leaning with it (outward -Y)."""
    (o, yaw, _) = frame_of(box, s)
    dz = 1.0
    tilt = math.atan2(ins(z + dz) - ins(z), dz)
    out = (math.cos(yaw - math.pi / 2), math.sin(yaw - math.pi / 2))
    tx, ty = math.cos(yaw), math.sin(yaw)
    p = (o[0] + tx * xl - out[0] * ins(z), o[1] + ty * xl - out[1] * ins(z), z)
    return k.at(p, yaw=yaw, pitch=-tilt)


def blast_door(k, w, h, xc=0.0, depth=.55, back=.1, fw=.42, yaw=0.0, label=None, lamp=True, sill=True):
    """Armoured blast door in a deep chamfered portal (local wall frame, outward -Y, floor z=0):
    plate jambs and lintel with hazard strips, ribbed split leaves with a hazard foot band, hazard sill."""
    x0, x1 = xc - w / 2, xc + w / 2
    for a, b in ((x0 - fw, x0), (x1, x1 + fw)):
        k.slab(a, b, 0, h + fw, -depth, back, 'paint.plate', 'frame', front=.07)
    k.slab(x0 - fw, x1 + fw, h, h + fw, -depth, back, 'paint.plate', 'frame', front=.07)
    k.prism([(x0 - fw - .1, h + fw), (x1 + fw + .1, h + fw), (x1 + fw - .1, h + fw + .28), (x0 - fw + .1, h + fw + .28)],
            -depth - .08, back, 'paint.second', 'frame', axis='y')
    for a, b in ((x0 - fw + .07, x0 - .05), (x1 + .05, x1 + fw - .07)):
        k.slab(a, b, .12, min(1.6, h * .5), -depth - .012, -depth + .02, 'hazard', 'detail')
    yl = min(-depth + .32, -.04)                # leaf always proud of the wall face (never buried in shallow portals)
    k.slab(x0, x1, 0, h, yl, yl + .14, 'paint.door', 'skin', door=True, feature='door')
    n = max(2, int(h / .85))
    for i in range(1, n):
        z = h * i / n
        k.slab(x0 + .1, x1 - .1, z - .06, z + .06, yl - .07, yl + .01, 'paint.trim', 'frame')
    k.slab(xc - .07, xc + .07, 0, h, yl - .09, yl + .01, 'paint.trim', 'frame')
    k.slab(x0 + .04, x1 - .04, .12, min(.62, h * .2), yl - .012, yl + .01, 'hazard', 'detail')
    if sill:
        k.slab(x0 - fw, x1 + fw, 0, .08, -depth - .55, -depth + .02, 'hazard', 'frame', walk=True)
    if label:
        stencil(k, label, xc, h + fw + .9 if w > 3 else h - .5, min(1.4, w * .22), -depth - .01 if w <= 3 else -.01)
    if lamp:
        I.wall_lamp(k, x1 + fw + .35, min(h + .2, h + fw - .1), yaw) if w > 3 else I.wall_lamp(k, xc, h + fw + .45 if not label else h + fw + .45, yaw)
    PW.status_light(k, (x1 + fw * .5, -depth - .02, h * .55 if h < 3 else 1.6), 'lamp_red', r=.06)
    count(k, 'doors')


def slit_window(k, xc, zc, w=1.4, h=.42, d=.22):
    """Armoured slit window: proud chamfered frame, dark glass set back, sloped visor above."""
    x0, x1, z0, z1 = xc - w / 2, xc + w / 2, zc - h / 2, zc + h / 2
    f = .16
    k.slab(x0 - f, x0, z0 - f, z1 + f, -d, .04, 'paint.plate', 'frame', front=.04)
    k.slab(x1, x1 + f, z0 - f, z1 + f, -d, .04, 'paint.plate', 'frame', front=.04)
    k.slab(x0, x1, z0 - f, z0, -d, .04, 'paint.plate', 'frame', front=.04)
    k.slab(x0, x1, z1, z1 + f, -d, .04, 'paint.plate', 'frame', front=.04)
    k.slab(x0, x1, z0, z1, -.06, .0, 'glass', 'glass', pane=True)
    k.prism([(x0 - f - .05, z1 + f), (x1 + f + .05, z1 + f), (x1 + f + .05, z1 + f + .12), (x0 - f - .05, z1 + f + .12)], -d - .12, .04,
            'paint.second', 'frame', axis='y')
    count(k, 'windows')


SEG = {'0': 'abcdef', '1': 'bc', '2': 'abged', '3': 'abgcd', '4': 'fgbc', '5': 'afgcd', '6': 'afgedc', '7': 'abc', '8': 'abcdefg',
       '9': 'abcdfg', '-': 'g', 'H': 'bcefg'}


def stencil(k, text, xc, zc, h, yf, mat='paint.stencil'):
    """Stencilled unit/bay number (segment glyphs with stencil bridges) in a wall plane."""
    w, t, gap = h * .56, h * .14, h * .22
    x = xc - (len(text) * w + (len(text) - 1) * gap) / 2
    R = {'a': (t * .6, h - t, w - t * .6, h), 'g': (t * .6, h / 2 - t / 2, w - t * .6, h / 2 + t / 2), 'd': (t * .6, 0, w - t * .6, t),
         'f': (0, h / 2 + t * .45, t, h - t * .45), 'b': (w - t, h / 2 + t * .45, w, h - t * .45),
         'e': (0, t * .45, t, h / 2 - t * .45), 'c': (w - t, t * .45, w, h / 2 - t * .45)}
    for ch in text:
        for s in SEG.get(ch, ''):
            a = R[s]
            k.slab(x + a[0], x + a[2], zc - h / 2 + a[1], zc - h / 2 + a[3], yf - .012, yf + .004, mat, 'detail')
        x += w + gap


def stencil_panel(k, text, xc, zc, h, yf):
    """Faction/unit plate: dark chamfered plate with a stencil number and a hazard tab."""
    w = len(text) * h * .8 + h * .6
    k.slab(xc - w / 2, xc + w / 2, zc - h * .75, zc + h * .75, yf - .05, yf + .01, 'paint.trim', 'frame', front=.02)
    stencil(k, text, xc, zc, h, yf - .05)
    k.slab(xc + w / 2 - .02, xc + w / 2 + .25, zc - h * .75, zc + h * .75, yf - .04, yf + .01, 'hazard', 'detail')


def antenna(k, x, y, z, h, r=.06):
    k.box((x, y, z + .12), (.5, .5, .24), 'paint.plate', 'frame', ch=.06, top=.04)
    k.prism(geo.reg_poly(r * 1.6, 8, cx=x, cy=y), z + .24, z + .24 + h * .4, 'metal', 'frame', top=r * .5)
    k.cyl((x, y, z + .24 + h * .7), r * .6, h * .6, 'metal', 'frame', sides=8)
    k.cyl((x, y, z + .3 + h), r * 1.4, .12, 'lamp_red', 'frame', sides=8)
    count(k, 'antennas_dishes')


def dish(k, c, r, yaw, tilt, z, mat='paint.second'):
    """Parabolic dish on a yoke: shell, rim, feed horn on three struts. Opens toward yaw (outward), tilted up."""
    with k.at((c[0], c[1], z), yaw=yaw):
        k.cyl((0, 0, -.35), r * .22, .7, 'paint.plate', 'frame', sides=12, top=.04)
        with k.at((0, 0, 0), pitch=math.pi / 2 - tilt):
            # local +z = boresight after the pitch; dish opens toward local +z
            ip.lathe(k, [(0.0, 0.0), (r * .5, r * .07), (r, r * .28), (r * .98, r * .33), (r * .5, r * .12), (0.0, .06)], mat, 'frame', n=32)
            ip.tube(k, [(math.cos(a) * r * .97, math.sin(a) * r * .97, r * .3) for a in [i * math.tau / 32 for i in range(33)]], .03,
                    'paint.trim', 'frame', sides=6, caps=False)
            for a in (0, 2.1, 4.2):
                k.beam((math.cos(a) * r * .9, math.sin(a) * r * .9, r * .28), (0, 0, r * .75), .035, 'metal', 'frame')
            k.cyl((0, 0, r * .78), r * .08, r * .16, 'paint.trim', 'frame', sides=10)
            k.box((0, 0, -.25), (r * .4, r * .4, .3), 'paint.plate', 'frame', ch=.05)
    count(k, 'antennas_dishes')


from contextlib import contextmanager  # noqa: E402


@contextmanager
def anim_node(k, name, pivot, parent=None, axis=(0.0, 0.0, 1.0), stock=None):
    """Everything built inside becomes the animated stock node `name` (exact stock node name): its faces carry
    an id the bake splits into a child glTF node whose origin is `pivot` (on the rotation axis)."""
    specs = k.meta.setdefault('anim_nodes', [])
    aid = len(specs) + 1
    specs.append(dict(name=name, pivot=tuple(pivot), parent=parent, axis=tuple(axis), stock=stock or name))
    n0 = len(k.pieces)
    yield
    for pc in k.pieces[n0:]:
        pc['anim_id'] = aid
        pc['anim'] = name


@contextmanager
def _null0():
    yield


def radar_dipole(k, name, pivot, L=3.2, stock=None, beacon=False):
    """Rotating radar: static pedestal under the pivot, rotating hub, a lattice-faced dipole bar and end fins."""
    x, y, z = pivot
    k.cyl((x, y, z - .3), .32, .6, 'paint.plate', 'frame', sides=14, top=.06)
    with anim_node(k, name, pivot, stock=stock):
        k.cyl((x, y, z + .12), .42, .24, 'paint.trim', 'frame', sides=14, top=.05)
        k.box((x, y, z + .45), (L, .36, .38), 'paint.plate', 'frame', ch=.08, top=.06)
        k.box((x, y - .2, z + .45), (L - .3, .04, .28), 'grate', 'frame')
        for s_ in (-1, 1):
            k.box((x + s_ * (L / 2 - .1), y, z + .55), (.12, .5, .7), 'paint.second', 'frame', ch=.03)
        if beacon:       # rotating airfield beacon on a short post above the radar bar: twin lens drum, cap
            k.cyl((x, y, z + 1.0), .22, .5, 'paint.trim', 'frame', sides=10)
            k.cyl((x, y, z + 1.95), 1.25, 1.4, 'paint.plate', 'frame', sides=24, top=.12)
            for s_ in (-1, 1):
                with k.at((x + s_ * 1.15, y, z + 1.95), yaw=s_ * math.pi / 2, pitch=math.pi / 2):
                    k.cyl((0, 0, 0), .85, .2, 'lamp', 'frame', sides=20)
            k.cyl((x, y, z + 2.75), 1.0, .25, 'paint.second', 'frame', sides=24, top=.1)
            k.cyl((x, y, z + 3.0), .1, .25, 'lamp_red', 'frame', sides=8)
        else:
            k.cyl((x, y, z + .75), .06, .3, 'lamp_red', 'frame', sides=8)
    count(k, 'antennas_dishes')


def spin_antenna(k, name, pivot, h=.9, arm=.7, stock=None, anim=True):
    """Small rotating whip: fixed stub mast below the pivot, rotating crossbar with two dipoles."""
    x, y, z = pivot
    with (anim_node(k, name, pivot, stock=stock) if anim else _null0()):
        k.cyl((x, y, z + .08), .12, .16, 'paint.trim', 'frame', sides=10)
        k.box((x, y, z + .2), (2 * arm, .08, .08), 'metal', 'frame')
        for s_ in (-1, 1):
            k.cyl((x + s_ * arm, y, z + .2 + h / 2), .03, h, 'metal', 'frame', sides=6)
        k.cyl((x, y, z + .35), .05, .3, 'metal', 'frame', sides=6)
    count(k, 'antennas_dishes')


def sandbag(k, p, yaw, L=.62, W=.36, H=.21):
    j = k.rng.uniform
    with k.at(p, yaw=yaw + j(-.06, .06)):
        k.box((0, 0, H / 2), (L * j(.94, 1.04), W * j(.95, 1.05), H * j(.92, 1.04)), 'canvas.sand', 'mass', ch=.1, top=.08, bot=.04)


def bag_run(k, pts, courses, wide=0, z0=0.0, step=.19, closed=False):
    """Sandbag wall along a plan polyline: staggered courses, two rows wide for the lower `wide` courses."""
    P = list(pts) + ([pts[0]] if closed else [])
    for c in range(courses):
        z = z0 + c * step
        rows = (-.19, .19) if c < wide else (0.0,)
        for a, b in zip(P, P[1:]):
            dx, dy = b[0] - a[0], b[1] - a[1]
            L = math.hypot(dx, dy)
            yaw = math.atan2(dy, dx)
            n = max(1, round(L / .6))
            odd = c % 2
            for r_i, off in enumerate(rows):
                nx, ny = -dy / L * off, dx / L * off
                m = n - (1 if (odd ^ r_i) else 0)
                sh = .5 if (odd ^ r_i) else 0.0
                for i in range(m):
                    t = (i + .5 + sh) / n
                    sandbag(k, (a[0] + dx * t + nx, a[1] + dy * t + ny, z), yaw)
    return z0 + courses * step
