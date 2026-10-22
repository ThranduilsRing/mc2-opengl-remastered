"""Pure-Python geometry for the art-directed sci-fi kit (no bpy).

Coordinates are metres, Z up, building fronts face -Y. Every primitive is
built from the same language (lang.json): ferrocrete plinth course, accent base
band, chamfered cladding panels, chamfered coping, and openings set in chamfered
reveals. Geometry is emitted as *pieces*: closed meshes tagged with a material
key ("class.slot") and a structural role (mass, skin, frame, glass, roof,
detail). Conditions act on pieces, so a missing panel is a whole panel, and a
destroyed panel tears along its own seams.
"""
import json
import math
import random
from contextlib import contextmanager
from pathlib import Path

LANG = json.loads((Path(__file__).resolve().parent / 'lang.json').read_text(encoding='utf-8'))
P = LANG['profiles']
G = LANG['grid']


# ---------------------------------------------------------------- math utils
def mat_mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]


def mat_vec(m, v):
    return tuple(m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2] for i in range(3))


def rot_z(a):
    c, s = math.cos(a), math.sin(a)
    return [[c, -s, 0], [s, c, 0], [0, 0, 1]]


def rot_x(a):
    c, s = math.cos(a), math.sin(a)
    return [[1, 0, 0], [0, c, -s], [0, s, c]]


def rot_y(a):
    c, s = math.cos(a), math.sin(a)
    return [[c, 0, s], [0, 1, 0], [-s, 0, c]]


IDENTITY = [[1, 0, 0], [0, 1, 0], [0, 0, 1]]


def rotate_about(p, origin, axis, angle):
    """Rodrigues rotation of point p about a line (origin, unit axis)."""
    ax, ay, az = axis
    n = math.sqrt(ax * ax + ay * ay + az * az) or 1.0
    ax, ay, az = ax / n, ay / n, az / n
    x, y, z = p[0] - origin[0], p[1] - origin[1], p[2] - origin[2]
    c, s = math.cos(angle), math.sin(angle)
    dot = ax * x + ay * y + az * z
    cx, cy, cz = ay * z - az * y, az * x - ax * z, ax * y - ay * x
    return (origin[0] + x * c + cx * s + ax * dot * (1 - c),
            origin[1] + y * c + cy * s + ay * dot * (1 - c),
            origin[2] + z * c + cz * s + az * dot * (1 - c))


def offset_poly(poly, d):
    """Inward offset of a convex CCW polygon; vertex i stays vertex i."""
    if d <= 0:
        return list(poly)
    n = len(poly)
    lines = []
    for i in range(n):
        a, b = poly[i], poly[(i + 1) % n]
        ex, ey = b[0] - a[0], b[1] - a[1]
        length = math.hypot(ex, ey) or 1e-9
        nx, ny = -ey / length, ex / length
        lines.append(((a[0] + nx * d, a[1] + ny * d), (ex, ey)))
    out = []
    for i in range(n):
        (p1, d1), (p2, d2) = lines[i - 1], lines[i]
        den = d1[0] * d2[1] - d1[1] * d2[0]
        if abs(den) < 1e-12:
            out.append(p2)
            continue
        t = ((p2[0] - p1[0]) * d2[1] - (p2[1] - p1[1]) * d2[0]) / den
        out.append((p1[0] + t * d1[0], p1[1] + t * d1[1]))
    return out


def rect_poly(x0, y0, x1, y1, ch=0.0):
    ch = min(ch, (x1 - x0) * .45, (y1 - y0) * .45)
    if ch <= 1e-6:
        return [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]
    return [(x0 + ch, y0), (x1 - ch, y0), (x1, y0 + ch), (x1, y1 - ch), (x1 - ch, y1), (x0 + ch, y1), (x0, y1 - ch), (x0, y0 + ch)]


def reg_poly(r, n, phase=None, cx=0.0, cy=0.0):
    phase = math.pi / n if phase is None else phase
    return [(cx + r * math.cos(phase + i * math.tau / n), cy + r * math.sin(phase + i * math.tau / n)) for i in range(n)]


def subtract_rect(r, holes):
    """Axis-aligned rect minus rect list -> list of rects (x0,z0,x1,z1)."""
    out = [r]
    for h in holes:
        nxt = []
        for a in out:
            x0, z0, x1, z1 = a
            hx0, hz0, hx1, hz1 = h
            if hx1 <= x0 or hx0 >= x1 or hz1 <= z0 or hz0 >= z1:
                nxt.append(a)
                continue
            if hx0 > x0:
                nxt.append((x0, z0, hx0, z1))
            if hx1 < x1:
                nxt.append((hx1, z0, x1, z1))
            ix0, ix1 = max(x0, hx0), min(x1, hx1)
            if hz0 > z0:
                nxt.append((ix0, z0, ix1, hz0))
            if hz1 < z1:
                nxt.append((ix0, hz1, ix1, z1))
        out = [a for a in nxt if a[2] - a[0] > 0.02 and a[3] - a[1] > 0.02]
    return out


