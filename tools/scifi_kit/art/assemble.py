"""Building assemblies from the kit language (pure Python), plus the review-sheet CLI.

Assemblies use only the shared wall module, openings, roof and plant primitives
from geo.py, so every building speaks the same language as the contact sheets.
Footprints follow the stock catalog bounds (metres = world units / 4).
"""
import json
import math
import random
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import geo  # noqa: E402
from geo import G, P, Kit, rect_poly, reg_poly, wall_segment  # noqa: E402

REF = Path('G:/mc2-assets/ref/style/MW5 and MWO')
THUMBS = Path('E:/mc2-archive-2026-10/asset-modernize/docs/assets/sheets/thumbs')
ROUND1 = Path('G:/mc2-assets/out/scifi-kit')


def snap(v, step=None):
    step = step or G['sub_m']
    return max(step, round(v / step) * step)


def sides(w, d):
    """(origin, yaw, length) for front(-Y), right(+X), back(+Y), left(-X)."""
    t = G['wall_thickness_m']
    return [((0, -d / 2, 0), 0.0, w), ((w / 2, 0, 0), math.pi / 2, d - 2 * t), ((0, d / 2, 0), math.pi, w), ((-w / 2, 0, 0), -math.pi / 2, d - 2 * t)]


def corner_columns(k, w, d, h, mat='paint.trim'):
    """Godfrey chamfered plan corners: the corner block covers both wall ends."""
    for sx in (-1, 1):
        for sy in (-1, 1):
            x, y = sx * w / 2, sy * d / 2
            poly = rect_poly(x - .45, y - .45, x + .45, y + .45, .28)
            k.prism(poly, 0, .7, 'ferro', 'mass', top=.12)
            k.prism(rect_poly(x - .36, y - .36, x + .36, y + .36, .22), .7, h, mat, 'frame', top=.08)
            k.prism(rect_poly(x - .37, y - .37, x + .37, y + .37, .225), .62, .78, 'paint.accent', 'detail')


def roof_deck(k, w, d, z, bays=(1.5, 1.5)):
    t = G['wall_thickness_m']
    nx, ny = max(1, round((w - 2 * t) / bays[0])), max(1, round((d - 2 * t) / bays[1]))
    sx, sy = (w - 2 * t) / nx, (d - 2 * t) / ny
    for i in range(nx):
        for j in range(ny):
            k.box((-w / 2 + t + (i + .5) * sx, -d / 2 + t + (j + .5) * sy, z - .12), (sx - .01, sy - .01, .24), 'gravel', 'roof', slab=(i, j))
    k.moss_sites.append(((-w / 2 + t + .1, -d / 2 + t + .1, z), (w / 2 - t - .1, -d / 2 + t + .1, z)))
    k.moss_sites.append(((-w / 2 + t + .1, d / 2 - t - .1, z), (-w / 2 + t + .1, -d / 2 + t + .1, z)))


# ------------------------------------------------------------------ round-1 types
def barracks(k):
    """Round-1 barracks re-built on the kit: infantry door front, window slits per bay."""
    w, d = snap(9.75), snap(19.5)
    deck = 3.75
    h = deck + .65
    for i, (o, yaw, length) in enumerate(sides(w, d)):
        openings = []
        if i == 0:
            openings = [(-.65, .45, .65, 2.9)]
        elif i in (1, 3):
            n = round(length / G['bay_m'])
            for b in range(n):
                cx = -length / 2 + (b + .5) * length / n
                openings.append((cx - .8, 2.0, cx + .8, 2.6))
        with k.at(o, yaw=yaw):
            wall_segment(k, -length / 2, length / 2, h, openings=openings, style='inset')
            if i == 0:
                infantry_door_parts(k, 1.3, 2.45)
            for a in openings if i in (1, 3) else []:
                k.window(a[0], a[2], a[1], a[3], 0.0, mullions=1)
            if i in (1, 3):
                n = round(length / G['bay_m'])
                for b in range(1, n):
                    k.pilaster(-length / 2 + b * length / n, .6, h - .3, w=.42, proud=.24)
    corner_columns(k, w, d, h + .3)
    roof_deck(k, w, d, deck)
    for y in (-d * .22, d * .12):
        with k.at((w * .12, y, deck)):
            k.hvac(1.9, 1.5, .9)
    with k.at((-w * .28, d * .30, deck)):
        k.mast(2.6)
    k.box((-w * .2, -d * .32, deck + .3), (1.0, 1.0, .6), 'paint.second', 'detail', ch=.1, top=.08)
    # front sign plate (Godfrey F-sign language) above the door canopy
    k.slab(-2.2, 2.2, 3.35, 3.85, -d / 2 - .12, -d / 2 + .02, 'paint.trim', 'detail', front=.05)
    k.slab(-2.0, 2.0, 3.42, 3.78, -d / 2 - .14, -d / 2 - .1, 'paint.accent', 'detail')
    k.hits.append(dict(p=(w / 2, d * .15, 2.4), d=(-1, 0, 0), r=1.8, depth=.6, sag=.45, rebar=True, debris=['ferro', 'paint.main', 'metal']))
    k.hits.append(dict(p=(w * .1, d * .2, deck), d=(0, 0, -1), r=1.8, depth=.7, sag=.5, rebar=True, debris=['gravel', 'ferro'], collapse='roof', reach=1.4))
    return dict(size=(w, d, h), stock='barracks', palette='slate_oxide', ref='paul-santoro-ma-fanart-barracks-img1.jpg', round1='barracks_17')


