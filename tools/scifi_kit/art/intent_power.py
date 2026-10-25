"""Intent pass, electrical/power redo (owner 2026-10-24: "the electrical ones need more sci-fi work, way too blocky").

powergenerator, l_turretgenerator, transformerstation. BattleTech power-plant language over the MC2 silhouette
and palette: chamfered, ribbed reactor shells, glowing cyan coil drums behind guards (the in-game mc2_10 cue),
heat-sink fin arrays and radiators, stacked insulator columns and HV bushings, capacitor banks, cable trays on
standoffs, armoured service hatches, warning placards and status lights, detail in clusters. MW5/BT2018 are
reference only (turbine drums with ring ribs, chamfered housings); nothing is copied.
"""
import math

import geo
import ind_parts as ip
import intent as I

count = I.count


# ------------------------------------------------------------------ power parts
def placard(k, p, yaw, w=.55, h=.4):
    """Warning placard (yellow-black border, dark plate) on a wall facing `yaw`."""
    with k.at(p, yaw=yaw + math.pi / 2):
        k.slab(-w / 2, w / 2, -h / 2, h / 2, -.04, 0.0, 'hazard', 'frame')
        k.slab(-w / 2 + .06, w / 2 - .06, -h / 2 + .06, h / 2 - .06, -.06, -.03, 'paint.trim', 'frame')


def status_light(k, p, mat='lamp', r=.09):
    k.cyl(p, r, .12, mat, 'frame', sides=10, top=.03)


def insulator(k, base, h, r=.13, sheds=7, cap='lamp_cool'):
    """Stacked porcelain insulator column (sheds), metal cap and a glowing collar at the tip."""
    x, y, z = base
    k.cyl((x, y, z + .12), r * 2.2, .24, 'metal', 'frame', sides=14, top=.04)
    k.cyl((x, y, z + h / 2), r, h, 'paint.insul', 'frame', sides=12)
    for i in range(sheds):
        zz = z + .4 + i * (h - .6) / max(1, sheds - 1)
        k.cyl((x, y, zz), r * 2.6, .07, 'paint.insul', 'frame', sides=14, top=.02)
    k.cyl((x, y, z + h + .1), r * 1.4, .2, 'metal', 'frame', sides=12, top=.05)
    k.cyl((x, y, z + h + .26), r * 1.1, .12, cap, 'frame', sides=12)


def capacitor_bank(k, c, nx, ny, step=.75, h=1.5, z=0.0, yaw=0.0):
    """Rack of capacitor cans on a skid: cans with terminals, bus bars, a status light."""
    with k.at((c[0], c[1], z), yaw=yaw):
        k.box((0, 0, .15), (nx * step + .5, ny * step + .5, .3), 'paint.steel', 'frame', ch=.05)
        for i in range(nx):
            for j in range(ny):
                x, y = (i - (nx - 1) / 2) * step, (j - (ny - 1) / 2) * step
                k.cyl((x, y, .3 + h / 2), .28, h, 'paint.second', 'frame', sides=14, top=.04)
                k.cyl((x, y, .3 + h + .05), .2, .1, 'paint.trim', 'frame', sides=12)
                k.cyl((x - .08, y, .3 + h + .16), .04, .14, 'metal', 'frame', sides=6)
                k.cyl((x + .08, y, .3 + h + .16), .04, .14, 'metal', 'frame', sides=6)
        for j in range(ny):
            k.box((0, (j - (ny - 1) / 2) * step, .3 + h + .25), (nx * step, .06, .04), 'metal', 'frame')
        status_light(k, (nx * step / 2 + .1, 0, .4), 'lamp_cool')


