"""Civilian family, group 1 (owner 2026-10-24): colony housing, the greenhouse, timber and shanty fences, street
props and marker beacons. Built by intent on the stock silhouettes (stock views and top-down plan read first),
in one frontier-colony look: riveted grey plate and rust-patched corrugated sheet over a cast footing, white-framed
windows, brick chimney stacks, pale prefab trims; pristine. Every builder is checked for access (doors with 2 m
clear, glazing on its frame, nothing floating) and the plan is compared with the stock footprint before baking."""
import math

import geo
import ind_parts as ip
import intent as I
import intent_military as M

count = I.count


def ccw(poly):
    a = sum(p[0] * q[1] - q[0] * p[1] for p, q in zip(poly, poly[1:] + poly[:1]))
    return poly if a > 0 else poly[::-1]


# ------------------------------------------------------------------ shared parts
def window(k, xl, zc, w, h, frame='paint.frame', mull=True):
    """White-framed window in a wall frame (outward -Y): proud frame, sill, glass on the frame, a glazing bar."""
    x0, x1, z0, z1 = xl - w / 2, xl + w / 2, zc - h / 2, zc + h / 2
    f = .1
    k.slab(x0 - f, x1 + f, z0 - f, z1 + f, -.08, .02, frame, 'frame', front=.015)
    k.slab(x0, x1, z0, z1, -.1, -.07, 'glass', 'glass', pane=True)
    if mull:
        k.slab(xl - .03, xl + .03, z0, z1, -.12, -.08, frame, 'frame')
        k.slab(x0, x1, zc + h * .1 - .03, zc + h * .1 + .03, -.12, -.08, frame, 'frame')
    k.slab(x0 - f - .05, x1 + f + .05, z0 - f - .08, z0 - f, -.2, .02, 'paint.trim', 'frame', front=.02)
    count(k, 'windows')


def door(k, xl, w=1.0, h=2.15, canopy=True):
    """Prefab entrance door (wall frame, outward -Y): frame proud of the face, leaf with a vision panel, step,
    optional canopy on brackets."""
    ip.personnel_door(k, xl, w, h, -.2, canopy=canopy)
    count(k, 'doors')


def seams(k, L, z0, z1, pitch=1.4, mat='paint.second'):
    """Riveted plate seams on a solid wall face (wall frame): vertical cover strips with bolt rows."""
    n = max(1, int(L / pitch))
    for i in range(1, n):
        x = -L / 2 + L * i / n
        k.slab(x - .05, x + .05, z0, z1, -.04, .01, mat, 'frame')


def chimney(k, x, y, z0, z1, w=1.1):
    """Two-stage brick chimney stack with a plate band and a cap."""
    zm = z0 + (z1 - z0) * .55
    k.box((x, y, (z0 + zm) / 2), (w, w, zm - z0), 'brick.main', 'mass', ch=.03)
    k.box((x, y, (zm + z1) / 2), (w * .66, w * .66, z1 - zm), 'brick.main', 'mass', ch=.03)
    k.box((x, y, zm + .06), (w + .08, w + .08, .12), 'paint.trim', 'frame', ch=.02)
    k.box((x, y, z1 + .06), (w * .66 + .12, w * .66 + .12, .12), 'paint.trim', 'frame', ch=.02)
    k.cyl((x, y, z1 + .3), w * .18, .4, 'paint.steel', 'frame', sides=10)


def stovepipe(k, x, y, z0, z1, side=1, r=.12):
    """Steel stovepipe with an elbow off a wall and a conical rain cap."""
    ip.tube(k, [(x, y, z0), (x + side * .5, y, z0), (x + side * .5, y, z1)], r, 'paint.steel', 'frame', sides=10)
    k.cyl((x + side * .5, y, z1 + .1), r * 2.6, .08, 'paint.rust', 'frame', sides=12, top=.03)
    M.loft(k, [[(x + side * .5 + r * 2.8 * math.cos(a), y + r * 2.8 * math.sin(a), z1 + .14) for a in [i * math.tau / 12 for i in range(12)]],
               [(x + side * .5 + .02 * math.cos(a), y + .02 * math.sin(a), z1 + .45) for a in [i * math.tau / 12 for i in range(12)]]],
           'paint.rust', 'frame')
    k.box((x, y, z0), (.3, .3, .3), 'paint.steel', 'frame')


