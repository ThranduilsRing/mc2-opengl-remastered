"""Intent pass, industrial set 2: large tanks, the water tower and the modular pipeline family.

Same method and family look as intent.py (palettes intent_tank / intent_pipe / intent_rust, safety-yellow
rails, red valve bodies, galvanised ladders). Access follows the owner's path rules and is verified by
clearance.py. Builders register into intent.ASSETS (imported at the end of intent.py).
"""
import math

import ind_parts as ip

# ------------------------------------------------------------------ shared: modular pipeline
PIPE_R, PIPE_Z, ARM = 1.38, 2.03, 15.975      # stock pipe radius, centre height, junction-to-module-end
HOUSE_H = 3.6


def count(k, key, n=1):
    c = k.meta.setdefault('counts', {})
    c[key] = c.get(key, 0) + n


def pipe_module(k, p0, p1, supports=(), joint0=True, joint1=True):
    """Large-bore olive pipeline from p0 to p1 (plan points) at the stock centre height: yellow ID bands,
    bolted flange collars at module joints, saddles with hold-down straps on ferrocrete sleepers."""
    a, b = (p0[0], p0[1], PIPE_Z), (p1[0], p1[1], PIPE_Z)
    d = ip.norm(ip.sub(b, a))
    L = ip.length(ip.sub(b, a))
    ip.tube(k, [a, b], PIPE_R, 'paint.pipe', 'mass', sides=40)
    for f, on in ((0.0, joint0), (1.0, joint1)):
        if on:   # module joint: collar sits fully inside this module so neighbours meet face to face
            q = ip.add(a, ip.mul(d, L * f + (.3 if f == 0 else -.3)))
            ip.tube(k, [ip.sub(q, ip.mul(d, .3)), ip.add(q, ip.mul(d, .3))], PIPE_R + .12, 'paint.pipe', 'frame', sides=40)
            ip.bolt_ring_axis(k, ip.add(q, ip.mul(d, .32 if f == 0 else -.32)), d, PIPE_R + .07)
            # anchor portal at the joint (the stock's square end plates): footing, two H-posts, head beam
            side = (-d[1], d[0], 0)
            g = ip.add((q[0], q[1], 0), ip.mul(d, .3 if f == 0 else -.3))
            k.add(*ip.box_along(ip.add((g[0], g[1], .2), ip.mul(side, -2.05)), ip.add((g[0], g[1], .2), ip.mul(side, 2.05)), d, (0, 0, 1), .9, .4),
                  'ferro', 'mass', walk=True)
            for sgn in (-1, 1):
                b0 = ip.add((g[0], g[1], .4), ip.mul(side, sgn * 1.75))
                ip.hbeam(k, b0, (b0[0], b0[1], 3.25), .3, .26)
            ip.hbeam(k, ip.add((g[0], g[1], 3.28), ip.mul(side, -1.9)), ip.add((g[0], g[1], 3.28), ip.mul(side, 1.9)), .26, .3)
            k.add(*ip.box_along(ip.add((g[0], g[1], .55), ip.mul(side, -1.6)), ip.add((g[0], g[1], .55), ip.mul(side, 1.6)), d, (0, 0, 1), .3, .3),
                  'paint.steel', 'frame')
    for t in (.18, .5, .82) if L > 12 else (.5,):
        q = ip.add(a, ip.mul(d, L * t))
        ip.tube(k, [ip.sub(q, ip.mul(d, .35)), ip.add(q, ip.mul(d, .35))], PIPE_R + .015, 'paint.accent', 'skin', sides=40)
    for s in supports:
        q = ip.add(a, ip.mul(d, s))
        side = (-d[1], d[0], 0)
        k.add(*ip.box_along(ip.add((q[0], q[1], .3), ip.mul(side, -1.25)), ip.add((q[0], q[1], .3), ip.mul(side, 1.25)), d, (0, 0, 1), 1.1, .6),
              'ferro', 'mass', walk=True)
        hs = PIPE_Z - PIPE_R + .25 - .6
        k.add(*ip.box_along(ip.add((q[0], q[1], .6 + hs / 2), ip.mul(side, -1.0)), ip.add((q[0], q[1], .6 + hs / 2), ip.mul(side, 1.0)), d, (0, 0, 1), .5, hs),
              'paint.steel', 'frame')
        strap = [ip.add((q[0], q[1], PIPE_Z), ip.add(ip.mul(side, (PIPE_R + .03) * math.cos(t)), (0, 0, (PIPE_R + .03) * math.sin(t))))
                 for t in [i * math.pi / 12 for i in range(13)]]
        ip.tube(k, strap, .035, 'metal', 'frame', sides=6)


