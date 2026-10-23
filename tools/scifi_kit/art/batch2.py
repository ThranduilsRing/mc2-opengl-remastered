"""Campaign stock remakes, batch two. Original construction; stock owns counts."""
import math
import random

import geo

SIZES = {
    'streetlight': (1.11645, 1.03345, 10.32517),
    'woodencrates': (10.6255, 3.77875, 4.24323),
    'runwaylight': (.50368, .58160, .71127),
    'tripodbarricade': (2.51270, 10.48685, 2.26730),
    'redbeaconlight': (.50368, .58160, .71127),
    'corrigatedmetalbarricade': (10.51517, 3.09370, 2.07748),
    'singlecivlivinga': (9.80862, 8.68680, 9.23065),
    'portable': (9.80860, 18.26408, 4.96780),
}


def window(k, x0, x1, z0, z1, yf=0, mullions=1):
    """Stock narrow reveals; no added canopy or broad kit-language hood."""
    for a,b,c,d in ((x0-.10,x0,z0-.10,z1+.10),(x1,x1+.10,z0-.10,z1+.10),
                    (x0,x1,z0-.10,z0),(x0,x1,z1,z1+.10)):
        k.slab(a,b,c,d,yf-.035,yf+.12,'paint.trim','frame',front=kit.C(.02))
    step=(x1-x0)/(mullions+1)
    for i in range(mullions+1):
        k.slab(x0+i*step+.02,x0+(i+1)*step-.02,z0+.02,z1-.02,
               yf+.045,yf+.075,'glass','glass',pane=True)
        if i:
            k.slab(x0+i*step-.025,x0+i*step+.025,z0,z1,yf-.015,yf+.10,'paint.trim','frame')


def streetlight(k):
    # Stock has one square pole, two dark collars and one inclined lamp head.
    k.box((0, .40, 4.92), (.21, .21, 9.84), 'metal', 'frame', ch=kit.C(.025))
    for z in (3.0, 7.9):
        k.box((0, .40, z), (.22, .22, .21), 'paint.trim', 'frame', ch=kit.C(.015))
    kit.hexa(k, [(-.558, -.517, 9.82), (.558, -.517, 9.82),
                 (.558, .517, 10.325), (-.558, .517, 10.325)],
                [(-.558, -.517, 9.93), (.558, -.517, 9.93),
                 (.558, .517, 10.325), (-.558, .517, 10.325)], 'paint.main', 'frame')
    with k.at((0, -.06, 10.06), pitch=.45):
        k.box((0, 0, -.035), (.94, .82, .035), 'lamp', 'glass')
    kit.bolts(k, (-.43, -.45, 9.94), (.43, -.45, 9.94), pitch=.4)


def marker(k):
    # The stock marker is a low faceted luminous wedge, not a tall new beacon.
    base = [(-.25184, 0), (.25184, -.2908), (.25184, .2908)]
    k.prism(base, 0, .07, 'metal', 'frame')
    vv = [(x, y, .07) for x, y in base] + [(-.11084, 0, .71127), (.18134, -.16868, .71127), (.18134, .16868, .71127)]
    k.add(vv, [(2, 1, 0), (3, 4, 5), (0, 1, 4, 3), (1, 2, 5, 4), (2, 0, 3, 5)],
          'lamp_red.marker' if k.name == 'redbeaconlight' else 'lamp.marker', 'skin')


def woodencrates(k):
    for i, x in enumerate(kit.CR['xs']):
        kit.crate(k, x, i, broken=k.cond == 'partial', end_midrail=True)


def crate_damage(k, condition):
    rng = random.Random(k.seed * 17 + 3)
    if condition == 'partial':
        for p in list(k.pieces):
            if p.get('group') == 'crate0' and p.get('board', (0, 0))[0] < 0 and p['board'][1] in (4, 5, 6, 7):
                geo.lay_flat(k, p, (-2.6 + rng.uniform(-1, 1), -2.5, .03),
                             rng.uniform(-.4, .4), rng, crumple=.02)
    else:
        k.pieces = []
        for cx in (-2.65, 2.65):
            k.box((cx, 0, .38), (4.2, 2.9, .76), 'wood.second', 'mass', ch=kit.C(.08))
            for i in range(14):
                with k.at((cx + rng.uniform(-1.5, 1.5), rng.uniform(-1.3, 1.3), rng.uniform(.6, 1.0)),
                          yaw=rng.uniform(-.5, .5), roll=rng.uniform(-.2, .2)):
                    k.box((0, 0, 0), (rng.uniform(1.8, 3.8), .28, .10), 'wood.main', 'skin')
            k.box((cx - 1.9, 1.3, .65), (.18, .18, 1.3), 'wood.second', 'frame')
    k.hits = [dict(p=(-2.6, -1.7, 1.4), d=(0, 1, 0), r=1.5, depth=0, sag=0)]


