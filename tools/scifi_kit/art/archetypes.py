"""Archetype pipeline: an asset is a small assets/<asset>.params.json (archetype + measured dims + counts + palette).

Each archetype builder (here and in arch_props.py) is parametric and already follows the ART-BAR:
construction order (slab -> frame -> secondary steel -> separate lapped skin panels -> trim -> fasteners),
removable skin + hit zones for the partial state, a damage kind for the destroyed state (volumetric rubble,
toppling, collapse), light sockets from feature tags, stable piece uids, and bounds taken from the stock mesh
(fit_stock). Pristine/worn/abandoned come from the shared wear table; bake/LODs are the shared pipeline.

params.json schema (all lengths in authoring metres, Z up, front = -Y):
  asset, archetype, size [w,d,h], origin [x,y,z], palette {main,second,trim,ferro,roof,accent,metal,corr,glass} (LINEAR),
  counts {doors,windows,openings_bays,panels_sections,crates,barrels,tanks,arches_ribs,legs_supports,antennas_dishes,
          stairs_ladders} (the stock count contract; the model is recounted against it),
  features [{kind,side,at,z,w,h}] optional explicit placements, dims {...archetype specific}, detail {...toggles},
  damage optional override, rubble optional spec override, budget optional, notes.

Pure Python (Blender imports this through drafts); heavy deps live in analyze.py / validate.py.
"""
import json
import math
import random
import sys
from pathlib import Path

import geo
from geo import rect_poly, subtract_rect

HERE = Path(__file__).resolve().parent
ASSETS = HERE / 'assets'
PAL_KEYS = ('main', 'second', 'trim', 'ferro', 'roof', 'accent', 'metal', 'corr', 'glass')
FIELD = dict(door='doors', window='windows', bay='openings_bays', panel='panels_sections', crate='crates', barrel='barrels',
             tank='tanks', rib='arches_ribs', support='legs_supports', antenna='antennas_dishes', stair='stairs_ladders')
KIND = {v: k for k, v in FIELD.items()}
SIDES = ('front', 'right', 'rear', 'left')


class _Kit:
    """Lazy handle on drafts.py (construction helpers), avoiding an import cycle."""
    def __getattr__(self, name):
        import drafts
        return getattr(drafts, name)


kit = _Kit()


def C(x):
    return 0.0 if geo.GAME_MESH else x


# ------------------------------------------------------------------ classification (inventory -> archetype)
RULES = (
    (r'dropship|^shuttle|^slayerp|^shilonep|extractionmarker|^nav1$', 'manual'),
    (r'^crash|^lifeboat|^junk|^mechpiece', 'junk'),
    (r'(light|medium|heavy)post(diag)?$', 'wall'),
    (r'gate$|milcheckpoint', 'gate'),
    (r'(gate|turret|sensor|spotlight)control$', 'tower'),
    (r'^hqtent$|generator', 'box'),
    (r'sandbag|fence|barricade|shanty', 'fence'),
    (r'wall(diag)?$', 'wall'),
    (r'^(tent|longtent|htent|camonetting)$', 'tent'),
    (r'turret|popup|camo\d$|^spotlight$|guntower', 'turret'),
    (r'truck|armoredcar|jeep|forklift|buggy|troop|ambulance', 'vehicle'),
    (r'barrel', 'barrel'),
    (r'munitions', 'container'),
    (r'crate', 'crate'),
    (r'^lockers$|dumpster', 'container'),
    (r'fueltank|liquidstorage|pipetank|watertower', 'tank'),
    (r'pipe$|^pipe|fuelpipe|pipe\d$', 'pipe'),
    (r'streetlight|runwaylight|beacon|sign$|flag$|banner|perimeteralarm', 'pylon'),
    (r'tower|gantry|trafcon|crane|relay|hpgstation|valvestation', 'tower'),
    (r'quonset', 'quonset'),
    (r'^bridge', 'bridge'),
)
CATEGORY_DEFAULT = dict(building='box', prop='container', turret='turret', wall='wall', gate='gate', fence='fence', bridge='bridge')
ARCHETYPES = ('box', 'quonset', 'tower', 'wall', 'gate', 'fence', 'tent', 'crate', 'barrel', 'tank', 'turret', 'vehicle',
              'container', 'pylon', 'junk', 'bridge', 'pipe')


def classify(row, analysis=None):
    import re
    ident = row['id'].lower()
    for pat, arch in RULES:
        if re.search(pat, ident):
            why = f'name /{pat}/'
            break
    else:
        arch = CATEGORY_DEFAULT.get(row['category'].split(';')[0], 'box')
        why = f'category {row["category"]}'
    if arch == 'tent' and 'masonry' in row.get('identity', ''):
        arch, why = 'box', 'tent name but masonry facade'
    return dict(archetype=arch, why=why, supported=arch in ARCHETYPES)


