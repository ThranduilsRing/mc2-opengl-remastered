"""Intent pass, industrial round 2: factories/plants and industrial services.

Same family as intent.py / intent_set2.py: corrugated steel on visible frames, brick halls, pale
cream tanks, safety-yellow rails, red valves, galvanised ladders; access verified by clearance.py.
Imported at the end of intent.py (uses its wall/shed helpers).
"""
import math

import geo
import ind_parts as ip
import intent as I

count = I.count


# ------------------------------------------------------------------ shared: steel-framed corrugated shed
def _local_top(side, F, top):
    """Convert a world roof-line function top(x, y) into the side's local wall coordinate."""
    origin, yaw, L, _ = F[side]
    ox, oy = origin[0], origin[1]
    c, s = math.cos(yaw), math.sin(yaw)
    return lambda xl: top(ox + c * xl, oy + s * xl)


def shed(k, x0, y0, x1, y1, H, sides='nesw', openings=None, top=None, zb=0.0, bay=3.2, mat='corr.main'):
    """Corrugated shed walls in construction order (frame, girts, sheets, trims) with openings:
    side -> [(kind, x_local, z0, w, h)], kind rollup | door | door2 | window | louvre."""
    F = I.faces_of(x0, y0, x1, y1, 0)
    openings = openings or {}
    for s in sides:
        origin, yaw, L, _ = F[s]
        ztop = _local_top(s, F, top) if top else (lambda xl, H=H: H)
        ops = openings.get(s, [])
        holes = []
        for kind, xl, z0, w, h in ops:
            pad = .36 if kind == 'rollup' else .15
            holes.append((xl - w / 2 - pad, z0, xl + w / 2 + pad, z0 + h + .12))
        n = max(1, round(L / bay))
        cols = [-L / 2 + L * i / n for i in range(n + 1)]
        with k.at(origin, yaw=yaw):
            I.corr_wall(k, L, zb, ztop, holes, cols=cols, mat=mat)
            for kind, xl, z0, w, h in ops:
                if kind == 'rollup':
                    with k.at((0, 0, z0)):
                        ip.rollup_door(k, xl, w, h, 0.0, grade=z0)
                    count(k, 'doors')
                    I.wall_lamp(k, xl, z0 + h + 1.25, yaw)
                elif kind in ('door', 'door2'):
                    with k.at((0, 0, z0)):
                        ip.personnel_door(k, xl, w, h, 0.0, leaves=2 if kind == 'door2' else 1, apron=z0 < .5)
                    count(k, 'doors')
                    I.wall_lamp(k, xl, z0 + h + .9, yaw)
                elif kind == 'window':
                    ip.steel_window(k, xl - w / 2, xl + w / 2, z0, z0 + h, 0.0, rows=1)
                    count(k, 'windows')
                elif kind == 'louvre':
                    k.slab(xl - w / 2, xl + w / 2, z0, z0 + h, .03, .1, 'grate', 'frame')
                    for j in range(int(h / .18)):
                        k.slab(xl - w / 2, xl + w / 2, z0 + j * .18 + .04, z0 + j * .18 + .12, -.05, .08, 'paint.second', 'frame')
                    k.slab(xl - w / 2 - .1, xl + w / 2 + .1, z0 - .1, z0 + h + .1, -.08, .03, 'paint.trim', 'frame')


def flat_roof(k, x0, y0, x1, y1, H, mat='paint.roof'):
    k.box(((x0 + x1) / 2, (y0 + y1) / 2, H - .12), (x1 - x0 - .1, y1 - y0 - .1, .24), mat, 'roof', walk=True)
    for a, b in (((x0, y0), (x1, y0)), ((x1, y0), (x1, y1)), ((x1, y1), (x0, y1)), ((x0, y1), (x0, y0))):
        k.beam((a[0], a[1], H + .15), (b[0], b[1], H + .15), .22, 'paint.trim', 'frame', h=.3)


def roof_pipes(k, pts, r=.16, mat='paint.pipe', z=None, step=2.6):
    ip.pipe(k, pts, r, mat, bend=.5)
    for a, b in zip(pts, pts[1:]):
        L = ip.length(ip.sub(b, a))
        for i in range(1, int(L / step) + 1):
            q = ip.add(a, ip.mul(ip.sub(b, a), i / (int(L / step) + 1)))
            zz = z if z is not None else q[2] - r - .25
            k.box((q[0], q[1], (zz + q[2] - r) / 2), (.22, .3, q[2] - r - zz), 'paint.steel', 'frame')


