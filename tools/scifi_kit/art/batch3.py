"""Next eight usage-ranked stock assets: faction precast walls and chain-link."""
import math
import random

import geo

SIZES = {
    's_mediumwall': (7.5, 35.75385, 13.98720),
    's_heavywall': (7.5, 35.75385, 22.20190),
    'l_lightwall': (5.20073, 34.60168, 5.79030),
    'l_mediumwall': (6.86495, 35.44908, 14.46063),
    'd_mediumwall': (6.84945, 35.45785, 14.36993),
    'wirefence': (.09030, 10.83575, 3.99248),
    'l_heavywall': (9.40500, 36.45998, 22.25758),
    'd_lightwall': (5.67315, 34.85180, 5.61175),
    'd_heavywall': (6.87240, 35.42655, 22.32495),
}


def wall(k, p=None, tag=True):
    """Footing, segmented cast units, joint steel, face fields, coping, fasteners.
    p (archetype params) may override family / variant / spans / wh / post; defaults reproduce the stock walls."""
    g = (p or {}).get('dims', {})
    first_piece = len(k.pieces)
    w, d, h = SIZES[k.name]
    family = g.get('family', k.name[0])
    variant = g.get('variant', 'heavy' if 'heavy' in k.name else 'light' if 'light' in k.name else 'medium')
    heavy = variant == 'heavy'
    light = variant == 'light'
    post = bool(g.get('post'))
    wh = h * g.get('wh', .86 if family == 's' else .83 if family == 'd' else .90)
    if family == 's' and heavy and 'wh' not in g and k.name == 's_heavywall':
        wh = 21.48
    if family == 'd' and heavy:
        wh = 21.18
    thickness = w * (.47 if family == 's' else .43)
    pier_d = w
    py = d/2-pier_d/2
    start, end = -d/2, py-pier_d*.35
    spans = 0 if post else g.get('spans', 4 if family == 's' else 6 if family == 'd' else 3)
    step = (end-start)/spans if spans else 0.0
    edges = [start+i*step for i in range(spans+1)]
    if family == 'l' and not light and not post and 'spans' not in g:
        edges = [start, -9.29 if heavy else -8.45, 4.77 if heavy else 4.94, end]
    footing = .18 if light else .32
    if spans:
        k.box((0, (start+end)/2, footing/2),
              (2*kit.wall_hw(k.name, 0)+.1, end-start, footing), 'ferro', 'mass', ch=kit.C(.08))
    for j in range(spans):
        first = len(k.pieces)
        a, b = edges[j]+.035, edges[j+1]-.035
        # Liao medium/heavy have one lowered central panel and sloping shoulders.
        top = [(a, wh), (b, wh)]
        if family == 'l' and not light:
            drop = h*(.13 if heavy else .10)
            if j == 1:
                top = [(a, wh), (a+(b-a)*.14, wh-drop),
                       (b-(b-a)*.14, wh-drop), (b, wh)]
        if family == 's':
            courses = [footing, wh*.347, wh*.554, wh*.778, wh-.22]
            if heavy:
                courses = [footing, 6.50, 10.37, 17.32, wh-.22]
            for row, (lo, hi) in enumerate(zip(courses, courses[1:])):
                kit.loft(k, (-thickness/2,a,thickness/2,b), lo+.025,
                         (-thickness/2,a,thickness/2,b), hi-.025,
                         'ferro', 'mass', ch0=.08, ch1=.06, course=row, rebar=True)
                for side in (-1, 1):
                    k.box((side*thickness*.48,(a+b)/2,(lo+hi)/2),
                          (.055, b-a-.16, hi-lo-.16), 'ferro.main', 'skin')
        else:
            profile = [(a, footing), (b, footing)] + list(reversed(top))
            k.extrude(profile, -thickness/2, thickness/2, 'paint.second',
                      'mass', axis='x', rebar=True)
            for side in (-1, 1):
                x = side*(thickness/2+.025)
                # The stock Davion face has six narrow units; Liao has three braces.
                k.beam((x,a,footing+.15), (x,b,footing+.15), .14,
                       'paint.main', 'frame')
                for y in (a+.12, b-.12):
                    k.beam((x,y,footing+.12), (x,y,wh-.32), .16,
                           'paint.main', 'frame')
                if family == 'l':
                    lower = wh*.33 if heavy else footing+.38
                    upper = min(z for _,z in top)-.6
                    if light:
                        pairs = [((a+.3,lower),(b-.3,upper))] if j != 1 else [((a+.3,upper),(b-.3,lower))]
                    else:
                        midy, midz = (a+b)/2, (lower+upper)/2
                        if j == 1:
                            pairs = [((a+.4,lower),(midy,midz)), ((a+.4,upper),(midy,midz)),
                                     ((midy,midz),(b-.4,lower)), ((midy,midz),(b-.4,upper))]
                        else:
                            tip, root = (a+.3,b-.4) if j == 0 else (b-.3,a+.4)
                            pairs = [((root,lower),(tip,midz)), ((root,upper),(tip,midz))]
                        hub = midy if j == 1 else tip
                        k.box((x,hub,midz),(.16,1.0,.85),'paint.main','frame')
                    for (y0,z0),(y1,z1) in pairs:
                        k.beam((x,y0,z0), (x,y1,z1), .20, 'paint.main', 'frame', stock_brace=True)
                    if heavy:
                        k.beam((x,a,lower), (x,b,lower), .28, 'paint.main', 'frame')
                        # One recessed lower field per stock span, no extra bays.
                        k.box((x,(a+b)/2,lower/2), (.075,b-a-.45,lower-.5),
                              'paint.second', 'skin', ch=kit.C(.10))
        # The coping is an authored 45 degree cap, slightly narrower on top.
        for (y0,z0),(y1,z1) in zip(top,top[1:]):
            k.extrude([(y0,z0-.24),(y1,z1-.24),(y1,z1),(y0,z0)],
                      -thickness/2-.08, thickness/2+.08, 'ferro.main', 'frame', axis='x')
            kit.hexa(k, [(-thickness/2-.08,y0,z0), (thickness/2+.08,y0,z0),
                         (thickness/2+.08,y1,z1), (-thickness/2-.08,y1,z1)],
                        [(-thickness/2+.10,y0,z0+.18), (thickness/2-.10,y0,z0+.18),
                         (thickness/2-.10,y1,z1+.18), (-thickness/2+.10,y1,z1+.18)],
                     'ferro.main', 'frame')
        # Subtle batter, shared by the high and game silhouettes.
        # Stock battered wall SEGMENT: half-width follows the decoded stock profile
        # (wide at the base, sloping inward); the coping rides on the top width.
        for p in k.pieces[first:]:
            p['v'] = [(x*kit.wall_hw(k.name, z/wh)/(thickness/2),y,z) for x,y,z in p['v']]
            p['wall_span'] = j
        for side in (-1, 1):
            for z in (footing+.2,wh-.28):
                hx = kit.wall_hw(k.name, z/wh)
                kit.bolts(k,(side*hx,a+.2,z),(side*hx,b-.2,z),pitch=.9)
        for p in k.pieces[first:]:
            p['wall_span'] = j
        k.moss_sites.append(((-kit.wall_hw(k.name,0)-.12,a,footing),(-kit.wall_hw(k.name,0)-.12,b,footing)))
    if family == 'd' and heavy and not post:      # a standalone post has no spans to brace
        # Stock lower truss: a chain of two hexagonal braced fields under a horizontal belt.
        lo, hi = footing + .5, wh * .40
        mid = (lo + hi) / 2
        L = end - start
        def tag(y0, y1):
            return min(spans - 1, max(0, int(((y0 + y1) / 2 - start) / step)))
        for cy in (start + L * .27, start + L * .73):
            hw = L * .19
            pts = [(cy - hw, mid), (cy - hw * .45, hi), (cy + hw * .45, hi), (cy + hw, mid), (cy + hw * .45, lo), (cy - hw * .45, lo)]
            for side in (-1, 1):
                for (y0, z0), (y1, z1) in zip(pts, pts[1:] + pts[:1]):
                    hx = kit.wall_hw(k.name, (z0 + z1) / 2 / wh) + .03
                    b = k.beam((side * hx, y0, z0), (side * hx, y1, z1), .26, 'ferro.pale', 'frame')
                    b['wall_span'] = tag(y0, y1)
        for side in (-1, 1):
            for j in range(spans):
                a0, b0 = edges[j], edges[j + 1]
                h0 = kit.wall_hw(k.name, (footing + .1) / wh) + .035
                h1 = kit.wall_hw(k.name, (hi + .3) / wh) + .035
                fq = [(side * h0, a0 + .05, footing + .1), (side * h0, b0 - .05, footing + .1), (side * h1, b0 - .05, hi + .3), (side * h1, a0 + .05, hi + .3)]
                bq = [(x - side * .12, y, z) for x, y, z in fq]
                if side < 0:
                    fq, bq = fq[::-1], bq[::-1]
                b = kit.hexa(k, fq, bq, 'ferro.pale', 'skin', removable=True)
                b['wall_span'] = j
                for zz, ww in ((hi + .45, .5), (footing + .12, .35)):
                    hx = kit.wall_hw(k.name, zz / wh) + .04
                    b = k.beam((side * hx, a0 + .03, zz), (side * hx, b0 - .03, zz), ww, 'ferro.pale', 'frame')
                    b['wall_span'] = j
    # One stock terminal pier. Cast stages and inset fields follow its taper.
    first = len(k.pieces)
    stages = 2 if light else 4 if (family == 'd' and heavy) else 3
    for j in range(stages):
        z0, z1 = h*j/stages, h*(j+1)/stages
        r0, r1 = w/2*(1-.40*j/stages), w/2*(1-.40*(j+1)/stages)
        kit.loft(k,(-r0,py-r0,r0,py+r0), z0,
                 (-r1,py-r1,r1,py+r1), z1-.035,
                 'ferro' if family != 's' else 'ferro.second', 'mass', ch0=.15, ch1=.12, rebar=True)
        # Counted cast face fields wrap the four faces of each pier stage.
        for side in range(4):
            with k.at((0,py,0), yaw=side*math.pi/2):
                kit.hexa(k,[(-r0*.64,-r0-.012,z0+.22),(r0*.64,-r0-.012,z0+.22),
                            (r1*.66,-r1-.012,z1-.22),(-r1*.66,-r1-.012,z1-.22)],
                           [(-r0*.64,-r0+.04,z0+.22),(r0*.64,-r0+.04,z0+.22),
                            (r1*.66,-r1+.04,z1-.22),(-r1*.66,-r1+.04,z1-.22)],
                           'ferro.pier' if family == 'l' else 'ferro.second' if family == 'd' else 'ferro.trim', 'skin')
                kit.bolts(k,(-r0*.4,-r0-.025,z0+.28),(r0*.4,-r0-.025,z0+.28),pitch=.6)
    kit.loft(k,(-w*.31,py-w*.31,w*.31,py+w*.31),h-.20,
             (-w*.28,py-w*.28,w*.28,py+w*.28),h,'ferro.main','frame')
    for p in k.pieces[first:]:
        p['pier'] = True
    if p and tag:
        for pc in k.pieces[first_piece:]:
            if pc.get('pier'):
                pc['group'] = 'support:0'
            elif 'wall_span' in pc:
                pc['group'] = f'panel:{pc["wall_span"]}'
    k.meta['wall_layout'] = (edges,thickness,wh,py)
    k.hits = [dict(p=(0,start+step*1.5,wh*.48),d=(1,0,0),r=max(step*.65,w*.6),depth=0,sag=0)]


