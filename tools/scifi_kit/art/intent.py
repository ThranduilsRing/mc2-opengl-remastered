"""Intent pass (owner 2026-10-24): each remake captures the asset's INTENT with clean
industrial kit parts (ind_parts.py). The stock mesh is a loose guide for footprint,
height and where the big masses sit; it is never a contour to fit.

Every asset: INTENT (one paragraph from the five-side stock study), a builder
assembled from the parts library, key COUNTS, and a palette. Pristine only until
the owner signs off shapes. Validation (check()) is loose: footprint/height within
tolerance, coarse mass placement, declared key counts. No contour IoU.
"""
import math
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import geo  # noqa: E402
import ind_parts as ip  # noqa: E402
from geo import Kit  # noqa: E402

TOL = dict(footprint=.15, height=.10, grid_mae=.22, centroid=.12)
BUDGET = {'prop': 12000, 'small': 30000, 'large': 60000}   # owner 2026-10-24: small may go to 30k for the bevel; others in proportion


# ================================================================== shared helpers
def faces_of(x0, y0, x1, y1, t):
    """(side, origin, yaw, length) of a rectangular block; local x = viewer's right, outward -Y.
    Side walls are shortened by the wall thickness so corners never overlap coplanar."""
    xc, yc = (x0 + x1) / 2, (y0 + y1) / 2
    return {'s': ((xc, y0, 0), 0.0, x1 - x0, 0.0),
            'e': ((x1, yc, 0), math.pi / 2, y1 - y0 - 2 * t, t),
            'n': ((xc, y1, 0), math.pi, x1 - x0, 0.0),
            'w': ((x0, yc, 0), -math.pi / 2, y1 - y0 - 2 * t, t)}


YAW = {'s': -math.pi / 2, 'e': 0.0, 'n': math.pi / 2, 'w': math.pi}   # outward direction angle per side


def brick_block(k, x0, y0, x1, y1, z0, z1, sides='nesw', t=.45, openings=None, pilaster=4.6, cornice=True,
                plinth=.6, belt=None, blind=False, mat='brick.main'):
    """Load-bearing brick block: plinth, wall faces cut for openings, brick pilasters,
    corbelled cornice, optional belt course and recessed blind panels.
    openings: side -> list of (kind, x_local, z0, w, h[, extra]); kind window|door|door2|rollup|vent."""
    openings = openings or {}
    F = faces_of(x0, y0, x1, y1, t)
    for s in sides:
        origin, yaw, L, _ = F[s]
        ops = openings.get(s, [])
        holes, notches = [], []
        for op in ops:
            kind, xl, zb, w, h = op[:5]
            (notches if kind == 'notch' else holes).append((xl - w / 2, zb, xl + w / 2, zb + h))
        allh = holes + notches
        with k.at(origin, yaw=yaw):
            a, b = -L / 2, L / 2
            rec = []
            if blind:
                nb_ = max(1, round(L / pilaster))
                for i in range(nb_):
                    xa, xb = a + .3 + (L - .6) * i / nb_ + .55, a + .3 + (L - .6) * (i + 1) / nb_ - .55
                    rec += [(xa, za, xb, zb_) for za, zb_ in blind if za >= z0]
            ip.wall_frame(k, a, b, z0, z1, 0.0, holes, mat=mat, t=t, recess=rec, cut=notches)
            for xa, za, xb, zb_ in rec:
                k.slab(xa - .05, xb + .05, za - .12, za, -.06, .05, 'ferro', 'frame', front=.02)
            if plinth and z0 < .1:
                for r in geo.subtract_rect((a - .06, 0, b + .06, plinth), [hh for hh in holes if hh[1] < plinth]):
                    k.slab(r[0], r[2], r[1], r[3], -.07, .05, 'ferro', 'mass', front=.03)
            # pilasters at bay lines, kept clear of openings
            if pilaster:
                n = max(1, round(L / pilaster))
                for i in range(n + 1):
                    x = a + .3 + (L - .6) * i / n
                    if any(hh[0] - .45 < x < hh[2] + .45 for hh in allh):
                        continue
                    k.slab(x - .3, x + .3, max(z0, plinth if z0 < .1 else z0), z1 - (.9 if cornice else 0), -.16, .02, mat, 'skin')
            if belt:
                for za, zb in belt:
                    for r in geo.subtract_rect((a - .02, za, b + .02, zb), allh):
                        k.slab(r[0], r[2], r[1], r[3], -.2, .04, 'paint.belt', 'frame', front=.03)
            if cornice:
                for r in geo.subtract_rect((a - .04, z1 - .9, b + .04, z1 - .62), notches):
                    k.slab(r[0], r[2], r[1], r[3], -.1, .05, mat, 'skin')
                for r in geo.subtract_rect((a - .08, z1 - .62, b + .08, z1 - .3), notches):
                    k.slab(r[0], r[2], r[1], r[3], -.2, .05, 'brick.second', 'skin')
                for r in geo.subtract_rect((a - .14, z1 - .3, b + .14, z1), notches):
                    k.slab(r[0], r[2], r[1], r[3], -.3, t, 'paint.coping', 'frame', front=.05, walk=True)
            for op in ops:
                kind, xl, zb, w, h = op[:5]
                xa, xb = xl - w / 2, xl + w / 2
                if kind == 'window':
                    ip.steel_window(k, xa, xb, zb, zb + h, 0.0)
                    count(k, 'windows')
                elif kind in ('door', 'door2'):
                    with k.at((0, 0, zb)):
                        ip.personnel_door(k, xl, w, h, 0.0, leaves=2 if kind == 'door2' else 1, apron=zb < .1)
                    count(k, 'doors')
                    wall_lamp(k, xl, zb + h + .95, yaw)
                elif kind == 'rollup':
                    with k.at((0, 0, zb)):
                        ip.rollup_door(k, xl, w, h, 0.0, grade=zb)
                    count(k, 'doors')
                    wall_lamp(k, xl, zb + h + 1.3, yaw)
                elif kind == 'vent':
                    k.slab(xa, xb, zb, zb + h, .05, .12, 'grate', 'frame')
                    for j in range(int(h / .16)):
                        k.slab(xa, xb, zb + j * .16 + .03, zb + j * .16 + .1, -.02, .1, 'paint.second', 'frame')
                    k.slab(xa - .1, xb + .1, zb - .1, zb + h + .1, -.06, .04, 'paint.trim', 'frame')


def yaw_out(yaw):
    return yaw - math.pi / 2


def wall_lamp(k, xl, z, yaw):
    """Lamp on the current local wall frame (outward -Y) plus its world-space door socket."""
    ip.lamp(k, xl, -.02, z, -math.pi / 2)
    f = yaw_out(yaw)
    p = k.world((xl, -.3, z - .1))
    k.meta['sockets'].append(dict(name='LIGHT_door_%02d' % (len(k.meta['sockets']) + 1), position_m=p,
                                  direction=(math.cos(f) * .6, math.sin(f) * .6, -1), type='spot', intensity_class='door'))


def count(k, key, n=1):
    c = k.meta.setdefault('counts', {})
    c[key] = c.get(key, 0) + n


def roof_deck(k, x0, y0, x1, y1, z, mat='gravel'):
    k.box(((x0 + x1) / 2, (y0 + y1) / 2, z - .15), (x1 - x0, y1 - y0, .3), mat, 'roof', walk=True)


def lamp_world(k, x, y, z, face):
    ip.lamp(k, x, y, z, YAW[face], k.meta['sockets'])


# ================================================================== smallfueltank
SFT = dict(c=(.55, 0.0), r=4.05, z_bot=2.75, z_top=6.7, roof=1.6, col=(-3.3, 0.0), col_r=1.3, col_top=9.85)


