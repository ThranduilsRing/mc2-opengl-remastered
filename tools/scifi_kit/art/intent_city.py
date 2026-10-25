"""Civilian family, group 2 (owner 2026-10-24): the twelve city blocks on the stock 22 m lots, 2/4/6/8 storeys in
three stock facade families. Clean and glass-heavy (owner): a dark plinth with storefront or lobby glazing, pale
cladding, window grids with thin mullions, curtain-wall strips, an entrance canopy where the stock puts its door,
a coping and a little tidy rooftop plant. No stairs, ladders, rails or hazard marks."""
import math

import intent as I
import intent_military as M

count = I.count
H_ST = {2: 8.68, 4: 17.73, 6: 26.67, 8: 35.46}
DOOR = {'city2storya': 'w', 'city2storyb': 'e', 'city2storyc': 'w', 'city4storya': 'w', 'city4storyb': 'w', 'city4storyc': 'w',
        'city6storya': 'e', 'city6storyb': 'e', 'city6storyc': 'e', 'city8storya': 'w', 'city8storyb': 'e', 'city8storyc': 'e'}


def pane(k, x0, x1, z0, z1, mull=1.2, frame='paint.frame', t=(-.12, -.08)):
    """Glazing on a frame (wall frame, outward -Y): glass a few cm proud of the wall on a perimeter frame with thin
    vertical mullions every `mull` m and a transom."""
    yf, yb = t
    k.slab(x0 - .06, x1 + .06, z0 - .06, z1 + .06, yf + .02, .01, frame, 'frame')
    k.slab(x0, x1, z0, z1, yf, yf + .03, 'glass', 'glass', pane=True)
    n = max(1, round((x1 - x0) / mull))
    for i in range(1, n):
        x = x0 + (x1 - x0) * i / n
        k.slab(x - .03, x + .03, z0, z1, yf - .03, yf + .01, frame, 'frame')
    if z1 - z0 > 1.6:
        zt = z0 + (z1 - z0) * .78
        k.slab(x0, x1, zt - .03, zt + .03, yf - .03, yf + .01, frame, 'frame')


def facade(k, L, H, storeys, style, door_face, ground=4.6):
    """One face of a block (wall frame centred on the face, outward -Y, z from the ground)."""
    sh = (H - ground - .8) / max(1, storeys - 1)            # upper storey height
    # plinth: dark base course with a glazed storefront / lobby
    k.slab(-L / 2, L / 2, 0, .6, -.12, .02, 'paint.plinth', 'frame')
    if door_face:
        pane(k, -L / 2 + 1.0, -2.6, .7, ground - .5, 1.4)
        pane(k, 2.6, L / 2 - 1.0, .7, ground - .5, 1.4)
        pane(k, -2.4, 2.4, ground - 1.6, ground - .5, 1.2)                     # transom over the doors
        for xl in (-1.15, 1.15):                                                 # glazed double doors
            k.slab(xl - 1.0, xl + 1.0, .05, ground - 1.75, -.1, -.06, 'glass', 'glass', pane=True, door=True, feature='door')
            k.slab(xl - 1.06, xl + 1.06, .05, ground - 1.69, -.05, .01, 'paint.frame', 'frame')
        count(k, 'doors')
        k.slab(-3.4, 3.4, ground - .45, ground - .25, -.45, .01, 'paint.second', 'frame')   # slim canopy ledge over the doors
        k.slab(-2.2, 2.2, ground - .2, ground + .5, -.16, .01, 'paint.sign', 'frame')                  # name panel
        I.wall_lamp(k, -3.0, ground - .9, 0.0)
    else:
        pane(k, -L / 2 + 1.0, L / 2 - 1.0, 1.0, ground - .6, 2.0)
    k.slab(-L / 2, L / 2, ground - .2, ground + .05, -.18, .02, 'paint.second', 'frame')            # ground floor cornice
    # upper storeys by family
    z = ground
    for s in range(storeys - 1):
        z0, z1 = z + .9, z + sh - .45
        if style == 'a':          # punched window grid in pale cladding, deep reveals
            n = 6
            for i in range(n):
                xc = -L / 2 + L * (i + .5) / n
                pane(k, xc - 1.0, xc + 1.0, z0, z1, 1.0, t=(-.04, -.0))
                k.slab(xc - 1.15, xc + 1.15, z0 - .18, z0 - .06, -.14, .02, 'paint.frame', 'frame')
        elif style == 'b':        # continuous ribbon glazing per floor
            pane(k, -L / 2 + .6, L / 2 - .6, z0, z1, 1.35)
            k.slab(-L / 2, L / 2, z1 + .12, z1 + .3, -.16, .02, 'paint.second', 'frame')
        else:                      # two dark curtain-wall strips with punched windows between
            for xc in (-L * .2, L * .2):
                pane(k, xc - 1.7, xc + 1.7, z0 - .5, z + sh + .2 if s < storeys - 2 else z1, 1.7, t=(-.14, -.1))
            for xc in (-L * .42, 0.0, L * .42):
                pane(k, xc - .8, xc + .8, z0, z1, .8)
        z += sh
    count(k, 'windows', storeys - 1)
    # corner piers for the brick family, crown band for all
    if style == 'a':
        for x in (-L / 2, L / 2):
            k.slab(x - .45, x + .45, .6, H, -.2, .02, 'paint.second', 'frame')
    k.slab(-L / 2, L / 2, H - .6, H - .2, -.18, .02, 'paint.frame', 'frame')