# ---------------------------------------------------------------- the kit
class Kit:
    def __init__(self, name, palette, seed=17):
        self.name, self.palette, self.seed = name, palette, seed
        self.rng = random.Random(seed)
        self.pieces = []
        self.M, self.O = IDENTITY, (0.0, 0.0, 0.0)
        self.hits = []          # damage recipes for the Blender stage
        self.moss_sites = []    # (p0, p1) ground/roof lines where vegetation can grow
        self.glass_sills = []
        self.meta = {}
        self.group = None

    # transforms --------------------------------------------------------
    @contextmanager
    def at(self, origin=(0, 0, 0), yaw=0.0, pitch=0.0, roll=0.0):
        oldM, oldO = self.M, self.O
        local = mat_mul(rot_z(yaw), mat_mul(rot_x(pitch), rot_y(roll)))
        o = mat_vec(oldM, origin)
        self.O = (oldO[0] + o[0], oldO[1] + o[1], oldO[2] + o[2])
        self.M = mat_mul(oldM, local)
        try:
            yield
        finally:
            self.M, self.O = oldM, oldO

    @contextmanager
    def tagged(self, group):
        old = self.group
        self.group = group
        try:
            yield
        finally:
            self.group = old

    def world(self, p):
        v = mat_vec(self.M, p)
        return (self.O[0] + v[0], self.O[1] + v[1], self.O[2] + v[2])

    def add(self, verts, faces, mat, role='detail', **flags):
        piece = dict(v=[self.world(p) for p in verts], f=[tuple(f) for f in faces], m=mat, role=role,
                     group=flags.pop('group', self.group), **flags)
        self.pieces.append(piece)
        return piece

    # shapes -------------------------------------------------------------
    def prism(self, poly, z0, z1, mat, role='detail', top=0.0, bot=0.0, axis='z', top_h=None, bot_h=None, **flags):
        """Extrude a CCW polygon from z0 to z1 with optional chamfer insets at either end.

        axis 'z': poly in (x,y); 'y': poly in (x,z) extruded along y (bot = front at z0);
        'x': poly in (y,z) extruded along x.
        """
        h = z1 - z0
        if h <= 1e-6:
            return None
        span = min(max(p[0] for p in poly) - min(p[0] for p in poly), max(p[1] for p in poly) - min(p[1] for p in poly))
        top = min(top, span * .45 if top_h else h * .45, span * .40 if not top_h else span * .45)
        bot = min(bot, h * .45, span * .40)
        th = min(top_h, h) if top_h else top
        bh = min(bot_h, h - th) if bot_h else bot
        rings = []
        if bot > 1e-6:
            rings += [(offset_poly(poly, bot), z0), (poly, z0 + bh)]
        else:
            rings += [(poly, z0)]
        if top > 1e-6:
            if z1 - th > rings[-1][1] + 1e-6:
                rings += [(poly, z1 - th)]
            rings += [(offset_poly(poly, top), z1)]
        else:
            rings += [(poly, z1)]
        n = len(poly)
        verts, faces = [], []
        for ring, w in rings:
            for u, v in ring:
                verts.append((u, v, w))
        faces.append(tuple(reversed(range(n))))
        for k in range(len(rings) - 1):
            a, b = k * n, (k + 1) * n
            for i in range(n):
                j = (i + 1) % n
                faces.append((a + i, a + j, b + j, b + i))
        last = (len(rings) - 1) * n
        faces.append(tuple(range(last, last + n)))
        if axis == 'y':
            verts = [(u, w, v) for u, v, w in verts]
            faces = [tuple(reversed(f)) for f in faces]
        elif axis == 'x':
            verts = [(w, u, v) for u, v, w in verts]
        return self.add(verts, faces, mat, role, **flags)

    def box(self, c, s, mat, role='detail', ch=0.0, top=0.0, bot=0.0, **flags):
        """Box centred at c, size s, vertical-edge chamfer ch and top/bottom chamfers."""
        x, y, z = c
        w, d, h = s
        return self.prism(rect_poly(x - w / 2, y - d / 2, x + w / 2, y + d / 2, ch), z - h / 2, z + h / 2, mat, role, top, bot, **flags)

    def slab(self, x0, x1, z0, z1, yf, yb, mat, role='skin', front=0.0, back=0.0, ch=0.0, **flags):
        """Wall-plane slab: rect in (x,z), front face at yf with chamfer `front`."""
        return self.prism(rect_poly(x0, z0, x1, z1, ch), yf, yb, mat, role, top=back, bot=front, axis='y', **flags)

    def cyl(self, c, r, h, mat, role='detail', sides=16, top=0.0, bot=0.0, **flags):
        x, y, z = c
        return self.prism(reg_poly(r, sides, cx=x, cy=y), z - h / 2, z + h / 2, mat, role, top, bot, **flags)

    def extrude(self, profile, w0, w1, mat, role='detail', axis='x', **flags):
        """Extrude an arbitrary (possibly concave) CCW profile; profile in (y,z) for axis x."""
        return self.prism(profile, w0, w1, mat, role, axis=axis, **flags)

    def beam(self, a, b, w, mat='metal', role='frame', h=None, seg=1, **flags):
        h = h or w
        dx, dy, dz = b[0] - a[0], b[1] - a[1], b[2] - a[2]
        length = math.sqrt(dx * dx + dy * dy + dz * dz)
        if length < 1e-6:
            return None
        t = (dx / length, dy / length, dz / length)
        up = (0, 0, 1) if abs(t[2]) < .95 else (1, 0, 0)
        u = (t[1] * up[2] - t[2] * up[1], t[2] * up[0] - t[0] * up[2], t[0] * up[1] - t[1] * up[0])
        un = math.sqrt(sum(c * c for c in u))
        u = tuple(c / un for c in u)
        v = (t[1] * u[2] - t[2] * u[1], t[2] * u[0] - t[0] * u[2], t[0] * u[1] - t[1] * u[0])
        verts, faces = [], []
        for k in range(seg + 1):
            f = k / seg
            p = (a[0] + dx * f, a[1] + dy * f, a[2] + dz * f)
            for s1, s2 in [(-1, -1), (1, -1), (1, 1), (-1, 1)]:
                verts.append(tuple(p[i] + s1 * w / 2 * u[i] + s2 * h / 2 * v[i] for i in range(3)))
        faces.append((3, 2, 1, 0))
        for k in range(seg):
            a0, b0 = k * 4, (k + 1) * 4
            for i in range(4):
                j = (i + 1) % 4
                faces.append((a0 + i, a0 + j, b0 + j, b0 + i))
        faces.append((seg * 4, seg * 4 + 1, seg * 4 + 2, seg * 4 + 3))
        return self.add(verts, faces, mat, role, **flags)

    def tube(self, pts, r, mat='metal', role='frame', sides=8, **flags):
        """Polyline tube (pipes, rails, cables) with mitred rings."""
        verts, faces = [], []
        n = len(pts)
        for k, p in enumerate(pts):
            a = pts[max(0, k - 1)]
            b = pts[min(n - 1, k + 1)]
            t = [b[i] - a[i] for i in range(3)]
            tl = math.sqrt(sum(c * c for c in t)) or 1
            t = [c / tl for c in t]
            up = (0, 0, 1) if abs(t[2]) < .95 else (1, 0, 0)
            u = (t[1] * up[2] - t[2] * up[1], t[2] * up[0] - t[0] * up[2], t[0] * up[1] - t[1] * up[0])
            ul = math.sqrt(sum(c * c for c in u))
            u = [c / ul for c in u]
            v = (t[1] * u[2] - t[2] * u[1], t[2] * u[0] - t[0] * u[2], t[0] * u[1] - t[1] * u[0])
            for i in range(sides):
                ang = i * math.tau / sides
                verts.append(tuple(p[j] + r * (math.cos(ang) * u[j] + math.sin(ang) * v[j]) for j in range(3)))
        faces.append(tuple(reversed(range(sides))))
        for k in range(n - 1):
            for i in range(sides):
                j = (i + 1) % sides
                faces.append((k * sides + i, k * sides + j, (k + 1) * sides + j, (k + 1) * sides + i))
        faces.append(tuple(range((n - 1) * sides, n * sides)))
        return self.add(verts, faces, mat, role, **flags)

    # ------------------------------------------------------------ language
    def plinth(self, x0, x1, yf=0.0, depth=None, h=None, mat='ferro'):
        """Ferrocrete base course: proud of the wall face, chamfered top front edge."""
        h = h or P['plinth']['height']
        proud = P['plinth']['proud']
        ch = P['plinth']['top_chamfer']
        yb = yf + (depth or G['wall_thickness_m'])
        prof = [(yf - proud, 0.0), (yb, 0.0), (yb, h), (yf - proud + ch, h), (yf - proud, h - ch)]
        self.extrude(prof, x0, x1, mat, 'mass', plinth=True)
        self.moss_sites.append(((x0, yf - proud, 0.0), (x1, yf - proud, 0.0)))

    def base_band(self, x0, x1, yf=0.0, z=None):
        z = z if z is not None else P['plinth']['height']
        hb = P['base_band']['height']
        self.slab(x0, x1, z, z + hb, yf - .03, yf + .06, 'paint.accent', 'detail', front=.012)

    def coping(self, x0, x1, z, yf=0.0, depth=None, mat='paint.trim'):
        oh, h, ch = P['coping']['overhang'], P['coping']['height'], P['coping']['chamfer']
        yb = yf + (depth or G['wall_thickness_m']) + oh * .4
        prof = [(yf - oh, z + ch * .5), (yf - oh + ch * .5, z), (yb, z), (yb, z + h), (yf - oh + ch, z + h), (yf - oh, z + h - ch)]
        self.extrude(prof, x0 - oh * .3, x1 + oh * .3, mat, 'mass', coping=True)

    def panels(self, x0, x1, z0, z1, yf, mat='paint.main', inset_mat='paint.second', rows=2, sub=None, holes=(), style='plate', removable=True):
        """Cladding: chamfered plates on the module sub-grid, cut around openings."""
        sub = sub or G['sub_m'] * 2
        rows = max(rows, round((z1 - z0) / 2.6))
        seam = P['panel']['seam']
        cols = max(1, round((x1 - x0) / sub))
        cw = (x1 - x0) / cols
        rh = (z1 - z0) / rows
        t = .07
        for r in range(rows):
            for c in range(cols):
                cell = (x0 + c * cw, z0 + r * rh, x0 + (c + 1) * cw, z0 + (r + 1) * rh)
                for a in subtract_rect(cell, holes):
                    ax0, az0, ax1, az1 = a[0] + seam / 2, a[1] + seam / 2, a[2] - seam / 2, a[3] - seam / 2
                    if ax1 - ax0 < .12 or az1 - az0 < .12:
                        continue
                    if style == 'corr':
                        self.corr_sheet(ax0, ax1, az0, az1, yf - .02, mat, removable=removable)
                        continue
                    self.slab(ax0, ax1, az0, az1, yf - t, yf + .01, mat, 'skin', front=P['panel']['face_chamfer'], removable=removable)
                    full = abs((ax1 - ax0) - (cw - seam)) < .01 and abs((az1 - az0) - (rh - seam)) < .01
                    if style == 'inset' and full and (r + c) % 2 == 0:
                        m = .16
                        self.slab(ax0 + m, ax1 - m, az0 + m, az1 - m, yf - t - .025, yf - t + .01, inset_mat, 'skin', front=.02, removable=False, rides=True)
        if style != 'corr':
            for r in range(1, rows):
                z = z0 + r * rh
                for a in subtract_rect((x0, z - .07, x1, z + .07), holes):
                    self.slab(a[0], a[2], a[1], a[3], yf - t - .05, yf + .01, 'paint.trim', 'frame', front=.03)

    def corr_sheet(self, x0, x1, z0, z1, yf, mat, removable=True, pitch=.152, amp=.028, vertical=True):
        """Real corrugation geometry: a waved sheet with thickness (vertical ribs)."""
        n = max(4, int((x1 - x0) / pitch * 4))
        rowsz = max(2, int((z1 - z0) / 0.75) + 1)
        verts, faces = [], []
        for layer, off in [(0, 0.0), (1, .012)]:
            for j in range(rowsz):
                z = z0 + (z1 - z0) * j / (rowsz - 1)
                for i in range(n + 1):
                    x = x0 + (x1 - x0) * i / n
                    y = yf - amp * (.5 + .5 * math.sin(i / 4 * math.tau)) + off
                    verts.append((x, y, z))
        stride = n + 1
        layer = stride * rowsz
        for j in range(rowsz - 1):
            for i in range(n):
                a = j * stride + i
                faces.append((a, a + 1, a + stride + 1, a + stride))
                faces.append((layer + a, layer + a + stride, layer + a + stride + 1, layer + a + 1))
        # close the edges so the sheet is a solid
        for j in range(rowsz - 1):
            for i in (0, n):
                a, b = j * stride + i, (j + 1) * stride + i
                f = (a, b, layer + b, layer + a)
                faces.append(f if i == 0 else tuple(reversed(f)))
        for j in (0, rowsz - 1):
            for i in range(n):
                a = j * stride + i
                f = (a, layer + a, layer + a + 1, a + 1)
                faces.append(f if j == 0 else tuple(reversed(f)))
        return self.add(verts, faces, mat, 'skin', removable=removable)

    def pilaster(self, x, z0, z1, yf=0.0, w=.36, proud=.16, mat='paint.trim'):
        self.slab(x - w / 2, x + w / 2, z0, z1, yf - proud, yf + .05, mat, 'frame', front=.06, ch=0.0)

    def frame_grid(self, x0, x1, z0, z1, ymid, step=1.5):
        """Hidden structural steel inside the wall core; exposed by damage."""
        n = max(1, round((x1 - x0) / step))
        for i in range(n + 1):
            x = x0 + (x1 - x0) * i / n
            self.beam((x, ymid, z0), (x, ymid, z1), .16, 'metal', 'frame', h=.10, seg=max(2, int((z1 - z0) / .4)))
        for z in [z0 + (z1 - z0) * .5, z1 - .1]:
            self.beam((x0, ymid, z), (x1, ymid, z), .14, 'metal', 'frame', h=.10, seg=max(2, int((x1 - x0) / .4)))

    def reveal(self, x0, x1, z0, z1, yf=0.0, depth=None, jamb=None, mat='paint.trim', head=True, sill=True):
        """Chamfered frame around an opening: jambs, head, sill, all proud of the face."""
        depth = depth or P['reveal']['depth']
        j = jamb or P['reveal']['jamb']
        ch = P['reveal']['chamfer']
        for x in (x0 - j / 2, x1 + j / 2):
            self.slab(x - j / 2, x + j / 2, z0 - (j if sill else 0), z1 + (j if head else 0), yf - .10, yf + depth, mat, 'frame', front=ch)
        if head:
            self.slab(x0, x1, z1, z1 + j, yf - .10, yf + depth, mat, 'frame', front=ch)
        if sill:
            self.slab(x0 - .05, x1 + .05, z0 - j, z0, yf - .16, yf + depth, 'ferro', 'frame', front=ch)
        # opening liner (dark interior returns)
        self.slab(x0, x1, z0, z1, yf + depth - .02, yf + depth + .06, 'rubber', 'mass')

    def window(self, x0, x1, z0, z1, yf=0.0, mullions=1, shutter=False):
        self.reveal(x0, x1, z0, z1, yf)
        gy = yf + P['reveal']['depth'] * .55
        n = mullions + 1
        w = (x1 - x0) / n
        for i in range(n):
            self.slab(x0 + i * w + .03, x0 + (i + 1) * w - .03, z0 + .03, z1 - .03, gy, gy + .03, 'glass', 'glass', pane=True)
        for i in range(1, n):
            x = x0 + i * w
            self.slab(x - .04, x + .04, z0, z1, gy - .06, gy + .05, 'paint.trim', 'frame')
        self.glass_sills.append(((x0, gy - .3, z0), (x1, gy - .3, z0)))
        # brow hood, Godfrey style chamfered overhang
        self.slab(x0 - .35, x1 + .35, z1 + P['reveal']['jamb'], z1 + P['reveal']['jamb'] + .14, yf - .38, yf + .02, 'paint.trim', 'frame', front=.05)
        if shutter:
            self.slab(x0 - .05, x1 + .05, z1 - .04, z1 + .06, yf - .22, yf - .12, 'paint.second', 'skin')

    def lamp_head(self, x, z, y=0.0, w=.55, color='lamp'):
        self.box((x, y - .30, z), (w, .42, .24), 'paint.trim', 'detail', ch=.06, bot=.05)
        self.slab(x - w * .38, x + w * .38, z - .12, z - .09, y - .45, y - .16, color, 'detail')
        self.beam((x, y - .1, z), (x, y + .02, z), .10, 'metal', 'detail')

    def bollard(self, x, y, h=1.0, r=.22):
        poly = reg_poly(r, 8, cx=x, cy=y)
        self.prism(poly, 0, h, 'paint.trim', 'detail', top=.07)
        self.prism(reg_poly(r + .015, 8, cx=x, cy=y), h * .62, h * .74, 'hazard', 'detail')

    def hazard_strip(self, x0, x1, y0, y1, z=0.0, h=.04):
        self.box(((x0 + x1) / 2, (y0 + y1) / 2, z + h / 2), (x1 - x0, y1 - y0, h), 'hazard', 'detail', top=.015)

    def hvac(self, w=1.8, d=1.4, h=1.0):
        """Rooftop unit after the Godfrey C6 language: chamfered box + fan drum + louvre face."""
        self.box((0, 0, .06), (w + .2, d + .2, .12), 'metal', 'detail', ch=.05)
        self.box((0, 0, .12 + h / 2), (w, d, h), 'paint.main', 'detail', ch=.10, top=.08)
        self.box((0, 0, .17), (w + .02, d + .02, .10), 'paint.accent', 'detail', ch=.10)
        self.slab(-w * .38, w * .38, .28, .12 + h * .78, -d / 2 - .02, -d / 2 + .03, 'paint.second', 'detail', front=.025)
        for i in range(5):
            z = .34 + i * (h * .78 - .30) / 5
            self.slab(-w * .34, w * .34, z, z + .05, -d / 2 - .06, -d / 2 + .02, 'paint.trim', 'detail')
        r = min(w, d) * .30
        self.cyl((0, d * .05, .12 + h + .09), r + .06, .18, 'paint.trim', 'detail', sides=16, top=.04)
        self.cyl((0, d * .05, .12 + h + .16), r * .85, .05, 'grate', 'detail', sides=16)

    def mast(self, h=3.0, w=.22):
        """Godfrey A2: chamfered slab mast on a splayed footing with an accent band and red tip."""
        self.box((0, 0, .08), (w * 3.2, w * 2.4, .16), 'paint.trim', 'detail', ch=.08, top=.05)
        self.box((0, 0, .16 + h * .2), (w * 1.6, w * 1.2, h * .4), 'paint.main', 'detail', ch=.06, top=.10)
        self.box((0, 0, .16 + h * .6), (w, w * .8, h * .8), 'paint.main', 'detail', ch=.05, top=.12)
        self.box((0, 0, .16 + h * .42), (w * 1.05, w * .85, .08), 'paint.accent', 'detail', ch=.05)
        self.cyl((0, 0, .16 + h + .05), .04, .12, 'lamp_red', 'detail', sides=6)