def topple(k, condition):
    # Whole sections fall together, preserving every stock support.
    angle = math.radians(35 if condition == 'partial' else 88)
    axis = (0,1,0)
    if k.name == 'corrigatedmetalbarricade':
        angle = math.radians(-20 if condition == 'partial' else -42)
        axis = (1,0,0)
    for p in k.pieces:
        p['v'] = [geo.rotate_about(v, (0, 0, 0), axis, angle) for v in p['v']]
    floor = min(v[2] for p in k.pieces for v in p['v'])
    for p in k.pieces:
        p['v'] = [(x, y, z - floor + .01) for x, y, z in p['v']]
    k.hits = [dict(p=(0, 0, .3), d=(0, 0, -1), r=1, depth=0, sag=0)]


def barricade_damage(k, condition):
    if condition == 'partial':
        return topple(k, condition)
    # The A-frames fold under the fallen ramp; the sheets cannot balance on edge.
    for i,p in enumerate(k.pieces):
        x,y,z = geo.centroid(p)
        if p['role']=='skin':
            p['v']=[geo.rotate_about(v,(0,0,0),(1,0,0),-math.atan2(1.99,2.25)) for v in p['v']]
        geo.lay_flat(k,p,(x,y,.10 if p['role']=='skin' else .015),0,
                     random.Random(k.seed*7919+i),crumple=.01)
    k.hits = [dict(p=(0,0,.1),d=(0,0,-1),r=2,depth=0,sag=0)]


def tripod(k):
    for y in (-4.20, -2.08, .035, 2.15, 4.25):
        # Three timber members; the two side legs carry the stock hazard X.
        k.beam((-1.20, y, .06), (1.20, y, 1.93), .13, 'wood.main', 'frame')
        for sign in (-1, 1):
            k.beam((.44, y + sign * .93, .06), (.64, y - sign * .93, 2.20),
                   .13, 'hazard', 'frame')
        k.box((.55, y, 1.12), (.25, .25, .22), 'metal', 'detail', ch=kit.C(.025))


def metal_barricade(k):
    # Three A-frames and their purlin are erected before the two lapped sheets.
    for x in (-5.10, -.035, 5.13):
        for y in (-1.45, 1.49):
            k.beam((x, y, .04), (x, .73, 1.99), .10, 'metal', 'frame')
    k.beam((-5.16, .71, 1.96), (5.16, .71, 1.96), .10, 'metal', 'frame')
    for i, (x0, x1) in enumerate(((-5.25, -1.79), (-1.85, 5.25))):
        n = 1 if geo.GAME_MESH else max(2, round((x1 - x0) / .04))
        v = []
        for zoff in (0, -.035):
            for y, z in ((-1.50, .055), (.75, 2.045)):
                for j in range(n + 1):
                    x = x0 + (x1 - x0) * j / n
                    wave = 0 if geo.GAME_MESH else .018 * math.sin(x * math.tau / .152)
                    v.append((x, y, z + zoff + wave + i * .014))
        stride = n + 1
        f = []
        for j in range(n):
            f += [(j, j+1, stride+j+1, stride+j),
                  (2*stride+j, 3*stride+j, 3*stride+j+1, 2*stride+j+1)]
        for j in (0, n):
            f.append((j, stride+j, 3*stride+j, 2*stride+j))
        for j in range(n):
            f += [(j, 2*stride+j, 2*stride+j+1, j+1),
                  (stride+j, stride+j+1, 3*stride+j+1, 3*stride+j)]
        k.add(v, f, 'corr.main', 'skin', removable=True)
    kit.bolts(k, (-5, .76, 2.08), (5, .76, 2.08), pitch=1.0)