def valve_house(k, c, arms, ladder_side, ladder_off=0.0):
    """Battered ferrocrete valve chamber where pipeline arms meet: pipes enter through flanged wall sleeves,
    gate-valve stems rise through a railed, cantilevered grating deck to handwheels; louvred and
    hazard-marked access panels on the free faces; caged ladder on `ladder_side` (yaw) to the deck."""
    hb, ht, H = 4.05, 2.55, HOUSE_H
    tilt = math.atan((hb - ht) / H)
    k.prism([(c[0] - hb, c[1] - hb), (c[0] + hb, c[1] - hb), (c[0] + hb, c[1] + hb), (c[0] - hb, c[1] + hb)], 0, H,
            'ferro', 'mass', top=hb - ht, top_h=H)
    k.box((c[0], c[1], H + .1), (2 * ht + .1, 2 * ht + .1, .2), 'paint.second', 'mass', ch=.05)
    deck = 3.95
    zd = H + .3
    gap = ip.polar(c, deck - .05, ladder_side, 0)[:2]
    gx, gy = math.cos(ladder_side), math.sin(ladder_side)
    gap = (c[0] + gx * (deck - .05) - gy * ladder_off, c[1] + gy * (deck - .05) + gx * ladder_off)
    ip.rect_platform(k, c[0] - deck, c[1] - deck, c[0] + deck, c[1] + deck, zd, legs=False, gaps=[gap], name='valve deck')
    for i in range(4):     # deck carried on knee braces off the chamber
        a = i * math.pi / 2 + math.pi / 4
        k.beam(ip.polar(c, ht * 1.3, a, H), ip.polar(c, deck * 1.33, a, zd - .2), .12, ip.STEEL, 'frame', h=.12)
        k.beam(ip.polar(c, (hb - .5) * 1.35, a, 2.0), ip.polar(c, deck * 1.3, a, zd - .25), .1, ip.STEEL, 'frame', h=.1)
    for ang in arms:
        dx, dy = math.cos(ang), math.sin(ang)
        wall = hb - (hb - ht) * PIPE_Z / H
        ip.flange(k, (c[0] + dx * (wall + .05), c[1] + dy * (wall + .05), PIPE_Z), (dx, dy, 0), PIPE_R + .35, t=.22)
        # gate valve inside the chamber, stem and handwheel above the deck
        sx, sy = c[0] + dx * 1.2, c[1] + dy * 1.2
        k.cyl((sx, sy, H + .5), .14, 1.1, 'metal', 'frame', sides=10)
        k.cyl((sx, sy, H + .05), .35, .25, 'paint.valve', 'frame', sides=16)
        ring = [(sx + .55 * math.cos(t * math.tau / 16), sy + .55 * math.sin(t * math.tau / 16), zd + .85) for t in range(16)]
        ip.tube(k, ring, .03, 'paint.valve', 'frame', sides=6, closed=True)
        for t in range(3):
            ip.tube(k, [(sx, sy, zd + .85), (sx + .55 * math.cos(t * math.tau / 3), sy + .55 * math.sin(t * math.tau / 3), zd + .85)], .02,
                    'paint.valve', 'frame', sides=5)
        k.cyl((sx, sy, zd + .45), .1, .8, 'metal', 'frame', sides=10)
        count(k, 'valves')
    free = [a for a in (0, math.pi / 2, math.pi, -math.pi / 2) if all(abs(math.remainder(a - b, math.tau)) > .1 for b in arms)]
    for a in free:
        with k.at((c[0] + math.cos(a) * hb, c[1] + math.sin(a) * hb, 0), yaw=a + math.pi / 2, pitch=-tilt):
            if abs(math.remainder(a - ladder_side, math.tau)) < .1:
                k.box((0, -.08, 1.6), (2.2, .16, 1.4), 'hazard', 'skin')
                k.box((0, -.14, 1.6), (1.6, .1, 1.0), 'paint.second', 'frame', ch=.03)
            else:
                k.box((0, -.08, 1.7), (2.4, .16, 1.3), 'paint.trim', 'frame', ch=.04)
                for j in range(5):
                    k.box((0, -.17, 1.2 + j * .22), (2.1, .06, .08), 'grate', 'frame')
                k.box((-1.6, -.1, .9), (.7, .12, .5), 'paint.valve', 'frame', ch=.03)
    lx, ly = math.cos(ladder_side), math.sin(ladder_side)
    base = (c[0] + lx * (hb + .05) - ly * ladder_off, c[1] + ly * (hb + .05) + lx * ladder_off)
    ip.ladder(k, base[0], base[1], ladder_side, 0.0, zd, off=.18, reach=.4, standoff=False, cage=True, name='valve deck ladder')
    count(k, 'stairs_ladders')