def smallfueltank(k):
    s = SFT
    c, r = s['c'], s['r']
    # pad: round ferrocrete pad with a chamfered edge and a drain kerb
    ip.lathe(k, [(4.78, 0.0), (4.78, .28), (4.62, .44), (0.0, .44)], 'ferro', 'mass', n=72, walk=True)
    ip.lathe(k, [(4.5, .44), (4.5, .52), (4.38, .52), (4.38, .44)], 'hazard.wrap', 'frame', n=72, c=(0, 0), cap=False,
             uv_scale=(36 / 12, 1 / .78))
    z0 = .44
    # stand: eight H-columns, ring girder, X-bracing between columns
    legs = 8
    for i in range(legs):
        a = (i + .5) * math.tau / legs
        p = ip.polar(c, 3.35, a, z0)
        k.box((p[0], p[1], z0 + .04), (.6, .6, .08), 'paint.steel', 'frame', ch=.05)
        ip.hbeam(k, p, (p[0], p[1], s['z_bot'] - .25), .32, .28)
        b = ip.polar(c, 3.35, (i + 1.5) * math.tau / legs, z0)
        ip.angle_brace(k, (p[0], p[1], z0 + .3), (b[0], b[1], s['z_bot'] - .5), s=.08)
        ip.angle_brace(k, (b[0], b[1], z0 + .3), (p[0], p[1], s['z_bot'] - .5), s=.08)
    ip.lathe(k, [(3.55, s['z_bot'] - .45), (3.55, s['z_bot']), (3.15, s['z_bot']), (3.15, s['z_bot'] - .45)],
             'paint.steel', 'frame', n=64, c=c)
    count(k, 'legs_supports', legs)
    # vessel: welded courses, conical roof, conical (hopper) bottom
    # hazmat zone over the lower half of the shell (owner: "hazmat yellow-black on half of it"), part of the shell surface
    zb_hi = s['z_bot'] + (s['z_top'] - s['z_bot']) * .52
    ip.vessel(k, c, r, s['z_bot'], s['z_top'], top='cone', bottom='cone', courses=3, mat='paint.main', head_mat='paint.roof',
              bands=[(s['z_bot'] + .12, zb_hi, 'hazard.wrap')])
    count(k, 'tanks')
    ip.ring_band(k, c, r, zb_hi, zb_hi + .1, 'paint.trim', proud=.035)
    ip.ring_band(k, c, r, s['z_top'] - .3, s['z_top'] - .08, 'paint.second', proud=.12)     # wind girder
    for i in range(16):      # roof plate lap ribs, rim to crown
        a = i * math.tau / 16
        k.beam(ip.polar(c, r - .05, a, s['z_top'] + .03), ip.polar(c, .45, a, s['z_top'] + s['roof'] * (1 - .45 / r) + .03), .07, 'paint.roof', 'frame', h=.04)
    # ACCESS (owner 2026-10-24: ladders are a path, landing through a rail opening, nothing passes through):
    # front-side caged shell ladder pad -> roof walkway; column ladders pad -> P1 -> P2 on the pad side.
    ztop = s['z_top']
    apex = ztop + s['roof']
    mid = lambda d: ztop + s['roof'] * (1 - d / r)  # noqa: E731
    a_l = math.radians(250)
    # roof walkway: grated strip on the cone from the rim to the centre gauge nozzle, handrail on one side
    side = (-math.sin(a_l), math.cos(a_l), 0.0)
    wr = ip._aid(k)
    wa, wb = ip.polar(c, r - .05, a_l, mid(r - .05) + .07), ip.polar(c, .75, a_l, mid(.75) + .07)
    k.add(*ip.box_along(wa, wb, side, (0, 0, 1), .8, .06), 'grate', 'frame', walk=True)
    for d in (r - .6, 2.0, .9):
        p = ip.add(ip.polar(c, d, a_l, mid(d) + .08), ip.mul(side, .45))
        ip.tube(k, [p, ip.add(p, (0, 0, 1.0))], .024, ip.RAIL, 'frame', sides=6, access='post', aid=wr)
    ra, rb = ip.add(ip.polar(c, r - .6, a_l, mid(r - .6) + 1.08), ip.mul(side, .45)), ip.add(ip.polar(c, .9, a_l, mid(.9) + 1.08), ip.mul(side, .45))
    ip.tube(k, [ra, rb], .028, ip.RAIL, 'frame', sides=8, access='rail', aid=wr)
    # roof-edge railing either side of the ladder head, open at the head
    gap = ip.polar(c, r - .25, a_l, 0)[:2]
    ip.railing(k, ip.circle_pts(c, r - .25, math.radians(205), math.radians(295)), mid(r - .25) + .02, h=1.05, gaps=[gap])
    for d, a, kind in ((1.6, math.radians(20), 'vent'), (2.2, math.radians(140), 'vent'), (1.7, math.radians(285), 'hatch')):
        p = ip.polar(c, d, a, mid(d))
        if kind == 'vent':
            ip.mushroom_vent(k, p[0], p[1], p[2] - .1, .28)
        else:
            ip.hatch(k, p[0], p[1], p[2] - .15, .8)
    k.cyl((c[0], c[1], apex + .1), .35, .3, 'paint.second', 'frame', sides=20, top=.05)
    ip.flange(k, (c[0], c[1], apex + .27), (0, 0, 1), .45, t=.08)
    lp = ip.polar(c, r, a_l, 0)
    ip.ladder(k, lp[0], lp[1], a_l, z0, ztop, off=.18, name='tank shell ladder')
    count(k, 'stairs_ladders')
    # bottom outlet: hopper apex -> gate valve -> buried line through the pad (front)
    apex_b = s['z_bot'] - r * .40
    ip.pipe(k, [(c[0], c[1], apex_b), (c[0], c[1], .95), (c[0], -3.0, .95), (c[0], -3.0, .1)], .16, 'paint.pipe', end_flanges=False)
    ip.flange(k, (c[0], c[1], apex_b + .05), (0, 0, 1), .24, t=.06)
    ip.valve(k, (c[0], -2.0, .95), (0, -1, 0), .16)
    ip.pipe_support(k, c[0], -2.6, .95 - .16, .16, w=.5)
    ip.lathe(k, [(.42, .44), (.42, .56), (.3, .56), (.3, .44)], 'ferro', 'frame', n=16, c=(c[0], -3.0))     # pipe sleeve at the pad
    # two shell nozzles (the stock's round ports) with gate valves, dropping into buried lines
    for a in (math.radians(300), math.radians(330)):
        n0 = ip.polar(c, r, a, s['z_bot'] + .45)
        n1 = ip.polar(c, r + .55, a, s['z_bot'] + .45)
        ip.pipe(k, [n0, n1, (n1[0], n1[1], .1)], .11, 'paint.pipe', end_flanges=False)
        ip.flange(k, ip.polar(c, r + .04, a, s['z_bot'] + .45), ip.sub(n1, n0), .17, t=.05)
        ip.valve(k, ip.polar(c, r + .3, a, s['z_bot'] + .45), ip.sub(n1, n0), .11)
        ip.lathe(k, [(.3, .44), (.3, .54), (.2, .54), (.2, .44)], 'ferro', 'frame', n=16, c=n1[:2])
    # process column at the -X flank: skirt, shell, two railed platforms, two caged ladders, overhead line
    cc, cr = s['col'], s['col_r']
    ip.lathe(k, [(cr + .12, z0), (cr + .12, z0 + .3), (cr, z0 + .42), (cr, 1.5)], 'paint.second', 'mass', n=40, c=cc)
    ip.vessel(k, cc, cr, 1.5, s['col_top'], top='ellip', bottom='flat', courses=5, mat='paint.main', head_mat='paint.main',
              bands=[(3.0, 3.6, 'paint.accent')])
    count(k, 'tanks')
    ip.mushroom_vent(k, cc[0], cc[1], s['col_top'] + cr * .5 - .05, .22)
    p1z, p2z = 5.9, s['col_top'] - .35
    p1o, p2o = cr + 1.15, cr + .75
    a1, a2 = math.radians(238), math.radians(195)
    ip.ring_platform(k, cc, cr + .05, p1o, p1z, math.radians(140), math.radians(252), brackets=5,
                     gaps=[ip.polar(cc, p1o - .05, a1, 0)[:2]], name='column platform 1')
    ip.ring_platform(k, cc, cr + .05, p2o, p2z, math.radians(150), math.radians(330), brackets=5,
                     gaps=[ip.polar(cc, p2o - .05, a2, 0)[:2]], name='column platform 2')
    l1 = ip.polar(cc, p1o, a1, 0)
    ip.ladder(k, l1[0], l1[1], a1, 0.0, p1z, off=.18, reach=p1o - cr + .18, standoff=False, name='column ladder to P1')
    l2 = ip.polar(cc, p2o, a2, 0)
    ip.ladder(k, l2[0], l2[1], a2, p1z, p2z, off=.18, reach=p2o - cr + .18, standoff=False, name='column ladder P1 to P2')
    count(k, 'stairs_ladders', 2)
    top_out = (cc[0] + cr, cc[1], s['col_top'] - 1.2)
    ip.pipe(k, [top_out, (-.5, 0, s['col_top'] - 1.2), (-.5, 0, mid(1.05) + .1)], .17, 'paint.pipe', bend=.7)
    ip.pipe(k, [(cc[0] + cr, cc[1] + .4, 2.1), (cc[0] + 2.4, .4, 2.1), (cc[0] + 2.4, .4, 1.3), (c[0] - .15, .4, 1.3), (c[0] - .15, .17, 1.3)],
            .12, 'paint.pipe', end_flanges=False)
    ip.flange(k, (cc[0] + cr + .03, cc[1] + .4, 2.1), (1, 0, 0), .18, t=.05)
    ip.valve(k, (cc[0] + 1.85, .4, 2.1), (1, 0, 0), .12)
    ip.gauge_board(k, *ip.polar(cc, cr + .02, math.radians(235), 0)[:2], 1.9, math.radians(235))
    ip.lamp(k, *ip.polar(cc, cr + .03, math.radians(250), 0)[:2], 3.2, math.radians(250), k.meta['sockets'], 'ladder')
    k.meta['hero_access'] = ip.polar(c, r, a_l, ztop)