def fin_array(k, x0, x1, y, z0, z1, depth=.45, pitch=.22, s=-1, mat='paint.second'):
    """Heat-sink fin array on a face (outward = s along y)."""
    n = max(2, int((x1 - x0) / pitch))
    k.box(((x0 + x1) / 2, y + s * .05, (z0 + z1) / 2), (x1 - x0 + .2, .1, z1 - z0 + .2), 'paint.trim', 'frame', ch=.03)
    for i in range(n + 1):
        x = x0 + (x1 - x0) * i / n
        k.box((x, y + s * (.1 + depth / 2), (z0 + z1) / 2), (.05, depth, z1 - z0), mat, 'frame')
    for z in (z0 + .1, z1 - .1):
        k.box(((x0 + x1) / 2, y + s * (.1 + depth), z), (x1 - x0, .06, .06), 'metal', 'frame')


def cable_tray(k, pts, z, off_dir=None, step=1.6):
    """Ladder-type cable tray with three cables on standoff brackets, along a plan polyline at height z."""
    P = [(p[0], p[1], z) for p in pts]
    for a, b in zip(P, P[1:]):
        d = ip.norm(ip.sub(b, a))
        side = (-d[1], d[0], 0)
        for sgn in (-1, 1):
            k.beam(ip.add(a, ip.mul(side, sgn * .22)), ip.add(b, ip.mul(side, sgn * .22)), .04, 'metal', 'frame', h=.12)
        L = ip.length(ip.sub(b, a))
        for i in range(int(L / step) + 1):
            q = ip.add(a, ip.mul(ip.sub(b, a), i / max(1, int(L / step))))
            k.box((q[0], q[1], z - .07), (.5 if abs(d[0]) > .5 else .08, .08 if abs(d[0]) > .5 else .5, .04), 'metal', 'frame')
            if off_dir is not None:
                k.beam((q[0], q[1], z - .08), (q[0] + off_dir[0] * .45, q[1] + off_dir[1] * .45, z - .08), .06, ip.STEEL, 'frame', h=.06)
    for j, o in enumerate((-.12, 0.0, .12)):
        path = []
        for i, q in enumerate(P):
            a_, b_ = P[max(0, i - 1)], P[min(len(P) - 1, i + 1)]
            d = ip.norm(ip.sub(b_, a_))
            path.append(ip.add(q, ((-d[1]) * o, d[0] * o, .06)))
        ip.tube(k, path, .045, 'rubber' if j != 1 else 'paint.valve', 'frame', sides=6)


def armoured_hatch(k, p, yaw, w=1.6, h=2.4):
    """Recessed armoured service hatch: chamfered frame, plate door with ribs, hazard sill, lamp above."""
    with k.at(p, yaw=yaw + math.pi / 2):
        for a_, b_ in ((-w / 2 - .35, -w / 2), (w / 2, w / 2 + .35)):            # jambs, lintel and a back plate: the leaf stays visible
            k.slab(a_, b_, 0, h + .35, -.32, .02, 'paint.trim', 'frame', front=.1)
        k.slab(-w / 2, w / 2, h, h + .35, -.32, .02, 'paint.trim', 'frame', front=.1)
        k.slab(-w / 2, w / 2, 0, h, -.12, .02, 'paint.trim', 'frame')
        k.slab(-w / 2, w / 2, .05, h, -.2, -.14, 'paint.door', 'skin', front=.04, door=True)
        for z in (h * .3, h * .55, h * .8):
            k.slab(-w / 2 + .12, w / 2 - .12, z - .05, z + .05, -.24, -.2, 'metal', 'frame')
        k.slab(-w / 2 - .35, w / 2 + .35, 0, .12, -.42, -.3, 'hazard', 'frame')
        k.box((w / 2 - .25, -.26, h * .5), (.12, .08, .3), 'metal', 'detail')
    count(k, 'doors')