# ------------------------------------------------------------------ factorysm
def factorysm(k):
    H = 5.3
    k.box((0, 0, .15), (36.6, 27.2, .3), 'ferro', 'mass', ch=.1, walk=True)
    zb = .3
    shed(k, -18.2, -13.5, 18.2, 1.6, H, sides='swe', zb=zb, openings={
        's': [('door2', -13.5, zb, 2.0, 2.4), ('window', -6.0, 2.4, 6.0, 1.1), ('window', 2.0, 2.4, 4.0, 1.1)],
        'w': [('window', 0.0, 2.4, 6.0, 1.1)],
        'e': [('louvre', -4.5, 1.6, 1.6, 1.6), ('door2', 0.5, zb, 2.0, 2.4), ('window', 4.8, 2.4, 3.2, 1.1)]})
    shed(k, -12.1, 1.6, 3.0, 13.5, H, sides='wne', zb=zb, openings={
        'n': [('door2', 2.5, zb, 2.0, 2.4), ('window', -3.0, 2.4, 4.0, 1.1)], 'w': [('window', 0.0, 2.4, 4.0, 1.1)]})
    shed(k, 3.0, 1.6, 18.2, 6.7, 4.6, sides='ne', zb=zb, openings={'n': [('rollup', 0.0, zb, 4.0, 3.6)]})
    flat_roof(k, -18.2, -13.5, 18.2, 1.6, H)
    flat_roof(k, -12.1, 1.6, 3.0, 13.5, H)
    flat_roof(k, 3.0, 1.6, 18.2, 6.7, 4.6)
    # raised clerestory monitor over the front-right bay: glazed band, hipped standing-seam roof
    mx0, mx1, my0, my1 = 6.4, 16.8, -12.4, -3.4
    shed(k, mx0, my0, mx1, my1, 8.2, sides='nesw', zb=H, bay=2.6, openings={
        's': [('window', 0.0, H + .6, 8.0, 1.6)], 'n': [('window', 0.0, H + .6, 8.0, 1.6)]})
    k.prism(geo.rect_poly(mx0 - .3, my0 - .3, mx1 + .3, my1 + .3), 8.2, 9.2, 'paint.roof', 'roof', top=2.2, top_h=1.0)
    # roof plant: turret fans, pipe runs (the stock roof lines), hatch, vents
    ip.roof_fan(k, -8.0, -6.0, H, .9)
    ip.roof_fan(k, -2.0, -6.0, H, .9)
    roof_pipes(k, [(-15.0, -1.0, H + .6), (2.0, -1.0, H + .6), (2.0, -9.0, H + .6)], z=H)
    roof_pipes(k, [(-5.0, 11.0, H + .5), (-5.0, 3.0, H + .5), (-14.0, 3.0, H + .5)], r=.12, mat='paint.pipe2', z=H)
    ip.hatch(k, -9.0, 8.0, H, .9)
    for x in (-15.0, -11.0, -4.0):
        ip.mushroom_vent(k, x, -10.0, H, .28)
    # external stair from the yard to the rear-wing roof via a landing on the east side
    ip.rect_platform(k, 3.1, 6.85, 4.8, 8.1, H, rail_sides='es', legs=True, gaps=[], name='roof landing')
    ip.stair(k, (3.95, 13.35, .3), (3.95, 8.1, H), name='roof stair')
    count(k, 'stairs_ladders')


# ------------------------------------------------------------------ factorylg
def factorylg(k):
    k.box((0, 0, .15), (37.2, 20.5, .3), 'ferro', 'mass', ch=.1, walk=True)
    zb, Ha = .3, 5.3
    x0, x1, y0, y1 = -18.45, 18.45, -10.1, 10.1
    hx0, hy0 = -12.3, -6.7
    yr, zr = 5.6, 16.0                      # ridge set toward the rear (stock asymmetric gable)
    top = lambda x, y: (11.3 + (zr - 11.3) * (y - hy0) / (yr - hy0)) if y <= yr else (zr - (zr - 13.5) * (y - yr) / (y1 - yr))  # noqa: E731
    # annex: west strip and front strip, lean-to roofs
    shed(k, x0, y0, hx0, y1, Ha, sides='wns', zb=zb, openings={
        'w': [('door', 3.0, zb, 1.2, 2.3), ('window', -3.0, 2.4, 4.0, 1.1)], 's': [('window', 0.0, 2.4, 3.0, 1.1)]})
    shed(k, hx0, y0, x1, hy0, Ha, sides='se', zb=zb, openings={
        's': [('window', -10.0, 2.4, 5.0, 1.1), ('louvre', -2.0, 1.4, 1.8, 1.8), ('window', 6.0, 2.4, 6.0, 1.1), ('door2', 12.5, zb, 2.0, 2.4)]})
    flat_roof(k, x0, y0, hx0, y1, Ha)
    flat_roof(k, hx0, y0, x1, hy0, Ha)
    # main hall: asymmetric gable, two roll-up doors in the rear wall, clerestory windows east
    shed(k, hx0, hy0, x1, y1, 16.0, sides='ne', zb=zb, top=top, openings={
        'n': [('rollup', -7.0, zb, 5.0, 5.6), ('rollup', 7.0, zb, 5.0, 5.6)],
        'e': [('window', -2.0, 7.5, 7.0, 1.4)]})
    shed(k, hx0, hy0, x1, y1, 16.0, sides='sw', zb=Ha - .3, top=top)
    for yy, z0, yb_, z1 in ((hy0 - .2, 11.2, yr, zr + .1), (yr, zr + .1, y1 + .2, 13.4)):
        k.prism([(yy, z0), (yb_, z1), (yb_, z1 + .25), (yy, z0 + .25)], hx0 - .25, x1 + .25, 'paint.roof', 'roof', axis='x')
    k.box(((hx0 + x1) / 2, yr, zr + .3), (x1 - hx0 + .5, .4, .2), 'paint.trim', 'frame')
    # rooftop ventilator housing straddling the ridge, louvred both sides
    ip.louvre_box(k, -4.0, yr - .4, zr - .6, 4.0, 3.0, 2.2)
    for x in (-9.0, 3.0, 10.0):
        ip.mushroom_vent(k, x, yr - 1.5, top(x, yr - 1.5) - .1, .32)
    ip.roof_fan(k, -15.4, 0.0, Ha, .8)
    roof_pipes(k, [(-17.0, -8.5, Ha + .5), (10.0, -8.5, Ha + .5)], z=Ha)