def bolt_ring_axis(k, p, d, r, n=16):
    import geo
    if geo.GAME_MESH:
        return
    d = ip.norm(d)
    up = (0, 0, 1) if abs(d[2]) < .9 else (1, 0, 0)
    u = ip.norm(ip.cross(d, up))
    v = ip.cross(d, u)
    for i in range(n):
        a = i * math.tau / n
        k.box(ip.add(p, ip.add(ip.mul(u, r * math.cos(a)), ip.mul(v, r * math.sin(a)))), (.06, .06, .06), 'metal', 'detail')


ip.bolt_ring_axis = bolt_ring_axis


def straight(length, supports):
    def fn(k):
        pipe_module(k, (0, -length / 2), (0, length / 2), supports=supports)
        count(k, 'pipes')
    return fn


def junction(block, arms, ladder_side, ladder_off=0.0):
    def fn(k):
        for ang in arms:
            dx, dy = math.cos(ang), math.sin(ang)
            wall = 4.05 - (4.05 - 2.55) * PIPE_Z / HOUSE_H
            p0 = (block[0] + dx * (wall - .3), block[1] + dy * (wall - .3))
            p1 = (block[0] + dx * ARM, block[1] + dy * ARM)
            pipe_module(k, p0, p1, supports=(ARM - wall - 4.5,), joint0=False)
            count(k, 'pipes')
        valve_house(k, block, arms, ladder_side, ladder_off)
        count(k, 'valve_houses')
    return fn


def fuelpipe(k):
    """Small-bore fuel line (stock fuelpipe): flanged spool on saddle stands, ID bands, actuated ball valve."""
    z, r = 1.25, .82
    ip.pipe(k, [(0, -10.25, z), (0, 10.25, z)], r, 'paint.pipe', flanges=[((0, -3.4, z), (0, 1, 0)), ((0, 3.4, z), (0, 1, 0))])
    for y in (-8.2, -1.6, 1.6, 8.6):
        ip.pipe_support(k, 0, y, z - r, r, w=.8)
    for y in (-6.0, 0.0):
        ip.tube(k, [(0, y - .25, z), (0, y + .25, z)], r + .015, 'paint.accent', 'skin', sides=32)
    # actuated ball valve: flanged body, gearbox and actuator on the +X side, lock-out handle
    ip.tube(k, [(0, 5.6, z), (0, 7.0, z)], r * 1.25, 'paint.valve', 'frame', sides=32)
    for y in (5.55, 7.05):
        ip.flange(k, (0, y, z), (0, 1, 0), r * 1.45, t=.12, mat='paint.valve')
    k.box((.95, 6.3, z + .2), (.55, .8, .9), 'paint.second', 'frame', ch=.05, top=.04)
    k.box((.95, 6.3, z + .8), (.4, .5, .35), 'paint.accent', 'frame', ch=.04)
    ip.tube(k, [(.75, 6.3, z + .2), (r + .02, 6.3, z + .2)], .12, 'metal', 'frame', sides=10)
    ip.tube(k, [(0, 6.3, z + r * 1.25), (0, 6.3, z + r * 1.25 + .3), (-.7, 6.3, z + r * 1.25 + .3)], .04, 'paint.valve', 'frame', sides=6)
    count(k, 'pipes')
    count(k, 'valves')


# ------------------------------------------------------------------ largefueltank
LFT = dict(c=(2.3, 0.0), r=11.0, foot=6.2, z_top=20.18)