# ------------------------------------------------------------------ feature placement
def face_frame(side, w, d):
    """(origin, yaw, length) of a wall face in building coordinates; outward is local -Y."""
    return {'front': ((0, -d / 2, 0), 0.0, w), 'right': ((w / 2, 0, 0), math.pi / 2, d),
            'rear': ((0, d / 2, 0), math.pi, w), 'left': ((-w / 2, 0, 0), -math.pi / 2, d)}[side]


def face_world(side, w, d, u, out, z):
    (ox, oy, _), yaw, _ = face_frame(side, w, d)
    c, s = math.cos(yaw), math.sin(yaw)
    return (ox + u * c + out * s, oy + u * s - out * c, z)


def face_dir(side):
    yaw = {'front': 0.0, 'right': math.pi / 2, 'rear': math.pi, 'left': -math.pi / 2}[side]
    return (math.sin(yaw), -math.cos(yaw), 0.0)


def plan_features(p, w, d, eave, kinds=('door', 'bay', 'window')):
    """Explicit placements first, then default placements until the count contract is met."""
    counts = p.get('counts', {})
    long_sides = ('right', 'left') if d >= w else ('front', 'rear')
    short_sides = ('front', 'rear') if d >= w else ('right', 'left')
    out = {}
    for kind in kinds:
        n = int(counts.get(FIELD[kind], 0))
        have = [dict(f) for f in p.get('features', []) if f['kind'] == kind][:n]
        need = n - len(have)
        i = 0
        while need > 0:
            if kind == 'window':
                side = long_sides[i % 2]
                per = math.ceil(n / 2)
                j = i // 2
                length = d if side in ('right', 'left') else w
                u = -length / 2 + length * (j + .5) / per
                have.append(dict(kind=kind, side=side, at=round(u, 2), z=max(1.1, eave * .42), w=1.2, h=min(1.1, eave * .25)))
            else:
                side = (short_sides + long_sides)[i % 4]
                spread = i // 4
                length = d if side in ('right', 'left') else w
                u = (spread - (0 if spread % 2 == 0 else 0)) * 3.0 * (1 if spread % 2 else -1) if spread else 0.0
                bay = kind == 'bay'
                have.append(dict(kind=kind, side=side, at=round(max(-length / 2 + 2, min(length / 2 - 2, u)), 2), z=.45 if not bay else 0.0,
                                 w=min(4.2, length * .4) if bay else 1.4, h=min(4.0, eave - .9) if bay else min(2.4, eave - .8)))
            i += 1
            need -= 1
        for f in have:
            length = d if f['side'] in ('right', 'left') else w
            f['w'] = min(f['w'], length - 1.6)
            f['at'] = max(-length / 2 + f['w'] / 2 + .8, min(length / 2 - f['w'] / 2 - .8, f['at']))
            f['h'] = max(.5, min(f['h'], eave - f['z'] - .35))
        out[kind] = have
    return out


def add_socket(k, label, kind, pos, direction, intensity):
    k.meta.setdefault('sockets', []).append(dict(name='LIGHT_' + label, position_m=tuple(pos), direction=tuple(direction), type=kind,
                                                 intensity_class=intensity))


# ------------------------------------------------------------------ measured batter (stock walls lean in)
def _interp(knots, t):
    t = min(max(t, 0.0), 1.0)
    for (t0, v0), (t1, v1) in zip(knots, knots[1:]):
        if t <= t1:
            return v0 + (v1 - v0) * (t - t0) / max(t1 - t0, 1e-9)
    return knots[-1][1]


def apply_batter(k, h, px, py, first=0):
    """Scale plan extents by the measured height profile (knots (t, factor), 1.0 at the base)."""
    fx = lambda z: _interp(px, z / h)  # noqa: E731
    fy = lambda z: _interp(py, z / h)  # noqa: E731
    for pc in k.pieces[first:]:
        pc['v'] = [(x * fx(z), y * fy(z), z) for x, y, z in pc['v']]
    k.hits = [dict(hh, p=(hh['p'][0] * fx(hh['p'][2]), hh['p'][1] * fy(hh['p'][2]), hh['p'][2])) for hh in k.hits]
    for sk in k.meta.get('sockets', []):
        x, y, z = sk['position_m']
        sk['position_m'] = (x * fx(z), y * fy(z), z)