# ------------------------------------------------------------------ industrialcomplex
def industrialcomplex(k):
    k.box((0, 0, .15), (30.5, 31.5, .3), 'ferro', 'mass', ch=.1, walk=True)
    zb = .3
    y0, y1 = -15.6, 15.6
    for s in (-1, 1):
        xo, xi = s * 15.1, s * 2.6
        bat = 1.6
        prof = [(xi, zb), (xo, zb), (xo - bat, 14.0), (xi, 16.2)] if s > 0 else [(xo, zb), (xi, zb), (xi, 16.2), (xo + bat, 14.0)]
        k.prism(prof, y0, y1, 'paint.panel', 'mass', axis='y')
        # vertical panel joints and a rust-brick service shaft on the outer battered face
        for i in range(9):
            yy = y0 + 1.6 + i * (y1 - y0 - 3.2) / 8
            k.add(*ip.box_along((xo + s * .02, yy, zb), (xo - s * (bat - .02), yy, 14.0), (0, 1, 0), (s, 0, 0), .12, .06), 'paint.second', 'frame')
        with k.at((xo, 4.5, 0), yaw=s * math.pi / 2, pitch=-math.atan(bat / 14)):
            k.box((0, -.4, 7.0), (2.4, .8, 14.0), 'brick.main', 'mass', ch=.08)
            for j in range(4):
                k.box((0, -.84, 3.0 + j * 3.0), (1.6, .1, 1.2), 'grate', 'frame')
        # front/back end walls of the wing (flat), with doors and louvres
        ip.roof_fan(k, s * 9.0, -6.0, 15.0 + .2, .9)
        ip.mushroom_vent(k, s * 9.0, 6.0, 15.2, .35)
    # recessed central steel bay: front half lower, rear half raised plant housing
    shed(k, -2.6, -14.0, 2.6, 15.6, 15.1, sides='sn', zb=zb, bay=2.6, openings={
        's': [('rollup', 0.0, zb, 4.2, 6.0)], 'n': [('door2', 0.0, zb, 2.0, 2.6)]})
    k.slab(-2.2, 2.2, 7.0, 7.25, -14.12, -14.02, 'lamp', 'frame')   # green-lit transom strip (stock), as a lamp bar
    flat_roof(k, -2.6, -14.0, 2.6, 2.6, 15.1)
    k.box((0, 9.1, 16.05), (5.2, 13.0, 1.9), 'paint.second', 'mass', ch=.1, top=.08)
    ip.roof_fan(k, 0, -9.0, 15.1, 1.2)
    ip.roof_fan(k, 0, -3.0, 15.1, 1.2)
    ip.louvre_box(k, 0, 9.0, 17.0, 3.6, 4.0, 1.2)
    count(k, 'wings', 2)


# ------------------------------------------------------------------ processingplant
def processingplant(k):
    k.box((0, 0, .15), (15.1, 26.0, .3), 'ferro', 'mass', ch=.1, walk=True)
    zb = .3
    # main processing block and the taller front block, orange safety band at waist height
    shed(k, -7.4, -4.3, 7.4, 12.9, 8.4, sides='wne', zb=zb, openings={
        'w': [('window', -3.0, 2.4, 4.0, 1.1)], 'e': [('window', 3.0, 2.4, 4.0, 1.1), ('louvre', -4.0, 1.8, 1.6, 1.6)],
        'n': [('door2', 3.0, zb, 2.0, 2.4)]})
    shed(k, -7.4, -4.3, -2.5, -4.0, 8.4, sides='s', zb=zb, bay=2.5)
    shed(k, -2.5, -10.8, 5.0, -4.3, 10.9, sides='swe', zb=zb, openings={
        's': [('door2', -1.4, zb, 2.0, 2.5), ('window', 2.4, 2.4, 2.4, 1.1)], 'w': [('window', 0.0, 4.0, 3.0, 1.1)],
        'e': [('louvre', 0.0, 2.0, 1.8, 1.8)]})
    shed(k, 5.0, -4.3, 7.4, -4.0, 8.4, sides='s', zb=zb, bay=2.4)
    # waist band as four wall strips, broken at the doorways (a solid band ran across both double doors)
    for (x0, y0, x1, y1), gaps in (((-7.4, -4.3, 7.4, 12.9), {'n': (-4.15, -1.85)}),
                                   ((-2.5, -10.8, 5.0, -4.3), {'s': (-1.3, 1.0)})):
        for side, (ya, yb) in (('s', (y0 - .05, y0 + .1)), ('n', (y1 - .1, y1 + .05))):
            g = gaps.get(side)
            for a, b in (((x0 - .05, g[0]), (g[1], x1 + .05)) if g else ((x0 - .05, x1 + .05),)):
                k.slab(a, b, 1.0, 1.25, ya, yb, 'paint.accent', 'frame')
        for xa, xb in ((x0 - .05, x0 + .1), (x1 - .1, x1 + .05)):
            k.slab(xa, xb, 1.0, 1.25, y0 + .1, y1 - .1, 'paint.accent', 'frame')
    flat_roof(k, -7.4, -4.3, 7.4, 12.9, 8.4)
    flat_roof(k, -2.5, -10.8, 5.0, -4.3, 10.9)
    k.box((1.25, -11.6, 9.2), (7.9, 1.6, .25), 'paint.second', 'frame')           # entry canopy (stock porch)
    for x in (-1.5, 4.0):
        k.beam((x, -12.2, 9.1), (x, -10.85, 7.5), .1, ip.STEEL, 'frame', h=.1)
    ip.louvre_box(k, 1.2, -7.5, 10.9, 3.0, 2.2, 1.4)
    # two tapered steel process flues on a stack plinth at the rear-west, with a sampling platform
    for cy in (3.4, 9.2):
        c = (-3.7, cy)
        ip.lathe(k, [(2.9, 8.4), (2.9, 9.4), (2.6, 9.6), (1.75, 17.4), (1.85, 17.5), (1.85, 17.8), (1.6, 17.8), (1.6, 17.0)],
                 'paint.panel', 'mass', n=40, c=c)
        ip.ring_band(k, c, 2.48, 10.6, 10.8, 'paint.second', proud=.05, n=40)
        ip.ring_band(k, c, 2.06, 14.2, 14.4, 'paint.accent', proud=.04, n=40)
    count(k, 'stacks', 2)
    pz = 13.2
    ip.ring_platform(k, (-3.7, 3.4), 2.3, 3.4, pz, math.radians(-30), math.radians(110), brackets=4,
                     gaps=[ip.polar((-3.7, 3.4), 3.35, math.radians(-10), 0)[:2]], name='flue platform')
    lp = ip.polar((-3.7, 3.4), 3.4, math.radians(-10), 0)
    ip.ladder(k, lp[0], lp[1], math.radians(-10), 8.4, pz, off=.18, reach=1.0, standoff=False, name='flue ladder')
    count(k, 'stairs_ladders')
    roof_pipes(k, [(-1.0, 5.0, 9.0), (5.5, 5.0, 9.0), (5.5, -3.0, 9.0)], z=8.4)


