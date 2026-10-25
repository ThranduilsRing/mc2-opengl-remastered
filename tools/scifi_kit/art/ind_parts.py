"""Industrial kit parts for the intent pass (owner 2026-10-24: intent over measured fit).

Clean, well-made parts in the reference style (tanks with heads, columns, pipe runs
with elbows/flanges/valves, racks, lattice towers, caged ladders, railings,
platforms, stairs, vents, hazard bands, bolted seams). Pure Python on geo.Kit:
metres, Z up, front -Y. Vocabulary note: E:/mc2-ref/scifi-kit/art/intent-pass/VOCABULARY.md.

Roles: silhouette masses 'mass'/'skin'; ladders, rails, pipes and steel 'frame'
(kept as game geometry: thin parts cannot be baked onto a carrier); bolts and
small fittings 'detail' (baked into the atlas, dropped from the game carrier).
"""
import math

import geo
from geo import rect_poly, subtract_rect

RAIL = 'paint.accent'       # safety-yellow handrails and cages (MW5 set dressing)
STEEL = 'paint.steel'       # dark structural steel
GALV = 'metal'              # galvanised ladders, bolts, bare pipe


def game():
    return geo.GAME_MESH


def seg(n_high, n_game=None):
    return (n_game or max(8, n_high * 2 // 3)) if geo.GAME_MESH else n_high


# ------------------------------------------------------------------ vector helpers
def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def add(a, b):
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])


def mul(a, s):
    return (a[0] * s, a[1] * s, a[2] * s)


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def norm(a):
    ln = math.sqrt(dot(a, a)) or 1e-9
    return (a[0] / ln, a[1] / ln, a[2] / ln)


def length(a):
    return math.sqrt(dot(a, a))


def polar(c, r, a, z):
    return (c[0] + r * math.cos(a), c[1] + r * math.sin(a), z)


# ------------------------------------------------------------------ surfaces of revolution
def lathe(k, prof, mat, role='mass', n=48, c=(0.0, 0.0), uv_scale=None, a0=0.0, a1=None, cap=True, **fl):
    """Revolve a (r, z) profile about the vertical axis at c. Poles (r=0) close cleanly.

    uv_scale=(su, sv): u = turn fraction * su, v = z * sv (hazard.wrap stripes).
    a0/a1: partial revolution (open sheet) in radians.
    """
    n = seg(n)
    full = a1 is None
    a1 = a0 + math.tau if full else a1
    su, sv = uv_scale or (1.0, 1.0)
    verts, faces, uv, rings = [], [], [], []
    for r, z in prof:
        if r < 1e-5:
            rings.append(('pole', len(verts)))
            verts.append((c[0], c[1], z))
            uv.append((.5 * su, z * sv))
            continue
        start = len(verts)
        for j in range(n + 1):
            a = a0 + (a1 - a0) * j / n
            verts.append((c[0] + r * math.cos(a), c[1] + r * math.sin(a), z))
            uv.append((j / n * su, z * sv))
        rings.append(('ring', start))
    for (ta, ia), (tb, ib) in zip(rings, rings[1:]):
        for j in range(n):
            if ta == 'pole' and tb == 'pole':
                continue
            if ta == 'pole':
                faces.append((ia, ib + j, ib + j + 1))
            elif tb == 'pole':
                faces.append((ia + j, ia + j + 1, ib))
            else:
                faces.append((ia + j, ia + j + 1, ib + j + 1, ib + j))
    if cap and full:
        if rings[0][0] == 'ring':
            faces.append(tuple(reversed(range(rings[0][1], rings[0][1] + n))))
        if rings[-1][0] == 'ring':
            faces.append(tuple(range(rings[-1][1], rings[-1][1] + n)))
    return k.add(verts, faces, mat, role, uv=uv, **fl)


def head_profile(r, z, kind, up=True, n=8):
    """Vessel head from the shell edge (r, z) to the pole. kind: ellip | hemi | cone | flat | dish."""
    s = 1 if up else -1
    if kind == 'flat':
        return [(r, z), (0.0, z)]
    if kind == 'cone':
        return [(r, z), (r * .08, z + s * r * .38), (0.0, z + s * r * .40)]
    hh = r * {'ellip': .5, 'dish': .26}.get(kind, 1.0)
    return [(r * math.cos(i / n * math.pi / 2), z + s * hh * math.sin(i / n * math.pi / 2)) for i in range(n + 1)]


def vessel(k, c, r, z0, z1, top='ellip', bottom='flat', courses=4, mat='paint.main', head_mat=None, n=56, seams=True,
           bands=(), top_prof=None, bolts=True):
    """Vertical vessel as ONE continuous surface of revolution (review 2026-10-24: no proud course sleeves).

    Profile ascends pole-to-pole so the winding is uniform; material zones (heads, shell,
    hazard bands) are consecutive lathes sharing exact boundary rings, so recalc welds
    them into one closed body. Course seams are 12 mm weld beads inside the profile.
    bands: [(za, zb, 'hazard.wrap' | material)]; top_prof: custom [(r, z)...] from the rim up.
    """
    seam_z = [z0 + (z1 - z0) * i / max(1, courses) for i in range(1, max(1, courses))] if seams else []
    bot = list(reversed(head_profile(r, z0, bottom, up=False)))
    lathe(k, bot, head_mat or mat, 'mass', n=n, c=c, cap=False)
    cuts = sorted({z0, z1, *[z for zz in bands for z in zz[:2]]})
    zone = {}
    for za, zb, m in bands:
        zone[(za, zb)] = m
    for za, zb in zip(cuts, cuts[1:]):
        m = zone.get((za, zb), mat)
        prof = [(r, za)]
        for zs in seam_z:
            if za + .08 < zs < zb - .08:
                prof += [(r, zs - .035), (r + .012, zs - .018), (r + .012, zs + .018), (r, zs + .035)]
        prof.append((r, zb))
        if m.startswith('hazard'):
            stripes = max(6, round(math.tau * r / .9))
            lathe(k, prof, m, 'mass', n=n, c=c, cap=False, uv_scale=(stripes / 12.0, stripes / (math.tau * r)))
        else:
            lathe(k, prof, m, 'mass', n=n, c=c, cap=False)
    lathe(k, top_prof or head_profile(r, z1, top), head_mat or mat, 'mass', n=n, c=c, cap=False)
    if bolts:
        for zs in seam_z:
            bolt_ring(k, c, r + .014, zs, pitch=.42)