# ================================================================== pipetank
PT = dict(c=(5.04, 0.0), r=5.72, z_bot=5.75, z_top=14.9, ped_r=4.3, ped_h=5.0, pipe_z=1.62, pipe_r=.55)


def pipetank(k):
    s = PT
    c, r = s['c'], s['r']
    # ferrocrete pedestal (pump room): battered drum, plinth, ring beam
    ip.lathe(k, [(s['ped_r'] + .5, 0.0), (s['ped_r'] + .5, .5), (s['ped_r'] + .25, .7), (s['ped_r'] - .2, s['ped_h']), (0, s['ped_h'])],
             'ferro', 'mass', n=64, c=c)
    ip.lathe(k, [(s['ped_r'] - .1, s['ped_h'] - .05), (s['ped_r'] + .05, s['ped_h'] - .05), (s['ped_r'] + .05, s['ped_h'] + .25),
                 (s['ped_r'] - .1, s['ped_h'] + .25)], 'paint.second', 'frame', n=64, c=c)
    # steel bearing ring and cantilever brackets carrying the tank overhang
    ip.lathe(k, [(s['ped_r'], s['ped_h'] + .25), (s['ped_r'] + .25, s['ped_h'] + .25), (s['ped_r'] + .25, s['z_bot']), (s['ped_r'], s['z_bot'])],
             'paint.steel', 'frame', n=64, c=c)
    nb = 12
    for i in range(nb):
        a = (i + .5) * math.tau / nb
        ip.hbeam(k, ip.polar(c, s['ped_r'] - .05, a, s['ped_h'] - 1.6), ip.polar(c, r - .3, a, s['z_bot'] - .05), .26, .2)
        ip.hbeam(k, ip.polar(c, s['ped_r'] + .1, a, s['z_bot'] - .15), ip.polar(c, r - .1, a, s['z_bot'] - .15), .26, .2)
    count(k, 'legs_supports', nb)
    # pedestal openings: pump-room door (front), louvred vents, hazard-marked service hatches
    for a, kind in ((-math.pi / 2, 'door'), (math.radians(-30), 'vent'), (math.radians(-150), 'vent'), (math.radians(30), 'hatch'),
                    (math.radians(150), 'hatch'), (math.pi / 2, 'vent')):
        p = ip.polar(c, s['ped_r'] + .5, a, 0)
        with k.at((p[0], p[1], 0), yaw=a + math.pi / 2):
            # flat-faced cast surround standing proud of the battered drum
            k.box((0, .55, 2.0), (2.5, 1.1, 4.0), 'ferro', 'mass', ch=.12, top=.1)
            if kind == 'door':
                ip.personnel_door(k, 0, 1.4, 2.4, -.3, leaves=2)
                count(k, 'doors')
            elif kind == 'vent':
                k.slab(-.9, .9, 2.3, 3.6, -.24, .05, 'paint.trim', 'frame', front=.03)
                for j in range(7):
                    k.slab(-.8, .8, 2.4 + j * .17, 2.48 + j * .17, -.3, -.1, 'grate', 'frame')
            else:
                k.slab(-.85, .85, .9, 2.6, -.26, .05, 'hazard', 'skin', front=.03)
                k.slab(-.65, .65, 1.1, 2.4, -.3, -.2, 'paint.second', 'frame', front=.02)
        if kind == 'door':
            ip.lamp(k, p[0], p[1] - .35, 3.0, -math.pi / 2, k.meta['sockets'])
    # the tank: plate courses, low domed roof, wind girder, upper hazard band
    ip.vessel(k, c, r, s['z_bot'], s['z_top'], bottom='flat', courses=5, mat='paint.main', head_mat='paint.roof', n=72,
              bands=[(12.0, 13.5, 'hazard.wrap')],
              top_prof=[(r, s['z_top']), (r - .9, s['z_top'] + .35), (r * .5, s['z_top'] + .58), (0, s['z_top'] + .65)])
    count(k, 'tanks')
    ip.ring_band(k, c, r, s['z_bot'], s['z_bot'] + .22, 'paint.second', proud=.06, n=72)
    ip.ring_band(k, c, r, s['z_top'] - .28, s['z_top'] - .02, 'paint.second', proud=.14, n=72)     # wind girder
    ip.ring_band(k, c, r, 11.88, 12.0, 'paint.trim', proud=.03, n=72)
    ip.ring_band(k, c, r, 13.5, 13.62, 'paint.trim', proud=.03, n=72)
    # refinery fittings on the shell: bottom-course manhole, float level gauge, foam/product riser on brackets
    mh = math.radians(-20)
    with k.at(ip.polar(c, r, mh, 0), yaw=mh + math.pi / 2):
        ip.tube(k, [(0, .05, 6.7), (0, -.35, 6.7)], .42, 'paint.main', 'frame', sides=24)
        ip.flange(k, (0, -.36, 6.7), (0, -1, 0), .55, t=.1)
        k.box((.62, -.4, 6.7), (.12, .16, .5), 'metal', 'frame')
    gz = math.radians(-60)
    with k.at(ip.polar(c, r, gz, 0), yaw=gz + math.pi / 2):
        k.box((0, -.18, (s['z_bot'] + 1.0 + s['z_top']) / 2), (.34, .1, s['z_top'] - s['z_bot'] - 1.2), 'paint.second', 'frame')
        k.box((0, -.25, 9.6), (.26, .06, .5), 'paint.accent', 'frame')
        for zz in (s['z_bot'] + 1.0, s['z_top'] - .3):
            k.beam((0, 0, zz), (0, -.3, zz), .08, 'paint.steel', 'frame', h=.08)
    rz = math.radians(20)
    rp = lambda d, z: ip.polar(c, r + d, rz, z)  # noqa: E731
    ip.pipe(k, [rp(.55, .6), rp(.55, s['z_top'] + .5), ip.polar(c, r - .6, rz, s['z_top'] + .5), ip.polar(c, r - .6, rz, s['z_top'] + .1)], .13, 'paint.pipe2', bend=.5)
    for zz in ip.frange(s['z_bot'] + .8, s['z_top'] - .3, 2.2):
        k.beam(rp(.0, zz), rp(.55, zz), .07, 'paint.steel', 'frame', h=.07)
    k.box(rp(.55, .3)[:2] + (.3,), (.6, .6, .6), 'ferro', 'mass', ch=.05)
    # roof: perimeter railing, centre gauge hatch, breather vents, walkway from the stair head
    zr = s['z_top'] + .05
    ip.railing(k, ip.circle_pts(c, r - .2, 0, math.tau, 40), zr + .05, closed=True,
               gaps=[ip.polar(c, r - .2, math.radians(120), 0)[:2]])
    roofz = lambda d: s['z_top'] + .65 * (1 - (d / r) ** 1.6)  # noqa: E731
    k.cyl((c[0], c[1], roofz(0) + .25), .55, .5, 'paint.second', 'frame', sides=24, top=.06)
    ip.hatch(k, c[0], c[1], roofz(0) + .45, .7)
    for d, a in ((3.0, math.radians(40)), (3.0, math.radians(160)), (3.0, math.radians(280))):
        p = ip.polar(c, d, a, roofz(d))
        ip.mushroom_vent(k, p[0], p[1], p[2] - .05, .35)
    aw = math.radians(120)      # roof walkway from the rim landing to the centre gauge hatch
    sd = (-math.sin(aw), math.cos(aw), 0.0)
    for d0, d1 in ((r - .5, r * .62), (r * .62, r * .3), (r * .3, .8)):
        pa, pb = ip.polar(c, d0, aw, roofz(d0) + .07), ip.polar(c, d1, aw, roofz(d1) + .07)
        k.add(*ip.box_along(pa, pb, sd, (0, 0, 1), .8, .06), 'grate', 'frame', walk=True)
    wr = ip._aid(k)
    for d in (r - 1.2, r * .55, 1.0):
        q = ip.add(ip.polar(c, d, aw, roofz(d) + .08), ip.mul(sd, .45))
        ip.tube(k, [q, ip.add(q, (0, 0, 1.0))], .024, ip.RAIL, 'frame', sides=6, access='post', aid=wr)
    ip.tube(k, [ip.add(ip.polar(c, d, aw, roofz(d) + 1.08), ip.mul(sd, .45)) for d in (r - 1.2, r * .55, 1.0)], .028, ip.RAIL,
            'frame', sides=8, access='rail', aid=wr)
    # access: caged ladder from grade to a tank-bottom landing, then a spiral stair up the -X shell to a rim landing
    zl = s['z_bot'] - .12
    a_lo, a_in = math.radians(236), math.radians(250)
    lad = ip.polar(c, r + 1.15, a_in, 0)
    ip.ring_platform(k, c, r + .05, r + 1.15, zl, a_lo, math.radians(262), brackets=3,
                     gaps=[ip.polar(c, r + 1.1, a_in, 0)[:2], ip.polar(c, r + .6, a_lo, 0)[:2]], name='tank-bottom landing')
    ip.ladder(k, lad[0], lad[1], a_in, 0.0, zl, off=.18, reach=.3, standoff=False, name='grade ladder')
    zt = s['z_top'] + .12
    a_top = math.radians(128)
    ip.ring_platform(k, c, r - .15, r + 1.15, zt, math.radians(110), a_top + .02, brackets=3, rail=True,
                     gaps=[ip.polar(c, r + .55, a_top, 0)[:2]], name='rim landing')
    a_h = a_lo + math.radians(3)
    ip.helix_stair(k, c, r + .16, zl + .12, zt, a_h, turn=-(a_h - a_top) / math.tau)
    count(k, 'stairs_ladders', 2)
    # transfer pipeline leaving the pedestal on -X, on T-sleepers, gate valve and end flange
    pz, pr = s['pipe_z'], s['pipe_r']
    x_in = c[0] - s['ped_r'] - .1
    ip.pipe(k, [(x_in, 0, pz), (-10.75, 0, pz)], pr, 'paint.pipe', flanges=[((x, 0, pz), (1, 0, 0)) for x in (-1.0, -5.5)])
    ip.valve(k, (-8.9, 0, pz), (1, 0, 0), pr)
    for x in (-2.8, -6.6, -10.1):
        k.box((x, 0, .2), (1.0, 3.2, .4), 'ferro', 'mass', ch=.08, top=.06)
        ip.hbeam(k, (x, 0, .4), (x, 0, pz - pr - .12), .22, .22)
        k.box((x, 0, pz - pr - .07), (.3, 3.0, .12), 'paint.steel', 'frame')
    # companion product/steam lines on the same sleepers, rising into the pedestal
    for y, rr in ((1.15, .17), (-1.15, .14)):
        pts = [(-10.3, y, pz - pr - .01 + rr), (x_in - 1.0, y, pz - pr - .01 + rr), (x_in - .2, y, 2.9), (x_in + .2, y, 2.9)]
        ip.pipe(k, pts, rr, 'paint.pipe2' if y > 0 else 'paint.pipe', bend=.6)
    # small pump skid on +Y beside the pedestal
    k.box((c[0] - 2.6, 4.2, .15), (2.4, 1.3, .3), 'ferro', 'mass', ch=.05)
    k.cyl((c[0] - 3.2, 4.2, .7), .35, .8, 'paint.valve', 'frame', sides=20, top=.05)
    ip.tube(k, [(c[0] - 2.85, 4.2, .7), (c[0] - 1.9, 4.2, .7)], .25, 'paint.steel', 'frame', sides=16)
    ip.pipe(k, [(c[0] - 3.2, 4.2, 1.1), (c[0] - 3.2, 4.2, 1.6), (c[0] - 3.2, 3.4, 1.6)], .1, 'paint.pipe')
    ip.gauge_board(k, *ip.polar(c, s['ped_r'] + .2, math.radians(-115), 0)[:2], 1.6, math.radians(-115))


