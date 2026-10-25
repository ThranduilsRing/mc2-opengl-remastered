"""Intent pass, military base family, batch 1 (owner 2026-10-24: "on to the next set").

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


# ================================================================== field group
def fit_plan(k, x0, x1, y0, y1, n0=0):
    """Scale the plan of pieces[n0:] so its extents are exactly the stock ones (owner footprint rule: runs that
    tile must keep the stock ends and thickness; used where the parts are hand-placed bags or boards)."""
    P = [v for pc in k.pieces[n0:] for v in pc['v']]
    ax0, ax1 = min(v[0] for v in P), max(v[0] for v in P)
    ay0, ay1 = min(v[1] for v in P), max(v[1] for v in P)
    fx, fy = (x1 - x0) / (ax1 - ax0), (y1 - y0) / (ay1 - ay0)
    for pc in k.pieces[n0:]:
        pc['v'] = [(x0 + (v[0] - ax0) * fx, y0 + (v[1] - ay0) * fy, v[2]) for v in pc['v']]


def sandbagwall(k):
    bag_run(k, [(0.0, -4.98), (0.0, 4.98)], 5, wide=3)
    fit_plan(k, -.415, .415, -5.272, 5.272)          # the stock run exactly (tiles at 42.667 WU)
    count(k, 'panels_sections')


def sandbagbunker(k):
    """Stock: the C opens west, firing shield and gun on the east wall."""
    with k.at((0, 0, 0), yaw=math.pi):
        _bunker(k)


def _bunker(k):
    pts = [(3.35, 4.95), (-1.35, 4.95), (-3.2, 3.1), (-3.2, -3.1), (-1.35, -4.95), (3.35, -4.95)]
    top = bag_run(k, pts, 6, wide=3)
    # firing step along the west wall, timber boards on bag footings
    k.box((-2.55, 0, .17), (.55, 5.4, .34), 'wood.crate', 'mass', ch=.03, walk=True)
    for y in ip.frange(-2.6, 2.6, .26):
        k.box((-2.55, y, .345), (.55, .22, .02), 'wood.crate', 'detail')
    # sloped steel firing shield on the west wall, with a gun slot
    with k.at((-3.2, 0, top - .05), yaw=-math.pi / 2):
        with k.at((0, 0, 0), pitch=-.32):
            for a, b, za, zb in ((-1.4, -.17, 0, .7), (.17, 1.4, 0, .7), (-.17, .17, 0, .2), (-.17, .17, .42, .7)):
                k.slab(a, b, za, zb, -.04, .04, 'paint.plate', 'mass', front=.015)
            k.slab(-1.45, 1.45, .66, .74, -.07, .06, 'paint.trim', 'frame')
            stencil(k, '7', -.85, .36, .32, -.04)
        for x in (-1.1, 1.1):
            k.beam((x, .05, 0), (x, .4, -.35), .06, 'metal', 'frame')
    # tripod machine gun behind the slot (static mount)
    gx, gz = -2.35, top + .1
    for a in (0, 2.1, 4.2):
        k.beam((gx, 0, gz - .05), (gx + .55 * math.cos(a), .55 * math.sin(a), .34), .04, 'metal', 'frame')
    k.box((gx - .1, 0, gz + .05), (.6, .16, .18), 'paint.trim', 'frame', ch=.03)
    ip.tube(k, [(gx - .4, 0, gz + .06), (-3.75, 0, gz + .06)], .035, 'metal', 'frame', sides=8)
    k.box((gx - .05, .13, gz - .05), (.22, .12, .2), 'paint.second', 'frame', ch=.02)
    # ammunition boxes and a water can on the floor
    for x, y in ((-1.4, 3.6), (-.9, 3.65), (-1.15, 3.6)):
        k.box((x, y, .16 if x != -1.15 else .46), (.45, .26, .3), 'paint.second', 'frame', ch=.03, top=.02)
    k.box((-1.6, -3.7, .25), (.22, .36, .5), 'paint.second', 'frame', ch=.04, top=.04)
    count(k, 'panels_sections')


def army_tent(k, L, W, H, eave, vents=0, door_ends=('s', 'n')):
    """Olive canvas ridge tent along local y: timber deck, canvas body sagging between pole stations,
    reinforcing straps, eave valance, ridge pole ends, guy lines and pegs, framed doors in the gables."""
    zf = .22
    k.box((0, 0, zf / 2), (W + .5, L + .5, zf), 'wood.crate', 'mass', ch=.04, walk=True)
    for s in (-1, 1):
        k.box((s * (W / 2 + .22), 0, zf / 2), (.08, L + .55, zf + .04), 'wood.crate', 'frame')
    nb = max(1, round(L / 2.6))
    ys = [-L / 2 + L * i / (2 * nb) for i in range(2 * nb + 1)]
    rings = []
    for i, y in enumerate(ys):
        s = 0.0 if i % 2 == 0 else .13
        if i in (0, len(ys) - 1):
            s = 0.0
        rings.append([(-W / 2 + .08 + s * .5, y, zf), (W / 2 - .08 - s * .5, y, zf), (W / 2 - s * .7, y, eave - s * .5),
                      (W * .25 - s * .2, y, (eave + H) / 2 - s), (0, y, H - s * .7), (-W * .25 + s * .2, y, (eave + H) / 2 - s),
                      (-W / 2 + s * .7, y, eave - s * .5)])
    loft(k, rings, 'canvas.canvas', 'mass', flip=True)
    for i, y in enumerate(ys):
        if i % 2:
            continue
        band = []
        for dy in ((-.07, .07) if 0 < i < len(ys) - 1 else ((.03, .17) if i == 0 else (-.17, -.03))):
            yy = min(max(y + dy, -L / 2), L / 2)
            band.append([(-W / 2 + .06, yy, zf + .02), (W / 2 - .06, yy, zf + .02), (W / 2 + .03, yy, eave + .02), (W * .25 + .02, yy, (eave + H) / 2 + .03),
                         (0, yy, H + .03), (-W * .25 - .02, yy, (eave + H) / 2 + .03), (-W / 2 - .03, yy, eave + .02)])
        loft(k, band, 'canvas.canvas2', 'skin', flip=True)
        for s in (-1, 1):
            ex, ez = s * (W / 2 + .03), eave
            gx = s * (W / 2 + .62)
            ip.tube(k, [(ex, y, ez), (gx, y, .05)], .012, 'canvas.sand', 'frame', sides=5, caps=False)
            k.box((gx, y, .12), (.06, .06, .28), 'wood.crate', 'frame')
        if i in (0, len(ys) - 1):
            ye = y + (-.05 if i == 0 else .05)
            k.cyl((0, ye, (zf + 2.35 + H) / 2), .055, H - zf - 2.35, 'wood.crate', 'frame', sides=8)
            k.cyl((0, ye + (-.12 if i == 0 else .12), H + .05), .05, .3, 'metal', 'frame', sides=8)
    for s in (-1, 1):
        k.beam((s * (W / 2 + .02), -L / 2, eave - .12), (s * (W / 2 + .02), L / 2, eave - .12), .05, 'canvas.canvas2', 'frame', h=.28)
    for end in door_ends:
        sg = -1 if end == 's' else 1
        with k.at((0, sg * (L / 2 + .02), zf), yaw=0.0 if sg < 0 else math.pi):
            for a, b in ((-.62, -.5), (.5, .62)):
                k.slab(a, b, 0, 2.05, -.06, .03, 'wood.crate', 'frame')
            k.slab(-.62, .62, 2.05, 2.17, -.06, .03, 'wood.crate', 'frame')
            k.slab(-.5, .5, 0, 2.05, -.02, .02, 'paint.door', 'skin', door=True, feature='door')
            k.slab(-.32, .32, 1.15, 1.75, -.035, -.02, 'glass', 'glass', pane=True)
            for z in (1.12, 1.45, 1.78):
                k.slab(-.34, .34, z - .02, z + .02, -.045, -.02, 'wood.crate', 'frame')
            k.slab(-.8, .8, 0, .06, -.6, .0, 'wood.crate', 'frame', walk=True)
            stencil(k, '4', -1.4, 1.2, .38, -.01, mat='paint.stencil')
        count(k, 'doors')
    for j in range(vents):
        y = -L / 2 + L * (j + .5) / vents
        for s in (-1, 1):
            with k.at((s * (W / 2 - .03), y, 0), yaw=s * math.pi / 2):
                k.slab(-.6, .6, 1.0, 1.55, -.03, .03, 'grate', 'frame')
                ip.tube(k, [(-.65, -.09, 1.62), (.65, -.09, 1.62)], .07, 'canvas.canvas2', 'frame', sides=8)
            count(k, 'windows')


def tent(k):
    army_tent(k, 9.6, 8.4, 4.0, 1.75)


def longtent(k):
    with k.at((0, 0, 0), yaw=math.pi / 2):
        army_tent(k, 19.6, 8.4, 3.9, 1.7, vents=4)


def crate(k, c, w, d, h, yaw=0.0, label='3'):
    """Munitions crate: timber body, corner posts and rim battens, plank slats, hazard band, stencil plate,
    rope handles and steel corner brackets."""
    with k.at(c, yaw=yaw):
        k.box((0, 0, h / 2), (w, d, h), 'wood.crate', 'mass', ch=.03)
        for sx in (-1, 1):
            for sy in (-1, 1):
                k.box((sx * (w / 2 - .05), sy * (d / 2 - .05), h / 2), (.14, .14, h), 'wood.crate', 'frame')
                k.box((sx * (w / 2 - .03), sy * (d / 2 - .03), h - .07), (.1, .1, .14), 'metal', 'detail')
                k.box((sx * (w / 2 - .03), sy * (d / 2 - .03), .07), (.1, .1, .14), 'metal', 'detail')
        for z in (.08, h - .08):
            k.box((0, 0, z), (w + .03, d + .03, .14), 'wood.crate', 'frame')
        k.box((0, 0, h - .32), (w + .02, d + .02, .2), 'hazard', 'skin')
        for z in ip.frange(.35, h - .5, .27):
            for sy in (-1, 1):
                k.box((0, sy * (d / 2 + .004), z), (w - .3, .012, .02), 'paint.trim', 'detail')
        k.slab(-w * .3, w * .3, h * .35, h * .6, -d / 2 - .02, -d / 2 + .01, 'paint.stencil', 'detail')
        stencil(k, label, 0, h * .475, h * .16, -d / 2 - .02, mat='paint.trim')
        for sx in (-1, 1):
            ip.tube(k, [(sx * (w / 2 + .02), -.2, h * .62), (sx * (w / 2 + .1), -.12, h * .55), (sx * (w / 2 + .1), .12, h * .55),
                        (sx * (w / 2 + .02), .2, h * .62)], .025, 'canvas.sand', 'frame', sides=6)
    count(k, 'crates')


def pallet(k, x, w, d):
    k.box((x, 0, .12), (w, d, .04), 'wood.crate', 'frame')
    for y in (-d / 2 + .1, 0, d / 2 - .1):
        k.box((x, y, .055), (w, .14, .11), 'wood.crate', 'frame')


def munitionscrates(k):
    w, d, h = 1.72, 2.0, 1.6
    for x in (-3.5, 0.0, 3.5):
        pallet(k, x, 3.5, 2.05)
    for i in range(6):
        crate(k, (-4.375 + 1.75 * i, 0, .14), w, d, h, label=str(i + 1))
    for i, x in enumerate((-.6, 1.15)):
        crate(k, (x, .03, .14 + h), w, d, h, yaw=.03 * (1 if i else -1), label=str(7 + i))
    # ratchet strap over the lower row ends
    for x in (-5.24, 5.24):
        k.box((x, 0, .14 + h / 2), (.02, .06, h), 'paint.valve', 'detail')


# ================================================================== armoured halls
DOORS = ('blast', 'bay', 'door', 'rollup')


def wedge(k, a, b, h, d, ea, eb, mat):
    """Glacis skirt piece in the local wall frame (outward -Y): slopes from (y=-d, z=0) to the face at z=h;
    mitred 45 degrees at block corners (ea/eb)."""
    da, db = (d if ea else 0.0), (d if eb else 0.0)
    ta, tb = (.1 if ea else 0.0), (.1 if eb else 0.0)
    v = [(a - da, -d, 0), (b + db, -d, 0), (b, 0, 0), (a, 0, 0), (a - ta, -.1, h), (b + tb, -.1, h), (b, 0, h), (a, 0, h)]
    f = [(3, 2, 1, 0), (4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)]
    k.add(v, f, mat, 'mass')


def mil_hall(k, box, H, openings=None, sides='nesw', t=.5, skirt=(1.9, .7), skirt_sides=None, rib=4.4, band=True,
             mat='ferro.main', roof=True, coping=True):
    """Armoured hall: cast walls cut for openings, mitred glacis skirt, plate ribs at bay lines, olive band,
    plate coping, roof deck flush with the wall head (walkable). openings: side -> [(kind, xl, z0, w, h[, label])];
    kinds blast | bay | door | rollup | window | slit | vent | stencil."""
    x0, y0, x1, y1 = box
    openings = openings or {}
    skirt_sides = sides if skirt_sides is None else skirt_sides
    F = I.faces_of(x0, y0, x1, y1, t)
    for s in sides:
        origin, yaw, L, _ = F[s]
        ops = openings.get(s, [])
        for op in ops:
            if op[0] != 'stencil' and abs(op[1]) + op[3] / 2 + .3 > L / 2:
                raise ValueError(f'{k.name}: {op[0]} at {op[1]} (w {op[3]}) runs off the {s} wall (length {L:.2f})')
        holes = [(op[1] - op[3] / 2, op[2], op[1] + op[3] / 2, op[2] + op[4]) for op in ops if op[0] in DOORS + ('window', 'vent')]
        with k.at(origin, yaw=yaw):
            ip.wall_frame(k, -L / 2, L / 2, 0, H, 0.0, holes, mat=mat, t=t)
            sk = skirt[0] if s in skirt_sides else 0.0
            # plate ribs at bay lines, clear of openings
            if rib:
                xs = list(rib) if isinstance(rib, (list, tuple)) else [-L / 2 + .6 + (L - 1.2) * i / max(1, round(L / rib)) for i in range(max(1, round(L / rib)) + 1)]
                for x in xs:
                    if any(hh[0] - .9 < x < hh[2] + .9 for hh in holes) or any(op[0] == 'slit' and abs(op[1] - x) < op[3] / 2 + .6 for op in ops):
                        continue
                    k.slab(x - .28, x + .28, max(.0, sk - .2), H - .45, -.26, .02, 'paint.plate', 'frame', front=.08)
            if band:
                for r in geo.subtract_rect((-L / 2 - .02, H - 1.55, L / 2 + .02, H - 1.0), holes):
                    k.slab(r[0], r[2], r[1], r[3], -.07, .03, 'paint.second', 'skin', front=.02)
            if coping:
                k.slab(-L / 2 - (t if s in 'sn' else 0) - .0, L / 2 + (t if s in 'sn' else 0), H - .28, H, -.16, t, 'paint.plate', 'frame',
                       front=.06, walk=True)
            for op in ops:
                kind, xl, zb, w, h = op[:5]
                extra = op[5] if len(op) > 5 else None
                if kind in ('blast', 'bay'):
                    with k.at((0, 0, zb)):
                        blast_door(k, w, h, xc=xl, depth=.55 if kind == 'blast' else .9, back=.05,
                                   fw=(.3 if w <= 1.0 else .42) if kind == 'blast' else .8,
                                   yaw=yaw, label=extra, sill=zb < .1)
                elif kind == 'door':
                    with k.at((0, 0, zb)):
                        ip.personnel_door(k, xl, w, h, 0.0, apron=zb < .1)
                    count(k, 'doors')
                    I.wall_lamp(k, xl, zb + h + .95, yaw)
                elif kind == 'rollup':
                    with k.at((0, 0, zb)):
                        ip.rollup_door(k, xl, w, h, 0.0)
                    count(k, 'doors')
                    I.wall_lamp(k, xl, zb + h + 1.3, yaw)
                elif kind == 'window':
                    ip.steel_window(k, xl - w / 2, xl + w / 2, zb, zb + h, 0.0, rows=1)
                    count(k, 'windows')
                elif kind == 'slit':
                    slit_window(k, xl, zb + h / 2, w, h)
                elif kind == 'vent':
                    xa, xb = xl - w / 2, xl + w / 2
                    k.slab(xa, xb, zb, zb + h, .05, .12, 'grate', 'frame')
                    for j in range(int(h / .2)):
                        k.slab(xa, xb, zb + j * .2 + .03, zb + j * .2 + .12, -.03, .1, 'paint.second', 'frame')
                    k.slab(xa - .12, xb + .12, zb - .12, zb + h + .12, -.08, .04, 'paint.plate', 'frame')
                elif kind == 'stencil':
                    stencil_panel(k, extra, xl, zb, w, -.0)
    for s in skirt_sides:
        if s not in sides or not skirt[0]:
            continue
        o, yaw, L = frame_of(box, s)
        ops = openings.get(s, [])
        _, _, Lf, _ = F[s]
        cuts = sorted((op[1] - op[3] / 2 - (.8 if op[0] == 'bay' else .45 if op[0] == 'blast' else .7), op[1] + op[3] / 2 + (.8 if op[0] == 'bay' else .45 if op[0] == 'blast' else .7))
                      for op in ops if op[0] in DOORS and op[2] < .1)
        segs, a = [], -L / 2
        for c0, c1 in cuts:
            if c0 > a:
                segs.append((a, c0))
            a = max(a, c1)
        if a < L / 2:
            segs.append((a, L / 2))
        with k.at((o[0], o[1], 0), yaw=yaw):
            for a, b in segs:
                wedge(k, a, b, skirt[0], skirt[1], abs(a + L / 2) < 1e-6, abs(b - L / 2) < 1e-6, 'ferro.second')
    if roof:
        I.roof_deck(k, x0 + (t if 'w' in sides else 0), y0 + (t if 's' in sides else 0), x1 - (t if 'e' in sides else 0),
                    y1 - (t if 'n' in sides else 0), H)


def roof_kit(k, pts_vents=(), hatches=(), z=0.0):
    for x, y in pts_vents:
        ip.mushroom_vent(k, x, y, z)
    for x, y in hatches:
        ip.hatch(k, x, y, z)


# ------------------------------------------------------------------ barracks
def barracks(k):
    box = (-5.08, -10.43, 5.08, 10.43)
    ins = batter_mass(k, box, 0.0, 4.05, 1.7, plinth=.5, cap=.3, ch=.45, proud=.16)
    # stepped armour ribs over the roof and down both battered flanks
    for yc in (-7.3, -2.6, 2.6, 7.3):
        prof = [(-5.26, 0.0), (5.26, 0.0), (5.26, .52), (3.56, 3.88), (3.16, 4.22), (-3.16, 4.22), (-3.56, 3.88), (-5.26, .52)]
        k.prism(prof, yc - .3, yc + .3, 'paint.plate', 'frame', axis='y')
        k.prism([(-5.3, 0.0), (5.3, 0.0), (5.3, .58), (-5.3, .58)], yc - .36, yc + .36, 'paint.second', 'frame', axis='y')
    for s in 'ew':
        for xl in (-5.0, 0.0, 5.0):
            with on_face(k, box, s, xl, 2.1, ins):
                k.slab(-1.9, 1.9, -1.1, .95, -.035, .02, 'paint.second', 'skin', front=.015)
                slit_window(k, 0, 0, 1.7, .42)
        with on_face(k, box, s, 8.6 if s == 'e' else -8.6, 1.4, ins):
            PW.placard(k, (0, -.02, 0), -math.pi / 2)
    for s in 'sn':
        o, yaw, _ = frame_of(box, s)
        with k.at((o[0], o[1], 0), yaw=yaw):
            blast_door(k, 1.8, 2.5, depth=.4, back=2.2, fw=.4, yaw=yaw)
            bag_run(k, [(-3.75, -.75), (-1.75, -.75)], 4, wide=0)
            bag_run(k, [(1.75, -.75), (3.75, -.75)], 4, wide=0)
        with on_face(k, box, s, 3.1, 1.9, ins):
            stencil_panel(k, '21' if s == 's' else '22', 0, 0, .5, -.02)
    roof_kit(k, [], [(0, 0), (0, 5.6)], 4.06)
    PW.fin_array(k, -1.4, 1.4, -.4, 4.06, 4.36, depth=.3, s=1)


# ------------------------------------------------------------------ vehiclehangar
def vehiclehangar(k):
    # drive-through hall, blast bay door each end under the stencilled bay number
    mil_hall(k, (-12.5, -7.1, 12.5, 8.0), 11.9, {
        'e': [('bay', 0.0, 0, 6.6, 7.2, '13'), ('slit', -5.2, 8.4, 1.6, .45), ('slit', 5.2, 8.4, 1.6, .45)],
        'w': [('bay', 0.0, 0, 6.6, 7.2, '13'), ('slit', -5.2, 8.4, 1.6, .45), ('slit', 5.2, 8.4, 1.6, .45)],
        'n': [('vent', 3.0, 7.6, 2.4, 1.2)]}, sides='enw', rib=4.2)
    # windowed office lean-to along the front
    mil_hall(k, (-12.5, -13.9, 12.5, -7.1), 7.2, {
        's': [('window', -7.0, 1.3, 7.2, 1.3), ('window', 7.0, 1.3, 7.2, 1.3), ('blast', 0.0, 0, 1.6, 2.5),
              ('stencil', 0.0, 5.3, .7, 0, '13')],
        'e': [('door', 1.8, 0, 1.2, 2.3)], 'w': [('door', 0.0, 0, 1.2, 2.3)]}, sides='sew', skirt_sides='sw', rib=4.6)
    # the hall's front wall above the lean-to (walls never overlap: it starts at the lean-to roof)
    with k.at((0, -7.1, 0)):
        ip.wall_frame(k, -12.5, 12.5, 7.2, 11.9, 0.0, (), mat='ferro.main', t=.5)
        for x in (-8.4, -4.2, 4.2, 8.4):
            k.slab(x - .28, x + .28, 7.2, 11.45, -.26, .02, 'paint.plate', 'frame', front=.08)
        k.slab(-12.5, 12.5, 10.35, 10.9, -.07, .03, 'paint.second', 'skin', front=.02)
        k.slab(-13.0, 13.0, 11.62, 11.9, -.16, .5, 'paint.plate', 'frame', front=.06, walk=True)
        for x in (-6.0, 6.0):
            slit_window(k, x, 8.9, 2.0, .45)
    # rear annexes: plant room (west, taller) and stores (east)
    mil_hall(k, (-11.6, 8.0, 1.6, 14.0), 6.4, {
        'n': [('blast', -2.5, 0, 1.6, 2.5), ('slit', 2.5, 3.6, 1.6, .42)], 'w': [('vent', 0, 2.6, 1.8, 1.4)]}, sides='nwe', rib=4.4)
    mil_hall(k, (1.6, 8.0, 11.6, 14.0), 4.6, {
        'n': [('rollup', 0.5, 0, 3.6, 3.4)], 'e': [('door', 0.0, 0, 1.2, 2.3)]}, sides='ne', rib=4.4, skirt=(1.5, .55))
    # exhaust/plant tower rising through the hall roof, bent exhaust duct onto the annex roof
    mil_hall(k, (-9.9, 3.5, -5.0, 10.6), 14.9, {
        's': [('vent', 0, 12.5, 2.6, 1.2)], 'e': [('vent', -1.4, 12.5, 2.0, 1.2)], 'w': [('vent', 1.0, 12.5, 2.0, 1.2)]},
        sides='nesw', rib=False, skirt=(0, 0))
    k.cyl((-7.45, 5.0, 15.3), .42, .8, 'paint.plate', 'frame', sides=16, top=.04)
    k.cyl((-7.45, 5.0, 15.75), .5, .1, 'paint.trim', 'frame', sides=16)
    ip.pipe(k, [(-5.0, 9.6, 13.3), (-1.4, 9.6, 13.3), (-1.4, 9.6, 6.75)], .55, 'paint.duct', bend=1.0)
    k.box((-1.4, 9.6, 6.55), (1.6, 1.6, .3), 'paint.plate', 'frame', ch=.1)
    ip.hbeam(k, (-3.2, 9.6, 6.4), (-3.2, 9.6, 12.7), .26, .26)
    k.beam((-3.2, 9.6, 12.75), (-3.2, 9.6, 12.76), .3, 'metal', 'frame')
    # roofs: hatches, vents, a mast; ladders lean-to -> hall roof and ground -> lean-to
    roof_kit(k, [(4.0, -2.0), (8.0, -2.0), (4.0, 4.0), (8.0, 4.0)], [(0.0, 1.0), (10.5, -5.5)], 11.9)
    roof_kit(k, [(-6.0, -10.5), (6.0, -10.5)], [], 7.2)
    antenna(k, 10.6, 6.0, 11.9, 3.2)
    ip.ladder(k, 9.2, -7.1, -math.pi / 2, 7.2, 11.9, name='lean-to to hall roof')
    ip.ladder(k, 12.5, -11.4, 0.0, 0.0, 7.2, name='ground to lean-to roof')
    count(k, 'stairs_ladders', 2)
    # wheel stops and hazard aprons at both bay doors
    for s in (-1, 1):
        x = s * 13.0
        k.box((x + s * .2, .0, .03), (1.0, 7.4, .06), 'hazard', 'frame', walk=True)


# ------------------------------------------------------------------ repairbay
MECH = dict(height=16.94, width=13.83, src='stock srcdata/tgl ASE: tallest atlas.ase 16.94 m, widest bloodasp.ase 13.83 m across the arms '
                                              '(35 mech ASEs measured, copters and the slayer aircraft excluded)')


def repairbay(k):
    """Mech repair bay that fits every stock mech (owner: "should fit all mechs"). Tallest stock mech atlas.ase 16.94 m,
    widest bloodasp.ase 13.83 m: open pad 15.7 m clear between the gantries (0.9 m each side) and 12.6 m deep, crane
    runway underside at 19.0 m (2.06 m over the Atlas), gantry platforms at knee 4.4, waist 8.6, shoulder 13.0 m, a
    16 x 19.5 m bay door into an 11 m deep, 22 m tall hall. Stock massing kept: tall brown services block west, low
    wings either side of the open pad."""
    HH = 22.0                                   # hall height (stock 11.3; grown so the bay door clears an Atlas)
    hall = (-10.8, -9.5, 0.2, 12.15)
    yc = (hall[1] + hall[3]) / 2
    mil_hall(k, hall, HH, {
        'e': [('bay', 0.0 - yc, 0, 16.0, 19.5)],
        'w': [('blast', -(-0.5 - yc), 8.6, 1.0, 2.2)],
        'n': [('vent', 0.0, 17.0, 2.4, 1.4), ('slit', -2.2, 3.0, 1.2, .42), ('slit', 2.2, 10.0, 1.2, .42)],
        's': [('slit', -2.2, 8.4, 1.2, .42), ('slit', 2.2, 8.4, 1.2, .42), ('slit', -2.2, 15.0, 1.2, .42), ('slit', 2.2, 15.0, 1.2, .42)]},
        skirt_sides='ns', rib=4.0)
    # owner round 4: one west services block (was three stepped slabs that read as a stair step)
    x0, x1, h = -13.9, -10.75, 18.5
    k.box(((x0 + x1) / 2, 5.05, h / 2), (x1 - x0, 7.9, h), 'ferro.main', 'mass', ch=.08, top=.06)
    k.box(((x0 + x1) / 2, 5.05, h - .14), (x1 - x0 + .3, 8.2, .28), 'paint.plate', 'frame', ch=.06)
    k.box(((x0 + x1) / 2, 5.05, h + .01), (x1 - x0 - .3, 7.6, .04), 'gravel', 'roof', walk=True)
    for y in (1.6, 8.5):
        k.box((x0 - .05, y, h * .5), (.12, .5, h - 1.0), 'paint.plate', 'frame', ch=.03)
    with k.at((-13.9, 5.05, 0), yaw=-math.pi / 2):
        blast_door(k, 1.4, 2.4, depth=.5, back=.1, fw=.36, yaw=-math.pi / 2)
    mil_hall(k, (-3.3, -12.15, 0.2, -9.5), 6.0, {'s': [('blast', 0.0, 0, 1.6, 2.5, None), ('stencil', 0.0, 4.4, .6, 0, '04')]},
             sides='sew', rib=False, skirt=(1.4, .45))
    # workshop wings beyond the gantries
    for wb, s in (((0.2, 9.9, 13.5, 16.5), 's'), ((0.2, -16.5, 13.5, -9.9), 'n')):
        mil_hall(k, wb, 7.0, {'e': [('rollup', 0.0, 0, 3.6, 4.2)]}, sides='ens', skirt=(1.6, .5),
                 skirt_sides='e', rib=3.6)
    # mech pad: 15.7 m clear between the gantries, 12.6 m deep
    x0p, x1p, yp = 0.9, 13.5, 9.6
    k.box(((x0p + x1p) / 2, 0, .08), (x1p - x0p, 2 * yp, .16), 'ferro.second', 'mass', ch=.05, walk=True)
    for y in (-7.75, 7.75):
        k.box(((x0p + x1p) / 2, y, .165), (x1p - x0p - .2, .3, .02), 'hazard', 'detail')
    k.box((x1p - .2, 0, .165), (.3, 15.5, .02), 'hazard', 'detail')
    for y in (-2.4, 2.4):
        k.box((7.2, y, .17), (4.0, 2.8, .02), 'paint.stencil', 'detail')
        k.box((7.2, y, .175), (3.6, 2.4, .02), 'ferro.second', 'detail')
    for x in (3.4, 11.0):
        k.box((x, 0, .165), (.14, 14.0, .02), 'metal', 'frame')
    with k.at((12.0, 0, .17), pitch=-math.pi / 2):
        stencil(k, '04', 0, 0, 1.8, 0.0)
    # service gantry (cradle) each side: knee 4.4, waist 8.6, shoulder 13.0
    gx0, gx1 = 1.75, 12.5
    levels = ((4.4, 2.0, 11.6), (8.6, 2.5, 12.2), (13.0, 2.5, 11.6))
    for sg in (-1, 1):
        ya, yb = sorted((sg * 7.9, sg * 9.45))
        for x in (gx0, gx1):
            for y in (ya + .1, yb - .1):
                ip.hbeam(k, (x, y, .16), (x, y, 13.0 - .25), .3, .3)
            ip.angle_brace(k, (x, ya + .1, .4), (x, yb - .1, 4.0))
            ip.angle_brace(k, (x, ya + .1, 4.6), (x, yb - .1, 8.3))
        ym = (ya + yb) / 2
        gaps = ([(11.6, ym)], [(2.5, ym)], [(11.6, ym)])
        inner = 's' if sg > 0 else 'n'
        for i, (z, xa, xb) in enumerate(levels):
            ip.rect_platform(k, xa, ya, xb, yb, z, rail_sides=inner + 'ew', legs=False, gaps=gaps[i],
                             name=f'gantry {"north" if sg > 0 else "south"} {("knee", "waist", "shoulder")[i]}')
            for x in (xa, xb):
                k.beam((x, ya + .1, z - .2), (x, yb - .1, z - .2), .2, ip.STEEL, 'frame', h=.25)
            for y in (ya + .1, yb - .1):
                k.beam((gx0, y, z - .2), (gx1, y, z - .2), .2, ip.STEEL, 'frame', h=.25)
        for x, z0, z1 in ((11.52, .16, 4.4), (2.58, 4.4, 8.6), (11.52, 8.6, 13.0)):
            ip.hbeam(k, (x, ym, z0), (x, ym, z1 - .3), .14, .14)
        ip.ladder(k, 11.6, ym, 0.0, 0.16, 4.4, name='pad to knee platform')
        ip.ladder(k, 2.5, ym, math.pi, 4.4, 8.6, name='knee to waist platform')
        ip.ladder(k, 11.6, ym, 0.0, 8.6, 13.0, name='waist to shoulder platform')
        count(k, 'stairs_ladders', 3)
        k.box((1.15, sg * 8.6, .9), (.6, 1.4, 1.5), 'paint.second', 'frame', ch=.05)
        for j in range(3):
            k.box((1.15, sg * 8.6, .35 + j * .45), (.62, 1.3, .04), 'paint.trim', 'detail')
    # armour plate racks against the hall facade beside the bay door
    for y in (-9.0, 9.4):
        if y > 0:
            continue
        for j in range(4):
            with k.at((1.2, y - .5 + j * .32, .16), roll=.12):
                k.box((0, 0, 1.3), (.12, .26, 2.6), 'paint.main', 'frame', ch=.02)
        k.box((1.4, y, .3), (.9, 1.6, .28), ip.STEEL, 'frame')
    # overhead crane: runway underside 19.0 m (2.06 m over the tallest stock mech), portal at the pad's east edge
    zr = 19.25
    for y in (-8.6, 8.6):
        ip.hbeam(k, (0.2, y, zr), (14.0, y, zr), .5, .3)
        ip.hbeam(k, (14.0, y, .16), (14.0, y, zr - .25), .5, .5)
        k.box((14.0, y, .3), (1.0, 1.0, .6), 'ferro.main', 'mass', ch=.06)
        ip.angle_brace(k, (14.0, y, 15.0), (14.0, y - (1.8 if y > 0 else -1.8), 18.9))
        k.box((14.0, y, 1.0), (.52, .52, .9), 'hazard', 'detail')
    ip.hbeam(k, (14.0, -8.9, zr), (14.0, 8.9, zr), .5, .35)
    ip.hbeam(k, (7.2, -8.9, zr + .5), (7.2, 8.9, zr + .5), .5, .3)
    for y in (-8.6, 8.6):
        k.box((7.2, y, zr + .4), (1.1, .8, .4), 'paint.second', 'frame', ch=.06)
    k.box((7.2, 0, zr + .05), (1.2, 1.0, .5), 'hazard', 'frame', ch=.08)
    for dy in (-.25, .25):
        ip.tube(k, [(7.2, dy, zr - .2), (7.2, dy * .3, 14.5)], .02, 'metal', 'frame', sides=6)
    k.box((7.2, 0, 14.25), (.5, .35, .55), 'hazard', 'frame', ch=.05)
    for yy in (-9.0, 9.2):
        if abs(yy) > 9.1:
            continue
        with k.at((0.2, yy, 0), yaw=math.pi / 2):
            k.slab(-.5, .5, 2.8, 4.4, -.12, .02, 'paint.trim', 'frame', front=.03)
            for j in range(3):
                PW.status_light(k, (-.3 + .3 * j, -.18, 4.1), 'lamp_cool' if j else 'lamp_red', r=.06)
    # external fire stair to the upper gallery door (west)
    ip.rect_platform(k, -13.75, -9.5, -11.4, -7.7, 4.8, rail_sides='snw', gaps=[(-12.95, -7.7), (-11.85, -7.7)], name='stair landing')
    ip.stair(k, (-12.95, -1.6, 0.0), (-12.95, -7.7, 4.8), name='fire stair lower')
    ip.stair(k, (-11.85, -7.7, 4.8), (-11.85, -1.6, 8.6), w=.8, name='fire stair upper')
    ip.rect_platform(k, -12.5, -1.6, -11.35, .8, 8.6, rail_sides='snw', gaps=[(-11.85, -1.6)], name='gallery landing')
    count(k, 'stairs_ladders', 2)
    roof_kit(k, [(-8.0, 6.0), (-8.0, -6.0)], [(-5.5, 9.5)], HH)
    mech_bay_dressing(k)
    repairbay_moving(k)


def repairbay_moving(k):
    """The stock RepairBayRise animation nodes (anim_parity): LOS_Gantry, a pair of lift work platforms on the pad
    gantries that rise 3.6-5.4 m to the mech's height, carrying the two floodlights Spot_Gantry / Spot_Gantry01 that
    tilt onto the mech; Ramp, a loading ramp plate that slides out of the hall onto the pad. Pivots are the stock node
    pivots in the authored frame."""
    with anim_node(k, 'LOS_Gantry', (5.331, -0.003, -11.895), axis=(0.0, 0.0, 1.0)):
        for sy in (-1, 1):
            y0, y1 = sy * 6.5, sy * 8.75
            k.box((5.33, (y0 + y1) / 2, 2.0), (3.6, abs(y1 - y0), .16), 'grate', 'frame', walk=True)
            k.box((5.33, (y0 + y1) / 2, 1.86), (3.8, abs(y1 - y0) + .1, .14), ip.STEEL, 'frame')
            ip.railing(k, [(3.53, y0), (7.13, y0)], 2.08, h=1.0)
            for x in (3.6, 7.06):            # guide shoes on the gantry legs
                k.box((x, sy * 8.85, 2.4), (.25, .3, 1.0), 'paint.second', 'frame', ch=.03)
            k.box((5.33, sy * 7.45, 4.1), (.16, .16, 4.2), ip.STEEL, 'frame')         # lamp mast inboard of the gantry rail
            k.box((5.33, sy * 7.7, 6.13), (.14, .6, .14), ip.STEEL, 'frame')          # arm out to the lamp head
    for name, y in (('Spot_Gantry', -7.943), ('Spot_Gantry01', 7.997)):
        # authored facing +y for both: the stock clip turns Spot_Gantry01 179 degrees about vertical onto the pad
        with anim_node(k, name, (5.342, y, 6.539), parent='LOS_Gantry', axis=(-1.0, 0.0, 0.0)):
            k.box((5.342, y, 6.539), (.9, .5, .6), 'paint.trim', 'frame', ch=.05)
            k.box((5.342, y + .27, 6.539), (.76, .04, .46), 'lamp', 'frame')
            k.box((5.342, y, 6.2), (.2, .2, .4), ip.STEEL, 'frame')
    with anim_node(k, 'Ramp', (-3.959, -0.003, 0.0), axis=(1.0, 0.0, 0.0)):
        k.prism([(-9.0, 0.0), (-1.3, 0.0), (-1.0, .02), (-1.0, .3), (-9.0, .3)], -3.9, 3.9, 'paint.plate', 'frame', axis='y',
                walk=True)
        for y in (-3.6, 3.6):
            k.box((-5.0, y, .32), (8.0, .2, .06), 'hazard', 'frame')
        for x in ip.frange(-8.6, -1.6, .7):
            k.box((x, 0, .31), (.08, 7.4, .03), ip.STEEL, 'frame')


def mech_bay_dressing(k):
    """Working-bay dressing (orchestrator: the pad read as an empty scaffold): braced gantry towers, floodlights,
    armour plates on A-frame racks, a plate hanging from the crane hook, parts crates and drums, hazard chevron
    borders and guide lines, pad flood masts."""
    gx0, gx1 = 1.75, 12.5
    for sg in (-1, 1):
        yo = sg * 9.35
        for z0, z1 in ((.2, 4.2), (4.6, 8.4), (8.8, 12.8)):
            for xa, xb in ((gx0, (gx0 + gx1) / 2), ((gx0 + gx1) / 2, gx1)):
                k.beam((xa, yo, z0), (xb, yo, z1), .14, ip.STEEL, 'frame', h=.14)
                k.beam((xb, yo, z0), (xa, yo, z1), .14, ip.STEEL, 'frame', h=.14)
        ip.hbeam(k, ((gx0 + gx1) / 2, yo, .16), ((gx0 + gx1) / 2, yo, 12.75), .26, .26)
        k.beam((gx0, yo, 13.07), (gx1, yo, 13.07), .3, ip.STEEL, 'frame', h=.54)      # top chord carrying the floods
        for x in (3.2, 6.0, 8.8, 11.4):
            k.box((x, sg * 9.3, 13.6), (.9, .35, .5), 'paint.trim', 'frame', ch=.05)
            k.box((x, sg * 9.12, 13.55), (.75, .04, .35), 'lamp', 'frame')
        # armour plates on an A-frame rack under the gantry, outside the mech clearance
        for x0 in (3.6, 7.6):
            for xx in (x0, x0 + 2.2):
                k.beam((xx, sg * 8.3, .16), (xx, sg * 9.0, 2.4), .1, ip.STEEL, 'frame')
                k.beam((xx, sg * 9.6, .16), (xx, sg * 9.0, 2.4), .1, ip.STEEL, 'frame')
            k.beam((x0, sg * 9.0, 2.4), (x0 + 2.2, sg * 9.0, 2.4), .1, ip.STEEL, 'frame')
            for j, side in enumerate((-1, 1)):
                with k.at((x0 + 1.1, sg * (9.0 + side * .38), 1.25), roll=side * sg * .32):
                    k.box((0, 0, 0), (2.4, .14, 2.1), 'paint.main', 'frame', ch=.06, top=.03)
                    k.box((0, -.08, .6), (1.6, .02, .3), 'hazard', 'detail')
        # parts crates and coolant drums at the wing ends
        for i in range(3):
            k.box((12.4 - i * 1.1, sg * 7.2, .6), (1.0, 1.0, 1.0), 'wood.crate' if i % 2 else 'paint.second', 'frame', ch=.05)
        for i in range(3):
            k.cyl((9.0 + i * .65, sg * 7.0, .6), .3, .9, 'paint.valve' if i == 1 else 'paint.second', 'frame', sides=14, top=.04)
    # plate hanging from the crane hook
    for dy in (-.6, .6):
        ip.tube(k, [(7.2, 0, 14.0), (7.2, dy, 12.9)], .02, 'metal', 'frame', sides=5)
    k.box((7.2, 0, 11.6), (3.0, .2, 2.6), 'paint.main', 'frame', ch=.08, top=.04)
    k.box((7.2, -.11, 12.4), (2.0, .02, .3), 'hazard', 'detail')
    # hazard chevron border and guide lines on the pad
    for y in (-7.6, 7.6):
        k.box((7.2, y, .17), (12.2, .55, .03), 'hazard', 'frame')
    k.box((13.25, 0, .17), (.55, 15.2, .03), 'hazard', 'frame')
    for x in ip.frange(2.0, 12.6, 1.2):
        k.box((x, 0, .172), (.6, .1, .02), 'paint.stencil', 'detail')
    # pad flood masts at the open corners
    for sy in (-1, 1):
        x, y = 13.6, sy * 7.2
        k.box((x, y, .25), (.6, .6, .5), 'ferro', 'mass', ch=.05)
        k.cyl((x, y, 5.0), .1, 9.5, 'metal', 'frame', sides=8)
        for a in (-.5, .5):
            with k.at((x, y, 9.6), yaw=math.pi + a * sy):
                k.box((0, -.15, 0), (.7, .3, .45), 'paint.trim', 'frame', ch=.04)
                k.box((0, -.31, 0), (.6, .02, .32), 'lamp', 'frame')


# ------------------------------------------------------------------ hangar
def hangar(k):
    R, peak, half = 50.3, 19.5, 23.5
    zc = peak - R
    zr = lambda y: zc + math.sqrt(R * R - y * y)  # noqa: E731
    Hw = zr(half)
    xs_rib = [-23.0 + 46.0 * i / 6 for i in range(7)]
    win = [('window', -23.0 + 46.0 * (i + .5) / 6, 9.9, 5.0, 1.7) for i in range(6)]
    mil_hall(k, (-23.0, -half, 23.0, half), Hw, {
        # owner round 4: ground windows one per bay between the ribs, framed like the upper row (door bay excepted)
        's': win + [('window', -23.0 + 46.0 * (i + .5) / 6, 2.6, 5.0, 1.4) for i in (0, 1, 2, 4, 5)] + [('blast', 3.8, 0, 1.6, 2.6),
                    ('stencil', -3.8, 5.0, .9, 0, '07')],
        'n': [('slit', -23.0 + 46.0 * (i + .5) / 6, 10.4, 2.6, .5) for i in range(6)] + [('blast', -6.0, 0, 1.6, 2.6)]},
        sides='sn', rib=False, skirt=(2.1, .5), roof=False, coping=False)
    # gables: cast walls cut for the doors, arched tympanum above the spring line
    F = I.faces_of(-23.0, -half, 23.0, half, .5)
    for s in 'ew':
        origin, yaw, L, _ = F[s]
        if s == 'e':
            holes = [(-15.0, 0, 15.0, 10.5)]
        else:
            holes = [(-17.6, 0, -12.4, 5.2), (-9.0, 0, -7.8, 2.3)]
        with k.at(origin, yaw=yaw):
            ip.wall_frame(k, -L / 2, L / 2, 0, Hw, 0.0, holes, mat='ferro.main', t=.5)
            arc = [(x, zr(x)) for x in [L / 2 - L * i / 48 for i in range(49)]]
            k.prism([(-L / 2, Hw - .01), (L / 2, Hw - .01)] + arc[1:-1], 0.0, .5, 'ferro.main', 'mass', axis='y')
            k.slab(-L / 2 - .5, L / 2 + .5, Hw - 1.6, Hw - 1.05, -.07, .03, 'paint.second', 'skin', front=.02)
            for x in (-L / 2 + .45, L / 2 - .45):
                k.slab(x - .45, x + .45, 0, Hw + .3, -.4, .02, 'paint.plate', 'frame', front=.1)
            if s == 'e':
                # sliding hangar doors: six staggered leaves on a top track, hazard pilasters, header and number
                for i in range(6):
                    xa = -15.0 + 5.0 * i
                    yl = .12 if i % 2 else -.12
                    k.slab(xa + .03, xa + 4.97, .05, 10.45, yl, yl + .14, 'paint.door', 'skin', door=True, feature='door')
                    for z in (2.6, 5.2, 7.8):
                        k.slab(xa + .2, xa + 4.8, z - .08, z + .08, yl - .07, yl, 'paint.trim', 'frame')
                    k.slab(xa + .5, xa + 4.5, 8.6, 9.4, yl - .01, yl, 'glass', 'glass', pane=True)
                    k.slab(xa + .05, xa + 4.95, .15, .8, yl - .012, yl, 'hazard', 'detail')
                    count(k, 'doors')
                ip.hbeam(k, (-15.8, -.45, 10.75), (15.8, -.45, 10.75), .5, .35)
                k.slab(-16.4, 16.4, 10.5, 11.8, -.9, .02, 'paint.plate', 'frame', front=.12)
                for x in (-15.6, 15.6):
                    k.slab(x - .6, x + .6, 0, 10.5, -.9, .02, 'paint.plate', 'frame', front=.1)
                    k.slab(x - .61, x + .61, .1, 2.4, -.912, -.88, 'hazard', 'detail')
                stencil(k, '07', 0, 13.2, 1.8, -.02)
                k.slab(-16.0, 16.0, 0, .08, -2.4, .0, 'hazard', 'frame', walk=True)
                for x in (-8, 8):
                    I.wall_lamp(k, x, 12.3, yaw)
            else:
                with k.at((0, 0, 0)):
                    ip.rollup_door(k, -15.0, 4.6, 5.0, 0.0)
                    ip.personnel_door(k, -8.4, 1.0, 2.2, 0.0)
                count(k, 'doors', 2)
                I.wall_lamp(k, -15.0, 6.6, yaw)
                I.wall_lamp(k, -8.4, 3.2, yaw)
                # clerestory louvres up in the tympanum
                for xl in (-6.0, 0.0, 6.0):
                    xa, xb, za = xl - 2.0, xl + 2.0, 15.0
                    k.slab(xa - .15, xb + .15, za - .15, za + 2.15, -.12, .04, 'paint.plate', 'frame')
                    for j in range(10):
                        k.slab(xa, xb, za + j * .2 + .03, za + j * .2 + .13, -.2, -.05, 'paint.second', 'frame')
                stencil_panel(k, '07', 6.0, 4.0, .9, 0.0)
    # barrel vault shell on the walls, proud armour ribs, ridge vent, skylights
    ys = [-23.9 + 47.8 * i / 48 for i in range(49)]
    outer = [(y, zr(y) + .02) for y in ys]
    inner = [(y, zr(y) - .38) for y in reversed(ys)]
    sec = outer + inner
    loft(k, [[(x, y, z) for y, z in sec] for x in (-23.35, 23.35)], 'paint.roof', 'roof')
    for x in xs_rib:
        rib = [(y, zr(y) + .32) for y in ys] + [(y, zr(y) - .05) for y in reversed(ys)]
        loft(k, [[(xx, y, z) for y, z in rib] for xx in (x - .32, x + .32)], 'paint.plate', 'frame')
    k.box((0, 0, peak + .3), (44.0, 1.6, .5), 'paint.plate', 'frame', ch=.1, top=.12)
    for x in ip.frange(-21.0, 21.0, 1.0):
        k.box((x, 0, peak + .3), (.08, 1.64, .36), 'grate', 'detail')
    for i in range(6):
        xm = (xs_rib[i] + xs_rib[i + 1]) / 2
        for y in (-9.0, 9.0):
            a = math.asin(y / R)
            with k.at((xm, y, zr(y)), pitch=-a):
                k.box((0, 0, .1), (5.0, 1.8, .22), 'paint.trim', 'frame', ch=.06)
                k.box((0, 0, .2), (4.6, 1.4, .06), 'glass', 'glass', pane=True)
            count(k, 'windows')
    # buttress feet under every rib along the long walls
    for x in xs_rib:
        for s in (-1, 1):
            k.prism([(s * (half + .02), 0), (s * (half + 1.3), 0), (s * (half + .3), Hw - .2), (s * (half + .02), Hw - .2)][::(1 if s > 0 else -1)],
                    x - .4, x + .4, 'paint.plate', 'frame', axis='x')


# ------------------------------------------------------------------ quonset2 (approved quonset + sensor deck)
def quonset2(k):
    import drafts
    drafts.quonset_build(k)
    z = 5.75
    ip.rect_platform(k, 1.2, 1.6, 5.7, 5.2, z, rail_sides='nesw', legs=False, gaps=[(5.7, 3.4)], name='sensor deck')
    for y in (1.7, 5.1):
        ip.hbeam(k, (5.6, y, 0.0), (5.6, y, z - .25), .2, .2)
        k.box((5.6, y, .1), (.5, .5, .2), 'ferro', 'mass', ch=.04)
        for x in (1.3, 3.4):
            zr = .45 + math.sqrt(max(0.0, 4.93 ** 2 - x * x))
            ip.hbeam(k, (x, y, zr), (x, y, z - .25), .18, .18)
            k.box((x, y, zr + .03), (.4, .3, .06), 'metal', 'frame')
        ip.angle_brace(k, (5.6, y, 2.0), (3.4, y, .45 + math.sqrt(4.93 ** 2 - 3.4 ** 2)))
    ip.hbeam(k, (5.62, 3.4, 0.0), (5.62, 3.4, z - .25), .16, .16)
    ip.ladder(k, 5.7, 3.4, 0.0, 0.0, z, name='ground to sensor deck')
    count(k, 'stairs_ladders')
    k.box((2.2, 4.4, z + .55), (1.3, 1.0, 1.1), 'paint.second', 'frame', ch=.08, top=.05)
    PW.status_light(k, (2.2, 3.88, z + .9), 'lamp_cool', r=.06)
    PW.placard(k, (2.95, 3.9, z + .55), -math.pi / 2)
    dish(k, (3.7, 3.2), 1.45, -math.pi / 2 - .5, .55, z + 1.35)
    ip.tube(k, [(2.2, 4.95, z + .4), (2.2, 5.6, z + .2), (1.0, 6.4, 3.0), (.6, 10.0, 2.4)], .05, 'rubber', 'frame', sides=6)


# ================================================================== command group
def poly_ring(poly, z):
    return [(x, y, z) for x, y in poly]


def blast_wall(k, pts, h=1.6, base=1.0, top=.45, mat='ferro.second'):
    """Battered cast blast wall along a plan polyline (trapezoid section, chamfered head band)."""
    for a, b in zip(pts, pts[1:]):
        L = math.hypot(b[0] - a[0], b[1] - a[1])
        yaw = math.atan2(b[1] - a[1], b[0] - a[0])
        with k.at((a[0], a[1], 0), yaw=yaw):
            k.prism([(-base / 2, 0), (base / 2, 0), (top / 2, h), (-top / 2, h)], -base * .3, L + base * .3, mat, 'mass', axis='x')
            k.prism([(-top / 2 - .05, h - .25), (top / 2 + .05, h - .25), (top / 2 + .05, h + .05), (-top / 2 - .05, h + .05)],
                    -base * .3, L + base * .3, 'paint.plate', 'frame', axis='x')
            for x in ip.frange(1.0, L - .5, 2.4):
                k.box((x, -base * .38, .45), (.12, .06, .7), 'hazard', 'detail')


def tower_details(k, c, rf, z_slit, n_faces, phase, yaw_door, door_z=0.0, slits=True):
    """Blast door on the door face, slit band near the head on alternate faces."""
    for i in range(n_faces):
        a = phase + i * math.tau / n_faces
        p = (c[0] + math.cos(a) * rf(z_slit), c[1] + math.sin(a) * rf(z_slit), z_slit)
        if slits and i % 2 == 0:
            with k.at(p, yaw=a + math.pi / 2):
                slit_window(k, 0, 0, 1.5, .45)
    a = yaw_door
    p = (c[0] + math.cos(a) * rf(1.2), c[1] + math.sin(a) * rf(1.2), door_z)
    with k.at(p, yaw=a + math.pi / 2):
        blast_door(k, 1.8, 2.6, depth=.5, back=1.6, fw=.42, yaw=a + math.pi / 2)


def turret_control(k, tower, blocks, walls, extras):
    """Faction turret control: tall armoured command tower (shape per faction) over battered annex blocks."""
    kind, c, r0, r1, H = tower['kind'], tower['c'], tower['r0'], tower['r1'], tower['H']
    rf = lambda z: r0 + (r1 - r0) * min(1.0, max(0.0, (z - .6) / (H - .6)))  # noqa: E731
    if kind == 'oct':
        n, ph = 8, math.pi / 8
    elif kind == 'sq':
        n, ph = 4, math.pi / 4
    else:
        n, ph = 12, math.pi / 12
    shape = lambda r, z: [(c[0] + r / math.cos(math.pi / n) * math.cos(ph + i * math.tau / n),  # noqa: E731
                           c[1] + r / math.cos(math.pi / n) * math.sin(ph + i * math.tau / n), z) for i in range(n)]
    if kind == 'sq':
        shape = lambda r, z: [(x, y, z) for x, y in geo.rect_poly(c[0] - r, c[1] - r * tower.get('ar', 1.0), c[0] + r,  # noqa: E731
                                                                   c[1] + r * tower.get('ar', 1.0), r * .18)]
    loft(k, [shape(r0 + .25, 0.0), shape(r0 + .25, .55), shape(r0, .6), shape(rf(H - .5), H - .5), shape(rf(H - .5) - .4, H)],
         'paint.main', 'mass')
    loft(k, [shape(r0 + .06, .55), shape(rf(2.8) + .06, 2.8), shape(rf(2.8) + .02, 2.95)], 'paint.second', 'skin')
    for zb in tower.get('collars', ()):
        loft(k, [shape(rf(zb) + .12, zb), shape(rf(zb + .5) + .12, zb + .5)], 'paint.plate', 'frame')
    # proud corner ribs (vertex ribs) for the faceted towers
    if kind in ('oct', 'rnd'):
        for v0, v1 in zip(shape(rf(3.0) + .1, 3.0), shape(rf(H - 1.0) + .1, H - 1.0)):
            k.beam(v0, v1, .3, 'paint.plate', 'frame', h=.22)
    rf_face = lambda z: rf(z) * (1.0 if kind != 'sq' else 1.0)  # noqa: E731
    if kind == 'sq':
        for s, (dx, dy) in enumerate(((0, -1), (1, 0), (0, 1), (-1, 0))):
            rr = rf(H - 2.6) * (tower.get('ar', 1.0) if dy else 1.0)
            with k.at((c[0] + dx * rr, c[1] + dy * rr, H - 2.6), yaw=math.atan2(dy, dx) + math.pi / 2):
                slit_window(k, -1.4, 0, 1.3, .45)
                slit_window(k, 1.4, 0, 1.3, .45)
        rd = r0 * tower.get('ar', 1.0)
        with k.at((c[0], c[1] - rd, 0), yaw=0.0):
            blast_door(k, 1.8, 2.6, depth=.5, back=1.6, fw=.42, yaw=0.0)
    else:
        tower_details(k, c, rf_face, H - 2.4, n, 0.0 if kind == 'oct' else 0.0, -math.pi / 2)
    # vertical plate strips on every face between base band and slit band (the stock panel rhythm)
    for i in range(n):
        a0, a1 = ph + i * math.tau / n, ph + (i + 1) * math.tau / n
        am = (a0 + a1) / 2
        for zz in ((3.1, H - 3.4),):
            for f in (-.28, .28) if kind != 'rnd' else (0.0,):
                def fp(z, f=f):
                    r = (rf(z) if kind != 'sq' else rf(z) * (tower.get('ar', 1.0) if abs(math.sin(am)) > .7 else 1.0)) + .03
                    half = r * math.tan(math.pi / n)
                    return (c[0] + math.cos(am) * r - math.sin(am) * f * half * 2, c[1] + math.sin(am) * r + math.cos(am) * f * half * 2, z)
                k.beam(fp(zz[0]), fp(zz[1]), .34 if kind != 'rnd' else .26, 'paint.plate', 'frame', h=.06)
    # head: cupola / crown / spire
    hk = tower['head']
    if hk == 'cupola':
        loft(k, [shape(2.7, H), shape(2.7, H + .5), shape(2.5, H + .6), shape(2.5, H + 1.6), shape(2.9, H + 1.7), shape(2.9, H + 1.95),
                 shape(2.4, H + 2.2)], 'paint.second', 'mass')
        for i in range(n):
            a = ph + (i + .5) * math.tau / n
            p = (c[0] + math.cos(a) * 2.5, c[1] + math.sin(a) * 2.5, H + 1.1)
            with k.at(p, yaw=a + math.pi / 2):
                k.slab(-.75, .75, -.32, .32, -.04, .02, 'glass', 'glass', pane=True)
                k.slab(-.85, .85, -.4, -.32, -.1, .02, 'paint.trim', 'frame')
        count(k, 'windows', n)
        rp = tower.get('radar', (c[0], c[1]))
        radar_dipole(k, tower.get('radar_node', 'LOS_Radar'), (rp[0], rp[1], H + 2.5), L=3.6)
    elif hk == 'crown':
        rr = rf(H) + .5
        ar = tower.get('ar', 1.0)
        k.box((c[0], c[1], H + .6), (2 * rr, 2 * rr * ar, 1.2), 'paint.second', 'mass', ch=.5, top=.25)
        for s in (-1, 1):
            with k.at((c[0], c[1] + s * rr * ar, H + .6), yaw=0.0 if s < 0 else math.pi):
                k.slab(-rr + .6, rr - .6, -.22, .22, -.04, .02, 'glass', 'glass', pane=True)
            count(k, 'windows')
        rp = tower.get('radar', (c[0], c[1]))
        radar_dipole(k, tower.get('radar_node', 'Spinner'), (rp[0], rp[1], H + 1.5), L=5.2)
    else:
        loft(k, [shape(rf(H) + .2, H), shape(rf(H) + .2, H + .9), shape(rf(H) - .1, H + 1.1)], 'paint.second', 'mass')
        for i in range(0, n, 2):
            a = ph + (i + .5) * math.tau / n
            p = (c[0] + math.cos(a) * (rf(H) + .2), c[1] + math.sin(a) * (rf(H) + .2), H + .45)
            with k.at(p, yaw=a + math.pi / 2):
                k.slab(-.5, .5, -.2, .2, -.03, .02, 'glass', 'glass', pane=True)
            count(k, 'windows')
        loft(k, [shape(rf(H) - .3, H + 1.1), shape(rf(H) * .45, H + 1.9), shape(.32, H + 3.3)], 'paint.main', 'frame')
        for i in range(0, n, 3):
            a = ph + i * math.tau / n
            k.beam((c[0] + math.cos(a) * (rf(H) - .2), c[1] + math.sin(a) * (rf(H) - .2), H + 1.15),
                   (c[0] + math.cos(a) * .35, c[1] + math.sin(a) * .35, H + 3.25), .16, 'paint.plate', 'frame', h=.16)
        spin_antenna(k, tower.get('radar_node', 'LOS_ant3'), (c[0], c[1], H + 3.3), h=.8, arm=.55)
    PW.placard(k, (c[0] + .9, c[1] - rf(2.0) - .03, 2.0), -math.pi / 2)
    # annex blocks
    for b in blocks:
        if b['kind'] == 'hall':
            mil_hall(k, b['box'], b['H'], b.get('ops', {}), sides=b.get('sides', 'nesw'), skirt=b.get('skirt', (1.4, .5)),
                     skirt_sides=b.get('skirt_sides'), rib=b.get('rib', 3.6), mat='paint.main')
        else:
            ins = batter_mass(k, b['box'], b.get('z0', 0.0), b['H'], b.get('bat', 1.0), plinth=b.get('plinth', .55), cap=.35,
                              ch=b.get('ch', .5), mat='paint.main', plinth_mat='paint.second')
            for s, xl, z in b.get('slits', ()):
                with on_face(k, b['box'], s, xl, z, ins):
                    slit_window(k, 0, 0, 1.4, .42)
            for s, xl in b.get('doors', ()):
                o, yaw, _ = frame_of(b['box'], s)
                tx, ty = math.cos(yaw), math.sin(yaw)
                with k.at((o[0] + tx * xl, o[1] + ty * xl, b.get('z0', 0.0)), yaw=yaw):
                    blast_door(k, 1.6, 2.5, depth=.45, back=b.get('bat', 1.0) * 1.2 + .6, fw=.4, yaw=yaw)
            if b.get('stencil'):
                s, xl, z, txt = b['stencil']
                with on_face(k, b['box'], s, xl, z, ins):
                    stencil_panel(k, txt, 0, 0, .55, -.02)
    for w in walls:
        blast_wall(k, w)
    for e in extras:
        e(k)


def _mast_spin(k, name, xy, z0, z1):
    """Static guyed pole on a roof with a rotating antenna head (stock animated node) at its top."""
    x, y = xy
    k.box((x, y, z0 + .12), (.6, .6, .24), 'paint.plate', 'frame', ch=.06)
    k.cyl((x, y, (z0 + z1) / 2), .09, z1 - z0, 'metal', 'frame', sides=8)
    for a in (0.3, 2.4, 4.5):
        ip.tube(k, [(x, y, z0 + (z1 - z0) * .7), (x + math.cos(a) * 1.6, y + math.sin(a) * 1.6, z0 + .02)], .01, 'metal', 'frame', sides=4,
                caps=False)
    spin_antenna(k, name, (x, y, z1), h=1.0, arm=.8, anim=False)


def _spin_dish(k, name, xy, r, z):
    with _null0():      # stock 'small Dish' has no animation track: static
        dish(k, xy, r, -math.pi / 2, .8, z)


def _ladder_roof(k, x, y, face, z1, name='ground to annex roof'):
    ip.ladder(k, x, y, face, 0.0, z1, name=name)
    count(k, 'stairs_ladders')


def s_turretcontrol(k):
    turret_control(k, dict(kind='oct', c=(-11.84, 7.0), r0=3.9, r1=3.4, H=16.9, head='cupola', collars=(6.0, 11.0), radar=(-11.84, 7.76)), [
        dict(kind='pod', box=(-6.2, -12.4, 8.4, -3.8), H=7.4, bat=1.3, ch=2.6, slits=[('s', -3.5, 4.4), ('s', 3.5, 4.4), ('e', 0, 4.4)],
             doors=[('s', 0.0)], stencil=('w', 0.0, 3.6, '31')),
        dict(kind='pod', box=(6.2, 0.4, 15.6, 12.4), H=7.9, bat=1.3, ch=2.6, slits=[('e', -2.5, 4.6), ('e', 2.5, 4.6), ('n', 0, 4.6)],
             doors=[('e', 0.0)]),
        dict(kind='hall', box=(-7.8, -3.8, 6.2, 9.8), H=5.0, sides='nesw', skirt_sides='e', rib=3.4,
             ops={'n': [('blast', -3.5, 0, 1.6, 2.5), ('slit', 3.2, 2.6, 1.4, .42)]})],
        [[(-15.6, 1.4), (-15.6, -10.6), (-12.6, -12.4), (-8.6, -12.4)]],
        [lambda k: _ladder_roof(k, -2.0, 9.8, math.pi / 2, 5.0),
         lambda k: _mast_spin(k, 'LOS_Antenae', (4.2, 1.7), 5.0, 12.6),
         lambda k: k.cyl((11.0, 6.4, 7.56), .3, 1.5, 'paint.plate', 'frame', sides=12),       # dish pedestal (round 6 check)
         lambda k: dish(k, (11.0, 6.4), 1.2, -math.pi / 2, .7, 9.0),
         lambda k: PW.cable_tray(k, [(-7.8, 2.0), (-2.0, 2.0)], 5.1),
         lambda k: roof_kit(k, [(-4.0, 6.0), (-4.0, 0.0)], [(2.0, 2.0)], 5.0)])


def d_turretcontrol(k):
    turret_control(k, dict(kind='sq', c=(-10.3, 3.1), r0=4.7, r1=3.4, ar=.97, H=16.7, head='crown', collars=(7.0, 12.0), radar=(-9.81, 4.48)), [
        dict(kind='hall', box=(-3.75, -12.4, 11.25, 6.2), H=4.8, sides='nesw', skirt_sides='sen', rib=3.6,
             ops={'s': [('blast', -3.0, 0, 1.6, 2.5), ('slit', 2.6, 2.5, 1.4, .42), ('stencil', -3.0, 3.9, .5, 0, '42')],
                  'e': [('slit', -4.0, 2.5, 1.4, .42), ('slit', -7.6, 2.5, 1.4, .42)]}),
        dict(kind='pod', box=(1.9, -3.1, 9.4, 6.2), z0=4.8, H=7.1, bat=.6, ch=1.0, plinth=.3, slits=[('s', -2.0, 6.0), ('s', 2.0, 6.0)]),
        dict(kind='hall', box=(-3.75, 6.2, 3.75, 12.4), H=4.1, sides='new', skirt_sides='ne', rib=3.6,
             ops={'n': [('blast', 0.0, 0, 1.6, 2.5)]}),
        dict(kind='pod', box=(11.25, -1.5, 15.0, 6.2), H=3.6, bat=.6, ch=.9, slits=[('e', 0.0, 2.0)]),
        dict(kind='pod', box=(-11.2, -9.0, -7.6, -5.4), H=3.0, bat=.5, ch=.9, doors=[('e', 0.0)])],
        [],
        [lambda k: _ladder_roof(k, -3.75, -9.0, math.pi, 4.8),
         lambda k: k.cyl((4.53, 1.47, 6.56), .24, 1.5, 'paint.plate', 'frame', sides=12),      # dish pedestal (round 6 check)
         lambda k: _spin_dish(k, 'small Dish', (4.53, 1.47), 1.0, 8.0),
         lambda k: antenna(k, -9.4, -7.2, 3.0, 3.0),
         lambda k: PW.cable_tray(k, [(-5.6, 0.0), (-3.75, 0.0)], 4.2),
         lambda k: roof_kit(k, [(-1.5, -9.5), (6.0, -9.5)], [(-1.0, 2.0)], 4.8)])


def l_turretcontrol(k):
    turret_control(k, dict(kind='rnd', c=(-10.61, 6.05), r0=4.4, r1=3.6, H=15.4, head='spire', collars=(6.0, 10.5)), [
        dict(kind='hall', box=(-3.8, -11.9, 9.5, 4.4), H=5.1, sides='nesw', skirt_sides='sw', rib=3.6,
             ops={'s': [('blast', 2.85, 0, 1.6, 2.5), ('slit', -2.0, 2.5, 1.4, .42), ('slit', 5.0, 2.5, 1.4, .42)],
                  'w': [('slit', 4.0, 2.5, 1.4, .42)]}),
        dict(kind='pod', box=(-3.6, -10.4, 9.3, -4.5), z0=5.1, H=7.4, bat=.7, ch=1.2, plinth=.3,
             slits=[('s', -3.0, 6.4), ('s', 3.0, 6.4)], stencil=('s', 0.0, 6.4, '17')),
        dict(kind='pod', box=(1.0, 0.75, 13.3, 6.7), H=7.4, bat=1.0, ch=1.2, slits=[('n', -3.0, 4.2), ('n', 3.0, 4.2)],
             doors=[('n', 0.0)]),
        dict(kind='pod', box=(12.6, 1.4, 15.2, 6.0), H=5.2, bat=.6, ch=1.0)],
        [[(-14.9, -2.8), (-14.9, -9.4), (-12.0, -11.6), (-9.2, -11.6)]],
        [lambda k: _ladder_roof(k, 9.5, -2.2, 0.0, 5.1),
         lambda k: antenna(k, 6.0, 3.2, 7.4, 2.4),
         lambda k: k.cyl((5.0, -1.4, 4.76), .26, 1.5, 'paint.plate', 'frame', sides=12),       # dish pedestal (round 6 check)
         lambda k: dish(k, (5.0, -1.4), .9, -math.pi / 2, .7, 6.2),
         lambda k: spin_antenna(k, 'LOS_ant1', (-0.93, -1.26, 5.1), h=.7, arm=.22, anim=False),
         lambda k: spin_antenna(k, 'LOS_ant2', (-1.48, -1.26, 5.1), h=.7, arm=.22, anim=False),
         lambda k: roof_kit(k, [(2.0, -3.0), (6.5, -2.6)], [(1.0, 2.6)], 5.1)])


# ------------------------------------------------------------------ relaystation
def mast_lattice(k, c, w0, w1, z0, z1, panel=2.2):
    """Four-leg tapered lattice mast: corner legs, X bracing and ring struts per panel."""
    zs = ip.frange(z0, z1, panel)
    if zs[-1] < z1 - .3:
        zs.append(z1)
    hw = lambda z: (w0 + (w1 - w0) * (z - z0) / (z1 - z0)) / 2  # noqa: E731
    P = lambda z: [(c[0] - hw(z), c[1] - hw(z), z), (c[0] + hw(z), c[1] - hw(z), z), (c[0] + hw(z), c[1] + hw(z), z),  # noqa: E731
                   (c[0] - hw(z), c[1] + hw(z), z)]
    for a, b in zip(P(z0), P(z1)):
        ip.hbeam(k, a, b, .2, .2)
    for za, zb in zip(zs, zs[1:]):
        A, B = P(za), P(zb)
        for i in range(4):
            j = (i + 1) % 4
            ip.angle_brace(k, B[i], B[j])
            ip.angle_brace(k, A[i], B[j], s=.06)
            ip.angle_brace(k, A[j], B[i], s=.06)


def sector_panel(k, p, yaw, w=.9, h=2.1, tilt=.06):
    with k.at(p, yaw=yaw, pitch=-tilt):
        k.box((0, -.18, 0), (w, .22, h), 'paint.second', 'frame', ch=.06, top=.1, bot=.1)
        k.box((0, -.3, 0), (w * .8, .04, h * .9), 'paint.plate', 'detail')
        k.beam((0, -.05, h * .35), (0, .55, h * .35), .07, 'metal', 'frame')
        k.beam((0, -.05, -h * .35), (0, .55, -h * .35), .07, 'metal', 'frame')
    count(k, 'antennas_dishes')


def relaystation(k):
    import intent_set3 as S
    x0, x1, y0, y1 = -5.0, 5.0, -3.1, 3.1
    top = lambda x, y: 4.6 + (6.05 - 4.6) * min(1.0, max(0.0, (5.0 - abs(x)) / 2.8))  # noqa: E731
    k.box((0, 0, .1), (10.4, 6.6, .2), 'ferro.second', 'mass', ch=.06, walk=True)
    S.shed(k, x0, y0, x1, y1, 6.0, sides='nesw', zb=.2, top=top, bay=2.4, openings={
        's': [('louvre', 1.8, 2.3, 4.0, 1.0), ('door', -3.6, .2, 1.0, 2.2)], 'n': [('louvre', 0.0, 2.3, 6.0, 1.0)],
        'e': [('door2', 0.0, .2, 2.0, 2.6)], 'w': [('louvre', 0.0, 2.4, 3.0, .8)]})
    for x in (x0, x1):
        for y in (y0, y1):
            k.box((x, y, 2.4), (.6, .6, 4.4), 'ferro.main', 'mass', ch=.08, top=.06)
    prof = [(-5.3, 4.42), (5.3, 4.42), (5.3, 4.64), (2.2, 6.22), (-2.2, 6.22), (-5.3, 4.64)]
    k.prism(prof, -3.32, 3.32, 'paint.roof', 'roof', axis='y')
    k.box((0, 0, 6.26), (4.3, 6.6, .1), 'grate', 'frame', walk=True)
    # tapered lattice mast standing on the roof deck, service platform inside the mast at 13 m
    mast_lattice(k, (0, 0), 3.0, 3.0, 6.31, 13.0)
    mast_lattice(k, (0, 0), 3.0, 3.0, 13.0, 15.2)
    mast_lattice(k, (0, 0), 3.0, 1.1, 15.2, 19.4)
    pid = ip._platform(k, 'mast platform')
    k.box((0, 0, 12.96), (2.7, 2.7, .08), 'grate', 'frame', walk=True, platform=pid)
    ip.railing(k, [(-1.25, -1.25), (1.25, -1.25), (1.25, 1.25), (-1.25, 1.25)], 13.0, closed=True, gaps=[(1.25, .6)])
    ip.hbeam(k, (1.5, .6, 6.31), (1.5, .6, 13.0), .14, .14)
    ip.ladder(k, 1.58, .6, 0.0, 6.31, 13.0, name='roof to mast platform')
    ip.ladder(k, -1.2, y0 - .12, -math.pi / 2, 0.0, 6.31, name='ground to roof deck')
    count(k, 'stairs_ladders', 2)
    # sector antennas around the head, two link dishes, beacon
    for i, (z, a) in enumerate(((18.2, 0.3), (17.6, 2.2), (16.4, 4.0), (19.0, 5.3), (15.4, 1.2))):
        r = .55 + (19.4 - z) * .08 + .35
        sector_panel(k, (math.cos(a) * r, math.sin(a) * r, z), a + math.pi / 2)
    dish(k, (1.95, -.9), .7, -math.pi / 2 - .4, 1.2, 15.8)
    dish(k, (-1.85, 1.0), .6, math.pi / 2 + .5, 1.3, 16.7)
    k.cyl((0, 0, 19.9), .12, 1.0, 'metal', 'frame', sides=8)
    k.cyl((0, 0, 20.5), .18, .16, 'lamp_red', 'frame', sides=8)
    PW.cable_tray(k, [(.9, -.9), (.9, -2.0)], 6.5)


# ------------------------------------------------------------------ sensortower
def sensortower(k):
    # buttressed cast footing with steps cut in on the south side
    batter_mass(k, (-2.9, -3.2, 2.9, 3.2), 0.0, 2.0, .9, plinth=.4, cap=.25, ch=.4, mat='ferro.main', plinth_mat='ferro.second')
    for sx in (-1, 1):
        k.prism([(1.2, 0), (3.0, 0), (3.0, .45), (1.2, 5.0)][::1] if sx > 0 else [(-3.0, 0), (-1.2, 0), (-1.2, 5.0), (-3.0, .45)],
                -.18, .18, 'paint.plate', 'frame', axis='y')
        k.prism([(.95, 0), (3.3, 0), (3.3, .45), (.95, 4.4)], sx * 1.0 - .17, sx * 1.0 + .17, 'paint.plate', 'frame', axis='x')
        k.prism([(-3.3, 0), (-.95, 0), (-.95, 4.4), (-3.3, .45)], sx * 1.0 - .17, sx * 1.0 + .17, 'paint.plate', 'frame', axis='x')
    for i in range(9):
        z = .22 * (i + 1)
        k.box((0, -3.45 + .2 * i, z / 2), (1.3, .24, z), 'ferro.second', 'mass', walk=True)
    for sx in (-1, 1):
        k.prism([(-3.6, 0), (-1.6, 0), (-1.6, 2.1), (-3.6, .4)], sx * .65 - .08, sx * .65 + .08, 'paint.plate', 'frame', axis='x')
    # ribbed pylon: vertical south face (ladder), tapering in x
    z0, z1 = 2.0, 19.6
    rings = [rring(-1.3 + .55 * f, -1.0, 1.3 - .55 * f, 1.0, z0 + (z1 - z0) * f, .35) for f in (0.0, 1.0)]
    loft(k, rings, 'paint.main', 'mass')
    for zb in ip.frange(4.4, 18.0, 3.4):
        f = (zb - z0) / (z1 - z0)
        loft(k, [rring(-1.38 + .55 * f, -1.08, 1.38 - .55 * f, 1.08, zb, .38), rring(-1.38 + .55 * f, -1.08, 1.38 - .55 * f, 1.08, zb + .45, .38)],
             'paint.second', 'frame')
    for sx in (-1, 1):
        k.beam((sx * 1.25, 0, 2.2), (sx * .7, 0, 19.4), .22, 'paint.plate', 'frame', h=.5)
    PW.placard(k, (.6, -1.03, 3.2), -math.pi / 2)
    # head platform with a ladder hatch; the ladder runs the full south face
    pid = ip._platform(k, 'sensor head platform')
    zd = 19.8
    for (a, b, c_, d) in ((-1.9, -2.1, 1.9, -1.7), (-1.9, -1.0, 1.9, 1.7), (-1.9, -1.7, -.52, -1.0), (.52, -1.7, 1.9, -1.0)):
        k.box(((a + c_) / 2, (b + d) / 2, zd - .04), (c_ - a, d - b, .08), 'grate', 'frame', walk=True, platform=pid)
    for a, b in (((-1.9, -2.1), (1.9, -2.1)), ((1.9, -2.1), (1.9, 1.7)), ((1.9, 1.7), (-1.9, 1.7)), ((-1.9, 1.7), (-1.9, -2.1))):
        k.beam((a[0], a[1], zd - .16), (b[0], b[1], zd - .16), .1, ip.STEEL, 'frame', h=.18)
    for sx in (-1, 1):
        for sy in (-1, 1):
            ip.angle_brace(k, (sx * .78, sy * .9, 18.0), (sx * 1.8, sy * 1.6 if sy > 0 else -2.0, zd - .2))
    ip.railing(k, [(-1.9, -2.1), (1.9, -2.1), (1.9, 1.7), (-1.9, 1.7)], zd, closed=True)
    ip.ladder(k, 0.0, -1.0, -math.pi / 2, 2.0, zd, name='footing to sensor head')
    count(k, 'stairs_ladders')
    # sensor head = stock animated nodes: Sensorpivot (slews about the vertical axis through the stock pivot
    # (0.09, 0.08)) carrying Sensordish (the twin arrays, their own child node). A slim column keeps the ladder
    # landing clear while the head turns.
    px, py_ = .09, .08
    with anim_node(k, 'Sensorpivot', (px, py_, zd)):
        k.cyl((px, py_, zd + .7), .3, 1.4, 'paint.plate', 'frame', sides=14)
        k.cyl((px, py_, zd + 1.5), .95, .22, 'paint.trim', 'frame', sides=20, top=.05)
        k.box((px, py_ + .2, zd + 2.05), (1.8, 1.1, .9), 'paint.second', 'mass', ch=.25, top=.15)
        PW.status_light(k, (px + .6, py_ - .37, zd + 2.2), 'lamp_cool', r=.06)
        k.cyl((px, py_ + .2, zd + 3.0), .1, 1.0, 'metal', 'frame', sides=8)
        k.cyl((px, py_ + .2, zd + 3.55), .15, .12, 'lamp_red', 'frame', sides=8)
        k.beam((px, py_ - .3, zd + 2.2), (px, -1.5, zd + 2.3), .3, 'paint.plate', 'frame')
    with anim_node(k, 'Sensordish', (px, -1.5, zd + 2.3), parent='Sensorpivot', axis=(1.0, 0.0, 0.0)):
        k.box((px, -1.5, zd + 2.3), (6.4, .3, .3), 'paint.plate', 'frame', ch=.06)
        for s in (-1, 1):
            with k.at((px + s * 2.45, -2.0, zd + 2.6), yaw=s * .45, pitch=-.2):
                k.box((0, 0, 0), (3.3, .26, 1.6), 'paint.second', 'frame', ch=.08)
                k.box((0, -.15, 0), (3.1, .04, 1.4), 'paint.trim', 'frame')
                for i in range(5):
                    k.box((-1.2 + i * .6, -.18, 0), (.5, .02, 1.26), 'paint.plate', 'detail')
                k.box((0, -.16, -.65), (2.9, .03, .06), 'lamp_cool', 'frame')
            count(k, 'antennas_dishes')

# ================================================================== walls and gates (batch 2): built on the approved wall code
# Gates and posts reuse batch3.wall (the approved s/l/d light/medium/heavy walls): the same cast runs, battered
# profile, coping and staged terminal pier, in the same wall palettes. Gate towers are taller piers; the opening
# carries two armoured sliding leaves on a ground track with a faction roundel, hazard foot band and lamps.
GATE_LAYOUT = {'s': (7.5, 12.0, 19.5), 'l': (7.5, 15.0, 21.0), 'd': (6.0, 15.0, 19.5)}
# name: (length, h_end, h_run, h_tower, h_leaf, w_end, w_tower, w_leaf)  (from the stock gate height/width profile)
GATES = {
    's_lightgate': (69.2, 5.9, 4.0, 5.9, 5.0, 5.0, 6.0, 2.6),
    's_mediumgate': (71.5, 14.0, 12.0, 15.7, 13.0, 7.5, 9.8, 3.0),
    's_heavygate': (71.5, 22.2, 21.5, 27.4, 22.0, 7.5, 9.8, 3.0),
    'l_lightgate': (69.4, 5.8, 5.0, 7.8, 4.5, 5.2, 4.8, 3.0),
    'l_mediumgate': (73.0, 14.5, 12.0, 14.0, 11.0, 9.0, 6.4, 4.4),
    'l_heavygate': (73.0, 22.3, 21.0, 22.6, 20.0, 9.4, 6.4, 4.4),
    'd_lightgate': (69.7, 5.6, 5.0, 7.1, 5.0, 5.7, 6.4, 3.0),
    'd_mediumgate': (70.9, 14.4, 12.0, 14.5, 12.0, 6.85, 8.0, 3.6),
    'd_heavygate': (70.9, 22.3, 21.0, 22.7, 21.0, 6.9, 6.9, 3.6),
}
POSTS = {'s_lightpost': (5.0, 5.6), 's_mediumpost': (7.5, 14.0), 's_heavypost': (7.5, 22.2), 'l_lightpost': (5.2, 5.8),
         'l_mediumpost': (6.9, 14.5), 'l_heavypost': (9.2, 22.3), 'd_lightpost': (5.7, 5.6), 'd_mediumpost': (6.85, 14.4),
         'd_heavypost': (6.9, 22.3)}


def _variant(name):
    return 'heavy' if 'heavy' in name else 'light' if 'light' in name else 'medium'


def _wall_call(k, size, dims):
    """batch3.wall on a temporary size (the approved wall code keys its sizes by asset name)."""
    import batch3
    import drafts
    name = k.name
    fam, var = getattr(k, 'wall_family', None) or (name[0], _variant(name))   # generic gates name their wall family
    drafts.WALL_PROFILE[name] = drafts.WALL_PROFILE[f'{fam}_{var}wall']
    saved = batch3.SIZES.get(name), drafts.SIZES.get(name)
    batch3.SIZES[name] = drafts.SIZES[name] = size
    try:
        batch3.wall(k, dict(dims=dict(dims, family=fam, variant=var)), tag=False)
    finally:
        for reg, old in ((batch3.SIZES, saved[0]), (drafts.SIZES, saved[1])):
            if old is None:
                reg.pop(name, None)
            else:
                reg[name] = old
    k.meta.pop('wall_layout', None)
    k.hits = []


def pier(k, y, w, h, d=None):
    """Approved cast pier (wall post code) w square; with d, stretched or shortened along the run to d (stock span)."""
    n0 = len(k.pieces)
    with k.at((0, y, 0)):
        _wall_call(k, (w, w, h), dict(post=True))
    if d is not None and abs(d - w) > 1e-6:
        ys = [v[1] for pc in k.pieces[n0:] for v in pc['v']]
        c = (min(ys) + max(ys)) / 2
        L = max(ys) - min(ys)
        f = d / L
        for pc in k.pieces[n0:]:
            pc['v'] = [(v[0], y + (v[1] - c) * f, v[2]) for v in pc['v']]


def beacon(k, x, y, z, r=.16):
    k.cyl((x, y, z + .1), r * 1.6, .2, 'paint.trim', 'frame', sides=12, top=.04)
    k.cyl((x, y, z + .32), r, .26, 'lamp_red', 'frame', sides=10, top=.04)


def house_emblem(k, house, r, depth=.16):
    """House emblem plate in the current local frame (plate in local xy, local y up, facing +z): a framed octagonal
    plate with a coloured ring and the raised House device (owner: the gates carry the house logo).
      s: the Lyran fist (clenched gauntlet with knuckle ridges, thumb and cuff)
      d: the Federated Suns sunburst (rayed sun pierced by an upright sword)
      l: the Capellan emblem (a triangle field with an upright sword)"""
    oct_ = [(r * math.cos(math.pi / 8 + i * math.pi / 4), r * math.sin(math.pi / 8 + i * math.pi / 4)) for i in range(8)]
    k.prism(oct_, -depth, .02, 'paint.trim', 'frame')                                     # thin frame, flush on the face
    k.prism([(x * .9, y * .9) for x, y in oct_], .02, .06, 'paint.emblem_ring', 'frame')
    k.prism([(x * .8, y * .8) for x, y in oct_], .06, .08, 'paint.emblem_field', 'frame')
    z0, z1 = .08, .16
    if house == 's':
        k.box((0, r * .12, (z0 + z1) / 2), (r * .82, r * .62, z1 - z0), 'paint.emblem', 'frame', ch=r * .06)   # fist
        for i in range(4):                                                                                    # knuckles
            x = -r * .3 + i * r * .2
            k.box((x, r * .38, z1 + .02), (r * .17, r * .14, .05), 'paint.emblem', 'frame', ch=r * .03)
            k.box((x, r * .12, z1 + .01), (r * .02, r * .44, .03), 'paint.emblem_field', 'detail')
        k.box((-r * .48, -r * .02, (z0 + z1) / 2 + .02), (r * .2, r * .42, z1 - z0 + .04), 'paint.emblem', 'frame', ch=r * .05)  # thumb
        k.box((r * .02, -r * .4, (z0 + z1) / 2), (r * .56, r * .3, z1 - z0), 'paint.emblem', 'frame', ch=r * .03)            # cuff
        k.box((r * .02, -r * .58, (z0 + z1) / 2), (r * .62, r * .07, z1 - z0 + .02), 'paint.emblem_ring', 'frame')
    elif house == 'd':
        k.cyl((0, 0, (z0 + z1) / 2), r * .26, z1 - z0, 'paint.emblem', 'frame', sides=20)
        for i in range(16):
            a = i * math.pi / 8
            L = r * (.62 if i % 2 else .7)
            c, s_ = math.cos(a), math.sin(a)
            k.prism(_tri_ccw([(c * r * .24 - s_ * r * .07, s_ * r * .24 + c * r * .07), (c * L, s_ * L),
                              (c * r * .24 + s_ * r * .07, s_ * r * .24 - c * r * .07)]), z0, z1 - .02, 'paint.emblem', 'frame')
        k.box((0, 0, z1 + .02), (r * .07, r * 1.4, .05), 'paint.emblem_ring', 'frame')                    # sword blade
        k.box((0, r * .45, z1 + .03), (r * .36, r * .06, .05), 'paint.emblem_ring', 'frame')              # crossguard
        k.box((0, r * .58, z1 + .03), (r * .08, r * .2, .05), 'paint.emblem_ring', 'frame')               # grip
    elif house == 'n1':                 # neutral: hazard-cornered octagon plate (stock generic Gate_1)
        k.prism([(x * .55, y * .55) for x, y in oct_], z0, z1 - .03, 'paint.emblem', 'frame')
        k.prism([(x * .4, y * .4) for x, y in oct_], z1 - .03, z1, 'paint.emblem_field', 'frame')
        for i in range(4):
            a = math.pi / 4 + i * math.pi / 2
            c, s_ = math.cos(a) * r * .72, math.sin(a) * r * .72
            u, v = -math.sin(a) * r * .14, math.cos(a) * r * .14
            k.prism(_tri_ccw([(c - u, s_ - v), (c + u, s_ + v), (c * .78, s_ * .78)]), z0, z1 - .02, 'hazard', 'frame')
    elif house == 'n2':                 # neutral: plain rust boss plate with four bolts (stock generic Gate_2)
        k.prism([(x * .62, y * .62) for x, y in oct_], z0, z1, 'paint.emblem', 'frame')
        for i in range(4):
            a = math.pi / 4 + i * math.pi / 2
            k.cyl((math.cos(a) * r * .5, math.sin(a) * r * .5, z1 + .02), r * .05, .04, 'paint.emblem_ring', 'detail', sides=8)
    elif house == 'n3':                 # neutral: twin upward chevrons (stock generic Gate_3)
        for yo in (-r * .28, r * .12):
            for sx in (-1, 1):          # each chevron as two convex arms meeting on the centre line
                q = [(sx * r * .52, yo), (0, yo + r * .32), (0, yo + r * .48), (sx * r * .52, yo + r * .16)]
                area = sum(q[i][0] * q[(i + 1) % 4][1] - q[(i + 1) % 4][0] * q[i][1] for i in range(4))
                k.prism(q if area > 0 else q[::-1], z0, z1, 'paint.emblem', 'frame')
    else:
        tri = [(-r * .66, -r * .42), (r * .66, -r * .42), (0, r * .72)]
        k.prism(tri, z0, z1 - .03, 'paint.emblem', 'frame')
        k.prism([(-r * .46, -r * .3), (r * .46, -r * .3), (0, r * .5)], z1 - .03, z1, 'paint.emblem_field', 'frame')
        k.box((0, -r * .02, z1 + .02), (r * .08, r * 1.1, .05), 'paint.emblem', 'frame')                  # sword blade
        k.box((0, -r * .4, z1 + .03), (r * .4, r * .07, .05), 'paint.emblem', 'frame')                    # crossguard
        k.box((0, -r * .55, z1 + .03), (r * .09, r * .2, .05), 'paint.emblem_ring', 'frame')              # grip
    count(k, 'emblems')


def _tri_ccw(t):
    a = (t[1][0] - t[0][0]) * (t[2][1] - t[0][1]) - (t[1][1] - t[0][1]) * (t[2][0] - t[0][0])
    return t if a > 0 else t[::-1]


def roundel(k, x, y, z, r, mat='paint.second'):
    """Faction roundel on a gate leaf face (disc along x, stencil ring and bars)."""
    with k.at((x, y, z), yaw=math.pi / 2, pitch=math.pi / 2):
        k.cyl((0, 0, 0), r, .12, mat, 'frame', sides=24, top=.03)
        k.cyl((0, 0, .07), r * .82, .04, 'paint.stencil', 'detail', sides=24)
        k.cyl((0, 0, .09), r * .7, .04, mat, 'detail', sides=24)
        for a in range(3):
            with k.at((0, 0, .1), yaw=a * math.pi / 3):
                k.box((0, 0, 0), (r * 1.2, r * .14, .03), 'paint.stencil', 'detail')


# owner 2026-10-24 footprint rule: every gate keeps the stock run exactly. Read from the stock mesh profile along the
# run (stock_runs.py): y_max (run end), tower faces (+outer, +inner, -inner, -outer), tower width, gate-zone width.
GATE_RUNS = {
    's_lightgate': (34.58, 22.33, 16.03, -16.17, -22.77, 6.0, 2.5),
    's_mediumgate': (35.75, 22.39, 16.14, -16.16, -22.71, 8.5, 2.5),
    's_heavygate': (35.75, 22.41, 16.15, -16.12, -22.69, 8.5, 10.0),
    'd_lightgate': (34.85, 20.0, 16.2, -16.2, -20.0, 6.5, 3.0),
    'd_mediumgate': (35.45, 19.96, 16.06, -16.09, -20.04, 8.0, 6.0),
    'd_heavygate': (35.43, 20.88, 16.1, -16.1, -20.92, 6.5, 6.5),
    'l_lightgate': (34.70, 20.01, 16.21, -16.24, -19.99, 5.0, 1.0),
    'l_mediumgate': (36.50, 22.0, 16.14, -16.1, -21.96, 6.5, 5.0),
    'l_heavygate': (36.50, 20.9, 16.1, -16.1, -20.9, 6.5, 4.5),
}


# stock gate door node (name, pivot in the authored frame = the stock pivot (minus the root offset the bake restores):
# the audit (anim_parity) shows the stock absolute position track lands the leaves on the stock bind pose this way; open travel in m from the
# stock <name>open.ase position track, door envelope thickness and height from the stock door geometry, floor pad width
# and height from the stock plan across the opening)
GATE_DOORS = {
    's_lightgate': ('S_LightGateDoor', (-.044, .028, .030), 5.451, 2.55, 5.67, 0.0, 0.0),
    's_mediumgate': ('S_MediumGateDoor', (-.028, .011, .025), 13.060, 2.56, 13.37, 0.0, 0.0),
    's_heavygate': ('S_HeavyGateDoor', (-.019, .005, .092), 22.101, 2.56, 22.15, 9.8, .2),
    'd_lightgate': ('D_LightGateDoor', (.054, -.006, .466), 5.209, 1.71, 5.33, 0.0, 0.0),
    'd_mediumgate': ('D_MediumGateDoor', (-.026, .023, .130), 11.476, 2.55, 12.03, 6.0, .2),
    'd_heavygate': ('D_HeavyGateDoor', (.013, .035, .113), 21.241, 2.16, 21.33, 6.5, .1),
    'l_lightgate': ('L_LightGateDoor', (.004, 0.0, .005), 5.071, 1.25, 4.82, 0.0, 0.0),
    'l_mediumgate': ('L_MediumGateDoor', (.004, 0.0, .093), 12.516, 2.48, 12.14, 5.0, .1),
    'l_heavygate': ('L_HeavyGateDoor', (.004, 0.0, .057), 21.826, 2.50, 21.52, 4.6, .1),
}


GATE_ENDPAD = {}   # (name, +1/-1) -> (width, height): low stock slab beyond a jamb instead of a wall run
GATE_CAP = {}      # name -> (eave z, ridge inset along the run): hipped door lintel inside the stock travel
GATE_APRON = {'s_mediumgate': (9.8, -18.08, 17.93)}
# leaf end clearance to the towers (+y, -y): the stock track's absolute positions shift each door node along the run
# (measured with tools/anim_parity, where TG z runs along -y), so the sinking leaves stay clear of the towers
LEAF_END = {'s_lightgate': (.05, .35), 's_heavygate': (.05, .3)}
GATE_BED = {'d_lightgate': (3.13, -17.12, 17.22, .51)}


def wall_gate(k):
    name = k.name
    fam = getattr(k, 'emblem', None) or name[0]       # generic gates carry a neutral stencil plate (n1/n2/n3)
    L, h_end, h_run, h_tower, h_leaf, w_end, w_tower_, w_leaf = GATES[name]
    y_max, po, pi_, ni, no, w_tower, gate_w = GATE_RUNS[name]
    # end post + wall run on each side (approved wall code, pier at the outer end), from the tower face to the stock end
    for sgn, face in ((1, po), (-1, -no)):
        run = y_max - face
        if run < .05:                    # the jamb is the stock end (tunnel portals)
            continue
        if (name, sgn) in GATE_ENDPAD:   # the stock end beyond the jamb is a low slab, not a wall run
            pw, ph = GATE_ENDPAD[(name, sgn)]
            k.box((0, sgn * (face + run / 2), ph / 2), (pw, run, ph), 'ferro', 'mass', ch=.03, walk=True)
            continue
        with k.at((0, sgn * (face + run / 2), 0), yaw=0.0 if sgn > 0 else math.pi):
            _wall_call(k, (w_end, run, h_end), dict(spans=1, wh=min(1.0, h_run / h_end)))
        count(k, 'panels_sections')
    # gate towers: piers in the same cast language, sized to the stock tower span, slit windows, lamps, beacon
    for sgn, (ya, yb) in ((1, (pi_, po)), (-1, (no, ni))):
        yt = (ya + yb) / 2
        pier(k, yt, w_tower, h_tower - .45, d=yb - ya)      # the beacon brings it to the stock tower height
        count(k, 'legs_supports')
        beacon(k, 0, yt, h_tower - .45)
        for side in (-1, 1):
            r = w_tower / 2 * (1 - .4 * .55)
            with k.at((side * r, yt, h_tower * .55), yaw=side * math.pi / 2):
                slit_window(k, 0, 0, min(1.6, w_tower * .3, (yb - ya) * .4), .45)
            zl = min(h_leaf + .8, h_tower * .45)
            ip.lamp(k, side * (w_tower / 2 * (1 - .4 * zl / h_tower) + .02), yt - (yb - ya) * .3 * (1 if sgn > 0 else -1), zl,
                    0.0 if side > 0 else math.pi, k.meta['sockets'], name='gate')
    # the gate leaves: one animated node with the exact stock name and pivot (stock <X>GateDoor; the stock open
    # animation translates it straight down). Two armoured leaves inside the stock door envelope, their height kept
    # within the stock travel so the open pose is entirely below ground; they drop through a slot in the floor pad.
    node, pivot, travel, door_w, door_h, pad_w, pad_h = GATE_DOORS[name]
    leaf_h = min(door_h, travel - .05)
    cap = GATE_CAP.get(name)            # (eave z, ridge inset): a hipped lintel on the leaves, riding with them
    if cap:
        cap_top, leaf_h = leaf_h, cap[0]
    t = door_w - .3
    slot = door_w + {'d_lightgate': .45}.get(name, .8)   # clear of the leaves, their plates and the stock track x offset
    if pad_w > slot:                 # floor pad (the stock low slab across the opening) split by the leaf slot
        for sx in (-1, 1):
            k.box((sx * (slot / 2 + (pad_w - slot) / 4), (pi_ + ni) / 2, pad_h / 2), ((pad_w - slot) / 2, pi_ - ni, pad_h), 'ferro', 'mass',
                  ch=.03, walk=True)
    for sx in (-1, 1):               # steel slot guides, tower to tower: they also close the run line across the opening
        gh = GATE_BED[name][3] + .04 if name in GATE_BED else min(pad_h + .12, .28)   # kerb lip the leaves drop past
        k.box((sx * (slot / 2 + .09), (pi_ + ni) / 2, gh / 2), (.18, pi_ - ni, gh), 'paint.steel', 'frame', walk=True)
    if name in GATE_BED:             # stock raised rail bed across the opening (Davion light gate), in short cast units
        bw, by0, by1, bz = GATE_BED[name]
        n_ = 2                          # two units meeting at the centre, as the stock bed (its vertices set the blocking)
        for i in range(n_):
            ya_, yb_ = by0 + (by1 - by0) * i / n_, by0 + (by1 - by0) * (i + 1) / n_
            for sx in (-1, 1):
                k.box((sx * (slot / 2 + (bw - slot) / 4), (ya_ + yb_) / 2, bz / 2), ((bw - slot) / 2, yb_ - ya_ - .04, bz), 'ferro', 'mass',
                      ch=.03, walk=True)
    if name in GATE_APRON:           # stock ground apron plate (flush, below the blocking height) round the opening
        aw, ay0, ay1 = GATE_APRON[name]
        for sx in (-1, 1):
            k.box((sx * (slot / 2 + (aw - slot) / 4), (ay0 + ay1) / 2, .012), ((aw - slot) / 2, ay1 - ay0, .024), 'ferro', 'mass', walk=True)
    with anim_node(k, node, pivot, axis=(0.0, 0.0, -1.0)):
        # leaves meet at the centre and stop 0.35 m short of the towers: the stock track's absolute positions shift the
        # node up to ~0.9 WU along the run, and the sinking leaves must stay clear of the tower footings
        mp, mn = LEAF_END.get(name, (.2, .2))
        for sgn, (ya, yb) in ((1, (0.0, pi_ - mp)), (-1, (ni + mn, 0.0))):
            g0 = abs(yb - ya)
            k.box((0, (ya + yb) / 2, .4), (t + .3, yb - ya, .8), 'paint.trim', 'frame', ch=.06)
            rows = max(2, round(leaf_h / 3.4))
            rh = (leaf_h - 1.2) / rows
            cols = max(2, round((yb - ya) / 4.0))
            cw = (yb - ya) / cols
            for r_ in range(rows):
                for c_ in range(cols):
                    k.box((0, ya + (c_ + .5) * cw, .8 + (r_ + .5) * rh), (t, cw - .12, rh - .12), 'paint.door', 'skin', ch=.06,
                          door=True, feature='gate')
            for c_ in range(cols + 1):           # stiles; the end ones flush with the leaf ends (they sink through the slot)
                yc_ = min(max(ya + c_ * cw, ya + .15), yb - .15)
                k.box((0, yc_, leaf_h / 2 + .4), (t + .2, .3, leaf_h - .8), 'paint.trim', 'frame', ch=.05)
            k.box((0, (ya + yb) / 2, leaf_h - .15), (t + .3, yb - ya, .3), 'paint.trim', 'frame', ch=.06)
            for side in (-1, 1):
                k.box((side * (t / 2 + .03), (ya + yb) / 2, 1.05), (.06, yb - ya - .4, .5), 'hazard', 'detail')
                if fam == 'd':          # Davion: one sunburst per leaf; it rides down with the leaf
                    with k.at((side * (t / 2 - .05), (ya + yb) / 2, leaf_h * .58), yaw=side * math.pi / 2, pitch=math.pi / 2):
                        house_emblem(k, fam, min(leaf_h * .22, 3.2, g0 * .4), depth=.26)
            count(k, 'doors')
        if cap:                         # hipped riveted lintel across both leaves, under the stock travel
            ya_, yb_ = ni + LEAF_END.get(name, (.2, .2))[1], pi_ - LEAF_END.get(name, (.2, .2))[0]
            loft(k, [rring(-(t + .3) / 2, ya_, (t + .3) / 2, yb_, leaf_h - .02, .05),
                     rring(-t / 2 + .2, ya_ + cap[1], t / 2 - .2, yb_ - cap[1], cap_top, .05)], 'paint.trim', 'frame')
            for side in (-1, 1):        # rivet row along the eave, both faces
                for j in range(13):
                    yy = ya_ + .6 + (yb_ - ya_ - 1.2) * j / 12
                    k.box((side * ((t + .3) / 2 + .01), yy, leaf_h + .2), (.05, .14, .14), 'metal', 'detail')
        if fam != 'd':                  # Steiner fist / Capellan emblem: one plate centred on the leaves, moving with them
            for side in (-1, 1):
                with k.at((side * (t / 2 - .05), 0.0, leaf_h * .62), yaw=side * math.pi / 2, pitch=math.pi / 2):
                    house_emblem(k, fam, min(leaf_h * .26, 3.8), depth=.26)
    for ya, yb in ((pi_, po), (no, ni)):     # a smaller emblem on each gate-tower pier, both faces, above the slit window
        yt = (ya + yb) / 2
        re_ = min(w_tower * .18, (yb - ya) * .3, (h_tower - (h_tower * .55 + .45)) / 2 - .15, 1.4)
        zp = h_tower * .55 + .35 + re_ + .1
        for side in (-1, 1):
            xf = w_tower / 2 * (1 - .4 * zp / h_tower)
            with k.at((side * xf, yt, zp), yaw=side * math.pi / 2, pitch=math.pi / 2):
                house_emblem(k, fam, re_, depth=.4)


# owner footprint rule: posts take the stock plan exactly (x, y) and the stock height including the beacon
POST_DIMS = {'s_lightpost': (5.042, 5.042, 5.57), 's_mediumpost': (7.5, 7.5, 13.986), 's_heavypost': (7.5, 7.5, 22.197),
             'd_lightpost': (5.673, 5.707, 5.608), 'd_mediumpost': (6.849, 6.895, 14.37), 'd_heavypost': (6.872, 6.892, 22.319),
             'l_lightpost': (5.201, 5.296, 5.79), 'l_mediumpost': (6.865, 6.991, 14.461), 'l_heavypost': (9.405, 9.019, 22.258)}


def wall_post(k):
    wx, wy, h = POST_DIMS[k.name]
    pier(k, 0.0, wx, h - .45, d=wy)
    n0 = len(k.pieces)
    beacon(k, 0, 0, h - .45)
    count(k, 'legs_supports')


# ================================================================== batch 3: stock-massing driven command/HQ buildings
_MASS = None


def massing(name):
    """Stock massing (assets/military_massing.json, from stock_massing.py): blocks + the stock height grid."""
    global _MASS
    if _MASS is None:
        import json
        from pathlib import Path
        _MASS = json.loads((Path(__file__).resolve().parent / 'assets' / 'military_massing.json').read_text())
    return _MASS[name]


class Field:
    """Stock height field lookups (kit metres)."""

    def __init__(self, m):
        self.g = m['grid']
        self.x0, self.y0 = m['origin']
        self.c = m['cell']
        self.ny, self.nx = len(self.g), len(self.g[0])

    def at(self, x, y):
        i, j = math.floor((x - self.x0) / self.c), math.floor((y - self.y0) / self.c)
        if 0 <= i < self.nx and 0 <= j < self.ny:
            return self.g[j][i]
        return 0.0


def _outside(F, box, s, t, d=.45):
    """Stock height just outside side s of box at fraction t along the side."""
    x0, y0, x1, y1 = box
    if s == 's':
        return F.at(x0 + (x1 - x0) * t, y0 - d)
    if s == 'n':
        return F.at(x1 - (x1 - x0) * t, y1 + d)
    if s == 'e':
        return F.at(x1 + d, y0 + (y1 - y0) * t)
    return F.at(x0 - d, y1 - (y1 - y0) * t)


def part_blocks(name, pivots, max_area=10.0):
    """Indices of small stock blocks that are the moving part itself (they hold a stock pivot)."""
    out = set()
    for i, b in enumerate(massing(name)['blocks']):
        x0, y0, x1, y1 = b['box']
        if (x1 - x0) * (y1 - y0) <= max_area and any(x0 - .3 <= x <= x1 + .3 and y0 - .3 <= y <= y1 + .3 for x, y in pivots):
            out.add(i)
    return out


def masts(k, name):
    """Stock masts/spires (tall slivers above every block): a steel pole with collars and a beacon."""
    for x, y, h, w in massing(name).get('masts', []):
        z0 = roof_z(name, x, y)
        if h - z0 < 2.0:
            continue
        r = max(.1, min(.35, w * .25))
        k.box((x, y, z0 + .15), (r * 5, r * 5, .3), 'paint.plate', 'frame', ch=.05)
        k.cyl((x, y, (z0 + h) / 2), r, h - z0, 'metal', 'frame', sides=10)
        for z in ip.frange(z0 + 2.0, h - 1.0, 2.5):
            k.cyl((x, y, z), r * 1.6, .12, 'paint.trim', 'frame', sides=10)
        k.cyl((x, y, h + .08), r * 1.4, .16, 'lamp_red', 'frame', sides=10)
        count(k, 'antennas_dishes')


def massed(k, name, mat='paint.main', storey=3.6, door=None, rib=4.0, skip=(), pivots=(), arrays=False):
    """Build every stock block: flat blocks as armoured halls (only the sides the stock leaves exposed, slit windows
    per storey where the outside is open at that height), profile blocks (arched/sloped stock roofs) as one mass."""
    m = massing(name)
    F = Field(m)
    parts = part_blocks(name, pivots)
    blocks = []
    for i, b in enumerate(m['blocks']):
        if i in skip or b['h'] < 1.0:
            continue
        if i in parts:
            x0, y0, x1, y1 = b['box']
            touch = [o['h'] for j, o in enumerate(m['blocks']) if j not in parts and o['box'][0] <= x1 + .1 and o['box'][2] >= x0 - .1
                     and o['box'][1] <= y1 + .1 and o['box'][3] >= y0 - .1]
            b = dict(b, h=max(touch) if touch else 0.0)
            if b['h'] < 1.0:
                continue
        blocks.append(b)
    blocks.sort(key=lambda b: (b['axis'] is not None, -(b['box'][2] - b['box'][0]) * (b['box'][3] - b['box'][1])))
    door_done = door is not None
    masts(k, name)
    for b in blocks:
        box, H = b['box'], b['h']
        x0, y0, x1, y1 = box
        if b['axis']:
            profile_block(k, b, F, mat)
            continue
        area = (x1 - x0) * (y1 - y0)
        tower = H >= 12 and area <= 160
        array = arrays and H >= 18 and min(x1 - x0, y1 - y0) < 4.6
        sides, ops, skirts, ground = '', {}, '', ''
        for s in 'senw':
            L = (x1 - x0) if s in 'sn' else (y1 - y0)
            exp = [_outside(F, box, s, (i + .5) / 8) for i in range(8)]
            if sum(e < H - .3 for e in exp) < 1:      # any exposure: no open seams between unequal blocks
                continue
            sides += s
            if sum(e < .8 for e in exp) >= 6 and H > 2.5 and L > 3.0:
                skirts += s
            if sum(e < .8 for e in exp[3:5]) == 2:
                ground += s
            lst = []
            Lw = L - (2 * .5 if s in 'ew' else 0)
            n = max(0, int((Lw - 2.4) / 3.4))
            for z in ([] if array else ip.frange(1.9, H - (3.4 if tower else 1.4), storey)):
                for i in range(n):
                    xl = -Lw / 2 + 1.7 + (Lw - 3.4) * (i + .5) / max(1, n)
                    t = (xl + L / 2) / L
                    if max(_outside(F, box, s, t), _outside(F, box, s, t, 1.5)) < z - .4 and abs(xl) + 1.0 < Lw / 2:
                        lst.append(('slit', xl, z, 1.3, .42))
            ops[s] = lst
        if not door_done and H >= 3.2:
            for s in skirts + ground:
                L = (x1 - x0) if s in 'sn' else (y1 - y0)
                if L > 4.0:
                    ops[s] = [o for o in ops.get(s, []) if not (abs(o[1]) < 1.8 and o[2] < 4.0)] + [('blast', 0.0, 0, 1.6, 2.5)]
                    door_done = True
                    break
        if door and door[0] == blocks.index(b):
            s = door[1]
            ops[s] = [o for o in ops.get(s, []) if not (abs(o[1] - door[2]) < 2.0 and o[2] < 4.0)] + [('blast', door[2], 0, 1.6, 2.5)]
        if not sides:
            continue
        n0 = len(k.pieces)
        mil_hall(k, box, H, ops, sides=sides, skirt=(min(1.9, H * .25), .55) if skirts else (0, 0), skirt_sides=skirts or None,
                 rib=(False if array else rib) if H > 4 else False, mat='paint.trim' if array else mat, band=H > 5 and not array)
        if tower and not array:
            cab = ''.join(s_ for s_ in sides if all(_outside(F, box, s_, (i + .5) / 6, d) < H - 3.0 for i in range(6) for d in (.45, 1.5)))
            control_cab(k, box, H, cab)
        if array:
            sensor_array(k, box, H, sides)
        for pc in k.pieces[n0:]:
            if pc['role'] == 'roof':
                pc['lint_skip'] = True      # the deck spans this stock block only; abutting neighbours carry the open edges
        if 'n' not in sides or 's' not in sides or 'e' not in sides or 'w' not in sides:
            pass


def control_cab(k, box, H, sides):
    """Stock control-cab signature on a tower head: a dark glazing band with mullions under a chamfered visor cap."""
    F = I.faces_of(*box, .5)
    for s_ in sides:
        origin, yaw, L, _ = F[s_]
        with k.at(origin, yaw=yaw):
            k.slab(-L / 2 + .3, L / 2 - .3, H - 2.7, H - 1.25, -.08, .02, 'paint.trim', 'frame', front=.02)
            k.slab(-L / 2 + .45, L / 2 - .45, H - 2.55, H - 1.4, -.1, -.06, 'glass', 'glass', pane=True)
            n = max(2, int(L / 1.4))
            for i in range(1, n):
                x = -L / 2 + .45 + (L - .9) * i / n
                k.slab(x - .05, x + .05, H - 2.55, H - 1.4, -.14, -.08, 'paint.trim', 'frame')
            k.prism([(-L / 2 - .2, H - 1.25), (L / 2 + .2, H - 1.25), (L / 2 + .2, H - 1.05), (-L / 2 - .2, H - 1.05)], -.7, .05,
                    'paint.second', 'frame', axis='y')
        count(k, 'windows')


def sensor_array(k, box, H, sides):
    """Tall stock sensor slab: dark array face panels in a grid with lit emitter strips."""
    F = I.faces_of(*box, .5)
    for s_ in sides:
        origin, yaw, L, _ = F[s_]
        with k.at(origin, yaw=yaw):
            rows = max(2, int((H - 3.0) / 2.4))
            cols = max(1, int(L / 1.6))
            for r in range(rows):
                z0 = 2.4 + r * (H - 3.4) / rows
                for c in range(cols):
                    x0 = -L / 2 + .25 + (L - .5) * c / cols
                    k.slab(x0 + .06, x0 + (L - .5) / cols - .06, z0 + .06, z0 + (H - 3.4) / rows - .06, -.06, .01, 'paint.plate', 'detail')
                k.slab(-L / 2 + .3, L / 2 - .3, z0 - .03, z0 + .03, -.08, .01, 'lamp_cool', 'frame')


def profile_block(k, b, F, mat):
    """One mass under a stock arched/sloped roof: section follows the stock roof profile along its axis."""
    x0, y0, x1, y1 = b['box']
    ax = b['axis']
    a0, a1, w0, w1 = (x0, x1, y0, y1) if ax == 'x' else (y0, y1, x0, x1)
    prof = sorted((p[0], p[1]) for p in b['prof'])
    top = [(a0, prof[0][1])] + prof + [(a1, prof[-1][1])]
    poly = [(a0, 0.0), (a1, 0.0)] + list(reversed(top))
    roof = [(p, h + .02) for p, h in top]
    skin = [(p, h - .18) for p, h in roof]
    rpoly = roof + list(reversed(skin))
    if ax == 'x':
        k.prism(poly, w0, w1, mat, 'mass', axis='y')
        k.prism(rpoly, w0 - .15, w1 + .15, 'paint.roof', 'roof', axis='y', lint_skip=True)
    else:
        k.prism(poly, w0, w1, mat, 'mass', axis='x')
        k.prism(rpoly, w0 - .15, w1 + .15, 'paint.roof', 'roof', axis='x', lint_skip=True)
    hof = lambda p: float(np.interp(p, [q[0] for q in top], [q[1] for q in top]))  # noqa: E731
    n = max(1, int((a1 - a0) / 4.0))
    for i in range(n + 1):
        p = a0 + .35 + (a1 - a0 - .7) * i / n
        h = hof(p)
        c = (p, (w0 + w1) / 2, h / 2 + .05) if ax == 'x' else ((w0 + w1) / 2, p, h / 2 + .05)
        s = (.45, w1 - w0 + .3, h + .2) if ax == 'x' else (w1 - w0 + .3, .45, h + .2)
        k.box(c, s, 'paint.plate', 'frame', ch=.06)
    c = ((a0 + a1) / 2, (w0 + w1) / 2, .3) if ax == 'x' else ((w0 + w1) / 2, (a0 + a1) / 2, .3)
    s = (a1 - a0 + .3, w1 - w0 + .3, .6) if ax == 'x' else (w1 - w0 + .3, a1 - a0 + .3, .6)
    k.box(c, s, 'paint.second', 'mass', ch=.08, top=.06)
    # slit windows along both long faces where the outside is open
    for sgn in (-1, 1):
        wf = w0 if sgn < 0 else w1
        nwin = max(0, int((a1 - a0 - 2.0) / 3.2))
        for i in range(nwin):
            p = a0 + 1.6 + (a1 - a0 - 3.2) * (i + .5) / max(1, nwin)
            if hof(p) < 3.6:
                continue
            outside = F.at(p, wf + sgn * .45) if ax == 'x' else F.at(wf + sgn * .45, p)
            if outside > 1.6:
                continue
            if ax == 'x':
                fr = ((p, wf, 2.1), 0.0 if sgn < 0 else math.pi)
            else:
                fr = ((wf, p, 2.1), -math.pi / 2 if sgn < 0 else math.pi / 2)
            with k.at(fr[0], yaw=fr[1]):
                slit_window(k, 0, 0, 1.3, .42)


def top_at(name, x, y):
    return Field(massing(name)).at(x, y)


def helipad(k, x, y, z, r):
    """Roof helipad: ring, H and hazard edge (detail paint on the roof deck)."""
    k.cyl((x, y, z + .03), r, .06, 'paint.second', 'frame', sides=32)
    k.cyl((x, y, z + .07), r * .86, .03, 'paint.stencil', 'detail', sides=32)
    k.cyl((x, y, z + .085), r * .8, .03, 'paint.second', 'detail', sides=32)
    with k.at((x, y, z + .1), pitch=-math.pi / 2):
        stencil(k, 'H', 0, 0, r * .9, 0.0)
    for a in range(8):
        aa = a * math.pi / 4
        k.box((x + math.cos(aa) * r * .95, y + math.sin(aa) * r * .95, z + .1), (.25, .25, .1), 'lamp', 'frame')


def searchlight(k, name, pivot, r=.7, anim=True):
    """Searchlight on a yoke: drum with a glowing lens; rotates as the stock node about its vertical axis."""
    x, y, z = pivot
    k.cyl((x, y, z - .25), .45, .5, 'paint.plate', 'frame', sides=14, top=.05)
    ctx = anim_node(k, name, pivot) if anim else _null()
    with ctx:
        k.cyl((x, y, z + .1), .55, .2, 'paint.trim', 'frame', sides=14)
        for s_ in (-1, 1):
            k.box((x + s_ * (r + .15), y, z + .55), (.12, .3, .9), 'paint.plate', 'frame', ch=.03)
        with k.at((x, y, z + .75), pitch=math.pi / 2 - .35):
            k.cyl((0, 0, 0), r, 1.1, 'paint.second', 'frame', sides=16, top=.06)
            k.cyl((0, 0, .58), r * .85, .06, 'lamp', 'frame', sides=16)
            k.cyl((0, 0, -.62), r * .6, .2, 'paint.trim', 'frame', sides=12)
    count(k, 'antennas_dishes')


@contextmanager
def _null():
    yield


def roof_at(name, x, y, inset=.9):
    """(x, y, z) on the roof of the stock block nearest the stock pivot (ties: the taller), pulled inside its edge;
    the small block that is the stock moving part itself is not a roof."""
    best = None
    part = part_blocks(name, [(x, y)])
    for i, b in enumerate(massing(name)['blocks']):
        if i in part:
            continue
        x0, y0, x1, y1 = b['box']
        d = math.hypot(max(x0 - x, 0, x - x1), max(y0 - y, 0, y - y1)) - .001 * b['h']
        if best is None or d < best[0]:
            best = (d, b)
    x0, y0, x1, y1 = best[1]['box']
    xi = min(max(x, x0 + inset), x1 - inset) if x1 - x0 > 2 * inset else (x0 + x1) / 2
    yi = min(max(y, y0 + inset), y1 - inset) if y1 - y0 > 2 * inset else (y0 + y1) / 2
    return xi, yi, best[1]['h']


def roof_z(name, x, y):
    return roof_at(name, x, y)[2]


def spin_dish(k, name, x, y, z, r, stand=0.0):
    """Rotating dish (stock animated node): pedestal (on a braced stand up to the stock pivot height) under the pivot,
    dish on a yoke turning about the vertical axis."""
    if stand > .5:
        k.cyl((x, y, z + stand / 2), .3, stand, 'metal', 'frame', sides=12)
        for a in (0.4, 2.5, 4.6):
            k.beam((x + math.cos(a) * 1.3, y + math.sin(a) * 1.3, z + .05), (x, y, z + stand * .6), .12, ip.STEEL, 'frame')
        z += stand
    k.cyl((x, y, z + .2), .45, .4, 'paint.plate', 'frame', sides=14, top=.05)
    with anim_node(k, name, (x, y, z + .4)):
        dish(k, (x, y), r, -math.pi / 2, .7, z + .4 + r * .6)


def gate_control(k, name, radar=None, dish_=None, rd=None):
    massed(k, name, pivots=[p[:2] for p in (radar, dish_) if p])
    if radar:
        x, y, z = roof_at(name, *radar)
        radar_dipole(k, rd or 'Base01', (x, y, z + .6), L=3.4)
    if dish_:
        x, y, z = roof_at(name, dish_[0], dish_[1])
        spin_dish(k, 'Dish', x, y, z, dish_[2])


def s_gatecontrol(k):
    gate_control(k, 's_gatecontrol', radar=(6.51, -2.05))


def d_gatecontrol(k):
    gate_control(k, 'd_gatecontrol', dish_=(-2.27, 4.02, 1.3))
    # stock front stairs: a steel flight along the front up to a landing at the gate block roof
    ip.stair(k, (-1.2, -6.1, 0.0), (-11.0, -6.1, 7.6), w=1.0, name='front stair')
    ip.rect_platform(k, -12.8, -6.75, -11.0, -4.55, 7.6, rail_sides='ws', name='stair landing')
    count(k, 'stairs_ladders')


def l_gatecontrol(k):
    gate_control(k, 'l_gatecontrol', dish_=(-10.24, 1.03, 1.2))


def bturretcontrol(k):
    """Generic turret control: the batch-1 turret-control family (battered square tower with crown and spinning radar
    bar at the stock Spinner_PAB pivot) over the stock battered annex and raised block."""
    turret_control(k, dict(kind='sq', c=(-11.5, 4.82), r0=3.6, r1=2.7, ar=1.25, H=20.2, head='crown', collars=(7.0, 13.0),
                           radar=(-11.5, 4.82), radar_node='Spinner_PAB'), [
        dict(kind='pod', box=(-6.1, -13.5, 13.7, 9.3), H=5.8, bat=1.6, ch=1.2, slits=[('s', -6.0, 3.0), ('s', 0.0, 3.0), ('s', 6.0, 3.0),
             ('e', -6.0, 3.0), ('w', -6.0, 3.0)], doors=[('s', 3.0)], stencil=('s', -3.0, 3.0, '08')),
        dict(kind='pod', box=(7.7, 5.7, 15.5, 12.9), H=8.5, bat=.9, ch=1.0, slits=[('n', 0.0, 5.5), ('e', 0.0, 5.5)])],
        [[(-15.6, -3.0), (-15.6, -10.0), (-13.0, -12.6), (-8.0, -12.6)]],
        [lambda k: _whip(k, 1.78, 1.12, 5.8, 10.0),
         lambda k: roof_kit(k, [(-1.0, -6.0), (8.0, -6.0)], [(3.0, 0.0)], 5.8)])


def _whip(k, x, y, z, h):
    k.box((x, y, z + .15), (.6, .6, .3), 'paint.plate', 'frame', ch=.05)
    k.cyl((x, y, z + h / 2), .1, h, 'metal', 'frame', sides=8)
    k.cyl((x, y, z + h + .08), .14, .16, 'lamp_red', 'frame', sides=8)
    count(k, 'antennas_dishes')


def sensor_control(k, name, dish_xy, twin=None):
    massed(k, name, rib=3.6, pivots=[dish_xy] + ([twin] if twin else []), arrays=True)
    x, y, z = roof_at(name, *dish_xy)
    if twin:
        # stock L_Dish (turntable, vertical axis) carrying L_Sdish (the dish, tilting about its horizontal axis)
        k.cyl((x, y, z + .3), .5, .6, 'paint.plate', 'frame', sides=14, top=.05)
        with anim_node(k, 'L_Dish', (x, y, z + .6)):
            k.cyl((x, y, z + .75), .9, .3, 'paint.trim', 'frame', sides=18, top=.05)
            for s_ in (-1, 1):
                k.box((x + s_ * .9, y, z + 1.6), (.2, .4, 1.7), 'paint.plate', 'frame', ch=.04)
        sx, sy = x + (twin[0] - dish_xy[0]), y + (twin[1] - dish_xy[1])
        with anim_node(k, 'L_Sdish', (sx, sy, z + 2.4), parent='L_Dish', axis=(0.0, -1.0, 0.0)):
            with k.at((sx, sy, z + 2.4), yaw=-math.pi / 2, pitch=-.35):
                ip.lathe(k, [(0.0, 0.0), (1.0, .07), (1.9, .52), (1.86, .62), (1.0, .25), (0.0, .08)], 'paint.second', 'frame', n=32)
                k.cyl((0, 0, 1.4), .12, .3, 'paint.trim', 'frame', sides=10)
                for a in (0, 2.1, 4.2):
                    k.beam((math.cos(a) * 1.7, math.sin(a) * 1.7, .55), (0, 0, 1.3), .04, 'metal', 'frame')
        count(k, 'antennas_dishes')
    else:
        spin_dish(k, 'L_Dish', x, y, z, 2.8, stand=max(0.0, 26.0 - z))


def s_sensorcontrol(k):
    sensor_control(k, 's_sensorcontrol', (-0.83, 3.95))


def d_sensorcontrol(k):
    sensor_control(k, 'd_sensorcontrol', (-0.86, 3.94))


def l_sensorcontrol(k):
    sensor_control(k, 'l_sensorcontrol', (-2.69, 3.94), twin=(-3.45, 3.94))


def s_spotlightcontrol(k):
    massed(k, 's_spotlightcontrol', pivots=[(-8.29, 8.18)])
    x, y, z = roof_at('s_spotlightcontrol', -8.29, 8.18)
    searchlight(k, 'Light', (x, y, z + .5), r=.85)


def l_spotlightcontrol(k):
    massed(k, 'l_spotlightcontrol', pivots=[(-6.73, 7.24)])
    x, y, z = roof_at('l_spotlightcontrol', -6.73, 7.24)
    searchlight(k, 'light', (x, y, z + .5), r=.75)


def hq(k, name, pad_scale=.38):
    massed(k, name)
    m = massing(name)
    tall = max(m['blocks'], key=lambda b: b['h'])
    x0, y0, x1, y1 = tall['box']
    r = min(x1 - x0, y1 - y0) * pad_scale
    helipad(k, (x0 + x1) / 2, (y0 + y1) / 2, tall['h'], r)
    for (xx, yy) in ((x0 + .6, y0 + .6), (x1 - .6, y1 - .6)):
        antenna(k, xx, yy, tall['h'], .9)


def s_hq(k):
    hq(k, 's_hq')


def l_hq(k):
    hq(k, 'l_hq')


def d_hq(k):
    hq(k, 'd_hq')


def generic_hq(k):
    hq(k, 'generic_hq')


# ------------------------------------------------------------------ slender defence towers
def guntower(k):
    """Timber trestle gun tower (stock): four raked legs with X bracing and girts, a ladder inside to an armoured gun
    cab with a slit band and a gun on a mantlet (static; the stock has no animation block)."""
    cx, cy = .71, .03
    zc = 12.6
    b0, b1 = 2.25, 1.55
    legs = [((cx + sx * b0, cy + sy * b0, 0), (cx + sx * b1, cy + sy * b1, zc)) for sx, sy in ((-1, -1), (1, -1), (1, 1), (-1, 1))]
    for a, b in legs:
        k.box((a[0], a[1], .3), (.8, .8, .6), 'ferro.main', 'mass', ch=.06)
        k.beam(a, b, .34, 'wood.crate', 'frame')
    zs = [0.6, 3.6, 6.6, 9.6, zc - .2]
    P = lambda z: [tuple(a[i] + (b[i] - a[i]) * z / zc for i in range(3)) for a, b in legs]  # noqa: E731
    for za, zb in zip(zs, zs[1:]):
        A, B = P(za), P(zb)
        for i in range(4):
            j = (i + 1) % 4
            k.beam(A[i], B[j], .16, 'wood.crate', 'frame')
            k.beam(A[j], B[i], .16, 'wood.crate', 'frame')
            k.beam(B[i], B[j], .2, 'wood.crate', 'frame')
    # cab floor with a ladder hatch, armoured cab, roof, gun
    pid = ip._platform(k, 'gun cab floor')
    hx0, hx1, hy0, hy1 = cx + .6, cx + 1.35, cy - .4, cy + .4
    for (a, b, c_, d) in ((cx - 2.5, cy - 2.3, cx + 2.5, hy0), (cx - 2.5, hy1, cx + 2.5, cy + 2.3), (cx - 2.5, hy0, hx0, hy1), (hx1, hy0, cx + 2.5, hy1)):
        k.box(((a + c_) / 2, (b + d) / 2, zc + .1), (c_ - a, d - b, .2), 'paint.plate', 'mass', walk=True, platform=pid)
    for s_ in (-1, 1):
        k.box((cx, cy + s_ * 2.2, zc + 1.0), (5.0, .2, 1.6), 'paint.plate', 'mass', ch=.04)
        k.box((cx + s_ * 2.4, cy, zc + 1.0), (.2, 4.2, 1.6), 'paint.plate', 'mass', ch=.04)
    for s_ in (-1, 1):
        k.box((cx, cy + s_ * 2.31, zc + 1.25), (4.2, .04, .3), 'glass', 'glass', pane=True)
        k.box((cx + s_ * 2.51, cy, zc + 1.25), (.04, 3.4, .3), 'glass', 'glass', pane=True)
    count(k, 'windows', 4)
    k.box((cx, cy, zc + 1.95), (5.5, 4.9, .3), 'paint.second', 'roof', ch=.1, top=.05)
    k.box((cx - 2.55, cy, zc + 1.0), (.5, 1.2, .9), 'paint.trim', 'frame', ch=.08)
    ip.tube(k, [(cx - 2.8, cy, zc + 1.0), (-3.25, cy, zc + 1.0)], .09, 'metal', 'frame', sides=10)
    ip.hbeam(k, (hx1 + .08, cy, .0), (hx1 + .08, cy, zc - .1), .14, .14)
    ip.ladder(k, hx1, cy, math.pi, 0.0, zc + .2, name='ground to gun cab')
    count(k, 'stairs_ladders')
    count(k, 'legs_supports', 4)


def mast_platform(k, cx, cy, zp, half, name):
    """Railed service platform round a pole: grated deck with a ladder hatch on the south side, knee braces, and a
    caged ladder from the ground on its own post (rungs clear of the pole and of any plinth)."""
    pid = ip._platform(k, name)
    hy0, hy1 = cy - half + .05, cy - half + .9
    for (a, b, c_, d) in ((cx - half, cy - half, cx + half, hy0), (cx - half, hy1, cx + half, cy + half), (cx - half, hy0, cx - .48, hy1),
                          (cx + .48, hy0, cx + half, hy1)):
        if c_ - a > .05 and d - b > .05:
            k.box(((a + c_) / 2, (b + d) / 2, zp - .04), (c_ - a, d - b, .08), 'grate', 'frame', walk=True, platform=pid)
    for a, b in (((cx - half, cy - half), (cx + half, cy - half)), ((cx + half, cy - half), (cx + half, cy + half)),
                 ((cx + half, cy + half), (cx - half, cy + half)), ((cx - half, cy + half), (cx - half, cy - half))):
        k.beam((a[0], a[1], zp - .16), (b[0], b[1], zp - .16), .1, ip.STEEL, 'frame', h=.18)
    for sx in (-1, 1):
        ip.angle_brace(k, (cx + sx * .3, cy, zp - 2.2), (cx + sx * (half - .1), cy, zp - .2))
    ip.angle_brace(k, (cx, cy + .3, zp - 2.2), (cx, cy + half - .1, zp - .2))
    ip.railing(k, [(cx - half, cy - half), (cx + half, cy - half), (cx + half, cy + half), (cx - half, cy + half)], zp, closed=True)
    yl = cy - half + .66
    ip.hbeam(k, (cx, yl + .07, 0.0), (cx, yl + .07, zp - .2), .14, .14)
    ip.ladder(k, cx, yl, -math.pi / 2, 0.0, zp, name=name + ' ladder')
    count(k, 'stairs_ladders')


def _ccw_toward(q, n):
    """Order quad q so its winding faces along n (loft stacks rings along n)."""
    e1 = [q[1][i] - q[0][i] for i in range(3)]
    e2 = [q[2][i] - q[0][i] for i in range(3)]
    c = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
    return q if sum(c[i] * n[i] for i in range(3)) > 0 else q[::-1]


def spotlight(k):
    """Field searchlight after the stock (owner: no platform, no ladder, nothing on top of the lamp): a battered
    armoured plinth (diamond plan), a slim mast, and the stock L_SpotRotar fork turning on the mast with the lamp drum
    held at its sides on a horizontal trunnion axis, lens facing out, between the fork arms.
    The stock L_SpotRotar pivot sits 0.14 m ahead of the mast axis."""
    px, py, pz = -0.004, -0.477, 18.995          # stock L_SpotRotar node in the stock frame (anim_parity)
    mx, my = 0.0, -.62                           # stock mast axis
    base = [(0, -2.52), (1.93, -.03), (0, 2.0), (-1.93, -.03)]
    top = [(0, -2.11), (1.41, -.26), (0, .86), (-1.41, -.26)]
    ring = lambda pts, z, f=1.0: [(x * f, my + (y - my) * f, z) for x, y in pts]   # noqa: E731
    loft(k, [ring(base, 0), ring(base, .45), ring(top, 7.95)], 'paint.second', 'mass')
    loft(k, [ring(base, 0, 1.03), ring(base, .45, 1.03)], 'paint.trim', 'frame')            # footing collar
    loft(k, [ring(top, 7.95, 1.04), ring(top, 8.25, .96)], 'paint.plate', 'frame')           # cap
    for i in range(4):                                                                      # armoured face panels
        a0, b0 = base[i], base[(i + 1) % 4]
        a1, b1 = top[i], top[(i + 1) % 4]
        lerp = lambda p, q, t: (p[0] + (q[0] - p[0]) * t, p[1] + (q[1] - p[1]) * t)        # noqa: E731
        z0, z1 = 1.0, 7.2
        f0, f1 = z0 / 7.95, z1 / 7.95
        lo0, lo1 = lerp(a0, a1, f0), lerp(b0, b1, f0)
        hi0, hi1 = lerp(a0, a1, f1), lerp(b0, b1, f1)
        q = []
        for (u, v), z in ((lerp(lo0, lo1, .14), z0), (lerp(lo0, lo1, .86), z0), (lerp(hi0, hi1, .86), z1), (lerp(hi0, hi1, .14), z1)):
            q.append((u, v, z))
        ex, ey = b0[0] - a0[0], b0[1] - a0[1]
        n = (ey, -ex, 0.0)                       # outward for the CCW plan ring
        L = (n[0] ** 2 + n[1] ** 2) ** .5
        n = (n[0] / L, n[1] / L, 0.0)
        q = [(x, my + (y - my), z) for x, y, z in q]
        q = _ccw_toward(q, n)
        loft(k, [q, [(x + n[0] * .07, y + n[1] * .07, z) for x, y, z in q]], 'paint.main', 'frame')
        for j in range(2):                       # bolts at the panel head
            u, v = lerp(hi0, hi1, .3 + .4 * j)
            k.box((u + n[0] * .1, v + n[1] * .1, z1 - .25), (.12, .12, .12), 'metal', 'detail')
    zt = pz - .55
    k.box((mx, my, (8.25 + zt) / 2), (.7, .7, zt - 8.25), 'paint.plate', 'mass', ch=.06)     # slim mast
    for z in (11.0, 14.5):
        k.box((mx, my, z), (.84, .84, .16), 'paint.trim', 'frame', ch=.03)
    k.box(((mx + px) / 2, (my + py) / 2, zt + .15), (1.0, 1.0, .3), 'paint.trim', 'frame', ch=.05)   # bearing plinth
    k.cyl((px, py, pz - .2), .55, .4, 'paint.plate', 'frame', sides=16)                    # slew bearing
    with anim_node(k, 'L_SpotRotar', (px, py, pz)):
        # lamp after the stock housing: round lens face forward (-y, 2.8 m across), body widest just
        # behind it, tapering to a narrow back with the vent ring; trunnion axis through the body at the slew axis
        zc, rb = 21.75, 1.62
        k.cyl((px, py, pz + .15), .6, .3, 'paint.trim', 'frame', sides=16)               # turntable
        k.box((px, py, pz + .45), (2 * rb + .9, .9, .3), 'paint.second', 'frame', ch=.05)   # fork crosshead
        for sx in (-1, 1):                       # fork arms up to the trunnions at the drum sides
            xa = px + sx * (rb + .28)
            k.box((xa, py, (pz + .3 + zc + .35) / 2), (.3, .8, zc + .35 - pz - .3), 'paint.second', 'frame', ch=.05)
            with k.at((xa - sx * .2, py, zc), roll=math.pi / 2):
                k.cyl((0, 0, 0), .32, .5, 'paint.trim', 'frame', sides=12)                # trunnion boss
            for dz in (-.25, .25):
                k.box((xa + sx * .17, py + .25, zc + dz), (.06, .1, .1), 'metal', 'detail')
        with k.at((px, py, zc), pitch=-.1):                                                  # lens raised a touch
            with k.at((0, 0, 0), pitch=math.pi / 2):                                         # drum axis along -y
                ip.lathe(k, [(0.0, -2.98), (.55, -2.98), (.68, -2.9), (1.15, -1.9), (1.55, -.6), (rb, .5),
                             (1.55, 1.45), (1.42, 1.62), (0.0, 1.62)], 'paint.main', 'mass', n=28)
                k.cyl((0, 0, 1.7), 1.47, .16, 'paint.trim', 'frame', sides=28)             # lens bezel
                k.cyl((0, 0, 1.79), 1.28, .04, 'lamp', 'frame', sides=28)                  # lens
                k.cyl((0, 0, .5), rb + .03, .12, 'paint.trim', 'frame', sides=28)          # hoop at the trunnions
                for i in range(8):                                                          # vent ring on the back
                    a_ = i * math.pi / 4
                    with k.at((math.cos(a_) * .95, math.sin(a_) * .95, -2.2), yaw=a_):
                        k.box((0, 0, 0), (.08, .34, .5), 'paint.dark', 'detail')
                for i in range(6):                                                          # bezel bolts
                    a_ = i * math.pi / 3
                    k.box((math.cos(a_) * 1.38, math.sin(a_) * 1.38, 1.8), (.1, .1, .06), 'metal', 'detail')
        # cable from the turntable up the fork arm into the drum back
        k.beam((px + .5, py + .3, pz + .6), (px + .35, py + 2.3, zc - .45), .08, 'rubber', 'detail')
    count(k, 'antennas_dishes')


def perimeteralarm(k):
    """Perimeter alarm mast: equipment cabinet, steel pole, caged ladder to a ring platform, and the rotating
    siren head (stock animated node `spinny`): four horns and a red beacon turning about the pole axis."""
    px, py = -1.17, 0.0
    k.box((2.05, -.08, 1.55), (2.4, 2.9, 3.1), 'paint.second', 'mass', ch=.08, top=.06)
    for z in (.9, 2.1):
        k.box((.83, -.08, z), (.06, 2.4, .9), 'paint.trim', 'frame')
    PW.placard(k, (.82, .9, 2.6), math.pi)
    PW.status_light(k, (.82, -1.0, 2.8), 'lamp_red', r=.06)
    k.box((px, py, .3), (1.2, 1.2, .6), 'ferro.main', 'mass', ch=.08)
    k.cyl((px, py, 8.6), .25, 16.0, 'paint.plate', 'mass', sides=14)
    PW.cable_tray(k, [(px + .3, py), (.8, py)], 1.0)
    zp = 15.6
    mast_platform(k, px, py, zp, 1.6, 'siren platform')
    k.cyl((px, py, 16.9), .3, .5, 'paint.plate', 'frame', sides=12)
    with anim_node(k, 'spinny', (px, py, 17.2)):
        k.box((px, py, 17.6), (.8, .8, .8), 'paint.trim', 'frame', ch=.08)
        for a in range(4):
            with k.at((px, py, 17.6), yaw=a * math.pi / 2):
                with k.at((.4, 0, 0), yaw=0.0, pitch=0.0, roll=math.pi / 2):
                    ip.lathe(k, [(.16, 0.0), (.65, 1.6), (.6, 1.66), (.1, .05)], 'paint.valve', 'frame', n=14)
        k.cyl((px, py, 18.25), .22, .5, 'lamp_red', 'frame', sides=12, top=.05)
    count(k, 'antennas_dishes')


# ================================================================== batch 3 rebuild: HQs and controls by intent
# Owner/orchestrator: the massing-built versions read as grey boxes. Each building is rebuilt from the 3-5 features
# that make its stock recognisable (glazed overhanging control cab with balcony, stacked setbacks, arched ribbed
# hangar roofs, colour bands, door types, roof clutter). Faction colour goes on bands and trim; walls are cast.
# stock_massing.json is kept only as a loose envelope reference (the intent check still compares to the stock mesh).


def face(box, s, z=0.0):
    """Context frame on side s of a plan box at height z (local x along the side, outward -Y)."""
    (o, yaw, L) = frame_of(box, s)
    return k_at_face(o, yaw, z), L


def k_at_face(o, yaw, z):
    return (o[0], o[1], z), yaw


def block(k, box, z0, z1, wall='ferro', band='paint.main', trim='paint.second', plinth=True, coping=True, bands=(), ch=.12,
          walk=True):
    """Cast storey block: mass, proud plinth (ground blocks), faction colour bands, plate coping, roof deck."""
    x0, y0, x1, y1 = box
    k.box(((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2), (x1 - x0, y1 - y0, z1 - z0), wall, 'mass', ch=ch)
    if plinth and z0 < .1:          # a low kerb: doors at ground stay usable
        k.box(((x0 + x1) / 2, (y0 + y1) / 2, .12), (x1 - x0 + .3, y1 - y0 + .3, .24), trim, 'mass', ch=ch + .1, top=.06)
    for za, zb in bands:
        k.box(((x0 + x1) / 2, (y0 + y1) / 2, (za + zb) / 2), (x1 - x0 + .12, y1 - y0 + .12, zb - za), band, 'skin', ch=ch + .04)
    if coping:
        k.box(((x0 + x1) / 2, (y0 + y1) / 2, z1 - .15), (x1 - x0 + .3, y1 - y0 + .3, .3), 'paint.plate', 'frame', ch=ch + .12, top=.06)
        k.box(((x0 + x1) / 2, (y0 + y1) / 2, z1 + .01), (x1 - x0 - .4, y1 - y0 - .4, .04), 'gravel', 'roof', walk=walk)


def ribbon(k, box, s, z0, z1, a=None, b=None, mull=1.3, deep=.06):
    """Ribbon window: dark glass strip with a frame, mullions and a sill on side s between local a..b."""
    (o, yaw, L) = frame_of(box, s)
    a = -L / 2 + .6 if a is None else a
    b = L / 2 - .6 if b is None else b
    with k.at((o[0], o[1], 0), yaw=yaw):
        k.slab(a - .12, b + .12, z0 - .12, z1 + .12, -.1, .02, 'paint.trim', 'frame', front=.02)
        k.slab(a, b, z0, z1, -.13, -.09, 'glass', 'glass', pane=True)
        n = max(1, int((b - a) / mull))
        for i in range(1, n):
            x = a + (b - a) * i / n
            k.slab(x - .05, x + .05, z0, z1, -.16, -.1, 'paint.trim', 'frame')
        k.slab(a - .2, b + .2, z0 - .22, z0 - .1, -.25, .02, 'paint.plate', 'frame', front=.02)
    count(k, 'windows')


def glazed_bays(k, box, s, n, w, h, a=None, b=None):
    """Tall dark glazed bays at ground (stock HQ / gate fronts): framed curtain glass with transoms."""
    (o, yaw, L) = frame_of(box, s)
    a = -L / 2 + 1.0 if a is None else a
    b = L / 2 - 1.0 if b is None else b
    with k.at((o[0], o[1], 0), yaw=yaw):
        for i in range(n):
            xc = a + (b - a) * (i + .5) / n
            k.slab(xc - w / 2 - .25, xc + w / 2 + .25, .7, h + .25, -.12, .02, 'paint.trim', 'frame', front=.03)
            k.slab(xc - w / 2, xc + w / 2, .7, h, -.15, -.11, 'glass', 'glass', pane=True)
            for z in (h * .45, h * .75):
                k.slab(xc - w / 2, xc + w / 2, z - .05, z + .05, -.18, -.12, 'paint.trim', 'frame')
            k.slab(xc - .05, xc + .05, .7, h, -.18, -.12, 'paint.trim', 'frame')
            count(k, 'windows')


def doors(k, box, s, kind, xs, w, h, yaw_lamp=True):
    (o, yaw, L) = frame_of(box, s)
    with k.at((o[0], o[1], 0), yaw=yaw):
        for xl in xs:
            if kind == 'rollup':
                ip.rollup_door(k, xl, w, h, -.32)       # slats proud of a solid block face (never buried in the mass)
                I.wall_lamp(k, xl, h + 1.2, yaw)
                count(k, 'doors')
            else:
                blast_door(k, w, h, xc=xl, depth=.45, back=.3, fw=.35, yaw=yaw)


def cab(k, c, w, d, z, h, oct_=False, flare=.45, cap=.9, band='paint.main', balcony=1.1, cap_mat='paint.second', cap_t=.45, roof_mat='gravel'):
    """Control-tower cab: sill band, outward-slanted dark glazing with mullions, overhanging chamfered cap and a
    railed balcony round its base (the stock glazed control cab). A negative `cap` sets the cap back from the glass
    head behind a thin trim ledge, so the glazing is the dominant top feature (traffic control towers)."""
    x, y = c
    ch0 = min(w, d) * (.3 if oct_ else .06)
    r0 = rring(x - w / 2, y - d / 2, x + w / 2, y + d / 2, z + .45, ch0)
    r1 = rring(x - w / 2 - flare, y - d / 2 - flare, x + w / 2 + flare, y + d / 2 + flare, z + h - cap_t, ch0 + flare * .4)
    k.box((x, y, z + .22), (w + .2, d + .2, .45), band, 'mass', ch=ch0 + .05)
    loft(k, [r0, r1], 'glass', 'glass', caps=False, pane=True)
    loft(k, [rring(x - w / 2 + .3, y - d / 2 + .3, x + w / 2 - .3, y + d / 2 - .3, z + .4, ch0),
             rring(x - w / 2 + .3, y - d / 2 + .3, x + w / 2 - .3, y + d / 2 - .3, z + h - .4, ch0)], 'paint.trim', 'mass')
    for (pa, pb) in zip(r0, r1):
        k.beam(pa, pb, .1, 'paint.trim', 'frame', h=.1)
    for i in range(len(r0)):
        a0, a1 = r0[i], r0[(i + 1) % len(r0)]
        b0, b1 = r1[i], r1[(i + 1) % len(r1)]
        L = math.dist(a0, a1)
        for j in range(1, max(1, int(L / 1.4))):
            t = j / max(1, int(L / 1.4))
            k.beam(tuple(a0[q] + (a1[q] - a0[q]) * t for q in range(3)), tuple(b0[q] + (b1[q] - b0[q]) * t for q in range(3)), .07,
                   'paint.trim', 'frame', h=.07)
    k.prism(geo.rect_poly(x - w / 2 - flare - cap, y - d / 2 - flare - cap, x + w / 2 + flare + cap, y + d / 2 + flare + cap,
                          ch0 + flare * .4 + cap * .4), z + h - cap_t, z + h, cap_mat, 'mass', top=min(.15, cap_t / 3), bot=min(.25, cap_t / 2))
    if cap < 0:      # glass head ledge: closes the flared glazing under the set-back cap
        k.prism(geo.rect_poly(x - w / 2 - flare - .08, y - d / 2 - flare - .08, x + w / 2 + flare + .08, y + d / 2 + flare + .08,
                              ch0 + flare * .4), z + h - cap_t - .12, z + h - cap_t + .02, 'paint.trim', 'frame')
    if oct_ and cap < 0:     # octagonal cab with a set-back cap: the roof deck follows the octagon (owner round 6)
        hw, hd = (w + flare) / 2 + cap, (d + flare) / 2 + cap
        k.prism(geo.rect_poly(x - hw, y - hd, x + hw, y + hd, ch0 + flare * .4 + cap * .4), z + h, z + h + .04, roof_mat, 'roof',
                walk=True, lint_skip=True)
    else:
        k.box((x, y, z + h + .02), (w + flare, d + flare, .04), roof_mat, 'roof', walk=True, lint_skip=True)
    if balcony:
        bw, bd = w / 2 + balcony, d / 2 + balcony
        k.prism(geo.rect_poly(x - bw, y - bd, x + bw, y + bd, ch0 + .2), z - .25, z, 'paint.plate', 'frame', walk=True)
        ip.railing(k, [(x - bw + .05, y - bd + .05), (x + bw - .05, y - bd + .05), (x + bw - .05, y + bd - .05),
                       (x - bw + .05, y + bd - .05)], z, closed=True)
        for sx in (-1, 1):
            for sy in (-1, 1):
                ip.angle_brace(k, (x + sx * w * .4, y + sy * d * .4, z - 1.6), (x + sx * (bw - .2), y + sy * (bd - .2), z - .28))
    count(k, 'windows')


def barrel(k, box, axis, z0, rise, rib=3.0, mat='paint.main', ribm='paint.plate', ends='ferro.main', windows=0):
    """Arched ribbed hangar roof (stock Liao): barrel shell on the walls, proud ribs, cast end tympana."""
    x0, y0, x1, y1 = box
    a0, a1, w0, w1 = (x0, x1, y0, y1) if axis == 'x' else (y0, y1, x0, x1)
    half = (w1 - w0) / 2
    wc = (w0 + w1) / 2
    R = (half * half + rise * rise) / (2 * rise)
    zc = z0 + rise - R
    n = 24
    ts = [-half - .25 + (2 * half + .5) * i / n for i in range(n + 1)]
    arc = [(wc + t, zc + math.sqrt(max(0.0, R * R - t * t))) for t in ts]
    sec = [(p, z + .02) for p, z in arc] + [(p, z - .3) for p, z in reversed(arc)]

    def P(a, p, z):
        return (a, p, z) if axis == 'x' else (p, a, z)
    loft(k, [[P(a, p, z) for p, z in sec] for a in (a0 - .25, a1 + .25)], mat, 'roof')
    tymp = [(w0, z0)] + [(p, z) for p, z in arc if w0 <= p <= w1] + [(w1, z0)]
    for a in (a0, a1):
        if axis == 'x':
            k.prism([(p, z) for p, z in tymp], a - .1, a + .1, ends, 'mass', axis='x')
        else:
            k.prism([(p, z) for p, z in tymp], a - .1, a + .1, ends, 'mass', axis='y')
    nr = max(1, round((a1 - a0) / rib))
    for i in range(nr + 1):
        a = a0 + .2 + (a1 - a0 - .4) * i / nr
        rs = [(p, z + .28) for p, z in arc] + [(p, z - .02) for p, z in reversed(arc)]
        loft(k, [[P(aa, p, z) for p, z in rs] for aa in (a - .22, a + .22)], ribm, 'frame')
    for i in range(windows):
        a = a0 + (a1 - a0) * (i + .5) / windows
        for sgn in (-1, 1):
            t = sgn * half * .55
            zz = zc + math.sqrt(R * R - t * t)
            k.box(P(a, wc + t, zz + .15), (1.6, .9, .3) if axis == 'x' else (.9, 1.6, .3), 'paint.trim', 'frame', ch=.05)
            k.box(P(a, wc + t, zz + .3), (1.3, .7, .05) if axis == 'x' else (.7, 1.3, .05), 'glass', 'glass', pane=True)
            count(k, 'windows')


def clutter(k, box, z, n=3, seed=0):
    """Roof clutter: HVAC units with fan grilles, mushroom vents, a hatch and a small tank."""
    x0, y0, x1, y1 = box
    rr = (seed * 7 + 3) % 5
    pts = [(x0 + (x1 - x0) * f, y0 + (y1 - y0) * g) for f, g in ((.25, .3), (.7, .65), (.4, .75), (.75, .25), (.5, .5))]
    for i in range(min(n, len(pts))):
        x, y = pts[(i + rr) % len(pts)]
        kind = (i + seed) % 3
        if kind == 0:
            k.box((x, y, z + .5), (1.8, 1.2, 1.0), 'paint.second', 'frame', ch=.08, top=.05)
            k.cyl((x - .4, y, z + 1.02), .38, .06, 'grate', 'frame', sides=14)
        elif kind == 1:
            ip.mushroom_vent(k, x, y, z)
            ip.mushroom_vent(k, x + .9, y + .4, z)
        else:
            ip.hatch(k, x, y, z)
            k.cyl((x + 1.4, y, z + .6), .45, 1.2, 'paint.plate', 'frame', sides=16, top=.1)


def tower_storeys(k, box, z0, z1, wall, band, storey=3.6, sides='nesw', ribbons=True, a=None, b=None, skip=()):
    """Ribbon windows and thin faction bands per storey on a tower's sides (skip: (side, z) pairs hidden by neighbours)."""
    for z in ip.frange(z0 + 1.2, z1 - 1.8, storey):
        for s_ in sides:
            if any(s_ == q and z < zq for q, zq in skip):
                continue
            if ribbons:
                ribbon(k, box, s_, z, z + 1.0, a, b)
    for z in ip.frange(z0 + storey, z1 - 1.0, storey):
        x0, y0, x1, y1 = box
        k.box(((x0 + x1) / 2, (y0 + y1) / 2, z), (x1 - x0 + .1, y1 - y0 + .1, .22), band, 'skin', ch=.14)