def infantry_door_parts(k, ow, oh):
    k.reveal(-ow / 2, ow / 2, .45, .45 + oh, 0.0, sill=False)
    k.slab(-ow / 2 + .02, ow / 2 - .02, .45, .45 + oh, .14, .22, 'paint.second', 'skin', front=.03)
    k.slab(-ow / 2 + .25, ow / 2 - .25, .45 + oh * .62, .45 + oh * .86, .10, .14, 'glass', 'glass', pane=True)
    k.slab(-ow / 2 - .6, ow / 2 + .6, .45 + oh + .38, .45 + oh + .58, -1.05, .02, 'paint.trim', 'frame', front=.12)
    k.slab(-ow / 2 - .6, ow / 2 + .6, .45 + oh + .58, .45 + oh + .66, -1.0, -.6, 'paint.accent', 'detail')
    for s in (-1, 1):
        with k.at((s * (ow / 2 + .5), 0, .45 + oh + .38)):
            k.extrude([(-.9, 0), (0, -.55), (0, 0)], -.06, .06, 'paint.trim', 'frame')
    k.lamp_head(0, .45 + oh + .30, -.35, w=.45)
    for d, z in [(1.3, .45), (.75, .225)]:
        k.slab(-ow / 2 - .45, ow / 2 + .45, 0, z, -d, .02, 'ferro', 'mass', front=.04)
        k.slab(-ow / 2 - .45, ow / 2 + .45, z - .05, z, -d - .01, -d + .07, 'hazard', 'detail')
    k.box((ow / 2 + .45, -.08, 1.55), (.22, .12, .32), 'paint.accent', 'detail', ch=.03)


def factory(k):
    """Round-1 factory: corrugated hall, two vehicle bays, clerestory monitor, stack."""
    w, d = snap(18.0), snap(30.0)
    h = 9.6
    for i, (o, yaw, length) in enumerate(sides(w, d)):
        openings = [(-6.4, 0, -1.6, 4.6), (1.6, 0, 6.4, 4.6)] if i == 0 else []
        if i in (1, 3):
            n = round(length / G['bay_m'])
            openings = [(-length / 2 + (b + .5) * length / n - 1.0, 6.4, -length / 2 + (b + .5) * length / n + 1.0, 7.1) for b in range(n) if b % 2 == 0]
        with k.at(o, yaw=yaw):
            wall_segment(k, -length / 2, length / 2, h, openings=openings, style='corr', mat='corr.main')
            if i == 0:
                for cx in (-4.0, 4.0):
                    with k.at((cx, 0, 0)):
                        rollup(k, 4.8, 4.6)
            for a in openings if i in (1, 3) else []:
                k.window(a[0], a[2], a[1], a[3], 0.0, mullions=2)
            if i in (1, 3):
                n = round(length / 6.0)
                for b in range(1, n):
                    x = -length / 2 + b * length / n
                    k.prism(rect_poly(x - .35, -.55, x + .35, .05, .18), 0, h - .3, 'paint.trim', 'frame', top=.25)
                    k.prism(rect_poly(x - .5, -.75, x + .5, .1, .25), 0, .7, 'ferro', 'mass', top=.15)
                # catwalk on the right wall at mid height
                if i == 1:
                    catwalk(k, -length * .35, length * .2, 5.2)
    corner_columns(k, w, d, h + .3)
    deck = h - .65
    roof_deck(k, w, d, deck, (3.0, 3.0))
    # clerestory monitor: raised box with a continuous glazed band (window module) and coping
    mw, md, mh = 6.0, d * .7, 2.6
    for i, (o, yaw, length) in enumerate(sides(mw, md)):
        with k.at((o[0], o[1], deck), yaw=yaw):
            k.slab(-length / 2, length / 2, 0, mh, .04, .35, 'paint.main', 'mass', front=.03)
            if i in (1, 3):
                n = int(length / 1.5)
                for b in range(n):
                    x0 = -length / 2 + b * length / n + .1
                    k.slab(x0, x0 + length / n - .2, .7, 1.9, -.02, .02, 'glass', 'glass', pane=True)
                    k.slab(x0 - .1, x0, .6, 2.0, -.08, .05, 'paint.trim', 'frame')
                k.slab(-length / 2, length / 2, .55, .7, -.12, .05, 'paint.trim', 'frame', front=.03)
                k.slab(-length / 2, length / 2, 1.9, 2.05, -.12, .05, 'paint.trim', 'frame', front=.03)
            k.coping(-length / 2, length / 2, mh, 0.0, depth=.35)
    k.box((0, 0, deck + mh + .1), (mw - .7, md - .7, .2), 'paint.second', 'roof')
    for j, y in enumerate((-d * .38, d * .38)):
        with k.at((w * .3 * (1 if j else -1), y, deck)):
            k.hvac(2.2, 1.7, 1.1)
    with k.at((w * .32, d * .05, deck)):
        k.hvac(2.2, 1.7, 1.1)
    # stack: chamfered octagon flue on a plinth with accent band
    sx, sy = -w * .32, d * .3
    k.prism(reg_poly(.9, 8, cx=sx, cy=sy), deck, deck + .5, 'ferro', 'mass', top=.12)
    k.prism(reg_poly(.6, 8, cx=sx, cy=sy), deck + .5, deck + 5.5, 'paint.second', 'frame', top=.1)
    k.prism(reg_poly(.62, 8, cx=sx, cy=sy), deck + 4.6, deck + 4.9, 'hazard', 'detail')
    k.cyl((sx, sy, deck + 5.55), .45, .1, 'rubber', 'detail', sides=8)
    with k.at((w * .3, -d * .2, deck)):
        k.mast(3.2)
    k.hits.append(dict(p=(-w / 2, -d * .1, 6.0), d=(1, 0, -.1), r=2.4, depth=.8, sag=.6, rebar=True, debris=['corr.main', 'ferro', 'metal']))
    k.hits.append(dict(p=(w * .2, -d * .25, deck), d=(0, 0, -1), r=2.6, depth=.9, sag=.6, rebar=True, debris=['gravel', 'ferro', 'metal'], collapse='roof', reach=1.4))
    return dict(size=(w, d, h + 5), stock='factorymd', palette='slate_oxide', ref='viona-halim-spacetower01.jpg', round1='factory_17')