# ------------------------------------------------------------------ box building
def box_building(k, p):
    """Podium -> columns and roof framing -> girts/purlins -> lapped skin panels -> openings -> roof -> trim -> fasteners."""
    from assemble import infantry_door_parts, rollup
    w, d, h = p['size']
    g = p.get('dims', {})
    det = p.get('detail', {})
    roof = g.get('roof', 'flat')
    rise = g.get('rise', 0.0) if roof != 'flat' else 0.0
    eave = g.get('eave_h') or (h - rise)
    corr = g.get('skin', 'plate') == 'corr'
    skin_mat = 'corr.corr' if corr else 'paint.main'
    plinth = .45
    t = .24
    bay = g.get('bay_m', 4.5)
    bx = int(g.get('bays_x') or max(1, round(w / bay)))
    by = int(g.get('bays_y') or max(1, round(d / bay)))
    feats = plan_features(p, w, d, eave)
    # 1. podium / footing
    k.box((0, 0, plinth / 2), (w + .3, d + .3, plinth), 'ferro', 'mass', ch=C(.08), top=C(.05), part='footing')
    # 2. primary frame: perimeter columns on the bay lines + rafters across the short span
    xs = [-w / 2 + t / 2 + (w - t) * i / bx for i in range(bx + 1)]
    ys = [-d / 2 + t / 2 + (d - t) * j / by for j in range(by + 1)]
    for x in xs:
        for y in (ys[0], ys[-1]):
            kit.ibeam(k, (x, y, plinth), (x, y, eave), d=.26, w=.16)
    for y in ys[1:-1]:
        for x in (xs[0], xs[-1]):
            kit.ibeam(k, (x, y, plinth), (x, y, eave), d=.26, w=.16)
    ridge_x = g.get('ridge_axis', 'y' if d >= w else 'x') == 'x'
    span, run = (d, w) if ridge_x else (w, d)
    nb = bx if ridge_x else by
    rl = [-run / 2 + t / 2 + (run - t) * j / nb for j in range(nb + 1)]
    with k.at((0, 0, 0), yaw=math.pi / 2 if ridge_x else 0.0):       # local frame: x across the ridge, y along it
        for q in rl:
            a0, b0 = (-span / 2 + t / 2, q, eave), (span / 2 - t / 2, q, eave)
            if roof == 'flat':
                kit.ibeam(k, a0, b0, d=.30, w=.16)
            elif roof == 'mono':
                kit.ibeam(k, a0, (b0[0], b0[1], eave + rise), d=.30, w=.16)
            else:
                m = (0.0, q, eave + rise)
                kit.ibeam(k, a0, m, d=.28, w=.15)
                kit.ibeam(k, m, b0, d=.28, w=.15)
    # 3. secondary steel: wall girts behind the skin, roof purlins
    girt_z = [plinth + (eave - plinth) * f for f in ((1 / 3, 2 / 3) if eave > 4 else (.5,))]
    for z in girt_z:
        cs = [(-w / 2 + t, -d / 2 + t, z), (w / 2 - t, -d / 2 + t, z), (w / 2 - t, d / 2 - t, z), (-w / 2 + t, d / 2 - t, z)]
        for a, b in zip(cs, cs[1:] + cs[:1]):
            kit.cee(k, a, b, d=.18, w=.06)
    # 4. wall skin per face, hung on the frame, cut around the planned openings
    skin_top = eave
    rows = max(1, round((skin_top - plinth) / 3.0))
    rh = (skin_top - plinth) / rows
    face_len = {'front': w, 'rear': w, 'right': d, 'left': d}
    insets = g.get('inset', False)
    for side in SIDES:
        o, yaw, L = face_frame(side, w, d)
        lines = [-L / 2 + t / 2 + (L - t) * i / (bx if L == w else by) for i in range((bx if L == w else by) + 1)]
        holes = []
        for kind in ('door', 'bay', 'window'):
            for f in feats[kind]:
                if f['side'] == side:
                    holes.append((f['at'] - f['w'] / 2 - .05, f['z'] - .05 if kind != 'window' else f['z'] - .05, f['at'] + f['w'] / 2 + .05, f['z'] + f['h'] + .05))
        with k.at(o, yaw=yaw):
            for i in range(len(lines) - 1):
                for r in range(rows):
                    a = lines[i] - (.03 if i else 0) - (.03 if i % 2 == 0 and i else 0)
                    b = lines[i + 1] + (.03 if i < len(lines) - 2 else 0) + (.06 if i % 2 == 0 and i < len(lines) - 2 else 0)
                    z0, z1 = plinth + r * rh - (.0 if r == 0 else .03), plinth + (r + 1) * rh
                    proud = .014 * ((i + r) % 2)
                    for q in subtract_rect((a, z0, b, z1), holes):
                        if corr:
                            k.corr_sheet(q[0], q[2], q[1], q[3], -.02 - proud, skin_mat, removable=True)
                        else:
                            k.slab(q[0], q[2], q[1], q[3], -.02 - proud, .03, skin_mat, 'skin', front=C(.012), removable=True)
                            if insets and (i + r) % 2 == 0 and q[2] - q[0] > .9 and q[3] - q[1] > .9:
                                k.slab(q[0] + .22, q[2] - .22, q[1] + .22, q[3] - .22, -.035 - proud, -.01, 'paint.second', 'skin', front=C(.015), rides=True)
            # gable end triangle sits on the faces perpendicular to the ridge
            if roof == 'gable' and ((side in ('front', 'rear')) != ridge_x):
                k.prism([(-L / 2, eave), (L / 2, eave), (0, eave + rise)], -.02, .03, skin_mat, 'skin', axis='y', removable=True)
            # openings: frame, leaf, glazing, lamp
            for kind in ('door', 'bay', 'window'):
                for idx, f in enumerate(feats[kind]):
                    if f['side'] != side:
                        continue
                    with k.tagged(f'{kind}:{idx}'):
                        with k.at((f['at'], 0, 0)):
                            if kind == 'door':
                                infantry_door_parts(k, f['w'], f['h'])
                            elif kind == 'bay':
                                rollup(k, f['w'], f['h'])
                            else:
                                k.window(-f['w'] / 2, f['w'] / 2, f['z'], f['z'] + f['h'], 0.0, mullions=1 if f['w'] > 1.0 else 0)
            # corner trim + fastener rows along the girts
            for z in girt_z:
                kit.bolts(k, (-L / 2 + .3, -.06, z), (L / 2 - .3, -.06, z), pitch=.6)
            k.moss_sites.append(((-L / 2 + .3, -.2, plinth), (L / 2 - .3, -.2, plinth)))
            if det.get('service_panel', True) and side == 'rear':
                k.service_panel(-L / 4, 1.7, w=.8, h=.55)
    for sx in (-1, 1):
        for sy in (-1, 1):
            k.box((sx * (w / 2 - .04), sy * (d / 2 - .04), (plinth + eave) / 2), (.34, .34, eave - plinth), 'paint.trim', 'frame', ch=C(.03))
    # 5. roof
    lap = .06
    if roof == 'flat':
        nx, ny = max(1, bx), max(1, by)
        sx_, sy_ = (w - .5) / nx, (d - .5) / ny
        for i in range(nx):
            for j in range(ny):
                k.box((-w / 2 + .25 + (i + .5) * sx_, -d / 2 + .25 + (j + .5) * sy_, eave + .06), (sx_ - .01, sy_ - .01, .14), 'gravel', 'roof', removable=True, slab=(i, j))
        for (cx, cy, sx2, sy2) in ((0, -d / 2 + .1, w + .3, .24), (0, d / 2 - .1, w + .3, .24), (-w / 2 + .1, 0, .24, d), (w / 2 - .1, 0, .24, d)):
            k.box((cx, cy, eave + .3), (sx2, sy2, .30), 'paint.trim', 'frame', ch=C(.03), top=C(.03))
        k.moss_sites.append(((-w / 2 + .4, -d / 2 + .4, eave + .13), (w / 2 - .4, -d / 2 + .4, eave + .13)))
        for q in range(int(det.get('vents', 1))):
            with k.at((w * (.18 - .3 * q), d * .12, eave + .13)):
                k.hvac(1.8, 1.4, .9)
    else:
        ov = .35
        roofmat = 'corr.corr' if (corr or g.get('roof_mat') == 'corr') else 'paint.roof'
        with k.at((0, 0, 0), yaw=math.pi / 2 if ridge_x else 0.0):
            for sgn in ((-1, 1) if roof == 'gable' else (1,)):
                for j in range(nb):
                    a0 = -run / 2 + run * j / nb - (lap if j else 0)
                    a1 = -run / 2 + run * (j + 1) / nb
                    if roof == 'gable':
                        e = (sgn * (span / 2 + ov), eave - ov * rise / (span / 2))
                        rd = (0.0, eave + rise)
                    else:
                        e = (-span / 2 - ov, eave - ov * rise / span)
                        rd = (span / 2 + ov, eave + rise + ov * rise / span)
                    dx, dz = rd[0] - e[0], rd[1] - e[1]
                    ln = math.hypot(dx, dz) or 1
                    nrm = (-dz / ln, dx / ln)
                    if nrm[1] < 0:
                        nrm = (-nrm[0], -nrm[1])
                    tk = .035 + .014 * (j % 2)
                    fr = [(e[0], a0, e[1]), (rd[0], a0, rd[1]), (rd[0], a1, rd[1]), (e[0], a1, e[1])]
                    if (sgn > 0) == (roof == 'gable'):
                        fr = fr[::-1]
                    bk = [(x + nrm[0] * tk, y, z + nrm[1] * tk) for x, y, z in fr]
                    kit.hexa(k, fr, bk, roofmat, 'roof', removable=True, sheet=(sgn, j))
            if roof == 'gable':
                k.box((0, 0, eave + rise + .05), (.45, run + .2, .12), 'paint.trim', 'frame')
        for q in range(int(det.get('vents', 1))):
            k.box((w * (.12 - .24 * q) * (0 if ridge_x else 1), d * (.15 - .3 * q) * (1 if not ridge_x else 0), eave + rise * .55 + .3), (.5, .5, .3), 'paint.trim', 'detail', ch=.04)
    # 6. sockets from the feature tags (door spot, window point)
    for idx, f in enumerate(feats['door'] + feats['bay']):
        pos = face_world(f['side'], w, d, f['at'], .5, f['z'] + f['h'] + .35)
        dr = face_dir(f['side'])
        add_socket(k, f'door_{idx + 1:02}', 'spot', pos, (dr[0], dr[1], -.6), 'door')
    for idx, f in enumerate(feats['window']):
        pos = face_world(f['side'], w, d, f['at'], .08, f['z'] + f['h'] / 2)
        add_socket(k, f'window_{idx + 1:02}', 'point', pos, face_dir(f['side']), 'window')
    # 7. hit zones for the partial state: one wall zone on the long face, one roof corner
    long_side = 'right' if d >= w else 'front'
    hp = face_world(long_side, w, d, 0.0, 0.0, eave * .55)
    k.hits.append(dict(p=hp, d=(-face_dir(long_side)[0], -face_dir(long_side)[1], 0), r=max(2.0, min(w, d) * .22), depth=0, sag=.35))
    k.hits.append(dict(p=(w * .2, d * .15, eave), d=(0, 0, -1), r=max(1.6, min(w, d) * .16), depth=0, sag=.3))
    if g.get('batter'):
        apply_batter(k, h, g['batter']['x'], g['batter']['y'])