def portable(k):
    # The stock lot includes a left-hand timber porch; the room is only 7.4 m wide.
    x0, x1, depth, base, roof = -2.54, 4.88, 18.20, .80, 4.82
    cx = (x0 + x1) / 2
    k.box((cx, 0, .40), (7.20, 17.60, .80), 'ferro', 'mass', ch=kit.C(.10))
    for x in (x0+.12, x1-.12):
        for y in (-9, -3, 3, 9):
            k.box((x, y, 2.8), (.18, .18, 4.0), 'metal', 'frame')
        for z in (1.0, 2.5, 4.7):
            k.box((x, 0, z), (.12, 18.0, .12), 'metal', 'frame')
    for y in (-9, -3, 3, 9):
        kit.ibeam(k, (x0+.12,y,4.65), (x1-.12,y,4.65), d=.18, w=.12)
    # Precast wall units, steel joint battens, green recessed closure panels.
    for side, (o, yaw, length, openings) in enumerate([
        ((cx, -depth/2, 0), 0, x1-x0, [(0, 3.0, 3.55)]),
        ((x1, 0, 0), math.pi/2, depth, [(t, 2.7, 3.4) for t in (-6, 0, 6)]),
        ((cx, depth/2, 0), math.pi, x1-x0, [(0, 3.0, 3.55)]),
        ((x0, 0, 0), -math.pi/2, depth, [(t, 2.7, 3.4) for t in (-6, 6)]),
    ]):
        with k.at(o, yaw=yaw):
            holes = [(t-1.45, z0, t+1.45, z1) for t, z0, z1 in openings]
            if side == 3:
                holes.append((-.66, base, .66, 3.65))
            n = 3 if side in (1, 3) else 1
            for b in range(n):
                a, z = -length/2+b*length/n, -length/2+(b+1)*length/n
                for l, bot, r, top in geo.subtract_rect((a, base, z, 4.70), holes):
                    k.slab(l, r, bot, top, .0, .19, 'ferro', 'skin', front=kit.C(.025), removable=True)
                # A recessed sage panel under each window is part of that wall bay.
                t = (a+z)/2
                if side != 3 or b != 1:
                    k.slab(t-1.70, t+1.70, .90, 2.62, -.022, .025, 'paint.second', 'skin', removable=True)
                if b:
                    k.slab(a-.045, a+.045, base, 4.70, -.04, .06, 'metal', 'frame')
            for t, z0, z1 in openings:
                window(k, t-1.45, t+1.45, z0, z1, -.04, mullions=1)
            if side == 3:
                k.slab(-.64, .64, base, 3.65, .07, .18, 'paint.second', 'skin', removable=True)
                window(k, -.44, .44, 2.6, 3.4, -.02, mullions=0)
            k.slab(-length/2, length/2, 4.65, 4.94, -.04, .25, 'ferro', 'frame', front=kit.C(.05))
            kit.bolts(k, (-length/2+.15, -.05, 4.59), (length/2-.15, -.05, 4.59), pitch=1.5)
    for y in (-6.07, 0, 6.07):
        k.box((cx, y, roof), (7.15, 6.10, .20), 'gravel', 'roof', removable=True)
    for i, y in enumerate((-6.2, 0, 6.2)):
        k.box((cx + (.8 if i == 1 else 0), y, 4.96), (.85, 1.25 if i == 1 else .85, .025),
              'paint.main', 'detail', ch=kit.C(.025))
    # Porch posts/rails precede the boards. Four stock treads descend toward -Y.
    for x in (-4.75, -2.70):
        for y in (-1.25, 1.25):
            k.box((x, y, .40), (.14, .14, .80), 'wood.second', 'frame')
        k.box((x, 0, .69), (.16, 2.65, .18), 'wood.second', 'frame')
    for i in range(9):
        k.box((-3.72, -1.21+i*.30, .78), (2.35, .29, .08), 'wood.main', 'skin')
    for i in range(4):
        k.box((-3.72, -1.45-i*.23, .70-i*.21), (2.35, .30, .12), 'wood.main', 'skin')
    k.moss_sites.append(((x0, -9, 0), (x0, 9, 0)))
    k.hits = [dict(p=(x1, -4.5, 2.8), d=(-1, 0, 0), r=3.1, depth=.5, sag=.6),
              dict(p=(cx, -4.0, roof), d=(0, 0, -1), r=3.8, depth=.6, sag=.6)]