# ------------------------------------------------------------------ HQs
def s_hq(k):
    """Steiner HQ. Features: (1) tall office tower with stacked ribbon storeys and a glazed overhanging control cab with
    a balcony; (2) long central hall with a raised clerestory spine; (3) tall dark glazed bays on the front; (4) a low
    flat-roofed garage block built onto the hall with roll-up doors; (5) blue-grey faction bands, roof clutter and a fist-emblem roof plate."""
    W, B = 'ferro', 'paint.second'
    tw = (-11.9, 8.9, 3.1, 19.7)
    block(k, tw, 0, 16.6, W, B, bands=((.7, 1.1),))
    tower_storeys(k, tw, 0, 16.6, W, B, skip=(('e', 8.0),))
    cab(k, (-4.4, 14.3), 13.2, 9.0, 16.6, 3.4, band=B, balcony=.8)
    helipad(k, -4.4, 14.3, 20.05, 3.2)
    hall = (-3.5, -19.3, 12.1, 8.9)
    block(k, hall, 0, 9.9, W, B, bands=((.7, 1.1), (8.6, 9.1)))
    block(k, (1.3, -15.7, 7.9, 7.1), 9.9, 12.4, W, B, plinth=False)
    for s_ in ('e', 'w'):
        ribbon(k, (1.3, -15.7, 7.9, 7.1), s_, 10.4, 11.7)
    glazed_bays(k, hall, 's', 3, 3.2, 6.2)
    ribbon(k, hall, 's', 7.2, 8.2)
    ribbon(k, hall, 'e', 6.4, 7.6, -12.0, 12.0)
    doors(k, hall, 'e', 'blast', [-6.0], 1.6, 2.5)
    ribbon(k, hall, 'w', 6.4, 7.6, -12.0, 4.0)
    glazed_bays(k, hall, 'e', 2, 2.6, 4.6, 2.0, 12.0)
    ne = (3.1, 8.9, 12.1, 19.7)        # owner round 4: the wing fills the tower/hall corner, flush with both (was a detached block)
    block(k, ne, 0, 7.5, W, B, bands=((.7, 1.1),))
    ribbon(k, ne, 'e', 4.6, 5.6)
    ribbon(k, ne, 'n', 4.6, 5.6)
    doors(k, ne, 'n', 'blast', [1.5], 1.6, 2.5)
    gar = (-11.9, -19.3, -3.5, -3.1)
    # owner round 4: the garage is the stock's flat-roofed low block built onto the hall (was a foreign arched shed),
    # banded like the hall and carrying the fist-emblem roof plate
    block(k, gar, 0, 6.2, W, B, bands=((.7, 1.1), (5.0, 5.4)))
    doors(k, gar, 's', 'rollup', [-2.1, 2.1], 3.0, 3.4)
    doors(k, gar, 'w', 'rollup', [-4.0, 4.0], 3.0, 3.4)
    clutter(k, hall, 9.9, 3, 1)
    clutter(k, ne, 7.5, 2, 2)
    k.cyl((-7.7, -11.2, 6.25), 2.4, .06, 'paint.stencil', 'detail', sides=24)
    with k.at((-7.7, -11.2, 6.3), pitch=-math.pi / 2):
        stencil(k, '1', 0, 0, 1.6, 0.0)
    antenna(k, -10.8, 18.8, 16.6, 3.0)