def coil_bay(k, x0, x1, y, zc, r=.62, s=-1):
    """Exposed generator coil: a horizontal drum with glowing ring windings, end bearings, guard bars."""
    yc = y + s * (r + .15)
    ip.tube(k, [(x0, yc, zc), (x1, yc, zc)], r, 'paint.second', 'frame', sides=24)
    n = max(3, int((x1 - x0) / .55))
    for i in range(n):
        xa = x0 + .3 + (x1 - x0 - .6) * i / max(1, n - 1)
        ip.tube(k, [(xa - .1, yc, zc), (xa + .1, yc, zc)], r + .06, 'lamp_cool', 'frame', sides=24)
    for xe in (x0, x1):
        ip.tube(k, [(xe - .15, yc, zc), (xe + .15, yc, zc)], r + .22, 'paint.trim', 'frame', sides=24)
    for i in range(int((x1 - x0) / .9) + 1):
        xa = x0 + (x1 - x0) * i / max(1, int((x1 - x0) / .9))
        k.box((xa, yc + s * (r + .32), zc), (.06, .06, 2 * r + .5), 'metal', 'frame')


def ribbed_shell(k, prof, x0, x1, rib_step=2.6, mat='paint.main', rib_mat='paint.trim'):
    """Chamfered armoured shell extruded along x with proud structural ribs following its section."""
    k.prism(prof, x0, x1, mat, 'mass', axis='x')
    out = [((y + (.22 if y > 0 else -.22)) if abs(y) > .1 else y, z + (.22 if z > 1.0 else 0.0)) for y, z in prof]
    n = max(1, round((x1 - x0) / rib_step))
    for i in range(n + 1):
        x = x0 + (x1 - x0) * i / n
        x = min(max(x, x0 + .2), x1 - .2)
        k.prism(out, x - .18, x + .18, rib_mat, 'frame', axis='x')


# ------------------------------------------------------------------ dynamo generators (owner: closer to stock, "a dynamo thing")
def stadium(cx, cz, w, h, n=8):
    """Capsule (rounded-end slot) outline in (x, z), CCW."""
    r = h / 2
    pts = []
    for i in range(n + 1):                      # right cap, bottom to top
        a = -math.pi / 2 + math.pi * i / n
        pts.append((cx + w / 2 - r + r * math.cos(a), cz + r * math.sin(a)))
    for i in range(n + 1):                      # left cap, top to bottom
        a = math.pi / 2 + math.pi * i / n
        pts.append((cx - w / 2 + r + r * math.cos(a), cz + r * math.sin(a)))
    return pts


def dumbbell_rotor(k, x0, x1, y, zc, r, glow=True):
    """A rotor seen through a slot: shaft with flywheel discs at both ends and a larger wound armature
    drum in the middle (modest cyan windings), bearing collars at the shaft ends."""
    L = x1 - x0
    ip.tube(k, [(x0, y, zc), (x1, y, zc)], r * .28, 'metal', 'frame', sides=12)
    for xe in (x0 + L * .1, x1 - L * .1):
        ip.tube(k, [(xe - L * .04, y, zc), (xe + L * .04, y, zc)], r * .85, 'paint.second', 'frame', sides=24)
        ip.tube(k, [(xe - L * .045, y, zc), (xe - L * .035, y, zc)], r * .9, 'paint.trim', 'frame', sides=24)
    xa, xb = x0 + L * .26, x1 - L * .26
    ip.tube(k, [(xa, y, zc), (xb, y, zc)], r, 'paint.main', 'frame', sides=24)
    n = max(3, int((xb - xa) / .7))
    for i in range(n):
        xm = xa + (xb - xa) * (i + .5) / n
        ip.tube(k, [(xm - .06, y, zc), (xm + .06, y, zc)], r + .03, 'lamp_cool' if glow else 'paint.trim', 'frame', sides=24)
    for xe in (x0, x1):
        ip.tube(k, [(xe - .08, y, zc), (xe + .08, y, zc)], r * .45, 'paint.trim', 'frame', sides=16)