# ------------------------------------------------------------------ quonset (legacy builder, parametrised through Q)
def quonset_arch(k, p):
    g = p.get('dims', {})
    over = {kk: g[kk] for kk in ('L', 'R', 'curb', 'bays') if kk in g}
    with _override(kit.Q, **over):
        kit.quonset_build(k)


class _override:
    """Temporarily change a module-level parameter dict (the legacy builders read them as globals)."""
    def __init__(self, target, **vals):
        self.t, self.v = target, vals

    def __enter__(self):
        self.old = {kk: self.t.get(kk) for kk in self.v}
        self.t.update(self.v)

    def __exit__(self, *a):
        self.t.update(self.old)


# ------------------------------------------------------------------ walls / posts / gates (batch3.wall with params)
def wall_arch(k, p):
    import batch3
    batch3.wall(k, p)


def gate_arch(k, p):
    import batch3
    batch3.gate(k, p)


def fence_arch(k, p):
    import arch_props
    arch_props.fence(k, p)


# ------------------------------------------------------------------ damage kinds and registry
RUBBLE_DEFAULT = dict(
    box=dict(core='gravel', heap=.28, frame='ibeam', panel='lap', remnant='corner', slab='ferro', leaners=9, panels=10, chunks=22),
    quonset=dict(core='gravel', heap=.26, frame='arc', panel='arc', remnant='endarch', slab='ferro', leaners=4),
    tower=dict(core='ferro', heap=.2, frame='column', panel='plate', remnant='stub', slab='paint.main', leaners=5, foot=.98),
    tank=dict(core='ferro', heap=.22, frame='ibeam', panel='plate', remnant='stub', slab='ferro', leaners=4, foot=.9),
    turret=dict(core='ferro', heap=.3, frame='ibeam', panel='lap', remnant='stub', slab='ferro', leaners=3, foot=.95),
    wall_post=dict(core='ferro', heap=.25, frame='column', panel='plate', remnant='stub', slab='ferro', leaners=3, foot=.95),
)
DAMAGE_OF = dict(box='rubble', quonset='rubble', tower='rubble', tank='rubble', turret='rubble', wall='wall', gate='rubble', fence='fence',
                 tent='collapse', crate='crate', barrel='barrel', container='collapse', vehicle='collapse', pylon='topple', junk='collapse',
                 bridge='collapse', pipe='collapse')