def largefueltank(k):
    s = LFT
    c, r = s['c'], s['r']
    zf = s['foot']
    # battered ferrocrete ring foundation (the stock's faceted foot) with a steel bearing ring
    ip.lathe(k, [(r + 1.1, 0), (r + 1.1, .5), (r + .85, .7), (r - .3, zf - .3), (r - .3, zf), (0, zf)], 'ferro', 'mass', n=96, c=c, walk=True)
    ip.ring_band(k, c, r - .3, zf - .3, zf, 'paint.steel', proud=.4, n=96)
    for a, kind in ((-math.pi / 2, 'door'), (math.radians(-30), 'vent'), (math.radians(30), 'hatch'), (math.pi / 2, 'vent'),
                    (math.radians(250), 'hatch')):
        q = ip.polar(c, r + .95, a, 0)
        with k.at((q[0], q[1], 0), yaw=a + math.pi / 2):
            k.box((0, .6, 2.1), (2.6, 1.2, 4.2), 'ferro', 'mass', ch=.12, top=.1)
            if kind == 'door':
                ip.personnel_door(k, 0, 1.4, 2.4, -.3, leaves=2)
                count(k, 'doors')
            elif kind == 'vent':
                k.slab(-.95, .95, 2.2, 3.7, -.24, .05, 'paint.trim', 'frame', front=.03)
                for j in range(8):
                    k.slab(-.85, .85, 2.3 + j * .17, 2.38 + j * .17, -.3, -.1, 'grate', 'frame')
            else:
                k.slab(-.9, .9, .9, 2.7, -.26, .05, 'hazard', 'skin', front=.03)
                k.slab(-.7, .7, 1.1, 2.5, -.3, -.2, 'paint.valve', 'frame', front=.02)
        if kind == 'door':
            ip.lamp(k, q[0], q[1] - .35, 3.1, -math.pi / 2, k.meta['sockets'])
    ip.vessel(k, c, r, zf, s['z_top'], bottom='flat', courses=6, mat='paint.main', head_mat='paint.roof', n=96,
              bands=[(zf + 1.6, zf + 4.6, 'hazard.wrap')],
              top_prof=[(r, s['z_top']), (r - 1.4, s['z_top'] + 1.1), (r * .55, s['z_top'] + 2.2), (0, s['z_top'] + 2.6)])
    count(k, 'tanks')
    for z in (zf + 1.5, zf + 4.62):
        ip.ring_band(k, c, r, z, z + .12, 'paint.trim', proud=.035, n=96)
    ip.ring_band(k, c, r, s['z_top'] - .35, s['z_top'] - .05, 'paint.second', proud=.16, n=96)
    ip.ring_band(k, c, r, zf + 7.0, zf + 7.25, 'paint.second', proud=.15, n=96)     # mid-height stiffening girder
    for i in range(48):
        a = i * math.tau / 48
        k.beam(ip.polar(c, r, a, zf + 6.6), ip.polar(c, r + .13, a, zf + 6.98), .05, 'paint.second', 'frame', h=.05)
    roofz = lambda d: s['z_top'] + 2.6 * (1 - (d / r) ** 1.7)  # noqa: E731
    # roof: radial hazard hatch at the crown (the stock's yellow star), vents, railed edge, walkway
    cr_ = 4.2      # crown cap: a low cone painted in radial yellow-black sectors (the stock star), dark rim
    ip.lathe(k, [(cr_ + .25, roofz(cr_ + .25) - .05), (cr_ + .25, roofz(cr_) + .12), (cr_, roofz(cr_) + .12)], 'paint.trim', 'frame', n=96, c=c, cap=False)
    ip.lathe(k, [(cr_, roofz(cr_) + .12), (1.2, roofz(0) + .35), (0, roofz(0) + .42)], 'hazard.wrap', 'frame', n=96, c=c, cap=False,
             uv_scale=(24 / 12, 0.0))
    k.cyl((c[0], c[1], roofz(0) + .5), .6, .2, 'paint.second', 'frame', sides=24, top=.05)
    for a in (30, 150, 270):
        p = ip.polar(c, 6.0, math.radians(a), roofz(6.0))
        ip.mushroom_vent(k, p[0], p[1], p[2] - .05, .45)
    a_top = math.radians(140)
    ip.railing(k, ip.circle_pts(c, r - .25, 0, math.tau, 72), roofz(r - .25) + .02, closed=True,
               gaps=[ip.polar(c, r - .25, a_top - math.radians(4), 0)[:2]])
    sd = (-math.sin(a_top - .07), math.cos(a_top - .07), 0)
    aw = a_top - .07
    for d0, d1 in ((r - .3, r * .7), (r * .7, r * .4), (r * .4, 4.4)):
        pa, pb = ip.polar(c, d0, aw, roofz(d0) + .12), ip.polar(c, d1, aw, roofz(d1) + .12)
        k.add(*ip.box_along(pa, pb, sd, (0, 0, 1), .9, .06), 'grate', 'frame', walk=True)
    # access: caged ladder from grade to a foundation-top landing, spiral stair to a rim landing
    zl = zf + .05
    a_lo = math.radians(205)
    ip.ring_platform(k, c, r + .05, r + 1.15, zl, math.radians(200), math.radians(224), brackets=3,
                     gaps=[ip.polar(c, r + 1.1, math.radians(215), 0)[:2], ip.polar(c, r + .85, math.radians(200), 0)[:2]], name='foundation landing')
    lad = ip.polar(c, r + 1.15, math.radians(215), 0)
    ip.ladder(k, lad[0], lad[1], math.radians(215), 0.0, zl, off=.18, reach=.3, standoff=False, name='grade ladder')
    zt = s['z_top'] + .12
    ip.ring_platform(k, c, r - .15, r + 1.15, zt, a_top - math.radians(12), a_top + .02, brackets=3,
                     gaps=[ip.polar(c, r + .55, a_top, 0)[:2]], name='rim landing')
    a_h = math.radians(203)
    ip.helix_stair(k, c, r + .16, zl + .12, zt, a_h, turn=-(a_h - a_top) / math.tau)
    count(k, 'stairs_ladders', 2)
    # base fittings: shell manholes, product line to -X through a valve onto the pipeline stub
    for a in (math.radians(-60), math.radians(60)):
        with k.at(ip.polar(c, r, a, 0), yaw=a + math.pi / 2):
            ip.tube(k, [(0, .05, zf + .8), (0, -.4, zf + .8)], .45, 'paint.main', 'frame', sides=24)
            ip.flange(k, (0, -.41, zf + .8), (0, -1, 0), .6, t=.1)
    x_in = c[0] - (r + .5) + .1
    ip.pipe(k, [(c[0] - r + .3, 0, zf + .6), (x_in - .6, 0, zf + .6), (x_in - 1.6, 0, PIPE_Z), (-13.2, 0, PIPE_Z)], .5, 'paint.pipe', bend=1.0)
    ip.valve(k, (-11.3, 0, PIPE_Z), (1, 0, 0), .5)
    for x in (-12.4, x_in - 2.6):
        ip.pipe_support(k, x, 0, PIPE_Z - .5, .5, w=.9)
    k.meta['hero_access'] = ip.polar(c, r, a_top, zt)