def battered_housing(k, x0, x1, yb, yt, h, zb, ch=.6, mat='paint.main'):
    """Stock-style battered block: wide base, narrower top, chamfered top edges, tan lower band."""
    prof = [(-yb, zb), (yb, zb), (yb - .1, zb + 1.0), (yt + ch * .5, h - ch), (yt - ch * .3, h), (-yt + ch * .3, h), (-yt - ch * .5, h - ch),
            (-yb + .1, zb + 1.0)]
    k.prism(prof, x0, x1, mat, 'mass', axis='x')
    band = [(-yb - .04, zb), (yb + .04, zb), (yb - .02, zb + 2.6), (-yb + .02, zb + 2.6)]
    k.prism(band, x0 - .03, x1 + .03, 'paint.second', 'skin', axis='x')
    for x in (x0 + .25, x1 - .25):           # chamfered end frames
        k.prism([((p[0] * 1.02), p[1] + (.05 if p[1] > zb + .5 else 0)) for p in prof], x - .15, x + .15, 'paint.trim', 'frame', axis='x')
    return prof


def face_tilt(prof):
    """Lean of the battered long face (radians) and the face y at a height, from the housing profile."""
    (y0, z0), (y1, z1) = prof[2], prof[3]
    return math.atan2(y0 - y1, z1 - z0), (lambda z: y0 - (y0 - y1) * (z - z0) / (z1 - z0))


def slot_panel(k, half, prof, s, zs, rotor_r):
    """Big chamfered recessed frame on a long face with two capsule slots, each showing a dumbbell rotor."""
    tilt, yface = face_tilt(prof)
    zc = (zs[0] + zs[-1]) / 2
    with k.at((0, s * yface(zc), zc), yaw=0 if s < 0 else math.pi, pitch=-tilt):
        hw, hh = half * .9, (zs[-1] - zs[0]) / 2 + 2.1
        k.prism(geo.rect_poly(-hw, -hh, hw, hh, .7), -.28, .05, 'paint.trim', 'frame', axis='y')
        k.prism(geo.rect_poly(-hw + .35, -hh + .35, hw - .35, hh - .35, .5), -.36, -.24, 'paint.second', 'frame', axis='y')
        for x in (-hw + .2, hw - .2):
            for zz in (-hh + .2, hh - .2):
                k.box((x, -.32, zz), (.12, .1, .12), 'metal', 'detail')
        for z in zs:
            dz = z - zc
            w = 2 * half * .74
            k.prism(stadium(0, dz, w + .3, rotor_r * 2 + .7), -1.05, -.3, 'paint.trim', 'frame', axis='y')
            k.prism(stadium(0, dz, w, rotor_r * 2 + .4), -.32, -.28, 'paint.dark', 'frame', axis='y')
            dumbbell_rotor(k, -w / 2 + .35, w / 2 - .35, -.6, dz, rotor_r)


def pod(k, x0, x1, h, zb, s, louvre=True):
    """Stock flanking pod: battered block, louvred outer end, tan lower panels, bearing boss toward the
    centre, two insulators on top, a conduit drop and a placard."""
    prof = battered_housing(k, x0, x1, 6.0, 4.6, h, zb, ch=.5)
    xo, xi = (x1, x0) if s > 0 else (x0, x1)
    if louvre:
        with k.at((xo, 0, 0), yaw=s * math.pi / 2):
            k.slab(-3.2, 3.2, 3.2, h - 1.6, -.12, .02, 'paint.trim', 'frame', front=.05)
            for j in range(int((h - 5.2) / .32)):
                k.slab(-3.0, 3.0, 3.4 + j * .32, 3.52 + j * .32, -.22, -.08, 'grate', 'frame')
            k.slab(-2.4, -.6, zb + .5, zb + 2.0, -.08, .03, 'paint.trim', 'frame', front=.02)
            k.slab(.6, 2.4, zb + .5, zb + 2.0, -.08, .03, 'paint.trim', 'frame', front=.02)
    for yy in (-2.0, 2.0):
        insulator(k, ((x0 + x1) / 2, yy, h), 1.3, r=.12, sheds=5)
    k.box(((x0 + x1) / 2, 0, h + .3), (min(2.0, (x1 - x0) * .6), 1.6, .6), 'paint.second', 'frame', ch=.08, top=.06)
    for sy in (-1, 1):
        k.box(((x0 + x1) / 2, sy * 5.3, h * .62), (x1 - x0 - .6, .08, .1), 'paint.trim', 'frame')
    placard(k, ((x0 + x1) / 2, -6.05, zb + 4.6), -math.pi / 2, w=.5, h=.35)
    status_light(k, ((x0 + x1) / 2 + .7, -6.05, zb + 4.6), 'lamp_cool')
    return prof