# ================================================================== industrialplant
IP = dict(x0=-10.46, x1=10.46, y0=-11.83, y1=11.83, hx1=1.3, hy0=-.3, low=8.4, hall=24.9, deck=7.7)


def industrialplant(k):
    s = IP
    x0, x1, y0, y1 = s['x0'], s['x1'], s['y0'], s['y1']
    hx1, hy0, low, hall = s['hx1'], s['hy0'], s['low'], s['hall']
    # low wing A (full-width front band) and B (rear-right), one brick language
    brick_block(k, -6.5, y0, x1, hy0, 0, low, sides='swe', openings={
        's': [('window', 1.5, 2.2, 2.0, 2.4), ('window', 5.2, 2.2, 2.0, 2.4), ('window', 8.6, 2.2, 2.0, 2.4)],
        'w': [('door2', 3.2, .0, 2.0, 2.6), ('window', -2.0, 2.2, 1.8, 2.4)],
        'e': [('door2', -.8, .0, 2.0, 2.6), ('window', 3.2, 2.2, 1.8, 2.4)]})
    brick_block(k, hx1, hy0 - .9, x1, y1, 0, low, sides='en', openings={
        'n': [('window', 1.2, 2.2, 1.8, 2.4), ('window', 3.9, 2.2, 1.8, 2.4), ('vent', -2.4, 4.8, 1.4, 1.2)],
        'e': [('window', -2.6, 2.2, 1.8, 2.4), ('window', 2.6, 2.2, 1.8, 2.4)]})
    roof_deck(k, -6.1, y0 + .4, x1 - .4, hy0 + .05, s['deck'])
    roof_deck(k, hx1, hy0 - .2, x1 - .4, y1 - .4, s['deck'])
    # the boiler hall: tall brick block, belt courses, blind panels, louvred roof ventilator
    brick_block(k, x0, hy0 - .9, hx1, y1, 0, hall, sides='wn', pilaster=3.9, belt=[(15.65, 16.4), (7.9, 8.4)],
                blind=[(9.2, 15.0), (17.2, 22.6)], openings={'n': [('door', 3.0, 0, 1.3, 2.4)]})
    brick_block(k, x0, hy0 - .9, hx1, y1, 0, hall, sides='s', pilaster=3.9, belt=[(15.65, 16.4), (7.9, 8.4)],
                blind=[(9.2, 15.0), (17.2, 22.6)], openings={'s': [('door2', -3.9, 0, 2.0, 2.6)]})
    brick_block(k, x0, hy0 - .9, hx1, y1, low - .7, hall, sides='e', pilaster=3.9, belt=[(15.65, 16.4)], plinth=0,
                blind=[(9.2, 15.0), (17.2, 22.6)], openings={'e': [('notch', 9.6 - ((hy0 - .9) + y1) / 2, hall - .8, 1.3, .8)]})
    roof_deck(k, x0 + .4, hy0 + .4, hx1 - .4, y1 - .4, hall - .8)
    hc = ((x0 + hx1) / 2, (hy0 + y1) / 2)
    ip.louvre_box(k, hc[0], hc[1], hall - .8, 5.6, 2.6, 1.3)
    ip.louvre_box(k, hc[0], hc[1] + 2.6, hall - .8, 5.6, 2.4, 1.0, face='n')
    for dx in (-3.6, 3.6):
        ip.mushroom_vent(k, hc[0] + dx, hc[1] - 3.2, hall - .8, .32)
    # hot-gas flue: burner unit on the low roof, insulated duct with cladding bands and
    # bellows, carried by a lattice support tower, elbowing into the hall's flank
    fx, fy, dr = 5.4, 4.6, 1.2
    k.box((fx, fy, s['deck'] + 1.0), (4.0, 3.6, 2.0), 'paint.second', 'mass', ch=.12, top=.1)
    k.box((fx, fy, s['deck'] + 2.06), (4.1, 3.7, .12), 'paint.trim', 'frame', ch=.1)
    for j in range(6):
        k.slab(fx - 1.6, fx + 1.6, s['deck'] + .35 + j * .22, s['deck'] + .45 + j * .22, fy - 1.86, fy - 1.78, 'grate', 'frame')
    z_el = 18.3
    path = ip.fillet([(fx, fy, s['deck'] + 2.1), (fx, fy, z_el), (hx1 + .05, fy, z_el)], 2.4)
    ip.tube(k, path, dr, 'paint.duct', 'mass', sides=32)
    for z in ip.frange(s['deck'] + 3.2, z_el - 2.6, 1.6):
        ip.ring_band(k, (fx, fy), dr, z - .05, z + .05, 'paint.second', proud=.03)
    for j in range(4):
        ip.ring_band(k, (fx, fy), dr, s['deck'] + 2.3 + j * .16, s['deck'] + 2.38 + j * .16, 'paint.steel', proud=.09)
    ip.flange(k, (hx1 + .25, fy, z_el), (1, 0, 0), dr + .25, t=.2)
    ip.lattice(k, (fx, fy), 4.4, s['deck'], z_el - 3.0, panel=2.6)
    for z in (11.0, 14.5):
        ip.lathe(k, [(dr + .05, z - .12), (dr + .25, z - .12), (dr + .25, z + .12), (dr + .05, z + .12)], 'paint.steel', 'frame', n=32, c=(fx, fy))
    ip.rect_platform(k, fx - 2.3, fy - 2.3, fx + 2.3, fy + 2.3, z_el - 3.0, rail_sides='nesw', legs=False,
                     gaps=[(fx + 1.1, fy - 2.3)], name='flue platform')
    ip.ladder(k, fx + 1.1, fy - 2.3, -math.pi / 2, s['deck'], z_el - 3.0, off=.2, reach=.3, standoff=False, name='flue platform ladder')
    count(k, 'stairs_ladders')
    # rooftop plant on the low wing: two turret fans, AC louvre unit, pipe run
    ip.roof_fan(k, -6.6, -4.6, s['deck'], .9)
    ip.roof_fan(k, -2.8, -4.6, s['deck'], .9)
    ip.louvre_box(k, -4.8, -8.6, s['deck'], 3.4, 2.0, 1.4)
    ip.pipe(k, [(-8.5, -2.0, s['deck'] + .6), (4.0, -2.0, s['deck'] + .6), (4.0, 2.4, s['deck'] + .6), (4.0, 2.4, s['deck'] + 1.4)], .14, 'paint.pipe')
    for x in (-7, -4, -1, 2):
        k.box((x, -2.0, s['deck'] + .22), (.3, .5, .44), 'paint.steel', 'frame')
    # caged ladder from the low roof up the hall's east face, rest platform at the belt course
    ip.ladder(k, hx1, 9.6, 0.0, s['deck'], hall - .8, off=.38, name='hall roof ladder')
    count(k, 'stairs_ladders')
    # wall pipe run on the front of the low wing, downpipes at corners
    ip.wall_pipe(k, [(-5.9, y0 - .35, 5.6), (x1 - .8, y0 - .35, 5.6), (x1 - .8, y0 - .35, .5)], .12, -math.pi / 2)
    for x, y, f in ((-6.0, y0, 's'), (x1 - .5, y0, 's'), (x1, y1 - .5, 'e'), (x0, y1 - .5, 'w')):
        ip.downpipe(k, x, y, 0.1, low - .4 if not (f == 'w') else hall - .5, YAW[f])