def rollup(k, ow, oh):
    k.reveal(-ow / 2, ow / 2, 0.0, oh, 0.0, depth=.45, sill=False)
    n = int(oh / .28)
    for i in range(n):
        z = .04 + i * (oh - .1) / n
        k.slab(-ow / 2 + .02, ow / 2 - .02, z, z + (oh - .1) / n - .025, .26, .34, 'paint.second', 'skin', front=.03, removable=True)
    k.slab(-ow / 2 - .45, ow / 2 + .45, oh + .30, oh + .95, -.55, .02, 'paint.trim', 'frame', front=.16)
    k.slab(-ow / 2 - .45, ow / 2 + .45, oh + .44, oh + .56, -.57, -.50, 'paint.accent', 'detail')
    for s in (-1, 1):
        k.slab(s * ow / 2 - .08, s * ow / 2 + .08, 0, oh + .3, .10, .36, 'metal', 'frame')
        k.bollard(s * (ow / 2 + .55), -1.0, 1.05)
    k.lamp_head(0, oh + 1.25, 0.0)
    k.slab(-ow / 2 - .3, ow / 2 + .3, -.02, .05, -2.2, .30, 'ferro', 'mass')
    k.hazard_strip(-ow / 2, ow / 2, -.42, -.10, .05)


def catwalk(k, x0, x1, z):
    k.slab(x0, x1, z - .12, z, -1.1, 0.0, 'grate', 'detail')
    n = int((x1 - x0) / 1.5)
    for i in range(n + 1):
        x = x0 + (x1 - x0) * i / n
        k.beam((x, -1.05, z), (x, -1.05, z + 1.05), .05, 'grate', 'detail')
        k.beam((x, -1.0, z - .1), (x, -.02, z - .55), .07, 'paint.trim', 'detail')
    k.tube([(x0, -1.05, z + 1.05), (x1, -1.05, z + 1.05)], .03, 'paint.accent', 'detail', sides=5)
    k.tube([(x0, -1.05, z + .55), (x1, -1.05, z + .55)], .025, 'grate', 'detail', sides=5)
    # ladder down
    for s in (-.25, .25):
        k.beam((x0 + .4 + s, -1.3, 0), (x0 + .4 + s, -1.3, z + 1.0), .06, 'paint.accent', 'detail')
    for i in range(int(z / .3)):
        k.beam((x0 + .15, -1.3, .25 + i * .3), (x0 + .65, -1.3, .25 + i * .3), .035, 'metal', 'detail')


