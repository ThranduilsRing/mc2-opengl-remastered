"""Military base family (split from intent_military.py): batch 3 rebuild: block/ribbon/cab primitives and the rebuilt HQs, gate, sensor and spotlight controls."""

import math

import geo
import ind_parts as ip
import intent as I

from intent_military_parts import anim_node, antenna, blast_door, count, frame_of, loft, radar_dipole, rring, stencil
from intent_military_field import blast_wall
from intent_military_walls import helipad, searchlight, sensor_array, spin_dish


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