# ---------------------------------------------------------------- wall segments
def wall_segment(k, x0, x1, h, openings=(), style='inset', mat='paint.main', pilasters=True, plinth=True):
    """The shared wall module. openings: list of (ox0, oz0, ox1, oz1) in wall coordinates."""
    ph = P['plinth']['height'] if plinth else 0.0
    t = G['wall_thickness_m']
    holes = [(a, b, c, d) for a, b, c, d in openings]
    # plinth, interrupted by ground-level openings
    if plinth:
        xs = [(x0, x1)]
        for a, b, c, d in holes:
            if b < ph:
                nxt = []
                for s, e in xs:
                    if c <= s or a >= e:
                        nxt.append((s, e))
                        continue
                    if a - .25 > s:
                        nxt.append((s, a - .25))
                    if c + .25 < e:
                        nxt.append((c + .25, e))
                xs = nxt
        for s, e in xs:
            k.plinth(s, e, 0.0)
        k.base_band(x0, x1, 0.0, ph) if not holes else [k.base_band(s, e, 0.0, ph) for s, e in xs]
    top = h - P['coping']['height']
    core = (x0, ph, x1, top)
    for a in subtract_rect(core, [(o[0] - .25, o[1] - (.25 if o[1] > ph else 0), o[2] + .25, o[3] + .25) for o in holes]):
        k.slab(a[0], a[2], a[1], a[3], .04, t, 'ferro', 'mass', rebar=True)
    k.frame_grid(x0 + .2, x1 - .2, ph, top, t * .55)
    if style:
        k.panels(x0 + .02, x1 - .02, ph + P['base_band']['height'] + .02, top - .02, 0.0, mat,
                 holes=[(o[0] - .30, o[1] - .30, o[2] + .30, o[3] + .30) for o in holes], style=style)
    if pilasters:
        for x in (x0 + .18, x1 - .18):
            k.pilaster(x, ph, top)
    k.coping(x0, x1, top, 0.0)


# ---------------------------------------------------------------- primitives
def p_wall_panel(k):
    w, h = 4.5, 4.4
    wall_segment(k, -w / 2, w / 2, h, style='inset')
    k.pilaster(0.0, .6, h - .3, w=.30, proud=.12)
    k.hits.append(dict(p=(.55, 0.0, 2.5), d=(0, 1, 0), r=1.0, depth=.45, sag=.25, rebar=True, debris=['ferro', 'paint.main', 'metal']))
    k.meta.update(size=(w, 1.0, h))


def p_sloped_wall(k):
    """Barracks battered wall: sloped ferro mass with recessed sage panels and slit windows."""
    w, h = 6.0, 4.0
    ph = P['plinth']['height']
    tilt = math.radians(24)
    batter = (h - ph - .3) * math.tan(tilt)
    t = G['wall_thickness_m']
    k.plinth(-w / 2, w / 2, 0.0, depth=batter + t)
    zt = h - .3
    prof = [(0.0, ph), (batter + t, ph), (batter + t, zt), (batter, zt)]
    k.extrude(prof, -w / 2, w / 2, 'ferro', 'mass', rebar=True)
    k.coping(-w / 2, w / 2, zt, batter)
    slant = (zt - ph) / math.cos(tilt)
    with k.at((0, 0, ph), pitch=-tilt):
        # local wall plane: x along, z up the slope, -y out of the face
        for i, cx in enumerate([-1.5, 1.5]):
            x0, x1 = cx - 1.15, cx + 1.15
            z0, z1 = .35, slant - .35
            # proud trim frame with rivets
            for (a, b, c, d) in [(x0 - .12, z0 - .12, x1 + .12, z0), (x0 - .12, z1, x1 + .12, z1 + .12), (x0 - .12, z0, x0, z1), (x1, z0, x1 + .12, z1)]:
                k.slab(a, c, b, d, -.06, .02, 'paint.trim', 'frame', front=.025)
            wz0, wz1 = slant * .52, slant * .52 + .42
            k.slab(x0, x1, z0, wz0 - .14, -.02, .03, 'paint.second', 'skin', front=.02, removable=True)
            k.slab(x0, x1, wz1 + .14, z1, -.02, .03, 'paint.second', 'skin', front=.02, removable=True)
            for (a, c) in [(x0, x0 + .25), (x1 - .25, x1)]:
                k.slab(a, c, wz0 - .14, wz1 + .14, -.02, .03, 'paint.second', 'skin', front=.02)
            k.slab(x0 + .25, x1 - .25, wz0 - .14, wz0, -.03, .03, 'paint.trim', 'frame')
            k.slab(x0 + .25, x1 - .25, wz1, wz1 + .14, -.08, .03, 'paint.trim', 'frame', front=.03)
            k.slab(x0 + .25, x1 - .25, wz0, wz1, -.005, .012, 'glass', 'glass', pane=True)
            k.slab(x0 + .25, x1 - .25, wz0, wz1, .015, .03, 'rubber', 'detail')
            for xx in [x0 + .25 + (x1 - x0 - .5) / 3, x0 + .25 + 2 * (x1 - x0 - .5) / 3]:
                k.slab(xx - .035, xx + .035, wz0, wz1, -.03, .02, 'paint.trim', 'frame')
            for zz in [z0 - .06, z1 + .06]:
                for j in range(9):
                    xx = x0 + (x1 - x0) * j / 8
                    k.box((xx, -.07, zz), (.05, .03, .05), 'metal', 'detail')
        k.slab(-.10, .10, .2, slant - .2, -.04, .03, 'paint.trim', 'frame', front=.02)
    k.hits.append(dict(p=(-1.4, batter * .5, 2.2), d=(0, .9, -.43), r=1.0, depth=.30, sag=.12, rebar=True, debris=['ferro', 'paint.second']))
    k.meta.update(size=(w, batter + 1, h))