def aircraft_hangar(k):
    """Round-1 hangar: barrel vault over panel walls, two mech-scale doors, side pilasters."""
    w, d = snap(45.0, 3.0), snap(45.0, 3.0)
    h = 12.0
    rise = 6.5
    for i, (o, yaw, length) in enumerate(sides(w, d)):
        openings = [(-17.0, 0, -4.0, 10.5), (4.0, 0, 17.0, 10.5)] if i == 0 else []
        if i in (1, 3):
            n = round(length / G['bay_m'])
            openings = [(-length / 2 + (b + .5) * length / n - 1.0, 8.6, -length / 2 + (b + .5) * length / n + 1.0, 9.3) for b in range(n) if b % 2]
        with k.at(o, yaw=yaw):
            if i in (0, 2):
                # end wall continues up into the vault gable
                wall_segment(k, -length / 2, length / 2, h, openings=openings, style='inset')
                gable(k, length, h, rise)
            else:
                wall_segment(k, -length / 2, length / 2, h, openings=openings, style='inset')
            if i == 0:
                for cx in (-10.5, 10.5):
                    with k.at((cx, 0, 0)):
                        mech_door_parts(k, 13.0, 10.5)
            for a in openings if i in (1, 3) else []:
                k.window(a[0], a[2], a[1], a[3], 0.0, mullions=2)
            if i in (1, 3):
                n = round(length / 6.0)
                for b in range(1, n):
                    x = -length / 2 + b * length / n
                    k.prism(rect_poly(x - .55, -1.1, x + .55, .05, .3), 0, h - .3, 'paint.main', 'frame', top=.6)
                    k.prism(rect_poly(x - .75, -1.4, x + .75, .1, .35), 0, .9, 'ferro', 'mass', top=.2)
                    k.slab(x - .12, x + .12, 1.4, h - 2.0, -1.12, -1.05, 'paint.accent', 'detail')
    corner_columns(k, w, d, h + .3)
    # vault: corrugated roof sheets in bays running front to back, steel arch ribs
    t = G['wall_thickness_m']
    R = (rise ** 2 + (w / 2) ** 2) / (2 * rise)
    zc = h - .3 + rise - R
    span = w / 2
    a0 = math.asin(span / R)
    nb = round(d / G['bay_m'])
    nth = 120
    for b in range(nb):
        y0, y1 = -d / 2 + b * d / nb + .02, -d / 2 + (b + 1) * d / nb - .02
        verts, faces = [], []
        for layer, off in [(0, 0.0), (1, -.02)]:
            for y in (y0, (y0 + y1) / 2, y1):
                for i in range(nth + 1):
                    a = -a0 + 2 * a0 * i / nth
                    r = R + off + .05 * (.5 + .5 * math.sin(i * math.tau / 3))
                    verts.append((r * math.sin(a), y, zc + r * math.cos(a)))
        stride = nth + 1
        lay = stride * 3
        for j in range(2):
            for i in range(nth):
                a = j * stride + i
                faces.append((a, a + 1, a + stride + 1, a + stride))
                faces.append((lay + a, lay + a + stride, lay + a + stride + 1, lay + a + 1))
        for j in range(2):
            for i in (0, nth):
                a, c = j * stride + i, (j + 1) * stride + i
                f = (a, c, lay + c, lay + a)
                faces.append(f if i == 0 else tuple(reversed(f)))
        for j in (0, 2):
            for i in range(nth):
                a = j * stride + i
                f = (a, lay + a, lay + a + 1, a + 1)
                faces.append(f if j == 0 else tuple(reversed(f)))
        k.add(verts, faces, 'corr.corr', 'roof', removable=True)
    for b in range(0, nb + 1, 2):
        y = -d / 2 + b * d / nb
        pts = [((R + .08) * math.sin(-a0 + 2 * a0 * i / 30), y, zc + (R + .08) * math.cos(-a0 + 2 * a0 * i / 30)) for i in range(31)]
        k.tube(pts, .09, 'paint.trim', 'detail', sides=6)
    # eaves gutters and roof ridge walkway with vents
    for s in (-1, 1):
        k.box((s * (w / 2 - .1), 0, h - .05), (.5, d, .4), 'paint.trim', 'frame', ch=.0, top=.1)
    k.box((0, 0, h - .3 + rise + .15), (3.4, d * .62, .3), 'grate', 'detail')
    for j in range(5):
        y = -d * .28 + j * d * .14
        with k.at((0, y, h - .3 + rise + .3)):
            k.hvac(2.0, 1.6, .9)
    k.hits.append(dict(p=(-8, -6, h - .3 + rise - .6), d=(0, 0, -1), r=4.0, depth=1.6, sag=1.4, reach=1.3, rebar=False, debris=['corr.corr', 'metal'], floor=0.0))
    k.hits.append(dict(p=(w / 2, d * .2, 5.0), d=(-1, 0, 0), r=2.6, depth=.8, sag=.6, rebar=True, debris=['ferro', 'paint.main', 'metal']))
    return dict(size=(w, d, h + rise), stock='hangar', palette='slate_oxide', ref='paul-santoro-ma-fanart-weaponstorage-img1.jpg', round1='mech_hangar_17')