# ================================================================== heavyindustrial
HI = dict(x0=-10.57, x1=10.57, y0=-12.15, y1=12.15, eave=11.07, ridge=13.77, stack=(-10.3, -4.8), stack_y=(-5.26, 1.1),
          stack_w=5.0, s_hi=19.17, s_lo=15.6)


def heavyindustrial(k):
    s = HI
    x0, x1, y0, y1, eave = s['x0'], s['x1'], s['y0'], s['y1'], s['eave']
    floor = [(5.4, 5.94)]
    brick_block(k, x0, y0, x1, y1, 0, eave, pilaster=4.0, belt=floor, openings={
        's': [('window', -7.4, 1.8, 1.6, 2.4), ('window', -4.6, 1.8, 1.6, 2.4),
              ('window', 4.6, 1.8, 1.6, 2.4), ('window', 7.4, 1.8, 1.6, 2.4),
              ('window', -7.4, 7.0, 1.6, 2.4), ('window', -4.6, 7.0, 1.6, 2.4),
              ('window', 4.6, 7.0, 1.6, 2.4), ('window', 7.4, 7.0, 1.6, 2.4)],
        'w': [('window', 4.0, 7.0, 1.6, 2.4), ('window', 0.0, 7.0, 1.6, 2.4), ('window', -4.0, 7.0, 1.6, 2.4), ('window', 6.0, 1.8, 1.6, 2.4)],
        'n': [('rollup', 0.4, .0, 4.6, 4.8),
              ('window', -6.0, 7.0, 1.6, 2.4), ('window', 0.0, 7.0, 1.6, 2.4), ('window', 6.0, 7.0, 1.6, 2.4)],
        'e': [('window', -7.0, 1.8, 1.6, 2.4), ('window', -2.0, 1.8, 1.6, 2.4), ('window', 3.0, 1.8, 1.6, 2.4),
              ('window', -7.0, 7.0, 1.6, 2.4), ('window', -2.0, 7.0, 1.6, 2.4), ('window', 3.0, 7.0, 1.6, 2.4),
              ('door', 6.4, 6.0, 1.2, 2.3)]})
    # projecting centre entry pavilion: brick bay, stone quoins, pediment gable over the eaves line
    px = 3.1
    brick_block(k, -px, y0 - .6, px, y0 + 1.2, .0, eave + .9, sides='swe', pilaster=0, belt=floor, t=.5, cornice=True,
                openings={'s': [('door2', 0.0, .0, 2.4, 3.0), ('window', 0.0, 7.0, 3.6, 2.4)]})
    k.prism([(-px - .2, eave + .9), (px + .2, eave + .9), (0, eave + 3.1)], y0 - .78, y0 + 1.2, 'paint.roof', 'roof', axis='y')
    k.prism([(-px + .1, eave + .9), (px - .1, eave + .9), (0, eave + 2.85)], y0 - .6, y0 - .2, 'brick.second', 'skin', axis='y')
    k.box((0, y0 - .64, eave + 1.75), (.9, .1, .9), 'ferro', 'frame', ch=.25)
    for i in range(2):
        k.box((0, y0 - .9 - (1 - i) * .35, .1 + i * .15), (3.4 - i * .5, .7 + i * .0, .2 + i * .3), 'ferro', 'mass', ch=.03)
    # rear corner steel hoppers (the stock's silver drums), hooped and on a cast plinth
    for sx in (-1, 1):
        hc_ = (sx * (x1 - 2.1), y1 - .9)
        ip.lathe(k, [(2.15, 0), (2.15, .4), (0, .4)], 'ferro', 'mass', n=40, c=hc_)
        ip.vessel(k, hc_, 1.85, .4, 4.6, top='cone', bottom='flat', courses=3, mat='metal', head_mat='paint.roof', n=48)
        for zz in (1.2, 2.6, 4.0):
            ip.ring_band(k, hc_, 1.85, zz, zz + .1, 'paint.steel', proud=.05, n=48)
        count(k, 'tanks')
    # hipped standing-seam roof with a flat plant deck
    inset = 2.7
    k.prism(geo.rect_poly(x0 - .25, y0 - .25, x1 + .25, y1 + .25), eave - .05, s['ridge'], 'paint.roof', 'roof', top=inset, top_h=s['ridge'] - eave)
    for i in range(1, 22):
        x = x0 + (x1 - x0) * i / 22
        if x0 + inset < x < x1 - inset:
            for yy, sgn in ((y0, 1), (y1, -1)):
                k.beam((x, yy - .25 * sgn, eave), (x, yy + sgn * (inset - .2), s['ridge'] - .02), .05, 'paint.roof', 'detail', h=.05)
    k.box(((x0 + x1) / 2, (y0 + y1) / 2, s['ridge'] + .02), (x1 - x0 - 2 * inset + .2, y1 - y0 - 2 * inset + .2, .1), 'paint.trim', 'frame')
    ip.railing(k, [(x0 + inset, y0 + inset), (x1 - inset, y0 + inset), (x1 - inset, y1 - inset), (x0 + inset, y1 - inset)],
               s['ridge'] + .06, toe=False)
    ip.roof_fan(k, 2.0, -3.0, s['ridge'], 1.0)
    ip.roof_fan(k, 2.0, 3.6, s['ridge'], 1.0)
    ip.louvre_box(k, 6.4, .3, s['ridge'], 2.6, 3.4, 1.3)
    ip.pipe(k, [(-3.4, 6.5, s['ridge'] + .5), (6.0, 6.5, s['ridge'] + .5), (6.0, 2.2, s['ridge'] + .5)], .14, 'paint.pipe')
    # two brick ventilation stacks over the west bay: mono-pitch steel heads with louvres, steel flue outlets
    sx0, sx1 = s['stack']
    for yc in s['stack_y']:
        ya, yb = yc - s['stack_w'] / 2, yc + s['stack_w'] / 2
        brick_block(k, sx0, ya, sx1, yb, s['ridge'] - 1.2, s['s_lo'] - .4, sides='nesw', pilaster=0, cornice=False, plinth=0, t=.5,
                    openings={'w': [('vent', 0.0, 13.4, 3.0, 1.5)], 'n': [('vent', 0, 13.4, 2.4, 1.4)]})
        hi_x, lo_x = sx0 - .15, sx1 + .15
        # brick head following the mono-pitch, steel roof sheet and louvred west face on top
        k.prism([(sx0, s['s_lo'] - .4), (sx1, s['s_lo'] - .4), (sx1, s['s_lo']), (sx0, s['s_hi'] - .1)], ya, yb, 'brick.main', 'mass', axis='y')
        k.prism([(hi_x, s['s_hi'] - .15), (lo_x, s['s_lo'] - .05), (lo_x, s['s_lo'] + .12), (hi_x, s['s_hi'] + .02)], ya - .15, yb + .15,
                'paint.roof', 'roof', axis='y')
        k.box((sx0 - .06, yc, 16.6), (.14, 3.2, 1.8), 'paint.trim', 'frame')
        for j in range(8):
            k.box((sx0 - .16, yc, 15.85 + j * .2), (.08, 2.9, .09), 'grate', 'frame')
        sm = (-7.54, yc)
        zs = s['s_hi'] - (s['s_hi'] - s['s_lo']) * (sm[0] - hi_x) / (lo_x - hi_x)
        ip.lathe(k, [(.55, zs - .5), (.55, zs + 1.1), (.68, zs + 1.15), (.68, zs + 1.3), (.45, zs + 1.3), (.45, zs + .9)], 'paint.steel', 'frame', n=28, c=sm)
        ip.ring_band(k, sm, .55, zs + .55, zs + .7, 'paint.accent', proud=.02)
    count(k, 'stacks', 2)
    # caged ladder from the roof deck up the north stack, wall pipe from the stacks down the west face
    ip.hatch(k, -2.6, -6.4, s['ridge'], 1.0)          # plant deck reached from the stair tower inside
    ip.wall_pipe(k, [(x0 - .4, -8.6, eave - .6), (x0 - .4, -8.6, .5)], .16, math.pi)
    ip.wall_pipe(k, [(x0 - .4, 8.0, eave - .6), (x0 - .4, 8.0, 2.2), (x0 - .4, 10.6, 2.2)], .12, math.pi)
    # external steel stair to the east upper door, downpipes at the corners
    ip.stair(k, (x1 + .8, -1.4, 0.0), (x1 + .8, 5.6, 6.0), name='east fire stair')
    ip.rect_platform(k, x1 + .05, 5.6, x1 + 1.55, 7.2, 6.0, rail_sides='en', legs=True, name='east door landing')
    count(k, 'stairs_ladders', 1)
    for x, y, f in ((x0 + .4, y0, 's'), (x1 - .4, y0, 's'), (x0 + .4, y1, 'n'), (x1 - .4, y1, 'n')):
        ip.downpipe(k, x, y, .1, eave - .2, YAW[f])
    k.meta['smoke_m'] = [(-7.54, y, 16.97) for y in s['stack_y']]


