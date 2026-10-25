"""Stock selections built in construction order (Opus art bar).

RULE 1: frame first, then skin. Metal buildings: slab/footing -> columns, ribs,
rafters -> girts and purlins -> separate sheets lapped over that frame -> trim at
corners, eaves and ridge -> fastener rows on the girts. Timber: posts and rails,
then boards. Partial damage peels skin off the frame; rubble is a bent frame
and fallen skin resting on a solid mound. See E:/mc2-ref/scifi-kit/ART-BAR.md.

No source mesh, texture, UV or vertex data is read here. Dimensions are catalog
bounds / 4 (authoring metres, Z up, front -Y); references donate detail only.
"""
import math
import random

import geo
from geo import Kit, rect_poly

SIZES = {
    'razorwirefence': (2.15365, 10.77768, 6.83865),
    's_lightwall': (5.04235, 34.52278, 5.57268),
    'rpbuilding': (30.82582, 26.51732, 8.5168),
    'quonsetclean': (10.52915, 21.05695, 5.26457),
    'crateswithtarp': (10.6255, 3.77875, 4.24323),
    'lookouttower': (9.98818, 9.07228, 18.6375),
    'barrels': (1.81450, 3.69078, 1.73127),
    'quonset': (10.52915, 21.05695, 5.26457),
}
FIT_CACHE = {}
BUILDINGS = {'rpbuilding', 'quonsetclean', 'quonset', 'lookouttower'}


def C(x):
    """Chamfer only on the high source; the game carrier gets its bevel in bake."""
    return 0.0 if geo.GAME_MESH else x


# ------------------------------------------------------------ construction helpers
def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def _unit(a):
    n = math.sqrt(sum(c * c for c in a)) or 1.0
    return tuple(c / n for c in a)


def sweep(k, a, b, profile, mat, role='frame', **fl):
    """Extrude a closed (u, v) profile along the straight member a -> b."""
    t = _unit([b[i] - a[i] for i in range(3)])
    up = (0, 0, 1) if abs(t[2]) < .95 else (1, 0, 0)
    u = _unit(_cross(t, up))
    v = _cross(t, u)
    n = len(profile)
    verts = [tuple(p[i] + pu * u[i] + pv * v[i] for i in range(3)) for p in (a, b) for pu, pv in profile]
    faces = [tuple(reversed(range(n))), tuple(range(n, 2 * n))]
    faces += [(i, (i + 1) % n, (i + 1) % n + n, i + n) for i in range(n)]
    return k.add(verts, faces, mat, role, **fl)


def ibeam(k, a, b, d=.30, w=.16, mat='metal', role='frame', **fl):
    """Rolled I-section member (a box on the game carrier)."""
    if geo.GAME_MESH:
        return k.beam(a, b, w, mat, role, h=d, **fl)
    tf, tw, hw, hd = max(.018, d * .08), max(.012, w * .09), w / 2, d / 2
    prof = [(-hw, -hd), (hw, -hd), (hw, -hd + tf), (tw / 2, -hd + tf), (tw / 2, hd - tf), (hw, hd - tf),
            (hw, hd), (-hw, hd), (-hw, hd - tf), (-tw / 2, hd - tf), (-tw / 2, -hd + tf), (-hw, -hd + tf)]
    return sweep(k, a, b, prof, mat, role, **fl)


def cee(k, a, b, d=.20, w=.07, mat='metal', role='frame', **fl):
    """Cold-formed C girt/purlin."""
    if geo.GAME_MESH:
        return k.beam(a, b, w, mat, role, h=d, **fl)
    t, hd = .012, d / 2
    prof = [(0, -hd), (w, -hd), (w, -hd + t), (t, -hd + t), (t, hd - t), (w, hd - t), (w, hd), (0, hd)]
    return sweep(k, a, b, prof, mat, role, **fl)


def bolts(k, p0, p1, pitch=.45, s=.035, mat='metal'):
    """Fastener row: detail role, baked into the game atlas, not kept as geometry."""
    if geo.GAME_MESH:
        return
    n = max(1, int(math.dist(p0, p1) / pitch))
    for i in range(n + 1):
        f = i / n
        k.box(tuple(p0[j] + (p1[j] - p0[j]) * f for j in range(3)), (s, s, s), mat, 'detail')


def loft(k, r0, z0, r1, z1, mat, role='mass', ch0=0.0, ch1=None, **fl):
    """Tapered block from rect r0=(x0,y0,x1,y1) at z0 to rect r1 at z1."""
    ch1 = ch0 if ch1 is None else ch1
    b = rect_poly(*r0, C(ch0))
    t = rect_poly(*r1, C(ch1))
    if len(b) != len(t):
        b, t = rect_poly(*r0), rect_poly(*r1)
    n = len(b)
    vv = [(x, y, z0) for x, y in b] + [(x, y, z1) for x, y in t]
    ff = [tuple(reversed(range(n))), tuple(range(n, 2 * n))] + [(i, (i + 1) % n, (i + 1) % n + n, i + n) for i in range(n)]
    return k.add(vv, ff, mat, role, **fl)


def hexa(k, front, back, mat, role='skin', **fl):
    """Closed six-sided solid from a front quad and a back quad (same vertex order)."""
    vv = list(front) + list(back)
    ff = [(3, 2, 1, 0), (4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)]
    return k.add(vv, ff, mat, role, **fl)


def lap_board(k, x0, x1, z0, z1, mat, lap=.06, kick=.035, t=.03, **fl):
    """Horizontal lap board in a wall plane (front -y): the bottom edge kicks out
    over the board below; the top edge tucks behind the board above."""
    front = [(x0, -kick, z0), (x1, -kick, z0), (x1, 0, z1 + lap), (x0, 0, z1 + lap)]
    back = [(x, y + t, z) for x, y, z in front]
    return hexa(k, front, back, mat, 'skin', **fl)


def plate(k, x0b, x1b, x0t, x1t, z0, z1, yf, yb, mat, role='skin', **fl):
    """Trapezoid plate in a wall plane: bottom x0b..x1b at z0, top x0t..x1t at z1."""
    front = [(x0b, yf, z0), (x1b, yf, z0), (x1t, yf, z1), (x0t, yf, z1)]
    back = [(x, yb, z) for x, _, z in front]
    return hexa(k, front, back, mat, role, **fl)


def arc_sheet(k, R, th0, th1, y0, y1, zc, mat='corr.corr', role='roof', amp=.035, pitch=.152, **fl):
    """Curved corrugated sheet. Waves run around the arch and are phased in
    absolute Y, so two lapped sheets nest instead of intersecting."""
    game = geo.GAME_MESH
    nth = max(3, int(round((th1 - th0) / math.pi * (20 if game else 72))))
    if game:
        ys, a = [y0, y1], 0.0
    else:
        ny = max(2, int((y1 - y0) / pitch * 4) + 1)
        ys, a = [y0 + (y1 - y0) * j / (ny - 1) for j in range(ny)], amp
    t = .012
    verts, faces = [], []
    rows, stride = len(ys), nth + 1
    for off in (0.0, -t):
        for y in ys:
            rr = R + off + a * (.5 + .5 * math.sin(y * math.tau / pitch))
            for i in range(nth + 1):
                th = th0 + (th1 - th0) * i / nth
                verts.append((rr * math.cos(th), y, zc + rr * math.sin(th)))
    lay = stride * rows
    for j in range(rows - 1):
        for i in range(nth):
            q = j * stride + i
            faces.append((q, q + stride, q + stride + 1, q + 1))
            faces.append((lay + q, lay + q + 1, lay + q + stride + 1, lay + q + stride))
    for j in range(rows - 1):
        for i in (0, nth):
            q, c = j * stride + i, (j + 1) * stride + i
            f = (q, lay + q, lay + c, c)
            faces.append(f if i == 0 else tuple(reversed(f)))
    for j in (0, rows - 1):
        for i in range(nth):
            q = j * stride + i
            f = (q, q + 1, lay + q + 1, lay + q)
            faces.append(f if j == 0 else tuple(reversed(f)))
    return k.add(verts, faces, mat, role, **fl)


def arc_member(k, R, y, zc, d=.24, w=.14, seg=None, mat='metal'):
    seg = seg or (10 if geo.GAME_MESH else 28)
    pts = [(R * math.cos(math.pi * i / seg), y, zc + R * math.sin(math.pi * i / seg)) for i in range(seg + 1)]
    for a, b in zip(pts, pts[1:]):
        ibeam(k, a, b, d=d, w=w, mat=mat)


def wire(k, pts, r, mat='metal', role='skin', sides=6, **fl):
    """Polyline tube without game-mesh subsampling (helices must survive)."""
    sides = min(sides, 4) if geo.GAME_MESH else sides
    verts, faces = [], []
    n = len(pts)
    for i, p in enumerate(pts):
        a, b = pts[max(0, i - 1)], pts[min(n - 1, i + 1)]
        t = _unit([b[j] - a[j] for j in range(3)])
        up = (0, 0, 1) if abs(t[2]) < .95 else (1, 0, 0)
        u = _unit(_cross(t, up))
        v = _cross(t, u)
        for s in range(sides):
            ang = s * math.tau / sides
            verts.append(tuple(p[j] + r * (math.cos(ang) * u[j] + math.sin(ang) * v[j]) for j in range(3)))
    faces.append(tuple(reversed(range(sides))))
    for i in range(n - 1):
        for s in range(sides):
            q = (s + 1) % sides
            faces.append((i * sides + s, i * sides + q, (i + 1) * sides + q, (i + 1) * sides + s))
    faces.append(tuple(range((n - 1) * sides, n * sides)))
    return k.add(verts, faces, mat, role, **fl)


def wrapped_band(k, r, z0, z1, v0, v1, n=64, mat='hazard.wrap'):
    """Cylindrical paint band with real UVs: stripes wrap without warping."""
    n = 20 if geo.GAME_MESH else n
    vv, uv = [], []
    for z, v in ((z0, v0), (z1, v1)):
        for j in range(n + 1):
            a = math.tau * j / n
            vv.append((r * math.cos(a), r * math.sin(a), z))
            uv.append((j / n, v))
    ff = [(j, j + 1, j + n + 2, j + n + 1) for j in range(n)]
    return k.add(vv, ff, mat, 'skin', uv=uv)


def rock(k, c, size, mat, rng, flat=1.0):
    """Volumetric chunk (role mass, so it survives into the game mesh)."""
    n = 6
    phase = rng.uniform(0, math.tau)
    rad = [size / 2 * rng.uniform(.75, 1.1) for _ in range(n)]
    h = size * rng.uniform(.38, .62) * flat
    yaw, pitch, roll = rng.uniform(-math.pi, math.pi), rng.uniform(-.35, .35), rng.uniform(-.3, .3)
    poly = [(rad[i] * math.cos(phase + i * math.tau / n), rad[i] * math.sin(phase + i * math.tau / n)) for i in range(n)]
    with k.at(c, yaw=yaw, pitch=pitch, roll=roll):
        k.prism(poly, -h * .35, h * .65, mat, 'mass', top=h * .22)


# Stock wall SEGMENT cross-section (half-width in metres vs height fraction of the
# stock wall top), measured from round2/stock/<name>.json vertices_m at y < -6.
# The segment is battered (wide at the base, sloping inward), not just the end pier.
WALL_PROFILE = {
    's_lightwall': ((0, 1.88), (3.96 / 4.37, 1.62), (1, .94)),
    's_mediumwall': ((0, 2.51), (4.16 / 11.98, 2.17), (6.63 / 11.98, 1.24), (1, 1.24)),
    's_heavywall': ((0, 2.50), (6.5 / 21.5, 2.16), (10.4 / 21.5, 1.23), (1, 1.23)),
    'l_lightwall': ((0, 2.31), (1, .85)),
    'l_mediumwall': ((0, 2.77), (10.53 / 12.0, 1.16), (1, 1.16)),
    'd_mediumwall': ((0, 2.51), (1, 1.31)),
    'l_heavywall': ((0, 2.77), (17.9 / 21.3, 1.62), (1, 1.62)),
    'd_lightwall': ((0, 2.0), (1, 1.31)),
    'd_heavywall': ((0, 2.92), (1, 1.51)),
}