# ------------------------------------------------------------------ purificationplant
def purificationplant(k):
    k.box((0, 0, .15), (19.0, 20.3, .3), 'ferro', 'mass', ch=.1, walk=True)
    zb = .3
    # brick pump house with a steel flue, a lower plant hall
    I.brick_block(k, -9.4, -1.0, -4.7, 10.0, 0, 8.5, pilaster=3.6, openings={
        'w': [('window', 2.0, 3.6, 1.4, 2.2), ('window', -2.5, 3.6, 1.4, 2.2)], 'n': [('door', 0.0, 0, 1.2, 2.3)]})
    I.roof_deck(k, -9.0, -.6, -5.1, 9.6, 7.7)
    k.box((-6.3, 7.5, 10.75), (1.5, 1.5, 6.3), 'paint.steel', 'mass', ch=.15)      # flue stands on the roof deck
    k.box((-6.3, 7.5, 13.75), (1.7, 1.7, .3), 'paint.trim', 'frame', ch=.05)
    I.brick_block(k, -4.7, -10.0, 0.9, 10.0, 0, 6.5, sides='swe', pilaster=3.8, openings={
        's': [('rollup', 0.0, 0, 3.4, 3.8)], 'e': [('window', 5.0, 2.5, 3.0, 2.0), ('window', -5.0, 2.5, 3.0, 2.0)],
        'w': [('door2', 6.0, 0, 2.0, 2.4)]})
    I.roof_deck(k, -4.3, -9.6, .5, 9.6, 5.7)
    ip.roof_fan(k, -2.0, -4.0, 5.7, .8)
    # two round clarifier tanks with red bands, railed rims, fed by sloping pipe bridges
    for cy in (5.0, -5.0):
        c = (5.85, cy)
        ip.lathe(k, [(4.0, 0), (4.0, .3), (0, .3)], 'ferro', 'mass', n=64, c=c, walk=True)
        ip.vessel(k, c, 3.7, .3, 2.8, top='flat', bottom='flat', courses=1, mat='paint.main', n=64, bands=[(1.5, 2.0, 'paint.valve')])
        ip.lathe(k, [(3.3, 2.8), (3.3, 3.1), (1.4, 3.6), (0, 3.65)], 'paint.roof', 'mass', n=64, c=c, cap=False)
        a = math.radians(-60 if cy < 0 else 60)
        ip.ring_platform(k, c, 3.72, 4.5, 2.85, brackets=10, gaps=[ip.polar(c, 4.45, a, 0)[:2]], name='clarifier rim walk')
        count(k, 'tanks')
        ip.pipe(k, [(.9, cy * .7, 5.0), (3.6, cy * .9, 5.0), (c[0], c[1], 4.0), (c[0], c[1], 3.5)], .22, 'paint.pipe', bend=.8)
        # one caged ladder up to each rim walk, front side
        a = math.radians(-60 if cy < 0 else 60)
        lp = ip.polar(c, 4.5, a, 0)
        ip.ladder(k, lp[0], lp[1], a, .3, 2.85, off=.18, reach=.3, standoff=False, cage=False, name='clarifier ladder')
        count(k, 'stairs_ladders')