def chamfer_tower(k, c, w, d, z0, z1, ch, wall, band, storey=3.6, flare_top=0.0, ribbons=True, skip=()):
    """Chamfered tower (stock Liao/generic HQ): cast mass with large corner chamfers, a band per storey, ribbon windows
    on the four long faces, optional flare at the head."""
    x, y = c
    rings = [rring(x - w / 2, y - d / 2, x + w / 2, y + d / 2, z0, ch), rring(x - w / 2, y - d / 2, x + w / 2, y + d / 2, z1 - (1.2 if flare_top else 0), ch)]
    if flare_top:
        rings.append(rring(x - w / 2 - flare_top, y - d / 2 - flare_top, x + w / 2 + flare_top, y + d / 2 + flare_top, z1, ch + flare_top * .4))
    loft(k, rings, wall, 'mass')
    box = (x - w / 2, y - d / 2, x + w / 2, y + d / 2)
    for z in ip.frange(z0 + storey, z1 - 1.6, storey):
        loft(k, [rring(x - w / 2 - .08, y - d / 2 - .08, x + w / 2 + .08, y + d / 2 + .08, z - .12, ch + .03),
                 rring(x - w / 2 - .08, y - d / 2 - .08, x + w / 2 + .08, y + d / 2 + .08, z + .12, ch + .03)], band, 'skin')
    if ribbons:
        for z in ip.frange(z0 + 1.3, z1 - 2.6, storey):
            for s_ in 'nesw':
                if any(s_ == q and z < zq for q, zq in skip):
                    continue
                L = (w if s_ in 'sn' else d) - 2 * ch
                ribbon(k, box, s_, z, z + 1.0, -L / 2 + .4, L / 2 - .4)
    return box