def glazed_band(k, L, z0, z1, pitch=1.2):
    """Ground-floor sunroom band (wall frame): a run of tall panes between plate posts, a sill and a head rail."""
    n = max(1, round(L / pitch))
    for i in range(n):
        xa = -L / 2 + L * i / n
        xb = -L / 2 + L * (i + 1) / n
        k.slab(xa + .06, xb - .06, z0, z1, -.06, -.03, 'glass', 'glass', pane=True)
        k.slab(xa - .06, xa + .06, z0, z1, -.1, .01, 'paint.second', 'frame')
    k.slab(L / 2 - .06, L / 2 + .06, z0, z1, -.1, .01, 'paint.second', 'frame')
    k.slab(-L / 2, L / 2, z0 - .14, z0, -.16, .02, 'paint.second', 'frame')
    k.slab(-L / 2, L / 2, z1, z1 + .14, -.14, .02, 'paint.second', 'frame')
    count(k, 'windows', n)


def arch_prof(w, zw, zt, n=10):
    """Colony vault profile across a width w: steep haunches easing into a flat crown (stock arched gables)."""
    pts = []
    for i in range(n + 1):
        t = -1 + 2 * i / n
        pts.append((t * w / 2, zw + (zt - zw) * (1 - abs(t) ** 2.6)))
    return pts


def vault_house(k, W, L, zw, zt, chimneys, doors_w, win_e, win_w, band=True):
    """Colony vault house (singlecivliving a/b): riveted plate walls on a cast footing with a glazed sunroom band
    round the ground floor, an arched riveted roof (vault along y) with seam ribs, clerestory gable windows, brick
    chimneys, doors on the west side."""
    k.box((0, 0, .15), (W + .4, L + .4, .3), 'ferro', 'mass', ch=.08, top=.05)
    prof = arch_prof(W, zw, zt)
    body = [(-W / 2, .3)] + prof + [(W / 2, .3)]
    k.prism(ccw([(x, z) for x, z in body]), -L / 2, L / 2, 'paint.main', 'mass', axis='y')
    # roof skin proud of the body, with seam ribs every 1.4 m and end trims
    out = [(x * 1.03, z + .12) for x, z in prof]
    skin = out + [(x, z) for x, z in reversed(prof)]
    k.prism(ccw(skin), -L / 2 - .25, L / 2 + .25, 'paint.roof', 'roof', axis='y', lint_skip=True)
    n = max(2, int(L / 1.4))
    for i in range(n + 1):
        y = -L / 2 - .2 + (L + .4) * i / n
        rib = [(x * 1.035, z + .16) for x, z in prof] + [(x * 1.03, z + .1) for x, z in reversed(prof)]
        k.prism(ccw(rib), y - .05, y + .05, 'paint.second', 'frame', axis='y')
    for s in (-1, 1):                     # gable ends: clerestory ribbon and riveted panel seams
        with k.at((0, s * L / 2, 0), yaw=0.0 if s < 0 else math.pi):
            for xl in (-W * .22, 0.0, W * .22):
                window(k, xl, zw + (zt - zw) * .55, W * .18, .7)
            if band:
                glazed_band(k, W - .6, .55, 2.2)
            seams(k, W - .6, 2.6, zw, 1.6)
    for s, wins in ((-1, win_w), (1, win_e)):
        with k.at((s * W / 2, 0, 0), yaw=s * math.pi / 2):
            if band:
                ds = [d_ for d_ in (doors_w if s < 0 else [])]
                spans = [(-L / 2 + .3, L / 2 - .3)]
                for dx in ds:                                    # band broken at the doors
                    nxt = []
                    for a, b in spans:
                        if a < dx - .9 < b:
                            nxt.append((a, dx - .9))
                        if a < dx + .9 < b:
                            nxt.append((dx + .9, b))
                        if not (a < dx - .9 < b or a < dx + .9 < b):
                            nxt.append((a, b))
                    spans = nxt
                for a, b in spans:
                    if b - a > 1.0:
                        with k.at(((a + b) / 2, 0, 0)):
                            glazed_band(k, b - a, .55, 2.2)
            for xl in wins:
                window(k, xl, zw - .85, 1.5, 1.1)
            for dx in (doors_w if s < 0 else []):
                door(k, dx)
                I.wall_lamp(k, dx + 1.0, 2.6, s * math.pi / 2)
    for (x, y) in chimneys:
        zr = zw + (zt - zw) * (1 - abs(2 * x / W) ** 2.6)
        chimney(k, x, y, zr - .4, 9.0, 1.0)
    for y in (-L * .25, L * .25):
        ip.mushroom_vent(k, -W * .1, y, zt + .1)


def singlecivlivinga(k):
    """Colony vault house. Features: (1) riveted plate walls on a cast footing with a glazed sunroom band round the
    ground floor; (2) an arched riveted roof with seam ribs and clerestory windows in both gables; (3) a two-stage
    brick chimney; (4) the side door with a canopy, lamp and step; (5) upper windows and roof vents."""
    vault_house(k, 8.6, 8.2, 3.6, 6.6, [(1.6, 2.4)], [-1.8], [0.0], [1.8])


