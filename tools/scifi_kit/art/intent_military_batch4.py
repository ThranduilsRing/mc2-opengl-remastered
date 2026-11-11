"""Military base family (split from intent_military.py): batch 4: high-power-generator corridors, traffic control, terminals, support sheds."""

import math

import ind_parts as ip
import intent_power as PW

from intent_military_parts import blast_door, count, frame_of, loft, radar_dipole, rring
from intent_military_rebuild import barrel, block, cab, clutter, doors, glazed_bays, ribbon, tower_storeys


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