# ================================================================== factorymd
FM = dict(x0=-9.68, x1=9.68, y0=-15.75, y1=15.75, ys=0.0, hall=16.1, a_lo=9.4, a_hi=12.6, bay=3.2)


def corr_wall(k, L, zb, ztop_fn, holes=(), cols=None, girt=2.2, sheet=1.07, mat='corr.main'):
    """Local wall frame: H-columns and girts first, then corrugated sheets hung over them
    (alternate sheets 14 mm proud, 70 mm laps), cut around openings; corner/top trims."""
    a, b = -L / 2, L / 2
    for x in cols or []:
        ip.hbeam(k, (x, .32, zb), (x, .32, ztop_fn(x) - .1), .3, .25)
        if a + .3 < x < b - .3:      # pale column cover strip: the stock's vertical structural rhythm
            for r in geo.subtract_rect((x - .16, zb, x + .16, ztop_fn(x) - .05), holes):
                k.slab(r[0], r[2], r[1], r[3], -.075, -.01, 'paint.main', 'frame', front=.02)
    for z in ip.frange(zb + 1.2, max(ztop_fn(a), ztop_fn(b)) - .4, girt):
        k.beam((a, .1, z), (b, .1, z), .1, 'paint.steel', 'frame', h=.18)
    n = max(1, math.ceil((b - a) / (sheet - .07)))
    for i in range(n):
        sa = a + i * (sheet - .07)
        sb = min(b, sa + sheet)
        zt = min(ztop_fn(sa), ztop_fn(sb))
        yf = -.014 * (i % 2)
        for r in geo.subtract_rect((sa, zb, sb, zt), holes):
            k.corr_sheet(r[0], r[2], r[1], r[3], yf, mat, removable=True)
    k.slab(a - .1, a + .12, zb, ztop_fn(a), -.12, .05, 'paint.trim', 'frame')
    k.slab(b - .12, b + .1, zb, ztop_fn(b), -.12, .05, 'paint.trim', 'frame')
    # sloped/flat top flashing follows the eave line
    k.prism([(a - .1, ztop_fn(a) - .05), (b + .1, ztop_fn(b) - .05), (b + .1, ztop_fn(b) + .3), (a - .1, ztop_fn(a) + .3)], -.16, .15,
            'paint.trim', 'frame', axis='y')