def singlecivlivingb(k):
    """Long colony vault house: the same vault house at twice the length, two brick chimneys, two side doors with
    canopies, rows of upper windows over the glazed sunroom band."""
    vault_house(k, 8.8, 16.6, 3.6, 6.8, [(1.4, -2.0), (1.4, 2.0)], [-1.0, 1.0], [-5.5, -2.0, 2.0, 5.5], [-5.5, -3.2, 3.2, 5.5])


def singlecivlivingc(k):
    """Corrugated settler hut. Features: (1) a corrugated shell with one vertical wall and one curved shoulder
    (the stock single-sweep roof); (2) rust repair plates riveted over the sheet; (3) a steel stovepipe with an
    elbow and a conical rain cap; (4) the gable door with a step, a framed window on each side."""
    W, L = 5.4, 6.6
    k.box((0, 0, .1), (W + .3, L + .3, .2), 'ferro', 'mass', ch=.05)
    prof = [(-W / 2, .2), (W / 2, .2), (W / 2, 2.9), (W * .3, 4.2), (-W / 2, 4.75)]
    k.prism(ccw(prof), -L / 2, L / 2, 'corr', 'mass', axis='y')
    roof = [(W / 2 + .2, 2.82), (W * .3, 4.22), (-W / 2 - .2, 4.79), (-W / 2 - .2, 4.95), (W * .3, 4.38), (W / 2 + .2, 2.98)]
    k.prism(ccw(roof), -L / 2 - .2, L / 2 + .2, 'paint.roof', 'roof', axis='y', lint_skip=True)
    for y in ip.frange(-L / 2, L / 2, .6):
        k.prism(ccw([(W / 2 + .2, 2.98), (W * .3, 4.38), (-W / 2 - .2, 4.95), (-W / 2 - .2, 5.01), (W * .3, 4.44), (W / 2 + .2, 3.04)]),
                y - .03, y + .03, 'paint.roof', 'detail', axis='y')
    with k.at((0, -L / 2, 0)):
        window(k, -.6, 2.7, 1.6, .6)
    with k.at((0, L / 2, 0), yaw=math.pi):
        door(k, .4, .95, 2.1)
    with k.at((W / 2, 0, 0), yaw=math.pi / 2):
        window(k, .4, 1.9, .9, .8)
        for (a, b, z0, z1) in ((-2.6, -1.4, .7, 1.4), (1.2, 2.5, 1.9, 2.6)):
            k.slab(a, b, z0, z1, -.05, .01, 'paint.rust', 'frame')
    with k.at((-W / 2, 0, 0), yaw=-math.pi / 2):
        window(k, -.8, 2.1, .9, .8)
        k.slab(.6, 2.0, .8, 1.7, -.05, .01, 'paint.rust', 'frame')
    stovepipe(k, -W / 2 - .15, -L * .25, 3.4, 5.0, side=-1)


def singlecivlivingd(k):
    """Prefab settler shack. Features: (1) a small timber-and-plate box on a cast pad; (2) a low monopitch roof that
    runs out over a side porch on two steel posts; (3) the gable door with a step and a framed window either side;
    (4) a brick flue with a mushroom cap; (5) rust patch plates on the roof."""
    W, L = 4.4, 6.0
    k.box((0, -.6, .1), (W + .3, L + 1.6, .2), 'ferro', 'mass', ch=.05)
    k.box((0, .3, 1.95), (W, L, 3.5), 'paint.main', 'mass', ch=.05)
    for s in (-1, 1):
        with k.at((s * W / 2, .3, 0), yaw=s * math.pi / 2):
            seams(k, L, .3, 3.6, 1.0, 'paint.second')
            window(k, -.6 * s, 2.2, 1.6, .8)
    with k.at((0, .3 - L / 2, 0)):
        door(k, 0.0, .95, 2.1, canopy=False)
        window(k, -1.4, 2.2, .7, .9)
        window(k, 1.4, 2.2, .7, .9)
    with k.at((0, .3 + L / 2, 0), yaw=math.pi):
        window(k, 0.0, 2.2, 1.6, .8)
    k.prism(ccw([(-L / 2 - 1.8 + .3, 3.75), (L / 2 + .5 + .3, 4.15), (L / 2 + .5 + .3, 4.35), (-L / 2 - 1.8 + .3, 3.95)]),
            -W / 2 - .3, W / 2 + .3, 'paint.roof', 'roof', axis='x', lint_skip=True)
    for x in (-W / 2 - .1, W / 2 + .1):
        k.box((x, .3 - L / 2 - 1.6, 1.9), (.12, .12, 3.8), 'paint.steel', 'mass')
    k.box((0, .3 - L / 2 - 1.6, 3.75), (W + .4, .14, .16), 'paint.steel', 'frame')
    for (x, y, w, d) in ((-1.0, 1.6, 1.6, 1.0), (1.2, -.8, 1.2, .9)):
        k.box((x, y, 4.2), (w, d, .05), 'paint.rust', 'detail')
    chimney(k, .8, 2.0, 3.6, 4.35, .55)