def sphere(k, c, r, zc, mat='paint.main', n=48, rings=16):
    prof = [(r * math.sin(i / rings * math.pi), zc - r * math.cos(i / rings * math.pi)) for i in range(rings + 1)]
    prof[0] = (0.0, prof[0][1])
    prof[-1] = (0.0, prof[-1][1])
    return lathe(k, prof, mat, 'mass', n=n, c=c)


def ring_band(k, c, r, z0, z1, mat='paint.second', proud=.04, n=56):
    """Stiffener/trim ring seated on a shell of radius r: closed annular section, inner face buried."""
    return lathe(k, [(r - .02, z0), (r + proud, z0), (r + proud, z1), (r - .02, z1)], mat, 'frame', n=n, c=c, cap=False)


def bolt_ring(k, c, r, z, pitch=.3, s=.035, mat=GALV):
    if game():
        return
    cnt = max(8, int(math.tau * r / pitch))
    for i in range(cnt):
        a = i * math.tau / cnt
        k.box(polar(c, r, a, z), (s, s, s), mat, 'detail')


def bolt_row(k, p0, p1, pitch=.3, s=.035, mat=GALV):
    if game():
        return
    d = sub(p1, p0)
    cnt = max(2, int(length(d) / pitch))
    for i in range(cnt + 1):
        k.box(add(p0, mul(d, i / cnt)), (s, s, s), mat, 'detail')