def l_hq(k):
    """Liao HQ. Features: (1) pale chamfered tower with a dark window band per storey and a flared glazed head with a
    helipad; (2) the long green ribbed arched hangar roof; (3) tan lower walls with doors and windows under the arch;
    (4) green faction bands; (5) roof clutter and masts."""
    W, B = 'ferro', 'paint.main'
    box = chamfer_tower(k, (-6.6, 12.2), 12.6, 13.0, 0, 18.6, 2.2, 'paint.coping', B, skip=(('e', 13.0),))
    cab(k, (-6.6, 12.2), 10.4, 10.8, 18.6, 3.2, oct_=True, band=B, balcony=0, cap_mat='paint.main')
    helipad(k, -6.6, 12.2, 21.85, 3.6)
    block(k, (-11.1, -18.9, 0.9, 5.9), 0, 9.7, W, B, bands=((.7, 1.1), (8.7, 9.2)))
    for z in (2.0, 5.6):
        ribbon(k, (-11.1, -18.9, 0.9, 5.9), 'w', z, z + 1.0, -10.0, 10.0)
    doors(k, (-11.1, -18.9, 0.9, 5.9), 's', 'blast', [0.0], 1.6, 2.5)
    clutter(k, (-11.1, -18.9, 0.9, 5.9), 9.7, 3, 3)
    hb = (0.9, -18.9, 12.9, 16.5)
    block(k, hb, 0, 8.4, 'paint.second', B, bands=((.7, 1.1),), coping=False)
    barrel(k, hb, 'y', 8.4, 4.3, rib=3.6, mat='paint.main', ribm='paint.plate', ends='paint.second', windows=4)
    doors(k, hb, 'e', 'blast', [-12.0, 0.0, 12.0], 1.6, 2.5)
    for xl in (-6.0, 6.0, -15.0, 15.0):
        ribbon(k, hb, 'e', 3.4, 4.6, xl - 1.6, xl + 1.6)
    doors(k, hb, 's', 'rollup', [0.0], 4.0, 4.4)
    antenna(k, -11.2, 17.8, 18.6, 2.6)