def flat_block(k, box, z0, z1, wall='paint.main', parapet=True):
    """Prefab flat-roof block: riveted plate mass, a pale parapet coping, a gravel roof."""
    x0, y0, x1, y1 = box
    k.box(((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2), (x1 - x0, y1 - y0, z1 - z0), wall, 'mass', ch=.05)
    if parapet:
        k.box(((x0 + x1) / 2, (y0 + y1) / 2, z1 + .15), (x1 - x0 + .2, y1 - y0 + .2, .3), 'paint.frame', 'frame', ch=.04)
        k.box(((x0 + x1) / 2, (y0 + y1) / 2, z1 + .02), (x1 - x0 - .4, y1 - y0 - .4, .04), 'gravel', 'roof', walk=True)


def face(box, s):
    o, yaw, L = M.frame_of(box, s)
    return (o[0], o[1], 0), yaw, L


def civliving2(k):
    """L-plan colony row house. Features: (1) a single-storey L of riveted plate prefab units on a cast footing,
    a pale parapet and gravel roof; (2) two entrance doors with canopies and lamps on the outer faces; (3) framed
    windows on every face; (4) roof units: an air handler, a water tank and hatches; (5) plate seams."""
    a = (-8.6, 1.6, 8.6, 8.6)              # long arm along the north (stock plan)
    b = (2.0, -8.6, 8.6, 1.6)
    for bx in (a, b):
        k.box(((bx[0] + bx[2]) / 2, (bx[1] + bx[3]) / 2, .1), (bx[2] - bx[0] + .4, bx[3] - bx[1] + .4, .2), 'ferro', 'mass', ch=.05)
        flat_block(k, bx, .2, 3.3)
    for bx, s, wins, drs in ((a, 'n', [-5.5, -1.5, 2.5, 6.0], [-7.2]), (a, 'w', [0.0], []), (a, 's', [-6.0, -2.0], []),
                             (b, 'e', [-1.0, 1.5, 4.0], [-3.6]), (b, 's', [-1.4, 1.6], []), (b, 'w', [-2.5, 2.5], [])):
        p, yaw, L = face(bx, s)
        with k.at(p, yaw=yaw):
            seams(k, L, .4, 3.2, 2.4)
            for xl in wins:
                window(k, xl, 1.95, 2.2, 1.4)   # generous glazing (owner: civilian = lots of glass)
            for dx in drs:
                door(k, dx)
                I.wall_lamp(k, dx + (1.0 if dx < 0 else -1.0), 2.6, yaw)
    k.box((-5.0, 5.0, 3.62), (1.8, 1.2, .6), 'paint.second', 'frame', ch=.05)
    k.cyl((-5.4, 5.0, 3.95), .4, .06, 'grate', 'frame', sides=14)
    k.cyl((5.3, -5.0, 3.62), .8, .6, 'paint.steel', 'frame', sides=16, top=.08)
    ip.hatch(k, 0.0, 4.6, 3.52)
    ip.hatch(k, 5.0, 0.0, 3.52)


def storycivliving2(k):
    """Two-storey colony house. Features: (1) a two-storey riveted plate block and a single-storey wing on one
    cast footing, pale parapets and gravel roofs; (2) the entrance door on the wing with canopy and lamp;
    (3) stacked framed windows on both storeys and a clerestory strip; (4) a storey band and plate seams;
    (5) a roof water tank and vent."""
    hi = (-4.3, -8.3, 4.3, 0.6)
    lo = (-3.6, 0.6, 3.6, 8.3)
    k.box((0, 0, .1), (9.0, 17.0, .2), 'ferro', 'mass', ch=.05)
    flat_block(k, hi, .2, 6.0)
    flat_block(k, lo, .2, 3.4)
    k.box((0, (hi[1] + hi[3]) / 2, 3.3), (8.75, 9.05, .2), 'paint.second', 'skin', ch=.04)
    for s in 'sew':
        p, yaw, L = face(hi, s)
        with k.at(p, yaw=yaw):
            seams(k, L, .4, 5.9, 2.2)
            for xl in ((-2.0, 2.0) if s == 's' else (-2.5, 2.5)):
                window(k, xl, 1.9, 1.6, 1.3)
                window(k, xl, 4.5, 1.6, 1.3)
    for s in 'ewn':
        p, yaw, L = face(lo, s)
        with k.at(p, yaw=yaw):
            seams(k, L, .4, 3.3, 2.2)
            if s == 'e':
                door(k, -1.2)
                I.wall_lamp(k, -.1, 2.6, yaw)
                window(k, 2.2, 1.9, 1.1, 1.0)
            else:
                window(k, 0.0 if s == 'n' else 1.4, 1.9, 1.1, 1.0)
    p, yaw, L = face(hi, 'n')
    with k.at((p[0], p[1], 3.6), yaw=yaw):
        for xl in (-1.6, 1.6):
            window(k, xl, 1.6, 1.4, .5, mull=False)
    k.cyl((2.4, -6.0, 6.55), .6, .7, 'paint.steel', 'frame', sides=14, top=.08)
    ip.mushroom_vent(k, -1.8, -3.0, 6.22)


def greenhouse(k):
    """Colony greenhouse. Features: (1) a long glazed hall with sloped glass sides on a low cast kerb (the stock
    trapezoid section), glazing bars over every pane; (2) a central ridge spine carrying five ventilation fan
    housings; (3) solid plate gable ends with a roll-up door and a personnel door; (4) planting benches inside."""
    W, L, hb, ht, wt = 16.4, 22.2, .5, 4.6, 5.2
    k.box((0, 0, .25), (W + .4, L + .4, .5), 'ferro', 'mass', ch=.06)
    for s in (-1, 1):                       # sloped glazing on a frame grid
        a, b = (s * W / 2, hb), (s * wt / 2, ht)
        n = 12
        for i in range(n + 1):
            y = -L / 2 + L * i / n
            k.beam((a[0], y, a[1]), (b[0], y, b[1]), .1, 'paint.frame', 'frame', h=.1)
        for z in (.33, .66):
            p = (a[0] + (b[0] - a[0]) * z, a[1] + (b[1] - a[1]) * z)
            k.beam((p[0], -L / 2, p[1]), (p[0], L / 2, p[1]), .06, 'paint.frame', 'frame', h=.06)
        k.prism(ccw([(a[0], a[1]), (b[0], b[1]), (b[0] - s * .04, b[1]), (a[0] - s * .04, a[1])]), -L / 2, L / 2, 'glass', 'glass',
                axis='y', pane=True)
        k.beam((a[0], -L / 2, a[1]), (a[0], L / 2, a[1]), .14, 'paint.frame', 'frame', h=.14)
    k.box((0, 0, ht + .1), (wt, L, .2), 'paint.frame', 'frame')
    k.box((0, 0, (ht + 4.85) / 2 + .1), (2.2, L, .45), 'paint.main', 'mass', ch=.08)
    for i in range(5):
        y = -L / 2 + L * (i + .5) / 5
        k.cyl((0, y, 5.0), .82, .2, 'paint.second', 'frame', sides=20, top=.05)
        k.cyl((0, y, 5.12), .7, .06, 'grate', 'frame', sides=20)
        k.cyl((0, y, 5.16), .16, .06, 'paint.steel', 'frame', sides=10)
    for s in (-1, 1):                       # solid plate gables
        poly = [(-W / 2, .5), (W / 2, .5), (wt / 2, ht + .2), (-wt / 2, ht + .2)]
        k.prism(ccw(poly), s * L / 2 - .15, s * L / 2 + .15, 'paint.main', 'mass', axis='y')
        with k.at((0, s * (L / 2 + .15), 0), yaw=0.0 if s < 0 else math.pi):
            with k.at((0, 0, .5)):
                ip.rollup_door(k, .8, 3.0, 3.2, -.2, hood=False, grade=.5, civil=True)
            with k.at((-3.4, 0, .5)):
                door(k, 0.0, .95, 2.1, canopy=False)
            k.slab(-1.0, 1.0, 3.75, 4.15, -.1, .02, 'paint.stencil', 'frame')
    for y in ip.frange(-L / 2 + 2.0, L / 2 - 2.0, 3.0):  # benches
        for x in (-4.6, 4.6):
            k.box((x, y, .95), (3.4, 2.0, .1), 'wood.crate', 'frame')
            k.box((x, y, .72), (3.0, 1.6, .44), 'paint.steel', 'frame')
            k.box((x, y, 1.08), (3.2, 1.8, .16), 'veg.grass', 'detail')


# ------------------------------------------------------------------ fences and street props
def plank_fence(k, L, h=2.6, diag=False):
    """Timber plank fence along y, exactly the stock run L (owner footprint rule): end posts on small footings flush
    with the run ends, one boarded panel between them on two rails, a capping rail; 0.28 m thick overall. The panel,
    rails and cap are single spans with vertices only at the run ends, like the stock plane, so the engine blocks the
    same cells at every rotation (it blocks the cells mesh vertices fall in); the board joints are surface detail."""
    for y in (-L / 2 + .14, L / 2 - .14):
        k.box((0, y, .1), (.28, .28, .2), 'ferro', 'mass', ch=.03)
        k.box((0, y, (h + .2) / 2 + .05), (.12, .12, h + .1), 'wood.crate', 'mass', top=.03)
    for z in (.6, h - .45):
        k.box((.075, 0, z), (.05, L - .02, .12), 'wood.crate', 'frame')
    k.box((.115, 0, (.12 + h) / 2), (.03, L - .3, h - .12), 'wood.plank', 'skin')
    nb = int(L / .16)
    for i in range(1, nb):
        y = -L / 2 + i * L / nb
        k.box((.132, y, (.12 + h) / 2), (.006, .012, h - .14), 'wood.crate', 'detail')
    k.box((.06, 0, h + .02), (.16, L, .06), 'wood.crate', 'frame')
    count(k, 'panels_sections', 1)


def woodenfence(k):
    """Timber plank fence (stock woodenfence): posts on small footings, two rails, close boards, a capping rail."""
    plank_fence(k, 10.843, 2.7)


def woodenfencediag(k):
    """The timber plank fence as a diagonal run over the stock diagonal length."""
    plank_fence(k, 15.226, 2.7)


def shantyfence(k):
    """Shanty fence (along x): salvaged corrugated sheets, plank panels and a door-sized plate lashed between
    leaning timber posts and a top wire, rust patches."""
    L, h = 10.648, 2.8               # the stock run exactly (owner footprint rule)
    rng = k.rng
    for i in range(6):
        x = -L / 2 + .14 + (L - .28) * i / 5
        k.box((x, 0, .15), (.28, .28, .3), 'ferro', 'mass', ch=.03)
        with k.at((x, 0, .3), roll=rng.uniform(-.03, .03)):
            k.box((0, 0, (h - .2) / 2), (.12, .12, h - .2), 'wood.crate', 'mass')
    mats = ['corr', 'wood.plank', 'paint.rust', 'corr', 'wood.plank']
    for i in range(5):
        x0, x1 = -L / 2 + L * i / 5 + .09, -L / 2 + L * (i + 1) / 5 - .09
        top = h - rng.uniform(.1, .5)
        k.box(((x0 + x1) / 2, .09, (.2 + top) / 2), (x1 - x0, .03, top - .2), mats[i], 'skin')
        k.box(((x0 + x1) / 2, .115, (.2 + top) / 2 + .3), (rng.uniform(.4, 1.0), .02, rng.uniform(.3, .7)), 'paint.rust', 'detail')
        for z in (.7, top - .3):
            k.box(((x0 + x1) / 2, .02, z), (x1 - x0 + .16, .06, .1), 'wood.crate', 'frame')
    ip.tube(k, [(-L / 2 + .14, 0, h + .05), (L / 2 - .14, 0, h + .05)], .015, 'metal', 'frame', sides=5)
    count(k, 'panels_sections', 5)


def dumpster(k):
    """Steel dumpster: a tapered blue skip on castors with ribbed sides, lifting pockets, two closed hinged lids
    and a stencil plate."""
    W, D = 2.9, 1.8
    k.prism(ccw([(-D / 2, .35), (D / 2, .35), (D / 2 + .1, 2.1), (-D / 2, 2.35)]), -W / 2, W / 2, 'paint.blue', 'mass', axis='x')
    for x in (-1.0, 0.0, 1.0):
        k.prism(ccw([(-D / 2 - .06, .4), (D / 2 + .06, .4), (D / 2 + .16, 2.05), (-D / 2 - .06, 2.3)]), x - .05, x + .05, 'paint.blue', 'frame',
                axis='x')
    for s in (-1, 1):
        k.box((s * (W / 2 + .1), 0, 1.6), (.2, .5, .3), 'paint.steel', 'frame')
        k.box((s * .9, 0, .2), (.3, D * .8, .1), 'paint.steel', 'frame')
        for y in (-.6, .6):
            k.cyl((s * .9, y, .12), .12, .1, 'rubber', 'frame', sides=10)
    k.prism(ccw([(-D / 2 - .05, 2.35), (D / 2 + .15, 2.1), (D / 2 + .15, 2.16), (-D / 2 - .05, 2.41)]), -W / 2, 0.0, 'paint.second', 'frame', axis='x')
    k.prism(ccw([(-D / 2 - .05, 2.35), (D / 2 + .15, 2.1), (D / 2 + .15, 2.16), (-D / 2 - .05, 2.41)]), .02, W / 2, 'paint.second', 'frame', axis='x')
    for x in (-.7, .7):
        k.box((x, -.1, 2.3), (.5, .08, .06), 'paint.steel', 'frame')
    with k.at((-.7, D / 2 + .05, 0), yaw=math.pi):
        k.slab(-.4, .4, 1.3, 1.7, -.02, .0, 'paint.stencil', 'frame')


def burningtrashbarrel(k):
    """Burning trash drum: a hazard-banded steel drum with rolling hoops and vent holes, glowing coals and
    flames licking over the rim."""
    r = .62
    k.cyl((0, 0, .65), r, 1.3, 'paint.accent', 'mass', sides=24, top=.03)
    for z in (.35, .95):
        k.cyl((0, 0, z), r + .03, .06, 'paint.steel', 'frame', sides=24)
    k.cyl((0, 0, .65), r + .005, .3, 'hazard', 'skin', sides=24)
    k.cyl((0, 0, 1.29), r * .92, .04, 'lamp', 'frame', sides=20)
    for i in range(7):
        a = i * math.tau / 7
        rr = r * (.2 + .55 * (i % 3) / 2)
        x, y = math.cos(a) * rr, math.sin(a) * rr
        h = .22 + .12 * (i % 3)
        M.loft(k, [[(x + .1 * math.cos(b), y + .1 * math.sin(b), 1.3) for b in [j * math.tau / 6 for j in range(6)]],
                   [(x + .01 * math.cos(b), y + .01 * math.sin(b), 1.3 + h) for b in [j * math.tau / 6 for j in range(6)]]], 'lamp', 'frame')


def road_sign(k, kind):
    """Road sign: a galvanised pole on a small cast footing, a reflective sign plate with a white border and a
    back bracket."""
    k.box((0, 0, .1), (.45, .45, .2), 'ferro', 'mass', ch=.05)
    top = 3.05 if kind != 'yield' else 2.75
    k.cyl((0, 0, (.2 + top) / 2), .045, top - .2, 'metal', 'mass', sides=10)
    k.cyl((0, 0, top + .03), .05, .06, 'paint.steel', 'frame', sides=10)
    with k.at((0, -.06, 0)):
        if kind == 'stop':
            octo = [(.45 * math.cos(math.pi / 8 + i * math.pi / 4), 2.9 + .45 * math.sin(math.pi / 8 + i * math.pi / 4)) for i in range(8)]
            k.prism(ccw(octo), -.02, .01, 'paint.stencil', 'frame', axis='y')
            k.prism(ccw([(x * .9, 2.9 + (z - 2.9) * .9) for x, z in octo]), -.03, -.02, 'paint.sign_red', 'frame', axis='y')
            k.slab(-.3, .3, 2.84, 2.96, -.04, -.03, 'paint.stencil', 'frame')
        elif kind == 'yield':
            tri = [(-.47, 3.05), (.47, 3.05), (0, 2.25)]
            k.prism(ccw(tri), -.03, .01, 'paint.sign_red', 'frame', axis='y')
            k.prism(ccw([(-.33, 2.97), (.33, 2.97), (0, 2.42)]), -.04, -.03, 'paint.sign_yellow', 'frame', axis='y')
        else:
            dia = [(0, 2.45), (.42, 2.87), (0, 3.29), (-.42, 2.87)]
            k.prism(ccw(dia), -.03, .01, 'paint.sign_orange', 'frame', axis='y')
            pts = [(.12 * math.sin(i * 1.1), -.04, 2.6 + i * .065) for i in range(11)]
            ip.tube(k, pts, .025, 'paint.dark', 'frame', sides=5)
    k.box((0, .0, 2.85), (.12, .1, .5), 'paint.steel', 'frame')


def stopsign(k):
    """Stop sign: an octagonal red plate with a white border and bar on a galvanised pole and footing."""
    road_sign(k, 'stop')


def yieldsign(k):
    """Yield sign: an inverted red-bordered yellow triangle on a galvanised pole and footing."""
    road_sign(k, 'yield')


def slipperysign(k):
    """Slippery road sign: an amber diamond with a black winding-road symbol on a galvanised pole and footing."""
    road_sign(k, 'slippery')


def beacon_marker(k, lamp):
    """Marker beacon (the approved redbeaconlight shape): a low faceted luminous wedge on a steel base."""
    base = [(-.25184, 0), (.25184, -.2908), (.25184, .2908)]
    k.prism(base, 0, .07, 'metal', 'frame')
    vv = [(x, y, .07) for x, y in base] + [(-.11084, 0, .71127), (.18134, -.16868, .71127), (.18134, .16868, .71127)]
    k.add(vv, [(2, 1, 0), (3, 4, 5), (0, 1, 4, 3), (1, 2, 5, 4), (2, 0, 3, 5)], f'{lamp}.marker', 'skin')


def greenbeaconlight(k):
    """Green marker beacon: the approved beacon wedge in green."""
    beacon_marker(k, 'lamp_green')


def bluebeaconlight(k):
    """Blue marker beacon: the approved beacon wedge in blue."""
    beacon_marker(k, 'lamp_blue')


SPECS = {
    'singlecivlivinga': dict(fn=singlecivlivinga, palette='intent_civ', cls='small', counts=dict(),
        intent=('Colony vault house. Features: (1) riveted plate walls on a cast footing with a glazed sunroom band round the '
                'ground floor; (2) an arched riveted roof with seam ribs and clerestory windows in both gables; (3) a two-stage '
                'brick chimney; (4) the side door with a canopy, lamp and step; (5) upper windows and roof vents.')),
    'singlecivlivingb': dict(fn=singlecivlivingb, palette='intent_civ', cls='small', counts=dict(),
        intent=('Long colony vault house: the vault house at twice the length, two brick chimneys, two side doors with '
                'canopies, rows of upper windows over the glazed sunroom band.')),
    'singlecivlivingc': dict(fn=singlecivlivingc, palette='intent_civ', cls='prop', counts=dict(), tol=dict(grid_mae=.3),
        intent=('Corrugated settler hut: a corrugated shell with one vertical wall and one curved shoulder, rust repair plates, a '
                'steel stovepipe with an elbow and rain cap, the gable door with a step and framed windows.')),
    'singlecivlivingd': dict(fn=singlecivlivingd, palette='intent_civ', cls='prop', counts=dict(),
        intent=('Prefab settler shack: a small plate box on a cast pad, a monopitch roof running out over a side porch on two '
                'posts, the gable door with framed windows either side, a brick flue, rust roof patches.')),
    '2civliving': dict(fn=civliving2, palette='intent_civ', cls='small', counts=dict(),
        intent=('L-plan colony row house: single-storey riveted plate prefab units on a cast footing with a pale parapet and '
                'gravel roof, two entrance doors with canopies and lamps, framed windows on every face, roof units.')),
    '2storycivliving': dict(fn=storycivliving2, palette='intent_civ', cls='small', counts=dict(),
        intent=('Two-storey colony house: a two-storey riveted plate block and a single-storey wing, pale parapets, the entrance '
                'on the wing, stacked framed windows, a storey band, a roof water tank.')),
    'greenhouse': dict(fn=greenhouse, palette='intent_civ', cls='small', counts=dict(),
        intent=('Colony greenhouse: a long glazed hall with sloped glass sides on a cast kerb, a ridge spine with five ventilation '
                'fans, solid plate gables with a roll-up and a personnel door, planting benches inside.')),
    'woodenfence': dict(fn=woodenfence, palette='intent_civ', cls='prop', counts=dict(), tol=dict(footprint=12.0, grid_mae=.5, centroid=.5),
        intent='Timber plank fence: posts on small footings, two rails, close vertical boards, a capping rail.'),
    'woodenfencediag': dict(fn=woodenfencediag, palette='intent_civ', cls='prop', counts=dict(), tol=dict(footprint=12.0, grid_mae=.5, centroid=.5),
        intent='The timber plank fence as a diagonal run over the stock diagonal length.'),
    'shantyfence': dict(fn=shantyfence, palette='intent_civ', cls='prop', counts=dict(), tol=dict(footprint=1000.0, grid_mae=.7, centroid=.5),
        intent='Shanty fence: salvaged corrugated sheets, plank panels and rust plates lashed between timber posts, a top wire.'),
    'dumpster': dict(fn=dumpster, palette='intent_civ', cls='prop', counts=dict(), tol=dict(grid_mae=.35),
        intent='Steel dumpster: a tapered blue skip on castors, ribbed sides, lifting pockets, two closed lids with handles, a stencil plate.'),
    'burningtrashbarrel': dict(fn=burningtrashbarrel, palette='intent_civ', cls='prop', counts=dict(), tol=dict(grid_mae=.5),
        intent='Burning trash drum: a hazard-banded steel drum with rolling hoops, glowing coals and flames over the rim.'),
    'stopsign': dict(fn=stopsign, palette='intent_civ', cls='prop', counts=dict(), tol=dict(footprint=1.0, grid_mae=.6, centroid=.3),
        intent='Stop sign: an octagonal red plate with a white border and bar on a galvanised pole and footing.'),
    'yieldsign': dict(fn=yieldsign, palette='intent_civ', cls='prop', counts=dict(), tol=dict(footprint=1.0, grid_mae=.6, centroid=.3),
        intent='Yield sign: an inverted red-bordered yellow triangle on a galvanised pole and footing.'),
    'slipperysign': dict(fn=slipperysign, palette='intent_civ', cls='prop', counts=dict(), tol=dict(footprint=1.0, grid_mae=.6, centroid=.3),
        intent='Slippery road sign: an amber diamond with a black winding-road symbol on a galvanised pole and footing.'),
    'greenbeaconlight': dict(fn=greenbeaconlight, palette='galvanised', cls='prop', counts=dict(),
        intent='Green marker beacon: the approved beacon wedge in green.'),
    'bluebeaconlight': dict(fn=bluebeaconlight, palette='galvanised', cls='prop', counts=dict(),
        intent='Blue marker beacon: the approved beacon wedge in blue.'),
}