def gate(k, p):
    """Two battered wall runs with terminal piers, a lintel-free central opening and a pair of gate leaves
    (frame first, then separate skin panels). Built from wall() so the piers/profile/courses are shared."""
    import drafts
    w, d, h = SIZES[k.name]
    g = p.get('dims', {})
    gap = g.get('gap', d * .46)
    seg = (d - gap) / 2
    saved = SIZES[k.name]
    try:
        SIZES[k.name] = (w, seg, h)
        drafts.SIZES[k.name] = SIZES[k.name]
        for sgn in (-1, 1):
            with k.at((0, sgn * (gap / 2 + seg / 2), 0), yaw=math.pi if sgn < 0 else 0.0):
                wall(k, dict(p, dims=dict(g, spans=g.get('spans_each', 2))), tag=False)
    finally:
        SIZES[k.name] = saved
        drafts.SIZES[k.name] = saved
    k.meta.pop('wall_layout', None)
    lh = g.get('leaf_h', h * .85)
    t = g.get('leaf_t', min(w * .3, 2.5))
    gw = gap / 2 - .25
    for sgn in (-1, 1):
        with k.tagged(f'panel:{0 if sgn < 0 else 1}'):
            y0, y1 = sgn * .12, sgn * (gw + .1)
            ya, yb = min(y0, y1), max(y0, y1)
            for z in (.3, lh - .3):
                k.beam((0, ya, z), (0, yb, z), .3, 'metal', 'frame', h=.3)
            for i in range(5):
                y = ya + (yb - ya) * i / 4
                k.beam((0, y, .3), (0, y, lh - .3), .3, 'metal', 'frame', h=.28)
            rows = max(2, round(lh / 3.2))
            rh = (lh - .8) / rows
            for r in range(rows):
                k.box((0, (ya + yb) / 2, .4 + (r + .5) * rh), (t, yb - ya - .2, rh - .06), 'paint.main', 'skin', ch=kit.C(.03), removable=True)
            k.box((0, (ya + yb) / 2, lh * .45), (t + .08, yb - ya - .2, .7), 'hazard', 'detail')
    k.hits.append(dict(p=(0, 0, lh * .5), d=(1, 0, 0), r=min(gap * .3, 8), depth=0, sag=0))