BUILDERS = dict(box=box_building, quonset=quonset_arch, wall=wall_arch, gate=gate_arch, fence=fence_arch)


def _builder(arch):
    import arch_props
    return BUILDERS.get(arch) or getattr(arch_props, 'BUILD')[arch]


def load_params(name):
    path = ASSETS / f'{name}.params.json'
    return json.loads(path.read_text())


def contract(p):
    import counts
    c = p.get('counts', {})
    return counts.known(**{f: [f'{KIND[f]} {i + 1}/{c[f]}' for i in range(int(c[f]))] for f in FIELD.values() if int(c.get(f, 0))})


def budget_of(p):
    if 'budget' in p:
        return int(p['budget'])
    w, d, h = p['size']
    return 8000 if (p['archetype'] in ('box', 'quonset', 'tower', 'tank') and w * d <= 450 and h <= 14) else 15000 if p['archetype'] in ('box', 'tower', 'tank') and w * d > 450 else 3000


def register(p):
    """Install one asset into the drafts/counts/geo/bake registries (idempotent)."""
    import bake_budget
    import counts
    import drafts
    name, arch = p['asset'], p['archetype']
    size = tuple(float(x) for x in p['size'])
    drafts.SIZES[name] = size
    if arch in ('wall', 'gate'):
        import batch3
        batch3.SIZES[name] = size
    pal = {key: [.3, .3, .3] for key in PAL_KEYS}
    pal.update(p['palette'])
    pal['source'] = f'auto-draft from stock {name}'
    geo.LANG['palettes'][name] = pal
    build_fn = _builder(arch)
    drafts.DRAFTS[name] = ((lambda k, _p=p, _f=build_fn: _f(k, _p)), name)
    dm = p.get('damage') or DAMAGE_OF[arch]
    if arch == 'wall' and p.get('dims', {}).get('post'):
        dm = 'rubble'
    drafts.ARCH_OF[name] = arch
    if dm == 'rubble':
        drafts.BUILDINGS.add(name)
        spec = dict(RUBBLE_DEFAULT['wall_post' if arch in ('wall', 'gate') else arch])
        spec.update(p.get('rubble', {}))
        spec.setdefault('scale', max(.12, min(1.0, min(size[0], size[1], size[2] * 1.2) / 10)))
        drafts.RUBBLE[name] = spec
    else:
        import arch_props
        fn = arch_props.DAMAGE.get(dm) or getattr(drafts, 'DAMAGE_' + dm, None)
        if dm == 'wall':
            import batch3
            fn = batch3.wall_damage
        elif dm == 'fence':
            fn = arch_props.fence_damage
        elif dm == 'topple':
            fn = drafts.fence_damage
        if fn:
            drafts.DAMAGE[name] = fn
    drafts.WALL_PROFILE[name] = tuple(tuple(x) for x in p.get('dims', {}).get('profile', ((0, 1.0), (1, 1.0))))
    counts.CONTRACTS[name] = contract(p)
    counts.NOTES[name] = p.get('notes', 'auto-drafted from stock analysis')
    counts.ORIGINS[name] = tuple(p.get('origin', (0, 0, 0)))
    bake_budget.BUDGETS[name] = budget_of(p)