# ------------------------------------------------------------------ tubes, pipes, fittings
def tube(k, pts, r, mat=GALV, role='frame', sides=12, closed=False, caps=True, **fl):
    """Polyline tube with parallel-transport frames (no twist at vertical turns)."""
    sides = seg(sides, max(6, sides // 2))
    pts = [tuple(p) for p in pts]
    if 'access' in fl:
        fl['axis'] = [k.world(p) for p in pts]
    if closed:
        pts = pts + [pts[0]]
    n = len(pts)
    if n < 2:
        return None
    tang = []
    for i in range(n):
        a = pts[max(0, i - 1)]
        b = pts[min(n - 1, i + 1)]
        if closed and (i == 0 or i == n - 1):
            a, b = pts[-2], pts[1]
        tang.append(norm(sub(b, a)))
    t0 = tang[0]
    up = (0, 0, 1) if abs(t0[2]) < .9 else (1, 0, 0)
    u = norm(cross(t0, up))
    verts, faces = [], []
    for i, p in enumerate(pts):
        t = tang[i]
        u = norm(sub(u, mul(t, dot(u, t))))
        v = cross(t, u)
        for j in range(sides):
            a = j * math.tau / sides
            verts.append(add(p, add(mul(u, r * math.cos(a)), mul(v, r * math.sin(a)))))
    for i in range(n - 1):
        for j in range(sides):
            jj = (j + 1) % sides
            faces.append((i * sides + j, i * sides + jj, (i + 1) * sides + jj, (i + 1) * sides + j))
    if caps and not closed:
        faces.append(tuple(reversed(range(sides))))
        faces.append(tuple(range((n - 1) * sides, n * sides)))
    return k.add(verts, faces, mat, role, **fl)


def fillet(pts, bend):
    """Round every interior corner of a polyline with a circular-ish arc of radius `bend`."""
    if len(pts) < 3:
        return list(pts)
    out = [pts[0]]
    for i in range(1, len(pts) - 1):
        p0, p1, p2 = pts[i - 1], pts[i], pts[i + 1]
        d0, d1 = norm(sub(p1, p0)), norm(sub(p2, p1))
        cosang = max(-1.0, min(1.0, dot(d0, d1)))
        th = math.acos(cosang)
        if th < 1e-3:
            out.append(p1)
            continue
        t = min(bend * math.tan(th / 2), length(sub(p1, p0)) * .48, length(sub(p2, p1)) * .48)
        a, b = sub(p1, mul(d0, t)), add(p1, mul(d1, t))
        m = max(3, int(th / (math.pi / 2) * (5 if game() else 9)))
        for s in range(m + 1):
            f = s / m
            q = add(add(mul(a, (1 - f) ** 2), mul(p1, 2 * f * (1 - f))), mul(b, f * f))
            out.append(q)
    out.append(pts[-1])
    return out


def flange(k, p, d, r, t=.10, mat=GALV, bolts=True):
    d = norm(d)
    tube(k, [sub(p, mul(d, t / 2)), add(p, mul(d, t / 2))], r, mat, 'frame', sides=20)
    if bolts and not game():
        up = (0, 0, 1) if abs(d[2]) < .9 else (1, 0, 0)
        u = norm(cross(d, up))
        v = cross(d, u)
        nb = 8
        for i in range(nb):
            a = i * math.tau / nb
            q = add(p, add(mul(u, r * .8 * math.cos(a)), mul(v, r * .8 * math.sin(a))))
            k.box(add(q, mul(d, t * .6)), (.04, .04, .04), GALV, 'detail')


def pipe(k, pts, r, mat='paint.pipe', bend=None, flanges=(), end_flanges=True, sides=16, role='frame'):
    """Pipe run: filleted elbows, flanged ends and optional flanged joints at given points."""
    path = fillet(pts, bend or max(2.2 * r, .25))
    tube(k, path, r, mat, role, sides=sides)
    if end_flanges:
        flange(k, pts[0], sub(pts[1], pts[0]), r * 1.45, t=max(.06, r * .35))
        flange(k, pts[-1], sub(pts[-1], pts[-2]), r * 1.45, t=max(.06, r * .35))
    for q, d in flanges:
        flange(k, q, d, r * 1.4, t=max(.05, r * .3))
    return path


def valve(k, p, d, r, wheel=True, mat='paint.valve'):
    """Gate valve on a pipe of radius r at p, axis d: body, bonnet, stem and handwheel."""
    d = norm(d)
    tube(k, [sub(p, mul(d, r * 1.4)), add(p, mul(d, r * 1.4))], r * 1.55, mat, 'frame', sides=16)
    for s in (-1, 1):
        flange(k, add(p, mul(d, s * r * 1.5)), d, r * 1.75, t=max(.05, r * .3), mat=mat)
    up = (0, 0, 1) if abs(d[2]) < .9 else (1, 0, 0)
    top = add(p, mul(up, r * 3.2))
    tube(k, [add(p, mul(up, r * 1.2)), add(p, mul(up, r * 2.4))], r * .75, mat, 'frame', sides=12)
    tube(k, [add(p, mul(up, r * 2.4)), top], r * .14, GALV, 'frame', sides=6)
    if wheel:
        u = norm(cross(up, d)) if abs(dot(up, d)) < .99 else (1, 0, 0)
        v = norm(cross(up, u))
        R = r * 1.5
        ring = [add(top, add(mul(u, R * math.cos(a * math.tau / 16)), mul(v, R * math.sin(a * math.tau / 16)))) for a in range(16)]
        tube(k, ring, max(.018, r * .1), 'paint.accent', 'frame', sides=6, closed=True)
        for a in range(3):
            q = add(top, add(mul(u, R * math.cos(a * math.tau / 3)), mul(v, R * math.sin(a * math.tau / 3))))
            tube(k, [top, q], max(.012, r * .06), 'paint.accent', 'frame', sides=5)


def pipe_support(k, x, y, z_top, r_pipe, w=.6, mat=STEEL):
    """Sleeper/saddle under a horizontal pipe: concrete footing, steel post and saddle."""
    k.box((x, y, .15), (w + .3, w + .3, .3), 'ferro', 'mass', ch=.06, top=.05)
    zc = z_top          # callers pass the pipe's underside: the saddle carries the pipe, no gap
    if zc > .5:
        k.box((x, y, (zc + .3) / 2), (.22, .22, zc - .3), mat, 'frame')
    k.box((x, y, zc - .05), (.2, w, .1), mat, 'frame')


# ------------------------------------------------------------------ steel sections
def hbeam(k, a, b, d=.30, w=.20, mat=STEEL, role='frame', up=None):
    """H/I section from a to b (web along `up`)."""
    t = norm(sub(b, a))
    up = up or ((0, 0, 1) if abs(t[2]) < .9 else (1, 0, 0))
    u = norm(cross(t, up))
    v = cross(u, t)
    tf, tw = max(.02, d * .09), max(.015, w * .08)
    if game():
        return k.add(*box_along(a, b, u, v, w, d), mat, role)
    for off, ww, dd in ((d / 2 - tf / 2, w, tf), (-d / 2 + tf / 2, w, tf), (0, tw, d - 2 * tf)):
        aa, bb = add(a, mul(v, off)), add(b, mul(v, off))
        k.add(*box_along(aa, bb, u, v, ww, dd), mat, role)


def box_along(a, b, u, v, w, h):
    verts = []
    for p in (a, b):
        for s1, s2 in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
            verts.append(add(p, add(mul(u, s1 * w / 2), mul(v, s2 * h / 2))))
    faces = [(3, 2, 1, 0), (4, 5, 6, 7)]
    for i in range(4):
        j = (i + 1) % 4
        faces.append((i, j, 4 + j, 4 + i))
    return verts, faces


def angle_brace(k, a, b, s=.09, mat=STEEL):
    return k.beam(a, b, s, mat, 'frame', h=s * .6)


# ------------------------------------------------------------------ access: ladders, rails, platforms, stairs
# Owner 2026-10-24: ladders and stairs are a PATH from one walkable surface to another, landing at an
# opening in that level's railing; cages start ~2.3 m up and stop at the landing; rungs ~0.18 m off the
# climbed surface; nothing passes through anything. Every access part is tagged (access=..., aid=...)
# and carries its world-space centreline (axis) so intent.clearance() can verify it.
def _aid(k):
    k.meta['_aid'] = k.meta.get('_aid', 0) + 1
    return k.meta['_aid']


def _access(k, entry):
    k.meta.setdefault('access', []).append(entry)


def abeam(k, a, b, w, mat, h=None, **fl):
    """Beam with its world centreline recorded (for clearance checks)."""
    return k.beam(a, b, w, mat, 'frame', h=h, axis=[k.world(a), k.world(b)], **fl)


def ladder(k, x, y, face_yaw, z0, z1, w=.46, off=.18, cage=True, mat=GALV, cage_mat=None, ext=1.07, reach=None,
           standoff=True, name='ladder'):
    """Fixed ladder: (x, y) is the point on the climbed face, outward = face_yaw. Stiles run from the lower
    walkable surface z0 to 1.07 m above the landing z1 (walk-through), cage hoops from z0+2.3 to the landing,
    stand-off brackets back to the structure (`reach`, default the stand-off). Records an access path."""
    cage_mat = cage_mat or mat
    aid = _aid(k)
    reach = off if reach is None else reach
    tag = dict(access='ladder', aid=aid)
    with k.at((x, y, 0), yaw=face_yaw + math.pi / 2):
        # local: outward -Y, climbed face at y=0, ladder along Z
        top = z1 + ext
        for sx in (-w / 2, w / 2):
            abeam(k, (sx, -off, z0), (sx, -off, top), .065, mat, h=.02, **tag)
            for z in frange(z0 + .9, z1 - .2, 2.0):
                k.beam((sx, -off + .02, z), (sx, -off + reach, z), .05, mat, 'frame', h=.012, access='bracket', aid=aid)
        for z in frange(z0 + .3, z1 - .05, .3):
            tube(k, [(-w / 2, -off, z), (w / 2, -off, z)], .016, mat, 'frame', sides=6, caps=False, **tag)
        if cage and z1 - z0 > 3.0:
            R = .38
            hoops = frange(z0 + 2.3, z1, .9)
            if hoops and z1 - hoops[-1] > .25:
                hoops.append(z1)
            for z in hoops:
                arc = [(R * math.cos(a), -off + R * math.sin(a), z) for a in [math.pi * (1 + i / 10) for i in range(11)]]
                tube(k, arc, .014, cage_mat, 'frame', sides=5, caps=False, **tag)
            if len(hoops) > 1:
                for i in range(5):
                    a = math.pi * (1 + i / 4)
                    px, py = R * math.cos(a), -off + R * math.sin(a)
                    abeam(k, (px, py, hoops[0]), (px, py, hoops[-1]), .04, cage_mat, h=.008, **tag)
        bottom, topp = k.world((0, -off, z0)), k.world((0, -off, z1))
        o = k.world((0, -1, 0))
        org = k.world((0, 0, 0))
        out = norm((o[0] - org[0], o[1] - org[1], 0))
    _access(k, dict(kind='ladder', aid=aid, name=name, bottom=bottom, top=topp, step=mul(out, -1), out=out, off=off,
                    standoff=standoff))
    return aid


def frange(a, b, step):
    out = []
    x = a
    while x <= b + 1e-6:
        out.append(x)
        x += step
    return out


def _runs(P, gaps, closed):
    """Resample a polyline and cut it at every gap point (0.5 m either side): list of open runs."""
    if not gaps:
        return [P + ([P[0]] if closed else [])]
    Q = P + ([P[0]] if closed else [])
    pts = []
    for a, b in zip(Q, Q[1:]):
        L = math.hypot(b[0] - a[0], b[1] - a[1])
        n = max(1, math.ceil(L / .15))
        pts += [(a[0] + (b[0] - a[0]) * i / n, a[1] + (b[1] - a[1]) * i / n) for i in range(n)]
    pts.append(Q[-1])
    keep = [all(math.hypot(p[0] - g[0], p[1] - g[1]) > .5 for g in gaps) for p in pts]
    runs, cur = [], []
    for p, ok in zip(pts, keep):
        if ok:
            cur.append(p)
        elif cur:
            runs.append(cur)
            cur = []
    if cur:
        runs.append(cur)
    if closed and len(runs) > 1 and keep[0] and keep[-1]:
        runs[0] = runs.pop() + runs[0][1:]
    # simplify collinear resample points back to corners
    out = []
    for r in runs:
        if len(r) < 2:
            continue
        s = [r[0]]
        for i in range(1, len(r) - 1):
            a, b, c = s[-1], r[i], r[i + 1]
            if abs((b[0] - a[0]) * (c[1] - b[1]) - (b[1] - a[1]) * (c[0] - b[0])) > 1e-4:
                s.append(b)
        s.append(r[-1])
        out.append(s)
    return out


def railing(k, pts, z, h=1.1, post=1.6, mat=RAIL, closed=False, toe=True, posts=True, gaps=()):
    """Handrail + knee rail + posts (+ toe board) along a plan polyline at deck height z.
    gaps: plan points where the rail opens for a ladder or stair head (0.5 m each side, end posts kept)."""
    aid = _aid(k)
    P = [(p[0], p[1]) for p in pts]
    runs = _runs(P, list(gaps), closed)
    full_loop = closed and not gaps
    for R in runs:
        tag = dict(access='rail', aid=aid)
        tube(k, [(x, y, z + h) for x, y in R], .028, mat, 'frame', sides=8, caps=not full_loop, **tag)
        tube(k, [(x, y, z + h * .5) for x, y in R], .02, mat, 'frame', sides=6, caps=not full_loop, **tag)
        for i, (a, b) in enumerate(zip(R, R[1:])):
            L = math.hypot(b[0] - a[0], b[1] - a[1])
            if toe:
                k.beam((a[0], a[1], z + .05), (b[0], b[1], z + .05), .012, GALV, 'frame', h=.10, access='post', aid=aid)
            if posts:
                nposts = max(1, math.ceil(L / post))
                last = i == len(R) - 2 and not full_loop
                for j in range(nposts + (1 if last else 0)):
                    f = j / nposts
                    x, y = a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f
                    tube(k, [(x, y, z), (x, y, z + h)], .024, mat, 'frame', sides=6, access='post', aid=aid)
    return aid


def circle_pts(c, r, a0=0.0, a1=math.tau, n=None):
    n = n or max(8, int(abs(a1 - a0) * r / .6))
    return [(c[0] + r * math.cos(a0 + (a1 - a0) * i / n), c[1] + r * math.sin(a0 + (a1 - a0) * i / n)) for i in range(n + (0 if abs(a1 - a0 - math.tau) < 1e-6 else 1))]


def _platform(k, kind):
    pid = len(k.meta.setdefault('platforms', [])) + 1
    k.meta['platforms'].append(dict(pid=pid, kind=kind))
    return pid


def ring_platform(k, c, r_in, r_out, z, a0=0.0, a1=math.tau, brackets=8, rail=True, deck='grate', gaps=(), name='ring platform'):
    """Circular (or arc) catwalk on knee-braced brackets off a vessel wall, railed outer edge (and arc ends),
    with rail openings at `gaps` (plan points). Returns the platform id."""
    pid = _platform(k, name)
    full = abs(a1 - a0 - math.tau) < 1e-6
    lathe(k, [(r_in, z - .07), (r_out, z - .07), (r_out, z), (r_in, z)], deck, 'frame', n=64, c=c,
          a0=a0, a1=None if full else a1, cap=full, walk=True, platform=pid)
    if not full:
        for a in (a0, a1):
            p0, p1 = polar(c, r_in, a, z - .035), polar(c, r_out, a, z - .035)
            k.beam(p0, p1, .05, STEEL, 'frame', h=.07)
    lathe(k, [(r_out, z - .12), (r_out + .015, z - .12), (r_out + .015, z + .09), (r_out, z + .09)], STEEL, 'frame',
          n=64, c=c, a0=a0, a1=None if full else a1, cap=False)
    for i in range(brackets):
        a = a0 + (a1 - a0) * (i + (.5 if full else 0)) / (brackets - (0 if full else 1) or 1)
        k.beam(polar(c, r_in - .02, a, z - .1), polar(c, r_out - .05, a, z - .1), .08, STEEL, 'frame', h=.1)
        k.beam(polar(c, r_in - .02, a, z - .9), polar(c, r_out - .12, a, z - .14), .07, STEEL, 'frame', h=.07)
    if rail:
        pts = circle_pts(c, r_out - .05, a0, a1)
        if not full:   # close the arc ends back to the shell
            pts = [polar(c, r_in + .1, a0, 0)[:2]] + pts + [polar(c, r_in + .1, a1, 0)[:2]]
        railing(k, pts, z, closed=full, gaps=gaps)
    return pid


def rect_platform(k, x0, y0, x1, y1, z, rail_sides='nesw', legs=True, deck='grate', gaps=(), name='platform'):
    """Grated platform on a steel frame; rail_sides subset of 'nesw' (n=+Y, s=-Y); rail openings at `gaps`."""
    pid = _platform(k, name)
    k.box(((x0 + x1) / 2, (y0 + y1) / 2, z - .04), (x1 - x0, y1 - y0, .08), deck, 'frame', walk=True, platform=pid)
    for a, b in (((x0, y0), (x1, y0)), ((x1, y0), (x1, y1)), ((x1, y1), (x0, y1)), ((x0, y1), (x0, y0))):
        k.beam((a[0], a[1], z - .16), (b[0], b[1], z - .16), .10, STEEL, 'frame', h=.18)
    if legs:
        for x in (x0 + .1, x1 - .1):
            for y in (y0 + .1, y1 - .1):
                hbeam(k, (x, y, 0), (x, y, z - .25), .18, .18)
    sides = {'s': [(x0, y0), (x1, y0)], 'e': [(x1, y0), (x1, y1)], 'n': [(x1, y1), (x0, y1)], 'w': [(x0, y1), (x0, y0)]}
    order = 'senw'
    chain = [s for s in order if s in rail_sides]
    for s in chain:
        railing(k, sides[s], z, gaps=gaps)
    return pid


def stair(k, a, b, w=.9, mat=STEEL, rail_mat=RAIL, name='stair'):
    """Straight steel stair from foot a=(x,y,z) to head b: channel stringers, grated treads, rails.
    The head b sits on the landing edge; the landing's railing needs a gap at b."""
    aid = _aid(k)
    d = sub(b, a)
    run = math.hypot(d[0], d[1])
    rise = d[2]
    n = max(2, round(rise / .2))
    hd = norm((d[0], d[1], 0))
    side = (-hd[1], hd[0], 0)
    for s in (-1, 1):
        o = mul(side, s * w / 2)
        abeam(k, add(a, o), add(b, o), .05, mat, h=.22, access='stair', aid=aid)
        rail = [add(add(a, o), (0, 0, .95)), add(add(b, o), (0, 0, .95))]
        tube(k, rail, .025, rail_mat, 'frame', sides=6, access='stair', aid=aid)
        for f in (.0, .5, 1.0):
            p = add(add(a, mul(d, f)), o)
            tube(k, [p, add(p, (0, 0, .95))], .022, rail_mat, 'frame', sides=6, access='post', aid=aid)
    for i in range(1, n):
        f = i / n
        p = add(a, (d[0] * f, d[1] * f, rise * f))
        c = add(p, mul(hd, run / n * .5))
        k.add(*box_along(sub(c, mul(side, w / 2 - .03)), add(c, mul(side, w / 2 - .03)), hd, (0, 0, 1), run / n * .95, .04),
              'grate', 'frame', access='post', aid=aid)
    _access(k, dict(kind='stair', aid=aid, name=name, bottom=k.world(a), top=k.world(b),
                    step=norm(sub(k.world(add(b, hd)), k.world(b))), standoff=False))
    return aid


def helix_stair(k, c, R, z0, z1, a0, turn=1.0, w=.85, rail_mat=RAIL, name='spiral stair'):
    """Tank-wall spiral stair: treads on knee-braced wall brackets, inner/outer stringers, outer handrail."""
    aid = _aid(k)
    rise = z1 - z0
    n = max(4, round(rise / .21))
    arc = turn * math.tau
    ri, ro = R + .05, R + .05 + w
    inner, outer, rail = [], [], []
    for i in range(n + 1):
        f = i / n
        a = a0 + arc * f
        z = z0 + rise * f
        inner.append(polar(c, ri, a, z - .12))
        outer.append(polar(c, ro, a, z - .12))
        rail.append(polar(c, ro, a, z + .95))
        if i < n:
            am = a + arc / n * .5
            lathe(k, [(ri, z - .03), (ro, z - .03), (ro, z + .02), (ri, z + .02)], 'grate', 'frame', n=6, c=c,
                  a0=a, a1=a + arc / n * .9, cap=False, access='post', aid=aid)
            if i % 4 == 0:
                tube(k, [polar(c, ro, am, z), polar(c, ro, am, z + .95)], .022, rail_mat, 'frame', sides=6, access='post', aid=aid)
                k.beam(polar(c, R - .02, am, z - .5), polar(c, ro, am, z - .1), .06, STEEL, 'frame', h=.06, access='post', aid=aid)
    step = max(1, n // 24) if game() else 1
    tag = dict(access='stair', aid=aid)
    tube(k, inner[::step] + [inner[-1]], .04, STEEL, 'frame', sides=6, **tag)
    tube(k, outer[::step] + [outer[-1]], .05, STEEL, 'frame', sides=6, **tag)
    tube(k, rail[::step] + [rail[-1]], .028, rail_mat, 'frame', sides=6, **tag)
    tube(k, [(p[0], p[1], p[2] - .48) for p in rail[::step] + [rail[-1]]], .02, rail_mat, 'frame', sides=6, **tag)
    a1 = a0 + arc
    mid = (ri + ro) / 2
    tan = (-math.sin(a1) * (1 if arc > 0 else -1), math.cos(a1) * (1 if arc > 0 else -1), 0)
    _access(k, dict(kind='stair', aid=aid, name=name, bottom=k.world(polar(c, mid, a0, z0)), top=k.world(polar(c, mid, a1, z1)),
                    step=tan, standoff=False))
    return aid


def walkway(k, pts, w=.9, rails=(1, -1), mat='grate', lift=.12):
    """Grated walkway along a surface polyline (3D points ON the surface), lifted clear of it, with
    handrails that follow its slope on the listed sides (+1 = left of travel, -1 = right)."""
    aid = _aid(k)
    P = [(p[0], p[1], p[2] + lift) for p in pts]
    for a_, b_ in zip(P, P[1:]):
        d = norm((b_[0] - a_[0], b_[1] - a_[1], 0))
        k.add(*box_along(a_, b_, (-d[1], d[0], 0), (0, 0, 1), w, .06), mat, 'frame', walk=True)
    for sd in rails:
        line = []
        for i, q in enumerate(P):
            a_, b_ = P[max(0, i - 1)], P[min(len(P) - 1, i + 1)]
            d = norm((b_[0] - a_[0], b_[1] - a_[1], 0))
            o = (-d[1] * sd * (w / 2 - .03), d[0] * sd * (w / 2 - .03), 0)
            line.append(add(q, o))
            tube(k, [add(add(q, o), (0, 0, .03)), add(add(q, o), (0, 0, 1.05))], .024, RAIL, 'frame', sides=6, access='post', aid=aid)
        tube(k, [add(q, (0, 0, 1.05)) for q in line], .028, RAIL, 'frame', sides=8, access='rail', aid=aid)
        tube(k, [add(q, (0, 0, .55)) for q in line], .02, RAIL, 'frame', sides=6, access='rail', aid=aid)
    return aid


def lattice(k, c, w, z0, z1, panel=2.4, mat=STEEL, legs=.16):
    """Four-leg lattice tower/frame: corner angles, X bracing per panel, ring struts."""
    x, y = c
    pts = [(x - w / 2, y - w / 2), (x + w / 2, y - w / 2), (x + w / 2, y + w / 2), (x - w / 2, y + w / 2)]
    for px, py in pts:
        hbeam(k, (px, py, z0), (px, py, z1), legs, legs)
    zs = frange(z0, z1, panel)
    if zs[-1] < z1 - .3:
        zs.append(z1)
    for za, zb in zip(zs, zs[1:]):
        for (ax, ay), (bx, by) in zip(pts, pts[1:] + pts[:1]):
            angle_brace(k, (ax, ay, zb), (bx, by, zb))
            angle_brace(k, (ax, ay, za), (bx, by, zb), s=.06)
            angle_brace(k, (bx, by, za), (ax, ay, zb), s=.06)


# ------------------------------------------------------------------ roof & wall equipment
def mushroom_vent(k, x, y, z, r=.25):
    k.cyl((x, y, z + .35), r * .55, .7, GALV, 'frame', sides=16)
    lathe(k, [(r * .5, z + .7), (r * 1.25, z + .72), (r * 1.2, z + .82), (r * .3, z + .95), (0.0, z + .97)], GALV, 'frame', n=20, c=(x, y))


def roof_fan(k, x, y, z, r=.8):
    """Turret ventilator: square curb, round fan housing, grille, rain cap."""
    k.box((x, y, z + .2), (2 * r + .4, 2 * r + .4, .4), 'paint.second', 'frame', ch=.06, top=.04)
    lathe(k, [(r, z + .4), (r, z + 1.0), (r * 1.06, z + 1.04), (r * 1.06, z + 1.12), (r * .92, z + 1.12)], 'paint.second', 'frame', n=32, c=(x, y))
    lathe(k, [(r * .92, z + 1.06), (0.0, z + 1.06)], 'grate', 'frame', n=32, c=(x, y))
    k.cyl((x, y, z + 1.14), .14, .16, STEEL, 'detail', sides=12)
    if not game():
        for i in range(8):
            a = i * math.tau / 8
            k.beam((x, y, z + 1.08), (x + r * .9 * math.cos(a), y + r * .9 * math.sin(a), z + 1.08), .04, STEEL, 'detail', h=.02)


def louvre_box(k, x, y, z, w, d, h, mat='paint.second', face='s'):
    """HVAC/ventilator housing with a louvred face and a lifting-lug lid."""
    k.box((x, y, z + h / 2), (w, d, h), mat, 'mass', ch=.05, top=.06)
    k.box((x, y, z + h + .06), (w + .12, d + .12, .12), 'paint.trim', 'frame', ch=.05)
    yf = y - d / 2 if face == 's' else y + d / 2
    s = -1 if face == 's' else 1
    nl = max(3, int((h - .4) / .16))
    for i in range(nl):
        zz = z + .22 + i * (h - .4) / nl
        k.slab(x - w * .42, x + w * .42, zz, zz + .07, yf + s * .06, yf - s * .01, 'grate', 'frame') if s < 0 else \
            k.slab(x - w * .42, x + w * .42, zz, zz + .07, yf - .01, yf + .06, 'grate', 'frame')
    bolt_row(k, (x - w / 2 + .05, yf + s * .02, z + h - .06), (x + w / 2 - .05, yf + s * .02, z + h - .06), .35)


def hatch(k, x, y, z, s=.9):
    k.box((x, y, z + .1), (s + .16, s + .16, .2), 'paint.trim', 'frame', ch=.04)
    k.box((x, y, z + .24), (s, s, .08), 'paint.second', 'frame', ch=.06, top=.03)
    k.box((x + s * .3, y, z + .3), (.06, .3, .05), GALV, 'detail')


def gauge_board(k, x, y, z, face_yaw):
    with k.at((x, y, z), yaw=face_yaw + math.pi / 2):
        k.box((0, -.08, 0), (.5, .16, .7), 'paint.second', 'frame', ch=.03, top=.02)
        k.cyl((0, -.17, .12), .11, .04, 'glass', 'detail', sides=12)
        k.cyl((0, -.17, -.15), .07, .04, 'glass', 'detail', sides=10)


def lamp(k, x, y, z, face_yaw, sockets=None, name='door'):
    """Bulkhead lamp on a short arm (records a door light socket)."""
    with k.at((x, y, z), yaw=face_yaw + math.pi / 2):
        k.box((0, -.10, 0), (.30, .2, .12), 'paint.trim', 'frame', ch=.03)
        k.box((0, -.16, -.08), (.22, .14, .05), 'lamp', 'frame')
    if sockets is not None:
        dx, dy = math.cos(face_yaw), math.sin(face_yaw)
        sockets.append(dict(name=f'LIGHT_{name}_{len(sockets) + 1:02d}', position_m=(x + dx * .2, y + dy * .2, z - .1),
                            direction=(dx * .6, dy * .6, -1), type='spot', intensity_class='door'))


# ------------------------------------------------------------------ building shells
def wall_frame(k, x0, x1, z0, z1, yf, holes=(), mat='brick.main', t=.45, role='mass', recess=(), recess_d=.14, cut=()):
    """Masonry/cast wall face in local wall coords (face at yf, outward -Y) cut around holes.
    recess: blind panels set back recess_d (brick infill, no coplanar overlays)."""
    for a in subtract_rect((x0, z0, x1, z1), list(holes) + list(recess) + list(cut)):
        k.slab(a[0], a[2], a[1], a[3], yf, yf + t, mat, role)
    for h in recess:
        k.slab(h[0], h[2], h[1], h[3], yf + recess_d, yf + t, mat, role)
    for h in holes:   # reveals: dark returns behind each opening
        k.slab(h[0], h[2], h[1], h[3], yf + t * .7, yf + t * .8, 'paint.dark', 'mass')


def steel_window(k, x0, x1, z0, z1, yf, cols=None, rows=None, frame='paint.trim', sill='ferro', lintel='ferro'):
    """Industrial steel-framed window: muntin grid, glazing set back, cast sill and lintel."""
    cols = cols or max(2, round((x1 - x0) / .55))
    rows = rows or max(2, round((z1 - z0) / .55))
    gy = yf + .16
    k.slab(x0, x1, z0, z1, gy + .01, gy + .03, 'glass', 'glass', pane=True)
    for i in range(cols + 1):
        x = x0 + (x1 - x0) * i / cols
        k.slab(x - .03, x + .03, z0, z1, gy - .04, gy + .01, frame, 'frame')
    for j in range(rows + 1):
        z = z0 + (z1 - z0) * j / rows
        k.slab(x0, x1, z - .03, z + .03, gy - .04, gy + .01, frame, 'frame')
    k.slab(x0 - .08, x1 + .08, z0 - .16, z0, yf - .08, yf + .25, sill, 'frame', front=.03)
    k.slab(x0 - .12, x1 + .12, z1, z1 + .3, yf - .03, yf + .2, lintel, 'frame', front=.02)


def personnel_door(k, xc, w, h, yf, leaves=1, mat='paint.door', canopy=True, apron=True):
    x0, x1 = xc - w / 2, xc + w / 2
    k.slab(x0 - .12, x0, 0, h + .12, yf - .06, yf + .3, 'paint.trim', 'frame', front=.02)
    k.slab(x1, x1 + .12, 0, h + .12, yf - .06, yf + .3, 'paint.trim', 'frame', front=.02)
    k.slab(x0 - .12, x1 + .12, h, h + .14, yf - .06, yf + .3, 'paint.trim', 'frame', front=.02)
    lw = w / leaves
    for i in range(leaves):
        a, b = x0 + i * lw + .02, x0 + (i + 1) * lw - .02
        k.slab(a, b, .02, h - .02, yf + .12, yf + .18, mat, 'skin', front=.015, door=True, feature='door')
        k.slab(a + .12, b - .12, h * .62, h * .85, yf + .105, yf + .125, 'glass', 'glass')
        k.box((b - .14 if i == 0 else a + .14, yf + .1, h * .48), (.04, .06, .18), GALV, 'detail')
        k.slab(a + .1, b - .1, .15, .32, yf + .108, yf + .122, 'grate', 'detail')
    if apron:
        k.slab(x0 - .3, x1 + .3, -.0, .16, yf - .9, yf + .02, 'ferro', 'mass', front=.03, walk=True)   # step/apron
    if canopy:
        k.slab(x0 - .35, x1 + .35, h + .35, h + .47, yf - .85, yf + .02, 'paint.second', 'frame', front=.03)
        for x in (x0 - .25, x1 + .25):
            k.beam((x, yf - .8, h + .37), (x, yf, h + .9), .05, STEEL, 'frame', h=.05)


def rollup_door(k, xc, w, h, yf, hood=True, grade=0.0, civil=False):
    """civil=True: the clean civilian door (no hazard guards, no bollards)."""
    x0, x1 = xc - w / 2, xc + w / 2
    # steel jamb guards with chevrons, slatted curtain recessed, hood box
    for x in (x0 - .18, x1 + .18):
        k.slab(x - .18, x + .18, 0, h + .1, yf - .12, yf + .3, 'paint.trim', 'frame', front=.03)
        if not civil:
            k.slab(x - .19, x + .19, 0, 1.2, yf - .13, yf - .11, 'hazard', 'skin')
    nsl = max(6, int(h / .22))
    for i in range(nsl):
        z0 = i * h / nsl
        k.slab(x0, x1, z0 + .01, z0 + h / nsl - .01, yf + .14, yf + .2, 'paint.door', 'skin', front=.012, feature='rollup' if i == 0 else None)
    for j in range(4):   # vision slots
        xx = x0 + w * (.2 + .2 * j)
        k.slab(xx - .22, xx + .22, h * .55, h * .62, yf + .125, yf + .14, 'glass', 'glass')
    if hood:
        k.box((xc, yf - .2, h + .45), (w + .8, .5, .7), 'paint.second', 'frame', ch=.06, top=.05)
    k.slab(x0 - .4, x1 + .4, 0, .05, yf - 1.6, yf + .02, 'ferro', 'mass')
    for x in (() if civil else (x0 - .7, x1 + .7)):   # bollards
        k.cyl((x, yf - .7, .55), .13, 1.1, 'hazard', 'frame', sides=14, top=.04)
        if grade > .05:            # door on a raised plinth: the bollard footing goes down to grade (round 6 check)
            k.box((x, yf - .7, -grade / 2 + .01), (.4, .4, grade + .02), 'ferro', 'mass', ch=.04)


def downpipe(k, x, y, z0, z1, face_yaw, off=.18):
    dx, dy = math.cos(face_yaw) * off, math.sin(face_yaw) * off
    tube(k, [(x + dx, y + dy, z0), (x + dx, y + dy, z1)], .07, 'paint.second', 'frame', sides=10)
    for z in frange(z0 + 1.0, z1 - .4, 2.0):
        k.box((x + dx * .55, y + dy * .55, z), (.12, .12, .05), STEEL, 'detail')


def wall_pipe(k, pts, r, face_yaw, mat='paint.pipe', clip=2.2):
    """Pipe run on wall brackets (bracket arms back to the wall plane)."""
    pipe(k, pts, r, mat)
    dx, dy = -math.cos(face_yaw), -math.sin(face_yaw)
    for a, b in zip(pts, pts[1:]):
        L = length(sub(b, a))
        for i in range(1, max(1, int(L / clip)) + 1):
            p = add(a, mul(sub(b, a), i / (int(L / clip) + 1)))
            k.beam(p, (p[0] + dx * (r + .25), p[1] + dy * (r + .25), p[2]), .06, STEEL, 'frame', h=.06)