HOUSE_ARCH = [(-4.43, .20), (-4.0, 2.33), (-2.81, 5.15), (-1.08, 6.13),
              (.99, 6.13), (3.0, 4.92), (4.40, 2.73), (4.43, .20)]


def house(k):
    # Cast footing -> polygonal ribs -> purlins -> lapped arched cladding.
    k.box((0, 0, .15), (8.9, 8.60, .30), 'ferro', 'mass', ch=kit.C(.12))
    for y in (-4.18, 0, 4.18):
        for (x0, z0), (x1, z1) in zip(HOUSE_ARCH, HOUSE_ARCH[1:]):
            kit.ibeam(k, (x0, y, z0), (x1, y, z1), d=.16, w=.12)
    for x, z in HOUSE_ARCH[1:-1]:
        kit.cee(k, (x, -4.25, z-.12), (x, 4.25, z-.12), d=.12, w=.06)
    for j, ((x0, z0), (x1, z1)) in enumerate(zip(HOUSE_ARCH, HOUSE_ARCH[1:])):
        for i in range(3):
            y0, y1 = -4.34+i*8.68/3, -4.34+(i+1)*8.68/3+.06
            proud = (i % 2)*.014
            kit.hexa(k, [(x0-.03, y0, z0+proud), (x1+.03, y0, z1+proud),
                         (x1+.03, y1, z1+proud), (x0-.03, y1, z0+proud)],
                        [(x0-.03, y0, z0-.10), (x1+.03, y0, z1-.10),
                         (x1+.03, y1, z1-.10), (x0-.03, y1, z0-.10)],
                        'paint.roof', 'roof', removable=True)
    for x, z in HOUSE_ARCH[1:-1]:
        kit.bolts(k, (x,-4.20,z+.025), (x,4.20,z+.025), pitch=.50)
    # End infill follows the stock arch; each end has one three-pane window band.
    for end in (-1, 1):
        with k.at((0, end*4.24, 0), yaw=0 if end == -1 else math.pi):
            profile = HOUSE_ARCH if end == -1 else [(-x,z) for x,z in reversed(HOUSE_ARCH)]
            for (x0, z0), (x1, z1) in zip(profile, profile[1:]):
                for a, bot, b, top in geo.subtract_rect((x0, .3, x1, min(z0, z1)), [(-2.3, 3.25, 2.3, 4.05)]):
                    if top > bot:
                        k.slab(a, b, bot, top, 0, .12, 'paint.second', 'skin', removable=True)
                if max(z0, z1) > min(z0, z1):
                    triangle = [(x0,z0), (x1,z0), (x1,z1)] if z0 < z1 else [(x0,z1), (x1,z1), (x0,z0)]
                    k.extrude(triangle,
                              0, .12, 'paint.main', 'skin', axis='y', removable=True)
            window(k, -2.3, 2.3, 3.25, 4.05, -.08, mullions=2)
            for x in (-3.4, -2.5, -1.6, -.7, .2, 1.1, 2, 2.9, 3.8):
                k.box((x, -.05, 1.25), (.06, .12, 1.8), 'paint.trim', 'frame')
    # Two paired-pane windows follow each roof slope, without extra entrances.
    for side in (-1, 1):
        for y in (-2.3, 2.2):
            with k.at((side*3.1, y, 4.84), yaw=side*math.pi/2, pitch=-.57):
                k.box((0, .12, 0), (1.25, .12, 1.55), 'paint.main', 'frame', ch=kit.C(.04))
                window(k, -.48, .48, -.60, .60, -.11, mullions=0)
                k.box((0,-.14,0), (1.03,.08,.045), 'paint.trim', 'frame')
    k.box((-4.20, 3.32, 1.57), (1.40, 2.04, 3.14), 'ferro', 'mass', ch=kit.C(.08))
    with k.at((-4.91, 3.32, 0), yaw=-math.pi/2):
        k.slab(-.48, .48, .25, 2.60, -.025, .025, 'paint.main', 'skin', removable=True)
        window(k, -.26, .26, 1.85, 2.30, -.08, mullions=0)
    k.box((4.42, 2.17, 1.36), (.96, 4.34, 2.72), 'ferro', 'mass', ch=kit.C(.08))
    k.box((-.10, 3.34, 6.87), (1.57, 2.0, 1.48), 'ferro', 'mass', ch=kit.C(.08))
    k.cyl((-.10, 3.34, 8.42), .61, 1.62, 'metal', 'frame', sides=12)
    k.cyl((-.10, 3.34, 9.22), .46, .025, 'char', 'detail', sides=12)
    k.moss_sites.append(((-4.4, -4.2, 0), (-4.4, 4.2, 0)))
    k.hits = [dict(p=(3.0, -2.0, 4.2), d=(-1, 0, -.3), r=2.8, depth=.5, sag=.6)]