def segmented_drum(k, half, zc, R):
    """Horizontal stator drum in stepped ribbed segments (largest in the middle, stepping down to each end),
    cyan glow bands in the joints, longitudinal panel seams with rivets, radial end caps."""
    segs = [(-half * .4, half * .4, R)]
    for sgn in (-1, 1):
        segs += [tuple(sorted((sgn * (half * .4 + .4), sgn * (half * .76)))) + (R * .86,),
                 tuple(sorted((sgn * (half * .76 + .35), sgn * (half - .05)))) + (R * .72,)]
    for x0, x1, r in segs:
        ip.tube(k, [(x0, 0, zc), (x1, 0, zc)], r, 'paint.main', 'mass', sides=48)
        for xe in (x0, x1):
            ip.tube(k, [(xe - .14 if xe == x1 else xe, 0, zc), (xe if xe == x1 else xe + .14, 0, zc)], r + .1, 'paint.trim', 'frame', sides=48)
        n = 10 if r == R else 8
        for i in range(n):
            a = math.pi * .12 + (math.pi * .76) * i / (n - 1)
            p0 = (x0 + .2, -r * math.cos(a) * 1.005, zc + r * math.sin(a) * 1.005)
            p1 = (x1 - .2, -r * math.cos(a) * 1.005, zc + r * math.sin(a) * 1.005)
            k.add(*ip.box_along(p0, p1, (0, math.sin(a), math.cos(a)), (0, -math.cos(a), math.sin(a)), .1, .05), 'paint.trim', 'frame')
            ip.bolt_row(k, (p0[0], p0[1] * 1.01, p0[2]), (p1[0], p1[1] * 1.01, p1[2]), .55, .05)
    # glow bands in the joints between segments (modest: thin ring, set back)
    joints = sorted({round(v, 3) for x0, x1, r in segs for v in (x0, x1)})
    for j0, j1 in zip(joints, joints[1:]):
        if 0 < j1 - j0 < .5:
            rr = min(r for x0, x1, r in segs if abs(x0 - j0) < .01 or abs(x1 - j0) < .01 or abs(x0 - j1) < .01 or abs(x1 - j1) < .01)
            ip.tube(k, [(j0, 0, zc), (j1, 0, zc)], rr - .05, 'lamp_cool', 'frame', sides=48)
    # radial end caps on the outermost segments
    for sgn in (-1, 1):
        x0, x1, r = min(segs, key=lambda g: g[0]) if sgn < 0 else max(segs, key=lambda g: g[1])
        xe = x0 if sgn < 0 else x1
        ip.tube(k, [(xe, 0, zc), (xe + sgn * .2, 0, zc)], r * .62, 'paint.second', 'frame', sides=32)
        ip.tube(k, [(xe + sgn * .2, 0, zc), (xe + sgn * .3, 0, zc)], r * .5, 'lamp_cool', 'frame', sides=32)
        for i in range(8):
            a = i * math.tau / 8
            q0 = (xe + sgn * .05, r * .3 * math.cos(a), zc + r * .3 * math.sin(a))
            q1 = (xe + sgn * .05, r * .9 * math.cos(a), zc + r * .9 * math.sin(a))
            k.add(*ip.box_along(q0, q1, (1, 0, 0), (0, -math.sin(a), math.cos(a)), .16, .35), 'paint.trim', 'frame')
    count(k, 'drums')