def factorymd(k):
    s = FM
    x0, x1, y0, y1, ys = s['x0'], s['x1'], s['y0'], s['y1'], s['ys']
    H, alo, ahi = s['hall'], s['a_lo'], s['a_hi']
    slope = lambda x: alo + (ahi - alo) * (x - x0) / (x1 - x0)  # noqa: E731
    # slab/plinth
    k.box((0, 0, .2), (x1 - x0 + .5, y1 - y0 + .5, .4), 'ferro', 'mass', ch=.1, top=.06, walk=True)
    zb = .4
    F_h = faces_of(x0, y0, x1, ys, 0)
    F_a = faces_of(x0, ys, x1, y1, 0)
    flat = lambda z: (lambda x: z)  # noqa: E731

    def bays(L):
        n = max(1, round(L / s['bay']))
        return [-L / 2 + L * i / n for i in range(n + 1)]
    # main hall: front, west and east faces + the hall's exposed rear face above the annex
    for side in 'swe':
        origin, yaw, L, _ = F_h[side]
        with k.at(origin, yaw=yaw):
            holes = []
            if side == 's':
                for xl in (-5.5, 5.5):
                    holes.append((xl - .9, H - 3.6, xl + .9, H - 2.4))
            doors = [(-1.6, 4.4, 4.8)] if side == 'e' else []
            holes += [(xl - w / 2 - .36, zb, xl + w / 2 + .36, zb + h + .1) for xl, w, h in doors]
            corr_wall(k, L, zb, flat(H), holes, cols=bays(L))
            with k.at((0, 0, zb)):
                for xl, w, h in doors:
                    ip.rollup_door(k, xl, w, h, .0, grade=zb)
                    count(k, 'doors')
                    wall_lamp(k, xl, h + 1.25, yaw)
            for hh in holes:
                k.slab(hh[0], hh[2], hh[1], hh[3], .02, .1, 'grate', 'frame')
                k.slab(hh[0] - .1, hh[2] + .1, hh[1] - .1, hh[3] + .1, -.08, .03, 'paint.trim', 'frame')
                count(k, 'vents')
    origin, yaw, L, _ = F_h['n']
    with k.at(origin, yaw=yaw):
        corr_wall(k, L, alo - .3, flat(H), (), cols=None)
    # annex: west, east (two roll-up doors) and north (personnel door, switch cabinet) faces under a mono-pitch roof
    for side in 'wen':
        origin, yaw, L, _ = F_a[side]
        with k.at(origin, yaw=yaw):
            if side == 'w':
                corr_wall(k, L, zb, flat(alo), (), cols=bays(L))
            elif side == 'e':
                doors = [(-1.0, 4.4, 4.8)]
                holes = [(xl - w / 2 - .36, zb, xl + w / 2 + .36, zb + h + .1) for xl, w, h in doors]
                corr_wall(k, L, zb, flat(ahi), holes, cols=bays(L))
                with k.at((0, 0, zb)):
                    for xl, w, h in doors:
                        ip.rollup_door(k, xl, w, h, .0, grade=zb)
                        count(k, 'doors')
                        wall_lamp(k, xl, h + 1.25, yaw)
            else:
                fn = lambda xl: slope(-xl)  # noqa: E731  local x runs -world x on the north face
                hole = [(-4.0 - .7, zb, -4.0 + .7, zb + 2.5)]
                corr_wall(k, L, zb, fn, hole, cols=bays(L))
                with k.at((0, 0, zb)):
                    ip.personnel_door(k, -4.0, 1.2, 2.4, .0)
                count(k, 'doors')
                wall_lamp(k, -4.0, zb + 3.3, yaw)
                k.box((1.0, -.2, 1.6), (1.2, .4, 1.6), 'paint.second', 'frame', ch=.04)
                ip.tube(k, [(1.0, -.2, 2.4), (1.0, -.2, slope(-1.0) - .3)], .06, 'metal', 'frame', sides=8)
    # roofs: hall flat roof with parapet cap, skylights, turret fans; annex mono-pitch standing seam
    k.box((0, (y0 + ys) / 2, H - .1), (x1 - x0, ys - y0, .2), 'paint.roof', 'roof', walk=True)
    for i in range(4):
        y = y0 + 3.2 + i * 4.6
        if y + .9 > ys - .2:
            continue                     # past the hall roof (round 6 check: it floated over the annex)
        for x in (-4.5, 4.5):
            k.box((x, y, H + .15), (3.6, 1.8, .3), 'paint.trim', 'frame', ch=.05)
            k.box((x, y, H + .32), (3.3, 1.5, .06), 'glass', 'glass', pane=True)
    ip.roof_fan(k, 0, y0 + 5.5, H, .8)
    ip.roof_fan(k, 0, y0 + 13.0, H, .8)
    k.prism([(x0 - .3, alo - .05), (x1 + .3, ahi - .05), (x1 + .3, ahi + .2), (x0 - .3, alo + .2)], ys, y1 + .3, 'paint.roof', 'roof', axis='y')
    for x in ip.frange(x0, x1, .6):
        k.beam((x, ys, slope(x) + .22), (x, y1 + .3, slope(x) + .22), .04, 'paint.roof', 'detail', h=.04)
    k.box((x1 + .25, (ys + y1) / 2, ahi - .2), (.3, y1 - ys + .6, .3), 'paint.second', 'frame')   # gutter
    # annex roof pipe runs (the stock's L-shaped roof lines), on stools
    for j, (yy, rr) in enumerate(((ys + 2.0, .2), (ys + 3.0, .15), (ys + 3.8, .12))):
        x_end = 3.5 + j * 1.6
        pts = [(x0 + .3, yy, slope(x0 + .3) + .65), (x_end, yy, slope(x_end) + .65), (x_end, y1 - 1.4, slope(x_end) + .65),
               (x_end, y1 - 1.4, slope(x_end) + 1.6)]
        pts = [(p[0], p[1], max(p[2], slope(p[0]) + .65)) for p in pts]
        ip.pipe(k, [(x0 - .5, yy, slope(x0) - 1.4)] + [(x0 - .5, yy, pts[0][2])] + pts[1:], rr, 'paint.pipe2' if j == 1 else 'paint.pipe')
        for x in ip.frange(x0 + 1.5, x_end - .5, 2.6):
            k.box((x, yy, slope(x) + .4), (.25, .3, .5), 'paint.steel', 'frame')
    count(k, 'roof_pipes', 3)
    # caged ladder to the hall roof on the west face, roof-edge railing at the ladder head
    ip.ladder(k, x0 - .02, y0 + 3.0, math.pi, zb, H, off=.2, name='hall roof ladder')
    ip.railing(k, [(x0 + .3, y0 + 1.0), (x0 + .3, y0 + 6.0)], H, gaps=[(x0 + .3, y0 + 3.0)])
    count(k, 'stairs_ladders')
    for x, y, f in ((x0, y0 + .5, 'w'), (x1, y0 + .5, 'e'), (x1, y1 - .5, 'e')):
        ip.downpipe(k, x, y, .4, (H if y < ys else ahi) - .3, YAW[f])


# ================================================================== registry
ASSETS = {
    'smallfueltank': dict(
        fn=smallfueltank, palette='intent_tank', cls='small',
        intent=('A refinery storage tank on a stand. A round vertical fuel tank (conical roof, conical hopper bottom) '
                'raised on eight braced H-columns over a round ferrocrete pad, with hazmat yellow-black chevrons over the '
                'lower half of the shell. A slim process column stands at the -X flank (the "vaguely chemical" part) with '
                'two railed platforms reached by caged ladders (grade to P1, P1 to P2, each landing through a rail opening) '
                'and an overhead line into the tank roof. A caged front ladder climbs the shell to a grated roof walkway '
                'that runs to the centre gauge nozzle through an opening in the roof-edge railing; bottom outlet and two '
                'shell nozzles carry gate valves and drop into buried lines through pad sleeves.'),
        counts=dict(tanks=2, stairs_ladders=3, legs_supports=8)),
    'pipetank': dict(
        fn=pipetank, palette='intent_tank', cls='large',
        intent=('An elevated refinery product tank. A big round cream-grey tank with a low domed roof sits on a battered '
                'ferrocrete pedestal (the pump room: double door, louvred vents, hazard-marked hatches) and cantilevers '
                'over it on twelve steel brackets. Yellow-black hazard band round the upper shell, wind girder at the '
                'eave, railed roof with breather vents and a gauge hatch. Explicit refinery access: a caged ladder from grade '
                'to a tank-bottom landing, a spiral stair up the -X shell to a railed rim landing and a roof walkway. The large flanged transfer pipeline leaves the '
                'pedestal on -X over T-sleepers to a gate valve and end flange, with two companion lines and a pump skid.'),
        counts=dict(tanks=1, doors=1, stairs_ladders=2, legs_supports=12)),
    'industrialplant': dict(
        fn=industrialplant, palette='intent_brick', cls='large',
        intent=('A brick process plant. A tall brick boiler hall (rear-left) beside a long, low brick wing that wraps '
                'its front and east side. A big insulated hot-gas flue (clad bands, bellows) rises from a burner unit on '
                'the low roof, is carried by a lattice support tower with a railed platform, and elbows into the hall\'s '
                'flank (replaces the stock "white tube" with a real process duct). Steel belt courses and recessed blind '
                'panels on the hall, louvred ventilator on its roof; turret fans, an AC louvre unit and a pipe run on '
                'the low roof; caged ladder up the hall; steel factory windows, two double doors, a wall pipe run.'),
        counts=dict(doors=4, windows=9, stairs_ladders=2)),
    'heavyindustrial': dict(
        fn=heavyindustrial, palette='intent_brick', cls='large',
        intent=('A two-storey brick heavy-industry hall with a projecting, pedimented centre entry bay. Hipped standing-seam '
                'roof with a railed flat plant deck '
                '(turret fans, louvre unit, pipe run). Two brick ventilation stacks over the west bay carry mono-pitch '
                'steel heads, louvres and steel flue outlets at the stock smoke points. Front: centred double door on '
                'steps, ten steel windows over two floors with a concrete string course; rear: roll-up loading door '
                'between two hooped steel hoppers at the rear corners (the stock silver drums); east: six windows and an '
                'external steel stair to an upper door; roof hatch to the plant deck, wall pipes, downpipes.'),
        counts=dict(doors=3, windows=22, stairs_ladders=1, stacks=2, tanks=2)),
    'factorymd': dict(
        fn=factorymd, palette='intent_factory', cls='large',
        intent=('A medium corrugated-steel factory built in construction order: concrete slab, H-column frame and '
                'girts, then vertical corrugated sheets with laps, trims and pale column cover strips (the stock vertical rhythm). '
                'A tall main hall (front half) with a flat roof, '
                'skylights and turret fans, and a lower lean-to annex at the rear under a mono-pitch standing-seam '
                'roof. Two roll-up truck doors (hazard jambs, bollards, lamps) on the east face, one under each volume, as stock; a personnel door '
                'and switch cabinet on its rear; three L-shaped pipe runs on the annex roof (the stock roof lines); '
                'high louvred vents on the front; caged ladder to the hall roof; gutters and downpipes.'),
        counts=dict(doors=3, stairs_ladders=1, roof_pipes=3)),
}


def register():
    import counts
    import bake_budget
    import json
    for name, a in ASSETS.items():
        counts.CONTRACTS[name] = counts.known(**{f: [f'{f} {i + 1}' for i in range(n)] for f, n in a['counts'].items() if f in counts.FIELDS})
        counts.NOTES[name] = 'intent pass: ' + a['intent']
        bake_budget.BUDGETS[name] = BUDGET[a['cls']]
        try:      # same convention as analyze.origin_offset (no PIL inside Blender)
            bb = json.loads((counts.STOCK / f'{name}.json').read_text())['bounds']
            counts.ORIGINS[name] = (round((bb['min'][0] + bb['max'][0]) / 8, 6), round(-(bb['min'][2] + bb['max'][2]) / 8, 6),
                                    round(bb['min'][1] / 4, 6))
        except OSError:
            counts.ORIGINS[name] = (0.0, 0.0, 0.0)