def asset_names():
    return sorted(q.name.split('.params.json')[0] for q in ASSETS.glob('*.params.json'))


def register_all():
    for name in asset_names():
        register(load_params(name))


# ------------------------------------------------------------------ colour of a material slot (mirrors blend.material)
def slot_colour(mat, pal):
    """Linear RGB the shader would start from for kit material `mat` (before wear); None for emissive/vegetation."""
    cls, _, slot = mat.partition('.')
    g = pal.get
    if cls == 'paint':
        return g(slot or 'main', g('main'))
    if cls == 'corr':
        return g('corr') if slot in ('', 'corr') else g(slot, g('corr'))
    if cls == 'hazard':
        return g('accent')
    if cls in ('wood', 'canvas'):
        return g(slot, g('main'))
    if cls == 'metal':
        return g('metal')
    if cls == 'grate':
        return [c * .6 for c in g('metal')]
    if cls == 'ferro':
        return g(slot, g('ferro'))
    if cls == 'glass':
        return g('glass')
    if cls == 'gravel':
        return g('roof')
    if cls == 'rubber':
        return (.02, .021, .022)
    if cls == 'char':
        return (.018, .016, .014)
    if cls == 'lamp':
        return (0.6, 0.55, 0.4)
    if cls == 'lamp_red':
        return (0.5, 0.03, 0.02)
    return None