def dynamo(k, half, pods, link=None, hatch_back=False):
    """Generator as a dynamo (stock silhouette): a battered central armature housing with the two capsule
    slots (dumbbell rotors inside) on both long faces and three roof slots, a through-shaft with bearing
    bosses and couplings to the flanking pods/wings; modest glow, cable runs, a few insulators."""
    W = pods[1]
    zb = .4
    k.box((0, 0, .2), (2 * W + .6, 15.9, .4), 'ferro', 'mass', ch=.15, top=.06, walk=True)
    # battered cradle (the stock centre-piece's lower body, tan band) carrying the two slots on each face
    prof = battered_housing(k, -half, half, 7.6, 6.5, 6.6, zb, ch=.4)
    for s in (-1, 1):
        slot_panel(k, half, prof, s, (2.6, 4.8), .55)
    # the stator drum is the stock animated node 'Coil' (powergeneratoranim.ase / l_turretgeneratoranim.ase: one
    # CONTROL_ROT_TRACK about the length axis); built as its own node, origin on the drum axis, so the bake keeps it
    specs = k.meta.setdefault('anim_nodes', [])
    specs.append(dict(name='Coil', pivot=(0.0, 0.0, 6.9), parent=None, axis=(1.0, 0.0, 0.0), stock='Coil'))
    n0 = len(k.pieces)
    segmented_drum(k, half, 6.9, 6.0)
    for pc in k.pieces[n0:]:
        pc['anim_id'] = len(specs)
        pc['anim'] = 'Coil'
    for sy in (-1, 1):            # hazard floor markings along the plinth edges
        k.box((0, sy * 7.82, .42), (2 * pods[1] + .2, .2, .03), 'hazard', 'frame')
    # through-shaft: bearing bosses on the housing ends, couplings, bearing pedestals in the gaps
    p0, p1, ph = pods
    zs = 6.4
    for s in (-1, 1):
        xe = s * half
        ip.tube(k, [(xe, 0, zs), (xe + s * .55, 0, zs)], 2.0, 'paint.second', 'frame', sides=28)
        xg0, xg1 = xe + s * .55, s * p0
        ip.tube(k, [(xg0, 0, zs), (xg1, 0, zs)], .7, 'metal', 'frame', sides=16)
        xm = (xg0 + xg1) / 2
        ip.tube(k, [(xm - .2, 0, zs), (xm + .2, 0, zs)], 1.3, 'paint.trim', 'frame', sides=20)
        if abs(xg1 - xg0) > 1.2:
            k.box((xm, 0, (zs - .7 + zb) / 2), (.9, 2.6, zs - .7 - zb), 'paint.second', 'frame', ch=.08)
        ip.tube(k, [(s * p0 - s * .5, 0, zs), (s * p0, 0, zs)], 2.0, 'paint.second', 'frame', sides=24)
        ip.tube(k, [(s * p0 - s * .56, 0, zs), (s * p0 - s * .46, 0, zs)], 2.1, 'paint.trim', 'frame', sides=24)
        if link:
            lx0, lx1, lh = link
            k.prism(geo.rect_poly(min(s * lx0, s * lx1), -5.0, max(s * lx0, s * lx1), 5.0, .6), zb, lh, 'paint.main', 'mass', top=.4)
            k.box((s * (lx0 + lx1) / 2, 0, lh + .1), (abs(lx1 - lx0) - .4, 4.0, .2), 'grate', 'frame')
        pod(k, min(s * p0, s * p1), max(s * p0, s * p1), ph, zb, s)
        # cable run from the pod down the long face to a ground trench
        cable_tray(k, [(s * (p0 + .4), -6.6), (s * (half + .3), -6.6)], 1.2)
    # owner round 6 check: the placard and status light stood 2.4 m off the housing; now on its battered face
    placard(k, (-half * .9, -7.36, 2.2), -math.pi / 2)
    status_light(k, (half * .9, -7.36, 2.2), 'lamp_red')
    armoured_hatch(k, (half, 5.9, zb), 0.0, w=1.4, h=2.4) if not hatch_back else         armoured_hatch(k, (pods[1], 3.0, zb), 0.0, w=1.4, h=2.4)   # on the pod's outer end: 2 m clear in front
    count(k, 'halls')
    count(k, 'wings', 2)