def gable(k, length, h, rise):
    """Panelled infill between wall top and the vault on an end wall."""
    R = (rise ** 2 + (length / 2) ** 2) / (2 * rise)
    zc = h - .3 + rise - R
    n = 12
    for i in range(n):
        x0 = -length / 2 + i * length / n
        x1 = x0 + length / n
        xm = (x0 + x1) / 2
        top = zc + math.sqrt(max(0, R * R - xm * xm)) - .15
        if top > h - .3 + .2:
            k.slab(x0 + .02, x1 - .02, h - .3, top, .02, .38, 'paint.second', 'skin', front=.03, removable=True)
    pts = [(R * math.sin(a), -.08, zc + R * math.cos(a)) for a in [(-math.asin(length / 2 / R) + 2 * math.asin(length / 2 / R) * j / 30) for j in range(31)]]
    k.tube(pts, .16, 'paint.trim', 'frame', sides=6)


def mech_door_parts(k, ow, oh):
    pw = 1.3
    for s in (-1, 1):
        x = s * (ow / 2 + pw / 2 + .05)
        k.prism(rect_poly(x - pw / 2, -1.2, x + pw / 2, .3, .3), 0, oh + 1.2, 'paint.main', 'frame', top=.3)
        k.prism(rect_poly(x - pw / 2 - .2, -1.4, x + pw / 2 + .2, .35, .3), 0, 1.0, 'ferro', 'mass', top=.2)
        k.prism(rect_poly(x - pw / 2 - .21, -1.41, x + pw / 2 + .21, .36, .31), .7, 1.0, 'hazard', 'detail')
        k.slab(x - .16, x + .16, 1.4, oh - .4, -1.24, -1.12, 'paint.accent', 'detail')
        k.lamp_head(x, oh + 1.55, -.9, w=.65)
    k.slab(-ow / 2 - pw, ow / 2 + pw, oh + .05, oh + 1.35, -1.15, .30, 'paint.trim', 'frame', front=.28)
    k.slab(-ow / 2, ow / 2, oh + .25, oh + .5, -1.19, -1.1, 'hazard', 'detail')
    for s in (-1, 1):
        x0, x1 = (0.02, ow / 2 - .05) if s > 0 else (-ow / 2 + .05, -0.02)
        n = 4
        for i in range(n):
            z0 = .1 + i * (oh - .1) / n
            k.slab(x0, x1, z0, z0 + (oh - .1) / n - .06, .05, .32, 'paint.second' if i % 2 else 'paint.main', 'skin', front=.10, removable=True)
    k.slab(-.05, .05, 0, oh, -.02, .3, 'rubber', 'detail')
    k.extrude([(-2.6, 0), (.3, 0), (.3, .16), (-2.5, .03)], -ow / 2, ow / 2, 'ferro', 'mass')
    k.hazard_strip(-ow / 2, ow / 2, -.55, -.2, .12)


# ------------------------------------------------------------------ stock remakes
def quonset_stock(k):
    """Faithful remake of the stock quonset: rust corrugated barrel, grey corrugated ends,
    olive framed door and windows, ferro curbs. Stock bounds 10.63 x 21.04 x 5.33 m."""
    L = 21.0
    radius = 4.85
    geo.p_quonset_shell(k, length=L, radius=radius)
    k.hits.clear()
    curb = .45
    for e, ysign in enumerate((-1, 1)):
        y = ysign * L / 2
        with k.at((0, y, 0), yaw=0 if ysign < 0 else math.pi):
            end_wall(k, radius, curb, door=(e == 0))
    k.hits.append(dict(p=(-1.5, 3.0, curb + radius * .92), d=(.3, 0, -1), r=1.6, depth=.8, sag=.75, reach=1.4, rebar=False, debris=['corr.corr', 'metal'], floor=0.0))
    k.hits.append(dict(p=(1.8, -L / 2, 2.2), d=(0, 1, 0), r=1.1, depth=.5, sag=.3, rebar=False, debris=['corr.main', 'paint.second']))
    return dict(size=(2 * radius + 1, L + 1, radius + curb), stock='quonset', palette='quonset_rust', ref='paul-santoro-ma-fanart-weaponstorage-img1.jpg', round1=None)