def wall_hw(name, t):
    """Stock wall-segment half-width at height fraction t (0..1)."""
    pts = WALL_PROFILE[name]
    t = min(max(t, 0.0), 1.0)
    for (t0, h0), (t1, h1) in zip(pts, pts[1:]):
        if t <= t1:
            return h0 + (h1 - h0) * (t - t0) / max(t1 - t0, 1e-9)
    return pts[-1][1]


# ------------------------------------------------------------ s_lightwall
def light_wall(k, y_start=-17.0, py=15.45, units=8, diag=False):
    """Footing -> precast units -> steel battens at span joints -> coping -> hardware.
    The defaults are the approved s_lightwall; s_lightwalldiag passes a longer run (the 45-degree map placement) and
    turns the terminal pier 45 degrees so it stays square to the world like the straight walls' piers."""
    W, H = 1.75, 4.65
    y_end = py - 2.0
    u = (y_end - y_start) / units
    k.box((0, (y_start + y_end) / 2, .18), (2 * wall_hw('s_lightwall', 0) + .1, y_end - y_start + .3, .36), 'ferro', 'mass', ch=C(.1), top=C(.08), part='footing')
    zt = H - .30
    for j in range(units):
        y0 = y_start + j * u
        yc = y0 + u / 2
        k.box((0, yc, (.36 + zt) / 2), (W - .08, u - .10, zt - .36), 'ferro', 'mass', ch=C(.08), top=C(.06), rebar=True, unit=j)
        for s in (-1, 1):
            # Raised cast face field: two per span, eight along the wall.
            with k.at((s * (W / 2 - .04), yc, 0), yaw=(math.pi / 2 if s > 0 else -math.pi / 2)):
                k.slab(-u / 2 + .28, u / 2 - .28, .75, zt - .35, -.05, .02, 'ferro', 'skin', front=C(.035), unit=j)
            k.box((s * (W / 2 + .005), yc, .55), (.02, .18, .06), 'rubber', 'detail', unit=j)
        for yy in (yc - u * .28, yc + u * .28):
            k.cyl((0, yy, zt + .02), .07, .05, 'metal', 'detail', sides=10, unit=j)
        k.moss_sites.append(((-W / 2 - .3, y0, .36), (-W / 2 - .3, y0 + u, .36)))
    for j in range(units // 2 + 1):
        # Steel shear battens at the span joints: flush strips, not extra posts.
        y = y_start + j * 2 * u
        k.box((0, y, (.36 + zt) / 2), (W + .05, .18, zt - .36), 'paint.second', 'frame', ch=C(.025), joint=j)
        for s in (-1, 1):
            bolts(k, (s * (W / 2 + .03), y, .8), (s * (W / 2 + .03), y, zt - .3), pitch=.6)
    for j in range(units // 2):
        yc = y_start + (j + .5) * 2 * u
        # The broad 45-degree coping profile belongs to both silhouettes.
        loft(k, (-W / 2 - .06, yc - u + .07, W / 2 + .06, yc + u - .07), zt,
             (-W / 2 + .24, yc - u + .37, W / 2 - .24, yc + u - .37), H,
             'ferro.main', 'skin', ch0=.04, ch1=.04, span=j)
    # Mild batter through the precast units and coping; the footing stays broad.
    # Stock battered wall SEGMENT: half-width 1.88 m at the base, 1.62 m at the coping
    # foot (stock profile). The coping keeps its own 45-degree taper above zt.
    for p in k.pieces:
        if p.get('part') not in ('footing', 'pier'):
            p['v'] = [(x * wall_hw('s_lightwall', .906 * min(max(z - .36, 0), zt - .36) / (zt - .36)) / (W / 2), y, z)
                      for x, y, z in p['v']]
    first_pier = len(k.pieces)
    # Stock pier: dark battered block with a pale chamfered cap.
    loft(k, (-2.5, py - 2.0, 2.5, py + 2.0), 0, (-1.85, py - 1.5, 1.85, py + 1.5), 4.6, 'paint.second', 'mass', ch0=.30, ch1=.22, rebar=True, part='pier')
    k.box((0, py, 5.0), (3.5, 2.75, .8), 'paint.main', 'frame', ch=C(.25), top=C(.18), part='pier')
    with k.at((-2.19, py, 2.65), yaw=-math.pi / 2):
        k.service_panel(w=.72, h=.44)
    if diag:
        # the stock diagonal pier is square (5.04 m, 7.13 m across the diamond): square the pier, then turn it 45 degrees
        for p in k.pieces[first_pier:]:
            p['v'] = [(x, py + (y - py) * 1.26, z) for x, y, z in p['v']]
        ca, sa = math.cos(math.pi / 4), math.sin(math.pi / 4)
        for p in k.pieces[first_pier:]:
            p['v'] = [(x * ca - (y - py) * sa, py + x * sa + (y - py) * ca, z) for x, y, z in p['v']]
    k.moss_sites.append(((-2.6, py - 2.1, 0), (-2.6, py + 2.1, 0)))
    k.hits.append(dict(p=(0, y_start + 2.5 * u, 2.2), d=(1, 0, 0), r=2.6, depth=0, sag=0))
    k.meta['wall_units'] = (y_start, u)


def wall_damage(k, condition):
    rng = random.Random(k.seed * 31 + 5)
    y_start, u = k.meta['wall_units']
    sy = k.meta['fit_scale_xyz'][1]
    cy = k.meta['fit_center'][1]
    Y = lambda y: (y - cy) * sy  # noqa: E731  authoring -> fitted Y
    if condition == 'partial':
        gone, spans, joints = {2}, {1}, set()
    else:
        gone, spans, joints = {0, 1, 2, 3, 4}, {0, 1, 2}, {0, 1, 2}
    k.pieces = [p for p in k.pieces if p.get('unit') not in gone and p.get('span') not in spans and p.get('joint') not in joints]
    W = 1.75
    for j in sorted(gone):
        y0, y1 = Y(y_start + j * u) + .03, Y(y_start + (j + 1) * u) - .03
        # Broken stub in the footing slot: jagged top, rebar out of the break.
        top = rng.uniform(.8, 1.5) if (condition == 'partial' or j == max(gone)) else rng.uniform(.25, .6)
        n = 6
        ridge = [(y0 + (y1 - y0) * i / n, .36 + top * (.55 + .45 * abs(math.sin(i * 2.3 + j)))) for i in range(n + 1)]
        prof = [(y0, .36)] + ridge + [(y1, .36)]
        k.extrude(list(reversed(prof)), -W / 2 + .04, W / 2 - .04, 'ferro', 'mass', axis='x', rebar=True)
        for i in range(4):
            x = rng.uniform(-.6, .6)
            yy = rng.uniform(y0 + .3, y1 - .3)
            z0 = .36 + top * .5
            wire(k, [(x, yy, z0), (x + rng.uniform(-.1, .1), yy + rng.uniform(-.1, .1), z0 + .6),
                     (x + rng.uniform(-.4, .4), yy + rng.uniform(-.3, .3), z0 + .85)], .025, 'metal', 'frame')
        # The face slab fell outward and lies tilted against the footing.
        side = -1 if j % 2 == 0 else 1
        with k.at((side * (W / 2 + 1.25), (y0 + y1) / 2 + rng.uniform(-.3, .3), .55), yaw=rng.uniform(-.2, .2), roll=side * rng.uniform(.18, .32)):
            k.box((0, 0, 0), (2.1, u * .78, .42), 'ferro', 'mass', ch=C(.06), top=C(.04))
        for i in range(5):
            rock(k, (side * rng.uniform(.9, 2.3), rng.uniform(y0, y1), .15), rng.uniform(.35, .8), 'ferro', rng)
    if condition == 'destroyed':
        for i in range(8):
            yy = Y(y_start + rng.uniform(0, 4.6 * u))
            with k.at((rng.uniform(-2.0, 2.0), yy, .3), yaw=rng.uniform(-.5, .5), roll=rng.uniform(-.3, .3)):
                k.box((0, 0, 0), (rng.uniform(1.0, 1.6), rng.uniform(1.3, 2.6), rng.uniform(.3, .45)), 'ferro', 'mass', ch=C(.05))
        for j in range(3):
            yc = Y(y_start + (j + .5) * 2 * u)
            with k.at((rng.uniform(-2.0, -1.5), yc, .2), yaw=rng.uniform(-.25, .25), roll=rng.uniform(-.2, .2)):
                k.box((0, 0, 0), (1.9, 2 * u * .7, .3), 'paint.main', 'skin', ch=C(.04))
            with k.at((rng.uniform(1.2, 1.8), Y(y_start + j * 2 * u) + 1.0, .1), yaw=rng.uniform(-.6, .6)):
                ibeam(k, (0, -1.4, 0), (0, 1.4, .25), d=.11, w=.5, mat='paint.second')
    k.hits = [dict(p=(0, Y(y_start + 2.5 * u), 1.2), d=(1, 0, 0), r=3.0 if condition == 'partial' else 7.0, depth=0, sag=0)]


# ------------------------------------------------------------ razorwirefence
def fence(k, L=10.6, h=5.7, sections=1):
    """Approved razorwirefence; razorwirefencediag passes a longer run in two sections (a middle post)."""
    posts = [-L / 2 + L * i / sections for i in range(sections + 1)]
    for y in posts:
        k.box((0, y, .15), (.72, .72, .30), 'ferro', 'mass', ch=C(.08), top=C(.06), part='footing')
        k.box((0, y, .32), (.44, .44, .04), 'metal', 'frame', part='footing')
        for sx in (-1, 1):
            for sy in (-1, 1):
                k.cyl((sx * .16, y + sy * .16, .36), .025, .06, 'metal', 'detail', sides=8)
        k.box((0, y, .34 + (h - .34) / 2), (.26, .26, h - .34), 'paint.trim', 'frame', ch=C(.03), top=C(.02))
        k.box((0, y, h + .03), (.34, .34, .06), 'paint.trim', 'frame', ch=C(.02))
        for s in (-1, 1):
            k.beam((0, y, h - .3), (s * 1.0, y, h + .72), .09, 'paint.trim', 'frame')
        for j in range(9):
            k.box((0, y, .75 + j * .56), (.30, .10, .07), 'metal', 'detail')
    for z in (.45, h - .15):
        wire(k, [(0, -L / 2, z), (0, L / 2, z)], .05, 'paint.trim', 'frame', sides=8)
    for j in range(9):
        z = .75 + j * .56
        n = 3 if geo.GAME_MESH else 41
        pts = [(0 if geo.GAME_MESH else .03 * math.sin(i * .9), -L / 2 + L * i / (n - 1),
                z - .05 * math.sin(math.pi * i / (n - 1))) for i in range(n)]
        wire(k, pts, .028, 'metal', 'skin', sides=6, removable=True)
        if not geo.GAME_MESH:
            for i in range(1, 30):
                y = -L / 2 + L * i / 30
                k.beam((-.06, y - .04, z - .04), (.06, y + .04, z + .04), .012, 'metal', 'detail')
    for ya, yb in zip(posts, posts[1:]):
        for a, b in (((0, ya, .5), (0, yb, h - .2)), ((0, yb, .5), (0, ya, h - .2))):
            wire(k, [a, b], .024, 'metal', 'frame', sides=6)
    for x in (-.98, .98):
        wire(k, [(x, -L / 2, h + .7), (x, L / 2, h + .7)], .026, 'metal', 'frame', sides=6)
    # Concertina razor coil on the crown: the strongest RTS read of a wire fence.
    turns = int(L / (.5 if geo.GAME_MESH else .22))
    per = 5 if geo.GAME_MESH else 14
    pts = [(.36 * math.cos(i * math.tau / per), -L / 2 + L * i / (turns * per), h + .62 + .36 * math.sin(i * math.tau / per))
           for i in range(turns * per + 1)]
    wire(k, pts, .03 if geo.GAME_MESH else .022, 'metal', 'skin', sides=5)
    k.moss_sites.append(((0, -L / 2, 0), (0, L / 2, 0)))
    k.hits.append(dict(p=(0, 0, 2.0), d=(1, 0, 0), r=3.0, depth=0, sag=0))


def fence_damage(k, condition):
    # Partial: the section leans 35 degrees on bent post bases. Destroyed: it lies flat.
    ang = math.radians(35 if condition == 'partial' else 86)
    c, s = math.cos(ang), math.sin(ang)
    base = .34 * k.meta['fit_scale_xyz'][2]
    for pc in k.pieces:
        if pc.get('part') == 'footing':
            continue
        out = []
        for x, y, z in pc['v']:
            z0 = max(0.0, z - base)
            xx = x * c + z0 * s
            zz = base + z0 * c - x * s
            out.append((xx, y + .08 * math.sin(z0 * .5) * (1 if condition == 'destroyed' else .3), max(.05, zz)))
        pc['v'] = out
    k.hits = [dict(p=(2.0 if condition == 'destroyed' else 1.0, 0, .6), d=(0, 0, -1), r=2.5, depth=0, sag=0)]


# ------------------------------------------------------------ rpbuilding
RP = dict(w=26.4, d=17.0, h=6.8, ribs=(-13.2, -4.4, 4.4, 13.2), door=(5.74, 5.6))


def rp_roof_z(y):
    return RP['h'] + .25 + .35 * (1 - abs(y) / (RP['d'] / 2))


def repair_facility(k):
    """Slab -> cast rib frames + steel columns -> girts, purlins -> lap cladding and
    standing-seam roof -> trims -> ONE centred door per end, two side closures."""
    w, d, h = RP['w'], RP['d'], RP['h']
    ribs = RP['ribs']
    dw, dh = RP['door']
    # 1. Slab and low base ledge.
    k.box((0, 0, .12), (w + .7, d + .7, .24), 'ferro', 'mass', ch=C(.1), top=C(.06))
    for s in (-1, 1):
        k.box((0, s * (d / 2 + .05), .5), (w + .3, .5, .52), 'ferro', 'mass', ch=C(.08), top=C(.08))
        k.box((s * (w / 2 + .05), 0, .5), (.5, d - .3, .52), 'ferro', 'mass', ch=C(.08), top=C(.08))
    # 2. Primary frame: four pale cast rib frames spanning front to back.
    for x in ribs:
        outer = abs(x) > w * .4
        for sy in (-1, 1):
            y = sy * d / 2
            k.prism(rect_poly(x - .55, y - .7, x + .55, y + .7, C(.18)), .24, h + .55, 'ferro', 'frame', top=C(.12))
            if outer:
                sx = (1 if x > 0 else -1) * 1.25
                syy = sy * 4.1
                k.prism(rect_poly(x + sx - 1.0, y + syy - .65, x + sx + 1.0, y + syy + .65, C(.2)), 0, 1.0, 'ferro', 'mass', top=C(.2))
                length = math.hypot(sx, syy)
                nx, ny = -syy / length, sx / length
                prof = [(0, 0, .2), (sx, syy, .2), (0, 0, h - .25)]
                vv = [(x + px + side * .40 * nx, y + py + side * .40 * ny, pz) for side in (-1, 1) for px, py, pz in prof]
                k.add(vv, [(2, 1, 0), (3, 4, 5), (0, 1, 4, 3), (1, 2, 5, 4), (2, 0, 3, 5)], 'ferro', 'frame')
        for sy in (-1, 1):
            ya, yb = 0.0, sy * (d / 2 + .7)
            za, zb = rp_roof_z(0) + .20, rp_roof_z(sy * d / 2) + .20
            ang = math.atan2(zb - za, abs(yb - ya)) * (-sy)
            with k.at((x, (ya + yb) / 2, (za + zb) / 2), pitch=-ang):
                k.box((0, 0, 0), (1.0, abs(yb - ya) + .02, .58), 'ferro', 'frame', ch=C(.12), top=C(.10))
    # 3. Secondary steel: wall columns, eave beams, girts and purlins.
    for sy in (-1, 1):
        for x in (-8.8, 8.8):
            ibeam(k, (x, sy * (d / 2 - .35), .24), (x, sy * (d / 2 - .35), h), d=.28, w=.16)
    for sx in (-1, 1):
        x = sx * (w / 2 - .35)
        for y in (-4.25, 0, 4.25):
            ibeam(k, (x, y, .24), (x, y, h), d=.28, w=.16)
        ibeam(k, (x, -d / 2, h - .12), (x, d / 2, h - .12), d=.30, w=.16)
    girt_z = (1.4, 3.0, 4.6, 6.2)
    for z in girt_z:
        for sy in (-1, 1):
            for x0, x1 in ((-12.65, -4.95), (4.95, 12.65)):
                cee(k, (x0, sy * (d / 2 - .16), z), (x1, sy * (d / 2 - .16), z))
        for sx in (-1, 1):
            cee(k, (sx * (w / 2 - .16), -d / 2 + .7, z), (sx * (w / 2 - .16), d / 2 - .7, z))
    lanes = [(ribs[i] + .5, ribs[i + 1] - .5) for i in range(3)]
    purlin_y = [-d / 2 + .9 + j * (d - 1.8) / 8 for j in range(9)]
    for x0, x1 in lanes:
        for y in purlin_y:
            cee(k, (x0, y, rp_roof_z(y) - .14), (x1, y, rp_roof_z(y) - .14), d=.18, w=.07)
    # 4. Skin: tan lap cladding hung on the girts, end walls first.
    board_h = .56
    rows = int((h - .78) / board_h)
    for sy in (-1, 1):
        with k.at((0, sy * d / 2, 0), yaw=0 if sy < 0 else math.pi):
            for x0, x1 in ((-12.65, -4.95), (4.95, 12.65)):
                xm = (x0 + x1) / 2
                for r in range(rows):
                    z0 = .78 + r * board_h
                    for a, b in ((x0, xm), (xm, x1)):
                        lap_board(k, a + .01, b - .01, z0, z0 + board_h, 'paint.main', removable=True)
                for z in girt_z:
                    bolts(k, (x0 + .15, -.05, z), (x1 - .15, -.05, z), pitch=.62)
                k.slab(xm - .07, xm + .07, .78, h, -.09, -.02, 'paint.trim', 'frame', front=C(.02))
            # Door surround: cast jambs and header.
            k.slab(-dw / 2 - 1.0, -dw / 2, .24, h, -.12, .2, 'ferro', 'frame', front=C(.05))
            k.slab(dw / 2, dw / 2 + 1.0, .24, h, -.12, .2, 'ferro', 'frame', front=C(.05))
            k.slab(-dw / 2 - 1.0, dw / 2 + 1.0, dh, h + .3, -.14, .2, 'ferro', 'frame', front=C(.06))
            # ONE centred door: two grey steel sliding leaves under a top track.
            for s in (-1, 1):
                xa, xb = min(0, s * dw / 2) + .02, max(0, s * dw / 2) - .02
                k.slab(xa, xb, .26, dh - .05, .02, .10, 'paint.second', 'skin', front=C(.02), door=True)
                for z in (1.6, 3.6):
                    k.slab(xa + .18, xb - .18, z, z + .18, -.02, .03, 'paint.trim', 'detail', front=.012)
                k.box((s * .25, -.03, 2.3), (.06, .08, .9), 'metal', 'detail', ch=.01)
            k.box((0, -.04, dh + .03), (dw + .3, .12, .14), 'metal', 'frame')
            k.slab(-dw / 2, dw / 2, .24, .30, -.6, .02, 'hazard', 'detail')
            k.lamp_head(0, dh + .45, -.14, w=.42)
    # Side walls: cladding around two elevated closures each (one wide, one narrow).
    closures = ((-2.5, 8.6), (5.2, 4.4))
    for sx in (-1, 1):
        with k.at((sx * w / 2, 0, 0), yaw=math.pi / 2 if sx > 0 else -math.pi / 2):
            run = (-d / 2 + .7, d / 2 - .7)
            holes = [(cx - pw / 2 - .2, 2.05, cx + pw / 2 + .2, h + .1) for cx, pw in closures]
            for r in range(rows):
                z0 = .78 + r * board_h
                for a in geo.subtract_rect((run[0], z0, run[1], z0 + board_h), holes):
                    if a[2] - a[0] > .3:
                        lap_board(k, a[0] + .01, a[2] - .01, z0, z0 + board_h, 'paint.main', removable=True)
            for z in girt_z:
                for a in geo.subtract_rect((run[0], z - .05, run[1], z + .05), holes):
                    bolts(k, (a[0] + .15, -.05, z), (a[2] - .15, -.05, z), pitch=.62)
            for cx, pw in closures:
                # Closure frame is a border (jambs, head, sill), not a plate over the shutter.
                for x0, x1, z0, z1 in ((cx - pw / 2 - .2, cx - pw / 2, 2.05, h + .05), (cx + pw / 2, cx + pw / 2 + .2, 2.05, h + .05),
                                       (cx - pw / 2, cx + pw / 2, h - .2, h + .05), (cx - pw / 2, cx + pw / 2, 2.05, 2.25)):
                    k.slab(x0, x1, z0, z1, -.10, .10, 'paint.trim', 'frame', front=C(.03))
                k.slab(cx - pw / 2, cx + pw / 2, 2.25, h - .2, -.03, .06, 'paint.second', 'skin', front=C(.02), door=True)
                for zz in (3.0, 3.8, 4.6, 5.4):
                    k.slab(cx - pw / 2 + .1, cx + pw / 2 - .1, zz, zz + .06, -.05, -.02, 'paint.trim', 'detail')
                k.box((cx, -.10, 4.2), (pw * .7, .07, .10), 'metal', 'detail', ch=.02)
                k.slab(cx - pw / 2 - .3, cx + pw / 2 + .3, 1.85, 2.05, -.35, .10, 'ferro', 'frame', front=C(.04))
                nb = int(pw / 1.4)
                for i in range(nb + 1):
                    k.box((cx - pw / 2 + .1 + i * (pw - .2) / max(1, nb), -.22, 1.75), (.14, .22, .2), 'metal', 'detail')
            if sx < 0:
                k.service_panel(-7.0, 1.4, w=.8, h=.5)
    # Eave fascia and gutters close the wall-to-roof joint.
    for sy in (-1, 1):
        for x0, x1 in lanes:
            k.box(((x0 + x1) / 2, sy * (d / 2 + .02), h + .2), (x1 - x0 + 1.0, .14, .42), 'paint.trim', 'frame', ch=C(.02))
            wire(k, [(x0, sy * (d / 2 + .16), h + .05), (x1, sy * (d / 2 + .16), h + .05)], .08, 'paint.trim', 'frame', sides=6)
    for sx in (-1, 1):
        k.box((sx * (w / 2 + .02), 0, h + .22), (.14, d - .2, .46), 'paint.trim', 'frame', ch=C(.02))
    # 5. Roof sheets over the purlins, running down the fall, lapped at seams.
    for x0, x1 in lanes:
        n = max(1, round((x1 - x0) / 1.0))
        sw = (x1 - x0) / n
        for i in range(n):
            xc = x0 + (i + .5) * sw
            for sy in (-1, 1):
                ya, yb = 0.0, sy * (d / 2 + .25)
                za, zb = rp_roof_z(0), rp_roof_z(sy * d / 2) - .015
                ang = math.atan2(zb - za, abs(yb - ya)) * (-sy)
                with k.at((xc, (ya + yb) / 2, (za + zb) / 2 + .012 * (i % 2)), pitch=-ang):
                    k.box((0, 0, 0), (sw + .06, abs(yb - ya) + .05, .035), 'corr.main', 'roof', removable=True)
                    if not geo.GAME_MESH:
                        k.box((sw / 2, 0, .04), (.035, abs(yb - ya), .06), 'corr.main', 'detail')
        k.box(((x0 + x1) / 2, 0, rp_roof_z(0) + .05), (x1 - x0, .42, .10), 'paint.trim', 'frame', ch=C(.03))
    for x in (-8.8, 8.8):
        k.box((x, 3.0, rp_roof_z(3.0) + .35), (1.1, .9, .6), 'paint.second', 'frame', ch=C(.06), top=C(.06))
    for sx in (-1, 1):
        for sy in (-1, 1):
            x, y = sx * 12.4, sy * (d / 2 + .16)
            wire(k, [(x, y, h + .05), (x, y + sy * .12, h - .3), (x, y + sy * .12, .35), (x, y + sy * .35, .12)], .07, 'paint.trim', 'frame', sides=6)
    k.box((0, 0, .27), (w - .6, d - .6, .06), 'ferro', 'mass')
    k.box((0, 0, h - .6), (w - 1.6, d - 1.6, .05), 'rubber', 'mass')  # dark ceiling liner seen through damage
    for sy in (-1, 1):
        k.moss_sites.append(((-12.6, sy * (d / 2 + .3), .24), (-4.9, sy * (d / 2 + .3), .24)))
        k.moss_sites.append(((4.9, sy * (d / 2 + .3), .24), (12.6, sy * (d / 2 + .3), .24)))
    k.hits.extend([
        dict(p=(-8.8, -d / 2, 3.4), d=(0, 1, 0), r=2.9, depth=0, sag=.35),
        dict(p=(-8.8, -2.6, h + .5), d=(0, 0, -1), r=3.3, depth=0, sag=.55)])


# ------------------------------------------------------------ quonsets
Q = dict(L=21.0, R=4.85, curb=.45, bays=7)


def quonset_build(k):
    """Slab, curbs -> arched I ribs -> purlins -> lapped corrugated arc sheets ->
    fastener rows -> framed end walls with door and windows."""
    from assemble import end_wall
    L, R, zc, bays = Q['L'], Q['R'], Q['curb'], Q['bays']
    bay = L / bays
    k.box((0, 0, .10), (2 * R + .4, L + .2, .20), 'ferro', 'mass', ch=C(.06), top=C(.04))
    for s in (-1, 1):
        k.box((s * R, 0, zc / 2 + .1), (.55, L + .1, zc - .1), 'ferro', 'mass', ch=C(.06), top=C(.08))
        k.moss_sites.append(((s * (R + .35), -L / 2, .2), (s * (R + .35), L / 2, .2)))
    for j in range(bays + 1):
        y = -L / 2 + j * bay + (.12 if j == 0 else -.12 if j == bays else 0)
        with k.tagged(f'rib:{j}'):          # arches_ribs = bays + 1
            arc_member(k, R - .22, y, zc, d=.24, w=.14)
        for s in (-1, 1):
            k.box((s * (R - .22), y, zc + .03), (.34, .30, .06), 'metal', 'frame')
    purlin_th = [math.pi * i / 8 for i in range(1, 8)]
    for th in purlin_th:
        r = R - .06
        k.beam((r * math.cos(th), -L / 2 + .05, zc + r * math.sin(th)), (r * math.cos(th), L / 2 - .05, zc + r * math.sin(th)), .07, 'metal', 'frame', h=.07)
    for b in range(bays):
        y0 = -L / 2 + b * bay - (0 if b == 0 else .07)
        y1 = -L / 2 + (b + 1) * bay + (0 if b == bays - 1 else .07)
        for q in range(4):
            th0 = max(0.0, q * math.pi / 4 - .035)
            th1 = min(math.pi, (q + 1) * math.pi / 4 + .035)
            arc_sheet(k, R + .016 * ((b + q) % 2), th0, th1, y0, y1, zc, 'corr.corr', 'roof', removable=True, sheet=(b, q))
    for th in purlin_th:
        for b in range(bays):
            ya = -L / 2 + b * bay + .1
            rr = R + .045
            bolts(k, (rr * math.cos(th), ya, zc + rr * math.sin(th)), (rr * math.cos(th), ya + bay - .2, zc + rr * math.sin(th)), pitch=.30, s=.03)
    # Three dormer windows per side, low on the barrel (stock count).
    for b in (1, 3, 5):
        y = -L / 2 + (b + .5) * bay
        for s in (-1, 1):
            th = math.radians(28)
            x = s * (R * math.cos(th) + .35)
            z = zc + R * math.sin(th)
            with k.at((x, y, z - .55), yaw=s * math.pi / 2):
                k.box((0, .30, .55), (1.2, .8, 1.15), 'corr.corr', 'skin')
                for a, c, bz, dz in [(-.42, -.33, .20, .95), (.33, .42, .20, .95), (-.33, .33, .20, .28), (-.33, .33, .87, .95)]:
                    k.slab(a, c, bz, dz, -.16, .02, 'paint.second', 'frame', front=C(.025))
                k.slab(-.33, .33, .28, .87, -.145, -.12, 'glass', 'glass', pane=True)
                k.slab(-.02, .02, .28, .87, -.10, -.04, 'paint.second', 'frame')
                k.slab(-.33, .33, .56, .60, -.10, -.04, 'paint.second', 'frame')
                k.slab(-.55, .55, 1.08, 1.18, -.25, .55, 'paint.trim', 'frame', front=C(.03))
    for sign in (-1, 1):
        with k.at((0, sign * L / 2, 0), yaw=0 if sign < 0 else math.pi):
            # End-wall studs and door header first, then the sheets hung on them.
            for x in (-3.6, -1.9, -.75, .75, 1.9, 3.6):
                top = zc + math.sqrt(max(0, (R - .25) ** 2 - x * x))
                k.box((x, .22, (.2 + top) / 2), (.10, .14, top - .2), 'metal', 'frame')
            k.box((0, .22, 2.45), (2.0, .14, .12), 'metal', 'frame')
            end_wall(k, R, zc, door=True)
            k.service_panel(0, 3.75, w=.75, h=.42)
    k.hits.extend([
        dict(p=(-3.0, -1.5, 4.2), d=(0, 0, -1), r=2.6, depth=0, sag=.4),
        dict(p=(1.7, -L / 2, 1.6), d=(0, 1, 0), r=1.4, depth=0, sag=.2)])


# ------------------------------------------------------------ lookouttower
LK = dict(z_base=4.05, z0=4.49, z1=15.2, W0=5.95, D0=5.4, W1=4.5, D1=4.1, cab=17.94, cw0=.95, cw1=.72)


def lookout(k):
    """Cast base -> tapered corner columns and hidden ring girts -> riveted plates
    (sage insets, khaki fill) -> cabin frame, window band -> roof slab."""
    z0, z1 = LK['z0'], LK['z1']
    W0, D0, W1, D1, cw0, cw1 = LK['W0'], LK['D0'], LK['W1'], LK['D1'], LK['cw0'], LK['cw1']
    H = z1 - z0
    zb = LK['z_base']
    # Base: blue-grey battered cast plinth, dark band, khaki ledge, four doors.
    # Stock flared skirt: full lot at the ground, ~79% at the band.
    loft(k, (-4.99, -4.53, 4.99, 4.53), 0, (-3.9, -3.55, 3.9, 3.55), zb, 'ferro', 'mass', ch0=.30, ch1=.22, rebar=True)
    k.box((0, 0, 4.2), (7.95, 7.25, .30), 'paint.trim', 'mass', ch=C(.22), top=C(.05))
    k.box((0, 0, 4.42), (7.55, 6.85, .14), 'paint.main', 'frame', ch=C(.2), top=C(.05))
    base_faces = (((0, -4.53, 0), 0.0, 9.98, (4.53 - 3.55) / zb), ((4.99, 0, 0), math.pi / 2, 9.06, (4.99 - 3.9) / zb),
                  ((0, 4.53, 0), math.pi, 9.98, (4.53 - 3.55) / zb), ((-4.99, 0, 0), -math.pi / 2, 9.06, (4.99 - 3.9) / zb))
    for o, yaw, wb, bat in base_faces:
        with k.at(o, yaw=yaw):
            with k.at((0, 0, 0), pitch=-math.atan(bat)):
                S = zb / math.cos(math.atan(bat))
                for x in (-wb * .30, wb * .30):
                    k.slab(x - .05, x + .05, .3, S - .2, -.02, .02, 'rubber', 'detail')
                k.slab(-1.0, -.78, 0, 3.15, -.12, .05, 'paint.corr', 'frame', front=C(.03))
                k.slab(.78, 1.0, 0, 3.15, -.12, .05, 'paint.corr', 'frame', front=C(.03))
                k.slab(-1.0, 1.0, 2.95, 3.2, -.12, .05, 'paint.corr', 'frame', front=C(.03))
                k.slab(-.78, .78, .05, 2.95, -.02, .06, 'paint.corr', 'skin', front=C(.02), door=True)
                k.slab(-.30, .30, 2.15, 2.55, -.04, -.01, 'rubber', 'detail')
                k.box((.55, -.06, 1.45), (.05, .08, .25), 'metal', 'detail')
                k.lamp_head(0, 3.55, -.02, w=.40)
            k.slab(-1.2, 1.2, 0, .12, -.9, .1, 'ferro', 'mass', front=C(.03))
            k.moss_sites.append(((-3.5, -.3, 0), (-1.4, -.3, 0)))
    # Shaft frame: dark inner core, then four tapered khaki corner columns.
    loft(k, (-W0 / 2 + .45, -D0 / 2 + .45, W0 / 2 - .45, D0 / 2 - .45), z0,
         (-W1 / 2 + .45, -D1 / 2 + .45, W1 / 2 - .45, D1 / 2 - .45), z1, 'paint.trim', 'mass')
    for sx in (-1, 1):
        for sy in (-1, 1):
            def rr(W, D, cw):
                xa, xb = sorted((sx * W / 2, sx * (W / 2 - cw)))
                ya, yb = sorted((sy * D / 2, sy * (D / 2 - cw)))
                return (xa, ya, xb, yb)
            loft(k, rr(W0, D0, cw0), z0, rr(W1, D1, cw1), z1, 'paint.main', 'frame', ch0=.10)
    girts = [z0 + H * f for f in (.25, .5, .75)]
    for g in girts:
        f = (g - z0) / H
        W, D = W0 + (W1 - W0) * f - .5, D0 + (D1 - D0) * f - .5
        corners = [(-W / 2, -D / 2, g), (W / 2, -D / 2, g), (W / 2, D / 2, g), (-W / 2, D / 2, g)]
        for a, b in zip(corners, corners[1:] + corners[:1]):
            cee(k, a, b, d=.16, w=.06)
    # Riveted plates on each battered face, split at the girts. Fractions are of
    # the open half-width between the columns: front/rear one wide sage inset,
    # left/right two narrow sage strips around a khaki mullion (stock).
    layouts = {'wide': ([(-.62, .62)], [(-1, -.62), (.62, 1)]),
               'pair': ([(-.95, -.40), (.40, .95)], [(-1, -.95), (-.40, .40), (.95, 1)])}
    faces = (((0, -D0 / 2), 0.0, W0, W1, D0, D1, 'wide'), ((W0 / 2, 0), math.pi / 2, D0, D1, W0, W1, 'pair'),
             ((0, D0 / 2), math.pi, W0, W1, D0, D1, 'wide'), ((-W0 / 2, 0), -math.pi / 2, D0, D1, W0, W1, 'pair'))
    for (ox, oy), yaw, fw0, fw1, dd0, dd1, kind in faces:
        bat = (dd0 - dd1) / 2 / H
        S = H / math.cos(math.atan(bat))
        sage, khaki = layouts[kind]

        def g(z):
            return fw0 / 2 - (fw0 - fw1) / 2 * z / S - (cw0 + (cw1 - cw0) * z / S) + .02
        cuts = [0.0] + [(gz - z0) / H * S for gz in girts] + [S]
        with k.at((ox, oy, z0), yaw=yaw):
            with k.at((0, 0, 0), pitch=-math.atan(bat)):
                for za, zt in zip(cuts, cuts[1:]):
                    ztop = zt + (.05 if zt < S else 0)
                    ga, gt = g(za), g(zt)
                    for fa, fb in sage:
                        plate(k, fa * ga, fb * ga, fa * gt, fb * gt, za + .02, ztop, .02, .10, 'paint.second', 'skin', removable=True)
                    for fa, fb in khaki:
                        plate(k, fa * ga, fb * ga, fa * gt, fb * gt, za + .02, ztop, -.03, .08, 'paint.main', 'skin', removable=True)
                        xm_a, xm_t = (fa + fb) / 2 * ga, (fa + fb) / 2 * gt
                        bolts(k, (xm_a, -.05, za + .2), (xm_t, -.05, zt - .2), pitch=.45, s=.04)
                    bolts(k, (-ga + .1, -.05, za + .1), (ga - .1, -.05, za + .1), pitch=.42, s=.04)
    # Cabin: floor slab, corner posts, sill and head bands, pale window band.
    cz, top = z1, LK['cab']
    CW, CD = W0, D0
    k.box((0, 0, cz + .15), (CW + .25, CD + .25, .30), 'paint.main', 'frame', ch=C(.12), top=C(.04))
    k.box((0, 0, (cz + top) / 2 + .15), (CW - .6, CD - .6, top - cz - .3), 'paint.trim', 'mass')
    for sx in (-1, 1):
        for sy in (-1, 1):
            k.box((sx * (CW / 2 - .25), sy * (CD / 2 - .25), (cz + top) / 2), (.5, .5, top - cz), 'paint.main', 'frame', ch=C(.06))
    for o, yaw, length in (((0, -CD / 2), 0.0, CW), ((CW / 2, 0), math.pi / 2, CD), ((0, CD / 2), math.pi, CW), ((-CW / 2, 0), -math.pi / 2, CD)):
        with k.at((o[0], o[1], cz), yaw=yaw):
            hl = length / 2 - .5
            k.slab(-hl, hl, .30, .72, -.06, .12, 'paint.main', 'skin', front=C(.03))
            k.slab(-hl, hl, 1.95, top - cz, -.06, .12, 'paint.main', 'skin', front=C(.03))
            step = (2 * hl - .5) / 3
            wins = [(-hl + .25 + j * step + .14, -hl + .25 + (j + 1) * step - .14) for j in range(3)]
            # Pale frame band built around three real openings; glass recessed behind.
            k.slab(-hl - .02, hl + .02, .72, .86, -.10, .05, 'paint.corr', 'frame', front=C(.03))
            k.slab(-hl - .02, hl + .02, 1.82, 1.95, -.10, .05, 'paint.corr', 'frame', front=C(.03))
            edges = [-hl - .02] + [v for w in wins for v in w] + [hl + .02]
            for q in range(0, len(edges), 2):
                k.slab(edges[q], edges[q + 1], .86, 1.82, -.10, .05, 'paint.corr', 'frame', front=C(.02))
            for x0, x1 in wins:
                k.slab(x0, x1, .86, 1.82, .10, .13, 'glass', 'glass', pane=True)
                k.slab((x0 + x1) / 2 - .025, (x0 + x1) / 2 + .025, .86, 1.82, .05, .10, 'paint.trim', 'frame')
            k.slab(-hl, hl, .70, .76, -.25, .05, 'paint.trim', 'frame', front=C(.02))
    k.box((0, 0, top + .23), (7.65, 7.0, .46), 'paint.main', 'roof', ch=C(.18), top=C(.06))
    k.box((0, 0, top + .48), (7.25, 6.6, .04), 'gravel', 'roof', ch=C(.12))
    for x in (-3.65, 3.65):
        k.box((x, 0, top + .50), (.16, 6.8, .10), 'paint.trim', 'frame', ch=C(.03))
    for y in (-3.32, 3.32):
        k.box((0, y, top + .50), (7.3, .16, .10), 'paint.trim', 'frame', ch=C(.03))
    k.box((.7, .5, top + .62), (1.0, .7, .28), 'paint.trim', 'detail', ch=.05, top=.04)
    k.hits.extend([
        dict(p=(0, -2.9, 9.0), d=(0, 1, 0), r=1.9, depth=0, sag=.15),
        dict(p=(-2.4, -2.6, 16.6), d=(0, 1, 0), r=1.4, depth=0, sag=.1)])


# ------------------------------------------------------------ crateswithtarp
CR = dict(L=5.1, W=3.6, H=4.1, xs=(-2.68, 2.68))
TARP = dict(R=.06, o=.05, hem_ends=1.85, hem_sides=2.4)


def crate(k, cx, idx, broken=False, end_midrail=False):
    """Skids -> corner posts and rails -> diagonal braces -> boards behind the frame."""
    L, W, H = CR['L'], CR['W'], CR['H']
    a, b = L / 2, W / 2
    z0 = .18
    with k.tagged(f'crate{idx}'):
        for y in (-b + .4, b - .4):
            k.box((cx, y, .09), (L - .12, .3, .18), 'wood.second', 'frame', ch=C(.02))
        for sx in (-1, 1):
            for sy in (-1, 1):
                k.box((cx + sx * (a - .08), sy * (b - .08), (z0 + H) / 2), (.16, .16, H - z0), 'wood.second', 'frame', ch=C(.025))
        mid = (z0 + H) / 2
        for z in (z0 + .08, mid, H - .08):
            for sy in (-1, 1):
                k.box((cx, sy * (b - .07), z), (L - .32, .14, .16), 'wood.second', 'frame', ch=C(.02))
            if z != mid or end_midrail:
                for sx in (-1, 1):
                    k.box((cx + sx * (a - .07), 0, z), (.14, W - .32, .16), 'wood.second', 'frame', ch=C(.02))
        for sy in (-1, 1):
            # Stock long faces are split into two bays by one centre post.
            k.box((cx, sy * (b - .07), (z0 + H) / 2), (.16, .14, H - z0 - .1), 'wood.second', 'frame', ch=C(.02))
        rows = 12
        bh = (H - z0 - .04) / rows
        merged = geo.GAME_MESH and not broken
        for sy in (-1, 1):
            if merged:
                k.box((cx, sy * (b - .09), (z0 + H) / 2), (L - .3, .05, H - z0 - .05), 'wood.main', 'skin')
            else:
                for r in range(rows):
                    k.box((cx, sy * (b - .09), z0 + .02 + (r + .5) * bh), (L - .3, .05, bh - .018), 'wood.main', 'skin', ch=C(.008), board=(sy, r))
        for sx in (-1, 1):
            if merged:
                k.box((cx + sx * (a - .09), 0, (z0 + H) / 2), (.05, W - .3, H - z0 - .05), 'wood.main', 'skin')
            else:
                for r in range(rows):
                    k.box((cx + sx * (a - .09), 0, z0 + .02 + (r + .5) * bh), (.05, W - .3, bh - .018), 'wood.main', 'skin', ch=C(.008))
        if merged:
            k.box((cx, 0, H - .04), (L - .3, W - .3, .08), 'wood.main', 'skin')
        else:
            n = 10
            for i in range(n):
                k.box((cx - a + .15 + (i + .5) * (L - .3) / n, 0, H - .04), ((L - .3) / n - .02, W - .3, .08), 'wood.main', 'skin', ch=C(.008))
        for sx in (-1, 1):
            for sy in (-1, 1):
                for z in (z0 + .1, 1.2):
                    k.box((cx + sx * (a - .02), sy * (b - .02), z), (.05, .05, .22), 'metal', 'detail')
        for sy in (-1, 1):
            k.box((cx + .9, sy * (b - .055), .75), (.8, .02, .3), 'paint.trim', 'detail')
        k.moss_sites.append(((cx - a, -b - .05, 0), (cx + a, -b - .05, 0)))


def drape_point(u, v, a, b, T, R, o, extra=0.0, floor=.12):
    """Map an unfolded tarp coordinate onto its drape over a rounded-box hull.

    The hull is the crate's outer frame line (half sizes a, b, top T) with edge
    radius R. Every point lies at radius >= R + o from the hull's inner
    rectangle, so the cloth is always outside the wood. Returns
    (point, unit normal, s = unfolded distance past the top, tangent coord)."""
    r = R + o + extra
    ia, ib = a - R, b - R
    ex, ey = abs(u) - ia, abs(v) - ib
    sx, sy = (1 if u >= 0 else -1), (1 if v >= 0 else -1)
    if ex <= 0 and ey <= 0:
        return (u, v, T + o + extra), (0, 0, 1), 0.0, 0.0
    corner = ex > 0 and ey > 0
    if corner:
        s = math.hypot(ex, ey)
        phi = math.atan2(ey, ex)
        dx, dy, ox, oy, t = math.cos(phi) * sx, math.sin(phi) * sy, ia * sx, ib * sy, phi * r
    elif ex > 0:
        s, dx, dy, ox, oy, t, phi = ex, sx, 0.0, ia * sx, v, v, 0.0
    else:
        s, dx, dy, ox, oy, t, phi = ey, 0.0, sy, u, ib * sy, u, math.pi / 2
    quarter = r * math.pi / 2
    if s <= quarter:
        th = s / r
        rho, z, nh, nz = r * math.sin(th), T - R + r * math.cos(th), math.sin(th), math.cos(th)
    else:
        rho, z, nh, nz = r, T - R - (s - quarter), 1.0, 0.0
    if corner and s > quarter:
        # Surplus corner cloth folds into a diagonal dog-ear that flares outward.
        rho += .30 * min(ex, ey) * abs(math.sin(2 * phi))
    return (ox + dx * rho, oy + dy * rho, max(floor, z)), (dx * nh, dy * nh, nz), s, t


def tarp(k, cx, idx, a=None, b=None, T=None, hem_ends=None, hem_sides=None, cy=0.0, flag=True, straps=True):
    """Cloth over a rounded-box hull. Stock: the cover hangs to ~45% of the case
    height on the short ends and ~58% on the long sides. Two tie-down straps per
    cover (owner ruling 2026-10-22: keep them although the stock has none)."""
    a = CR['L'] / 2 if a is None else a
    b = CR['W'] / 2 if b is None else b
    T = CR['H'] if T is None else T
    hem_ends = TARP['hem_ends'] if hem_ends is None else hem_ends
    hem_sides = TARP['hem_sides'] if hem_sides is None else hem_sides
    R, o = TARP['R'], TARP['o']
    r = R + o
    sx_max = r * math.pi / 2 + (T - R - hem_ends)
    sy_max = r * math.pi / 2 + (T - R - hem_sides)
    Ux, Uy = a - R + sx_max, b - R + sy_max
    nx, ny = (22, 16) if geo.GAME_MESH else (64, 46)
    smax = max(sx_max, sy_max)
    strap_u = (-a * .55, a * .55) if straps else ()
    verts, faces = [], []
    for j in range(ny + 1):
        v = -Uy + 2 * Uy * j / ny
        for i in range(nx + 1):
            u = -Ux + 2 * Ux * i / nx
            p, nrm, sd, t = drape_point(u, v, a, b, T, R, o, floor=.06)
            # Outward-only displacement: vertical folds growing toward the hem
            # and gentle billows on top, pinched flat under the straps. Never
            # inward, so never into the wood.
            hang = max(0.0, min(1.0, (sd - r) / max(.01, smax - r)))
            pinch = min([1.0] + [abs(u - q) / .45 for q in strap_u])
            fold = .065 * hang * (.5 + .5 * math.sin(t * math.tau / .62 + sd * 1.3 + idx)) ** 2
            billow = .03 * (1 - hang) * (.5 + .5 * math.sin(u * 2.3 + v * 1.7 + idx * 2))
            disp = (fold + billow) * min(1.0, pinch) * (.6 if geo.GAME_MESH else 1.0)
            verts.append((cx + p[0] + nrm[0] * disp, cy + p[1] + nrm[1] * disp, p[2] + nrm[2] * disp))
    for j in range(ny):
        for i in range(nx):
            q = j * (nx + 1) + i
            faces.append((q, q + 1, q + nx + 2, q + nx + 1))
    extra = dict(tarp=idx) if flag else {}
    k.add(verts, faces, 'canvas.corr', 'skin', canvas=True, **extra)
    if not geo.GAME_MESH:
        ring = [(0, i) for i in range(nx + 1)] + [(j, nx) for j in range(1, ny + 1)] + \
               [(ny, i) for i in range(nx - 1, -1, -1)] + [(j, 0) for j in range(ny - 1, 0, -1)]
        hem_pts = [verts[j * (nx + 1) + i] for j, i in ring]
        wire(k, hem_pts + hem_pts[:1], .022, 'canvas.corr', 'detail', sides=6, canvas=True, **extra)
    # Tie-down straps over the cloth, then straight down the boards to the bottom rail.
    for su in strap_u:
        n = 12 if geo.GAME_MESH else 40
        pts = []
        for j in range(n + 1):
            p = drape_point(su, -Uy + 2 * Uy * j / n, a, b, T, R, o, extra=.03, floor=.06)[0]
            pts.append((cx + p[0], cy + p[1], p[2]))
        pts = [(pts[0][0], cy - b - .02, .30)] + pts + [(pts[-1][0], cy + b + .02, .30)]
        wire(k, pts, .03, 'rubber', 'frame', sides=4, strap=idx)


def tarp_crates(k):
    for idx, cx in enumerate(CR['xs']):
        crate(k, cx, idx, broken=(getattr(k, 'cond', '') == 'partial' and idx == 0))
        tarp(k, cx, idx)
    k.hits.append(dict(p=(CR['xs'][0], -1.9, 2.0), d=(0, 1, 0), r=2.0, depth=0, sag=0))


def tarp_clearance(k):
    """Smallest clearance (m) of any tarp vertex outside the crate hulls; < 0 = inside wood.
    Valid on the unfitted pristine kit (authoring coordinates)."""
    a, b, T = CR['L'] / 2, CR['W'] / 2, CR['H']
    worst = 9.0
    for pc in k.pieces:
        if pc.get('tarp') is None:
            continue
        for x, y, z in pc['v']:
            for cx in CR['xs']:
                worst = min(worst, max(abs(x - cx) - a, abs(y) - b, z - T))
    return worst


def crumpled_tarp(k, cx, cy, sx, sy, h, rng):
    n = (12, 8) if geo.GAME_MESH else (36, 24)
    ph = [rng.uniform(0, 6) for _ in range(2)]
    verts, faces = [], []
    for j in range(n[1] + 1):
        for i in range(n[0] + 1):
            u, v = i / n[0] - .5, j / n[1] - .5
            mound_f = max(0.0, 1 - (2 * u) ** 2 - (2 * v) ** 2)
            z = .04 + h * mound_f * (.6 + .4 * abs(math.sin(u * 9 + ph[0]) * math.cos(v * 7 + ph[1])))
            verts.append((cx + u * sx, cy + v * sy, z))
    for j in range(n[1]):
        for i in range(n[0]):
            q = j * (n[0] + 1) + i
            faces.append((q, q + 1, q + n[0] + 2, q + n[0] + 1))
    # A closed underside keeps the crumpled cloth a solid for baking.
    k.add(verts, faces, 'canvas.corr', 'skin', canvas=True)


def crate_damage(k, condition):
    rng = random.Random(k.seed * 17 + 3)
    sc = k.meta['fit_scale_xyz']
    a, b = CR['L'] / 2 * sc[0], CR['W'] / 2 * sc[1]
    xs = [x * sc[0] for x in CR['xs']]
    if condition == 'partial':
        cx = xs[0]
        # Left case: tarp blown off and crumpled in front; four front boards
        # knocked out and lying at its foot; the cargo shows through.
        k.pieces = [p for p in k.pieces if p.get('tarp') != 0 and p.get('strap') != 0]
        for p in list(k.pieces):
            bd = p.get('board')
            if bd and bd[0] < 0 and bd[1] in (4, 5, 6, 7) and p.get('group') == 'crate0':
                k.pieces.remove(p)
                geo.lay_flat(k, p, (cx + rng.uniform(-1.2, 1.2), -b - .9 - rng.uniform(0, .6), 0.0), rng.uniform(-.5, .5), rng, crumple=.02)
                k.pieces.append(p)
        k.box((cx, 0, 1.9), (CR['L'] - .6, CR['W'] - .6, 3.2), 'paint.trim', 'mass', ch=C(.04))
        crumpled_tarp(k, cx - .4, -b - 1.5, 3.4, 1.5, .55, rng)
        k.hits = [dict(p=(cx, -b, 1.4), d=(0, 1, 0), r=1.2, depth=0, sag=0)]
        return
    # Destroyed: each case collapsed into a low heap of cargo and boards with its
    # cover still draped over it (same analytic drape: never through the wood);
    # loose boards and frame members pile against it outside the skirt.
    k.pieces = []
    for idx, cx in enumerate(xs):
        ha, hb, hT = 2.0, 1.45, 1.25
        k.box((cx, 0, hT / 2), (2 * ha - .1, 2 * hb - .1, hT), 'paint.second', 'mass', ch=C(.1), top=C(.12))
        tarp(k, cx, idx, a=ha, b=hb, T=hT, hem_ends=.15, hem_sides=.2, straps=False)
        for i in range(14):
            side = rng.choice((-1, 1))
            along = rng.random() < .5
            if along:
                x, y = cx + rng.uniform(-ha, ha), side * (hb + .45 + rng.uniform(0, .5))
            else:
                x, y = cx + side * (ha + .45 + rng.uniform(0, .5)), rng.uniform(-hb, hb)
            ln = rng.uniform(1.2, 3.2)
            z = .04 + rng.uniform(0, .35)
            with k.at((x, y, z), yaw=rng.uniform(-math.pi, math.pi), pitch=rng.uniform(-.3, .3), roll=rng.uniform(-.2, .2)):
                k.box((0, 0, 0), (ln, .30, .06), 'wood.main', 'skin')
        for i in range(4):
            side = rng.choice((-1, 1))
            with k.at((cx + rng.uniform(-ha, ha), side * (hb + .7), .3), yaw=rng.uniform(-.6, .6), roll=side * rng.uniform(.3, .7)):
                k.box((0, 0, 0), (rng.uniform(1.6, 3.0), .16, .16), 'wood.second', 'frame')
        k.box((cx - ha - .35, -hb - .35, .75), (.16, .16, 1.5), 'wood.second', 'frame')
    k.hits = [dict(p=(0, 0, .5), d=(0, 0, -1), r=3.0, depth=0, sag=0)]


# ------------------------------------------------------------ barrels
DRUMS = ((-.30, -1.2), (.30, 0), (-.30, 1.2))


def drum(k, i):
    R, H = .58, 1.68
    with k.tagged(f'drum{i}'):
        k.cyl((0, 0, (H - .06) / 2), R, H - .06, 'paint.main', 'mass', sides=48)
        n = 16 if geo.GAME_MESH else 48
        for z in (.035, H - .035):
            # Rolled chime: a ring, so the recessed lid stays visible inside it.
            ring = [(R * math.cos(j * math.tau / n), R * math.sin(j * math.tau / n), z) for j in range(n + 1)]
            wire(k, ring, .035, 'paint.main', 'frame', sides=8)
        for z in (.56, 1.12):
            # Black rolling hoops double as the stock band's border lines.
            k.cyl((0, 0, z), R + .03, .07, 'paint.trim', 'frame', sides=48, top=C(.02), bot=C(.02))
        # Stock: one black/yellow chevron band between the hoops, as a true UV wrap.
        wrapped_band(k, R + .004, .595, 1.085, 0.0, 1.1)
        k.cyl((0, 0, H - .065), R - .01, .03, 'paint.second', 'skin', sides=48, lid=True)
        k.cyl((.25, .1, H - .04), .07, .04, 'metal', 'detail', sides=16)
        k.cyl((-.2, -.14, H - .04), .035, .04, 'metal', 'detail', sides=12)


def hazard_barrels(k):
    for i, (x, y) in enumerate(DRUMS):
        with k.at((x, y, 0)):
            drum(k, i)
        k.moss_sites.append(((x - .5, y - .62, 0), (x + .5, y - .62, 0)))
    k.hits.append(dict(p=(0, -1.1, .8), d=(0, 1, 0), r=1.2, depth=0, sag=0))


def barrel_damage(k, condition):
    rng = random.Random(k.seed * 11 + 2)
    sc = k.meta['fit_scale_xyz']
    groups = {}
    for p in k.pieces:
        groups.setdefault(p.get('group'), []).append(p)

    def tip(pieces, cx, cy, yaw):
        # Knocked over about its base edge, then dented on the impact side.
        c, s = math.cos(yaw), math.sin(yaw)
        for p in pieces:
            out = []
            for x, y, z in p['v']:
                lx, ly = x - cx, y - cy
                nx, nz = z, -lx
                px, py = nx * c - ly * s, nx * s + ly * c
                pz = nz + .61
                dent = .12 * max(0.0, 1 - math.hypot(px - .8, py) / .6) if pz > .9 else 0.0
                out.append((cx + px, cy + py, max(.02, pz - dent)))
            p['v'] = out

    def crush(pieces, cx, cy, f):
        for p in pieces:
            out = []
            for x, y, z in p['v']:
                lx, ly = x - cx, y - cy
                bulge = 1 + .14 * math.sin(math.pi * min(1.0, z / 1.7))
                out.append((cx + lx * bulge + .06 * math.sin(z * 3 + cx * 5), cy + ly * bulge,
                            max(.02, z * f + .03 * math.sin(lx * 9 + ly * 7))))
            p['v'] = out
    centres = [(x * sc[0], y * sc[1]) for x, y in DRUMS]
    if condition == 'partial':
        cx, cy = centres[0]
        g = groups.get('drum0', [])
        lids = [p for p in g if p.get('lid')]
        tip([p for p in g if not p.get('lid')], cx, cy, -.35)
        for p in lids:
            geo.lay_flat(k, p, (cx - .9, cy - .7, 0), .4, rng, crumple=.01)
        k.hits = [dict(p=(cx, cy, .6), d=(0, 0, -1), r=1.0, depth=0, sag=0)]
        return
    for i, (cx, cy) in enumerate(centres):
        g = groups.get(f'drum{i}', [])
        lids = [p for p in g if p.get('lid')]
        body = [p for p in g if not p.get('lid')]
        if i == 1:
            tip(body, cx, cy, .6)
        else:
            crush(body, cx, cy, .45 if i == 0 else .55)
        for p in lids:
            geo.lay_flat(k, p, (cx + rng.uniform(.5, 1.0), cy + rng.uniform(-.6, .6), 0), rng.uniform(-1, 1), rng, crumple=.04)
    for i in range(5):
        rock(k, (rng.uniform(-.9, .9), rng.uniform(-1.6, 1.6), .03), rng.uniform(.18, .32), 'char', rng, flat=.3)
    k.hits = [dict(p=(0, 0, .5), d=(0, 0, -1), r=1.6, depth=0, sag=0)]


# ------------------------------------------------------------ authored peel (partial buildings)
def peel(k, seed):
    """Within each hit zone, skin panels are torn off (laid on the ground) or
    peeled about their fixed edge so the frame shows; frame members sag; panes
    break; volumetric chunks gather at the wall foot. Randomness is keyed per
    piece uid, so the high source and the game carrier receive the same damage."""
    w, d, h = k.meta['size']
    zones = list(k.hits)
    rng0 = random.Random(seed * 13 + 7)
    for p in list(k.pieces):
        c = geo.centroid(p)
        zone = next((z for z in zones if math.dist(c, z['p']) < z['r']), None)
        if zone is None:
            continue
        if p.get('pane'):
            k.pieces.remove(p)
            continue
        if p['role'] == 'frame' and zone.get('sag'):
            p['v'] = [(x, y, z - zone['sag'] * max(0.0, 1 - math.dist((x, y, z), zone['p']) / zone['r']) ** 2) for x, y, z in p['v']]
            continue
        if not p.get('removable') or 'uid' not in p:
            continue
        rng = random.Random(seed * 7919 + p['uid'])
        roll = rng.random()
        lo, hi = geo.bbox(p)
        flat = p['role'] == 'roof' and (hi[2] - lo[2]) < .5 * max(hi[0] - lo[0], hi[1] - lo[1])
        core = math.dist(c, zone['p']) < zone['r'] * .7
        if roll < (.75 if core else .35):
            k.pieces.remove(p)
            if flat:
                at = (c[0] + rng.uniform(-.6, .6), c[1] + rng.uniform(-.6, .6), .3)
            else:
                out = _unit((c[0], c[1], 0)) if math.hypot(c[0], c[1]) > .1 else (0, -1, 0)
                at = (c[0] + out[0] * rng.uniform(.6, 1.8), c[1] + out[1] * rng.uniform(.6, 1.8), 0.0)
            geo.lay_flat(k, p, at, rng.uniform(-.8, .8), rng, crumple=.05)
            k.pieces.append(p)
        elif roll < .9:
            ext = (hi[0] - lo[0], hi[1] - lo[1])
            if flat:
                # Roof sheet torn up about its far end.
                if ext[1] > ext[0]:
                    axis, hinge = (1, 0, 0), (c[0], hi[1] if zone['p'][1] < c[1] else lo[1], c[2])
                else:
                    axis, hinge = (0, 1, 0), (hi[0] if zone['p'][0] < c[0] else lo[0], c[1], c[2])
                ang = rng.uniform(.25, .55)
                if geo.rotate_about(c, hinge, axis, ang)[2] < c[2]:
                    ang = -ang
            else:
                # Wall panel hanging from its top fixings, bottom swung outward.
                axis = (1, 0, 0) if ext[0] > ext[1] else (0, 1, 0)
                hinge = (c[0], c[1], hi[2])
                ang = rng.uniform(.5, 1.1)
                test = geo.rotate_about((c[0], c[1], lo[2]), hinge, axis, ang)
                if math.hypot(test[0], test[1]) < math.hypot(c[0], c[1]):
                    ang = -ang
            p['v'] = [geo.rotate_about(v, hinge, axis, ang) for v in p['v']]
    for z in zones:
        for i in range(7):
            x = max(-w * .5, min(w * .5, z['p'][0] + rng0.uniform(-z['r'] * .6, z['r'] * .6)))
            y = max(-d * .5, min(d * .5, z['p'][1] + rng0.uniform(-z['r'] * .6, z['r'] * .6)))
            rock(k, (x, y, .1), rng0.uniform(.25, .6), 'ferro', rng0)


# ------------------------------------------------------------ full rubble (destroyed buildings)
RUBBLE = {
    'rpbuilding': dict(core='gravel', heap=.32, frame='ibeam', panel='lap', remnant='buttress', slab='ferro', leaners=13),
    'quonsetclean': dict(core='gravel', heap=.26, frame='arc', panel='arc', remnant='endarch', slab='ferro', leaners=4),
    'quonset': dict(core='gravel', heap=.26, frame='arc', panel='arc', remnant='endarch', slab='ferro', leaners=4),
    'lookouttower': dict(core='ferro', heap=.22, frame='column', panel='plate', remnant='base', slab='paint.main', leaners=6),
}


def mound(k, w, d, hgt, mat, n=18, rings=5, power=.8):
    """Closed polar mound: a solid aggregate core, not a carpet."""
    vv = [(0, 0, hgt)]
    for r in range(1, rings + 1):
        f = r / rings
        for j in range(n):
            a = j * math.tau / n
            jit = 1 + .10 * math.sin(j * 2.7 + r) + .06 * math.sin(j * 5.1)
            z = hgt * max(0.0, 1 - f * f) ** power * (1 + .12 * math.sin(j * 3.3 + r * 1.7))
            vv.append((w * .5 * f * jit * math.cos(a), d * .5 * f * jit * math.sin(a), max(.03, z) if r < rings else .02))
    ff = [(0, 1 + j, 1 + (j + 1) % n) for j in range(n)]
    for r in range(rings - 1):
        a0, b0 = 1 + r * n, 1 + (r + 1) * n
        for j in range(n):
            ff.append((a0 + j, b0 + j, b0 + (j + 1) % n, a0 + (j + 1) % n))
    last = 1 + (rings - 1) * n
    ff.append(tuple(range(last + n - 1, last - 1, -1)))
    k.add(vv, ff, mat, 'mass')


def full_rubble(k, name):
    """Bent frame + fallen skin on a solid mound + chunks + one identity remnant.
    Generated procedurally (no dependence on the intact piece list) so the game
    carrier rebuild is the same wreck."""
    if name in ('singlecivlivinga', 'portable'):
        return batch2.rubble(k, name)
    if name in batch_son.RUBBLE:
        return batch_son.rubble(k, name)
    rng = random.Random(k.seed * 31 + 7)
    w, d, h = SIZES[name]
    spec = RUBBLE[name]
    k.pieces, k.hits = [], []
    hgt = h * spec['heap']
    foot = spec.get('foot', .98 if name == 'lookouttower' else .86)
    fw, fd = w * foot, d * foot
    sc = spec.get('scale', 1.0)       # props/small assets shrink the fixed-metre debris; legacy specs are 1.0
    mound(k, fw, fd, hgt, spec['core'], power=.5)

    def hm(x, y):
        f = math.hypot(x / (fw / 2), y / (fd / 2))
        return hgt * max(0.0, 1 - f * f) ** .5 * .85
    # Bent columns standing out of the heap with chunky slabs leaning on them:
    # the readable volume of a collapse, not a decal-flat spread.
    for i in range(spec['leaners']):
        x, y = rng.uniform(-fw * .30, fw * .30), rng.uniform(-fd * .30, fd * .30)
        ang = rng.uniform(-math.pi, math.pi)
        tilt = rng.uniform(.25, .7)
        ln = rng.uniform(.45, .7) * h
        base = (x, y, hm(x, y) * .7)
        top_ = (x + math.cos(ang) * ln * math.sin(tilt), y + math.sin(ang) * ln * math.sin(tilt), base[2] + ln * math.cos(tilt))
        kneel = tuple(base[j] + (top_[j] - base[j]) * .55 for j in range(2)) + (base[2] + (top_[2] - base[2]) * .55,)
        bend = (kneel[0] + rng.uniform(-.25, .25), kneel[1] + rng.uniform(-.25, .25), kneel[2])
        top_ = (top_[0] + rng.uniform(-.6, .6), top_[1] + rng.uniform(-.6, .6), top_[2] - rng.uniform(0, .5))
        if spec['frame'] == 'column':
            loft(k, (x - .45 * sc, y - .45 * sc, x + .45 * sc, y + .45 * sc), base[2], (x - .38 * sc, y - .38 * sc, x + .38 * sc, y + .38 * sc), base[2] + 3.2 * sc, 'paint.main', 'frame', ch0=.08 * sc)
        else:
            ibeam(k, base, bend, d=.30 * sc, w=.18 * sc)
            ibeam(k, bend, top_, d=.30 * sc, w=.18 * sc)
        lean_dir = ang + rng.uniform(2.4, 3.9)
        dist = rng.uniform(1.4, 2.4) * sc
        foot = (x + math.cos(lean_dir) * dist, y + math.sin(lean_dir) * dist)
        foot = (foot[0], foot[1], hm(*foot) * .9)
        rest = (bend if spec['frame'] != 'column' else (x, y, base[2] + 3.0))
        dx, dy, dz = rest[0] - foot[0], rest[1] - foot[1], rest[2] - foot[2]
        horiz = math.hypot(dx, dy) or .01
        length = math.sqrt(horiz * horiz + dz * dz)
        sw, th = rng.uniform(1.8, 3.4) * sc, rng.uniform(.30, .50) * sc
        with k.at(((foot[0] + rest[0]) / 2, (foot[1] + rest[1]) / 2, (foot[2] + rest[2]) / 2),
                  yaw=math.atan2(dy, dx) - math.pi / 2, pitch=math.atan2(dz, horiz)):
            k.box((0, 0, -th / 2), (sw, length + .5, th), spec['slab'], 'mass', ch=C(.05), top=C(.05))
    skin = {'lap': 'paint.main', 'arc': 'corr.corr', 'plate': 'paint.second'}[spec['panel']]
    for i in range(spec.get('panels', 14 if name == 'rpbuilding' else 9)):
        x, y = rng.uniform(-fw * .38, fw * .38), rng.uniform(-fd * .38, fd * .38)
        ln = rng.uniform(.15, .30) * max(w, d)
        yaw = rng.uniform(-math.pi, math.pi)
        kink = rng.uniform(.25, .7)
        R = rng.uniform(2.6, 4.2) * sc
        ca, sa = math.cos(yaw), math.sin(yaw)
        if spec['frame'] == 'arc':
            # Buckled arch rib lying on its side across the heap.
            pts = []
            for s in range(9):
                ang = math.pi * s / 8
                px, py = R * math.cos(ang), R * math.sin(ang) * .32
                pts.append((x + px * ca - py * sa, y + px * sa + py * ca, max(.12, hm(x, y) * .6 + math.sin(ang) * (.6 + kink))))
            for p0, p1 in zip(pts, pts[1:]):
                ibeam(k, p0, p1, d=.24, w=.14)
        else:
            a = (x - ca * ln / 2, y - sa * ln / 2, max(.1, hm(x - ca * ln / 2, y - sa * ln / 2)))
            m = (x, y, hm(x, y) + .1 + ln * .12 * kink)
            b = (x + ca * ln / 2, y + sa * ln / 2, max(.1, hm(x + ca * ln / 2, y + sa * ln / 2) + ln * .2 * kink))
            size = (.5, .5, 'paint.main') if spec['frame'] == 'column' else (.28, .16, 'metal')
            ibeam(k, a, m, d=size[0], w=size[1], mat=size[2])
            ibeam(k, m, b, d=size[0], w=size[1], mat=size[2])
    for i in range(spec.get('chunks', 26 if name == 'rpbuilding' else 18)):
        x, y = rng.uniform(-fw * .40, fw * .40), rng.uniform(-fd * .40, fd * .40)
        yaw, pitch, roll = rng.uniform(-math.pi, math.pi), rng.uniform(-.55, .55), rng.uniform(-.45, .45)
        nb, ln, R, th0 = rng.randint(3, 6), rng.uniform(2.0, 3.8) * sc, rng.uniform(3.5, 5.0) * sc, rng.uniform(.3, 1.2)
        pw, pd, pm = rng.uniform(1.2, 2.2) * sc, rng.uniform(1.6, 2.8) * sc, rng.random()
        with k.at((x, y, hm(x, y) + .05), yaw=yaw, pitch=pitch, roll=roll):
            if spec['panel'] == 'lap':
                for j in range(nb):
                    k.box((0, (j * .5 - nb * .25) * sc, .02 * j * sc), (ln, .56 * sc, .035), skin, 'skin')
                k.box((0, 0, -.12), (ln * .9, .12, .18), 'metal', 'frame')
            elif spec['panel'] == 'arc':
                with k.at((0, 0, -R * math.sin(th0 + .22) + .15)):
                    arc_sheet(k, R, th0, th0 + .45, -1.5, 1.5, 0, skin, 'skin')
            else:
                k.box((0, 0, 0), (pw, pd, .14), skin if pm < .5 else 'paint.main', 'skin', ch=C(.03))
    for i in range(4):
        x, y = rng.uniform(-fw * .25, fw * .25), rng.uniform(-fd * .25, fd * .25)
        sa_, sb_ = rng.uniform(1.4, 3.8) * sc, rng.uniform(2.2, 6.0) * sc
        with k.at((x, y, hm(x, y) * .9 + .15), yaw=rng.uniform(-math.pi, math.pi), pitch=rng.uniform(-.4, .4), roll=rng.uniform(-.3, .3)):
            if name == 'lookouttower':
                k.box((0, 0, 0), (sa_, min(sb_, 3.2), .46), 'paint.main', 'roof', ch=C(.1))
            elif spec['panel'] == 'arc':
                k.box((0, 0, 0), (sa_ * .6, .6, .5), 'ferro', 'mass', ch=C(.08))
            else:
                k.box((0, 0, 0), (min(sa_, 2.4), sb_, .58), 'ferro', 'frame', ch=C(.1))
    for i in range(spec.get('rocks', 40)):
        x, y = rng.uniform(-fw * .46, fw * .46), rng.uniform(-fd * .46, fd * .46)
        size = rng.uniform(.45, 1.4) * (1.3 if name == 'lookouttower' else 1) * sc
        if (x / (fw * .5)) ** 2 + (y / (fd * .5)) ** 2 > 1:
            rng.random()
            continue
        rock(k, (x, y, hm(x, y) * .9), size, 'ferro', rng)
    if spec['remnant'] == 'buttress':
        # One splayed rib end still stands: the stock silhouette cue.
        x, y = -w / 2 + 2.6, -d / 2 + 2.4
        k.prism(rect_poly(x - .55, y - .7, x + .55, y + .7, C(.18)), 0, 3.6, 'ferro', 'frame', top=C(.3))
        k.add([(x - .4, y, 0), (x - 1.6, y - 3.9, 0), (x - .4, y, 3.3), (x + .4, y, 0), (x - .8, y - 3.9, 0), (x + .4, y, 3.3)],
              [(2, 1, 0), (3, 4, 5), (0, 1, 4, 3), (1, 2, 5, 4), (2, 0, 3, 5)], 'ferro', 'frame')
    elif spec['remnant'] == 'corner':
        # One corner column and a short wall stub still stand (the building's identity cue).
        x, y = -w / 2 + .5 * sc + .4, -d / 2 + .5 * sc + .4
        top_h = min(h * .55, 3.6)
        k.prism(rect_poly(x - .3, y - .3, x + .3, y + .3, C(.06)), 0, top_h, 'paint.trim', 'frame', top=C(.05))
        k.box((x + min(w * .12, 2.2), y, top_h * .3), (min(w * .24, 4.4), .30, top_h * .6), 'ferro', 'mass', ch=C(.04))
    elif spec['remnant'] == 'stub':
        # A broken pedestal of the main mass survives under the heap.
        r = min(w, d) * .22
        loft(k, (-r, -r, r, r), 0, (-r * .8, -r * .8, r * .8, r * .8), min(h * .22, 2.6), spec['slab'], 'mass', ch0=.1, rebar=True)
        rock(k, (r * .3, -r * .2, min(h * .22, 2.6)), r * .9, spec['slab'], rng)
    elif spec['remnant'] == 'endarch':
        y, R = -d / 2 + .6, 4.4
        for s in range(7):
            a0, a1 = math.pi * s / 10, math.pi * (s + 1) / 10
            ibeam(k, (R * math.cos(a0), y, .4 + R * math.sin(a0) * .55), (R * math.cos(a1), y, .4 + R * math.sin(a1) * .55), d=.24, w=.14)
        k.box((-R + .3, y, .4), (.5, .5, .8), 'ferro', 'mass')
    else:
        # Flared blue-grey base stub survives (stock cue), broken lower at the front.
        loft(k, (-4.99, -4.53, 4.99, 4.53), 0, (-4.45, -4.05, 4.45, 4.05), 2.2, 'ferro', 'mass', ch0=.3, ch1=.25, rebar=True)
        loft(k, (-4.45, -.5, 4.45, 4.05), 2.2, (-4.2, .1, 4.2, 3.85), 3.1, 'ferro', 'mass', ch0=.2, rebar=True)
        for i in range(8):
            rock(k, (rng.uniform(-4.0, 4.0), rng.uniform(-3.6, 3.6), 2.2), rng.uniform(.6, 1.2), 'ferro', rng)
        with k.at((rng.uniform(-1, 1), rng.uniform(-1, 1), 3.6), yaw=rng.uniform(-.6, .6), pitch=.35, roll=-.2):
            k.box((0, 0, 0), (7.0, 6.4, .46), 'paint.main', 'roof', ch=C(.15))
    # Everything rests on or in the heap; nothing pokes through the ground plane.
    for p in k.pieces:
        p['v'] = [(x, y, max(.01, z)) for x, y, z in p['v']]
    k.hits = [dict(p=(0, 0, .3), d=(0, 0, -1), r=min(w, d) * .28, depth=0, sag=0)]
    k.meta.update(collapse='full-rubble', rubble_height_ratio=spec['heap'])


DRAFTS = {
    'razorwirefence': (fence, 'galvanised'),
    's_lightwall': (light_wall, 'wall_cast'),
    'rpbuilding': (repair_facility, 'repair_sand'),
    'quonsetclean': (quonset_build, 'quonset_clean'),
    'crateswithtarp': (tarp_crates, 'canvas_timber'),
    'lookouttower': (lookout, 'lookout_stock'),
    'barrels': (hazard_barrels, 'hazard_yellow'),
    'quonset': (quonset_build, 'quonset_rust'),
}
ARCH_OF = {}
DAMAGE = {'razorwirefence': fence_damage, 's_lightwall': wall_damage,
          'crateswithtarp': crate_damage, 'barrels': barrel_damage}


def fit_stock(k, name):
    pts = [v for pc in k.pieces for v in pc['v']]
    lo = [min(v[i] for v in pts) for i in range(3)]
    hi = [max(v[i] for v in pts) for i in range(3)]
    scales = [SIZES[name][i] / (hi[i] - lo[i]) for i in range(3)]
    center = [(lo[i] + hi[i]) / 2 for i in range(2)] + [lo[2]]
    if geo.GAME_MESH and name in FIT_CACHE:
        # The game carrier must occupy the same surface as the bake source.
        scales, center = FIT_CACHE[name]
    elif not geo.GAME_MESH:
        FIT_CACHE[name] = (scales, center)

    def point(v):
        return tuple((v[i] - center[i]) * scales[i] for i in range(3))
    for pc in k.pieces:
        pc['v'] = [point(v) for v in pc['v']]
    for hit in k.hits:
        hit['p'] = point(hit['p'])
        hit['r'] *= sum(scales) / 3
    k.moss_sites = [tuple(point(v) for v in pair) for pair in k.moss_sites]
    for sk in k.meta.get('sockets', []):
        sk['position_m'] = point(sk['position_m'])
    k.meta['fit_scale_xyz'] = scales
    k.meta['fit_center'] = center


def build(name, condition, seed=17):
    import counts
    if name not in counts.CONTRACTS:
        raise ValueError(f'{name}: record stock model/texture counts before building')
    fn, palette = DRAFTS[name]
    k = Kit(name, palette, seed)
    k.cond = condition
    fn(k)
    # Stable ids, identical between the high source and the GAME_MESH rebuild:
    # only detail-role pieces differ, and those are never removable.
    uid = 0
    for pc in k.pieces:
        if pc.get('removable') or pc.get('pane'):
            pc['uid'] = uid
            uid += 1
    fit_stock(k, name)
    k.meta['size'] = SIZES[name]
    if condition == 'abandoned':
        geo.apply_abandoned(k, geo.LANG['conditions'][condition])
    if condition == 'destroyed' and name in BUILDINGS:
        full_rubble(k, name)
    elif condition in ('partial', 'destroyed'):
        (DAMAGE.get(name) or (lambda kk, c: peel(kk, seed)))(k, condition)
    if condition in ('partial', 'destroyed'):
        k.meta['authored_damage'] = True
        for pc in k.pieces:
            pc['v'] = [(x, y, max(.005, z)) for x, y, z in pc['v']]
        if condition == 'destroyed':
            k.pieces = [pc for pc in k.pieces if not pc.get('pane')]
    k.meta.update(name=name, stock=name, condition=condition, palette=palette,
                  size=SIZES[name], draft=True, original_geometry=True,
                  ref='alina-godfrey-battletech-urbanwarfare-props-5.jpg', round1=None,
                  construction_order=True)
    k.meta['stock_origin_offset_m'] = counts.origin(name)
    if name in ARCH_OF:
        k.meta['archetype'] = ARCH_OF[name]
    return k


import batch2  # noqa: E402: reuses the construction helpers above
SIZES.update(batch2.SIZES)
DRAFTS.update(batch2.DRAFTS)
DAMAGE.update(batch2.DAMAGE)
BUILDINGS.update(('singlecivlivinga', 'portable'))

import batch3  # noqa: E402: construction helpers and batch two are registered first
SIZES.update(batch3.SIZES)
DRAFTS.update(batch3.DRAFTS)
DAMAGE.update(batch3.DAMAGE)

import batch_son  # noqa: E402: Sonnet runner batches (building / turret / wall)
SIZES.update(batch_son.SIZES)
DRAFTS.update(batch_son.DRAFTS)
DAMAGE.update(batch_son.DAMAGE)
BUILDINGS.update(batch_son.BUILDINGS)

import archetypes  # noqa: E402: params-driven assets (assets/*.params.json) join the registries last
LEGACY = tuple(DRAFTS)
archetypes.register_all()
