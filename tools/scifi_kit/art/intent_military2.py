"""Military base family, batch 8 (owner queue 2026-10-24): faction barracks, the generic repair building, the diagonal
walls and fences, and the field props. Built by intent from the stock features (stock views read first; stock sizes
are an envelope only), in the BattleTech base language: cast walls, faction colour on bands and trim, pristine.
Moving stock parts are separate glTF nodes named like the stock node (milcheckpoint gate_b > gate_a, the
generic_rpbuilding doors)."""
import math

import geo
import ind_parts as ip
import intent as I
import intent_military as M
import intent_power as PW

count = I.count


def ccw(poly):
    a = sum(p[0] * q[1] - q[0] * p[1] for p, q in zip(poly, poly[1:] + poly[:1]))
    return poly if a > 0 else poly[::-1]


def gable_hall(k, x0, x1, y0, y1, zf, eave, ridge, wall='ferro', roof='paint.roof', ov=.6, t=.22):
    """Cast hall body with gables (ridge along x) and a pitched standing-seam roof with overhangs."""
    yc = (y0 + y1) / 2
    k.prism(ccw([(y0, zf), (y1, zf), (y1, eave), (yc, ridge), (y0, eave)]), x0, x1, wall, 'mass', axis='x')
    half = (y1 - y0) / 2
    sl = (ridge - eave) / half
    for s in (-1, 1):
        ye = yc + s * (half + ov)
        ze = eave - ov * sl
        k.prism(ccw([(yc, ridge), (ye, ze), (ye, ze + t), (yc, ridge + t)]), x0 - ov * .8, x1 + ov * .8, roof, 'roof', axis='x')
        n = int((x1 - x0 + ov * 1.6) / .9)
        for i in range(n + 1):           # standing seams
            x = x0 - ov * .8 + (x1 - x0 + ov * 1.6) * i / n
            k.beam((x, ye, ze + t), (x, yc, ridge + t), .05, roof, 'detail', h=.06)
        k.beam((x0 - ov * .8, ye, ze + t * .5), (x1 + ov * .8, ye, ze + t * .5), .08, 'paint.trim', 'frame', h=.24)   # fascia
    k.beam((x0 - ov * .8, yc, ridge + t + .05), (x1 + ov * .8, yc, ridge + t + .05), .3, 'paint.trim', 'frame', h=.14)  # ridge cap


def punched(k, box, s, xs, zc, w, h, z0=0.0):
    """Armoured punched windows on side s of a plan box at local positions xs (slit_window frames)."""
    o, yaw, _ = M.frame_of(box, s)
    with k.at((o[0], o[1], z0), yaw=yaw):
        for xl in xs:
            M.slit_window(k, xl, zc, w, h)