def build(name, condition='pristine', seed=17, raw=False):
    import counts
    a = ASSETS[name]
    k = Kit(name, a['palette'], seed)
    k.cond = condition
    k.meta['sockets'] = []
    k.emblem = a.get('emblem')
    k.wall_family = a.get('wall_family')   # (family, variant) of the approved wall code for gates without a House prefix
    k.name = a.get('base', name)         # generic variants reuse the approved base builder and its stock tables
    a['fn'](k)
    k.name = name
    k.pieces = [p for p in k.pieces if p['v']]
    if not raw:                          # owner 2026-10-24: buildings scaled to sit within the stock outline (fit.py)
        import fit
        fit.apply(k, name)
    uid = 0
    for pc in k.pieces:
        if pc.get('removable') or pc.get('pane'):
            pc['uid'] = uid
            uid += 1
    pts = [v for pc in k.pieces for v in pc['v']]
    lo = [min(v[i] for v in pts) for i in range(3)]
    hi = [max(v[i] for v in pts) for i in range(3)]
    k.meta.update(name=name, stock=name, condition=condition, palette=a['palette'], size=tuple(hi[i] - lo[i] for i in range(3)),
                  draft=True, original_geometry=True, intent=a['intent'], atlas_res=4096 if a['cls'] == 'large' else 2048, construction_order=True, meshopt_lods=True,
                  stock_origin_offset_m=counts.ORIGINS.get(name, counts.ORIGINS.get(a.get('base', name), (0, 0, 0))), bounds_m=(lo, hi))
    return k


# ================================================================== loose validation
def _sample_tris(tris, grid, box, N):
    import numpy as np
    x0, x1, y0, y1 = box
    for a, b, c in tris:
        e = max(np.linalg.norm(b - a), np.linalg.norm(c - a), np.linalg.norm(c - b))
        n = int(min(40, max(2, e / .25)))
        u, v = np.meshgrid(np.linspace(0, 1, n + 1), np.linspace(0, 1, n + 1))
        m = (u + v) <= 1
        u, v = u[m], v[m]
        P = a[None] * (1 - u - v)[:, None] + b[None] * u[:, None] + c[None] * v[:, None]
        ix = np.clip(((P[:, 0] - x0) / (x1 - x0) * N).astype(int), 0, N - 1)
        iy = np.clip(((P[:, 1] - y0) / (y1 - y0) * N).astype(int), 0, N - 1)
        ok = (P[:, 0] >= x0) & (P[:, 0] <= x1) & (P[:, 1] >= y0) & (P[:, 1] <= y1)
        np.maximum.at(grid, (iy[ok], ix[ok]), P[ok, 2])


def height_grid_stock(name, box, N=10):
    import numpy as np
    import analyze
    V = analyze.load_stock(name)['V']
    g = np.zeros((N, N))
    _sample_tris(V, g, box, N)
    return g


def height_grid_kit(k, box, N=10):
    import numpy as np
    g = np.zeros((N, N))
    tris = []
    for pc in k.pieces:
        if pc['role'] == 'detail':
            continue
        v = np.array(pc['v'], float)
        for f in pc['f']:
            for i in range(1, len(f) - 1):
                tris.append((v[f[0]], v[f[i]], v[f[i + 1]]))
    _sample_tris(tris, g, box, N)
    return g


def check(name):
    import numpy as np
    import analyze
    k = build(name)
    st = analyze.load_stock(name)['V'].reshape(-1, 3)
    slo, shi = st.min(0), st.max(0)
    lo, hi = np.array(k.meta['bounds_m'][0]), np.array(k.meta['bounds_m'][1])
    ssz, rsz = shi - slo, hi - lo
    res = dict(asset=name, stock_size=[round(x, 2) for x in ssz], remake_size=[round(x, 2) for x in rsz], fails=[])
    for i, ax in enumerate('xy'):
        d = abs(rsz[i] - ssz[i]) / ssz[i]
        res[f'footprint_{ax}_dev'] = round(d, 3)
        if d > ASSETS[name].get('tol', {}).get('footprint', TOL['footprint']):
            res['fails'].append(f'footprint {ax} off by {d:.0%}')
    d = abs(rsz[2] - ssz[2]) / ssz[2]
    res['height_dev'] = round(d, 3)
    if d > ASSETS[name].get('tol', {}).get('height', TOL['height']):
        res['fails'].append(f'height off by {d:.0%}')
    box = (min(slo[0], lo[0]), max(shi[0], hi[0]), min(slo[1], lo[1]), max(shi[1], hi[1]))
    gs, gk = height_grid_stock(name, box), height_grid_kit(k, box)
    H = ssz[2]
    mae = float(np.abs(gs - gk).mean() / H)
    res['grid_mae_h'] = round(mae, 3)
    if mae > ASSETS[name].get('tol', {}).get('grid_mae', TOL['grid_mae']):
        res['fails'].append(f'mass placement: mean height-grid error {mae:.0%} of H')

    def centroid(g):
        w = np.clip(g / H, 0, None) ** 2
        yy, xx = np.mgrid[0:g.shape[0], 0:g.shape[1]]
        return np.array([(xx * w).sum() / w.sum(), (yy * w).sum() / w.sum()]) / g.shape[0]
    off = float(np.linalg.norm(centroid(gs) - centroid(gk)))
    res['mass_centroid_off'] = round(off, 3)
    if off > ASSETS[name].get('tol', {}).get('centroid', TOL['centroid']):
        res['fails'].append(f'mass centroid off by {off:.2f} of the lot')
    got = k.meta.get('counts', {})
    res['counts'] = {}
    for key, want in ASSETS[name]['counts'].items():
        res['counts'][key] = (got.get(key, 0), want)
        if got.get(key, 0) != want:
            res['fails'].append(f'count {key}: {got.get(key, 0)} != {want}')
    res['pieces'] = len(k.pieces)
    res['high_tris_approx'] = sum(len(f) - 2 for pc in k.pieces for f in pc['f'])
    geo.GAME_MESH = True
    try:
        kg = build(name)
        res['game_tris_pre_bevel'] = sum(len(f) - 2 for pc in kg.pieces for f in pc['f'])
    finally:
        geo.GAME_MESH = False
    import clearance
    res['access'] = clearance.check(kg)
    res['fails'] += ['access: ' + x for x in res['access']]
    res['access_paths'] = len(kg.meta.get('access', []))
    res['platforms'] = len(kg.meta.get('platforms', []))
    import lint
    res['lint'] = lint.check(kg)
    import footprint                     # owner 2026-10-24: hard footprint rule (strict for walls/gates/fences/posts)
    fp = footprint.check(name)
    res['footprint'] = fp['metrics']
    res['footprint_kind'] = fp['kind']
    res['fails'] += fp['fails']
    res['budget'] = BUDGET[ASSETS[name]['cls']]
    res['ok'] = not res['fails']
    return res


import intent_set2  # noqa: E402  industrial set 2 (large tanks, water tower, modular pipeline)
ASSETS.update(intent_set2.SPECS)
import intent_set3  # noqa: E402  industrial round 2: factories, plants, services
ASSETS.update(intent_set3.SPECS)
import intent_set4  # noqa: E402  industrial round 2: depots, warehouses, yard structures
ASSETS.update(intent_set4.SPECS)
import intent_power  # noqa: E402  electrical/power redo (owner: "too blocky") supersedes set 3
ASSETS.update(intent_power.SPECS)
import intent_military  # noqa: E402  military base family, batch 1
ASSETS.update(intent_military.SPECS)
import intent_military2  # noqa: E402  military batch 8: barracks, repair building, diagonal walls, field props
ASSETS.update(intent_military2.SPECS)
import intent_civ  # noqa: E402  civilian family, group 1
ASSETS.update(intent_civ.SPECS)
import intent_city  # noqa: E402  civilian family, group 2: city blocks
ASSETS.update(intent_city.SPECS)
import intent_civic  # noqa: E402  civilian family, groups 3 and 4: civic, palace, government, bridge, promenades, flags
ASSETS.update(intent_civic.SPECS)
import intent_generic  # noqa: E402  walls/gates completion pass: generic and cut-content variants (stock from MC2X-CVE-G)
ASSETS.update(intent_generic.SPECS)
register()