def end_wall(k, radius, curb, door=True):
    """Vertical corrugated end wall under the arc; olive door and windows."""
    strip = .75
    n = int(2 * radius / strip)
    holes = []
    if door:
        holes = [(-.7, 0, .7, 2.3), (-3.0, 1.2, -1.8, 2.3), (1.8, 1.2, 3.0, 2.3)]
    else:
        holes = [(-.6, 2.0, .6, 2.9)]
    for i in range(n):
        x0 = -radius + i * strip + .01
        x1 = x0 + strip - .02
        xm = (x0 + x1) / 2
        top = curb + math.sqrt(max(0, radius ** 2 - xm ** 2)) - .08
        for a in geo.subtract_rect((x0, 0, x1, top), holes):
            k.corr_sheet(a[0], a[2], max(a[1], .02), a[3], .0, 'corr.main')
    pts = [(radius * math.cos(math.pi * j / 30), -.08, curb + radius * math.sin(math.pi * j / 30)) for j in range(31)]
    k.tube(pts, .09, 'paint.second', 'frame', sides=6)
    k.slab(-radius, radius, 0, .25, -.12, .1, 'ferro', 'mass', front=.04)
    for a in holes:
        x0, z0, x1, z1 = a
        for (p, q, r, s) in [(x0 - .1, z0 - (.1 if z0 > 0 else 0), x0, z1 + .1), (x1, z0 - (.1 if z0 > 0 else 0), x1 + .1, z1 + .1), (x0, z1, x1, z1 + .1)] + ([(x0, z0 - .1, x1, z0)] if z0 > 0 else []):
            k.slab(p, r, q, s, -.12, .04, 'paint.second', 'frame', front=.02)
        if z0 > 0:
            k.slab(x0, x1, z0, z1, -.04, -.02, 'glass', 'glass', pane=True)
            k.slab((x0 + x1) / 2 - .025, (x0 + x1) / 2 + .025, z0, z1, -.08, -.03, 'paint.second', 'frame')
            k.slab(x0, x1, (z0 + z1) / 2 - .025, (z0 + z1) / 2 + .025, -.08, -.03, 'paint.second', 'frame')
        else:
            k.slab(x0, x1, 0, z1, .02, .08, 'paint.second', 'skin', front=.02)
            k.slab(x0 + .15, x1 - .15, z1 * .62, z1 * .82, -.0, .02, 'glass', 'glass', pane=True)
            k.slab(x0 - .3, x1 + .3, z1 + .1, z1 + .22, -.7, .04, 'paint.second', 'frame', front=.05)
            k.slab(x0 - .2, x1 + .2, 0, .2, -.9, .02, 'ferro', 'mass', front=.04)
            k.lamp_head(0, z1 + .55, -.1, w=.35)