# ------------------------------------------------------------------ chemicalplant
def chemicalplant(k):
    k.box((0, 0, .15), (20.4, 23.1, .3), 'ferro', 'mass', ch=.1, walk=True)
    # tall brick reactor house with louvred upper vents, stepped lower brick front
    I.brick_block(k, -3.4, -2.0, 10.1, 11.4, 0, 17.8, pilaster=3.4, belt=[(8.4, 8.9)], openings={
        'e': [('vent', -3.0, 13.4, 2.4, 2.4), ('vent', 3.0, 13.4, 2.4, 2.4), ('window', 0.0, 3.0, 1.6, 2.4)],
        'n': [('vent', -3.0, 13.4, 2.4, 2.4), ('vent', 3.0, 13.4, 2.4, 2.4), ('door2', 0.0, 0, 2.0, 2.6)],
        's': [('vent', 3.5, 13.4, 2.4, 2.4)]})
    I.roof_deck(k, -3.0, -1.6, 9.7, 11.0, 17.0)
    ip.roof_fan(k, 4.0, 3.0, 17.0, 1.0)
    ip.roof_fan(k, 4.0, 7.5, 17.0, 1.0)
    I.brick_block(k, -3.4, -5.7, 6.7, -1.6, 0, 7.4, sides='swe', pilaster=3.4, openings={
        's': [('rollup', -0.5, 0, 3.6, 3.8), ('window', 3.6, 3.0, 1.4, 2.0)], 'e': [('door', 0.0, 0, 1.2, 2.3)]})
    k.prism([(-5.9, 7.35), (-1.6, 9.8), (-1.6, 10.05), (-5.9, 7.6)], -3.6, 6.9, 'paint.roof', 'roof', axis='x')
    # three pale silos with green bands along the west side; catwalk over their tops into the house
    xs = -6.75
    for cy in (-7.9, 0.0, 7.9):
        c = (xs, cy)
        ip.lathe(k, [(3.4, 0), (3.4, .6), (0, .6)], 'ferro', 'mass', n=48, c=c, walk=True)
        ip.vessel(k, c, 3.2, .6, 11.6, top='cone', bottom='flat', courses=4, mat='paint.main', head_mat='paint.roof', n=56,
                  bands=[(1.6, 2.4, 'paint.pipe2')])
        count(k, 'tanks')
    cz = 13.2
    ip.rect_platform(k, xs - .7, -11.0, xs + .7, 9.5, cz, rail_sides='ew', legs=False, gaps=[], name='silo catwalk')
    for cy in (-7.9, 0.0, 7.9):
        k.box((xs, cy, 12.35), (.5, .5, .9), ip.STEEL, 'frame')
    k.box((-4.6, 4.0, cz - .04), (3.0, 1.2, .08), 'grate', 'frame', walk=True)       # bridge into the house door
    with k.at((-3.4, 4.0, cz), yaw=-math.pi / 2):
        ip.personnel_door(k, 0, 1.0, 2.1, -.02, canopy=False, apron=False)
    count(k, 'doors')
    ip.ladder(k, xs, -7.9 - 3.2, -math.pi / 2, .6, cz, off=.18, name='silo ladder')
    count(k, 'stairs_ladders')
    for cy in (-7.9, 0.0):
        ip.pipe(k, [(xs + 1.2, cy, 11.9), (xs + 1.2, cy + 3.9, 11.9), (-3.5, cy + 3.9, 11.9)], .18, 'paint.pipe2', bend=.6)


# ------------------------------------------------------------------ powergenerator / l_turretgenerator
def generator(k, half, wings):
    """Battered armoured generator hall (cooling slots glowing in steel frames) with side pods/wings."""
    k.box((0, 0, .2), (2 * (half + wings[0] if wings else half) + .6, 15.8, .4), 'ferro', 'mass', ch=.12, walk=True)
    prof = [(-7.6, .4), (7.6, .4), (7.0, 8.4), (6.2, 10.6), (5.2, 13.0), (-5.2, 13.0), (-6.2, 10.6), (-7.0, 8.4)]
    k.prism(prof, -half, half, 'paint.main', 'mass', axis='x')
    # cooling slots: framed recesses on both long sides and the roof, lit strips inside
    for s in (-1, 1):
        with k.at((0, s * 7.45, 0), yaw=0 if s < 0 else math.pi):
            k.slab(-half * .8, half * .8, 2.6, 8.0, -.2, .1, 'paint.second', 'frame', front=.06)
            k.slab(-half * .74, half * .74, 2.8, 7.8, -.24, -.18, 'paint.trim', 'frame')
            for zc in (4.2, 6.4):
                k.slab(-half * .62, half * .62, zc - .45, zc + .45, -.3, -.22, 'paint.trim', 'frame', front=.04)
                k.slab(-half * .58, half * .58, zc - .3, zc + .3, -.24, -.2, 'paint.glow', 'frame')
            for x in (-half * .76, half * .76):
                for zz in (2.9, 7.7):
                    k.box((x, -.28, zz), (.14, .1, .14), 'metal', 'detail')
    for yc in (-3.2, 0.0, 3.2):
        k.box((0, yc, 13.05), (half * 1.5, 2.2, .14), 'paint.trim', 'frame', ch=.05)
        k.box((0, yc, 13.12), (half * 1.36, 1.5, .06), 'paint.glow', 'frame')
    for x in (-half + .6, half - .6):
        k.box((x, 0, 8.2), (.5, 15.4, .4), 'paint.second', 'frame')
    count(k, 'halls')
    if wings:
        w, hw = wings
        for s in (-1, 1):
            x0 = s * half
            x1 = s * (half + w)
            k.box(((x0 + x1) / 2, 0, hw / 2 + .2), (abs(x1 - x0), 13.0, hw), 'paint.second', 'mass', ch=.3, top=.4)
            for j in range(3):
                k.box(((x0 + x1) / 2, -4.2 + j * 4.2, hw + .35), (abs(x1 - x0) * .6, 3.0, .3), 'grate', 'frame')
            for j in range(int((hw - 2.6) / .3)):
                for yy in (-6.6, 6.6):
                    k.box(((x0 + x1) / 2, yy, 2.0 + j * .3), (abs(x1 - x0) * .75, .14, .1), 'grate', 'frame')
            count(k, 'wings')
            ip.pipe(k, [(x0 + s * .3, -4.5, 9.0), (x1 - s * 1.0, -4.5, 9.0), (x1 - s * 1.0, -4.5, hw + .2)], .25, 'paint.pipe', bend=.6)
    with k.at((0, -7.6, 0)):
        ip.personnel_door(k, half * .75, 1.4, 2.4, -.1, leaves=2)
    count(k, 'doors')