def powergenerator(k):
    dynamo(k, 6.9, (10.6, 20.6, 10.2), link=(7.6, 10.6, 8.2))


def l_turretgenerator(k):
    dynamo(k, 6.2, (7.8, 10.5, 9.4), hatch_back=True)   # the pod stands 1.6 m off the housing end, so the hatch goes on the pod


# ------------------------------------------------------------------ transformerstation
def transformer(k, xc, zd):
    """Sci-fi step-up transformer: chamfered octagonal tank, ribbed radiator banks with fans, three
    HV bushing columns with glowing collars, a conservator drum on saddles, conduit and placards."""
    k.prism(geo.rect_poly(xc - 1.9, -5.8, xc + 1.9, 5.8, .7), zd, zd + 5.4, 'paint.main', 'mass', top=.4)
    for z in (zd + 1.4, zd + 3.0, zd + 4.6):
        k.prism(geo.rect_poly(xc - 1.98, -5.88, xc + 1.98, 5.88, .72), z, z + .14, 'paint.trim', 'frame')
    for s in (-1, 1):
        x = xc + s * 1.9
        for j in range(3):
            y0 = -5.0 + j * 3.6
            k.box((x + s * .6, y0 + 1.2, zd + 2.7), (1.1, 2.6, 4.2), 'paint.second', 'mass', ch=.08)
            for i in range(11):
                k.box((x + s * 1.2, y0 + .05 + i * .23, zd + 2.7), (.32, .06, 3.9), 'paint.trim', 'frame')
            k.cyl((x + s * 1.38, y0 + 1.2, zd + 4.6), .42, .08, 'grate', 'frame', sides=16)
        ip.tube(k, [(x, -5.0, zd + .6), (x + s * .6, -5.0, zd + .6), (x + s * .6, 5.4, zd + .6)], .12, 'paint.pipe', 'frame', sides=8)
    for j, yy in enumerate((-3.8, 0.0, 3.8)):
        insulator(k, (xc, yy, zd + 5.4), 2.0, r=.16, sheds=7)
    k.box((xc, -6.2, zd + 4.6), (.9, .8, .6), 'paint.second', 'frame', ch=.05)
    for yy in (-4.0, 4.0):
        k.box((xc + 1.25, yy, zd + 5.75), (.3, .4, .7), ip.STEEL, 'frame')
    ip.tube(k, [(xc + 1.25, -5.0, zd + 6.55), (xc + 1.25, 5.0, zd + 6.55)], .5, 'paint.main', 'frame', sides=20)
    placard(k, (xc, -5.85, zd + 3.8), -math.pi / 2)
    status_light(k, (xc - .9, -5.9, zd + 4.7), 'lamp_cool')
    status_light(k, (xc + .9, -5.9, zd + 4.7), 'lamp_red')
    count(k, 'transformers')