# ------------------------------------------------------------------ liquidstorage
LQS = dict(c=(0.0, -.3), r=10.9, z_top=12.3)


def liquidstorage(k):
    s = LQS
    c, r = s['c'], s['r']
    ip.lathe(k, [(r + .55, 0), (r + .55, .38), (0, .38)], 'ferro', 'mass', n=96, c=c, walk=True)
    ip.vessel(k, c, r, .38, s['z_top'], bottom='flat', courses=3, mat='paint.main', head_mat='paint.roof', n=96,
              top_prof=[(r, s['z_top']), (r - 1.2, s['z_top'] + .75), (r * .5, s['z_top'] + 1.6), (0, s['z_top'] + 1.9)])
    count(k, 'tanks')
    # vertical stiffener ribs: the stock's panel rhythm, as real T-section stiffeners
    for i in range(32):
        a = (i + .5) * math.tau / 32
        if abs(math.remainder(a + math.pi / 2, math.tau)) < .2:
            continue
        p0, p1 = ip.polar(c, r + .02, a, .6), ip.polar(c, r + .02, a, s['z_top'] - .2)
        k.beam(ip.polar(c, r + .1, a, .6), ip.polar(c, r + .1, a, s['z_top'] - .2), .14, 'paint.second', 'frame', h=.2)
    for z in (4.3, 8.3):
        ip.ring_band(k, c, r, z, z + .2, 'paint.second', proud=.22, n=96)
    ip.ring_band(k, c, r, s['z_top'] - .3, s['z_top'], 'paint.trim', proud=.25, n=96)
    roofz = lambda d: s['z_top'] + 1.9 * (1 - (d / r) ** 1.7)  # noqa: E731
    # front lift/stair tower: enclosed shaft with a glazed strip, door at grade, roof service hut
    ty = c[1] - r - .45
    k.box((0, ty, (s['z_top'] + 1.0) / 2), (2.6, 2.6, s['z_top'] + 1.0), 'paint.main', 'mass', ch=.15, top=.05)
    k.slab(-.35, .35, 2.8, s['z_top'] - .4, ty - 1.32, ty - 1.24, 'glass', 'glass', pane=True)
    k.slab(-.5, .5, 2.6, s['z_top'] - .2, ty - 1.36, ty - 1.28, 'paint.trim', 'frame')
    with k.at((0, ty - 1.3, 0)):
        ip.personnel_door(k, 0, 1.2, 2.3, -.2)   # leaf proud of the shaft face, not buried in it
    count(k, 'doors')
    ip.lamp(k, 0, ty - 1.32, 3.2, -math.pi / 2, k.meta['sockets'])
    hut_y = c[1] - r + 1.6
    k.box((0, hut_y, roofz(r - 1.6) + 1.3), (3.0, 3.6, 2.6), 'paint.main', 'mass', ch=.1, top=.08)
    k.box((0, hut_y, roofz(r - 1.6) + 2.66), (3.3, 3.9, .14), 'paint.trim', 'frame', ch=.05)
    k.box((0, ty, s['z_top'] + 1.05), (2.8, 2.8, .12), 'paint.trim', 'frame', ch=.05)
    with k.at((0, hut_y + 1.8, roofz(r - 1.6)), yaw=math.pi):
        ip.personnel_door(k, 0, 1.0, 2.1, -.2, canopy=False, apron=False)
    count(k, 'doors')
    k.slab(1.55, 1.62, roofz(r - 1.6) + .8, roofz(r - 1.6) + 1.6, hut_y - 1.0, hut_y + .2, 'paint.valve', 'frame')
    # roof walkway from the hut door to the central dome hatch, railed both sides, with the
    # product lines running beside it on stools into the hatch collar (the stock roof plant strip)
    y0w, y1w = c[1] - r + 3.7, c[1] - 1.3     # starts clear of the proud hut door frame
    pts = [(0, y0w + (y1w - y0w) * i / 6, roofz(abs(c[1] - (y0w + (y1w - y0w) * i / 6)))) for i in range(7)]
    ip.walkway(k, pts, w=1.1)
    for x_, rr in ((1.05, .16), (1.45, .12)):
        line = [(x_, q[1], q[2] + .45) for q in pts[:-1]] + [(x_, y1w + .2, roofz(1.3) + .45), (.85, c[1] - .9, roofz(.9) + .45)]
        ip.pipe(k, line[:-1] + [(min(x_, 1.15), c[1] - .9, roofz(.9) + .45)], rr, 'paint.pipe2' if x_ > 1.2 else 'paint.pipe', bend=.4,
                end_flanges=False)
        for q in pts[1:-1:2]:
            k.box((x_, q[1], q[2] + .2), (.2, .2, .4 - rr), 'paint.steel', 'frame')
    k.cyl((c[0], c[1], roofz(0) + .25), 1.2, .5, 'paint.second', 'frame', sides=28, top=.1)
    ip.hatch(k, c[0], c[1], roofz(0) + .45, 1.0)
    for a in (20, 160, 230, 310):
        p = ip.polar(c, 6.5, math.radians(a), roofz(6.5))
        ip.mushroom_vent(k, p[0], p[1], p[2] - .05, .4)
    ip.railing(k, ip.circle_pts(c, r - .3, math.radians(-80), math.radians(260), 66), roofz(r - .3) + .02)
    # red service panels and inlet/outlet nozzles at the base
    for a in (math.radians(-35), math.radians(-145), math.radians(90)):
        with k.at(ip.polar(c, r, a, 0), yaw=a + math.pi / 2):
            k.box((0, -.12, 1.6), (1.4, .22, 1.8), 'paint.valve', 'frame', ch=.04)
            k.box((0, -.25, 1.6), (1.0, .06, .5), 'grate', 'frame')
    for a in (math.radians(-65), math.radians(-115)):
        n0, n1 = ip.polar(c, r, a, 1.4), ip.polar(c, r + 1.0, a, 1.4)
        ip.pipe(k, [n0, n1, (n1[0], n1[1], .2)], .22, 'paint.pipe', end_flanges=False)
        ip.flange(k, ip.polar(c, r + .05, a, 1.4), ip.sub(n1, n0), .33, t=.06)
        ip.valve(k, ip.polar(c, r + .5, a, 1.4), ip.sub(n1, n0), .22)
    k.meta['hero_access'] = (0, hut_y, roofz(r - 1.6))