def powergenerator(k):
    generator(k, 6.9, (12.8, 11.6))


def l_turretgenerator(k):
    generator(k, 6.2, (3.9, 10.6))


# ------------------------------------------------------------------ valvestation
def valvestation(k):
    k.box((0, 0, .15), (17.6, 14.3, .3), 'ferro', 'mass', ch=.1, walk=True)
    zb = .3
    shed(k, -.4, -6.0, 8.7, 6.0, 11.4, sides='nesw', zb=zb, openings={
        's': [('rollup', 0.0, zb, 4.4, 5.0)], 'n': [('rollup', 0.0, zb, 4.4, 5.0)], 'e': [('door', 4.0, zb, 1.2, 2.3)]})
    flat_roof(k, -.4, -6.0, 8.7, 6.0, 11.4)
    for x, y in ((2.5, 3.0), (6.0, -3.0)):
        ip.mushroom_vent(k, x, y, 11.4, .3)
    # brick-clad manifold wall carrying three pressure columns, overhead bridges into the manifold
    I.brick_block(k, -8.7, -7.1, -4.2, 7.1, 0, 8.0, pilaster=3.6, openings={'w': [('door', 0.0, 0, 1.2, 2.3)],
                                                                         'n': [('notch', 0.05, 7.2, 1.3, .8)]})
    I.roof_deck(k, -8.3, -6.7, -4.6, 6.7, 7.2)
    for cy in (-4.7, 0.0, 4.7):
        c = (-6.4, cy)
        ip.vessel(k, c, .95, 7.6, 9.8, top='ellip', bottom='flat', courses=1, mat='paint.main', n=32)
        k.cyl((c[0], c[1], 7.4), 1.2, .4, 'paint.second', 'frame', sides=24)
        ip.pipe(k, [(c[0] + .95, cy * .9, 9.0), (-.35, cy * .9, 9.0)], .28, 'paint.pipe', end_flanges=True)
        ip.valve(k, (-2.4, cy * .9, 9.0), (1, 0, 0), .28)
        count(k, 'valves')
    count(k, 'columns', 3)
    ip.railing(k, [(-4.25, -6.8), (-4.25, 6.8)], 8.0, gaps=[(-4.25, -4.23), (-4.25, 0.0), (-4.25, 4.23)])
    ip.railing(k, [(-8.6, 6.8), (-4.3, 6.8)], 8.0, gaps=[(-6.4, 6.8)])
    ip.ladder(k, -6.4, 7.1, math.pi / 2, 0.0, 7.2, off=.4, standoff=False, name='manifold ladder')   # clears the cornice line
    count(k, 'stairs_ladders')


# ------------------------------------------------------------------ transformerstation
def transformerstation(k):
    zd = 3.2
    k.box((0.5, 0, zd / 2), (19.6, 20.8, zd), 'ferro', 'mass', ch=.15, top=.08, walk=True)
    pts = [(-9.25, -10.15), (10.15, -10.15), (10.15, 10.15), (-9.25, 10.15)]
    ip.railing(k, pts, zd, h=1.1, closed=True, gaps=[(-9.25, -3.1)])
    # two transformer units: tank body, radiator banks with fans, HV bushings, conservator drum
    for xc in (-2.6, 6.1):
        k.box((xc, 0, zd + 3.0), (3.4, 13.0, 6.0), 'paint.main', 'mass', ch=.12, top=.1)
        for s in (-1, 1):
            for j in range(10):
                k.box((xc + s * 2.0, -5.5 + j * 1.22, zd + 3.0), (.6, .12, 5.0), 'paint.second', 'frame')
            for yy in (-3.0, 3.0):
                k.cyl((xc + s * 2.4, yy, zd + 4.6), .7, .25, 'grate', 'frame', sides=20)
        k.box((xc, 0, zd + 6.25), (3.6, 13.2, .5), 'paint.trim', 'frame', ch=.06)
        for j in range(3):
            yy = -4.0 + j * 4.0
            k.cyl((xc, yy, zd + 7.0), .22, 1.0, 'ferro', 'frame', sides=12)
            for zz in (zd + 6.7, zd + 7.0, zd + 7.3):
                k.cyl((xc, yy, zz), .34, .08, 'ferro', 'frame', sides=12)
        ip.tube(k, [(xc, -6.2, zd + 7.2), (xc, 6.2, zd + 7.2)], .55, 'paint.main', 'frame', sides=20)
        count(k, 'transformers')
    for y in (-8.0, 8.0):
        k.box((-7.0, y, zd + 1.2), (2.0, .8, 2.4), 'paint.second', 'frame', ch=.05)
        k.box((-7.0, y - .41, zd + 1.5), (1.6, .04, 1.4), 'paint.trim', 'frame')
    ip.stair(k, (-9.85, -10.3, 0.0), (-9.85, -3.85, zd), w=.9, name='switchyard stair')
    ip.rect_platform(k, -10.4, -3.85, -9.3, -2.4, zd, rail_sides='nw', legs=True, name='stair head')
    count(k, 'stairs_ladders')