def barracks_stock(k):
    """Faithful remake of the stock barracks: battered khaki ferro bunker, sage inset
    panels with slit windows, gravel roof with skylights, end door. 10.53 x 21.18 x 4.38 m."""
    w, d = 10.5, 21.0
    ph = .5
    zt = 3.85
    batter = 1.15
    k.prism(rect_poly(-w / 2 - .25, -d / 2 - .25, w / 2 + .25, d / 2 + .25, .25), 0, ph, 'ferro', 'mass', top=.12)
    k.moss_sites.append(((-w / 2, -d / 2 - .3, 0), (w / 2, -d / 2 - .3, 0)))
    k.moss_sites.append(((-w / 2 - .3, -d / 2, 0), (-w / 2 - .3, d / 2, 0)))
    k.prism(rect_poly(-w / 2, -d / 2, w / 2, d / 2, .35), ph, zt, 'ferro', 'mass', top=batter, top_h=zt - ph, rebar=True)
    # roof: coping ring, gravel deck, skylights
    rw, rd = w - 2 * batter, d - 2 * batter
    k.prism(rect_poly(-rw / 2 - .1, -rd / 2 - .1, rw / 2 + .1, rd / 2 + .1, .2), zt, zt + .28, 'paint.trim', 'mass', top=.08)
    roof_deck(k, rw + .4, rd + .4, zt + .2, (1.75, 1.75))
    for y in (-rd * .3, 0, rd * .3):
        k.box((0, y, zt + .32), (1.3, 1.3, .25), 'paint.trim', 'detail', ch=.12, top=.05)
        k.box((0, y, zt + .47), (1.1, 1.1, .06), 'glass', 'glass', ch=.1, pane=True)
    with k.at((rw * .28, rd * .38, zt + .2)):
        k.hvac(1.2, 1.0, .6)
    tilt = math.atan(batter / (zt - ph))
    slant = (zt - ph) / math.cos(tilt)
    for i, (o, yaw, length) in enumerate([((0, -d / 2, 0), 0, w), ((w / 2, 0, 0), math.pi / 2, d), ((0, d / 2, 0), math.pi, w), ((-w / 2, 0, 0), -math.pi / 2, d)]):
        with k.at((o[0], o[1], ph), yaw=yaw, pitch=-tilt):
            inner = length - 2 * (.35 + batter * .3)
            if i in (1, 3):
                n = 4
                pw = inner / n
                for b in range(n):
                    cx = -inner / 2 + (b + .5) * pw
                    sage_panel(k, cx - pw / 2 + .35, cx + pw / 2 - .35, .3, slant - .3, slit=True)
            elif i == 0:
                sage_panel(k, -inner / 2 + .3, -1.3, .3, slant - .3, slit=True)
                sage_panel(k, 1.3, inner / 2 - .3, .3, slant - .3, slit=True)
            else:
                sage_panel(k, -inner / 2 + .3, inner / 2 - .3, .3, slant - .3, slit=False)
    # door: vertical portal cut into the battered front, with a hood and steps
    with k.at((0, -d / 2 - .1, 0)):
        k.prism(rect_poly(-1.2, -.6, 1.2, 1.4, .1), ph, 3.2, 'ferro', 'mass', top=.1)
        k.slab(-.6, .6, ph, ph + 2.3, -.62, -.5, 'paint.second', 'skin', front=.02)
        k.slab(-.4, .4, ph + 1.4, ph + 1.95, -.64, -.6, 'glass', 'glass', pane=True)
        for s in (-1, 1):
            k.slab(s * .6 - .1 if s > 0 else -.7, .7 if s > 0 else -.6, ph, ph + 2.4, -.7, -.5, 'paint.trim', 'frame')
        k.slab(-.7, .7, ph + 2.3, ph + 2.45, -.7, -.5, 'paint.trim', 'frame')
        k.slab(-1.35, 1.35, 3.05, 3.3, -1.3, -.4, 'paint.trim', 'frame', front=.08)
        k.lamp_head(0, 3.0, -.85, w=.4)
        k.slab(-1.0, 1.0, 0, ph, -1.6, -.55, 'ferro', 'mass', front=.04)
        k.slab(-1.0, 1.0, ph - .05, ph, -1.61, -1.5, 'hazard', 'detail')
        k.box((1.55, -.7, 1.3), (.45, .3, .6), 'paint.accent', 'detail', ch=.06, top=.04)
    k.hits.append(dict(p=(w / 2 - .5, d * .12, 2.2), d=(-.9, 0, -.4), r=1.8, depth=.6, sag=.45, rebar=True, debris=['ferro', 'paint.second', 'gravel']))
    k.hits.append(dict(p=(.0, -d * .12, zt + .1), d=(0, 0, -1), r=1.7, depth=.8, sag=.55, rebar=True, debris=['gravel', 'ferro'], collapse='roof', reach=1.3))
    return dict(size=(w + 1, d + 1, zt + .8), stock='barracks', palette='barracks_sage', ref='paul-santoro-ma-fanart-barracks-img1.jpg', round1=None)


def sage_panel(k, x0, x1, z0, z1, slit=True):
    for (a, b, c, d) in [(x0 - .1, z0 - .1, x1 + .1, z0), (x0 - .1, z1, x1 + .1, z1 + .1), (x0 - .1, z0, x0, z1), (x1, z0, x1 + .1, z1)]:
        k.slab(a, c, b, d, -.06, .02, 'paint.trim', 'frame', front=.02)
    if slit:
        wz0, wz1 = (z0 + z1) * .5 - .05, (z0 + z1) * .5 + .3
        k.slab(x0, x1, z0, wz0 - .1, -.02, .03, 'paint.second', 'skin', front=.02, removable=True)
        k.slab(x0, x1, wz1 + .1, z1, -.02, .03, 'paint.second', 'skin', front=.02, removable=True)
        k.slab(x0, x1, wz0 - .1, wz0, -.04, .03, 'paint.trim', 'frame')
        k.slab(x0, x1, wz1, wz1 + .1, -.08, .03, 'paint.trim', 'frame', front=.03)
        k.slab(x0, x0 + .2, wz0, wz1, -.02, .03, 'paint.second', 'skin')
        k.slab(x1 - .2, x1, wz0, wz1, -.02, .03, 'paint.second', 'skin')
        k.slab(x0 + .2, x1 - .2, wz0, wz1, -.005, .012, 'glass', 'glass', pane=True)
        k.slab(x0 + .2, x1 - .2, wz0, wz1, .015, .03, 'rubber', 'detail')
    else:
        k.slab(x0, x1, z0, z1, -.02, .03, 'paint.second', 'skin', front=.02, removable=True)
    for zz in (z0 - .05, z1 + .05):
        n = max(2, int((x1 - x0) / .4))
        for j in range(n + 1):
            k.box((x0 + (x1 - x0) * j / n, -.07, zz), (.05, .03, .05), 'metal', 'detail')