# ------------------------------------------------------------------ watertower
WT = dict(c=(0.0, -1.25), r=3.3, deck=5.55, z_top=10.5)


def watertower(k):
    s = WT
    c, r = s['c'], s['r']
    zd = s['deck']
    # braced four-leg trestle, deck beams, railed service deck behind the tank
    xs, ys = (-3.25, 3.25), (-4.55, 4.55)
    for x in xs:
        for y in ys:
            k.box((x, y, .2), (.8, .8, .4), 'ferro', 'mass', ch=.08, top=.05, walk=True)
            ip.hbeam(k, (x, y, .4), (x, y, zd - .2), .3, .3)
    for (ax, ay), (bx, by) in (((xs[0], ys[0]), (xs[1], ys[0])), ((xs[1], ys[0]), (xs[1], ys[1])), ((xs[1], ys[1]), (xs[0], ys[1])),
                               ((xs[0], ys[1]), (xs[0], ys[0]))):
        ip.angle_brace(k, (ax, ay, .6), (bx, by, zd - .4), s=.1)
        ip.angle_brace(k, (bx, by, .6), (ax, ay, zd - .4), s=.1)
        ip.hbeam(k, (ax, ay, 2.9), (bx, by, 2.9), .2, .16)
    for y in ys:
        ip.hbeam(k, (xs[0], y, zd - .15), (xs[1], y, zd - .15), .3, .2)
    for x in xs:
        ip.hbeam(k, (x, ys[0], zd - .15), (x, ys[1], zd - .15), .3, .2)
    ip.rect_platform(k, xs[0] - .35, ys[0] - .35, xs[1] + .35, ys[1] + .1, zd + .05, rail_sides='nesw', legs=False,
                     gaps=[(0.0, ys[1] + .1)], name='trestle deck')
    # riveted rust-brown drum with a conical roof, riveted seams
    ip.vessel(k, c, r, zd + .05, s['z_top'], bottom='flat', courses=4, mat='paint.main', head_mat='paint.roof', n=64,
              top_prof=[(r, s['z_top']), (r * .5, s['z_top'] + .45), (0, s['z_top'] + .7)])
    ip.ring_band(k, c, r, zd + .05, zd + .3, 'paint.second', proud=.08, n=64)
    count(k, 'tanks')
    roofz = lambda d: s['z_top'] + .7 * (1 - d / r)  # noqa: E731
    k.cyl((c[0], c[1], roofz(0) + .1), .3, .3, 'paint.second', 'frame', sides=16, top=.04)
    ip.mushroom_vent(k, c[0] - .9, c[1] - .9, roofz(1.3) - .05, .2)
    # access: caged ladder up the rear to the service deck, then a shell ladder to the roof hatch
    ip.ladder(k, 0.0, ys[1] + .1, math.pi / 2, 0.0, zd + .05, off=.18, reach=.25, standoff=False, name='trestle ladder')
    ip.ladder(k, 1.4, c[1] + math.sqrt(r * r - 1.96), math.atan2(math.sqrt(r * r - 1.96), 1.4), zd + .05, s['z_top'], off=.18, cage=True,
              ext=1.0, name='tank ladder')
    lp = ip.polar(c, r - .6, math.atan2(math.sqrt(r * r - 1.96), 1.4), 0)
    lp = ip.polar(c, r - .05, math.atan2(math.sqrt(r * r - 1.96), 1.4), 0)
    k.add(*ip.box_along((lp[0], lp[1], roofz(r - .05) + .12), ip.polar(c, .5, math.atan2(math.sqrt(r * r - 1.96), 1.4), roofz(.5) + .12), (1, 0, 0),
                        (0, 0, 1), .7, .06), 'grate', 'frame', walk=True)
    ip.hatch(k, *ip.polar(c, 1.5, math.radians(200), 0)[:2], roofz(1.5) - .1, .7)
    count(k, 'stairs_ladders', 2)
    # pump house under the deck, riser from the tank bottom down through the trestle into it
    k.box((0.5, 3.6, 1.6), (3.0, 2.0, 3.2), 'paint.second', 'mass', ch=.08, top=.06)
    k.box((0.5, 3.6, 3.26), (3.2, 2.2, .12), 'paint.trim', 'frame', ch=.04)
    with k.at((0.5, 2.6, 0), yaw=0):
        ip.personnel_door(k, .9, 1.0, 2.2, -.2, canopy=False)   # proud of the pump-house face, clear of the riser
    count(k, 'doors')
    ip.pipe(k, [(c[0], c[1], zd - .05), (c[0], c[1], 1.4), (c[0], 2.4, 1.4)], .2, 'paint.pipe', end_flanges=False)
    ip.flange(k, (c[0], c[1], zd - .1), (0, 0, 1), .3, t=.06)
    ip.flange(k, (c[0], 2.55, 1.4), (0, 1, 0), .3, t=.06)
    ip.valve(k, (c[0], c[1], 2.4), (0, 0, 1), .2, wheel=False)
    k.meta['hero_access'] = (0.0, ys[1], zd)