def city_block(k):
    name = k.name
    storeys = int(name[4])
    style = name[-1]
    H = H_ST[storeys] - .5
    W = 21.6
    k.box((0, 0, H / 2), (W, W, H), 'paint.main', 'mass', ch=.1)
    box = (-W / 2, -W / 2, W / 2, W / 2)
    for s in 'nesw':
        o, yaw, L = M.frame_of(box, s)
        with k.at((o[0], o[1], 0), yaw=yaw):
            facade(k, L, H, storeys, style, s == DOOR[name], ground=4.6 if storeys > 2 else 4.2)
    # coping, roof deck, tidy plant (stock roof units)
    k.box((0, 0, H + .25), (W + .4, W + .4, .5), 'paint.frame', 'frame', ch=.05)
    k.box((0, 0, H + .02), (W - .6, W - .6, .04), 'gravel', 'roof', walk=True)
    k.box((4.0, -3.5, H + .6), (4.2, 3.0, 1.2), 'paint.second', 'mass', ch=.08, top=.05)
    for x in (2.9, 5.1):
        k.cyl((x, -3.5, H + 1.23), .8, .06, 'grate', 'frame', sides=18)
    k.box((-5.5, 5.5, H + .5), (2.2, 2.2, 1.0), 'paint.second', 'mass', ch=.06, top=.05)
    if storeys >= 6:
        k.cyl((-5.5, -4.0, H + .7), 1.3, 1.4, 'paint.second', 'mass', sides=20, top=.1)


SPECS = {}
_FAM = {'a': ('intent_city_a', 'pale stone cladding with a punched window grid and dark corner piers (the stock brick and panel blocks)'),
        'b': ('intent_city_b', 'grey cladding with continuous green-tinted ribbon glazing on every floor'),
        'c': ('intent_city_c', 'white cladding with two dark curtain-wall strips per face and punched windows between')}
for _st in (2, 4, 6, 8):
    for _f in 'abc':
        _n = f'city{_st}story{_f}'
        SPECS[_n] = dict(fn=city_block, palette=_FAM[_f][0], cls='small' if _st <= 4 else 'large', counts=dict(),
                         intent=(f'City block, {_st} storeys on the stock 22 m lot: {_FAM[_f][1]}; a dark plinth with storefront '
                                 'glazing, a glazed lobby with double doors, canopy, name panel and lamp where the stock has its '
                                 'entrance; a crown band, coping and tidy rooftop plant.'))