def generic_hq(k):
    """Generic HQ. Features: (1) chamfered blue-grey tower with dark storey bands and a flared glazed head with a
    helipad; (2) the long sloped riveted hall stepping down to the east; (3) the stepped west wing with a sloped roof;
    (4) riveted plate panels and bands; (5) doors and roof clutter."""
    W, B = 'ferro', 'paint.main'
    chamfer_tower(k, (-6.6, 12.2), 12.6, 13.0, 0, 18.6, 2.2, 'paint.coping', B, skip=(('e', 13.5), ('s', 10.5)))
    cab(k, (-6.6, 12.2), 10.4, 10.8, 18.6, 3.2, oct_=True, band=B, balcony=0, cap_mat='paint.main')
    helipad(k, -6.6, 12.2, 21.85, 3.6)
    hb = (0.3, -18.9, 12.9, 16.5)
    k.prism([(0.3, 0), (12.9, 0), (12.9, 7.0), (11.1, 8.6), (8.7, 11.6), (4.2, 12.9), (0.3, 12.9)], -18.9, 16.5, W, 'mass', axis='y')
    k.prism([(0.1, 12.6), (4.2, 12.6), (8.7, 11.3), (11.1, 8.3), (13.1, 6.7), (13.1, 7.15), (11.2, 8.9), (8.8, 11.95), (4.25, 13.2), (0.1, 13.2)],
            -19.1, 16.7, 'paint.roof', 'roof', axis='y', lint_skip=True)
    for y in ip.frange(-17.0, 15.0, 4.0):
        k.prism([(0.1, .3), (13.1, .3), (13.1, 7.3), (11.2, 9.0), (8.8, 12.1), (4.25, 13.35), (0.1, 13.35)], y - .25, y + .25, 'paint.plate', 'frame', axis='y')
    k.box((6.6, -1.2, .35), (12.9, 35.7, .7), 'paint.second', 'mass', ch=.15)
    for xl in (-12.0, -4.0, 4.0, 12.0):
        ribbon(k, hb, 'e', 3.2, 4.4, xl - 1.4, xl + 1.4)
    doors(k, hb, 'e', 'rollup', [-8.0, 8.0], 3.6, 4.0)
    doors(k, hb, 's', 'blast', [0.0], 1.6, 2.5)
    wb = (-12.3, -18.9, -0.3, 5.7)
    k.prism([(-12.3, 0), (-0.3, 0), (-0.3, 9.8), (-4.5, 9.8), (-9.3, 9.2), (-12.3, 6.4)], -18.9, 5.7, W, 'mass', axis='y')
    k.prism([(-12.5, 6.2), (-9.3, 9.0), (-4.5, 9.6), (-0.1, 9.6), (-0.1, 10.0), (-4.5, 10.0), (-9.3, 9.4), (-12.5, 6.6)], -19.1, 5.9,
            'paint.roof', 'roof', axis='y', lint_skip=True)
    k.box((-6.3, -6.6, .35), (12.3, 24.9, .7), 'paint.second', 'mass', ch=.15)
    for z in (2.0, 5.4):
        ribbon(k, wb, 's', z, z + 1.0)
    ribbon(k, wb, 'w', 2.0, 3.0, -10.0, 10.0)
    doors(k, wb, 'w', 'rollup', [-4.0, 4.0], 3.0, 3.4)
    clutter(k, (-9.0, -16.0, -1.0, 2.0), 9.8, 2, 4)
    antenna(k, -11.2, 17.8, 18.6, 2.6)