def p_quonset_shell(k, length=9.0, radius=5.0, waves=None, end_walls=False):
    """Stock-identity quonset barrel: rust corrugated sheets on steel arch ribs, ferro curbs."""
    bays = max(1, round(length / G['bay_m']))
    L = bays * G['bay_m']
    curb = .45
    for s in (-1, 1):
        prof = [(-.35, 0), (.35, 0), (.35, curb), (-.20, curb), (-.35, curb - .15)] if s < 0 else None
        with k.at((s * radius, 0, 0), yaw=-math.pi / 2 if s < 0 else math.pi / 2):
            k.plinth(-L / 2, L / 2, -.10, depth=.5, h=curb)
    zc = curb
    nth = 200
    amp = .045
    rows = 7
    for b in range(bays):
        y0, y1 = -L / 2 + b * G['bay_m'] + .02, -L / 2 + (b + 1) * G['bay_m'] - .02
        verts, faces = [], []
        for layer, off in [(0, 0.0), (1, -.014)]:
            for j in range(rows):
                y = y0 + (y1 - y0) * j / (rows - 1)
                for i in range(nth + 1):
                    th = math.pi * i / nth
                    r = radius + off + amp * (.5 + .5 * math.sin(i * math.tau / 4))
                    verts.append((r * math.cos(th), y, zc + r * math.sin(th)))
        stride = nth + 1
        lay = stride * rows
        for j in range(rows - 1):
            for i in range(nth):
                a = j * stride + i
                faces.append((a, a + stride, a + stride + 1, a + 1))
                faces.append((lay + a, lay + a + 1, lay + a + stride + 1, lay + a + stride))
        for j in range(rows - 1):
            for i in (0, nth):
                a, c = j * stride + i, (j + 1) * stride + i
                f = (a, lay + a, lay + c, c)
                faces.append(f if i == 0 else tuple(reversed(f)))
        for j in (0, rows - 1):
            for i in range(nth):
                a = j * stride + i
                f = (a, a + 1, lay + a + 1, lay + a)
                faces.append(f if j == 0 else tuple(reversed(f)))
        k.add(verts, faces, 'corr.corr', 'roof', removable=True, sheet=b)
        # lap strap over each sheet joint
    for b in range(bays + 1):
        y = -L / 2 + b * G['bay_m']
        pts = [((radius + .07) * math.cos(math.pi * i / 24), y, zc + (radius + .07) * math.sin(math.pi * i / 24)) for i in range(25)]
        k.tube(pts, .05, 'paint.trim', 'detail', sides=6)
        # arch ribs inside: the frame that survives
        pts = [((radius - .22) * math.cos(math.pi * i / 24), y, zc + (radius - .22) * math.sin(math.pi * i / 24)) for i in range(25)]
        for a, c in zip(pts, pts[1:]):
            k.beam(a, c, .18, 'metal', 'frame', h=.30, seg=1)
    # ridge vent cap and side windows (stock: square olive-framed windows low on the barrel)
    k.box((0, 0, zc + radius + .10), (.5, L - .3, .16), 'paint.trim', 'detail', ch=.05, top=.05)
    for b in range(bays):
        y = -L / 2 + (b + .5) * G['bay_m']
        for s in (-1, 1):
            th = math.radians(28)
            # dormer box: vertical window frame emerging from the barrel
            x = s * (radius * math.cos(th) - .05)
            z = zc + radius * math.sin(th)
            with k.at((x, y, z - .55), yaw=s * math.pi / 2):
                k.box((0, .30, .55), (1.2, .8, 1.15), 'corr.corr', 'skin', top=.0)
                k.slab(-.42, .42, .20, .95, -.12, .02, 'paint.second', 'frame', front=.03)
                k.slab(-.33, .33, .28, .87, -.06, -.02, 'glass', 'glass', pane=True)
                k.slab(-.02, .02, .28, .87, -.10, -.04, 'paint.second', 'frame')
                k.slab(-.33, .33, .56, .60, -.10, -.04, 'paint.second', 'frame')
                k.slab(-.55, .55, 1.08, 1.18, -.25, .55, 'paint.trim', 'frame', front=.03)
    if end_walls:
        pass
    k.hits.append(dict(p=(-1.6, 1.5, zc + radius * .9), d=(.3, 0, -1), r=1.15, depth=.55, sag=.5, reach=1.5, rebar=False, debris=['corr.corr', 'metal']))
    k.meta.update(size=(radius * 2 + 1, L, radius + curb))


def door_wall(k, w, h, ow, oh, z0=0.0, style='inset', mat='paint.main'):
    wall_segment(k, -w / 2, w / 2, h, openings=[(-ow / 2, z0, ow / 2, z0 + oh)], style=style, mat=mat)


def p_bay_door(k):
    """Vehicle bay: roll-up door set in a factory corrugated wall (stock factorymd identity)."""
    W, H, ow, oh = 9.0, 6.4, 4.8, 4.6
    door_wall(k, W, H, ow, oh, style='corr', mat='corr.main')
    k.reveal(-ow / 2, ow / 2, 0.0, oh, 0.0, depth=.45, sill=False)
    # roll-up curtain: real slats, recessed behind the reveal
    n = int(oh / .28)
    for i in range(n):
        z = .04 + i * (oh - .1) / n
        k.slab(-ow / 2 + .02, ow / 2 - .02, z, z + (oh - .1) / n - .025, .26, .34, 'paint.second', 'skin', front=.03, removable=True)
    k.slab(-ow / 2, ow / 2, 0, .12, .22, .34, 'rubber', 'detail')
    # hood box (roller housing), guide channels, hazard threshold, apron, bollards, lamp
    k.slab(-ow / 2 - .45, ow / 2 + .45, oh + .30, oh + .95, -.55, .02, 'paint.trim', 'frame', front=.16)
    k.slab(-ow / 2 - .45, ow / 2 + .45, oh + .44, oh + .56, -.57, -.50, 'paint.accent', 'detail')
    for s in (-1, 1):
        k.slab(s * ow / 2 - .08, s * ow / 2 + .08, 0, oh + .3, .10, .36, 'metal', 'frame')
        k.bollard(s * (ow / 2 + .55), -1.0, 1.05)
        k.lamp_head(s * (ow / 2 + .7), oh + .6, 0.0)
    k.slab(-ow / 2 - .3, ow / 2 + .3, -.02, .05, -2.2, .30, 'ferro', 'mass', front=.0)
    k.box((0, -2.23, .02), (ow + .5, .12, .08), 'ferro', 'mass', top=.03)
    k.hazard_strip(-ow / 2, ow / 2, -.42, -.10, .05)
    k.hits.append(dict(p=(1.3, 0.2, 2.6), d=(0, 1, 0), r=1.25, depth=.55, sag=.35, rebar=True, debris=['paint.second', 'corr.main', 'ferro']))
    k.meta.update(size=(W, 2.8, H))