def wall_damage(k, condition):
    """Broken standing stubs with rebar and thick fallen cast units, never sheets."""
    rng = random.Random(k.seed*31+19)
    edges,t,wh,py = k.meta['wall_layout']
    spans = len(edges)-1
    start,step = edges[0], edges[1]-edges[0]
    h = SIZES[k.name][2]
    # Coordinates must use the same fitted frame as the source geometry.
    sy = k.meta['fit_scale_xyz'][1]
    cy = k.meta['fit_center'][1]
    Y = lambda y: (y-cy)*sy
    gone = {1} if condition == 'partial' else set(range(spans))
    k.pieces = [p for p in k.pieces if p.get('wall_span') not in gone]
    if condition == 'destroyed':
        for p in k.pieces:
            if p.get('pier'):
                p['v'] = [(x,y,z*.24) for x,y,z in p['v']]
    for j in sorted(gone):
        a,b = Y(edges[j])+.05, Y(edges[j+1])-.05
        top = min(wh*.30,2.8) if condition == 'partial' else wh*.12
        ridge = [(a+(b-a)*i/6,.3+top*rng.uniform(.5,1)) for i in range(7)]
        k.extrude([(a,.15),(b,.15)]+list(reversed(ridge)), -t*.46,t*.46,
                  'ferro','mass',axis='x',rebar=True)
        for i in range(3):
            y=a+(b-a)*(i+1)/4
            kit.wire(k,[(0,y,top*.65),(.06,y,top+.5),(.28,y+.10,top+.75)],
                     .022,'metal','frame',sides=5)
        side = -1 if j%2 else 1
        # Actual fallen slab volume is sunk into its rubble bed, tilted <30 degrees.
        with k.at((side*t*.95,(a+b)/2,.33),roll=side*.12,yaw=rng.uniform(-.15,.15)):
            k.box((0,0,0),(t*.85,(b-a)*.80,.50),'ferro','mass',ch=kit.C(.08))
        for i in range(4):
            kit.rock(k,(side*rng.uniform(t*.5,t*1.3),rng.uniform(a,b),.20),
                     rng.uniform(.35,.70),'ferro',rng)
    k.hits=[dict(p=(0,Y(start+step*1.5),1),d=(1,0,0),r=step,depth=0,sag=0)]
    k.meta['collapse']='short-standing-wall-remnant'


