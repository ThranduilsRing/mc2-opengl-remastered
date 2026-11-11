"""Military base family (split from intent_military.py): field group (tents, bunkers, crates), armoured halls, command group (turret control, relay, sensor tower)."""

import math

import geo
import ind_parts as ip
import intent as I
import intent_power as PW

from intent_military_parts import _null0, anim_node, antenna, bag_run, batter_mass, blast_door, count, dish, frame_of, loft, on_face, radar_dipole, rring, slit_window, spin_antenna, stencil, stencil_panel


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