# ------------------------------------------------------------------ wasteprocessor
def wasteprocessor(k):
    I.brick_block(k, -10.5, -2.9, 10.5, 8.6, 0, 5.6, pilaster=3.5, openings={
        'w': [('rollup', 0.5, 0, 3.4, 3.6)], 'n': [('door', -6.0, 0, 1.2, 2.3)],
        's': [('window', -7.0, 3.6, 3.0, .9), ('window', -1.0, 3.6, 3.0, .9), ('window', 5.0, 3.6, 3.0, .9)],
        'e': [('window', 0.0, 3.6, 3.0, .9)]})
    I.roof_deck(k, -10.1, -2.5, 10.1, 8.2, 4.8)
    for x in (-6.0, -1.0, 4.0):
        ip.louvre_box(k, x, 4.5, 4.8, 3.0, 1.6, .9, face='n')
    ip.roof_fan(k, 7.5, 5.0, 4.8, .9)
    for i, cx in enumerate((-6.6, 0.4, 7.4)):
        c = (cx, -5.9)
        ip.lathe(k, [(2.9, 0), (2.9, .3), (0, .3)], 'ferro', 'mass', n=48, c=c, walk=True)
        ip.vessel(k, c, 2.6, .3, 3.2, top='cone', bottom='flat', courses=2, mat='paint.main', head_mat='paint.roof', n=48,
                  bands=[(2.2, 2.6, 'paint.valve')])
        count(k, 'tanks')
        # covered conveyor chute from the tank crown up into the hall
        a, b = (cx, -5.9, 4.25), (cx + 1.2, -2.95, 5.2)
        k.add(*ip.box_along(a, b, (1, 0, 0), (0, 0, 1), .9, .7), 'paint.second', 'frame')
        k.beam((cx + .6, -4.4, .3), (cx + .6, -4.4, 4.4), .12, ip.STEEL, 'frame', h=.12)
        ip.tube(k, [(cx - 1.4, -6.8, 3.0), (cx - 1.4, -6.8, 5.4)], .18, 'metal', 'frame', sides=10)
        k.cyl((cx - 1.4, -6.8, 5.5), .26, .1, 'paint.trim', 'frame', sides=12)
    count(k, 'chutes', 3)


# ------------------------------------------------------------------ weaponsfacility
def weaponsfacility(k):
    X0, X1, Y0, Y1 = -20.6, 20.6, -21.8, 21.8
    k.box((0, 0, .15), (X1 - X0, Y1 - Y0, .3), 'ferro', 'mass', ch=.1, walk=True)
    # raised assembly deck with hazard-banded edges, on concrete piers
    dz = 7.6
    k.box((-3.4, -7.3, dz - .4), (34.4, 14.6, .8), 'ferro', 'mass', ch=.1, walk=True)
    for x in (-19.5, -12.0, -4.0, 4.0, 12.5):
        for y in (-13.5, -1.0):
            k.box((x, y, (dz - .8) / 2 + .15), (1.2, 1.2, dz - .8), 'ferro', 'mass', ch=.15)
    for y in (-14.62, .02):
        k.box((-3.4, y, dz - .2), (34.4, .06, .4), 'hazard', 'skin')
    ip.railing(k, [(13.6, -14.4), (-20.4, -14.4), (-20.4, -.2), (13.6, -.2), (13.6, -14.4)], dz, gaps=[(13.6, -11.0)])
    ip.stair(k, (19.4, -11.0, .3), (13.95, -11.0, dz), w=1.1, name='deck stair')
    count(k, 'stairs_ladders')
    for i in range(3):     # assembly cradles on the deck
        k.box((-15.0 + i * 9.0, -7.3, dz + .5), (4.0, 2.4, 1.0), 'paint.second', 'frame', ch=.08)
        k.box((-15.0 + i * 9.0, -7.3, dz + 1.3), (3.0, 1.4, .6), 'paint.steel', 'frame', ch=.06)
    # three-bay munitions bunker along the front
    I.brick_block(k, -20.6, -21.8, 3.0, -16.2, 0, 5.3, pilaster=0, cornice=False, mat='ferro.second', openings={
        's': [('rollup', -7.8, 0, 4.4, 3.8), ('rollup', 0.0, 0, 4.4, 3.8), ('rollup', 7.8, 0, 4.4, 3.8)]})
    I.roof_deck(k, -20.2, -21.4, 2.6, -16.6, 4.5)
    k.box((-8.8, -21.88, 1.2), (23.6, .1, .5), 'hazard', 'skin')
    # rear yard perimeter wall with hazard band and a gate gap, crate stacks
    for a, b in (((X0, 1.0), (X0, Y1)), ((X0, Y1), (X1, Y1)), ((X1, Y1), (X1, -6.0))):
        k.add(*ip.box_along((a[0], a[1], 2.35), (b[0], b[1], 2.35), ip.norm((-(b[1] - a[1]), b[0] - a[0], 0)), (0, 0, 1), .6, 4.7),
              'ferro', 'mass')
    for x, y in ((14.0, 14.0), (16.5, 14.0), (14.0, 16.5), (17.5, 18.0), (-16.0, 18.0)):
        k.box((x, y, .3 + .9), (2.2, 2.2, 1.8), 'wood', 'frame', ch=.06)
    # derrick crane on the rear yard: lattice mast, boom, operator cab on a slewing base
    cx, cy = 4.0, 9.5
    k.cyl((cx, cy, .9), 2.2, 1.2, 'paint.second', 'mass', sides=32, top=.1)
    k.box((cx + 1.2, cy - .4, 2.6), (3.0, 2.4, 2.2), 'paint.main', 'mass', ch=.15, top=.1)
    k.slab(cx - .2, cx + 2.6, 2.2, 3.3, cy - 1.62, cy - 1.58, 'glass', 'glass', pane=True)
    ip.lattice(k, (cx - .6, cy), 1.2, 1.5, 12.0, panel=1.8)
    tip = (cx - 9.0, cy + 6.0, 17.6)
    for s in (-.4, .4):
        k.beam((cx - .6 + s, cy - .4, 2.0), (tip[0], tip[1], tip[2]), .18, 'paint.accent', 'frame', h=.18)
    ip.tube(k, [(cx - .6, cy, 12.0), tip], .03, 'metal', 'frame', sides=5)
    ip.tube(k, [tip, (tip[0], tip[1], 6.0)], .03, 'metal', 'frame', sides=5)
    k.box((tip[0], tip[1], 5.8), (.6, .6, .4), 'paint.accent', 'frame', ch=.06)
    count(k, 'cranes')