def wirefence(k, d=None, panels=2):
    """Two stock chain-link panels, three posts, one top rail; no razor crown (wirefencediag: a longer run, three panels)."""
    w,d0,h = SIZES['wirefence']
    d = d or d0
    for y in [-d/2+d*i/panels for i in range(panels+1)]:
        k.box((0,y,h/2),(w,.12,h),'metal','frame',ch=kit.C(.008))
    k.box((0,0,h-.045),(w,d,.09),'metal','frame')
    # Nine diamond pitches per stock texture tile, two tiles over the full span.
    pitch = d/(9*panels)
    slope = h/(pitch*8)
    for sign in (-1,1):
        for i in range(-9,9*panels+10):
            y0=-d/2+i*pitch
            lo=max(0,(-d/2-y0)*slope*sign)
            hi=min(h,(d/2-y0)*slope*sign)
            if sign<0:
                lo=max(0,(d/2-y0)*slope*sign)
                hi=min(h,(-d/2-y0)*slope*sign)
            if hi-lo<.02:
                continue
            points=[(sign*.010,y0+sign*lo/slope,lo),
                    (sign*.010,y0+sign*hi/slope,hi)]
            kit.wire(k,points,.012,'metal','skin',sides=4)
    k.hits=[dict(p=(0,0,h*.5),d=(1,0,0),r=2,depth=0,sag=0)]


DRAFTS = {name:(wirefence if name == 'wirefence' else wall,
                  'galvanised' if name == 'wirefence' else
                  'wall_cast' if name.startswith('s_') else
                  'wall_liao' if name.startswith('l_') else 'wall_davion')
          for name in SIZES}
DAMAGE = {name:wall_damage for name in SIZES if name != 'wirefence'}

import drafts as kit  # noqa: E402: reuse the existing construction helpers
DAMAGE['wirefence'] = kit.batch2.topple