def p_mech_bay_door(k):
    """Mech bay: massive chamfered pylons, girder lintel, armoured leaves (round-1 slate)."""
    W, H, ow, oh = 19.5, 16.5, 10.0, 13.0
    door_wall(k, W, H, ow, oh, style='inset')
    pw = 1.5
    for s in (-1, 1):
        x = s * (ow / 2 + pw / 2 + .05)
        poly = rect_poly(x - pw / 2, -1.3, x + pw / 2, .3, .32)
        k.prism(poly, 0, oh + 1.4, 'paint.main', 'frame', top=.35)
        k.prism(rect_poly(x - pw / 2 - .25, -1.55, x + pw / 2 + .25, .35, .35), 0, 1.1, 'ferro', 'mass', top=.22)
        k.slab(x - .18, x + .18, 1.5, oh - .5, -1.34, -1.2, 'paint.accent', 'detail', front=.03)
        k.prism(rect_poly(x - pw / 2 - .26, -1.56, x + pw / 2 + .26, .36, .36), .78, 1.1, 'hazard', 'detail')
        k.lamp_head(x, oh + 1.8, -1.0, w=.7)
    # girder lintel with hazard face band
    k.slab(-ow / 2 - pw, ow / 2 + pw, oh + .05, oh + 1.55, -1.25, .30, 'paint.trim', 'frame', front=.30)
    k.slab(-ow / 2, ow / 2, oh + .25, oh + .55, -1.29, -1.2, 'hazard', 'detail')
    for i in range(7):
        x = -ow / 2 + .7 + i * (ow - 1.4) / 6
        k.cyl((x, -1.30, oh + 1.25), .12, .12, 'lamp_red' if i % 3 == 0 else 'paint.second', 'detail', sides=8)
    # leaves: two armoured leaves of chamfered horizontal plates, recessed
    for s in (-1, 1):
        x0, x1 = (0.02, ow / 2 - .05) if s > 0 else (-ow / 2 + .05, -0.02)
        n = 5
        for i in range(n):
            z0 = .1 + i * (oh - .1) / n
            z1 = z0 + (oh - .1) / n - .06
            k.slab(x0, x1, z0, z1, .05, .32, 'paint.second' if i % 2 else 'paint.main', 'skin', front=.10, removable=True)
            k.slab(x0 + .3, x1 - .3, z0 + .35, z0 + .55, -.01, .06, 'paint.trim', 'detail')
    k.slab(-.05, .05, 0, oh, -.02, .3, 'rubber', 'detail')
    # threshold ramp with chevrons and guide rails
    k.extrude([(-3.0, 0), (.3, 0), (.3, .16), (-2.9, .03)], -ow / 2, ow / 2, 'ferro', 'mass')
    k.hazard_strip(-ow / 2, ow / 2, -.6, -.2, .12)
    for x in (-ow / 4, ow / 4):
        k.box((x, -1.6, .17), (.18, 2.6, .07), 'metal', 'detail')
    k.hits.append(dict(p=(2.4, 0.15, 7.5), d=(0, 1, 0), r=2.7, depth=1.2, sag=.8, rebar=True, debris=['paint.main', 'paint.second', 'metal']))
    k.meta.update(size=(W, 4.0, H + 2))


def p_infantry_door(k):
    W, H, ow, oh = 4.5, 4.4, 1.3, 2.45
    door_wall(k, W, H, ow, oh, z0=.45, style='inset')
    k.reveal(-ow / 2, ow / 2, .45, .45 + oh, 0.0, sill=False)
    k.slab(-ow / 2 + .02, ow / 2 - .02, .45, .45 + oh, .14, .22, 'paint.second', 'skin', front=.03)
    k.slab(-ow / 2 + .25, ow / 2 - .25, .45 + oh * .62, .45 + oh * .86, .10, .14, 'glass', 'glass', pane=True)
    k.slab(-ow / 2 + .1, ow / 2 - .1, .50, .78, .10, .14, 'metal', 'detail')
    k.box((ow / 2 + .45, -.08, 1.55), (.22, .12, .32), 'paint.accent', 'detail', ch=.03)
    # canopy hood on two chamfered brackets, lamp under it
    k.slab(-ow / 2 - .6, ow / 2 + .6, .45 + oh + .38, .45 + oh + .58, -1.05, .02, 'paint.trim', 'frame', front=.12)
    k.slab(-ow / 2 - .6, ow / 2 + .6, .45 + oh + .58, .45 + oh + .66, -1.0, -.6, 'paint.accent', 'detail')
    for s in (-1, 1):
        with k.at((s * (ow / 2 + .5), 0, .45 + oh + .38)):
            k.extrude([(-.9, 0), (0, -.55), (0, 0)], -.06, .06, 'paint.trim', 'frame')
    k.lamp_head(0, .45 + oh + .30, -.35, w=.45)
    # steps: two ferro treads with chamfered nosings and a hazard edge
    for i, (d, z) in enumerate([(1.3, .45), (.75, .225)]):
        k.slab(-ow / 2 - .45, ow / 2 + .45, 0, z, -d, .02, 'ferro', 'mass', front=.04)
        k.slab(-ow / 2 - .45, ow / 2 + .45, z - .05, z, -d - .01, -d + .07, 'hazard', 'detail')
    for s in (-1, 1):
        x = s * (ow / 2 + .38)
        k.tube([(x, -1.25, .3), (x, -1.25, 1.25), (x, -.15, 1.45), (x, -.15, .5)], .03, 'metal', 'detail')
    k.hits.append(dict(p=(.9, 0.0, 2.6), d=(0, 1, 0), r=.85, depth=.3, sag=.12, rebar=True, debris=['ferro', 'paint.main']))
    k.meta.update(size=(W, 2.0, H))


def p_window_slit(k):
    W, H = 4.5, 4.4
    ox0, ox1, oz0, oz1 = -1.0, 1.0, 2.15, 2.85
    wall_segment(k, -W / 2, W / 2, H, openings=[(ox0, oz0, ox1, oz1)], style='inset')
    k.window(ox0, ox1, oz0, oz1, 0.0, mullions=2, shutter=True)
    k.hits.append(dict(p=(-.4, 0.0, 2.4), d=(0, 1, 0), r=.95, depth=.32, sag=.2, rebar=True, debris=['ferro', 'paint.main', 'glass']))
    k.meta.update(size=(W, 1.2, H))


def p_flat_roof(k):
    """Roof corner: wall stub, gravel deck below a coped parapet, hatch, scuppers, roof plant."""
    w, d, hw = 7.5, 6.0, 1.6
    t = G['wall_thickness_m']
    for (o, yaw, length) in [((0, -d / 2, 0), 0, w), ((w / 2, 0, 0), math.pi / 2, d), ((0, d / 2, 0), math.pi, w), ((-w / 2, 0, 0), -math.pi / 2, d)]:
        with k.at(o, yaw=yaw):
            for a in [(-length / 2 + .02, 0, length / 2 - .02, hw - .2)]:
                k.slab(a[0], a[2], a[1], a[3], .04, t, 'ferro', 'mass')
            k.panels(-length / 2 + .02, length / 2 - .02, 0.0, hw - .2, 0.0, 'paint.main', rows=1, style='inset')
            k.slab(-length / 2, length / 2, hw - .2, hw + .55, -.04, t, 'paint.main', 'mass', front=.03)
            k.coping(-length / 2, length / 2, hw + .55, 0.0)
            k.slab(-length / 2 + .2, length / 2 - .2, hw + .02, hw + .55, t, t + .05, 'paint.trim', 'mass')
    # roof deck: 1.5m slab bays (each collapsible), gravel membrane on top
    deck = hw - .05
    nx, ny = 5, 4
    sx, sy = (w - 2 * t) / nx, (d - 2 * t) / ny
    for i in range(nx):
        for j in range(ny):
            x0 = -w / 2 + t + i * sx
            y0 = -d / 2 + t + j * sy
            k.box((x0 + sx / 2, y0 + sy / 2, deck - .12), (sx - .01, sy - .01, .24), 'gravel', 'roof', slab=(i, j))
    k.moss_sites.append(((-w / 2 + t + .05, -d / 2 + t + .05, deck), (w / 2 - t - .05, -d / 2 + t + .05, deck)))
    k.moss_sites.append(((-w / 2 + t + .05, d / 2 - t - .05, deck), (-w / 2 + t + .05, -d / 2 + t + .05, deck)))
    # roof hatch, scuppers, rust stain sites, HVAC and mast
    k.box((-w * .25, d * .18, deck + .25), (.9, .9, .5), 'paint.second', 'detail', ch=.08, top=.06)
    k.box((-w * .25, d * .18, deck + .53), (1.0, 1.0, .08), 'paint.trim', 'detail', ch=.08)
    for x in (-w * .3, w * .2):
        k.slab(x - .14, x + .14, deck + .02, deck + .2, -.45, .2, 'metal', 'detail')
    with k.at((w * .17, d * .05, deck)):
        k.hvac(1.8, 1.4, .9)
    with k.at((w * .32, d * .32, deck)):
        k.mast(2.4)
    for i in range(3):
        k.tube([(w * .17 - .6 + i * .3, d * .05 + .75, deck + .5), (w * .17 - .6 + i * .3, d * .05 + 1.2, deck + .5), (w * .17 - .6 + i * .3, d * .05 + 1.2, deck + .1)], .05, 'metal', 'detail', sides=6)
    k.hits.append(dict(p=(-.6, -.5, deck), d=(0, .2, -1), r=1.5, depth=.8, sag=.6, rebar=True, debris=['gravel', 'ferro', 'metal'], collapse='roof'))
    k.meta.update(size=(w + .5, d + .5, hw + 2.6))