def wall_band(k, box, z0, z1, mat, proud=.06):
    x0, y0, x1, y1 = box
    k.box(((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2), (x1 - x0 + 2 * proud, y1 - y0 + 2 * proud, z1 - z0), mat, 'skin', ch=.06)


def steps(k, x, y, yaw, w, rise, tread=.32, mat='ferro'):
    """Cast steps from grade up to a door sill at `rise`, climbing toward local +y (into the wall) in a wall frame."""
    n = max(1, round(rise / .18))
    with k.at((x, y, 0), yaw=yaw):
        for i in range(n):
            z1 = rise * (i + 1) / n
            k.slab(-w / 2, w / 2, 0, z1, -(n - i) * tread, -(n - i - 1) * tread, mat, 'mass', walk=True)
        for s in (-1, 1):                 # cheek walls
            k.slab(s * w / 2 - .11, s * w / 2 + .11, 0, rise + .25, -n * tread, 0, 'paint.plate', 'frame')


# ------------------------------------------------------------------ s_barracks
def s_barracks(k):
    """Steiner troop barracks. Features: (1) a long cast hall on a raised plinth under a steep standing-seam gable roof
    (ridge along the length); (2) a projecting gabled entrance porch on the north side with a blast door and broad
    cast steps; (3) armoured windows along both long sides above a blue-grey faction band; (4) a louvred vent high in
    each gable and a side door at the west gable reached by a ramp; (5) ridge ventilators and a flue."""
    W, B = 'ferro', 'paint.second'
    x0, x1, y0, y1 = -11.0, 11.0, -4.3, 4.3
    zf, eave, ridge = .9, 4.7, 8.85
    k.box((0, 0, zf / 2), (x1 - x0 + .5, y1 - y0 + .5, zf), 'paint.plate', 'mass', ch=.12, top=.05)
    gable_hall(k, x0, x1, y0, y1, zf, eave, ridge, W)
    box = (x0, y0, x1, y1)
    wall_band(k, box, zf + .05, zf + .45, B)
    wall_band(k, box, eave - .35, eave - .05, B)
    punched(k, box, 's', [-8.0, -4.0, 0.0, 4.0, 8.0], 2.9, 1.5, .8)
    punched(k, box, 'n', [-8.0, -4.4, 4.4, 8.0], 2.9, 1.5, .8)
    # north porch: a small gabled block in front of the blast door, steps down to grade
    pb = (-2.2, y1, 2.2, y1 + 1.6)
    gable_hall_y(k, pb, zf, 3.9, 5.4, W)
    with k.at((0, y1 + 1.6, zf), yaw=math.pi):
        M.blast_door(k, 1.8, 2.5, depth=.3, back=.3, fw=.32, yaw=math.pi, label='12')
    steps(k, 0, y1 + 1.6 + .25, math.pi, 3.6, zf)
    # west gable door, landing and ramp along the gable
    with k.at((x0, 1.6, zf), yaw=-math.pi / 2):
        M.blast_door(k, 1.4, 2.3, depth=.3, back=.3, fw=.28, yaw=-math.pi / 2, sill=False)
    k.box((x0 - 1.1, 1.6, zf / 2), (2.2, 2.4, zf), 'ferro', 'mass', walk=True, ch=.05)
    k.prism(ccw([(-4.4, 0), (.4, 0), (.4, zf), (0, zf)]), x0 - 2.2, x0 - .02, 'ferro', 'mass', axis='x', walk=True)
    ip.railing(k, [(x0 - 2.15, -4.3), (x0 - 2.15, 2.75), (x0 - .1, 2.75)], zf, h=1.0, post=1.4)
    # gable vents and ridge ventilators
    for s, x in ((-1, x0), (1, x1)):
        with k.at((x, 0, 0), yaw=s * math.pi / 2):
            k.slab(-1.1, 1.1, 5.7, 6.7, -.14, .02, 'paint.trim', 'frame', front=.02)
            for i in range(5):
                z = 5.8 + i * .18
                k.slab(-1.0, 1.0, z, z + .1, -.2, -.1, 'grate', 'frame')
        count(k, 'windows')
    for x in (-6.0, 0.0, 6.0):
        k.cyl((x, 0, ridge + .45), .45, .55, 'paint.second', 'frame', sides=14, top=.08)
        k.cyl((x, 0, ridge + .9), .62, .12, 'paint.trim', 'frame', sides=14, top=.04)
    k.cyl((8.6, -2.2, 7.4), .22, 2.2, 'paint.steel', 'frame', sides=10)
    k.cyl((8.6, -2.2, 8.55), .32, .15, 'paint.trim', 'frame', sides=10)
    with k.at((x1, -3.0, 0), yaw=math.pi / 2):
        M.stencil_panel(k, '12', 0, 2.4, .6, -.02)


def gable_hall_y(k, box, zf, eave, ridge, wall):
    """Small gabled block with its ridge along y (porches)."""
    x0, y0, x1, y1 = box
    xc = (x0 + x1) / 2
    k.prism(ccw([(x0, zf), (x1, zf), (x1, eave), (xc, ridge), (x0, eave)]), y0 - .02, y1, wall, 'mass', axis='y')
    k.box((xc, (y0 + y1) / 2, zf / 2), (x1 - x0 + .3, y1 - y0, zf), 'paint.plate', 'mass', ch=.06)
    half = (x1 - x0) / 2
    sl = (ridge - eave) / half
    for s in (-1, 1):
        xe = xc + s * (half + .4)
        ze = eave - .4 * sl
        k.prism(ccw([(xc, ridge), (xe, ze), (xe, ze + .2), (xc, ridge + .2)]), y0 - .1, y1 + .4, 'paint.roof', 'roof', axis='y')


# ------------------------------------------------------------------ s_barracks2
def s_barracks2(k):
    """Steiner raised barracks. Features: (1) a long cast dormitory block lifted on cast columns over an open
    undercroft (sheltered vehicle parking); (2) a railed steel access gallery along the east side serving three
    blast doors, reached by a stair at the south end; (3) armoured windows between the doors and along the west side;
    (4) a crenellated plate parapet over a blue-grey faction band; (5) roof vents and a stencilled unit plate."""
    W, B = 'ferro', 'paint.second'
    bx = (-4.0, -13.6, 2.6, 13.6)
    zf, zt = 3.6, 6.2
    k.box((-0.7, 0, zf - .2), (6.9, 27.6, .4), 'paint.plate', 'mass', ch=.1)          # floor slab
    for x in (-3.5, 2.1):
        for y in (-12.9, -8.6, -4.3, 0.0, 4.3, 8.6, 12.9):
            k.box((x, y, (zf - .4) / 2), (.6, .6, zf - .4), W, 'mass', ch=.08)
            k.box((x, y, .1), (.9, .9, .2), 'paint.plate', 'frame', ch=.05)
    k.box((-0.7, 0, (zf + zt) / 2), (6.6, 27.2, zt - zf), W, 'mass', ch=.12)
    wall_band(k, bx, zf, zf + .35, B)
    wall_band(k, bx, zt - .55, zt - .2, B)
    # crenellated parapet: plate merlons on a coping
    k.box((-0.7, 0, zt + .1), (6.9, 27.5, .2), 'paint.plate', 'frame', ch=.1)
    for x in (-3.95, 2.55):
        for i in range(12):
            y = -13.0 + 26.0 * i / 11
            k.box((x, y, zt + .4), (.3, .9, .4), 'paint.plate', 'frame', ch=.04)
    k.box((-0.7, 0, zt + .22), (6.0, 26.6, .04), 'gravel', 'roof', walk=True)
    M.roof_kit(k, [(-1.8, -8.0), (-1.8, 0.0), (-1.8, 8.0)], [(0.6, 4.0)], zt + .22)
    # east gallery and doors
    ip.rect_platform(k, 2.6, -10.2, 4.4, 13.4, zf, rail_sides='en', legs=False, gaps=[(4.4, -10.2)], name='door gallery')
    ip.railing(k, [(4.4, -10.2), (2.6, -10.2)], zf, gaps=[(3.5, -10.2)])
    for y in (-8.6, 0.0, 8.6):
        k.beam((4.3, y, zf - .2), (2.65, y, zf - 1.6), .12, 'paint.steel', 'frame', h=.12)
    for yl in (-6.0, 2.6, 11.2):
        with k.at((2.6, yl, zf), yaw=math.pi / 2):
            M.blast_door(k, 1.2, 2.2, depth=.25, back=.2, fw=.22, yaw=math.pi / 2, sill=False, lamp=False)
            I.wall_lamp(k, 0, 2.75, math.pi / 2)
    o, yaw, _ = M.frame_of(bx, 'e')
    with k.at((o[0], o[1], zf), yaw=yaw):
        for xl in (-11.0, -2.0, 6.6):
            M.slit_window(k, xl, 1.6, 1.4, .5)
    o, yaw, _ = M.frame_of(bx, 'w')
    with k.at((o[0], o[1], zf), yaw=yaw):
        for xl in (-10.5, -6.0, -1.5, 3.0, 7.5, 12.0):
            M.slit_window(k, xl, 1.6, 1.4, .5)
    ip.stair(k, (3.5, -14.6, 0.0), (3.5, -10.2, zf), w=1.0, name='gallery stair')
    count(k, 'stairs_ladders')
    with k.at((-0.7, -13.6, zf), yaw=0.0):
        M.stencil_panel(k, '7', 0, 1.6, .7, -.02)


# ------------------------------------------------------------------ l_elitebarracks
def pagoda_roof(k, box, z0, z1, ov=1.0, lift=.45, mat='paint.roof', ridge_mat='paint.second'):
    """Hipped roof with upturned eave corners and a ridge along the long axis (Liao)."""
    x0, y0, x1, y1 = box
    ring0 = [(x0 - ov, y0 - ov), (x1 + ov, y0 - ov), (x1 + ov, y1 + ov), (x0 - ov, y1 + ov)]
    eave = []
    for (x, y) in ring0:                 # chamfered corners lifted
        sx, sy = (1 if x > 0 else -1), (1 if y > (y0 + y1) / 2 else -1)
        eave += [(x - sx * .9, y, z0 + lift * .5), (x + sx * .15, y + sy * .15, z0 + lift), (x, y - sy * .9, z0 + lift * .5)]
    eave = eave[1:] + eave[:1]
    # order: walk the ring CCW starting mid-south
    pts0 = [(x0 - ov + .9, y0 - ov, z0), (x1 + ov - .9, y0 - ov, z0), (x1 + ov + .15, y0 - ov - .15, z0 + lift),
            (x1 + ov, y0 - ov + .9, z0), (x1 + ov, y1 + ov - .9, z0), (x1 + ov + .15, y1 + ov + .15, z0 + lift),
            (x1 + ov - .9, y1 + ov, z0), (x0 - ov + .9, y1 + ov, z0), (x0 - ov - .15, y1 + ov + .15, z0 + lift),
            (x0 - ov, y1 + ov - .9, z0), (x0 - ov, y0 - ov + .9, z0), (x0 - ov - .15, y0 - ov - .15, z0 + lift)]
    up = [(x, y, z + .25) for x, y, z in pts0]
    hx = (x1 - x0) / 2
    yc0, yc1 = y0 + hx * .9, y1 - hx * .9
    xc = (x0 + x1) / 2
    top = [(xc - .3, yc0, z1), (xc + .3, yc0, z1), (xc + .3, yc0, z1), (xc + .3, yc0 + .01, z1), (xc + .3, yc1 - .01, z1),
           (xc + .3, yc1, z1), (xc + .3, yc1, z1), (xc - .3, yc1, z1), (xc - .3, yc1, z1), (xc - .3, yc1 - .01, z1),
           (xc - .3, yc0 + .01, z1), (xc - .3, yc0, z1)]
    M.loft(k, [pts0, up, top], mat, 'roof')
    k.box((xc, (y0 + y1) / 2, z1 + .12), (.7, yc1 - yc0 + .6, .3), ridge_mat, 'frame', ch=.1)
    for y in (yc0 - .1, yc1 + .1):
        k.cyl((xc, y, z1 + .45), .2, .5, ridge_mat, 'frame', sides=10, top=.08)


def l_elitebarracks(k):
    """Liao elite barracks. Features: (1) a long battered cast podium in Liao green plate with a proud plinth;
    (2) two stepped dark-green cornice tiers over the podium (the stock layered eaves) carrying a terrace; (3) a
    narrower upper dormitory storey with a continuous row of red-framed windows; (4) a hipped pagoda roof with
    upturned eave corners, ridge and finials; (5) blast doors at both ends and a canopied main entrance with the
    Liao emblem on the east side."""
    pb = (-9.0, -25.8, 9.0, 25.8)
    ins = M.batter_mass(k, pb, 0.0, 5.0, .5, plinth=.6, cap=.2, ch=.4, mat='paint.main', plinth_mat='ferro', proud=.2)
    for z, d in ((5.0, .55), (5.8, .3)):
        k.box((0, 0, z + .2), (18.0 + 2 * d - 1.0, 51.6 + 2 * d - 1.0, .4), 'paint.plate', 'frame', ch=.2, top=.12)
        k.box((0, 0, z + .5), (18.0 + 2 * d - 1.4, 51.6 + 2 * d - 1.4, .2), 'paint.second', 'skin', ch=.18)
    k.box((0, 0, 5.9), (16.8, 50.4, .6), 'paint.main', 'mass', ch=.3)
    k.box((0, 0, 6.24), (16.0, 49.6, .04), 'gravel', 'roof', walk=True)
    ub = (-4.4, -20.0, 4.4, 20.0)
    k.box((0, 0, 6.2 + 2.6), (8.8, 40.0, 5.2), 'ferro', 'mass', ch=.15)
    k.box((0, 0, 6.35), (9.1, 40.3, .3), 'paint.plate', 'frame', ch=.1)
    k.box((0, 0, 10.9), (9.2, 40.4, .4), 'paint.second', 'skin', ch=.1)
    for s in 'ew':
        o, yaw, L = M.frame_of(ub, s)
        with k.at((o[0], o[1], 0), yaw=yaw):
            n = 14
            for i in range(n):
                xl = -L / 2 + 1.6 + (L - 3.2) * (i + .5) / n
                k.slab(xl - .7, xl + .7, 7.6, 9.9, -.12, .02, 'paint.valve', 'frame', front=.02)
                k.slab(xl - .55, xl + .55, 7.75, 9.75, -.15, -.11, 'glass', 'glass', pane=True)
                k.slab(xl - .55, xl + .55, 8.95, 9.05, -.18, -.12, 'paint.valve', 'frame')
                k.slab(xl - .04, xl + .04, 7.75, 9.75, -.18, -.12, 'paint.valve', 'frame')
            count(k, 'windows', n)
    for s in 'sn':
        o, yaw, L = M.frame_of(ub, s)
        with k.at((o[0], o[1], 0), yaw=yaw):
            k.slab(-1.2, 1.2, 7.6, 9.9, -.12, .02, 'paint.valve', 'frame', front=.02)
            k.slab(-1.05, 1.05, 7.75, 9.75, -.15, -.11, 'glass', 'glass', pane=True)
        count(k, 'windows')
    pagoda_roof(k, ub, 11.1, 13.3, ov=1.3)
    # podium: slit windows, end blast doors, east main entrance with a canopy and emblem
    for s in 'ew':
        for xl in (-20.0, -15.0, -10.0, 10.0, 15.0, 20.0):
            with M.on_face(k, pb, s, xl, 3.0, ins):
                M.slit_window(k, 0, 0, 1.8, .5)
    for s in 'sn':
        o, yaw, _ = M.frame_of(pb, s)
        with k.at((o[0], o[1], 0), yaw=yaw):
            M.blast_door(k, 3.0, 3.4, depth=.35, back=.6, fw=.45, yaw=yaw)
    o, yaw, _ = M.frame_of(pb, 'e')
    with k.at((o[0], o[1], 0), yaw=yaw):
        M.blast_door(k, 2.4, 3.0, depth=.35, back=.6, fw=.4, yaw=yaw, lamp=False)
        k.slab(-3.2, 3.2, 4.2, 4.45, -2.6, 0.3, 'paint.second', 'frame', front=.05)
        for xl in (-2.9, 2.9):
            k.slab(xl - .18, xl + .18, 0, 4.2, -2.5, -2.14, 'paint.plate', 'mass')
        I.wall_lamp(k, 0, 4.0, yaw)
    M.emblem(k, (9.0 + .02, 6.5, 3.0), math.pi / 2, .9, ring='paint.second', disc='paint.plate')
    M.emblem(k, (9.0 + .02, -6.5, 3.0), math.pi / 2, .9, ring='paint.second', disc='paint.plate')


# ------------------------------------------------------------------ generic_rpbuilding
def generic_rpbuilding(k):
    """Generic vehicle repair building. Features: (1) a long low cast hall with sloped corner buttresses and a plate
    coping; (2) a drive-through vertical-lift bay door at each end under a hazard-striped portal (stock door01 /
    Doors03 nodes, lifting); (3) two lift-up service hatches on each long side carried by one node (stock Doors02);
    (4) a raised roof monitor with louvres and a gantry rail; (5) an external service pipe run along the base."""
    W, B = 'ferro', 'paint.main'
    hb = (-13.5, -10.0, 13.5, 10.0)
    H = 6.8
    M.block(k, hb, 0, H, W, B, bands=((.7, 1.1),))
    for sx in (-1, 1):                    # corner buttresses, sloped
        for sy in (-1, 1):
            x, y = sx * 13.5, sy * 10.0
            with k.at((x, y, 0), yaw=0.0 if sx > 0 else math.pi):
                k.prism(ccw([(0, 0), (2.1, 0), (2.1, .7), (.35, H - .3), (0, H - .3)]), sy * 1.6 - .55, sy * 1.6 + .55,
                        'paint.plate', 'mass', axis='y')
            with k.at((x, y, 0), yaw=math.pi / 2 if sy > 0 else -math.pi / 2):
                k.prism(ccw([(0, 0), (2.1, 0), (2.1, .7), (.35, H - .3), (0, H - .3)]), -sx * 1.6 - .55, -sx * 1.6 + .55,
                        'paint.plate', 'mass', axis='y')
    # end bay doors: lifting leaves (stock door01 north, Doors03 south)
    for s, node, yv in (('n', 'door01', 10.0), ('s', 'Doors03', -10.0)):
        o, yaw, _ = M.frame_of(hb, s)
        with k.at((o[0], o[1], 0), yaw=yaw):
            for a, b in ((-3.6, -2.9), (2.9, 3.6)):
                k.slab(a, b, 0, 6.1, -.6, .1, 'paint.plate', 'frame', front=.05)
                k.slab(a + .02, b - .02, 0, 1.4, -.62, -.6, 'hazard', 'detail')
            k.slab(-3.6, 3.6, 5.8, 6.5, -.6, .1, 'paint.plate', 'frame', front=.05)
            k.slab(-3.6, 3.6, 5.82, 6.1, -.62, -.6, 'hazard', 'detail')
            k.slab(-3.4, 3.4, 0, .05, -2.0, .0, 'hazard', 'frame', walk=True)
            I.wall_lamp(k, 4.4, 5.4, yaw)
        # stock node pivot (anim_parity): the stock clip writes absolute translations from this common origin
        with M.anim_node(k, node, (-0.040, -0.043, 3.555), axis=(0.0, 0.0, 1.0)):
            with k.at((o[0], o[1], 0), yaw=yaw):
                n = 10
                for i in range(n):
                    z0 = 5.8 * i / n
                    k.slab(-2.9, 2.9, z0 + .01, z0 + 5.8 / n - .01, -.36, -.28, 'paint.door', 'skin', front=.012,
                           door=(i == 0), feature='door' if i == 0 else None)
                k.slab(-2.9, 2.9, .05, .6, -.4, -.36, 'hazard', 'detail')
                for j in range(4):
                    xx = -2.9 + 5.8 * (.2 + .2 * j)
                    k.slab(xx - .3, xx + .3, 3.2, 3.55, -.38, -.35, 'glass', 'glass')
        count(k, 'doors')
    # long-side service hatches, all four lift on one node (stock Doors02)
    with M.anim_node(k, 'Doors02', (-0.040, -0.043, 3.559), axis=(0.0, 0.0, 1.0)):
        for s in 'ew':
            o, yaw, _ = M.frame_of(hb, s)
            with k.at((o[0], o[1], 0), yaw=yaw):
                for xl in (-4.2, 4.2):
                    k.slab(xl - 2.4, xl + 2.4, 2.2, 6.1, -.3, -.22, 'paint.door', 'skin', front=.012)
                    for z in (3.2, 4.2, 5.2):
                        k.slab(xl - 2.3, xl + 2.3, z - .05, z + .05, -.34, -.3, 'paint.trim', 'frame')
    for s in 'ew':
        o, yaw, _ = M.frame_of(hb, s)
        with k.at((o[0], o[1], 0), yaw=yaw):
            for xl in (-4.2, 4.2):
                k.slab(xl - 2.7, xl + 2.7, 6.1, 6.45, -.4, .05, 'paint.plate', 'frame', front=.04)
                for a in (xl - 2.7, xl + 2.4):
                    k.slab(a, a + .3, 2.0, 6.2, -.4, .05, 'paint.plate', 'frame', front=.04)
                k.slab(xl - 2.4, xl + 2.4, 2.0, 2.2, -.42, .05, 'hazard', 'frame')
            M.blast_door(k, 1.2, 2.2, xc=0.0, depth=.3, back=.3, fw=.24, yaw=yaw, lamp=True)
            count(k, 'windows', 0)
    # roof monitor with louvres, gantry rail, vents
    k.box((0, 0, H + .8), (14.0, 5.0, 1.6), W, 'mass', ch=.12)
    k.box((0, 0, H + 1.65), (14.4, 5.4, .14), 'paint.plate', 'frame', ch=.06)
    k.box((0, 0, H + 1.73), (13.4, 4.4, .04), 'gravel', 'roof', walk=True)
    for s in (-1, 1):
        with k.at((0, s * 2.5, H), yaw=0.0 if s < 0 else math.pi):
            for i in range(6):
                xl = -5.6 + 2.24 * i
                k.slab(xl - .9, xl + .9, .35, 1.3, -.1, .02, 'paint.trim', 'frame', front=.02)
                for j in range(3):
                    k.slab(xl - .82, xl + .82, .45 + j * .27, .55 + j * .27, -.16, -.08, 'grate', 'frame')
    M.clutter(k, (-12.5, -9.0, -7.5, 9.0), H, 2, 3)
    M.clutter(k, (7.5, -9.0, 12.5, 9.0), H, 2, 1)
    # base service pipe run along the west and east faces on stools
    for s in (-1, 1):
        x = s * 13.85
        ip.pipe(k, [(x, -8.4, .55), (x, -1.6, .55)], .14, 'paint.pipe', end_flanges=True)
        ip.pipe(k, [(x, 1.6, .55), (x, 8.4, .55)], .14, 'paint.pipe', end_flanges=True)
        for y in (-7.4, -3.0, 3.0, 7.4):
            k.box((x, y, .2), (.3, .3, .4), 'paint.steel', 'frame')
    with k.at((0, -10.0, 0), yaw=0.0):
        M.stencil(k, '4', 5.6, 4.6, 1.2, -.02)


# ------------------------------------------------------------------ diagonal walls and fences
def s_lightwalldiag(k):
    """The approved Steiner light wall as a diagonal run: the same cast units, battens, coping and footing over the
    stock diagonal length (12 units), the terminal pier turned 45 degrees so it stays square to the map like the
    straight walls' piers."""
    import drafts
    drafts.light_wall(k, y_start=-24.27, py=21.2, units=12, diag=True)
    count(k, 'panels_sections', 12)


def razorwirefencediag(k):
    """The approved razor-wire fence as a diagonal run: the same posts, strands, braces and concertina crown over
    the stock diagonal length, in two sections with a middle post."""
    import drafts
    drafts.fence(k, L=15.239 - .72, sections=2)        # footings 0.72 at each end: total run = the stock 15.239 m
    count(k, 'panels_sections', 2)


def wirefencediag(k):
    """The approved chain-link fence as a diagonal run: the same posts, top rail and diamond mesh over the stock
    diagonal length, in three panels."""
    import batch3
    batch3.wirefence(k, d=15.24, panels=3)
    count(k, 'panels_sections', 3)


# ------------------------------------------------------------------ field props
def milcheckpoint(k):
    """Military checkpoint barrier. Features: (1) a cast guard post with a slit window, lamp and intercom; (2) a
    two-section folding boom (stock gate_b on the post, gate_a folding on it) in hazard stripes with a counterweight;
    (3) a rest post with a fork at the far end; (4) a hazard kerb strip and stop sign plate."""
    k.box((-3.6, 0, .9), (1.0, .85, 1.8), 'ferro', 'mass', ch=.08, top=.05)
    k.box((-3.6, 0, 1.86), (1.15, 1.0, .12), 'paint.plate', 'frame', ch=.04)
    with k.at((-3.6, -.425, 0), yaw=0.0):
        M.slit_window(k, 0, 1.45, .6, .22, d=.1)
        k.slab(-.3, .3, .5, .9, -.04, .0, 'paint.second', 'frame')
    k.cyl((-3.6, 0, 2.0), .09, .16, 'lamp_red', 'frame', sides=10)
    k.box((-3.6, 0, .03), (1.2, .8, .06), 'hazard', 'frame')
    k.box((-3.05, -.03, 1.18), (.2, .3, .5), 'paint.trim', 'frame', ch=.03)     # hinge bracket
    with M.anim_node(k, 'gate_b', (-3.05, -.03, 1.18), axis=(0.0, -1.0, 0.0)):
        k.box((-1.35, -.03, 1.18), (3.4, .12, .16), 'hazard', 'mass', ch=.03)
        k.box((-3.25, -.03, 1.18), (.3, .2, .3), 'paint.steel', 'frame', ch=.03)
    with M.anim_node(k, 'gate_a', (.33, -.03, 1.18), parent='gate_b', axis=(0.0, -1.0, 0.0)):
        k.box((2.2, -.03, 1.18), (3.7, .1, .14), 'hazard', 'mass', ch=.03)
        k.box((.33, -.03, 1.18), (.18, .16, .2), 'paint.trim', 'frame', ch=.02)
        k.box((4.0, -.03, 1.18), (.12, .14, .18), 'lamp_red', 'frame')
    k.box((3.8, -.03, .5), (.14, .14, 1.0), 'paint.steel', 'mass')
    for s in (-1, 1):
        k.box((3.8, -.03 + s * .1, 1.05), (.08, .04, .2), 'paint.steel', 'frame')
    k.box((3.8, -.03, .03), (.4, .4, .06), 'paint.plate', 'frame')


def sawhorsebarricade(k):
    """Sawhorse barricade. Features: (1) a hazard-striped top board; (2) splayed steel A-frame legs at each end;
    (3) a lower cross board; (4) an amber warning lamp; (5) sandbags weighting the feet."""
    for x in (-.75, .75):
        for s in (-1, 1):
            k.beam((x, 0, 1.2), (x, s * .62, .02), .06, 'paint.steel', 'frame', h=.06)
        k.box((x, 0, 1.22), (.1, .2, .1), 'paint.steel', 'frame')
    k.box((0, -.04, 1.2), (2.0, .05, .28), 'hazard', 'frame', ch=.01)
    k.box((0, 0, 1.36), (2.0, .2, .04), 'paint.plate', 'frame')
    k.box((0, -.32, .5), (1.8, .04, .18), 'hazard', 'frame')
    k.box((0, .32, .5), (1.8, .04, .18), 'paint.plate', 'frame')
    k.cyl((.85, 0, 1.42), .07, .06, 'paint.trim', 'frame', sides=10)
    k.cyl((.85, 0, 1.5), .06, .1, 'lamp', 'frame', sides=10)
    for x in (-.75, .75):
        for s in (-1, 1):
            M.sandbag(k, (x, s * .62, 0), 0.0, L=.5, W=.3, H=.18)


def htent(k):
    """Field hospital tent: the approved army frame tent with white roundels and red crosses on both roof slopes and
    both sides, framed gable doors, straps, guy lines and a timber deck."""
    M.army_tent(k, 9.6, 8.4, 4.0, 1.75)
    W, H, eave = 8.4, 4.0, 1.75
    zf = .22
    for s in (-1, 1):
        # roof slope roundel: a disc lying on the slope (rise from eave to ridge over W/2)
        ang = math.atan2(H - eave, W / 2)
        for y in (-2.4, 2.4):
            with k.at((s * W * .27, y, (eave + H) / 2 + .22), yaw=0.0, roll=s * ang):
                cross(k, 1.05)
    count(k, 'emblems', 2)


def cross(k, r):
    """White roundel with a red cross, lying in the local xy plane (normal +z)."""
    k.cyl((0, 0, 0), r, .06, 'paint.stencil', 'frame', sides=24)
    k.box((0, 0, .05), (r * 1.3, r * .36, .04), 'paint.valve', 'frame')
    k.box((0, 0, .05), (r * .36, r * 1.3, .04), 'paint.valve', 'frame')


def hqtent(k):
    """Field HQ tent: a long army frame tent with a perpendicular briefing wing (the stock T plan), framed doors at
    the outer ends, a whip antenna mast with guy lines, and a field generator with a cable run."""
    with k.at((0, -1.6, 0), yaw=math.pi / 2):
        M.army_tent(k, 19.6, 8.4, 3.9, 1.7, vents=3)
    with k.at((0, 4.6, 0)):
        M.army_tent(k, 5.4, 7.6, 3.6, 1.6, door_ends=('n',))
    k.cyl((8.6, 3.4, 2.05), .06, 4.1, 'metal', 'frame', sides=8)
    k.box((8.6, 3.4, .1), (.5, .5, .2), 'paint.plate', 'frame')
    for a in (0.5, 2.6, 4.7):
        ip.tube(k, [(8.6, 3.4, 3.6), (8.6 + 1.6 * math.cos(a), 3.4 + 1.6 * math.sin(a), .05)], .01, 'canvas.sand', 'frame',
                sides=4, caps=False)
    count(k, 'antennas_dishes')
    k.box((-8.4, 3.6, .55), (1.6, 1.0, 1.1), 'paint.second', 'frame', ch=.06, top=.05)
    k.cyl((-8.0, 3.6, 1.2), .3, .2, 'grate', 'frame', sides=12)
    k.box((-8.4, 3.6, .04), (1.9, 1.3, .08), 'paint.steel', 'frame')
    ip.tube(k, [(-7.6, 3.6, .3), (-6.4, 3.0, .05), (-4.4, 3.0, .05)], .03, 'paint.trim', 'frame', sides=6)


def camonetting(k):
    """Camouflage net (owner rework). Features: (1) a draped net canopy on four corner poles, sagging to the centre and
    dipping between the poles along every edge (the stock V profile), on the exact stock footprint; (2) the net is a
    lumpy garnished surface, not a sheet: a rolling base mesh carrying a dense layer of irregular scrim tufts in
    three camouflage colours, tilted and overlapping, some hanging below the edges as torn strips; (3) spreader caps
    on the pole heads; (4) guy lines from each pole head to a stake at the footprint edge."""
    X, Y = 4.36, 7.82                    # stock net extents
    px, py = 4.15, 7.6                   # pole positions (stock corner poles)
    rng = k.rng

    def z_at(x, y):
        u, v = abs(x) / X, abs(y) / Y
        return 5.8 - .9 * (1 - u) - .6 * (1 - v) + .08 * math.sin(x * 2.3 + y * .9) + .06 * math.cos(y * 1.7 - x)

    # poles with spreader caps and guy lines to stakes inside the stock outline
    for sx in (-1, 1):
        for sy in (-1, 1):
            x, y = sx * px, sy * py
            zt = z_at(x, y) - .05
            k.cyl((x, y, zt / 2), .08, zt, 'wood.crate', 'mass', sides=8)
            k.cyl((x, y, zt + .03), .32, .07, 'paint.steel', 'frame', sides=10)
            k.box((x, y, .06), (.3, .3, .12), 'paint.steel', 'frame')
            for gx, gy in ((sx * (X - .03), y), (x, sy * (Y - .03))):
                ip.tube(k, [(x, y, zt - .15), (gx, gy, .04)], .012, 'canvas.sand', 'frame', sides=4, caps=False)
                k.box((gx, gy, .1), (.05, .05, .2), 'wood.crate', 'frame')
    # base net: a rolling double-sided mesh (dark olive), edge hem rope
    nx, ny = 10, 18
    grid = [[(-X + 2 * X * i / nx, -Y + 2 * Y * j / ny) for i in range(nx + 1)] for j in range(ny + 1)]
    V, F = [], []
    for j in range(ny + 1):
        for i in range(nx + 1):
            x, y = grid[j][i]
            V.append((x, y, z_at(x, y)))
    for j in range(ny):
        for i in range(nx):
            a = j * (nx + 1) + i
            F.append((a, a + 1, a + nx + 2, a + nx + 1))
    off = len(V)
    V += [(x, y, z - .05) for x, y, z in V]
    F += [(d + off, c + off, b + off, a + off) for a, b, c, d in F]
    k.add(V, F, 'canvas.canvas2', 'skin')
    hem = [(x, y, z_at(x, y) + .02) for x, y in
           [(-X + 2 * X * i / 20, -Y) for i in range(21)] + [(X, -Y + 2 * Y * j / 30) for j in range(1, 31)] +
           [(X - 2 * X * i / 20, Y) for i in range(1, 21)] + [(-X, Y - 2 * Y * j / 30) for j in range(1, 30)]]
    ip.tube(k, hem, .025, 'canvas.sand', 'frame', sides=5, closed=True)
    # garnish: irregular scrim tufts in three colours, tilted, overlapping; torn strips hanging off the edges
    mats = ['canvas.canvas', 'canvas.canvas2', 'wood.crate', 'canvas.canvas', 'canvas.canvas2']   # olive, dark green, brown
    for _ in range(950):                  # dense overlapping scrim leaves: irregular two-sided quads, tilted off the net
        x, y = rng.uniform(-X + .15, X - .15), rng.uniform(-Y + .15, Y - .15)
        w, d = rng.uniform(.45, 1.0), rng.uniform(.3, .7)
        with k.at((x, y, z_at(x, y) + rng.uniform(.0, .05)), yaw=rng.uniform(0, math.pi), pitch=rng.uniform(-.5, .5),
                  roll=rng.uniform(-.5, .5)):
            q = [(-w / 2 * rng.uniform(.6, 1), -d / 2 * rng.uniform(.6, 1), 0), (w / 2 * rng.uniform(.6, 1), -d / 2 * rng.uniform(.4, 1), .02),
                 (w / 2 * rng.uniform(.6, 1), d / 2 * rng.uniform(.6, 1), 0), (-w / 2 * rng.uniform(.4, 1), d / 2 * rng.uniform(.6, 1), .03)]
            k.add(q, [(0, 1, 2, 3), (3, 2, 1, 0)], mats[rng.randrange(len(mats))], 'skin')
    for _ in range(46):
        side = rng.randrange(4)
        if side < 2:
            x, y = rng.uniform(-X + .2, X - .2), (-Y + .05) if side == 0 else (Y - .05)
        else:
            x, y = (-X + .05) if side == 2 else (X - .05), rng.uniform(-Y + .2, Y - .2)
        L = rng.uniform(.5, 1.3)
        z = z_at(x, y)
        with k.at((x, y, z), yaw=0.0 if side < 2 else math.pi / 2):
            k.slab(-.12, .12, -L, 0.0, -.015, .015, mats[rng.randrange(len(mats))], 'skin')
    M.fit_plan(k, -X, X, -Y, Y)          # the stock net extents exactly
    count(k, 'panels_sections')


def lockers(k):
    """Field equipment lockers. Features: (1) four tall steel lockers on a skid base; (2) a door per locker with a
    louvre grille, handle and stencil number; (3) vents on the backs; (4) a sloped rain cap; (5) a kick plate."""
    k.box((0, 0, .08), (4.7, 1.0, .16), 'paint.steel', 'frame', ch=.02)
    for s in (-1, 1):
        k.box((0, s * .42, .06), (4.8, .12, .12), 'paint.steel', 'frame')
    for i in range(4):
        x = -1.74 + 1.16 * i
        k.box((x, 0, 1.55), (1.12, .9, 2.8), 'paint.main', 'mass', ch=.03, top=.02)
        with k.at((x, -.45, 0)):
            k.slab(-.5, .5, .25, 2.85, -.03, .0, 'paint.door', 'skin', front=.01, door=True, feature='door')
            for j in range(6):
                z = .5 + j * .09
                k.slab(-.35, .35, z, z + .045, -.05, -.03, 'grate', 'detail')
            k.slab(.36, .42, 1.3, 1.6, -.07, -.03, 'paint.trim', 'frame')
            M.stencil(k, str(i + 1), 0, 2.3, .32, -.035)
        with k.at((x, .45, 0), yaw=math.pi):
            for j in range(5):
                z = 2.2 + j * .1
                k.slab(-.35, .35, z, z + .05, -.04, .0, 'grate', 'detail')
    k.prism(ccw([(-.55, 2.95), (.55, 2.95), (.55, 3.0), (-.55, 3.12)]), -2.4, 2.4, 'paint.plate', 'roof', axis='x')
    k.box((0, -.47, .2), (4.6, .04, .1), 'hazard', 'frame')


def l_banner(k):
    """Liao standard. Features: (1) two slim steel pylons on cast footings with a top crossbar and finials; (2) a long
    Liao-green cloth banner hanging from the bar with a gold border and a weighted bottom bar; (3) the Liao emblem
    plate on the banner; (4) a lit plinth plaque."""
    for x in (-3.3, 3.3):
        k.box((x, 0, .25), (.9, .9, .5), 'ferro', 'mass', ch=.08, top=.06)
        k.box((x, 0, 8.6), (.18, .18, 16.2), 'paint.steel', 'frame', ch=.03)
        k.cyl((x, 0, 16.85), .16, .3, 'paint.second', 'frame', sides=12)
        k.cyl((x, 0, 17.1), .08, .3, 'paint.second', 'frame', sides=8, top=.06)
    k.box((0, 0, 16.4), (6.9, .16, .2), 'paint.steel', 'frame', ch=.03)
    k.box((0, 0, 9.7), (5.8, .05, 13.0), 'paint.main', 'skin')
    for x in (-2.85, 2.85):
        k.box((x, 0, 9.7), (.12, .08, 13.0), 'paint.second', 'frame')
    k.box((0, 0, 16.1), (5.8, .08, .14), 'paint.second', 'frame')
    k.box((0, 0, 3.15), (6.0, .14, .18), 'paint.steel', 'frame')
    for s in (-1, 1):
        M.emblem(k, (0, s * .06, 12.2), 0.0 if s < 0 else math.pi, 1.6, ring='paint.second', disc='paint.main', rays=6)
    k.box((0, 0, .3), (2.0, .5, .6), 'ferro', 'mass', ch=.05)
    with k.at((0, -.25, 0)):
        k.slab(-.6, .6, .15, .45, -.03, .0, 'paint.second', 'frame')
    count(k, 'emblems')


def l_flag(k):
    """Liao flagpole. Features: (1) a 12 m tapered steel pole on a stepped cast plinth with a plaque; (2) a ball
    finial and halyard; (3) a Liao-green flag with a gold border and the emblem."""
    k.box((0, 0, .2), (1.4, 1.4, .4), 'ferro', 'mass', ch=.1, top=.06)
    k.box((0, 0, .5), (.9, .9, .2), 'ferro', 'mass', ch=.06, top=.04)
    M.loft(k, [[(x * .1, y * .1, .6) for x, y in ((-1, -1), (1, -1), (1, 1), (-1, 1))],
               [(x * .05, y * .05, 12.0) for x, y in ((-1, -1), (1, -1), (1, 1), (-1, 1))]], 'paint.steel', 'frame')
    k.cyl((0, 0, 12.12), .13, .25, 'paint.second', 'frame', sides=12, top=.06)
    ip.tube(k, [(.07, 0, 1.2), (.07, 0, 11.9)], .008, 'metal', 'frame', sides=4, caps=False)
    k.box((1.2, 0, 11.3), (2.3, .04, 1.4), 'paint.main', 'skin')
    k.box((1.2, 0, 11.98), (2.3, .06, .08), 'paint.second', 'frame')
    k.box((1.2, 0, 10.62), (2.3, .06, .08), 'paint.second', 'frame')
    k.box((2.33, 0, 11.3), (.06, .06, 1.4), 'paint.second', 'frame')
    for s in (-1, 1):
        M.emblem(k, (1.2, s * .03, 11.3), 0.0 if s < 0 else math.pi, .5, ring='paint.second', disc='paint.main', rays=6)
    with k.at((0, -.7, 0)):
        k.slab(-.35, .35, .1, .32, -.03, .0, 'paint.second', 'frame')
    count(k, 'emblems')


# ------------------------------------------------------------------ prison
def secure_fence(k, a, b, h=6.4, post=4.0):
    """Detention fence run: cast kerb with a red hazard band, steel posts with a Y-arm crown, chain-link panels
    and a razor coil along the crown."""
    L = math.hypot(b[0] - a[0], b[1] - a[1])
    yaw = math.atan2(b[1] - a[1], b[0] - a[0])
    with k.at((a[0], a[1], 0), yaw=yaw):
        k.box((L / 2, 0, .3), (L + .5, .55, .6), 'ferro', 'mass', ch=.06, top=.05)
        k.box((L / 2, 0, .45), (L + .52, .57, .14), 'paint.valve', 'skin')
        n = max(1, round(L / post))
        for i in range(n + 1):
            x = L * i / n
            k.box((x, 0, .6 + (h - .6) / 2), (.18, .18, h - .6), 'paint.steel', 'mass', ch=.02)
            for s_ in (-1, 1):
                k.beam((x, 0, h - .1), (x, s_ * .6, h + .6), .06, 'paint.steel', 'frame')
        k.box((L / 2, 0, (.6 + h) / 2), (L, .03, h - .7), 'grate', 'skin')
        k.box((L / 2, 0, h - .05), (L, .1, .1), 'paint.steel', 'frame')
        turns = max(4, int(L / .6))
        pts = [(L * i / (turns * 5), .32 * math.cos(i * math.tau / 5), h + .45 + .32 * math.sin(i * math.tau / 5))
               for i in range(turns * 5 + 1)]
        ip.tube(k, pts, .03, 'metal', 'skin', sides=5, caps=False)


def prison(k):
    """Detention centre. Features: (1) a two-storey battered cast cell block with rows of barred slit windows and a
    dark hipped roof; (2) four square corner bastions, the north-east one carrying a glazed guard cab with a
    searchlight; (3) a high chain-link fence with a razor-wire crown on a red-banded cast kerb, enclosing the exercise
    yard; (4) a sally-port gate between cast piers with a guard post in the east fence; (5) the block's double blast
    door opening onto the yard."""
    W, B = 'ferro', 'paint.second'
    bx = (-19.0, -5.6, -1.0, 23.6)
    ins = M.batter_mass(k, bx, 0.0, 10.4, .6, plinth=.6, cap=.25, ch=.3, mat=W, plinth_mat='paint.plate', proud=.15)
    wall_band(k, (bx[0] + .3, bx[1] + .3, bx[2] - .3, bx[3] - .3), 5.0, 5.4, B)
    pagoda_roof(k, (bx[0] + 1.0, bx[1] + 1.0, bx[2] - 1.0, bx[3] - 1.0), 10.4, 15.0, ov=.2, lift=0.0, mat='paint.roof')
    for (x, y) in ((bx[0], bx[1]), (bx[2], bx[1]), (bx[0], bx[3]), (bx[2], bx[3])):
        tb = (x - 2.3, y - 2.3, x + 2.3, y + 2.3)
        M.batter_mass(k, tb, 0.0, 11.8, .45, plinth=.6, cap=.25, ch=.2, mat=W, plinth_mat='paint.plate', proud=.12)
        wall_band(k, (tb[0] + .25, tb[1] + .25, tb[2] - .25, tb[3] - .25), 10.6, 11.0, B)
    for s in 'nesw':
        L = (bx[2] - bx[0]) if s in 'ns' else (bx[3] - bx[1])
        xs = [-3.0, 3.0] if s in 'ns' else [-L / 2 + 4.0 + (L - 8.0) * i / 6 for i in range(7)]
        for z0 in (2.4, 7.2):
            for xl in xs:
                if s == 'e' and abs(xl) < 2.5 and z0 < 5:
                    continue
                with M.on_face(k, bx, s, xl, z0, ins):
                    M.slit_window(k, 0, 0, .9, .5)
    # main door onto the yard (east face, middle)
    o, yaw, _ = M.frame_of(bx, 'e')
    with k.at((o[0] + .2, o[1], 0), yaw=yaw):
        M.blast_door(k, 2.4, 2.8, depth=.5, back=.6, fw=.42, yaw=yaw)
    # NE bastion: glazed guard cab with a searchlight
    cxy = (bx[2], bx[3])
    M.cab(k, cxy, 4.0, 4.0, 11.8, 2.4, flare=.35, cap=-.2, cap_t=.2, band=B, balcony=.7, cap_mat='paint.trim', roof_mat='paint.trim')
    M.searchlight(k, 'searchlight', (cxy[0], cxy[1], 14.4), r=.55, anim=False)
    # exercise yard fence (stock prisonwall2 / chainlinks), sally port in the east run
    runs = [((-1.0, 25.0), (19.6, 25.0)), ((19.6, 25.0), (19.6, 14.8)), ((19.6, 7.2), (19.6, -24.6)),
            ((19.6, -24.6), (-19.6, -24.6)), ((-19.6, -24.6), (-19.6, -8.0))]
    for a, b in runs:
        secure_fence(k, a, b)
    for y in (14.8, 7.2):
        k.box((19.6, y, 3.8), (1.4, 1.4, 7.6), W, 'mass', ch=.12, top=.08)
        k.box((19.6, y, 7.7), (1.6, 1.6, .2), 'paint.plate', 'frame', ch=.05)
        k.cyl((19.6, y, 7.95), .14, .3, 'lamp', 'frame', sides=10)
    with k.at((19.6, 11.0, 0), yaw=math.pi / 2):
        for xl in (-1.5, 1.5):
            k.slab(xl - 1.42, xl + 1.42, .1, 5.2, -.05, .05, 'paint.door', 'skin', door=True, feature='gate')
            for z in (1.2, 2.6, 4.0):
                k.slab(xl - 1.4, xl + 1.4, z - .06, z + .06, -.12, -.05, 'paint.trim', 'frame')
        k.slab(-2.9, 2.9, .0, .06, -2.0, 2.0, 'hazard', 'frame', walk=True)
    count(k, 'doors')
    gp = (15.4, 15.2, 18.2, 18.0)                 # guard post inside the sally port, door onto the yard
    M.block(k, gp, 0, 3.2, W, B, bands=((.7, 1.1),))
    M.doors(k, gp, 'w', 'blast', [0.0], 1.1, 2.2)
    o, yaw, _ = M.frame_of(gp, 's')
    with k.at((o[0], o[1], 0), yaw=yaw):
        M.slit_window(k, 0.0, 2.0, 1.6, .5)
    for (x, y) in ((19.6, 25.0), (19.6, -24.6), (-19.6, -24.6)):
        k.cyl((x, y, 7.4), .12, 1.6, 'paint.steel', 'frame', sides=8)
        k.box((x, y, 8.3), (.6, .4, .3), 'paint.plate', 'frame', ch=.04)
        k.box((x, y, 8.13), (.5, .32, .04), 'lamp', 'frame')
    with M.on_face(k, bx, 'e', -5.0, 5.0, ins):
        M.stencil_panel(k, '9', 0, 0, .8, -.02)


SPECS = {
    's_barracks': dict(fn=s_barracks, palette='intent_mil_s', cls='small', counts=dict(doors=2, windows=11),
        intent=('Steiner troop barracks. Features: (1) a long cast hall on a raised plinth under a steep standing-seam '
                'gable roof; (2) a projecting gabled entrance porch with a blast door and broad cast steps; (3) armoured '
                'windows along both long sides above a blue-grey faction band; (4) a louvred vent high in each gable and '
                'a west gable side door reached by a railed ramp; (5) ridge ventilators and a flue.')),
    's_barracks2': dict(fn=s_barracks2, palette='intent_mil_s', cls='small', counts=dict(doors=3, windows=9, stairs_ladders=1),
        intent=('Steiner raised barracks. Features: (1) a long cast dormitory block lifted on cast columns over an open '
                'undercroft; (2) a railed steel access gallery along the east side serving three blast doors, reached by '
                'a stair at the south end; (3) armoured windows between the doors and along the west side; (4) a '
                'crenellated plate parapet over a blue-grey faction band; (5) roof vents and a stencilled unit plate.')),
    'l_elitebarracks': dict(fn=l_elitebarracks, palette='intent_mil_l', cls='large', counts=dict(doors=3, windows=42),
        intent=('Liao elite barracks. Features: (1) a long battered cast podium in Liao green plate on a proud plinth; '
                '(2) two stepped dark cornice tiers over the podium carrying a terrace; (3) a narrower upper dormitory '
                'storey with a continuous row of red-framed windows; (4) a hipped pagoda roof with upturned eave corners, '
                'ridge and finials; (5) blast doors at both ends and a canopied main entrance with the Liao emblem.')),
    'generic_rpbuilding': dict(fn=generic_rpbuilding, palette='intent_mil', cls='large', counts=dict(doors=4),
        intent=('Generic vehicle repair building. Features: (1) a long low cast hall with sloped corner buttresses; '
                '(2) a drive-through vertical-lift bay door at each end under a hazard-striped portal (stock door01 / '
                'Doors03 nodes); (3) two lift-up service hatches on each long side on one node (stock Doors02) with a '
                'personnel door between them; (4) a raised louvred roof monitor; (5) a service pipe run along the base.')),
    's_lightwalldiag': dict(fn=s_lightwalldiag, palette='wall_cast', cls='prop', counts=dict(panels_sections=12), tol=dict(footprint=.2),
        intent=('The approved Steiner light wall as a diagonal run: same cast units, battens, coping and footing over '
                'the stock diagonal length, the terminal pier turned square to the map.')),
    'razorwirefencediag': dict(fn=razorwirefencediag, palette='galvanised', cls='prop', counts=dict(panels_sections=2), tol=dict(grid_mae=.35),
        intent=('The approved razor-wire fence as a diagonal run in two sections: same posts, strands, braces and '
                'concertina crown.')),
    'wirefencediag': dict(fn=wirefencediag, palette='galvanised', cls='prop', counts=dict(panels_sections=3), tol=dict(footprint=3.0, grid_mae=1.0),
        intent='The approved chain-link fence as a diagonal run in three panels: same posts, top rail and diamond mesh.'),
    'milcheckpoint': dict(fn=milcheckpoint, palette='intent_mil', cls='prop', counts=dict(), tol=dict(footprint=.35),
        intent=('Military checkpoint barrier. Features: (1) a cast guard post with a slit window, lamp and intercom; '
                '(2) a two-section folding hazard boom (stock gate_b on the post, gate_a folding on it); (3) a rest post '
                'with a fork; (4) a hazard pad under the post.')),
    'sawhorsebarricade': dict(fn=sawhorsebarricade, palette='intent_mil', cls='prop', counts=dict(), tol=dict(grid_mae=.3),
        intent=('Sawhorse barricade: hazard-striped top board, splayed steel A-frame legs, a lower cross board, an amber '
                'warning lamp and sandbags weighting the feet.')),
    'htent': dict(fn=htent, palette='intent_mil', cls='prop', counts=dict(doors=2, emblems=2),
        intent=('Field hospital tent: the approved army frame tent with white roundels and red crosses on both roof '
                'slopes, framed gable doors, straps, guy lines and a timber deck.')),
    'hqtent': dict(fn=hqtent, palette='intent_mil', cls='small', counts=dict(doors=3, windows=6, antennas_dishes=1),
        intent=('Field HQ tent: a long army frame tent with a perpendicular briefing wing (the stock T plan), framed '
                'doors at the outer ends, a whip antenna on a guyed mast and a field generator with a cable run.')),
    'camonetting': dict(fn=camonetting, palette='intent_mil', cls='prop', counts=dict(panels_sections=1), tol=dict(grid_mae=.4),
        intent=('Camouflage net: a draped net on four corner poles sagging to the centre and dipping between the poles (the '
                'stock V profile) on the exact stock footprint; a lumpy garnished surface of irregular scrim tufts in three '
                'camouflage colours with torn strips hanging off the edges; spreader caps; guy lines to stakes.')),
    'lockers': dict(fn=lockers, palette='intent_mil', cls='prop', counts=dict(), tol=dict(grid_mae=.3),
        intent=('Field equipment lockers: four tall steel lockers on a skid, a door each with a louvre grille, handle '
                'and stencil number, back vents, a sloped rain cap and a hazard kick strip.')),
    'l_banner': dict(fn=l_banner, palette='intent_mil_l', cls='prop', counts=dict(emblems=1), tol=dict(footprint=12.0, grid_mae=.5),
        intent=('Liao standard: two slim steel pylons on cast footings with a crossbar and finials, a long Liao-green '
                'banner with a gold border and weighted bar, the Liao emblem on both faces, a plinth plaque.')),
    'l_flag': dict(fn=l_flag, palette='intent_mil_l', cls='prop', counts=dict(emblems=1), tol=dict(footprint=9.0, grid_mae=.5, centroid=.3),
        intent=('Liao flagpole: a 12 m tapered steel pole on a stepped cast plinth with a plaque, ball finial and '
                'halyard, and a Liao-green flag with a gold border and the emblem.')),
    'prison': dict(fn=prison, palette='intent_mil', cls='large', counts=dict(doors=3),
        intent=('Detention centre. Features: (1) a two-storey battered cast cell block with rows of barred slit windows '
                'and a dark hipped roof; (2) four square corner bastions, the north-east one carrying a glazed guard cab '
                'with a searchlight; (3) a high chain-link fence with a razor-wire crown on a red-banded cast kerb round '
                'the exercise yard; (4) a sally-port gate between cast piers with a guard post; (5) the block double '
                'blast door opening onto the yard.')),

}
