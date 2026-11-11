"""Military base family (split from intent_military.py): walls and gates, batch 3 massing helpers and the first controls and HQs."""

import math
import numpy as np

import ind_parts as ip
import intent as I
import intent_power as PW

from intent_military_parts import anim_node, antenna, contextmanager, count, dish, loft, radar_dipole, rring, slit_window, stencil
from intent_military_field import mil_hall, roof_kit, turret_control


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