def p_turret(k):
    """Turret on base after Godfrey's chamfered language: stepped octagon footing + armoured housing."""
    k.prism(reg_poly(2.6, 8), 0, .55, 'ferro', 'mass', top=.16)
    k.prism(reg_poly(2.1, 8), .55, 1.35, 'paint.main', 'mass', top=.18)
    k.prism(reg_poly(2.13, 8), .62, .80, 'hazard', 'detail')
    for i in range(8):
        a = i * math.tau / 8 + math.pi / 8
        k.box((2.25 * math.cos(a), 2.25 * math.sin(a), .75), (.35, .35, .4), 'paint.trim', 'detail', ch=.08, top=.06)
    k.moss_sites.append(((-2.6, -2.0, 0), (2.6, -2.0, 0)))
    k.cyl((0, 0, 1.45), 1.55, .2, 'metal', 'detail', sides=24, top=.04)
    with k.tagged('turret'):
        k.prism(rect_poly(-1.45, -1.6, 1.45, 1.45, .45), 1.55, 3.05, 'paint.main', 'skin', top=.32, bot=.12)
        k.prism(rect_poly(-1.2, -1.75, 1.2, -.2, .25), 2.6, 3.25, 'paint.second', 'skin', top=.12)
        k.slab(-.8, .8, 2.75, 3.0, -1.80, -1.6, 'glass', 'glass', pane=True)
        k.slab(-1.46, 1.46, 1.75, 1.88, -1.62, 1.47, 'paint.accent', 'detail')
        # side drum (Godfrey C6) and ammo box
        k.cyl((1.55, .3, 2.2), .55, 1.1, 'paint.second', 'detail', sides=12, top=.08)
        k.box((-1.6, .4, 2.1), (.45, 1.4, .9), 'paint.trim', 'detail', ch=.1, top=.08)
        for s in (-1, 1):
            x = s * .55
            k.prism(rect_poly(x - .26, -1.6, x + .26, -1.1, .08), 2.0, 2.55, 'paint.trim', 'frame', axis='z')
            k.box((x, -2.2, 2.27), (.44, 1.3, .44), 'paint.main', 'frame', ch=.1, top=.08, bot=.08, barrel=True)
            k.tube([(x, -2.8, 2.27), (x, -4.6, 2.27)], .13, 'metal', 'frame', sides=10, barrel=True)
            k.tube([(x, -4.55, 2.27), (x, -4.95, 2.27)], .19, 'paint.trim', 'frame', sides=8, barrel=True)
        k.box((0, 1.0, 3.35), (.35, .35, .25), 'paint.trim', 'detail', ch=.06)
        k.tube([(0, 1.0, 3.45), (0, 1.0, 5.0)], .025, 'metal', 'detail', sides=5)
        k.cyl((0, 1.0, 5.02), .05, .08, 'lamp_red', 'detail', sides=6)
    k.hits.append(dict(p=(1.3, -.7, 2.5), d=(-.9, .3, -.2), r=1.1, depth=.4, sag=.15, rebar=False, debris=['paint.main', 'paint.second', 'metal'], tilt=dict(group='turret', pivot=(-1.5, 0, 1.55), axis=(0, 1, 0), angle=-.36, drop=.35, barrel_droop=.30)))
    k.meta.update(size=(5.2, 7.4, 5.2))


def p_dish_antenna(k):
    """Dish on yoke and Godfrey splayed pedestal (sensor tower identity: pale grey, yellow)."""
    k.prism(rect_poly(-1.8, -1.8, 1.8, 1.8, .55), 0, .45, 'ferro', 'mass', top=.14)
    for i in range(4):
        a = i * math.pi / 2 + math.pi / 4
        with k.at((0, 0, .45), yaw=a):
            k.extrude([(0, 0), (1.55, 0), (1.55, .22), (.55, .95), (0, .95)], -.22, .22, 'paint.trim', 'frame')
    k.prism(rect_poly(-.55, -.55, .55, .55, .18), .45, 3.4, 'paint.main', 'frame', top=.10)
    k.prism(rect_poly(-.58, -.58, .58, .58, .19), 1.0, 1.25, 'hazard', 'detail')
    k.box((0, -.62, 2.2), (.5, .2, .9), 'paint.second', 'detail', ch=.05)
    k.cyl((0, 0, 3.55), .75, .3, 'metal', 'detail', sides=16, top=.05)
    pivot = (0, 0, 4.6)
    with k.tagged('dish'):
        for s in (-1, 1):
            k.prism(rect_poly(s * 1.05 - .14, -.35, s * 1.05 + .14, .35, .09), 3.7, 4.9, 'paint.main', 'frame', top=.12)
        k.box((0, 0, 3.8), (2.5, .7, .3), 'paint.main', 'frame', ch=.1, top=.06)
        k.tube([(-1.25, 0, 4.6), (1.25, 0, 4.6)], .12, 'metal', 'frame', sides=10)
        tilt = math.radians(35)
        with k.at(pivot, pitch=tilt):
            R, depth, n, m = 2.6, .55, 32, 6
            verts, faces = [], []
            for layer, off in [(0, 0.0), (1, .09)]:
                for j in range(m + 1):
                    r = R * j / m
                    for i in range(n):
                        a = i * math.tau / n
                        z = depth * (r / R) ** 2
                        verts.append((r * math.cos(a), -(-.25 + z) + off, r * math.sin(a)))
            ring = n
            lay = n * (m + 1)
            for j in range(m):
                for i in range(n):
                    a, b = j * n + i, j * n + (i + 1) % n
                    faces.append((a, b, b + n, a + n))
                    faces.append((lay + a, lay + a + n, lay + b + n, lay + b))
            for i in range(n):
                a, b = m * n + i, m * n + (i + 1) % n
                faces.append((a, lay + a, lay + b, b))
            k.add(verts, faces, 'paint.main', 'skin', dish=True)
            # rim ring (chamfered) and back ribs
            pts = [(R * 1.01 * math.cos(i * math.tau / 24), -(-.25 + depth) - .02, R * 1.01 * math.sin(i * math.tau / 24)) for i in range(25)]
            k.tube(pts, .08, 'paint.trim', 'frame', sides=6)
            for i in range(8):
                a = i * math.tau / 8
                k.beam((0, .45, 0), (R * .95 * math.cos(a), -(-.25 + depth * .9) + .12, R * .95 * math.sin(a)), .10, 'metal', 'frame', seg=3)
            k.box((0, .55, 0), (.8, .5, .8), 'paint.second', 'frame', ch=.15)
            # feed horn on tripod
            tip = (0, -1.9, 0)
            for i in range(3):
                a = i * math.tau / 3 + math.pi / 2
                k.beam((R * .8 * math.cos(a), -(-.25 + depth * .64), R * .8 * math.sin(a)), tip, .07, 'metal', 'frame', seg=4)
            k.box(tip, (.32, .45, .32), 'paint.trim', 'detail', ch=.08)
        k.box((0, 1.05, 4.25), (.9, .5, .6), 'paint.second', 'detail', ch=.12)
    k.moss_sites.append(((-1.8, -1.9, 0), (1.8, -1.9, 0)))
    k.hits.append(dict(p=(.9, -.6, 5.0), d=(-.3, 1, -.3), r=.85, depth=.35, sag=.25, rebar=False, debris=['paint.main', 'metal'], tilt=dict(group='dish', pivot=(0, 0, 4.6), axis=(1, 0, 0), angle=-.62, drop=0.0, barrel_droop=0)))
    k.meta.update(size=(5.6, 5.6, 7.6))


def p_tank(k):
    """Fuel tank: 16-sided drum on stepped ferro footing, hoops, hazard band, cone roof, rail, ladder."""
    R, H = 3.6, 6.6
    sides = 16
    k.prism(reg_poly(R + 1.0, 8), 0, .5, 'ferro', 'mass', top=.16)
    k.prism(reg_poly(R + .45, 16), .5, .8, 'ferro', 'mass', top=.08)
    courses = 4
    ch = (H - .8) / courses
    for c in range(courses):
        z0 = .8 + c * ch
        for i in range(8):
            a0, a1 = i * math.tau / 8, (i + 1) * math.tau / 8
            seg = []
            n = 2
            for j in range(n + 1):
                a = a0 + (a1 - a0) * j / n
                seg.append(a)
            verts = []
            for z in (z0 + .02, z0 + ch - .02):
                for rr in (R, R - .05):
                    for a in seg:
                        verts.append((rr * math.cos(a), rr * math.sin(a), z))
            m = n + 1
            # indices: layer (z) * 2m + ring*m + j
            faces = []
            def idx(zl, rl, j):
                return zl * 2 * m + rl * m + j
            for j in range(n):
                faces.append((idx(0, 0, j), idx(0, 0, j + 1), idx(1, 0, j + 1), idx(1, 0, j)))
                faces.append((idx(0, 1, j + 1), idx(0, 1, j), idx(1, 1, j), idx(1, 1, j + 1)))
                faces.append((idx(0, 1, j), idx(0, 1, j + 1), idx(0, 0, j + 1), idx(0, 0, j)))
                faces.append((idx(1, 0, j), idx(1, 0, j + 1), idx(1, 1, j + 1), idx(1, 1, j)))
            faces.append((idx(0, 0, 0), idx(1, 0, 0), idx(1, 1, 0), idx(0, 1, 0)))
            faces.append((idx(0, 1, n), idx(1, 1, n), idx(1, 0, n), idx(0, 0, n)))
            k.add(verts, faces, 'hazard' if c == 0 else 'paint.main', 'skin', removable=c > 0, course=c)
        k.cyl((0, 0, z0 + ch), R + .06, .16, 'paint.trim', 'frame', sides=sides, top=.03, bot=.03)
    k.cyl((0, 0, .85), R + .06, .14, 'paint.trim', 'frame', sides=sides)
    # internal frame: vertical stiffeners revealed by a rupture
    for i in range(8):
        a = i * math.tau / 8
        k.beam((R * .93 * math.cos(a), R * .93 * math.sin(a), .8), (R * .93 * math.cos(a), R * .93 * math.sin(a), H), .14, 'metal', 'frame', seg=12)
    # cone roof with chamfered rim, hatch, vent
    k.prism(reg_poly(R + .14, sides), H, H + .22, 'paint.trim', 'roof', top=.06)
    verts = [((R + .05) * math.cos(i * math.tau / sides), (R + .05) * math.sin(i * math.tau / sides), H + .22) for i in range(sides)] + [(0, 0, H + 1.0)]
    faces = [(i, (i + 1) % sides, sides) for i in range(sides)] + [tuple(reversed(range(sides)))]
    k.add(verts, faces, 'paint.main', 'roof', removable=False)
    k.cyl((0, 0, H + 1.05), .55, .3, 'paint.second', 'detail', sides=8, top=.08)
    k.cyl((0, 0, H + 1.25), .62, .06, 'paint.accent', 'detail', sides=8)
    # roof rail
    posts = 16
    for i in range(posts):
        a = i * math.tau / posts
        k.beam(((R - .05) * math.cos(a), (R - .05) * math.sin(a), H + .22), ((R - .05) * math.cos(a), (R - .05) * math.sin(a), H + 1.25), .05, 'grate', 'detail')
    for z in (H + .75, H + 1.25):
        k.tube([((R - .05) * math.cos(i * math.tau / 32), (R - .05) * math.sin(i * math.tau / 32), z) for i in range(33)], .03, 'paint.accent' if z > H + 1 else 'grate', 'detail', sides=5)
    # caged ladder on the -Y side
    with k.at((0, -R - .12, 0)):
        for s in (-1, 1):
            k.beam((s * .25, 0, .5), (s * .25, 0, H + 1.2), .06, 'paint.accent', 'detail', seg=3)
        for i in range(int((H + .6) / .3)):
            k.beam((-.25, 0, .7 + i * .3), (.25, 0, .7 + i * .3), .035, 'metal', 'detail')
        for i in range(int((H - 1.6) / .9)):
            z = 2.4 + i * .9
            k.tube([(-.3, 0, z), (-.36, -.45, z), (0, -.62, z), (.36, -.45, z), (.3, 0, z)], .025, 'metal', 'detail', sides=4)
    # inlet pipe with flange and valve
    k.tube([(R * .7, -R * .72, 1.4), (R + .9, -R - .4, 1.4), (R + 1.8, -R - .4, 1.4), (R + 1.8, -R - .4, .0)], .22, 'paint.second', 'detail', sides=10)
    k.box((R + 1.8, -R - .4, 1.0), (.6, .6, .3), 'paint.accent', 'detail', ch=.1)
    k.moss_sites.append(((-R - 1, -R - 1.1, 0), (R + 1, -R - 1.1, 0)))
    k.hits.append(dict(p=(R * .45, -R * .9, 3.6), d=(-.45, .9, 0), r=1.35, depth=-.55, sag=.25, rebar=False, debris=['paint.main', 'metal'], roof_drop=dict(angle=.30)))
    k.meta.update(size=(2 * R + 3.2, 2 * R + 3.2, H + 1.6))