# ------------------------------------------------------------------ registry
SPECS = {
    'largefueltank': dict(
        fn=largefueltank, palette='intent_tank', cls='large',
        intent=('A large refinery fuel storage tank. A wide cream-grey tank in six welded courses on a battered '
                'ferrocrete ring foundation, yellow-black hazard band low on the shell, wind girder at the eave and a '
                'low dome roof with a hazard-marked crown hatch (the stock yellow star). Caged ladder from grade to a '
                'foundation landing, spiral stair up the shell to a railed rim landing and a roof walkway; railed roof '
                'edge, breather vents, shell manholes; the battered foot carries cast surrounds (pump-room door, louvres, hazard hatches) and the product line leaves to -X through a gate valve onto the pipeline stub.'),
        counts=dict(tanks=1, stairs_ladders=2, doors=1)),
    'liquidstorage': dict(
        fn=liquidstorage, palette='intent_tank', cls='large',
        intent=('A low, wide liquid storage tank. A squat round tank stiffened by vertical T-ribs and two ring girders '
                '(the stock panel rhythm), shallow dome roof with a central hatch. The front carries an enclosed '
                'lift/stair tower with a glazed strip and a door at grade, rising to a roof service hut whose door opens '
                'onto a railed walkway to the hatch; red service panels and two valved nozzles at the base.'),
        counts=dict(tanks=1, doors=2)),
    'watertower': dict(
        fn=watertower, palette='intent_rust', cls='small',
        intent=('An elevated water tank. A rust-brown riveted steel drum with a conical roof on a braced four-leg '
                'steel trestle; the trestle top is a railed grating deck around the tank, reached by a caged ladder '
                'on the rear; a short ladder climbs the drum to a roof walkway and hatch. A pump house sits under the '
                'deck, fed by a riser from the tank bottom.'),
        counts=dict(tanks=1, stairs_ladders=2, doors=1)),
}
SPECS['fuelpipe'] = dict(
    fn=fuelpipe, palette='intent_pipe', cls='prop', tol=dict(grid_mae=.34),
    intent=('A small-bore fuel line. A flanged olive spool on saddle stands with yellow ID bands and mid-run '
            'flanged joints, and an actuated ball valve (red body, gearbox and yellow actuator on the +X side, '
            'lock-out handle) near the +Y end, as the stock valve box.'),
    counts=dict(pipes=1, valves=1))