# ------------------------------------------------------------------ registry
SPECS = {
    'factorysm': dict(fn=factorysm, palette='intent_factory', cls='large', counts=dict(doors=4, stairs_ladders=1),
        intent=('A small corrugated factory: a long, low steel-framed shed with a rear wing and an east lean-to, flat '
                'parapet roofs with pipe runs, turret fans and vents, and a raised clerestory monitor with a hipped roof '
                'over the front-right bay. Double doors front, east and rear, a roll-up in the lean-to, window ribbons; '
                'an external stair to a railed landing on the rear-wing roof.')),
    'factorylg': dict(fn=factorylg, palette='intent_factory', cls='large', counts=dict(doors=4),
        intent=('A large stepped corrugated factory: a tall main hall under an asymmetric gable (ridge toward the rear) '
                'with a louvred ventilator housing on the ridge, wrapped west and front by a low lean-to annex. Two '
                'roll-up loading doors in the rear wall, clerestory windows east, annex door, windows and louvre.')),
    'industrialcomplex': dict(fn=industrialcomplex, palette='intent_factory', cls='large', counts=dict(doors=2, wings=2),
        intent=('An industrial complex hall: two tall battered concrete wing blocks with outward-sloping roofs and panel '
                'joints flank a recessed corrugated central bay with a big roll-up door and a lit transom (front) and a '
                'double door (rear); rust-brick louvred service shafts on the battered faces; turret fans and a raised '
                'plant housing on the roofs.')),
    'processingplant': dict(fn=processingplant, palette='intent_factory', cls='large', counts=dict(stacks=2, stairs_ladders=1),
        intent=('A processing plant: two tapered steel process flues rise from the rear-west of a corrugated processing '
                'block; a taller front block with an entry canopy and louvre plant room; an orange safety band at '
                'waist height all round (the stock band); a caged ladder from the roof to a railed flue sampling platform.')),
    'purificationplant': dict(fn=purificationplant, palette='intent_brick', cls='large', counts=dict(tanks=2, stairs_ladders=2),
        intent=('A water purification plant: a brick pump house with a steel flue and a lower brick plant hall (roll-up '
                'door), and two round clarifier tanks with red bands and low conical covers, each with a railed rim '
                'walk reached by a ladder and fed by a sloping pipe bridge from the hall.')),
    'chemicalplant': dict(fn=chemicalplant, palette='intent_brick', cls='large', counts=dict(tanks=3, stairs_ladders=1),
        intent=('A chemical plant: a tall brick reactor house with louvred upper vents and roof fans, a stepped lower '
                'brick front with a roll-up door, and a row of three pale silos with green bands along the west side; '
                'a railed catwalk runs over the silo tops into an upper door of the house, reached by a caged ladder; '
                'pipe bridges from the silos.')),
    'powergenerator': dict(fn=powergenerator, palette='intent_gen', cls='large', counts=dict(halls=1, wings=2),
        intent=('A power generator: a long battered armoured turbine hall with framed cooling slots glowing on the sides '
                'and roof, flanked by two louvred radiator wings on a ferrocrete plinth; cable conduit and a hall door.')),
    'l_turretgenerator': dict(fn=l_turretgenerator, palette='intent_gen', cls='small', counts=dict(halls=1, wings=2),
        intent=('A defence power plant: the generator family\'s battered armoured hall with glowing cooling slots on the '
                'sides and roof, with two short louvred side pods on a ferrocrete plinth.')),
    'valvestation': dict(fn=valvestation, palette='intent_factory', cls='small', counts=dict(valves=3, stairs_ladders=1),
        intent=('A valve station: a tall corrugated valve hall with roll-up doors front and rear; on its west side a '
                'brick manifold house carries three pressure columns whose lines cross on gate valves into flanged '
                'connections on the hall wall; a caged ladder to the railed manifold-house roof.')),
    'transformerstation': dict(fn=transformerstation, palette='intent_factory', cls='large', counts=dict(transformers=2, stairs_ladders=1),
        intent=('A transformer station: a raised ferrocrete switchyard deck behind a railing, reached by a steel stair; '
                'two large transformer units with radiator banks, fans, HV bushings and conservator drums, plus control '
                'cabinets.')),
    'wasteprocessor': dict(fn=wasteprocessor, palette='intent_brick', cls='large', counts=dict(tanks=3, doors=2),
        intent=('A waste processor: a low brick processing hall with a clerestory window band, roll-up door and roof '
                'louvre units, fed by three round digester tanks with red bands and conical hoppers through covered '
                'conveyor chutes; vent stacks on the tanks.')),
    'weaponsfacility': dict(fn=weaponsfacility, palette='intent_factory', cls='large', counts=dict(stairs_ladders=1, cranes=1),
        intent=('A weapons facility yard: a raised ferrocrete assembly deck on piers with hazard-banded edges, railings '
                'and a steel stair, holding assembly cradles; a three-bay munitions bunker with hazard band along the '
                'front; a perimeter wall round the rear yard with crate stacks and a derrick crane (lattice mast, '
                'yellow boom, operator cab).')),
}