def p_gantry(k):
    """Repair gantry: Godfrey F-post legs, box-girder bridge with hazard faces, trolley, rails."""
    span, H = 11.0, 8.4
    for s in (-1, 1):
        x = s * span / 2
        k.box((x, 0, .06), (1.0, 7.0, .12), 'ferro', 'mass', top=.04)
        k.box((x, 0, .18), (.22, 7.0, .14), 'metal', 'detail')
        with k.tagged('legA' if s < 0 else 'legB'):
            # boot base: splayed, chamfered, slotted
            k.prism(rect_poly(x - .8, -1.4, x + .8, 1.4, .35), .2, 1.4, 'paint.trim', 'frame', top=.4)
            k.slab(x - .55, x + .55, .5, .9, -1.42, -1.3, 'hazard', 'detail')
            for yy in (-1.05, 1.05):
                k.box((x, yy, .2), (.6, .5, .25), 'metal', 'detail', ch=.08)
            k.prism(rect_poly(x - .42, -.62, x + .42, .62, .16), 1.4, H, 'paint.main', 'frame', top=.10)
            k.slab(x - .10, x + .10, 2.0, H - 1.0, -.64, -.58, 'paint.second', 'detail')
            k.slab(x - .44, x + .44, 3.2, 3.36, -.64, .64, 'paint.accent', 'detail')
            for z in range(3):
                k.box((x + s * .45, 0, 2.6 + z * 1.6), (.12, .4, .5), 'paint.trim', 'detail', ch=.04)
    with k.tagged('bridge'):
        k.prism(rect_poly(-span / 2 - .8, -.8, span / 2 + .8, .8, 0), H, H + 1.3, 'paint.main', 'frame', bot=.25, top=.08, axis='z')
        k.slab(-span / 2 + .5, span / 2 - .5, H + .35, H + .85, -.82, -.76, 'hazard', 'detail')
        k.slab(-span / 2 + .5, span / 2 - .5, H + .35, H + .85, .76, .82, 'hazard', 'detail')
        for x in range(int(span) + 1):
            xx = -span / 2 + x
            k.beam((xx, -.75, H + 1.3), (xx, -.75, H + 2.3), .05, 'grate', 'detail')
        k.tube([(-span / 2, -.75, H + 2.3), (span / 2, -.75, H + 2.3)], .035, 'paint.accent', 'detail', sides=5)
        # trolley + hook block
        tx = 1.4
        k.box((tx, 0, H - .45), (1.6, 2.1, .9), 'paint.second', 'detail', ch=.25, bot=.12)
        k.box((tx, 0, H - .92), (1.62, 2.12, .12), 'paint.accent', 'detail', ch=.25)
        for yy in (-.25, .25):
            k.tube([(tx, yy, H - .9), (tx, yy, 3.9)], .025, 'metal', 'detail', sides=4)
        k.box((tx, 0, 3.6), (.7, .9, .6), 'paint.trim', 'detail', ch=.12, bot=.1)
        k.box((tx, 0, 3.15), (.36, .2, .3), 'hazard', 'detail')
    k.moss_sites.append(((-span / 2 - 1, -3.5, 0), (span / 2 + 1, -3.5, 0)))
    k.hits.append(dict(p=(span / 2, -.3, 4.0), d=(-.2, 1, 0), r=1.0, depth=.25, sag=.2, rebar=False, debris=['paint.main', 'metal'], gantry_fall=True))
    k.meta.update(size=(span + 2.5, 7.5, H + 2.5))


PRIMITIVES = {
    # name: (builder, palette, stock id for the comparison crop)
    'wall_panel': (p_wall_panel, 'hangar_sand', 'hangar'),
    'sloped_wall': (p_sloped_wall, 'barracks_sage', 'barracks'),
    'quonset_shell': (p_quonset_shell, 'quonset_rust', 'quonset'),
    'bay_door': (p_bay_door, 'factory_steel', 'factorymd'),
    'mech_bay_door': (p_mech_bay_door, 'slate_oxide', 'hangar'),
    'infantry_door': (p_infantry_door, 'barracks_sage', 'barracks'),
    'window_slit': (p_window_slit, 'slate_oxide', 'barracks'),
    'flat_roof': (p_flat_roof, 'barracks_sage', 'barracks'),
    'turret': (p_turret, 'gunmetal', 'gaussturret'),
    'dish_antenna': (p_dish_antenna, 'tank_hazard', 'sensortower'),
    'tank': (p_tank, 'tank_hazard', 'largefueltank'),
    'gantry': (p_gantry, 'gunmetal', 'gantry'),
}


# ---------------------------------------------------------------- conditions (piece level)
def centroid(piece):
    v = piece['v']
    return tuple(sum(p[i] for p in v) / len(v) for i in range(3))


def bbox(piece):
    v = piece['v']
    return [min(p[i] for p in v) for i in range(3)], [max(p[i] for p in v) for i in range(3)]


def transform_piece(piece, fn):
    piece['v'] = [fn(p) for p in piece['v']]


def lay_flat(k, piece, at, yaw, rng, crumple=.06):
    """Drop a detached piece to the ground: largest extent horizontal, resting on z=0."""
    lo, hi = bbox(piece)
    ext = [hi[i] - lo[i] for i in range(3)]
    c = [(lo[i] + hi[i]) / 2 for i in range(3)]
    pts = [(p[0] - c[0], p[1] - c[1], p[2] - c[2]) for p in piece['v']]
    if ext[2] > min(ext[0], ext[1]):
        # vertical plate: tip it over about its thin axis
        thin = 0 if ext[0] < ext[1] else 1
        if thin == 1:
            pts = [(x, -z, y) for x, y, z in pts]
        else:
            pts = [(-z, y, x) for x, y, z in pts]
    ca, sa = math.cos(yaw), math.sin(yaw)
    pts = [(x * ca - y * sa, x * sa + y * ca, z) for x, y, z in pts]
    # crumple: bend along x and add jitter, so sheets read as torn metal not cards
    pts = [(x, y, z + crumple * (abs(x) + abs(y)) * .5 + rng.uniform(-crumple, crumple) * .3) for x, y, z in pts]
    zmin = min(p[2] for p in pts)
    tilt = rng.uniform(-.25, .25)
    pts = [(x, y, z - zmin + x * tilt * .15) for x, y, z in pts]
    zmin = min(p[2] for p in pts)
    piece['v'] = [(at[0] + x, at[1] + y, z - zmin + at[2]) for x, y, z in pts]


def apply_abandoned(k, cond):
    rng = random.Random(k.seed * 7 + 3)
    debris = []
    for piece in list(k.pieces):
        if piece.get('removable') and piece['role'] in ('skin', 'roof') and rng.random() < cond['missing_panels']:
            k.pieces.remove(piece)
            lo, hi = bbox(piece)
            if rng.random() < .55 and piece['role'] == 'skin' and hi[2] - lo[2] < 3.5:
                # the panel fell off and now lies at the foot of the wall below where it hung
                c = centroid(piece)
                lay_flat(k, piece, (c[0] + rng.uniform(-.4, .4), min(lo[1], hi[1]) - .9 - rng.uniform(0, .6), 0.0), rng.uniform(-.5, .5), rng)
                debris.append(piece)
        elif piece.get('pane') and rng.random() < cond['broken_glass']:
            k.pieces.remove(piece)
            lo, hi = bbox(piece)
            # jagged shards left in two corners of the frame
            for cx, cz, sx, sz in [(lo[0], lo[2], 1, 1), (hi[0], hi[2], -1, -1)]:
                y = (lo[1] + hi[1]) / 2
                a = rng.uniform(.15, .35) * (hi[0] - lo[0])
                b = rng.uniform(.25, .6) * (hi[2] - lo[2])
                k.add([(cx, y, cz), (cx + sx * a, y, cz), (cx, y, cz + sz * b), (cx, y + .01, cz), (cx + sx * a, y + .01, cz), (cx, y + .01, cz + sz * b)],
                      [(0, 2, 1), (3, 4, 5), (0, 1, 4, 3), (1, 2, 5, 4), (2, 0, 3, 5)], 'glass', 'glass')
    k.pieces.extend(debris)
    add_vegetation(k, cond['moss'], rng)
    add_litter(k, cond.get('litter', 0), rng)