ASSEMBLIES = {
    'barracks': barracks,
    'factory': factory,
    'aircraft_hangar': aircraft_hangar,
    'quonset_stock': quonset_stock,
    'barracks_stock': barracks_stock,
}

# Draft names are also available through the existing build CLI.
from drafts import DRAFTS  # noqa: E402
ASSEMBLIES.update({name: (lambda k, n=name: _draft_info(k, n)) for name in DRAFTS})


def _draft_info(k, name):
    import drafts
    result = drafts.build(name, 'pristine', k.seed)
    k.pieces = result.pieces
    k.hits = result.hits
    k.moss_sites = result.moss_sites
    return result.meta


def build(name, condition, seed=17):
    if name in DRAFTS:
        import drafts
        return drafts.build(name, condition, seed)
    info_k = Kit(name, 'slate_oxide', seed)
    info = ASSEMBLIES[name](info_k)
    k = Kit(name, info['palette'], seed)
    ASSEMBLIES[name](k)
    cond = geo.LANG['conditions'][condition]
    if condition == 'abandoned':
        geo.apply_abandoned(k, cond)
    elif condition == 'destroyed':
        geo.apply_destroyed(k)
        for piece in list(k.pieces):
            if piece.get('pane'):
                k.pieces.remove(piece)
    k.meta.update(info, name=name, condition=condition)
    return k


# ------------------------------------------------------------------ CLI hook (called by run.py)
def run(a, out, blender):
    names = a.only.split(',') if a.only else list(DRAFTS)
    conds = a.conditions.split(',')
    if 'quonset' in names and 'quonsetclean' not in names:
        if any(not (out/a.tag/f'pbr/quonsetclean_{c}_lod0.glb').is_file() for c in conds):
            names.insert(0,'quonsetclean')
    # --suffix lets two (max) Blender workers share one pass directory without
    # clobbering each other's job file, log or report.
    tag = a.tag + (f'-{a.suffix}' if getattr(a, 'suffix', None) else '')
    job = {'kind': 'assembly', 'tag': tag, 'out': str(out / a.tag), 'assemblies': names, 'conditions': conds, 'samples': a.samples, 'export': not a.no_export}
    blender(job, out / 'logs' / f'{tag}.log')
    for n in names:
        montage(out / a.tag, n, conds)


def montage(d, name, conds):
    from PIL import Image, ImageDraw, ImageFont, ImageOps
    k = Kit(name, 'slate_oxide')
    info = ASSEMBLIES[name](k)

    def font(s):
        return ImageFont.truetype('C:/Windows/Fonts/segoeui.ttf', s)
    W = max(1900, 20 + len(conds) * 470)
    im = Image.new('RGB', (W, 1440), (24, 28, 32))
    dr = ImageDraw.Draw(im)
    dr.text((20, 12), f'{name.upper()}  /  palette {info["palette"]}  /  stock {info["stock"]}  /  built only from kit-language primitives', font=font(30), fill=(232, 234, 230))

    def panel(path, box, label, nearest=False):
        dr.text((box[0], box[1] - 30), label, font=font(19), fill=(190, 196, 190))
        if path and Path(path).is_file():
            src = Image.open(path).convert('RGB')
            if nearest:
                f = max(1, min(box[2] // src.width, box[3] // src.height))
                t = src.resize((src.width * f, src.height * f), Image.Resampling.NEAREST)
            else:
                t = ImageOps.contain(src, (box[2], box[3]), Image.Resampling.LANCZOS)
            im.paste(t, (box[0] + (box[2] - t.width) // 2, box[1] + (box[3] - t.height) // 2))
    panel(d / f'{name}_pristine_beauty.png', (20, 90, 1100, 760), 'BAKED GAME MESH / pristine / Cycles sun + sky')
    panel(d / f'{name}_pristine_rts.png', (1140, 90, 740, 300), 'MC2 RTS proxy (baked mesh at game pixel size, 2x nearest)', nearest=True)
    panel(THUMBS / f'{info["stock"]}.png', (1140, 440, 360, 230), f'STOCK {info["stock"]} (catalog)')
    panel(REF / info['ref'], (1520, 440, 360, 230), 'MW5/MWO artist ref (study only)')
    r1 = ROUND1 / (info.get('round1') or '_none') / 'beauty.png'
    if r1.is_file():
        panel(r1, (1140, 720, 740, 300), 'ROUND-1 draft (owner liked) for comparison')
    y = 1060
    dr.text((20, y - 34), 'CONDITIONS  ' + ' | '.join(conds), font=font(22), fill=(232, 234, 230))
    for i, c in enumerate(conds):
        panel(d / f'{name}_{c}_beauty.png', (20 + i * 470, y + 20, 460, 340), c)
    im.save(d.parent / f'assembly-{name}.png')
    print('[art] assembly sheet', d.parent / f'assembly-{name}.png')