def d_hq(k):
    """Davion HQ. Features: (1) tower with a tan glazed cab band and helipad over pale stacked storeys; (2) the tan front
    blocks with garage doors and repeated pilasters; (3) the sloped stair-ramp shoulder stepping down west; (4) tan-yellow
    faction bands on pale cast walls; (5) roof clutter."""
    W, B = 'ferro', 'paint.main'
    tw = (-3.7, 9.0, 12.5, 19.2)
    block(k, tw, 0, 18.4, W, B, bands=((.7, 1.1),))
    tower_storeys(k, tw, 0, 18.4, W, B, sides='new', skip=(('s', 13.0),))
    cab(k, (4.4, 14.1), 15.4, 9.4, 18.4, 3.4, band=B, balcony=.7, cap_mat='paint.main')
    helipad(k, 4.4, 14.1, 21.85, 3.4)
    mid = (-3.1, -19.8, 3.5, 9.0)
    block(k, mid, 0, 12.6, W, B, bands=((.7, 1.1), (11.6, 12.2)))
    ribbon(k, mid, 's', 9.0, 10.2)
    east = (3.5, -19.8, 11.9, 9.0)
    block(k, east, 0, 9.6, W, B, bands=((.7, 1.1), (5.4, 9.6)))
    for xl in (-10.0, -2.0, 6.0):
        doors(k, east, 'e', 'rollup', [xl], 3.6, 3.8)
    for xl in (-12.0, -6.0, 0.0, 6.0, 12.0):
        (o, yaw, L) = frame_of(east, 'e')
        with k.at((o[0], o[1], 0), yaw=yaw):
            k.slab(xl + 2.7, xl + 3.3, 0.7, 9.6, -.22, .02, 'paint.plate', 'frame', front=.05)
    ribbon(k, east, 's', 6.4, 7.6)
    ribbon(k, east, 'e', 6.4, 7.6, -13.0, 13.0)
    west = (-11.5, -10.2, -3.1, 10.2)
    k.prism([(-11.5, 0), (-3.1, 0), (-3.1, 9.2), (-4.3, 9.2), (-7.9, 8.7), (-11.5, 7.0)], -10.2, 9.0, W, 'mass', axis='y')
    k.prism([(-11.7, 6.8), (-7.9, 8.5), (-4.3, 9.0), (-3.0, 9.0), (-3.0, 9.4), (-4.3, 9.4), (-7.9, 8.9), (-11.7, 7.2)], -10.4, 9.2,
            'paint.main', 'roof', axis='y', lint_skip=True)
    ribbon(k, west, 'w', 2.4, 3.4, -8.0, 8.0)
    doors(k, west, 'w', 'blast', [0.0], 1.6, 2.5)
    sw = (-12.1, -18.6, -3.1, -10.2)
    block(k, sw, 0, 9.4, W, B, bands=((.7, 1.1), (8.4, 9.0)))
    doors(k, sw, 's', 'rollup', [0.0], 4.0, 4.2)
    ribbon(k, sw, 'w', 5.8, 6.8)

    clutter(k, mid, 12.6, 2, 5)
    clutter(k, east, 9.6, 3, 6)


# ------------------------------------------------------------------ gate controls
def s_gatecontrol(k):
    """Steiner gate control. Features: (1) long two-storey gate block with a balcony band and an upper ribbon of windows;
    (2) tall dark glazed bays at ground (the gatehouse front); (3) the tower with a dark overhanging control cab and the
    rotating radar (stock Base01); (4) blue-grey bands, roof clutter."""
    W, B = 'ferro', 'paint.second'
    gb = (-13.9, -7.3, 3.5, 7.7)
    block(k, gb, 0, 7.0, W, B, bands=((.7, 1.1), (3.6, 4.0)))
    block(k, (-13.9, -0.7, 3.5, 4.7), 7.0, 9.2, W, B, plinth=False)
    for s_ in 'sn':
        glazed_bays(k, gb, s_, 3, 3.4, 3.2)
        ribbon(k, gb, s_, 4.6, 6.0)
        (o, yaw, L) = frame_of(gb, s_)
        with k.at((o[0], o[1], 0), yaw=yaw):
            k.slab(-L / 2, L / 2, 3.95, 4.15, -.8, .02, 'paint.plate', 'frame', walk=True)
            for xb in ip.frange(-L / 2 + .6, L / 2 - .5, 2.9):
                k.prism([(-.75, 3.95), (0.0, 3.95), (0.0, 3.0)], xb - .08, xb + .08, ip.STEEL, 'frame', axis='x')
        y = -7.3 - .75 if s_ == 's' else 7.7 + .75
        ip.railing(k, [(-13.85, y), (3.45, y)], 4.15)
    doors(k, gb, 'w', 'blast', [0.0], 1.8, 2.6)
    tw = (3.5, -6.1, 14.3, 7.1)
    block(k, tw, 0, 14.6, W, B, bands=((.7, 1.1),))
    tower_storeys(k, tw, 0, 14.6, W, B, sides='nes', skip=(('w', 9.5),))
    cab(k, (8.9, 0.5), 9.6, 11.0, 14.6, 3.0, band=B, balcony=0, cap_mat='paint.trim', cap=1.1)
    radar_dipole(k, 'Base01', (8.9, 0.5, 18.25), L=3.6)
    clutter(k, gb, 7.0, 2, 7)


def d_gatecontrol(k):
    """Davion gate control. Features: (1) central tower: a battered tan base under a pale cab storey with a window band
    and the rotating dish (stock Dish); (2) twin side blocks with horizontally ribbed tan upper walls; (3) a garage
    door and a long low east wing with a ribbon of windows; (4) the steel front stair to the gate roof; (5) tan-yellow
    bands on pale cast walls."""
    W, B = 'ferro', 'paint.main'
    loft(k, [rring(-5.7, -1.0, 3.1, 6.4, 0, .3), rring(-4.9, -.2, 2.3, 5.6, 12.0, .3)], B, 'mass')
    for z in ip.frange(1.5, 11.0, 1.6):
        f = z / 12.0
        k.box((-1.3, 2.7, z), (8.8 - 1.6 * f + .1, 7.4 - 1.6 * f + .1, .12), 'paint.plate', 'skin', ch=.3)
    block(k, (-4.9, -.2, 2.3, 5.6), 12.0, 14.0, W, B, plinth=False)
    for s_ in 'nesw':
        ribbon(k, (-4.9, -.2, 2.3, 5.6), s_, 12.4, 13.5)
    cab(k, (-1.3, 2.7), 6.4, 5.0, 14.0, 2.4, band=B, balcony=.9, cap_mat='paint.second')
    spin_dish(k, 'Dish', -2.27, 3.6, 16.45, 1.2)
    for box in ((-15.1, -4.6, -6.1, 8.6), (-6.1, -4.6, 4.1, -1.0)):
        block(k, box, 0, 4.2, W, B, bands=((.7, 1.1),), coping=False)
        block(k, box, 4.2, 9.0 if box[0] < -10 else 8.7, 'paint.main', B, plinth=False)
        x0, y0, x1, y1 = box
        for z in ip.frange(4.6, 8.4, .45):
            k.box(((x0 + x1) / 2, (y0 + y1) / 2, z), (x1 - x0 + .12, y1 - y0 + .12, .1), 'paint.second', 'skin', ch=.08)
    doors(k, (-15.1, -4.6, -6.1, 8.6), 's', 'rollup', [-1.0], 4.0, 3.6)
    ribbon(k, (-15.1, -4.6, -6.1, 8.6), 'w', 1.6, 2.6, -5.0, 5.0)
    ew = (4.1, -3.4, 15.5, 4.4)
    block(k, ew, 0, 4.1, W, B, bands=((.7, 1.1),))
    for s_ in 'sne':
        ribbon(k, ew, s_, 1.8, 3.0)
    doors(k, ew, 'e', 'blast', [0.0], 1.6, 2.4)
    clutter(k, (-15.1, -4.6, -6.1, 8.6), 9.0, 2, 8)


def l_gatecontrol(k):
    """Liao gate control. Features: (1) the pale tower with a dark rounded overhanging cab and the rotating roof dish
    (stock Dish); (2) the green ribbed arched annex roof over tan walls; (3) tall framed windows and a door in the annex
    front; (4) green faction bands; (5) clutter."""
    W, B = 'ferro', 'paint.main'
    box = chamfer_tower(k, (-7.5, 3.3), 11.0, 9.6, 0, 12.4, 1.6, 'paint.coping', B, skip=(('e', 5.0),))
    cab(k, (-7.5, 3.3), 9.4, 8.2, 12.4, 2.6, oct_=True, band=B, balcony=0, cap_mat='paint.trim', cap=1.0)
    spin_dish(k, 'Dish', -10.24, 1.03, 15.0, 1.1)
    doors(k, box, 's', 'blast', [0.0], 1.6, 2.5)
    an = (0.3, -8.1, 13.5, 8.7)
    block(k, an, 0, 6.6, 'paint.second', B, bands=((.7, 1.1),), coping=False)
    barrel(k, an, 'x', 6.6, 4.3, rib=3.2, mat='paint.main', ends='paint.second', windows=2)
    for s_ in 'sn':
        (o, yaw, L) = frame_of(an, s_)
        with k.at((o[0], o[1], 0), yaw=yaw):
            for xl in (-4.0, -1.5, 1.5, 4.0):
                k.slab(xl - .6, xl + .6, 2.0, 4.6, -.12, .02, 'paint.trim', 'frame', front=.02)
                k.slab(xl - .45, xl + .45, 2.15, 4.45, -.15, -.11, 'glass', 'glass', pane=True)
                count(k, 'windows')
    doors(k, an, 'e', 'blast', [0.0], 1.6, 2.5)
    k.box((-0.6, 3.6, 2.35), (1.8, 6.6, 4.7), 'paint.second', 'mass', ch=.1)


# ------------------------------------------------------------------ sensor controls
def sensor_control2(k, name, B, array_mat='paint.trim', tw=(-9.9, -0.3, 2.7, 8.1), twin=False, ax1=9.9):
    """Sensor control. Features: (1) the tall dark sensor-array slab with a grid of panels and lit emitter strips;
    (2) the pale control tower beside it with ribbon storeys and a glazed cab near the top; (3) the rotating sensor dish
    on a stand on top (stock L_Dish, and L_Sdish on the Liao); (4) the low annex with a garage door; (5) faction bands."""
    W = 'ferro'
    sl = (-6.9, 2.1, -2.1, 5.7)
    k.box(((sl[0] + sl[2]) / 2, (sl[1] + sl[3]) / 2, 14.25), (sl[2] - sl[0], sl[3] - sl[1], 28.5), array_mat, 'mass', ch=.1)
    sensor_array(k, sl, 28.5, 'ns')
    block(k, (-9.9, -0.3, -6.9, 8.1), 0, 25.9, W, B, bands=((.7, 1.1),))
    block(k, (0.3, -0.3, 2.7, 8.1), 0, 25.9, W, B, bands=((.7, 1.1),))
    for bx in ((-9.9, -0.3, -6.9, 8.1), (0.3, -0.3, 2.7, 8.1)):
        for z in ip.frange(3.6, 24.0, 3.6):
            k.box(((bx[0] + bx[2]) / 2, (bx[1] + bx[3]) / 2, z), (bx[2] - bx[0] + .1, bx[3] - bx[1] + .1, .22), B, 'skin', ch=.14)
        ribbon(k, bx, 'w' if bx[0] < -5 else 'e', 21.2, 23.6, -3.4, 3.4)
    cab(k, (-3.6, 3.9), 6.0, 8.8, 25.9, 2.2, band=B, balcony=0, cap=.6)
    an = (3.3, -8.1, ax1, 8.7)
    block(k, an, 0, 11.0, W, B, bands=((.7, 1.1), (10.0, 10.6)))
    for s_ in 'es':
        for z in (2.6, 6.2):
            ribbon(k, an, s_, z, z + 1.0)
    doors(k, an, 'e', 'rollup', [4.0], 3.6, 3.6)
    lo = (-4.5, -8.1, 3.3, -0.3)
    block(k, lo, 0, 9.5, W, B, bands=((.7, 1.1),))
    ribbon(k, lo, 's', 5.4, 6.6)
    doors(k, lo, 's', 'blast', [0.0], 1.6, 2.5)
    clutter(k, an, 11.0, 2, 9)
    if twin:
        x, y, z = -2.69, 3.94, 29.0
        k.cyl((x, y, 28.75), .5, .5, 'paint.plate', 'frame', sides=14, top=.05)
        with anim_node(k, 'L_Dish', (x, y, z)):
            k.cyl((x, y, z + .15), .9, .3, 'paint.trim', 'frame', sides=18, top=.05)
            for s_ in (-1, 1):
                k.box((x + s_ * .9, y, z + 1.0), (.2, .4, 1.7), 'paint.plate', 'frame', ch=.04)
        with anim_node(k, 'L_Sdish', (x - .76, y, z + 1.8), parent='L_Dish', axis=(0.0, -1.0, 0.0)):
            with k.at((x - .76, y, z + 1.8), yaw=-math.pi / 2, pitch=-.35):
                ip.lathe(k, [(0.0, 0.0), (1.0, .07), (1.9, .52), (1.86, .62), (1.0, .25), (0.0, .08)], 'paint.second', 'frame', n=32)
                k.cyl((0, 0, 1.4), .12, .3, 'paint.trim', 'frame', sides=10)
        count(k, 'antennas_dishes')
    else:
        spin_dish(k, 'L_Dish', -0.85, 3.95, 28.6, 2.4)


def s_sensorcontrol(k):
    sensor_control2(k, 's_sensorcontrol', 'paint.second')


def d_sensorcontrol(k):
    sensor_control2(k, 'd_sensorcontrol', 'paint.main')


def l_sensorcontrol(k):
    sensor_control2(k, 'l_sensorcontrol', 'paint.main', twin=True, ax1=8.2)


# ------------------------------------------------------------------ spotlight controls
def compound_wall(k, runs, gate, h=1.5):
    """Low cast compound wall (owner round 4: a pseudo-defensive yard) along plan runs, with gate piers and lamps
    either side of the opening `gate` = ((x, y), (x, y))."""
    for r in runs:
        blast_wall(k, r, h=h, base=.8, top=.4)
    for (x, y) in gate:
        k.box((x, y, (h + .5) / 2), (.9, .9, h + .5), 'paint.plate', 'mass', ch=.1, top=.06)
        k.cyl((x, y, h + .62), .12, .24, 'lamp', 'frame', sides=10)


def spot_tower(k, c, z0, ztop, w, cab_w, cab_h, light, name, oct_=False, B='paint.second', W='ferro'):
    """Small control tower (owner round 4: read as a small air-traffic control): a slim shaft with ribbon storeys
    rising from the operations block, a glazed overhanging cab with a balcony, the searchlight on the cab roof."""
    x, y = c
    zc = light[2] - 1.45 - cab_h
    if oct_:
        chamfer_tower(k, c, w, w, z0, zc, w * .22, 'paint.coping', B, storey=3.0)
    else:
        sh = (x - w / 2, y - w / 2, x + w / 2, y + w / 2)
        block(k, sh, z0, zc, W, B, plinth=False, coping=False)
        tower_storeys(k, sh, z0, zc, W, B, storey=3.0)
    cab(k, c, cab_w, cab_w, zc, cab_h, oct_=oct_, flare=.7, cap=-.4, cap_t=.25, band=B, balcony=.9,
        cap_mat='paint.trim', roof_mat='paint.trim')
    k.box((light[0], light[1], zc + cab_h + .6), (2.2, 2.2, 1.2), 'paint.trim', 'mass', ch=.2, top=.1)
    searchlight(k, name, light, r=.8)


def s_spotlightcontrol(k):
    """Steiner spotlight control (owner round 4: a small air-traffic-control building in a walled yard). Features:
    (1) a slim control tower with ribbon storeys and a glazed overhanging cab with a balcony; (2) the searchlight on the
    cab roof (stock Light, rotating); (3) the two-storey operations block and a garage wing, their doors facing the
    yard; (4) a low cast compound wall closing the SE yard, its chamfered corner carrying the gate; (5) blue-grey bands and roof clutter."""
    W, B = 'ferro', 'paint.second'
    # owner round 6: plan matched to the stock top-down footprint (topdown.py): ops block and tower NW, garage wing
    # east, entrance block south, the yard in the SE corner behind a wall with a chamfered corner
    ops = (-12.0, -2.0, 3.0, 12.0)
    block(k, ops, 0, 6.0, W, B, bands=((.7, 1.1),))
    ribbon(k, ops, 'w', 3.8, 4.9, -6.0, 6.0)
    ribbon(k, ops, 'n', 3.8, 4.9, -6.0, 6.0)
    ribbon(k, ops, 's', 3.8, 4.9, 4.8, 7.0)
    doors(k, ops, 's', 'blast', [6.0], 1.6, 2.5)
    spot_tower(k, (-8.3, 6.9), 6.0, 16.85, 5.0, 7.2, 3.0, (-8.3, 6.9, 16.85), 'Light')
    gar = (3.0, 1.0, 12.0, 8.0)
    block(k, gar, 0, 4.7, W, B, bands=((.7, 1.1),))
    doors(k, gar, 's', 'rollup', [-1.2], 3.6, 3.4)
    ribbon(k, gar, 'e', 2.0, 3.0, -2.4, 2.4)
    clutter(k, gar, 4.7, 2, 10)
    ent = (-8.0, -11.0, 0.0, -2.0)
    block(k, ent, 0, 4.7, W, B, bands=((.7, 1.1),))
    doors(k, ent, 'e', 'blast', [0.0], 1.6, 2.5)
    ribbon(k, ent, 'w', 2.0, 3.0, -3.0, 3.0)
    ribbon(k, ent, 's', 2.0, 3.0, -2.6, 2.6)
    clutter(k, (-3.0, 2.0, 3.0, 11.0), 6.0, 2, 4)
    compound_wall(k, [[(9.0, 1.0), (9.0, -5.0), (8.1, -5.9)], [(6.1, -7.9), (5.0, -9.0), (0.0, -9.0)]],
                  ((8.1, -5.9), (6.1, -7.9)))


def l_spotlightcontrol(k):
    """Liao spotlight control (owner round 4: a small air-traffic-control building in a walled yard). Features:
    (1) a pale octagonal control tower with a band per storey and a glazed octagonal cab with a balcony; (2) the
    searchlight on the cab roof (stock light, rotating); (3) the operations block and the green ribbed arched annex,
    their doors facing the yard; (4) a low cast compound wall closing the SE yard, its chamfered corner carrying the gate; (5) green bands."""
    W, B = 'ferro', 'paint.main'
    # owner round 6: plan matched to the stock top-down footprint (topdown.py): ops block and tower NW, arched annex
    # east, arched entrance wing south, the yard in the SE corner behind a wall with a chamfered corner
    ops = (-10.5, -0.5, 1.0, 9.5)
    block(k, ops, 0, 4.8, 'paint.second', B, bands=((.7, 1.1),))
    for s_ in 'wn':
        ribbon(k, ops, s_, 2.6, 3.6, -3.6, 3.6)
    doors(k, ops, 's', 'blast', [4.0], 1.6, 2.4)
    spot_tower(k, (-6.65, 6.2), 4.8, 14.55, 5.2, 7.0, 2.7, (-6.65, 6.2, 14.55), 'light', oct_=True, B=B)
    an = (1.0, 3.0, 10.0, 8.0)
    block(k, an, 0, 3.4, 'paint.second', B, bands=((.7, 1.1),), coping=False)
    barrel(k, an, 'x', 3.4, 1.6, rib=2.6, mat='paint.main', ends='paint.second')
    doors(k, an, 's', 'blast', [0.0], 1.6, 2.4)
    ent = (-7.0, -10.0, -2.0, -0.5)
    block(k, ent, 0, 3.4, 'paint.second', B, bands=((.7, 1.1),), coping=False)
    barrel(k, ent, 'y', 3.4, 1.4, rib=2.6, mat='paint.main', ends='paint.second')
    doors(k, ent, 'e', 'blast', [0.0], 1.6, 2.4)
    compound_wall(k, [[(10.0, 3.0), (10.0, -4.0), (8.9, -5.1)], [(6.9, -7.1), (4.0, -10.0), (-2.0, -10.0)]],
                  ((8.9, -5.1), (6.9, -7.1)))


# ================================================================== batch 4 (by intent): traffic control, terminals, HPG, sheds
def emblem(k, p, yaw, r, ring='paint.stencil', disc='paint.second', rays=8):
    """Wall emblem (ComStar-style star roundel / unit plate) on a face whose outward direction is yaw-90deg."""
    with k.at(p, yaw=yaw, pitch=math.pi / 2):
        k.cyl((0, 0, 0), r, .1, disc, 'frame', sides=24, top=.03)
        k.cyl((0, 0, .06), r * .84, .03, ring, 'detail', sides=24)
        k.cyl((0, 0, .07), r * .74, .03, disc, 'detail', sides=24)
        k.cyl((0, 0, .09), r * .22, .04, ring, 'detail', sides=12)
        for i in range(rays):
            with k.at((0, 0, .09), yaw=i * math.pi / rays * 2):
                k.box((r * .45, 0, 0), (r * .5, r * .07, .03), ring, 'detail')


def batter_block(k, box, z0, z1, bat, mat='paint.main', cap='paint.second', ch=.3, slit=None, sides='nesw'):
    """HPG battered tier: sloped tan walls, a dark slit band, a capping slab with a chamfered head."""
    x0, y0, x1, y1 = box
    loft(k, [rring(x0, y0, x1, y1, z0, ch), rring(x0 + bat, y0 + bat, x1 - bat, y1 - bat, z1 - .4, ch),
             rring(x0 + bat - .25, y0 + bat - .25, x1 - bat + .25, y1 - bat + .25, z1 - .4, ch + .1),
             rring(x0 + bat - .25, y0 + bat - .25, x1 - bat + .25, y1 - bat + .25, z1, ch + .1)], mat, 'mass')
    k.box(((x0 + x1) / 2, (y0 + y1) / 2, z1 + .01), (x1 - x0 - 2 * bat - .2, y1 - y0 - 2 * bat - .2, .04), 'gravel', 'roof', walk=True,
          lint_skip=True)
    k.box(((x0 + x1) / 2, (y0 + y1) / 2, z0 + .3), (x1 - x0 + .25, y1 - y0 + .25, .6), cap, 'mass', ch=ch + .1)
    if slit:
        zs = slit
        f = (zs - z0) / max(.1, (z1 - .4 - z0))
        ins = bat * f
        for s_ in sides:
            bx = (x0 + ins, y0 + ins, x1 - ins, y1 - ins)
            (o, yaw, L) = frame_of(bx, s_)
            with k.at((o[0], o[1], zs), yaw=yaw, pitch=-math.atan2(bat, z1 - .4 - z0)):
                n = max(1, int((L - 2.0) / 3.2))
                for i in range(n):
                    xc = -L / 2 + 1.0 + (L - 2.0) * (i + .5) / n
                    k.slab(xc - 1.2, xc + 1.2, -.25, .25, -.06, .02, 'paint.trim', 'frame', front=.02)
                    k.slab(xc - 1.05, xc + 1.05, -.15, .15, -.08, -.05, 'glass', 'glass', pane=True)
                count(k, 'windows')


def hpg_corridor(k, arms, hub=7.8, arm_len=None, H=8.3, hubH=9.3, c=(0.0, 0.0)):
    with k.at((c[0], c[1], 0)):
        _hpg_corridor(k, arms, hub, H, hubH)


def _hpg_corridor(k, arms, hub, H, hubH):
    """HPG modular corridor: a battered chamfered hub (lantern roof grate) with battered corridor arms (dark slit
    band, roof walkway grate, end portals). arms: list of (dir, length) with dir in 'nesw'."""
    batter_block(k, (-hub, -hub, hub, hub), 0, hubH, 1.6, ch=1.4, slit=None)
    k.box((0, 0, hubH + .25), (5.0, 5.0, .5), 'paint.second', 'frame', ch=.8, top=.2)
    k.box((0, 0, hubH + .52), (4.2, 4.2, .06), 'grate', 'frame')
    for d, L in arms:
        yaw = {'s': 0.0, 'e': math.pi / 2, 'n': math.pi, 'w': -math.pi / 2}[d]
        with k.at((0, 0, 0), yaw=yaw):
            # arm runs along local -y from the hub edge outward
            prof = [(-7.6, 0), (7.6, 0), (7.6, .6), (5.4, H - .4), (5.6, H - .4), (5.6, H), (-5.6, H), (-5.6, H - .4), (-5.4, H - .4), (-7.6, .6)]
            k.prism(prof, -hub - L, -hub + .5, 'paint.main', 'mass', axis='y')
            k.prism([(-7.8, 0), (7.8, 0), (7.8, .6), (-7.8, .6)], -hub - L, -hub + .5, 'paint.second', 'mass', axis='y')
            k.box((0, -hub - L / 2, H + .03), (6.0, L, .06), 'grate', 'frame', walk=True)
            for sgn in (-1, 1):
                for i in range(max(1, int(L / 4.0))):
                    yc = -hub - (i + .5) * L / max(1, int(L / 4.0))
                    with k.at((sgn * 6.5, yc, 4.6), yaw=(math.pi / 2 if sgn > 0 else -math.pi / 2), pitch=-math.atan2(2.2, H - 1.0)):
                        k.slab(-1.3, 1.3, -.3, .3, -.06, .02, 'paint.trim', 'frame', front=.02)
                        k.slab(-1.15, 1.15, -.18, .18, -.08, -.05, 'glass', 'glass', pane=True)
                    count(k, 'windows')
            k.slab(-4.0, 4.0, 0, H - 1.0, -hub - L - .05, -hub - L + .25, 'paint.second', 'frame', front=.05)
    count(k, 'panels_sections', len(arms) + 1)