def transformerstation(k):
    zd = 3.2
    # raised, chamfered switchyard deck with panel joints, approved railing/stair/landing kept
    k.prism(geo.rect_poly(-9.3, -10.3, 10.3, 10.3, .2), 0, zd, 'ferro', 'mass', top=.08, walk=True)
    for x in (-5.0, 0.5, 6.0):
        k.box((x, -10.32, zd / 2), (.12, .06, zd - .4), 'paint.trim', 'frame')
    k.box((.5, -10.3, .5), (19.4, .1, .3), 'hazard', 'skin')
    pts = [(-9.25, -10.15), (10.15, -10.15), (10.15, 10.15), (-9.25, 10.15)]
    ip.railing(k, pts, zd, h=1.1, closed=True, gaps=[(-9.25, -3.1)])
    transformer(k, -3.4, zd)
    transformer(k, 5.6, zd)
    # busbar gantry: a portal over both units, tubular bus on insulator strings from the bushings
    for yy in (-7.6, 7.6):
        for x in (-6.6, 8.8):
            ip.hbeam(k, (x, yy, zd), (x, yy, zd + 8.0), .3, .3)
        ip.hbeam(k, (-6.8, yy, zd + 8.0), (9.0, yy, zd + 8.0), .3, .24)
        for x in (-4.0, 1.2, 6.2):
            ip.tube(k, [(x, yy, zd + 7.85), (x, yy, zd + 7.35)], .05, 'paint.insul', 'frame', sides=8)
    for j, yy in enumerate((-3.8, 0.0, 3.8)):
        ip.tube(k, [(-3.4, yy, zd + 7.76), (1.1, yy * 1.3, zd + 7.95), (5.6, yy, zd + 7.76)], .07, 'metal', 'frame', sides=8)
    # capacitor bank and control cabinets between the units, cable trays along the deck edges
    capacitor_bank(k, (1.1, -7.6), 3, 2, step=.8, z=zd, h=1.3)
    capacitor_bank(k, (1.1, 7.6), 3, 2, step=.8, z=zd, h=1.3)
    for y in (-8.6, 8.6):
        k.box((-7.4, y, zd + 1.2), (2.0, .9, 2.4), 'paint.second', 'frame', ch=.08, top=.05)
        k.box((-7.4, y - .47, zd + 1.5), (1.6, .04, 1.4), 'paint.trim', 'frame')
        status_light(k, (-6.9, y - .5, zd + 2.2), 'lamp_cool')
        status_light(k, (-7.9, y - .5, zd + 2.2), 'lamp_red')
        placard(k, (-7.4, y - .5, zd + 1.0), -math.pi / 2, w=.45, h=.3)
    cable_tray(k, [(-8.6, -9.6), (9.6, -9.6), (9.6, 9.6), (-8.6, 9.6)], zd + .35)
    for x in (-9.32, 10.32):
        for y in (-6.0, 0.0, 6.0):
            placard(k, (x, y, zd - 1.0), 0.0 if x > 0 else math.pi)
    ip.stair(k, (-9.85, -10.3, 0.0), (-9.85, -3.85, zd), w=.9, name='switchyard stair')
    ip.rect_platform(k, -10.4, -3.85, -9.3, -2.4, zd, rail_sides='nw', legs=True, name='stair head')
    count(k, 'stairs_ladders')


SPECS = {
    'powergenerator': dict(fn=powergenerator, palette='intent_gen', cls='large', counts=dict(halls=1, wings=2),
        intent=('A dynamo power generator in the stock silhouette: a battered tan-banded cradle carrying a horizontal '
                'stator drum in stepped ribbed segments (largest in the middle) with cyan glow in the joints, panel seams, '
                'rivets and radial end caps; a big chamfered frame on each cradle face holds the two capsule slots, each '
                'showing a dumbbell rotor (shaft, flywheel ends, wound drum, modest glow); a through-shaft '
                'with bearing bosses and couplings runs out through low link housings into the two long louvred wings; '
                'cable runs, a few insulators, placards, status lights and an armoured hatch.')),
    'l_turretgenerator': dict(fn=l_turretgenerator, palette='intent_gen', cls='large', counts=dict(halls=1, wings=2),
        intent=('A defence dynamo: the stock battered centre-piece as a tan-banded cradle under a horizontal stator drum '
                'in stepped ribbed segments (largest in the middle) with cyan joint glow and radial end caps; the two '
                'capsule slots on each cradle face show dumbbell rotors (shaft, flywheel ends, wound centre drum, modest '
                'glow); the shaft runs out through bearing bosses and couplings over bearing pedestals into '
                'the two smaller flanking pods (louvred ends, tan lower panels, insulators); cable runs and placards.')),
    'transformerstation': dict(fn=transformerstation, palette='intent_gen', cls='large', counts=dict(transformers=2, stairs_ladders=1),
        intent=('A step-up transformer station: a raised, chamfered switchyard deck with railing and stair; two '
                'chamfered transformer tanks with ribbed radiator banks and fans, three stacked HV bushing columns with '
                'glowing collars and a conservator drum each; a busbar portal with insulator strings over both units, '
                'capacitor banks, control cabinets with status lights, cable trays and warning placards.')),
}