# ------------------------------------------------------------------ auto-draft params from an analysis
def _feats(a, field, kind):
    out = []
    for f in a['counts'][field]['where']:
        if 'side' not in f:
            continue
        wd, ht = f['size_m']
        out.append(dict(kind=kind, side=f['side'], at=f['at_m'][0], z=round(max(0.0, f['at_m'][1] - ht / 2), 2), w=wd, h=ht))
    return out


def _batter_of(a):
    """Normalised (t, factor) knots when the stock footprint leans in by more than 8% at the top."""
    px, py = a['profile']['x_all'], a['profile']['y_all']
    top = lambda kn: _interp([(t, v) for t, v in kn], .95) / max(kn[0][1], 1e-6)  # noqa: E731
    if min(top(px), top(py)) > .92:
        return None
    norm = lambda kn: [[t, round(v / kn[0][1], 3)] for t, v in kn]  # noqa: E731
    return dict(x=norm(px), y=norm(py))


def re_any(s, pats):
    return any(q in s for q in pats)


def draft_params(row, a, arch=None):
    """Everything derivable from the stock measurement; the agent only confirms or corrects the result."""
    name = a['asset']
    arch = arch or a['archetype_guess']['archetype']
    w, d, h = a['bounds']['size_m']
    ident = row.get('identity', '').lower()
    nm = name.lower()
    c = a['counts']
    pal = (a.get('palette') or {}).get('draft') or {k: [.3, .3, .3] for k in PAL_KEYS}
    p = dict(asset=name, archetype=arch, size=[w, d, h], origin=a['bounds']['origin_offset_m'], palette=pal, counts={}, features=[], dims={}, detail={},
             notes=f'auto-drafted from stock analysis ({a["archetype_guess"]["why"]}); counts/features are DRAFT (see analysis.json confidences)')
    roof = a.get('roof', {})
    g = p['dims']
    n_ = lambda f: int(c[f]['n'])  # noqa: E731
    if arch == 'box':
        kind = roof.get('type', 'flat') if roof.get('confidence', 0) >= .6 else 'flat'
        bat = _batter_of(a)
        if bat:
            g['batter'] = bat
            kind = 'flat' if kind == 'arch' else kind
        kind = kind if kind in ('gable', 'mono') else 'flat'
        g.update(roof=kind, skin='corr' if 'corrugated' in ident else 'plate', inset='inset' in ident, bay_m=4.5 if max(w, d) < 40 else 6.0)
        if kind != 'flat':
            g.update(eave_h=round(roof['eave_h'], 2), rise=round(max(.3, roof['ridge_h'] - roof['eave_h']), 2), ridge_axis=roof.get('ridge_axis', 'y'))
        p['features'] = _feats(a, 'doors', 'door') + _feats(a, 'openings_bays', 'bay') + _feats(a, 'windows', 'window')
        p['detail'] = dict(vents=1 + (w * d > 300), service_panel=True)
        p['counts'].update(doors=len([f for f in p['features'] if f['kind'] == 'door']), openings_bays=len([f for f in p['features'] if f['kind'] == 'bay']),
                           windows=len([f for f in p['features'] if f['kind'] == 'window']))
    elif arch == 'quonset':
        g.update(L=round(d, 2), R=round(w / 2 - .4, 2), curb=.45, bays=max(3, round(d / 3.0)))
    elif arch in ('wall', 'gate'):
        g.update(profile=[[t, v] for t, v in a['profile']['x_body']], family=nm[0] if nm[0] in 'sld' and nm[1] == '_' else 's',
                 variant='heavy' if 'heavy' in nm else 'light' if 'light' in nm else 'medium', post=bool(arch == 'wall' and 'post' in nm))
        if arch == 'gate':
            door = next((q for q in a['parts'] if 'door' in q['name'].lower()), None)
            if door:
                g.update(gap=round(door['bbox_hi'][1] - door['bbox_lo'][1] + .5, 2), leaf_h=round(door['bbox_hi'][2], 2), leaf_t=round(door['bbox_hi'][0] - door['bbox_lo'][0], 2))
        else:
            p['counts'].update(legs_supports=1)
            if not g['post']:
                spans = 4 if g['family'] == 's' else 3 if g['family'] == 'l' else 6
                p['counts'].update(panels_sections=spans)
        if arch == 'gate':
            p['counts'].update(panels_sections=2)
    elif arch == 'fence':
        style = 'razor' if 'razor' in nm else 'wood' if 'wood' in nm else 'sandbag' if 'sandbag' in nm else 'sheet' if 'shanty' in nm else 'chain'
        g.update(style=style)
        posts = max(2, n_('legs_supports') or round(max(w, d) / 3.6) + 1)
        p['counts'].update(legs_supports=posts, panels_sections=posts - 1)
    elif arch == 'tent':
        g.update(eave_h=round(roof['eave_h'], 2) if roof.get('type') == 'gable' else round(h * .38, 2), bays=max(1, round(max(w, d) / 4.5)))
        p['counts']['doors'] = max(1, min(2, n_('doors') + n_('windows')))
    elif arch == 'container':
        g.update(lid=round(roof.get('pitch_deg', 0.0)) if roof.get('type') == 'mono' else 0, ribs=max(3, round(max(w, d) / .5)))
        p['counts'].update(crates=max(1, n_('crates')) if max(w, d) > 2.2 * min(w, d) else 1, doors=0)
    elif arch == 'crate':
        p['counts']['crates'] = max(1, n_('crates')) if n_('crates') else 2
        g.update(tarp='tarp' in nm or 'canvas' in ident)
    elif arch == 'barrel':
        p['counts']['barrels'] = max(1, n_('barrels'))
        g.update(flame='burning' in nm)
    elif arch == 'tank':
        p['counts']['tanks'] = max(1, n_('tanks') or (1 if min(w, d) > .6 * max(w, d) else 2))
        g.update(legs=4 if 'watertower' in nm else 0, band='yellow' in ident or 'band' in ident)
    elif arch == 'turret':
        g.update(popup=h > 1.4 * max(w, d), barrels=2 if re_any(nm, ('twin', 'auto')) else 1)
        basep = next((q for q in a['parts'] if q.get('role') == 'base' and q['tris']), None)
        headp = [q for q in a['parts'] if q.get('role') == 'head' and q['tris']]
        if basep:      # the stock's own base and head parts, not a generic base
            g.update(base_h=round(basep['bbox_hi'][2], 2), base_r=round(max(basep['bbox_hi'][0] - basep['bbox_lo'][0], basep['bbox_hi'][1] - basep['bbox_lo'][1]) / 2, 2))
        if headp:
            lo = [min(q['bbox_lo'][i] for q in headp) for i in range(3)]
            hi = [max(q['bbox_hi'][i] for q in headp) for i in range(3)]
            g.update(house=[round(hi[0] - lo[0], 2), round(hi[1] - lo[1], 2), round(hi[2] - lo[2], 2)])
        p['counts']['legs_supports'] = 1
    elif arch == 'vehicle':
        g.update(cargo='tank' if 'fuel' in nm else 'none' if re_any(nm, ('armored', 'jeep', 'forklift', 'buggy')) else 'ambulance' if 'ambulance' in nm else 'box',
                 axles=3 if max(w, d) > 10 else 2)
    elif arch == 'pylon':
        kind = 'marker' if re_any(nm, ('runway', 'beacon')) else 'sign' if nm.endswith('sign') else 'flag' if re_any(nm, ('flag', 'banner')) else 'alarm' if 'alarm' in nm else 'lamp'
        g.update(kind=kind, head_h=0.0 if kind != 'lamp' else .5)
        p['counts']['legs_supports'] = 0 if kind == 'marker' else 1
    elif arch == 'tower':
        g.update(lattice='lattice' in ident, mast='dish' in ident or 'antenna' in ident)
        p['features'] = _feats(a, 'doors', 'door') + _feats(a, 'windows', 'window')
        p['counts'].update(doors=min(4, n_('doors')), windows=min(12, n_('windows')), antennas_dishes=1 if g['mast'] else 0)
    elif arch == 'junk':
        g.update(pieces=max(8, min(24, round(w * d / 6))))
    elif arch == 'bridge':
        g.update(spans=max(2, round(max(w, d) / 8)))
        p['counts']['legs_supports'] = g['spans'] - 1
    elif arch == 'pipe':
        layout = 'cross' if 'fourway' in nm else 'tee' if 'threeway' in nm else 'corner' if 'corner' in nm else 'end' if nm.endswith('end') else 'straight'
        g.update(layout=layout)
    p['stock_model'] = a.get('stock_model', {}).get('intact_stem', name)
    p['round_parts'] = a.get('round_parts', [])
    p['draft_confidence'] = {f: c[f]['conf'] for f in FIELD.values() if p['counts'].get(f) or c[f]['n']}
    return p