def hpg_threeway(k):
    hpg_corridor(k, [('s', 8.2), ('n', 8.2), ('e', 8.2)], c=(-2.8, 0.0))


def hpg_corner(k):
    hpg_corridor(k, [('n', 8.6), ('e', 8.4)], c=(-2.9, -3.0))


def hpg_end(k):
    hpg_corridor(k, [('e', 8.4)], c=(-2.9, 0.0))
    emblem(k, (-10.75, 0, 5.0), -math.pi / 2, 2.2)
    k.box((-2.9, 9.2, 2.6), (14.0, 1.2, 5.2), 'paint.second', 'mass', ch=.2, top=.15)


def hpg_fourway(k):
    hpg_corridor(k, [('s', 8.5), ('n', 8.5), ('e', 8.5), ('w', 8.5)])


def hpg_walkway(k):
    prof = [(-7.6, 0), (7.6, 0), (7.6, .6), (5.4, 7.9), (5.6, 7.9), (5.6, 8.3), (-5.6, 8.3), (-5.6, 7.9), (-5.4, 7.9), (-7.6, .6)]
    k.prism(prof, -16.0, 16.0, 'paint.main', 'mass', axis='x')
    k.prism([(-7.8, 0), (7.8, 0), (7.8, .6), (-7.8, .6)], -16.0, 16.0, 'paint.second', 'mass', axis='x')
    k.box((0, 0, 8.33), (32.0, 6.0, .06), 'grate', 'frame', walk=True)
    for sgn in (-1, 1):
        for i in range(8):
            xc = -14.0 + 4.0 * i
            with k.at((xc, sgn * 6.5, 4.6), yaw=(0.0 if sgn < 0 else math.pi), pitch=-math.atan2(2.2, 7.3)):
                k.slab(-1.3, 1.3, -.3, .3, -.06, .02, 'paint.trim', 'frame', front=.02)
                k.slab(-1.15, 1.15, -.18, .18, -.08, -.05, 'glass', 'glass', pane=True)
            count(k, 'windows')
        for x in (-16.0, 16.0):
            k.box((x, sgn * 6.4, 4.0), (.5, 1.4, 8.0), 'paint.second', 'frame', ch=.1)
    for x in (-8.0, 8.0):
        ip.hatch(k, x, 0, 8.36)
    count(k, 'panels_sections')


def hpg_office(k):
    """HPG office. Features: (1) three stacked battered tan tiers stepping up to a 20 m crown; (2) the tall ComStar
    emblem slab on the west face; (3) dark slit bands on each tier; (4) the low east annex; (5) doors at the foot."""
    batter_block(k, (-13.0, -10.0, 8.4, 10.0), 0, 7.6, 2.0, slit=4.6, sides='nes')
    batter_block(k, (-9.6, -7.5, 6.3, 7.5), 7.6, 14.4, 1.5, slit=10.8)
    batter_block(k, (-8.7, -4.2, 2.4, 4.6), 14.4, 19.9, 1.0, slit=16.8)
    k.box((-11.9, 0.2, 9.0), (1.2, 4.6, 14.0), 'paint.second', 'mass', ch=.15, top=.1)
    k.box((-12.55, 0.2, 9.0), (.12, 3.6, 12.6), 'paint.trim', 'frame')
    emblem(k, (-12.62, 0.2, 10.5), -math.pi / 2, 1.6)
    for z in (5.0, 13.5, 16.0):
        k.box((-12.62, 0.2, z), (.06, 3.0, .12), 'lamp_cool', 'frame')
    batter_block(k, (8.2, -6.0, 14.6, 6.4), 0, 8.4, 1.0, slit=4.8, sides='ens')
    with k.at((-13.0, -6.0, 0), yaw=-math.pi / 2):
        blast_door(k, 1.6, 2.5, depth=.5, back=2.2, fw=.35, yaw=-math.pi / 2)
    with k.at((-13.0, 6.4, 0), yaw=-math.pi / 2):
        blast_door(k, 1.6, 2.5, depth=.5, back=2.2, fw=.35, yaw=-math.pi / 2)
    count(k, 'antennas_dishes', 0)


def hpg_gen(k):
    """HPG generator (owner round 4: a higher-tech ComStar look in the approved generator language). Features:
    (1) a long stator drum in stepped ribbed segments with cyan glow in the joints, lying in pale ComStar collars
    on a battered tan cradle plinth; (2) coolant manifolds and HV conduit running from the drum into the control
    block; (3) two louvred radiator banks on the south end of the cradle; (4) the battered east control block with
    the ComStar emblem, doors and roof grates."""
    xc, yc = -7.8, 5.1
    batter_block(k, (-14.8, -3.0, -0.8, 13.2), 0, 1.6, .5, sides='nesw')
    for y in (-.4, 5.1, 10.6):                       # cradle saddles under the drum
        k.prism([(-11.4, 1.5), (-4.2, 1.5), (-4.2, 3.4), (-5.2, 5.0), (-10.4, 5.0), (-11.4, 3.4)], y - .9, y + .9, 'paint.main', 'mass',
                axis='y')
    with k.at((xc, yc, 0), yaw=math.pi / 2):
        PW.segmented_drum(k, 6.8, 7.4, 3.5)
        for x in (-4.4, 0.0, 4.4):                   # pale ComStar collars round the drum
            ip.tube(k, [(x - .35, 0, 7.4), (x + .35, 0, 7.4)], 4.05, 'paint.stencil', 'frame', sides=48)
            ip.tube(k, [(x - .4, 0, 7.4), (x + .4, 0, 7.4)], 3.92, 'paint.second', 'frame', sides=48)
    for dz in (0.0, .9):                             # coolant manifolds into the control block
        ip.pipe(k, [(-4.0, 1.6 + dz * 2, 3.0 + dz), (-1.0, 1.6 + dz * 2, 3.0 + dz), (1.3, 1.6 + dz * 2, 3.0 + dz)], .22, 'paint.stencil',
                end_flanges=True)
    ip.pipe(k, [(-7.8, 12.1, 9.4), (-7.8, 12.6, 9.4), (-7.8, 12.6, 1.8), (0.0, 12.6, 1.8), (1.1, 12.6, 1.8)], .18, 'paint.trim', bend=.6)
    for y in (-1.6, 11.8):                           # maintenance gantry portals over the drum, crossbeam and hoist
        for x in (-13.6, -2.0):
            k.box((x, y, 7.4), (.7, .7, 11.6), 'paint.main', 'mass', ch=.1)
        k.box((-7.8, y, 13.0), (12.3, .8, .7), 'paint.stencil', 'mass', ch=.1)
    k.box((-7.8, 5.1, 13.2), (.5, 14.2, .4), 'paint.second', 'frame')
    k.box((-7.8, 3.0, 12.6), (1.0, 1.2, .8), 'paint.trim', 'frame', ch=.06)
    for xc_ in (-11.3, -4.3):                        # radiator banks on the south end
        k.box((xc_, -2.2, 4.4), (5.2, 1.2, 5.6), 'paint.main', 'mass', ch=.12, top=.08)
        with k.at((xc_, -2.8, 0)):
            for z in ip.frange(2.0, 6.8, .35):
                k.slab(-2.3, 2.3, z, z + .12, -.12, .0, 'metal', 'frame')
        k.box((xc_, -2.2, 7.3), (5.4, 1.4, .2), 'paint.stencil', 'frame', ch=.06)
        k.cyl((xc_, -2.2, 7.6), .5, .4, 'grate', 'frame', sides=14)
    batter_block(k, (0.4, -13.4, 14.6, 12.6), 0, 7.2, 1.2, slit=4.2, sides='nes')
    emblem(k, (7.5, -13.0, 3.8), 0.0, 1.5)
    with k.at((7.5, -13.4, 0), yaw=0.0):
        blast_door(k, 1.8, 2.6, depth=.5, back=1.6, fw=.35, yaw=0.0)
    with k.at((9.5, 12.6, 0), yaw=math.pi):
        blast_door(k, 1.8, 2.6, depth=.5, back=1.6, fw=.35, yaw=math.pi)
    for y in (-6.0, 0.0, 6.0):
        k.box((7.5, y, 7.3), (8.0, 3.0, .2), 'grate', 'frame')
    count(k, 'panels_sections', 2)


def hpgoutbldg(k):
    """HPG outbuilding. Features: (1) the low U-shaped battered tan block round a courtyard; (2) dark slit bands;
    (3) emblem pillars flanking the courtyard entrance; (4) a recessed glazed entrance and roof grates."""
    for box in ((-13.4, -12.0, 13.6, -3.0), (4.7, -3.0, 13.6, 12.0), (-13.4, -3.0, -4.3, 12.0)):
        batter_block(k, box, 0, 7.3, 1.3, slit=4.4)
    batter_block(k, (-4.3, -3.0, 4.7, 7.2), 0, 4.7, .8)
    for x in (-4.0, 4.4):
        k.box((x, 12.2, 3.6), (1.4, 1.0, 7.2), 'paint.second', 'mass', ch=.15, top=.1)
        emblem(k, (x, 12.72, 4.6), math.pi, .55)
    glazed_bays(k, (-4.3, -3.0, 4.7, 7.2), 'n', 2, 2.4, 3.0)
    for y in (-8.0, 4.0):
        k.box((9.2, y, 7.32), (4.0, 2.0, .06), 'grate', 'frame')
    count(k, 'doors')


def hpgstation(k):
    """HPG station (owner round 4: the dish must read as the ComStar HPG dish and grow out of the building).
    Features: (1) the large pale parabolic HPG dish, tilted skyward on an elevation axle in a two-arm yoke on a
    turntable; (2) the feed horn on a three-leg feed support at the focus, with a glowing emitter; (3) the battered
    tan pylon rising out of a broad podium that a two-storey link wing joins to the control complex; (4) the battered
    control complex with emblem slabs; (5) slit bands and roof grates."""
    cx, cy = 17.5, 0.5
    batter_block(k, (cx - 9.0, cy - 9.0, cx + 9.0, cy + 9.0), 0, 6.0, 1.2, slit=3.6, sides='nes')
    batter_block(k, (cx - 6.0, cy - 6.0, cx + 6.0, cy + 6.0), 6.0, 32.0, 2.4, slit=12.0)
    emblem(k, (cx, cy - 5.0, 14.0), 0.0, 2.2)
    batter_block(k, (-12.5, -4.5, cx - 8.4, 5.5), 0, 8.0, .8, slit=4.6, sides='ns')      # link wing, stepped roof
    batter_block(k, (-10.0, -2.6, cx - 10.6, 3.6), 8.0, 10.2, .7, sides='')
    for i, y in enumerate((-1.6, 0.5, 2.6)):                                             # conduit loops, drum to wing
        ip.pipe(k, [(cx - 3.6, y, 32.5), (cx - 7.4, y, 32.5), (cx - 7.4, y, 14.0), (cx - 10.2 - 1.6 * i, y, 10.2 if i == 1 else 8.0)],
                .45, 'paint.second', bend=1.6, end_flanges=True)
    k.cyl((cx, cy, 33.0), 4.4, 2.0, 'paint.second', 'mass', sides=32, top=.2)            # radial-ribbed turntable drum
    for i in range(28):
        with k.at((cx, cy, 32.0), yaw=i * math.tau / 28):
            k.prism([(4.3, 0.0), (5.6, 0.0), (5.6, .5), (4.3, 2.0)], -.12, .12, 'paint.main', 'frame', axis='y')
    k.cyl((cx, cy, 34.3), 3.4, .6, 'paint.stencil', 'frame', sides=32, top=.1)
    za = 40.0
    for sx in (-1, 1):                                                                   # yoke arms
        with k.at((cx + sx * 4.6, cy, 0)):
            k.prism([(-.45, 33.8), (.45, 33.8), (.45, za + .9), (-.45, za + .9)], -1.6, 1.6, 'paint.main', 'mass', axis='x', top=.2)
    ip.tube(k, [(cx - 5.2, cy, za), (cx + 5.2, cy, za)], .7, 'paint.trim', 'frame', sides=16)   # elevation axle
    R = 16.0
    with k.at((cx, cy, za), pitch=-.55):
        k.cyl((0, 0, .9), 2.4, 1.8, 'paint.main', 'mass', sides=24, top=.3)                  # dish hub
        with k.at((0, 0, 1.6)):
            ip.lathe(k, [(0.0, 0.0), (R * .4, R * .05), (R * .8, R * .21), (R, R * .33), (R - .3, R * .35), (R * .78, R * .25),
                         (R * .38, R * .08), (0.0, .5)], 'paint.stencil', 'mass', n=18)         # faceted panels
            for rf in (.42, .72):                                                        # concentric panel rings
                rr = R * rf
                ip.tube(k, [(math.cos(a) * rr, math.sin(a) * rr, R * .33 * rf * rf + .62) for a in [i * math.tau / 36 for i in range(37)]],
                        .1, 'paint.main', 'frame', sides=6, caps=False)
            for i in range(24):                                                          # edge tabs
                with k.at((0, 0, R * .33), yaw=i * math.tau / 24):
                    k.box((R + .15, 0, .1), (.8, 1.5, .5), 'paint.main', 'frame', ch=.08)
            for i in range(16):                                                          # back ribs
                a = i * math.tau / 16
                pts = [(math.cos(a) * r, math.sin(a) * r, R * .33 * (r / R) ** 2 - .35) for r in ip.frange(2.2, R - .3, 1.8)]
                ip.tube(k, pts, .14, 'paint.main', 'frame', sides=6, caps=False)
            ip.tube(k, [(math.cos(a) * (R - .15), math.sin(a) * (R - .15), R * .33 + .12) for a in [i * math.tau / 48 for i in range(49)]],
                    .22, 'paint.main', 'frame', sides=8, caps=False)
            f = R * R / (4 * R * .33)
            k.cyl((0, 0, (f + .6) / 2), .55, f - .6, 'paint.main', 'mass', sides=12)       # single central feed mast
            k.cyl((0, 0, 2.0), 1.1, 1.6, 'paint.second', 'mass', sides=12, top=.2)
            ip.lathe(k, [(0.0, f + 2.4), (1.2, f + 2.2), (1.4, f + 1.2), (.7, f - .2), (.45, f - .3), (0.0, f - .3)], 'paint.second', 'frame', n=20)
            k.cyl((0, 0, f - .45), .4, .3, 'lamp_cool', 'frame', sides=12)
            k.cyl((0, 0, f + 2.7), .12, .8, 'lamp_red', 'frame', sides=8)
    count(k, 'antennas_dishes')
    batter_block(k, (-36.0, -12.0, -12.0, 12.0), 0, 9.4, 1.8, slit=5.2, sides='nsw')
    batter_block(k, (-33.6, -6.0, -24.0, 6.0), 9.4, 14.2, 1.0, slit=11.6)
    batter_block(k, (-20.0, -24.0, -12.0, -12.0), 0, 9.4, 1.2, slit=5.2, sides='esw')
    batter_block(k, (-20.0, 12.0, -12.0, 24.0), 0, 9.4, 1.2, slit=5.2, sides='enw')
    for y in (-18.0, 18.0):
        batter_block(k, (-19.0, y - 3.0, -14.0, y + 3.0), 9.4, 12.2, .6)
    emblem(k, (-36.05, 0.0, 5.0), -math.pi / 2, 2.4)
    with k.at((-36.0, 6.0, 0), yaw=-math.pi / 2):
        blast_door(k, 1.8, 2.6, depth=.5, back=1.6, fw=.35, yaw=-math.pi / 2)
    for (x, y) in ((-28.0, -8.0), (-28.0, 8.0)):
        k.box((x, y, 9.45), (4.0, 3.0, .06), 'grate', 'frame')


# ------------------------------------------------------------------ traffic control and terminals
def trafcon(k):
    """Airfield traffic control. Features: (1) the dominant wide glazed control cab, a ring of slanted glass under a
    heavy cap, wider than the shaft; (2) the rotating radar with a big airfield beacon stacked on it, on the cab roof (stock RotatyBit); (3) the setback shaft
    with ribbon storeys rising out of a battered base; (4) a low east annex with a roll-up door;
    (5) the sloped ramp shoulder to the north."""
    W, B = 'ferro', 'paint.second'
    loft(k, [rring(-9.7, -11.8, 8.9, 6.2, 0, .4), rring(-8.7, -10.8, 7.9, 5.2, 9.0, .4)], W, 'mass')
    k.box((-0.4, -2.8, .35), (19.0, 18.4, .7), B, 'mass', ch=.5)
    k.box((-0.4, -2.8, 9.15), (17.0, 16.4, .3), 'paint.plate', 'frame', ch=.5, walk=True)
    sh = (-5.6, -8.4, 4.8, 2.8)
    block(k, sh, 9.0, 30.4, W, B, plinth=False, coping=False)
    tower_storeys(k, sh, 9.0, 30.4, W, B)
    for x in (-5.6, 4.8):
        for y in (-8.4, 2.8):
            k.box((x, y, 19.7), (.7, .7, 21.4), 'paint.plate', 'frame', ch=.15)
    # owner 2026-10-24: the glazed cab must dominate the iso view - taller, more slanted glass, thin cap set back
    cab(k, (-0.4, -2.8), 12.6, 12.6, 30.4, 6.6, flare=1.9, cap=-.7, cap_t=.25, band=B, balcony=1.4, cap_mat='paint.trim', roof_mat='paint.trim')
    k.box((-0.4, -2.8, 37.4), (3.6, 3.6, .8), 'paint.trim', 'mass', ch=.3, top=.15)
    k.cyl((-0.29, -2.94, 38.5), .5, 1.4, 'paint.trim', 'frame', sides=14)
    # owner round 5: a big rotating airfield beacon stacked on the radar - both on the stock RotatyBit node
    radar_dipole(k, 'RotatyBit', (-0.29, -2.94, 39.2), L=5.0, beacon=True)
    # owner round 4: the three free-standing raked fins on the west read as random triangles - removed
    ea = (8.9, -7.6, 14.5, 6.0)
    block(k, ea, 0, 7.0, W, B, bands=((.7, 1.1),))
    ribbon(k, ea, 'e', 3.2, 4.4)
    doors(k, ea, 'e', 'rollup', [-3.5], 3.0, 3.0)
    k.prism([(6.0, 9.0), (6.0, 0.0), (12.3, 0.0), (12.3, .6)], -5.4, 4.9, W, 'mass', axis='x')
    doors(k, (-9.7, -11.8, 8.9, 6.2), 's', 'blast', [-4.0, 4.0], 1.8, 2.6)
    ribbon(k, (-9.7, -11.8, 8.9, 6.2), 's', 4.6, 5.8, -8.0, 8.0)
    clutter(k, (-8.0, -10.0, 7.0, 4.0), 9.3, 2, 11)


def trafcon2(k):
    """Spaceport traffic control. Features: (1) the north tower with the dominant wide glazed control cab and the
    rotating radar (stock Spinny); (2) the 20 m office block to the south with three ribbon storeys and garage doors;
    (3) the 9 m podium between with side wings and a stair rising to the tower; (4) faction-neutral bands, clutter."""
    W, B = 'ferro', 'paint.second'
    sh = (-7.4, 15.0, 2.6, 25.6)
    block(k, sh, 0, 28.2, W, B, bands=((.7, 1.1),), coping=False)
    tower_storeys(k, sh, 0, 28.2, W, B, sides='new', skip=(('s', 9.5),))
    cab(k, (-2.4, 20.3), 12.2, 12.6, 28.2, 6.2, flare=1.8, cap=-.7, cap_t=.25, band=B, balcony=1.4,
        cap_mat='paint.trim', roof_mat='paint.trim')
    k.box((-2.4, 20.3, 34.8), (3.4, 3.4, .8), 'paint.trim', 'mass', ch=.25, top=.12)
    k.cyl((-2.0, 20.09, 35.8), .45, 1.2, 'paint.trim', 'frame', sides=14)
    radar_dipole(k, 'Spinny', (-2.0, 20.09, 36.4), L=4.6)
    pod = (-11.0, -6.6, 6.4, 13.0)
    block(k, pod, 0, 9.2, W, B, bands=((.7, 1.1),))
    for s_ in 'w':
        ribbon(k, pod, s_, 3.6, 4.8, -8.0, 8.0)
    for sgn, x in ((-1, -12.2), (1, 8.2)):
        bx = (min(x - 1.2, x + 1.2), -5.0, max(x - 1.2, x + 1.2), 13.0)
        block(k, bx, 0, 8.4, W, B, bands=((.7, 1.1),))
    off = (-14.0, -28.6, 9.6, -6.6)
    block(k, off, 0, 20.0, W, B, bands=((.7, 1.1), (19.0, 19.6)))
    for z in (8.4, 12.0, 15.6):
        for s_ in 'sew':
            if s_ == 'e' and z < 12.0:
                continue                     # behind the 12 m east block (owner round 6 check: hidden glazing)
            ribbon(k, off, s_, z, z + 1.2)
    doors(k, off, 's', 'rollup', [-8.0, 0.0, 8.0], 4.0, 5.0)
    doors(k, (9.6, -28.6, 13.6, -6.6), 'e', 'blast', [0.0], 1.6, 2.5)   # was on the office face, buried by the east block
    k.box((11.6, -17.6, 6.0), (4.0, 22.0, 12.0), W, 'mass', ch=.15)
    k.box((11.6, -17.6, 12.1), (4.3, 22.3, .3), 'paint.plate', 'frame', ch=.2)
    clutter(k, off, 20.0, 4, 12)
    count(k, 'antennas_dishes', 0)


def _terminal_hall(k, x0, x1, y0, y1, H=10.0, W='ferro', B='paint.second'):
    """Spaceport terminal hall: chamfered white mass with sloped upper walls, a continuous dark glazing band, a roof
    with skylight troughs and a perimeter plinth."""
    loft(k, [rring(x0, y0, x1, y1, 0, .3), rring(x0, y0, x1, y1, 6.2, .3), rring(x0 + 1.2, y0 + 1.2, x1 - 1.2, y1 - 1.2, H, .3)], W, 'mass')
    k.box(((x0 + x1) / 2, (y0 + y1) / 2, .35), (x1 - x0 + .3, y1 - y0 + .3, .7), B, 'mass', ch=.4)
    k.box(((x0 + x1) / 2, (y0 + y1) / 2, H + .02), (x1 - x0 - 2.6, y1 - y0 - 2.6, .04), 'gravel', 'roof', walk=True, lint_skip=True)
    for s_ in 'nesw':
        ribbon(k, (x0, y0, x1, y1), s_, 2.2, 5.4, mull=2.4)
    for x in ip.frange(x0 + 5.0, x1 - 4.0, 5.0):
        k.box((x, (y0 + y1) / 2, H + .1), (1.2, y1 - y0 - 4.0, .25), 'paint.trim', 'frame')
        k.box((x, (y0 + y1) / 2, H + .25), (.9, y1 - y0 - 4.4, .05), 'glass', 'glass', pane=True)
    k.box(((x0 + x1) / 2, (y0 + y1) / 2, 6.3), (x1 - x0 + .2, y1 - y0 + .2, .3), B, 'skin', ch=.35)


def roof_pylon(k, x, y, zr, ztop):
    """Roof light pylon (owner round 5, the stock's two tall dark pylons rising from the terminal roof): a cast
    plinth on the roof slab, a tapered dark pylon, and a lamp house with floodlights aimed down and out on all sides."""
    k.box((x, y, zr + .3), (2.6, 2.6, .6), 'paint.second', 'mass', ch=.2, top=.1)
    zh = ztop - 1.6
    loft(k, [rring(x - .8, y - .8, x + .8, y + .8, zr + .6, .2), rring(x - .45, y - .45, x + .45, y + .45, zh, .12)],
         'paint.trim', 'mass')
    for z in ip.frange(zr + 2.0, zh - .5, 2.2):
        f = (z - zr - .6) / (zh - zr - .6)
        hw = .8 - .35 * f + .04
        k.box((x, y, z), (2 * hw, 2 * hw, .12), 'paint.plate', 'frame')
    k.box((x, y, zh + .5), (1.6, 1.6, 1.0), 'paint.plate', 'mass', ch=.1, top=.08)
    k.box((x, y, zh + 1.08), (1.9, 1.9, .16), 'paint.second', 'frame', ch=.05)
    for i in range(4):
        with k.at((x, y, zh + .5), yaw=i * math.pi / 2):
            with k.at((0, -.8, 0), pitch=-.5):
                k.box((0, -.18, 0), (1.1, .36, .6), 'paint.plate', 'frame', ch=.05)
                k.box((0, -.37, 0), (.94, .03, .46), 'lamp', 'frame')
    k.cyl((x, y, zh + 1.35), .08, .4, 'lamp_red', 'frame', sides=8)


def light_pylon(k, base, top, r=.45):
    """Apron floodlight mast (owner round 4: no raked struts over the roof): a vertical tapered pole on a cast footing,
    a crossarm and a bank of floodlight heads aimed down and out onto the apron, away from the building
    (`top` only gives the side the building is on and the head height)."""
    x, y = base
    h = top[2] - 1.5
    out = math.atan2(base[1] - top[1], base[0] - top[0])          # away from the hall
    k.box((x, y, .3), (1.6, 1.6, .6), 'ferro', 'mass', ch=.15, top=.08)
    k.cyl((x, y, .6 + 3.0), r, 6.0, 'paint.trim', 'mass', sides=12)
    k.cyl((x, y, 6.6 + (h - 6.6) / 2), r * .7, h - 6.6, 'paint.trim', 'mass', sides=12)
    k.cyl((x, y, 6.65), r * 1.05, .14, 'paint.plate', 'frame', sides=12)
    with k.at((x, y, h), yaw=out - math.pi / 2):
        k.box((0, 0, .1), (3.2, .3, .25), 'paint.plate', 'mass', ch=.05)             # crossarm
        k.box((0, 0, .5), (.5, .5, .8), 'paint.plate', 'frame', ch=.06, top=.05)    # head box on the pole
        for xl in (-1.15, 0.0, 1.15):
            with k.at((xl, -.25, .1), pitch=-.6):
                k.box((0, -.3, 0), (.9, .5, .7), 'paint.plate', 'frame', ch=.06)
                k.box((0, -.56, 0), (.76, .04, .56), 'lamp', 'frame')
        k.cyl((0, 0, 1.05), .08, .3, 'lamp_red', 'frame', sides=8)               # aviation light


def ext_stair(k, x, y0, y1, z, side):
    ip.stair(k, (x, y0, 0.0), (x, y1, z), w=1.2, name='terminal stair')
    count(k, 'stairs_ladders')


def terminal1(k):
    """Spaceport terminal. Features: (1) the long white hall with sloped upper walls and a continuous dark glazing
    band; (2) roof skylight troughs; (3) a tall light pylon on the roof; (4) the external stairs at the ends; (5) a roof
    access hatch block."""
    _terminal_hall(k, -15.3, 15.3, -2.6, 10.6)
    roof_pylon(k, 6.3, 6.6, 10.0, 19.6)
    doors(k, (-15.3, -2.6, 15.3, 10.6), 's', 'blast', [-6.0, 0.0], 1.8, 2.6)
    for x in (13.2,):
        k.box((x, -4.0, 1.2), (2.4, 2.8, 2.4), 'paint.second', 'mass', ch=.15)
    count(k, 'antennas_dishes', 0)


def terminal2(k):
    _terminal_hall(k, -15.3, 15.3, -6.3, 6.3)
    roof_pylon(k, 6.3, -2.4, 10.0, 19.6)
    roof_pylon(k, -6.3, 2.4, 10.0, 19.6)
    doors(k, (-15.3, -6.3, 15.3, 6.3), 's', 'blast', [-4.0], 1.8, 2.6)
    doors(k, (-15.3, -6.3, 15.3, 6.3), 'n', 'blast', [-4.0], 1.8, 2.6)
    for (x, y) in ((13.5, -7.6), (-13.5, 7.6)):
        k.box((x, y, 1.2), (2.4, 2.4, 2.4), 'paint.second', 'mass', ch=.15)


def walkway(k):
    """Terminal link walkway: the same white hall section (sloped upper walls, dark glazing band, skylight trough)
    as a connector between terminals."""
    _terminal_hall(k, -15.9, 15.9, -6.3, 6.3)
    for x in (-15.9, 15.9):
        k.box((x, 0, 4.0), (.6, 12.8, 8.0), 'paint.second', 'frame', ch=.1)
    count(k, 'panels_sections')


# ------------------------------------------------------------------ support sheds
def tinshack(k):
    """Tin shack. Features: (1) a corrugated gabled shed; (2) the open lean-to porch on posts along the south side;
    (3) the tall dark flue/boiler stack on the north side; (4) louvred gable vent, door and windows."""
    import intent_set3 as S
    top = lambda x, y: 4.6 + 2.5 * max(0.0, 1 - abs(y - 1.0) / 8.0)  # noqa: E731
    S.shed(k, -9.0, -6.0, 9.0, 9.0, 7.0, sides='nesw', top=top, bay=3.0, openings={
        's': [('door', -4.0, 0.2, 1.2, 2.3), ('window', 2.0, 1.2, 1.6, 1.0), ('window', 5.5, 1.2, 1.6, 1.0)],
        'e': [('door2', 0.0, 0.2, 2.2, 2.6)], 'w': [('louvre', 0.0, 4.4, 2.0, 1.2)]})
    k.box((0, 1.5, .1), (18.6, 15.6, .2), 'ferro', 'mass', ch=.05, walk=True)
    k.prism([(-9.4, 4.4), (1.0, 7.2), (9.4, 4.4), (9.4, 4.6), (1.0, 7.4), (-9.4, 4.6)], -9.3, 9.3, 'corr.corr', 'roof', axis='x')
    k.prism([(-9.3, 3.2), (-6.0, 4.4), (-6.0, 4.55), (-9.3, 3.35)], -9.0, 9.0, 'corr.corr', 'roof', axis='x')
    for x in ip.frange(-8.6, 8.6, 3.44):
        k.cyl((x, -8.9, 1.6), .12, 3.2, 'paint.steel', 'frame', sides=8)
    k.box((4.6, 5.0, 4.6), (2.4, 4.2, 9.2), 'paint.steel', 'mass', ch=.1)
    k.cyl((4.6, 5.0, 10.0), .5, 2.0, 'paint.steel', 'frame', sides=12)
    k.cyl((4.6, 5.0, 11.1), .7, .2, 'paint.trim', 'frame', sides=12)
    count(k, 'panels_sections')