def rubble(k, name):
    rng = random.Random(k.seed*31+7)
    w, d, h = SIZES[name]
    fallen = [p for p in k.pieces if p.get('removable') and p['role'] == 'roof']
    k.pieces = []
    peak = h*.24
    kit.mound(k, w*.94, d*.94, peak, 'gravel', power=.65)
    def height(x, y):
        return peak*max(0, 1-(x/(w*.47))**2-(y/(d*.47))**2)**.65
    # Kinked frame members rest on the closed aggregate core.
    for i in range(5):
        x, y = rng.uniform(-w*.25, w*.25), rng.uniform(-d*.25, d*.25)
        z = height(x, y)
        a, b, c = (x-1.2, y-.8, z*.8), (x, y, z+.15), (x+1.3, y+.6, z*.8)
        kit.ibeam(k, a, b, d=.16, w=.12)
        kit.ibeam(k, b, c, d=.16, w=.12)
    for i, p in enumerate(fallen[::max(1,len(fallen)//6)][:6]):
        x, y = rng.uniform(-w*.15, w*.15), rng.uniform(-d*.15, d*.15)
        geo.lay_flat(k, p, (x, y, height(x,y)+.04), rng.uniform(-.4,.4), rng, crumple=.02)
        lo, hi = geo.bbox(p)
        dx = max(0, -w*.48-lo[0])-max(0, hi[0]-w*.48)
        dy = max(0, -d*.48-lo[1])-max(0, hi[1]-d*.48)
        p['v'] = [(x+dx,y+dy,z) for x,y,z in p['v']]
        k.pieces.append(p)
    for i in range(22):
        x, y = rng.uniform(-w*.40,w*.40), rng.uniform(-d*.40,d*.40)
        kit.rock(k, (x,y,height(x,y)*.88), rng.uniform(.30,.70), 'ferro', rng)
    if name == 'singlecivlivinga':
        k.box((-.10, 3.0, 1.40), (1.55, 1.70, 2.80), 'ferro', 'frame', ch=kit.C(.12))
        with k.at((.4, 2.6, 2.4), roll=.25):
            k.cyl((0,0,0), .60, 1.40, 'metal', 'frame', sides=12)
    else:
        k.box((-2.45, 7.3, .95), (.30, 1.6, 1.90), 'ferro', 'frame', ch=kit.C(.06))
        for y in (-.9,-.3,.3,.9):
            k.box((-3.7,y,.20), (2.1,.29,.12), 'wood.main', 'skin')
    for p in k.pieces:
        p['v']=[(x,y,max(.01,z)) for x,y,z in p['v']]
    k.hits=[dict(p=(0,0,.3),d=(0,0,-1),r=min(w,d)*.3,depth=0,sag=0)]
    k.meta.update(collapse='full-rubble',rubble_height_ratio=.24)


DRAFTS = {
    'streetlight': (streetlight, 'galvanised'),
    'woodencrates': (woodencrates, 'canvas_timber'),
    'runwaylight': (marker, 'galvanised'),
    'redbeaconlight': (marker, 'galvanised'),
    'tripodbarricade': (tripod, 'tripod_stock'),
    'corrigatedmetalbarricade': (metal_barricade, 'factory_steel'),
    'singlecivlivinga': (house, 'civ_arch'),
    'portable': (portable, 'portable_stock'),
}
DAMAGE = {'streetlight': topple, 'runwaylight': topple,
          'redbeaconlight': topple, 'woodencrates': crate_damage,
          'tripodbarricade': topple, 'corrigatedmetalbarricade': barricade_damage}

import drafts as kit  # noqa: E402: definitions must exist before draft registration