PIPE_INTENT = ('A modular large-bore pipeline piece. Olive-green steel pipe with yellow ID bands at the stock centre '
               'height, bolted flange collars at the module joints so pieces join face to face, steel saddles with '
               'hold-down straps on ferrocrete sleepers.')
HOUSE_INTENT = (' At the junction a battered ferrocrete valve chamber: pipes enter through flanged sleeves, gate-valve '
                'stems rise through a railed, knee-braced grating deck to red handwheels, louvred and hazard-marked '
                'panels on the free faces, a caged ladder to the deck.')
for name, fn, n_pipes, extra in (
        ('straightpipe1', straight(10.65, (5.325,)), 1, {}),
        ('straightpipe2', straight(21.3, (5.3, 16.0)), 1, {}),
        ('straightpipe3', straight(31.95, (10.6, 21.35)), 1, {}),
        ('cornerpipe', junction((5.964, -5.964), (math.pi, math.pi / 2), 0.0, 2.2), 2, dict(valve_houses=1, valves=2, stairs_ladders=1)),
        ('threewaypipe', junction((0.0, -5.964), (0.0, math.pi, math.pi / 2), -math.pi / 2, 2.2), 3, dict(valve_houses=1, valves=3, stairs_ladders=1)),
        ('fourwaypipe', junction((0.0, 0.0), (0.0, math.pi, math.pi / 2, -math.pi / 2), -math.pi / 2, 2.6), 4,
         dict(valve_houses=1, valves=4, stairs_ladders=1)),
        ('endpipe', junction((5.964, 0.0), (math.pi,), 0.0, 2.2), 1, dict(valve_houses=1, valves=1, stairs_ladders=1))):
    SPECS[name] = dict(fn=fn, palette='intent_pipe', cls='prop' if name.startswith('straight') else 'small',
                       # a slender round pipe against the stock's flat-topped octagon section: the coarse height grid
                       # measures section shape, not mass placement, so it gets a looser bound (owner: intent over fit)
                       tol=dict(grid_mae=.34) if name.startswith('straight') else {},
                       intent=PIPE_INTENT + (HOUSE_INTENT if extra else ''), counts=dict(pipes=n_pipes, **extra))