def meethall(k):
    """Mess hall. Features: (1) the tall central barrel-vault hall; (2) the semicircular spoked fanlight in the gable;
    (3) the long low lean-to wings either side with paired windows; (4) the timber-boarded walls and doors."""
    import intent_set3 as S
    hb = (-7.2, -2.9, 7.8, 13.3)
    S.shed(k, hb[0], hb[1], hb[2], hb[3], 9.5, sides='nesw', bay=3.2, openings={'s': [('door2', 0.0, 0.2, 2.6, 3.0)]})
    barrel(k, hb, 'y', 9.5, 6.6, rib=3.0, mat='corr.corr', ribm='paint.trim', ends='corr.main')
    with k.at((0.3, -3.14, 9.5)):
        for i in range(9):
            a = math.pi * i / 8
            k.beam((0, -.05, 0), (math.cos(a) * 5.8, -.05, math.sin(a) * 5.8), .12, 'paint.trim', 'frame')
        for r in (2.6, 5.8):
            ip.tube(k, [(math.cos(math.pi * i / 24) * r, -.05, math.sin(math.pi * i / 24) * r) for i in range(25)], .07, 'paint.trim', 'frame',
                    sides=6, caps=False)
        k.prism([(math.cos(math.pi * i / 24) * 6.0, math.sin(math.pi * i / 24) * 6.0) for i in range(25)], -.02, .02, 'glass', 'glass',
                axis='y', pane=True)
    count(k, 'windows')
    for wb in ((-13.8, -13.1, -7.2, 13.3), (7.8, -13.1, 14.4, 13.3)):
        side = 'e' if wb[0] > 0 else 'w'
        S.shed(k, wb[0], wb[1], wb[2], wb[3], 5.0, sides='nsew', bay=3.2, openings={
            side: [('window', -9.0, 1.4, 1.4, 1.1), ('window', -3.0, 1.4, 1.4, 1.1), ('window', 3.0, 1.4, 1.4, 1.1), ('window', 9.0, 1.4, 1.4, 1.1)],
            's': [('door', 0.0, 0.2, 1.1, 2.2)]})
        k.prism([(wb[0] - .2, 5.0), (wb[2] + .2, 5.0), (wb[2] + .2, 5.25), (wb[0] - .2, 5.25)], wb[1] - .2, wb[3] + .2, 'corr.corr', 'roof',
                axis='y')
    k.box((0.3, 0.1, .1), (28.8, 26.4, .2), 'ferro', 'mass', ch=.05, walk=True)


def shack(k):
    """Shack. Features: (1) a small corrugated gabled hut with a pale roof; (2) paired windows on each long side;
    (3) a steel door on the end; (4) a concrete pad."""
    import intent_set3 as S
    top = lambda x, y: 3.4 + 1.3 * max(0.0, 1 - abs(x) / 3.4)  # noqa: E731
    k.box((0, 0, .1), (7.6, 7.2, .2), 'ferro', 'mass', ch=.05, walk=True)
    S.shed(k, -3.4, -3.2, 3.4, 3.2, 4.7, sides='nesw', top=top, bay=2.2, zb=.2, openings={
        'n': [('window', -1.6, 1.4, 1.6, .8), ('window', 1.6, 1.4, 1.6, .8)], 's': [('window', -1.6, 1.4, 1.6, .8), ('window', 1.6, 1.4, 1.6, .8)],
        'e': [('door', 0.0, 0.2, 1.0, 2.1)]})
    k.prism([(-3.7, 3.3), (0.0, 4.75), (3.7, 3.3), (3.7, 3.5), (0.0, 4.95), (-3.7, 3.5)], -3.5, 3.5, 'corr.roof', 'roof', axis='y')


SPECS = {
    'sandbagwall': dict(fn=sandbagwall, palette='intent_mil', cls='prop', counts=dict(panels_sections=1), tol=dict(grid_mae=.3),
        intent='A straight sandbag wall: five staggered courses of slumped, individually set bags, two rows wide for the lower three.'),
    'sandbagbunker': dict(fn=sandbagbunker, palette='intent_mil', cls='small', counts=dict(panels_sections=1),
        intent=('A C-shaped sandbag gun pit: six staggered courses (two rows wide below), a timber firing step along the '
                'east wall, a sloped steel firing shield with a gun slot and unit number, a tripod machine gun behind the slot, '
                'ammunition boxes and a water can.')),
    'tent': dict(fn=tent, palette='intent_mil', cls='prop', counts=dict(doors=2),
        intent=('An army frame tent: olive canvas ridge tent on a timber deck, the canvas sagging between pole stations under '
                'darker reinforcing straps, an eave valance, ridge pole ends, guy lines to pegs, and framed doors in both gables.')),
    'longtent': dict(fn=longtent, palette='intent_mil', cls='small', counts=dict(doors=2, windows=8),
        intent=('A long army mess tent: the same olive canvas ridge tent at twice the length, with framed end doors, rolled side '
                'vent flaps over screens, straps, guy lines and pegs on a timber deck.')),
    'munitionscrates': dict(fn=munitionscrates, palette='intent_mil', cls='prop', counts=dict(crates=8),
        intent=('A munitions stack: six timber ammunition crates on pallets with two more on top; corner posts, rim battens, '
                'plank slats, a hazard band, stencilled number plates, rope handles and steel corner brackets.')),
    'barracks': dict(fn=barracks, palette='intent_mil', cls='small', counts=dict(doors=2, windows=6),
        intent=('A low battered ferrocrete troop barracks on a proud plinth: walls leaning in to a chamfered roof slab, four '
                'stepped plate armour ribs over the roof and down both flanks, three armoured slit windows under visors per side, '
                'a blast door in a deep plate portal with hazard jambs and sill at each end, sandbag blast walls flanking each '
                'door, unit stencil plates, roof vents, a hatch and a heat-sink fin pack.')),
    'vehiclehangar': dict(fn=vehiclehangar, palette='intent_mil', cls='large', counts=dict(doors=8, windows=9, stairs_ladders=2),
        intent=('A drive-through vehicle hangar: a cast ferrocrete hall on a mitred glacis skirt with plate ribs, an olive band '
                'and plate coping; an armoured bay door at each end in a deep hazard-striped portal under the stencilled bay '
                'number 13; a windowed office lean-to along the front; plant room and stores annexes behind; a louvred exhaust '
                'tower rising through the roof with a bent exhaust duct down onto the annex roof; caged ladders ground to '
                'lean-to roof to hall roof, roof hatches, vents and a mast.')),
    'repairbay': dict(fn=repairbay, palette='intent_mil_brown', cls='large', counts=dict(doors=6, stairs_ladders=8, windows=6),
        tol=dict(grid_mae=1.0, footprint=.55, height=1.1, centroid=.25),
        intent=('Mech repair bay that fits every stock mech (tallest atlas.ase 16.94 m, widest bloodasp.ase 13.83 m), in the '
                'stock brown stepped massing grown to suit: the open pad between the workshop wings is 15.7 m clear between '
                'the gantries and 12.6 m deep (hazard border, foot targets, tie-down rails, floor number); a service gantry '
                'each side with platforms at knee 4.4, waist 8.6 and shoulder 13.0 m on caged ladders; an overhead crane '
                'runway with its underside at 19.0 m (2.06 m over the Atlas) from the hall head to an east portal, with '
                'bridge, trolley and hook block; a 16 x 19.5 m armoured bay door into an 11 m deep, 22 m tall hall; racks, '
                'status boards, the west services block and an external fire stair to the upper gallery door.')),
    'hangar': dict(fn=hangar, palette='intent_mil', cls='large', counts=dict(doors=10, windows=29),
        intent=('A big arched dropship/aircraft hangar: cast long walls on a glacis skirt with a high window row and office '
                'strip windows, plate buttress feet carrying proud armour ribs over a barrel-vault roof with a ridge vent and '
                'skylights; six staggered sliding hangar doors on a top track between hazard pilasters under a big stencilled 07 '
                'on the east gable; a roll-up door, personnel door and clerestory louvres on the west gable.')),
    'quonset2': dict(fn=quonset2, palette='intent_quonset', cls='small', counts=dict(stairs_ladders=1, antennas_dishes=1),
        intent=('The approved quonset (rust corrugated barrel on arch ribs, framed end walls, dormer windows) carrying a sensor '
                'station: a railed steel deck on posts and roof stubs over the east flank with a caged ladder, an electronics '
                'cabinet with status light and placard, a tracking dish on a yoke, and a cable run down to the end wall.')),
    's_turretcontrol': dict(fn=s_turretcontrol, palette='intent_mil_s', cls='large', counts=dict(doors=4, windows=19, stairs_ladders=1, antennas_dishes=3),
        intent=('Steiner turret control in grey-silver plate: a tall octagonal command tower on a blue-grey base band with proud '
                'corner ribs and plate collars, a slit band and a glazed armoured cupola carrying the rotating radar dipole (stock node LOS_Radar); a blast door at its foot; a low '
                'link block and two chamfered hex pods with slit windows, blast doors and a unit stencil; an L-shaped cast blast '
                'wall, a cable tray to the tower, a roof ladder, a guyed mast with a rotating antenna head (stock node LOS_Antenae) and a tracking dish on a pod.')),
    'd_turretcontrol': dict(fn=d_turretcontrol, palette='intent_mil_d', cls='large', counts=dict(doors=4, windows=16, stairs_ladders=1, antennas_dishes=3),
        intent=('Davion turret control in tan-yellow plate over a pale grey base: a tall battered square tower with plate collars, '
                'paired slit windows on every face under an overhanging glazed crown carrying the rotating radar bar (stock node Spinner); a blast door at its foot; '
                'a stepped low annex (main hall, raised chamfered operations block with the rotating dish (stock node small Dish), north wing, east pod) and a gate '
                'kiosk with a mast; cable tray, roof ladder, stencil, vents and hatch.')),
    'l_turretcontrol': dict(fn=l_turretcontrol, palette='intent_mil_l', cls='large', counts=dict(doors=3, windows=19, stairs_ladders=1, antennas_dishes=5),
        intent=('Liao turret control in green plate over a tan base: a tall twelve-sided tapered tower with proud ribs and collars, a '
                'slit band and a glazed head ring under a pointed spire topped by a rotating dipole (stock node LOS_ant3); a blast door at its foot; a chamfered annex '
                '(main hall with a raised operations block, a command block and an end pod) with slit windows and stencils; an '
                'L-shaped blast wall, two small rotating roof antennas (stock nodes LOS_ant1, LOS_ant2), roof ladder, antenna and dish.')),
    'relaystation': dict(fn=relaystation, palette='intent_mil', cls='small', counts=dict(doors=2, stairs_ladders=2, antennas_dishes=7),
        intent=('Comms relay: a corrugated equipment shelter with cast corner piers, louvres, a double door and a personnel door '
                'under a hipped roof with a grated centre deck; a lattice mast standing on the deck (straight to a railed service '
                'platform at 13 m, tapering above) carrying five sector panels, two link dishes and a beacon; caged ladders ground '
                'to roof deck and roof to platform; cable tray.')),
    'sensortower': dict(fn=sensortower, palette='intent_mil', cls='small', counts=dict(doors=0, stairs_ladders=1, antennas_dishes=2),
        intent=('Sensor tower: a buttressed cast footing with steps cut in, a tapered ribbed pylon with plate collars and edge ribs, '
                'a full-height caged ladder through a hatch onto a railed head platform on knee braces, and the rotating sensor head (stock node Sensorpivot: column, turntable, housing, beacon) carrying the '
                'twin panel arrays in a V with glowing sensor strips (child node Sensordish).')),
    'd_heavygate': dict(fn=wall_gate, palette='intent_wall_d', cls='large', counts=dict(doors=2, legs_supports=2, panels_sections=2),
        intent=('Davion heavy gate on the approved d_heavywall code: end piers and short cast runs exactly as the walls, two taller gate-tower piers with slit windows, lamps and beacons, and two armoured sliding leaves (framed panels, hazard foot band, faction roundel both faces) on a ground track.')),
    'd_lightgate': dict(fn=wall_gate, palette='intent_wall_d', cls='large', counts=dict(doors=2, legs_supports=2, panels_sections=2),
        intent=('Davion light gate on the approved d_lightwall code: end piers and short cast runs exactly as the walls, two taller gate-tower piers with slit windows, lamps and beacons, and two armoured sliding leaves (framed panels, hazard foot band, faction roundel both faces) on a ground track.')),
    'd_mediumgate': dict(fn=wall_gate, palette='intent_wall_d', cls='large', counts=dict(doors=2, legs_supports=2, panels_sections=2),
        intent=('Davion medium gate on the approved d_mediumwall code: end piers and short cast runs exactly as the walls, two taller gate-tower piers with slit windows, lamps and beacons, and two armoured sliding leaves (framed panels, hazard foot band, faction roundel both faces) on a ground track.')),
    'l_heavygate': dict(fn=wall_gate, palette='intent_wall_l', cls='large', counts=dict(doors=2, legs_supports=2, panels_sections=2),
        intent=('Liao heavy gate on the approved l_heavywall code: end piers and short cast runs exactly as the walls, two taller gate-tower piers with slit windows, lamps and beacons, and two armoured sliding leaves (framed panels, hazard foot band, faction roundel both faces) on a ground track.')),
    'l_lightgate': dict(fn=wall_gate, palette='intent_wall_l', cls='large', counts=dict(doors=2, legs_supports=2, panels_sections=2),
        intent=('Liao light gate on the approved l_lightwall code: end piers and short cast runs exactly as the walls, two taller gate-tower piers with slit windows, lamps and beacons, and two armoured sliding leaves (framed panels, hazard foot band, faction roundel both faces) on a ground track.')),
    'l_mediumgate': dict(fn=wall_gate, palette='intent_wall_l', cls='large', counts=dict(doors=2, legs_supports=2, panels_sections=2),
        intent=('Liao medium gate on the approved l_mediumwall code: end piers and short cast runs exactly as the walls, two taller gate-tower piers with slit windows, lamps and beacons, and two armoured sliding leaves (framed panels, hazard foot band, faction roundel both faces) on a ground track.')),
    's_heavygate': dict(fn=wall_gate, palette='intent_wall_s', cls='large', counts=dict(doors=2, legs_supports=2, panels_sections=2),
        intent=('Steiner heavy gate on the approved s_heavywall code: end piers and short cast runs exactly as the walls, two taller gate-tower piers with slit windows, lamps and beacons, and two armoured sliding leaves (framed panels, hazard foot band, faction roundel both faces) on a ground track.')),
    's_lightgate': dict(fn=wall_gate, palette='intent_wall_s', cls='large', counts=dict(doors=2, legs_supports=2, panels_sections=2),
        intent=('Steiner light gate on the approved s_lightwall code: end piers and short cast runs exactly as the walls, two taller gate-tower piers with slit windows, lamps and beacons, and two armoured sliding leaves (framed panels, hazard foot band, faction roundel both faces) on a ground track.')),
    's_mediumgate': dict(fn=wall_gate, palette='intent_wall_s', cls='large', counts=dict(doors=2, legs_supports=2, panels_sections=2),
        intent=('Steiner medium gate on the approved s_mediumwall code: end piers and short cast runs exactly as the walls, two taller gate-tower piers with slit windows, lamps and beacons, and two armoured sliding leaves (framed panels, hazard foot band, faction roundel both faces) on a ground track.')),
    'd_heavypost': dict(fn=wall_post, palette='intent_wall_d', cls='small', counts=dict(legs_supports=1),
        intent=('Davion heavy wall post: the approved d_heavywall terminal pier (same staged cast taper, face fields, bolts and cap) as a free-standing post, with a red beacon on the cap.')),
    'd_lightpost': dict(fn=wall_post, palette='intent_wall_d', cls='small', counts=dict(legs_supports=1), tol=dict(grid_mae=.3),
        intent=('Davion light wall post: the approved d_lightwall terminal pier (same staged cast taper, face fields, bolts and cap) as a free-standing post, with a red beacon on the cap.')),
    'd_mediumpost': dict(fn=wall_post, palette='intent_wall_d', cls='small', counts=dict(legs_supports=1),
        intent=('Davion medium wall post: the approved d_mediumwall terminal pier (same staged cast taper, face fields, bolts and cap) as a free-standing post, with a red beacon on the cap.')),
    'l_heavypost': dict(fn=wall_post, palette='intent_wall_l', cls='small', counts=dict(legs_supports=1),
        intent=('Liao heavy wall post: the approved l_heavywall terminal pier (same staged cast taper, face fields, bolts and cap) as a free-standing post, with a red beacon on the cap.')),
    'l_lightpost': dict(fn=wall_post, palette='intent_wall_l', cls='small', counts=dict(legs_supports=1),
        intent=('Liao light wall post: the approved l_lightwall terminal pier (same staged cast taper, face fields, bolts and cap) as a free-standing post, with a red beacon on the cap.')),
    'l_mediumpost': dict(fn=wall_post, palette='intent_wall_l', cls='small', counts=dict(legs_supports=1),
        intent=('Liao medium wall post: the approved l_mediumwall terminal pier (same staged cast taper, face fields, bolts and cap) as a free-standing post, with a red beacon on the cap.')),
    's_heavypost': dict(fn=wall_post, palette='intent_wall_s', cls='small', counts=dict(legs_supports=1),
        intent=('Steiner heavy wall post: the approved s_heavywall terminal pier (same staged cast taper, face fields, bolts and cap) as a free-standing post, with a red beacon on the cap.')),
    's_lightpost': dict(fn=wall_post, palette='intent_wall_s', cls='small', counts=dict(legs_supports=1),
        intent=('Steiner light wall post: the approved s_lightwall terminal pier (same staged cast taper, face fields, bolts and cap) as a free-standing post, with a red beacon on the cap.')),
    's_mediumpost': dict(fn=wall_post, palette='intent_wall_s', cls='small', counts=dict(legs_supports=1),
        intent=('Steiner medium wall post: the approved s_mediumwall terminal pier (same staged cast taper, face fields, bolts and cap) as a free-standing post, with a red beacon on the cap.')),
    's_gatecontrol': dict(fn=s_gatecontrol, palette='intent_mil_s', cls='large', counts=dict(antennas_dishes=1, doors=1, windows=21),
        intent=('Steiner gate control. Features: (1) long two-storey gate block with a balcony band and an upper ribbon of windows; (2) tall dark glazed bays at ground (the gatehouse front); (3) the tower with a dark overhanging control cab and the rotating radar (stock Base01); (4) blue-grey bands, roof clutter.')),
    'd_gatecontrol': dict(fn=d_gatecontrol, palette='intent_mil_d', cls='large', counts=dict(antennas_dishes=1, doors=2, windows=9),
        intent=('Davion gate control. Features: (1) central tower: a battered tan base under a pale cab storey with a window band and the rotating dish (stock Dish); (2) twin side blocks with horizontally ribbed tan upper walls; (3) a garage door and a long low east wing with a ribbon of windows; (4) the steel front stair to the gate roof; (5) tan-yellow bands on pale cast walls.')),
    'l_gatecontrol': dict(fn=l_gatecontrol, palette='intent_mil_l', cls='large', counts=dict(antennas_dishes=1, doors=2, windows=23),
        intent=('Liao gate control. Features: (1) the pale tower with a dark rounded overhanging cab and the rotating roof dish (stock Dish); (2) the green ribbed arched annex roof over tan walls; (3) tall framed windows and a door in the annex front; (4) green faction bands; (5) clutter.')),
    'bturretcontrol': dict(fn=bturretcontrol, palette='intent_mil', cls='large', counts=dict(antennas_dishes=2, doors=2, windows=17),
        intent=('Generic turret control on the stock massing: tall command tower over a low battered annex, slit windows per storey, blast door, a tall whip antenna, and the rotating radar bar (stock animated node Spinner_PAB).')),
    's_sensorcontrol': dict(fn=s_sensorcontrol, palette='intent_mil_s', cls='large', counts=dict(antennas_dishes=1, doors=2, windows=8),
        intent=('Steiner sensor control. Features: (1) the tall dark sensor-array slab with a grid of panels and lit emitter strips; (2) the pale control tower beside it with ribbon storeys and a glazed cab near the top; (3) the rotating sensor dish on a stand on top (stock L_Dish, and L_Sdish on the Liao); (4) the low annex with a garage door; (5) faction bands.')),
    'd_sensorcontrol': dict(fn=d_sensorcontrol, palette='intent_mil_d', cls='large', counts=dict(antennas_dishes=1, doors=2, windows=8),
        intent=('Davion sensor control. Features: (1) the tall dark sensor-array slab with a grid of panels and lit emitter strips; (2) the pale control tower beside it with ribbon storeys and a glazed cab near the top; (3) the rotating sensor dish on a stand on top (stock L_Dish, and L_Sdish on the Liao); (4) the low annex with a garage door; (5) faction bands.')),
    'l_sensorcontrol': dict(fn=l_sensorcontrol, palette='intent_mil_l', cls='large', counts=dict(antennas_dishes=1, doors=2, windows=8),
        intent=('Liao sensor control. Features: (1) the tall dark sensor-array slab with a grid of panels and lit emitter strips; (2) the pale control tower beside it with ribbon storeys and a glazed cab near the top; (3) the rotating sensor dish on a stand on top (stock L_Dish, and L_Sdish on the Liao); (4) the low annex with a garage door; (5) faction bands.')),
    's_spotlightcontrol': dict(fn=s_spotlightcontrol, palette='intent_mil_s', cls='large', counts=dict(antennas_dishes=1, doors=3, windows=15),
        tol=dict(grid_mae=.3),
        intent=('Steiner spotlight control (a small air-traffic-control building in a walled yard). Features: (1) a slim control tower with ribbon storeys and a glazed overhanging cab with a balcony; (2) the searchlight on the cab roof (stock Light, rotating); (3) the two-storey operations block, a garage wing and a south entrance block placed on the stock plan, their doors facing the yard; (4) a low cast compound wall closing the SE yard, its chamfered corner carrying the gate; (5) blue-grey bands and roof clutter.')),
    'l_spotlightcontrol': dict(fn=l_spotlightcontrol, palette='intent_mil_l', cls='large', counts=dict(antennas_dishes=1, doors=3, windows=7),
        tol=dict(grid_mae=.3),
        intent=('Liao spotlight control (a small air-traffic-control building in a walled yard). Features: (1) a pale octagonal control tower with a band per storey and a glazed octagonal cab with a balcony; (2) the searchlight on the cab roof (stock light, rotating); (3) the operations block, the green ribbed arched annex and an arched entrance wing placed on the stock plan, their doors facing the yard; (4) a low cast compound wall closing the SE yard, its chamfered corner carrying the gate; (5) green bands.')),
    's_hq': dict(fn=s_hq, palette='intent_mil_s', cls='large', counts=dict(antennas_dishes=1, doors=6, windows=27),
        intent=('Steiner HQ. Features: (1) tall office tower with stacked ribbon storeys and a glazed overhanging control cab with a balcony; (2) long central hall with a raised clerestory spine; (3) tall dark glazed bays on the front; (4) a low flat-roofed garage block built onto the hall with roll-up doors; (5) blue-grey faction bands, roof clutter and a fist-emblem roof plate.')),
    'l_hq': dict(fn=l_hq, palette='intent_mil_l', cls='large', counts=dict(antennas_dishes=1, doors=5, windows=31),
        intent=('Liao HQ. Features: (1) pale chamfered tower with a dark window band per storey and a flared glazed head with a helipad; (2) the long green ribbed arched hangar roof; (3) tan lower walls with doors and windows under the arch; (4) green faction bands; (5) roof clutter and masts.')),
    'd_hq': dict(fn=d_hq, palette='intent_mil_d', cls='large', counts=dict(doors=5, windows=21),
        intent=('Davion HQ. Features: (1) tower with a tan glazed cab band and helipad over pale stacked storeys; (2) the tan front blocks with garage doors and repeated pilasters; (3) the sloped stair-ramp shoulder stepping down west; (4) tan-yellow faction bands on pale cast walls; (5) roof clutter.')),
    'generic_hq': dict(fn=generic_hq, palette='intent_mil', cls='large', counts=dict(antennas_dishes=1, doors=5, windows=21),
        intent=('Generic HQ. Features: (1) chamfered blue-grey tower with dark storey bands and a flared glazed head with a helipad; (2) the long sloped riveted hall stepping down to the east; (3) the stepped west wing with a sloped roof; (4) riveted plate panels and bands; (5) doors and roof clutter.')),
    'guntower': dict(fn=guntower, palette='intent_mil', cls='small', counts=dict(legs_supports=4, stairs_ladders=1, windows=4),
        intent=('Timber trestle gun tower: four raked legs on cast footings with X bracing and girts, a caged ladder inside to an armoured gun cab with a slit band and a gun on a mantlet.')),
    'spotlight': dict(fn=spotlight, palette='intent_spot', cls='small', counts=dict(antennas_dishes=1), tol=dict(grid_mae=.3),
        intent=('Field searchlight after the stock (owner: no platform, no ladder, nothing on top of the lamp): battered armoured plinth, slim mast, and the stock L_SpotRotar fork turning on the mast with the round lamp drum held at its sides on trunnions, lens out, cable and vent ring.')),
    'perimeteralarm': dict(fn=perimeteralarm, palette='intent_mil', cls='small', counts=dict(antennas_dishes=1, stairs_ladders=1),
        intent=('Perimeter alarm mast: equipment cabinet with placard and status light, steel pole, caged ladder to a railed ring platform, and the rotating siren head with four horns and a red beacon (stock animated node spinny).')),
    'trafcon': dict(fn=trafcon, palette='intent_terminal', cls='large', counts=dict(antennas_dishes=1, doors=3, windows=27),
        intent=('Airfield traffic control. Features: (1) the dominant wide glazed control cab, a ring of slanted glass under a heavy cap, wider than the shaft; (2) the rotating radar with a big airfield beacon stacked on it, on the cab roof (stock RotatyBit); (3) the setback shaft with ribbon storeys rising out of a battered base; (4) a low east annex with a roll-up door; (5) the sloped ramp shoulder to the north.')),
    'trafcon2': dict(fn=trafcon2, palette='intent_terminal', cls='large', counts=dict(antennas_dishes=1, doors=4, windows=34),
        intent=('Spaceport traffic control. Features: (1) the north tower with the dominant wide glazed control cab and the rotating radar (stock Spinny); (2) the 20 m office block to the south with three ribbon storeys and garage doors; (3) the 9 m podium between with side wings and a stair rising to the tower; (4) faction-neutral bands, clutter.')),
    'terminal1': dict(fn=terminal1, palette='intent_terminal', cls='large', counts=dict(antennas_dishes=0, doors=2, windows=4), tol=dict(footprint=.5, centroid=.2),
        intent=('Spaceport terminal. Features: (1) the long white hall with sloped upper walls and a continuous dark glazing band; (2) roof skylight troughs; (3) a tall light pylon on the roof; (4) the external stairs at the ends; (5) a roof access hatch block.')),
    'terminal2': dict(fn=terminal2, palette='intent_terminal', cls='large', counts=dict(doors=2, windows=4), tol=dict(footprint=.5),
        intent=('Spaceport terminal (two pylons): the white hall with sloped upper walls, the continuous dark glazing band and skylight troughs, two tall light pylons on the roof at opposite ends, doors front and back.')),
    'walkway': dict(fn=walkway, palette='intent_terminal', cls='large', counts=dict(panels_sections=1, windows=4),
        intent=('Terminal link walkway: the same white hall section (sloped upper walls, dark glazing band, skylight trough) as a connector between terminals.')),
    'hpgstation': dict(fn=hpgstation, palette='intent_hpg', cls='large', counts=dict(antennas_dishes=1, doors=1, windows=22),
        intent=('HPG station. Features: (1) the large pale faceted HPG dish with concentric panel rings and edge tabs, tilted skyward on an elevation axle in a two-arm yoke; (2) a single central feed mast with the feed horn and a glowing emitter; (3) the radial-ribbed turntable drum on a battered tan pylon that rises out of a broad podium, heavy conduit loops from the drum into a stepped link wing joining the control complex; (4) the battered control complex with emblem slabs; (5) slit bands and roof grates.')),
    'hpg_office': dict(fn=hpg_office, palette='intent_hpg', cls='large', counts=dict(antennas_dishes=0, doors=2, windows=14),
        intent=('HPG office. Features: (1) three stacked battered tan tiers stepping up to a 20 m crown; (2) the tall ComStar emblem slab on the west face; (3) dark slit bands on each tier; (4) the low east annex; (5) doors at the foot.')),
    'hpg_gen': dict(fn=hpg_gen, palette='intent_hpg', cls='large', counts=dict(doors=2, panels_sections=2, windows=3),
        intent=('HPG generator. Features: (1) a long stator drum in stepped ribbed segments with cyan glow in the joints, in pale ComStar collars on a battered tan cradle plinth; (2) a maintenance gantry with a hoist over the drum; (3) coolant manifolds and conduit into the control block; (4) two louvred radiator banks; (5) the battered east control block with the ComStar emblem, two doors and roof grates.')),
    'hpgoutbldg': dict(fn=hpgoutbldg, palette='intent_hpg', cls='large', counts=dict(doors=1, windows=14),
        intent=('HPG outbuilding. Features: (1) the low U-shaped battered tan block round a courtyard; (2) dark slit bands; (3) emblem pillars flanking the courtyard entrance; (4) a recessed glazed entrance and roof grates.')),
    'hpg_threeway': dict(fn=hpg_threeway, palette='intent_hpg', cls='large', counts=dict(panels_sections=4, windows=12),
        intent=('HPG three-way corridor node: a battered chamfered hub with a lantern grate and three battered corridor arms with dark slit bands, roof walkway grates and end portals (one module family for the HPG set).')),
    'hpg_corner': dict(fn=hpg_corner, palette='intent_hpg', cls='large', counts=dict(panels_sections=3, windows=8),
        intent=('HPG corner node: the battered hub with two corridor arms (same module family).')),
    'hpg_end': dict(fn=hpg_end, palette='intent_hpg', cls='large', counts=dict(panels_sections=2, windows=4),
        intent=('HPG end node: the battered hub with one corridor arm and a ComStar emblem on its blind end.')),
    'hpg_fourway': dict(fn=hpg_fourway, palette='intent_hpg', cls='large', counts=dict(panels_sections=5, windows=16),
        intent=('HPG four-way node: the battered hub with four corridor arms.')),
    'hpg_walkway': dict(fn=hpg_walkway, palette='intent_hpg', cls='large', counts=dict(panels_sections=1, windows=16),
        intent=('HPG corridor: the battered tan corridor section with a dark slit band, roof walkway grate, hatches and end pilasters.')),
    'tinshack': dict(fn=tinshack, palette='intent_shed', cls='small', counts=dict(doors=2, panels_sections=1, windows=2),
        intent=('Tin shack. Features: (1) a corrugated gabled shed; (2) the open lean-to porch on posts along the south side; (3) the tall dark flue/boiler stack on the north side; (4) louvred gable vent, door and windows.')),
    'meethall': dict(fn=meethall, palette='intent_shed', cls='large', counts=dict(doors=3, windows=9),
        intent=('Mess hall. Features: (1) the tall central barrel-vault hall; (2) the semicircular spoked fanlight in the gable; (3) the long low lean-to wings either side with paired windows; (4) the timber-boarded walls and doors.')),
    'shack': dict(fn=shack, palette='intent_shed', cls='small', counts=dict(doors=1, windows=4),
        intent=('Shack. Features: (1) a small corrugated gabled hut with a pale roof; (2) paired windows on each long side; (3) a steel door on the end; (4) a concrete pad.')),
}