def add_vegetation(k, amount, rng):
    """Weeds and moss pads along registered ground and roof lines (Halim set dressing)."""
    if amount <= 0:
        return
    for p0, p1 in k.moss_sites:
        length = math.dist(p0, p1)
        n = int(length * 2.2 * amount)
        for _ in range(n):
            f = rng.random()
            base = [p0[i] + (p1[i] - p0[i]) * f for i in range(3)]
            base[0] += rng.uniform(-.15, .15)
            base[1] += rng.uniform(-.25, .25)
            if rng.random() < .55:
                # moss / soil pad: flattened octagon lump
                r = rng.uniform(.08, .28)
                k.prism(reg_poly(r, 7, phase=rng.random(), cx=base[0], cy=base[1]), base[2] - .02, base[2] + rng.uniform(.03, .08), 'veg.moss', 'detail', top=.03)
            else:
                # grass tuft: a fan of thin bent blades
                blades = rng.randint(4, 7)
                for b in range(blades):
                    a = rng.uniform(0, math.tau)
                    hgt = rng.uniform(.25, .6)
                    lean = rng.uniform(.05, .25)
                    w = .03
                    dx, dy = math.cos(a), math.sin(a)
                    px, py = -dy * w, dx * w
                    x, y, z = base
                    verts = [(x - px, y - py, z), (x + px, y + py, z), (x + dx * lean * .5 + px * .5, y + dy * lean * .5 + py * .5, z + hgt * .6),
                             (x + dx * lean * .5 - px * .5, y + dy * lean * .5 - py * .5, z + hgt * .6), (x + dx * lean, y + dy * lean, z + hgt)]
                    k.add(verts, [(0, 1, 2, 3), (3, 2, 4), (3, 2, 1, 0), (2, 3, 4)], 'veg.grass', 'detail')


def chunk(k, c, size, mat, rng, flat=False):
    """Irregular chamfered rubble chunk resting on the ground."""
    n = rng.choice([5, 6, 7])
    r = size / 2
    poly = [(c[0] + r * rng.uniform(.6, 1.1) * math.cos(a), c[1] + r * rng.uniform(.6, 1.1) * math.sin(a)) for a in sorted(rng.uniform(0, math.tau) for _ in range(n))]
    # ensure convex-ish ordering by angle: sorted angles already CCW
    h = size * (rng.uniform(.12, .25) if flat else rng.uniform(.35, .7))
    p = k.prism(poly, c[2], c[2] + h, mat, 'detail', top=min(.05, h * .3) if not flat else 0, debris=True)
    if p and not flat:
        tilt = rng.uniform(-.35, .35)
        cc = centroid(p)
        transform_piece(p, lambda q: rotate_about(q, (cc[0], cc[1], c[2]), (rng.random(), rng.random(), 0), tilt))
        zmin = min(q[2] for q in p['v'])
        transform_piece(p, lambda q: (q[0], q[1], q[2] - zmin + c[2]))
    return p


def add_litter(k, count, rng):
    if count <= 0 or not k.moss_sites:
        return
    for i in range(count):
        p0, p1 = k.moss_sites[i % len(k.moss_sites)]
        f = rng.random()
        c = [p0[j] + (p1[j] - p0[j]) * f for j in range(3)]
        c[1] -= rng.uniform(0, .9)
        chunk(k, (c[0], c[1], c[2]), rng.uniform(.12, .35), rng.choice(['ferro', 'ferro', 'metal', 'paint.main']), rng, flat=rng.random() < .3)


def apply_destroyed(k):
    """Piece-level structural failure; the Blender stage then dents, tears and scorches.

    Within the breach panels are blown out and land at the foot of the wall;
    in the ring around it they hang from their last fixings (top edge) or fold
    down outward (bottom edge). Special recipes tilt a turret off its ring,
    droop a dish about its axle, drop a tank roof and fold a gantry leg.
    """
    rng = random.Random(k.seed * 13 + 1)
    landed = []
    for hit in k.hits:
        p, d, r = hit['p'], hit['d'], hit['r']
        dn = math.sqrt(sum(c * c for c in d))
        d = tuple(c / dn for c in d)
        horizontal = abs(d[2]) < .7
        for piece in list(k.pieces):
            if piece['role'] not in ('skin', 'glass', 'detail') or piece.get('debris') or piece.get('group'):
                continue
            c = centroid(piece)
            dist = math.dist(c, p)
            lo, hi = bbox(piece)
            size = max(hi[i] - lo[i] for i in range(3))
            if dist < r * .75 and size < 6.0:
                k.pieces.remove(piece)
                if piece['role'] == 'skin' and rng.random() < .7:
                    out = (-d[0], -d[1]) if horizontal else (rng.uniform(-1, 1), rng.uniform(-1, 1))
                    at = (c[0] + out[0] * rng.uniform(.6, 1.6) + rng.uniform(-.4, .4), c[1] + out[1] * rng.uniform(.6, 1.6) + rng.uniform(-.4, .4), 0.0)
                    lay_flat(k, piece, at, rng.uniform(-1, 1), rng, crumple=.12)
                    landed.append(piece)
            elif dist < r * 1.25 and piece['role'] == 'skin' and horizontal and size < 6.0:
                # hangs from the edge farthest from the breach; gravity + blast fold it outward
                if c[2] < p[2]:
                    hinge_z, sign = lo[2], 1
                else:
                    hinge_z, sign = hi[2], -1
                axis = (d[1], -d[0], 0)
                origin = (c[0], lo[1] if d[1] > 0 else hi[1], hinge_z)
                ang = sign * rng.uniform(.25, .7) * (1 if c[2] < p[2] else .6)
                transform_piece(piece, lambda q: rotate_about(q, origin, axis, ang))
                zmin = min(q[2] for q in piece['v'])
                if zmin < 0:
                    transform_piece(piece, lambda q: (q[0], q[1], q[2] - zmin))
        if 'tilt' in hit:
            t = hit['tilt']
            for piece in k.pieces:
                if piece.get('group') == t['group']:
                    transform_piece(piece, lambda q: rotate_about(q, t['pivot'], t['axis'], t['angle']))
                    if t.get('drop'):
                        transform_piece(piece, lambda q: (q[0], q[1], q[2] - t['drop']))
                    if piece.get('barrel') and t.get('barrel_droop'):
                        cb = centroid(piece)
                        transform_piece(piece, lambda q: rotate_about(q, (cb[0], cb[1] + .6, cb[2]), (1, 0, 0), t['barrel_droop']))
        if 'roof_drop' in hit:
            for piece in k.pieces:
                if piece['role'] == 'roof':
                    transform_piece(piece, lambda q: rotate_about(q, (-3.7, 0, 6.6), (0, 1, 0), hit['roof_drop']['angle']))
                    transform_piece(piece, lambda q: (q[0], q[1], q[2] - .5))
        if hit.get('collapse') == 'roof':
            for piece in list(k.pieces):
                if piece['role'] == 'roof' and 'slab' in piece:
                    c = centroid(piece)
                    dist = math.dist((c[0], c[1]), (p[0], p[1]))
                    if dist < r * .9:
                        # failed slab: drops into the interior, one edge still on its beam
                        lo, hi = bbox(piece)
                        edge_x = lo[0] if c[0] > p[0] else hi[0]
                        ang = (.9 if c[0] > p[0] else -.9) * rng.uniform(.7, 1.0)
                        transform_piece(piece, lambda q: rotate_about(q, (edge_x, c[1], hi[2]), (0, 1, 0), ang))
                    elif dist < r * 1.4:
                        transform_piece(piece, lambda q: (q[0], q[1], q[2] - .12 * (1 - dist / (r * 1.4)) * 2))
        if hit.get('gantry_fall'):
            # leg B buckles at mid-height; bridge end drops onto the folded leg
            kz = 3.6
            for piece in k.pieces:
                if piece.get('group') == 'legB' and centroid(piece)[2] > kz:
                    transform_piece(piece, lambda q: rotate_about(q, (5.5 - .42, 0, kz), (0, 1, 0), -.75))
                if piece.get('group') == 'bridge':
                    transform_piece(piece, lambda q: rotate_about(q, (-5.5, 0, 8.4), (0, 1, 0), .56))
    # sheets that land become rubble that matches the part (debris chunks added in Blender)
    k.pieces.extend(landed)


def build(name, condition, seed=17):
    fn, palette, stock = PRIMITIVES[name]
    k = Kit(name, palette, seed)
    fn(k)
    cond = LANG['conditions'][condition]
    if condition == 'abandoned':
        apply_abandoned(k, cond)
    elif condition == 'destroyed':
        apply_destroyed(k)
        rng = random.Random(seed)
        for piece in list(k.pieces):
            if piece.get('pane'):
                k.pieces.remove(piece)
    k.meta.update(name=name, condition=condition, palette=palette, stock=stock)
    return k


def to_json(k):
    return {'name': k.name, 'palette': k.palette, 'meta': k.meta, 'hits': k.hits,
            'pieces': [{'v': [list(map(lambda c: round(c, 5), p)) for p in pc['v']], 'f': pc['f'], 'm': pc['m'], 'role': pc['role'], 'group': pc.get('group')} for pc in k.pieces]}
