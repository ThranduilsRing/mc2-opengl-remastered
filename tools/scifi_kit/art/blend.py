"""Blender stage for the art-directed kit: materials with mask-driven conditions,
physical destroyed operator, review renders and clean glTF export.

Run only through run.py (it pins --threads 2, Cycles threads 2, CPU affinity off core 0).
"""
import json
import math
import random
import sys
from pathlib import Path

import bmesh
import bpy
from mathutils import Matrix, Vector, noise as mnoise

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import geo  # noqa: E402

LANG = geo.LANG
TEX = Path('G:/mc2-assets/pbr/scifi-kit')


# ------------------------------------------------------------------ scene
def reset():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.engine = 'CYCLES'
    scene.render.threads_mode = 'FIXED'
    scene.render.threads = 2
    scene.cycles.use_denoising = True
    scene.view_settings.view_transform = 'AgX'
    try:
        scene.view_settings.look = 'AgX - Medium High Contrast'
    except TypeError:
        pass
    prefs = bpy.context.preferences.addons['cycles'].preferences
    for backend in ['HIP', 'OPTIX', 'CUDA']:
        try:
            prefs.compute_device_type = backend
            prefs.get_devices()
            gpu = [d for d in prefs.devices if d.type == backend]
            if gpu:
                for d in prefs.devices:
                    d.use = d.type == backend
                scene.cycles.device = 'GPU'
                print('[art] Cycles GPU', backend, [d.name for d in gpu], flush=True)
                break
        except (TypeError, RuntimeError):
            continue
    return scene


# ------------------------------------------------------------------ node helpers
class NB:
    def __init__(self, mat):
        self.nt = mat.node_tree
        self.nodes = self.nt.nodes
        self.nodes.clear()

    def node(self, kind, **props):
        n = self.nodes.new(kind)
        for k, v in props.items():
            setattr(n, k, v)
        return n

    def feed(self, sock, v):
        if isinstance(v, bpy.types.NodeSocket):
            self.nt.links.new(v, sock)
        elif isinstance(v, (tuple, list)):
            sock.default_value = tuple(v) + ((1.0,) if len(v) == 3 and len(sock.default_value) == 4 else ())
        else:
            sock.default_value = v

    def math(self, op, a, b=None, clamp=False):
        n = self.node('ShaderNodeMath', operation=op, use_clamp=clamp)
        self.feed(n.inputs[0], a)
        if b is not None:
            self.feed(n.inputs[1], b)
        return n.outputs[0]

    def mul(self, a, b, clamp=False):
        return self.math('MULTIPLY', a, b, clamp)

    def add(self, a, b, clamp=False):
        return self.math('ADD', a, b, clamp)

    def remap(self, v, a, b, c=0.0, d=1.0, smooth=False):
        n = self.node('ShaderNodeMapRange', clamp=True, interpolation_type='SMOOTHSTEP' if smooth else 'LINEAR')
        self.feed(n.inputs['Value'], v)
        n.inputs['From Min'].default_value = a
        n.inputs['From Max'].default_value = b
        n.inputs['To Min'].default_value = c
        n.inputs['To Max'].default_value = d
        return n.outputs['Result']

    def noise(self, vec, scale, detail=4.0, rough=.55, distortion=0.0):
        n = self.node('ShaderNodeTexNoise')
        self.feed(n.inputs['Vector'], vec)
        n.inputs['Scale'].default_value = scale
        n.inputs['Detail'].default_value = detail
        n.inputs['Roughness'].default_value = rough
        n.inputs['Distortion'].default_value = distortion
        return n.outputs['Fac']

    def voronoi_edge(self, vec, scale):
        n = self.node('ShaderNodeTexVoronoi', feature='DISTANCE_TO_EDGE')
        self.feed(n.inputs['Vector'], vec)
        n.inputs['Scale'].default_value = scale
        return n.outputs['Distance']

    def vscale(self, vec, s):
        n = self.node('ShaderNodeVectorMath', operation='MULTIPLY')
        self.feed(n.inputs[0], vec)
        n.inputs[1].default_value = s
        return n.outputs[0]

    def mix(self, fac, a, b, blend='MIX'):
        n = self.node('ShaderNodeMix', data_type='RGBA', blend_type=blend, clamp_factor=True)
        ins = [i for i in n.inputs if i.type == 'RGBA']
        self.feed(n.inputs[0], fac)
        self.feed(ins[0], a)
        self.feed(ins[1], b)
        return [o for o in n.outputs if o.type == 'RGBA'][0]

    def mixf(self, fac, a, b):
        n = self.node('ShaderNodeMix', data_type='FLOAT', clamp_factor=True)
        ins = [i for i in n.inputs if i.type == 'VALUE']
        self.feed(n.inputs[0], fac)
        self.feed(ins[1], a)
        self.feed(ins[2], b)
        return [o for o in n.outputs if o.type == 'VALUE'][0]


def lum(c):
    return .2126 * c[0] + .7152 * c[1] + .0722 * c[2]


def image(path, noncolor=False):
    im = bpy.data.images.load(str(path), check_existing=True)
    if noncolor:
        im.colorspace_settings.name = 'Non-Color'
    return im


MAT_CACHE = {}


def material(key, palette_name, cond_name):
    ck = (key, palette_name, cond_name)
    if ck in MAT_CACHE:
        return MAT_CACHE[ck]
    pal = LANG['palettes'][palette_name]
    cond = LANG['conditions'][cond_name]
    cls, _, slot = key.partition('.')
    mat = bpy.data.materials.new(f'{key}|{palette_name}|{cond_name}')
    mat.use_nodes = True
    mat['mc2_material_class'] = cls
    b = NB(mat)
    out = b.node('ShaderNodeOutputMaterial')
    bs = b.node('ShaderNodeBsdfPrincipled')
    b.nt.links.new(bs.outputs[0], out.inputs['Surface'])
    tc = b.node('ShaderNodeTexCoord')
    P = tc.outputs['Object']
    geom = b.node('ShaderNodeNewGeometry')
    N = geom.outputs['Normal']
    sep = b.node('ShaderNodeSeparateXYZ')
    b.feed(sep.inputs[0], P)
    z = sep.outputs['Z']
    sepn = b.node('ShaderNodeSeparateXYZ')
    b.feed(sepn.inputs[0], N)
    nz = sepn.outputs['Z']
    ao = b.node('ShaderNodeAmbientOcclusion', only_local=True)
    ao.inputs['Distance'].default_value = .5
    AO = ao.outputs['AO']
    bev = b.node('ShaderNodeBevel')
    bev.inputs['Radius'].default_value = .04
    dot = b.node('ShaderNodeVectorMath', operation='DOT_PRODUCT')
    b.feed(dot.inputs[0], bev.outputs['Normal'])
    b.feed(dot.inputs[1], N)
    edge_raw = b.math('SUBTRACT', 1.0, dot.outputs['Value'])
    convex = b.mul(b.mul(edge_raw, 18.0, True), b.remap(AO, .55, .95))
    cavity = b.remap(AO, .9, .35)
    grunge = b.noise(P, 2.4, 8, .66)
    big = b.noise(P, .38, 3, .5)
    streak_n = b.noise(b.vscale(P, (5.5, 5.5, .16)), 1.0, 5, .6)
    vertical = b.remap(b.math('ABSOLUTE', nz), .7, .2)
    streak = b.mul(b.remap(streak_n, .48, .72), vertical)
    height_dirt = b.mul(b.remap(z, 1.6, 0.0), b.remap(b.noise(P, 1.3, 4), .3, .65, .45, 1.0))
    dmg_attr = b.node('ShaderNodeAttribute', attribute_name='dmg').outputs['Fac']
    # Streaks originate at panel tops (fasteners, laps, sills) and fade over ~1.4 m.
    seam_attr = b.node('ShaderNodeAttribute', attribute_name='seamd').outputs['Fac']
    streak = b.mul(streak, b.remap(seam_attr, 0.0, 1.4, 1.0, 0.18))

    base = pal.get(slot) if slot in pal else None
    metallic, rough = LANG['material_classes'].get(cls, {'metallic': 0, 'roughness': .7})['metallic'], LANG['material_classes'].get(cls, {'roughness': .7})['roughness']
    emission = None
    weather = True
    bump_h = None
    rough_s = rough

    if cls in ('paint', 'corr', 'hazard'):
        if cls == 'hazard':
            add3 = b.node('ShaderNodeVectorMath', operation='DOT_PRODUCT')
            b.feed(add3.inputs[0], tc.outputs['UV'] if slot=='wrap' else P)
            add3.inputs[1].default_value = (5.28, .44, 0) if slot=='wrap' else (1, .6, 1)
            ping = b.math('PINGPONG', add3.outputs['Value'], .22)
            stripe = b.math('GREATER_THAN', ping, .11)
            col = b.mix(stripe, (0.025, 0.025, 0.022), tuple(pal['accent']))
            c0 = pal['accent']
        else:
            c0 = (pal['corr'] if slot in ('', 'corr') else pal.get(slot, pal['corr'])) if cls == 'corr' else pal[slot or 'main']
            col = tuple(c0)
            if cls == 'corr' and lum(c0) < .12 and c0[0] > c0[2] * 2.5:
                # rust-coated corrugated (stock quonset): galvanised steel shows through
                galv = b.mul(b.remap(b.noise(b.vscale(P, (1.0, .25, 1.0)), 1.6, 5, .6), .56, .66), .5)
                col = b.mix(galv, col, (0.22, 0.21, 0.19))
                col = b.mix(b.mul(b.remap(grunge, .35, .75), .5), col, (0.24, 0.10, 0.035))
        # paint fade toward chalky desaturated
        lv = lum(c0)
        faded = tuple(min(1, lv * 1.35 + .045 + (c - lv) * .35) for c in c0)
        col = b.mix(b.mul(cond['fade'], b.remap(big, .3, .7, .55, 1.0)), col, faded)
        # peel: patches of paint gone to primer / rust
        peel_n = b.add(b.mul(b.noise(P, 1.7, 6, .6), .75), b.mul(convex, .5))
        peel = b.math('GREATER_THAN', peel_n, 1.0 - cond['peel'] * .62)
        under = b.mix(b.remap(grunge, .4, .6), (0.15, 0.16, 0.13), (0.12, 0.05, 0.02))
        col = b.mix(peel, col, under)
        bump_h = b.mul(peel, -1.0)
        # edge wear to bare steel
        wear = b.math('GREATER_THAN', b.mul(convex, b.remap(grunge, .3, .7, .5, 1.6)), 1.0 - cond['edge_wear'] * .8)
        col = b.mix(wear, col, (0.62, 0.60, 0.56))
        metallic = b.mixf(wear, metallic, .55)
        rough_s = b.mixf(wear, b.add(rough, b.mul(b.remap(grunge, .3, .7), .25)), .32)
        # rust bleeding from edges and under ledges
        rust = b.mul(cond['rust'], b.add(b.mul(streak, .7), b.mul(wear, .8)), True)
        rust = b.mul(rust, b.remap(b.noise(P, 3.2, 6), .35, .6))
        col = b.mix(rust, col, (0.13, 0.05, 0.018))
    elif cls in ('wood', 'canvas'):
        c0 = pal.get(slot, pal['main'])
        stretch = (0.25, 12.0, 10.0) if cls == 'wood' else (25.0, 25.0, 25.0)
        grain = b.noise(b.vscale(P, stretch), 2.2, 3, .6)
        col = b.mix(b.remap(grain,.2,.8), tuple(c*.65 for c in c0), tuple(c*1.2 for c in c0))
        faded = tuple(lum(c0)*1.2+.035+(c-lum(c0))*.45 for c in c0)
        col = b.mix(cond['fade'], col, faded)
        rough_s = .88 if cls == 'wood' else .96
        bump_h = b.mul(grain, .18 if cls == 'wood' else .05)
    elif cls == 'metal' or cls == 'grate':
        c0 = pal['metal'] if cls == 'metal' else [c * .6 for c in pal['metal']]
        col = tuple(c0)
        rust = b.mul(cond['rust'] + .05, b.add(b.add(b.mul(cavity, .9), streak), b.remap(grunge, .45, .75)), True)
        col = b.mix(rust, col, (0.12, 0.045, 0.016))
        metallic = b.mixf(rust, metallic, .1)
        rough_s = b.mixf(rust, rough, .85)
    elif cls == 'ferro':
        c0 = pal.get(slot, pal['ferro'])
        img = b.node('ShaderNodeTexImage', projection='BOX', projection_blend=.25)
        img.image = image(TEX / 'concrete_wall_003/concrete_wall_003_diff_2k.jpg')
        b.feed(img.inputs['Vector'], b.vscale(P, (.35, .35, .35)))
        bw = b.node('ShaderNodeRGBToBW')
        b.feed(bw.inputs[0], img.outputs['Color'])
        tone = b.remap(bw.outputs[0], .2, .7, .78, 1.18)
        cm = b.node('ShaderNodeMix', data_type='RGBA', blend_type='MULTIPLY', clamp_factor=True)
        ins = [i for i in cm.inputs if i.type == 'RGBA']
        b.feed(cm.inputs[0], 1.0)
        b.feed(ins[0], tuple(c0))
        comb = b.node('ShaderNodeCombineXYZ')
        for s in 'XYZ':
            b.feed(comb.inputs[s], tone)
        b.nt.links.new(comb.outputs[0], ins[1])
        col = [o for o in cm.outputs if o.type == 'RGBA'][0]
        # cracks (CrackIntensity) and rust bleed (FerrocreteRust)
        cr = b.voronoi_edge(b.vscale(P, (1, 1, .7)), 1.6)
        crack = b.mul(b.remap(cr, .025, .0), b.math('GREATER_THAN', b.noise(P, .9, 3), 1.0 - cond['cracks'] * .55))
        col = b.mix(crack, col, (0.035, 0.032, 0.028))
        bleed = b.mul(b.mul(cond['rust'], streak), b.remap(b.noise(P, 1.1, 3), .45, .7))
        col = b.mix(b.mul(bleed, .75), col, (0.16, 0.07, 0.025))
        chips = b.math('GREATER_THAN', b.mul(convex, b.remap(grunge, .3, .7, .4, 1.5)), 1.0 - cond['edge_wear'] * .7)
        col = b.mix(b.mul(chips, .6), col, tuple(min(1, c * 1.35) for c in c0))
        nimg = b.node('ShaderNodeTexImage', projection='BOX', projection_blend=.25)
        nimg.image = image(TEX / 'concrete_wall_003/concrete_wall_003_nor_gl_2k.jpg', True)
        b.feed(nimg.inputs['Vector'], b.vscale(P, (.35, .35, .35)))
        bump_h = b.add(bw.outputs[0], b.mul(crack, -1.5))
    elif cls == 'glass':
        col = tuple(pal['glass'])
        rough_s = .05
        cr = b.voronoi_edge(b.vscale(P, (1, 1, 1)), 7.0)
        gc = b.mul(b.remap(cr, .02, .0), cond['glass_cracks'])
        col = b.mix(gc, col, (0.5, 0.52, 0.5))
        bs.inputs['Alpha'].default_value = LANG['material_classes']['glass']['alpha']
        weather = True
    elif cls == 'gravel':
        c0 = pal['roof']
        speck = b.noise(P, 55.0, 2, .5)
        col = b.mix(b.remap(speck, .35, .7), tuple(c * .65 for c in c0), tuple(min(1, c * 1.5) for c in c0))
        bump_h = speck
    elif cls == 'rubber':
        col = (0.02, 0.021, 0.022)
    elif cls in ('lamp', 'lamp_red'):
        col = (0.6, 0.55, 0.4) if cls == 'lamp' else (0.5, 0.03, 0.02)
        if cond_name in ('pristine', 'worn'):
            emission = ((1.0, .82, .55) if cls == 'lamp' else (1.0, .05, .03), 6.0)
        else:
            col = (0.05, 0.05, 0.045)
    elif cls == 'veg':
        if slot == 'moss':
            col = b.mix(b.remap(b.noise(P, 4.0, 4), .3, .7), (0.018, 0.03, 0.008), (0.05, 0.06, 0.018))
        else:
            col = b.mix(b.remap(b.noise(P, 2.0, 2), .3, .7), (0.05, 0.09, 0.02), (0.20, 0.16, 0.06))
        weather = False
        rough_s = .9
    elif cls == 'char':
        col = (0.018, 0.016, 0.014)
        rough_s = .95
        weather = False
    else:
        col = (0.3, 0.3, 0.3)

    if weather:
        dirt_amt = b.mul(cond['dirt'], b.add(b.mul(height_dirt, .9), b.mul(cavity, .8)), True)
        col = b.mix(dirt_amt, col, (0.075, 0.062, 0.045))
        g = b.mul(cond['grunge'] * .45, b.remap(grunge, .35, .75))
        col = b.mix(g, col, (0.06, 0.055, 0.045))
        col = b.mix(b.mul(cond['streaks'] * .65, streak), col, (0.04, 0.037, 0.033))
        if cond['moss'] > 0:
            top = b.remap(nz, .45, .8)
            low = b.remap(z, .5, .1)
            mm = b.mul(b.add(top, low), b.remap(b.noise(P, 1.8, 6, .6), 1.0 - cond['moss'] * .55, 1.0 - cond['moss'] * .45), True)
            col = b.mix(mm, col, (0.025, 0.04, 0.01))
            rough_s = b.mixf(mm, rough_s, .95)
        # soot / scorch from damage
        soot = b.mul(b.remap(dmg_attr, .05, .55), b.remap(grunge, .25, .65, .55, 1.1), True)
        col = b.mix(soot, col, (0.018, 0.016, 0.014))
        heat = b.mul(b.remap(dmg_attr, .02, .25), b.remap(dmg_attr, .45, .2), True)
        col = b.mix(b.mul(heat, .5), col, (0.10, 0.05, 0.025))
        rough_s = b.mixf(soot, rough_s, .95)
        metallic = b.mixf(soot, metallic, 0.0)
    # CC0 base sources contribute grain/roughness without replacing MC2 colour.
    from material_sources import lookup
    source=lookup(key)
    if source:
        tex=b.node('ShaderNodeTexImage');tex.image=image(source['colour'])
        tex.projection='BOX';tex.projection_blend=.15
        b.feed(tex.inputs['Vector'],b.vscale(P,(.35,.35,.35)))
        bw=b.node('ShaderNodeRGBToBW');b.feed(bw.inputs[0],tex.outputs['Color'])
        grain=b.remap(bw.outputs[0],.05,.85,.72,1.15)
        col=b.mix(.30,col,b.mix(1.,col,grain,blend='MULTIPLY'))
        mat['mc2_base_source']=str(source['colour'])
        if source.get('height') and source['height'].is_file():
            hx=b.node('ShaderNodeTexImage');hx.image=image(source['height'],True)
            hx.projection='BOX';hx.projection_blend=.15
            b.feed(hx.inputs['Vector'],b.vscale(P,(.35,.35,.35)))
            bump_h=b.add(bump_h if bump_h is not None else grunge,b.mul(hx.outputs['Color'],.6))
    b.feed(bs.inputs['Base Color'], col)
    b.feed(bs.inputs['Metallic'], metallic)
    b.feed(bs.inputs['Roughness'], rough_s)
    if emission:
        b.feed(bs.inputs['Emission Color'], emission[0])
        bs.inputs['Emission Strength'].default_value = emission[1]
    bump = b.node('ShaderNodeBump')
    bump.inputs['Strength'].default_value = .18
    bump.inputs['Distance'].default_value = .02
    b.feed(bump.inputs['Height'], bump_h if bump_h is not None else grunge)
    b.feed(bs.inputs['Normal'], bump.outputs['Normal'])
    MAT_CACHE[ck] = mat
    return mat



# ------------------------------------------------------------------ mesh build
ROLES = ['mass', 'skin', 'frame', 'roof', 'glass', 'detail']


def mesh_from_pieces(name, pieces, palette, cond_name, collection, role):
    verts, faces, mats, keys, uvs, seam = [], [], [], [], [], []
    for pc in pieces:
        off = len(verts)
        verts += pc['v']
        # Distance below the piece's top edge: fastener rows and laps sit at
        # panel tops, so rust and dirt streaks start there and fade downward.
        top = max(v[2] for v in pc['v'])
        seam += [top - v[2] for v in pc['v']]
        for f in pc['f']:
            faces.append(tuple(off + i for i in f))
            uvs.append([pc.get('uv',[(0.,0.)]*len(pc['v']))[i] for i in f])
            if pc['m'] not in keys:
                keys.append(pc['m'])
            mats.append(keys.index(pc['m']))
    me = bpy.data.meshes.new(name)
    me.from_pydata(verts, [], faces)
    me.validate(clean_customdata=False)
    me.update()
    if len(me.vertices) == len(seam):
        attr = me.attributes.new('seamd', 'FLOAT', 'POINT')
        attr.data.foreach_set('value', seam)
    uv=me.uv_layers.new(name='UVMap')
    for poly,coords in zip(me.polygons,uvs):
        for loop,co in zip(poly.loop_indices,coords):uv.data[loop].uv=co
    for key in keys:
        me.materials.append(material(key, palette, cond_name))
    for poly, mi in zip(me.polygons, mats):
        poly.material_index = mi
    ob = bpy.data.objects.new(name, me)
    ob['mc2_role'] = role
    collection.objects.link(ob)
    return ob


def build_objects(k, cond_name, collection, near=None):
    """One object per structural role. With `near` (hits), pieces close to a hit
    become their own objects so booleans always act on closed, non-overlapping meshes."""
    if k.meta.get('collapse')=='full-rubble':cond_name='rubble'
    objs = {}
    for role in ROLES:
        pieces = [p for p in k.pieces if p['role'] == role]
        if not pieces:
            continue
        if near is None:
            objs[role] = mesh_from_pieces(f'{k.name}_{role}', pieces, k.palette, cond_name, collection, role)
            continue
        close, far = [], []
        for pc in pieces:
            lo, hi = geo.bbox(pc)
            hit_near = False
            for h in near:
                q = [max(lo[i], min(hi[i], h['p'][i])) for i in range(3)]
                if math.dist(q, h['p']) < 2.0 * h['r']:
                    hit_near = True
            (close if hit_near else far).append(pc)
        group = []
        if far:
            group.append(mesh_from_pieces(f'{k.name}_{role}', far, k.palette, cond_name, collection, role))
        for i, pc in enumerate(close):
            group.append(mesh_from_pieces(f'{k.name}_{role}_near{i}', [pc], k.palette, cond_name, collection, role))
        objs[role] = group
    return objs


def join(objs):
    objs = [o for o in objs if o and len(o.data.polygons)]
    if not objs:
        return None
    if len(objs) == 1:
        return objs[0]
    bpy.ops.object.select_all(action='DESELECT')
    for o in objs:
        o.select_set(True)
    bpy.context.view_layer.objects.active = objs[0]
    bpy.ops.object.join()
    return objs[0]


def recalc(ob):
    bm = bmesh.new()
    bm.from_mesh(ob.data)
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=1e-5)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    bm.to_mesh(ob.data)
    bm.free()


def finish(ob, bevel=True):
    # Smooth only connected shallow angles: cylindrical rails and dense arches
    # catch continuous highlights while panel seams and torn edges stay hard.
    bm = bmesh.new()
    bm.from_mesh(ob.data)
    for f in bm.faces:
        f.smooth = True
    for e in bm.edges:
        e.smooth = e.is_manifold and e.calc_face_angle() < math.radians(35)
    bm.to_mesh(ob.data)
    bm.free()
    if bevel:
        mod = ob.modifiers.new('edge catch', 'BEVEL')
        mod.width = .024
        mod.segments = 3
        mod.limit_method = 'ANGLE'
        mod.angle_limit = math.radians(40)
        mod.use_clamp_overlap = True
        mod.harden_normals = True
        normals = ob.modifiers.new('area weighted panel normals', 'WEIGHTED_NORMAL')
        normals.keep_sharp = True


# ------------------------------------------------------------------ destroyed
def hashf(*a):
    return (math.sin(sum(x * m for x, m in zip(a, (12.9898, 78.233, 37.719, 4.581)))) * 43758.5453) % 1.0


def densify(ob, p, r, passes=4, minlen=.22):
    bm = bmesh.new()
    bm.from_mesh(ob.data)
    near = [f for f in bm.faces if any((v.co - p).length < 2.2 * r for v in f.verts) or (f.calc_center_median() - p).length < 2.2 * r]
    if near:
        bmesh.ops.triangulate(bm, faces=near)
    for _ in range(passes):
        edges = []
        for e in bm.edges:
            a, c = e.verts[0].co, e.verts[1].co
            seg = c - a
            L = seg.length
            if L < minlen:
                continue
            t = max(0.0, min(1.0, (p - a).dot(seg) / (L * L)))
            if (a + seg * t - p).length < 2.0 * r:
                edges.append(e)
        if not edges:
            break
        bmesh.ops.subdivide_edges(bm, edges=edges, cuts=1, use_grid_fill=False)
        bmesh.ops.triangulate(bm, faces=[f for f in bm.faces if len(f.verts) > 4])
    bm.to_mesh(ob.data)
    bm.free()


def dent(ob, hit):
    p = Vector(hit['p'])
    d = Vector(hit['d']).normalized()
    r, depth, sag = hit['r'], hit['depth'], hit['sag']
    R = 1.9 * r
    for v in ob.data.vertices:
        off = v.co - p
        dist = off.length
        if dist >= R:
            continue
        f = (1 - dist / R) ** 2
        jitter = Vector((hashf(*v.co) - .5, hashf(v.co.y, v.co.z, v.co.x) - .5, hashf(v.co.z, v.co.x, 1) - .5)) * .09 * f
        v.co += d * depth * f + jitter
        if v.co.z > p.z - .2 * r:
            v.co.z -= sag * f * min(1.0, (v.co.z - p.z + .2 * r) / r + .3)
        v.co.z = max(v.co.z, 0.0)


def cutter(hit, scale=1.0, seed=0):
    p = Vector(hit['p'])
    d = Vector(hit['d']).normalized()
    r = hit['r'] * scale
    bm = bmesh.new()
    bmesh.ops.create_icosphere(bm, subdivisions=3, radius=1.0)
    rng = random.Random(seed)
    for v in bm.verts:
        n = mnoise.noise(v.co * 2.3 + Vector((seed * 1.7, 0, 0)))
        v.co *= r * (.86 + .42 * n + rng.uniform(-.08, .08))
    # stretch along the blast so it passes through the full wall thickness
    up = Vector((0, 0, 1)) if abs(d.z) < .9 else Vector((1, 0, 0))
    u = d.cross(up).normalized()
    w = d.cross(u)
    M = Matrix((u, w, d)).transposed()
    for v in bm.verts:
        local = Vector((v.co.x, v.co.y, v.co.z * hit.get('reach', 2.6)))
        v.co = M @ local + p
    me = bpy.data.meshes.new('cutter')
    bm.to_mesh(me)
    bm.free()
    ob = bpy.data.objects.new('cutter', me)
    bpy.context.scene.collection.objects.link(ob)
    return ob


def boolean(ob, cut, char_mat):
    if char_mat.name not in [m.name for m in cut.data.materials]:
        cut.data.materials.append(char_mat)
    mod = ob.modifiers.new('breach', 'BOOLEAN')
    mod.operation = 'DIFFERENCE'
    mod.solver = 'EXACT'
    mod.object = cut
    mod.use_self = False
    mod.material_mode = 'TRANSFER'
    bpy.context.view_layer.objects.active = ob
    bpy.ops.object.modifier_apply(modifier=mod.name)


def drop_fragments(ob, hit, rng):
    """After a breach, any island cut loose from its piece falls to the ground.

    The largest island stays (it is still fixed to the structure); smaller ones
    are rotated a little and dropped onto z=0 next to the breach, on the blast side.
    """
    bm = bmesh.new()
    bm.from_mesh(ob.data)
    bm.faces.ensure_lookup_table()
    seen = set()
    islands = []
    for f in bm.faces:
        if f.index in seen:
            continue
        stack, isl = [f], []
        seen.add(f.index)
        while stack:
            g = stack.pop()
            isl.append(g)
            for e in g.edges:
                for h in e.link_faces:
                    if h.index not in seen:
                        seen.add(h.index)
                        stack.append(h)
        islands.append(isl)
    if len(islands) > 1:
        islands.sort(key=lambda isl: -sum(f.calc_area() for f in isl))
        d = Vector(hit['d']).normalized()
        for isl in islands[1:]:
            verts = {v for f in isl for v in f.verts}
            c = sum((v.co for v in verts), Vector()) / len(verts)
            ang = rng.uniform(-.6, .6)
            R = Matrix.Rotation(ang, 3, Vector((rng.uniform(-1, 1), rng.uniform(-1, 1), 0)).normalized())
            out = Vector((-d.x, -d.y, 0))
            if out.length < .1:
                out = Vector((rng.uniform(-1, 1), rng.uniform(-1, 1), 0))
            out.normalize()
            target = Vector((c.x, c.y, 0)) + out * rng.uniform(.4, 1.2) * hit['r']
            for v in verts:
                v.co = R @ (v.co - c) + target
            zmin = min(v.co.z for v in verts)
            for v in verts:
                v.co.z -= zmin
    bm.to_mesh(ob.data)
    bm.free()


def rebar(k, hit, rng):
    p = Vector(hit['p'])
    d = Vector(hit['d']).normalized()
    r = hit['r']
    up = Vector((0, 0, 1)) if abs(d.z) < .9 else Vector((1, 0, 0))
    u = d.cross(up).normalized()
    w = d.cross(u).normalized()
    core = p + d * .22
    step = .24
    n = int(1.15 * r / step)
    for axis, other in ((u, w), (w, u)):
        for i in range(-n, n + 1):
            off = i * step
            if abs(off) > r * .95:
                continue
            half = math.sqrt(max(0, (r * 1.02) ** 2 - off * off))
            for s in (-1, 1):
                if rng.random() < .25:
                    continue
                start = core + other * off + axis * (s * half)
                inner = max(.2 * r, half - rng.uniform(.25, .55) * r)
                mid = core + other * off + axis * (s * inner)
                tip = mid + d * rng.uniform(.1, .35) + axis * (-s * rng.uniform(.05, .2)) + Vector((0, 0, -rng.uniform(0, .15)))
                k.tube([tuple(start), tuple(mid), tuple(tip)], .016, 'metal', 'frame', sides=4)


def debris(k, hit, rng):
    p = Vector(hit['p'])
    d = Vector(hit['d']).normalized()
    r = hit['r']
    mats = hit.get('debris', ['ferro'])
    count = int(26 * r * r) + 10
    roof = abs(d.z) > .7
    for i in range(count):
        m = mats[i % len(mats)]
        size = rng.uniform(.08, .42) * min(1.6, r) * (1.6 if i < 3 else 1.0)
        flat = m in ('paint.main', 'paint.second', 'corr.main', 'metal', 'glass') and rng.random() < .75
        if roof:
            a = rng.uniform(0, math.tau)
            rr = rng.uniform(.95, 1.6) * r
            base = (p.x + math.cos(a) * rr, p.y + math.sin(a) * rr, hit.get('floor', p.z))
            if hit.get('ground_debris', True) and i % 2:
                base = (p.x + rng.gauss(0, .5 * r), p.y + rng.gauss(0, .5 * r), 0.0)
        else:
            side = -1 if rng.random() < .65 else 1
            rr = abs(rng.gauss(0, .55 * r))
            a = rng.uniform(-1.4, 1.4)
            dir2 = Vector((d.x, d.y, 0)).normalized() * side
            perp = Vector((-dir2.y, dir2.x, 0))
            pos = Vector((p.x, p.y, 0)) + dir2 * (.25 + rr * math.cos(a) * .9 + .25) + perp * rr * math.sin(a) * 1.3
            heap = max(0.0, .45 * r * (1 - rr / (1.1 * r)))
            base = (pos.x, pos.y, heap * rng.uniform(.2, 1.0) if not flat else heap * .5)
        geo.chunk(k, base, size, m if m != 'glass' else 'glass', rng, flat=flat)


def soot(objs, hits):
    for ob in objs:
        me = ob.data
        attr = me.attributes.get('dmg') or me.attributes.new('dmg', 'FLOAT', 'POINT')
        vals = [0.0] * len(me.vertices)
        for hit in hits:
            p = Vector(hit['p'])
            r = hit['r']
            for i, v in enumerate(me.vertices):
                off = v.co - p
                dz = off.z * (.62 if off.z > 0 else 1.3)
                dist = math.sqrt(off.x * off.x + off.y * off.y + dz * dz)
                vals[i] = max(vals[i], min(1.0, max(0.0, 1.25 - dist / (1.55 * r))))
        attr.data.foreach_set('value', vals)


def destroy(groups, k, cond_name, collection):
    """groups: role -> [far object, near piece objects...]. Returns role -> joined object."""
    rng = random.Random(k.seed + 99)
    char = material('char', k.palette, cond_name)
    extra = geo.Kit(k.name + '_debris', k.palette, k.seed)
    scale = {'mass': 1.0, 'skin': 1.08, 'roof': 1.0, 'frame': .5, 'detail': .8, 'glass': 1.0}
    for hi, hit in enumerate(k.hits):
        p = Vector(hit['p'])
        for role, group in groups.items():
            for ob in group:
                if '_near' not in ob.name or not len(ob.data.polygons):
                    continue
                if role in ('mass', 'skin', 'roof', 'frame'):
                    densify(ob, p, hit['r'])
                    dent(ob, hit)
                cut = cutter(hit, scale.get(role, 1.0), seed=hi * 7)
                boolean(ob, cut, char)
                bpy.data.objects.remove(cut, do_unlink=True)
                drop_fragments(ob, hit, rng)
        if hit.get('rebar'):
            rebar(extra, hit, rng)
        debris(extra, hit, rng)
    objs = {role: join(group) for role, group in groups.items()}
    objs = {r: o for r, o in objs.items() if o}
    for role, ob in objs.items():
        ob.name = f'{k.name}_{role}'
    dobjs = build_objects(extra, cond_name, collection)
    for role, ob in dobjs.items():
        ob.name = f'{k.name}_debris_{role}'
    soot(list(objs.values()) + list(dobjs.values()), k.hits)
    for role, ob in dobjs.items():
        objs['debris_' + role] = ob
    return objs


# ------------------------------------------------------------------ render
def look_scene(scene, ground=True):
    world = bpy.data.worlds.new('sky')
    scene.world = world
    world.use_nodes = True
    sky = world.node_tree.nodes.get('Background')
    sky.inputs[0].default_value = (.55, .62, .74, 1)
    sky.inputs[1].default_value = .55
    sun_d = bpy.data.lights.new('sun', 'SUN')
    sun_d.energy = 3.6
    sun_d.angle = math.radians(5)
    sun_d.color = (1.0, .96, .9)
    sun = bpy.data.objects.new('sun', sun_d)
    scene.collection.objects.link(sun)
    sun.rotation_euler = (math.radians(42), math.radians(-28), math.radians(-38))
    if ground:
        me = bpy.data.meshes.new('ground')
        s = 400
        me.from_pydata([(-s, -s, 0), (s, -s, 0), (s, s, 0), (-s, s, 0)], [], [(0, 1, 2, 3)])
        g = bpy.data.objects.new('ground', me)
        gm = bpy.data.materials.new('ground')
        gm.use_nodes = True
        nb = gm.node_tree.nodes
        bsdf = nb.get('Principled BSDF')
        tex = nb.new('ShaderNodeTexNoise')
        tex.inputs['Scale'].default_value = .6
        ramp = nb.new('ShaderNodeValToRGB')
        ramp.color_ramp.elements[0].color = (.13, .125, .11, 1)
        ramp.color_ramp.elements[1].color = (.19, .18, .16, 1)
        gm.node_tree.links.new(tex.outputs['Fac'], ramp.inputs[0])
        gm.node_tree.links.new(ramp.outputs[0], bsdf.inputs['Base Color'])
        bsdf.inputs['Roughness'].default_value = .95
        me.materials.append(gm)
        scene.collection.objects.link(g)
    cam_d = bpy.data.cameras.new('cam')
    cam_d.type = 'ORTHO'
    cam = bpy.data.objects.new('cam', cam_d)
    scene.collection.objects.link(cam)
    scene.camera = cam
    return cam


def frame_camera(cam, objs, direction=(-1.0, -1.55, 1.1), margin=1.12, aspect=1.0):
    d = Vector(direction).normalized()
    pts = []
    for ob in objs:
        if ob.type != 'MESH':
            continue
        for c in ob.bound_box:
            pts.append(ob.matrix_world @ Vector(c))
    center = sum(pts, Vector()) / len(pts)
    rot = (-d).to_track_quat('-Z', 'Y')
    cam.rotation_euler = rot.to_euler()
    M = rot.to_matrix()
    right, up = M.col[0], M.col[1]
    xs = [(p - center).dot(right) for p in pts]
    ys = [(p - center).dot(up) for p in pts]
    cx, cy = (max(xs) + min(xs)) / 2, (max(ys) + min(ys)) / 2
    w, h = max(xs) - min(xs), max(ys) - min(ys)
    cam.data.ortho_scale = max(w, h * aspect) * margin
    cam.location = center + right * cx + up * cy + d * 200
    cam.data.clip_end = 1000
    return cam.data.ortho_scale, tuple(cam.location), tuple(cam.rotation_euler)


def render(path, res=(560, 560), samples=48):
    scene = bpy.context.scene
    scene.cycles.samples = samples
    scene.render.resolution_x, scene.render.resolution_y = res
    scene.render.resolution_percentage = 100
    scene.render.filepath = str(path)
    scene.render.image_settings.file_format = 'PNG'
    bpy.ops.render.render(write_still=True)


# ------------------------------------------------------------------ export
def export_glb(objs, path, k, cond_name):
    """Clean glTF: Y-up metres, identity-free transforms preserved, flat material
    classes (factor-only PBR) carrying mc2 class/condition extras, glass alpha blend."""
    keep = {}
    for ob in objs:
        keep[ob.name] = list(ob.data.materials)
        for i, m in enumerate(ob.data.materials):
            key, palette, cond = m.name.split('|') if m.name.count('|') == 2 else (m.name, k.palette, cond_name)
            name = f'mc2_{key}'
            em = bpy.data.materials.get(name + '|x|' + palette + cond)
            if em is None:
                em = bpy.data.materials.new(name + '|x|' + palette + cond)
                em.use_nodes = True
                bsdf = em.node_tree.nodes.get('Principled BSDF')
                cls, _, slot = key.partition('.')
                pal = LANG['palettes'][palette]
                c = pal.get(slot) or pal.get(cls) or {'glass': pal['glass'], 'gravel': pal['roof'], 'hazard': pal['accent'], 'ferro': pal['ferro'], 'metal': pal['metal'], 'grate': pal['metal'], 'rubber': [.02, .02, .02], 'char': [.02, .018, .016]}.get(cls, [.3, .3, .3])
                if cls == 'corr':
                    c = pal['corr']
                info = LANG['material_classes'].get(cls, {'metallic': 0, 'roughness': .8})
                bsdf.inputs['Base Color'].default_value = (*c[:3], 1)
                bsdf.inputs['Metallic'].default_value = info.get('metallic', 0)
                bsdf.inputs['Roughness'].default_value = info.get('roughness', .8)
                if cls == 'glass':
                    bsdf.inputs['Alpha'].default_value = info['alpha']
                    try:
                        em.surface_render_method = 'BLENDED'
                    except AttributeError:
                        em.blend_method = 'BLEND'
                if cls.startswith('lamp') and cond_name in ('pristine', 'worn'):
                    bsdf.inputs['Emission Color'].default_value = (1, .82, .55, 1) if cls == 'lamp' else (1, .05, .03, 1)
                    bsdf.inputs['Emission Strength'].default_value = 1.0
                em['mc2_material_class'] = cls
                em['mc2_slot'] = slot
                em['mc2_palette'] = palette
                em['mc2_condition'] = cond_name
            ob.data.materials[i] = em
    bpy.ops.object.select_all(action='DESELECT')
    for ob in objs:
        ob.select_set(True)
    bpy.ops.export_scene.gltf(filepath=str(path), export_format='GLB', use_selection=True, export_yup=True, export_apply=True,
                              export_extras=True, export_normals=True, export_materials='EXPORT', export_attributes=True)
    for ob in objs:
        for i, m in enumerate(keep[ob.name]):
            ob.data.materials[i] = m


# ------------------------------------------------------------------ jobs
def primitive_job(job):
    out = Path(job['out'])
    out.mkdir(parents=True, exist_ok=True)
    report = {}
    for name in job['primitives']:
        framing = None
        for cond in job['conditions']:
            reset()
            MAT_CACHE.clear()
            scene = bpy.context.scene
            col = scene.collection
            k = geo.build(name, cond, job.get('seed', 17))
            if cond in ('partial','destroyed') and not k.meta.get('authored_damage'):
                groups = build_objects(k, cond, col, near=k.hits)
                for g in groups.values():
                    for ob in g:
                        recalc(ob)
                objs = destroy(groups, k, cond, col)
            else:
                objs = build_objects(k, cond, col)
            for ob in objs.values():
                recalc(ob)
            for ob in objs.values():
                finish(ob, bevel=ob.get('mc2_role') not in ('glass',))
            cam = look_scene(scene)
            if framing is None:
                framing = frame_camera(cam, list(objs.values()))
            else:
                cam.data.ortho_scale, cam.location, cam.rotation_euler = framing[0], framing[1], framing[2]
            import time
            t0 = time.time()
            png = out / f'{name}_{cond}.png'
            render(png, tuple(job.get('res', (560, 560))), job.get('samples', 48))
            if job.get('export', True):
                glb = out / 'glb' / f'{name}_{cond}.glb'
                glb.parent.mkdir(parents=True, exist_ok=True)
                export_glb(list(objs.values()), glb, k, cond)
            tris = sum(sum(len(p.vertices) - 2 for p in ob.data.polygons) for ob in objs.values())
            report[f'{name}_{cond}'] = {'triangles': tris, 'pieces': len(k.pieces), 'palette': k.palette}
            print('[art] done', name, cond, tris, 'render+export s', round(time.time() - t0, 1), flush=True)
    (out / f'report_{job.get("tag", "prims")}.json').write_text(json.dumps(report, indent=1), encoding='utf-8')


def assembly_job(job):
    import time
    import assemble
    out = Path(job['out'])
    out.mkdir(parents=True, exist_ok=True)
    report = {}
    for name in job['assemblies']:
        framing = None
        for cond in job.get('conditions_by_asset',{}).get(name,job['conditions']):
            t0 = time.time()
            reset()
            MAT_CACHE.clear()
            scene = bpy.context.scene
            col = scene.collection
            k = assemble.build(name, cond, job.get('seed', 17))
            if cond in ('partial','destroyed') and not k.meta.get('authored_damage'):
                groups = build_objects(k, cond, col, near=k.hits)
                for g in groups.values():
                    for ob in g:
                        recalc(ob)
                objs = destroy(groups, k, cond, col)
            else:
                objs = build_objects(k, cond, col)
                if cond in ('partial','destroyed'):
                    soot(list(objs.values()), k.hits)
            for ob in objs.values():
                recalc(ob)
                finish(ob, bevel=ob.get('mc2_role') not in ('glass',))
            cam = look_scene(scene)
            if framing is None:
                framing = frame_camera(cam, list(objs.values()), margin=1.08, aspect=1100 / 760)
            baked=None
            if k.meta.get('draft') and job.get('export',True):
                import bake
                low,baked=bake.bake(list(objs.values()),k,cond,out/'pbr')
            cam.data.ortho_scale, cam.location, cam.rotation_euler = framing[0], framing[1], framing[2]
            render(out / f'{name}_{cond}_beauty.png', (1100, 760), job.get('samples', 48))
            if cond == 'pristine':
                scene.render.film_transparent=True
                scene.render.image_settings.color_mode='RGBA'
                bpy.data.objects['ground'].hide_render=True
                render(out/f'{name}_palette.png',(440,304),16)
                bpy.data.objects['ground'].hide_render=False
                scene.render.film_transparent=False
                frame_camera(cam, [low] if baked else list(objs.values()), direction=(-1.0, -1.25, 2.4), margin=2.6, aspect=2.0)
                # One world scale across the batch: 0.24 authoring m per pixel.
                # This is a declared RTS proxy, not a runtime camera capture.
                cam.data.ortho_scale = 360 * .24
                render(out / f'{name}_pristine_rts.png', (360, 180), 32)
                # All sides are visible in the count proof, including the left
                # tower door and rear quonset end omitted by the beauty camera.
                for side,direction in [('front',(0,-1,.08)),('left',(-1,0,.08)),
                                       ('rear',(0,1,.08)),('right',(1,0,.08))]:
                    frame_camera(cam,[low] if baked else list(objs.values()),
                                 direction=direction,margin=1.14,aspect=1.4)
                    render(out/f'{name}_count_{side}.png',(700,500),16)
            if job.get('export', True) and not baked:
                glb = out / 'glb' / f'{name}_{cond}.glb'
                glb.parent.mkdir(parents=True, exist_ok=True)
                export_glb(list(objs.values()), glb, k, cond)
            tris = sum(sum(len(p.vertices) - 2 for p in ob.data.polygons) for ob in objs.values())
            bounds = [ob.matrix_world @ Vector(v) for ob in objs.values() for v in ob.bound_box]
            report[f'{name}_{cond}'] = {'triangles': tris, 'pieces': len(k.pieces), 'palette': k.palette,
                                      'bounds_xyz_m': [max(v[i] for v in bounds)-min(v[i] for v in bounds) for i in range(3)],
                                      'stock_size_m': k.meta.get('size'), 'fit_scale_xyz': k.meta.get('fit_scale_xyz')}
            if baked:report[f'{name}_{cond}']['bake']=baked
            print('[art] done', name, cond, tris, 's', round(time.time() - t0, 1), flush=True)
    (out / f'report_{job.get("tag", "asm")}.json').write_text(json.dumps(report, indent=1), encoding='utf-8')


if __name__ == '__main__':
    argv = sys.argv[sys.argv.index('--') + 1:]
    job = json.loads(Path(argv[0]).read_text(encoding='utf-8'))
    if job['kind'] == 'primitives':
        primitive_job(job)
    elif job['kind'] == 'assembly':
        assembly_job(job)
    elif job['kind'] == 'baked_review':
        import baked_review
        baked_review.run(job,sys.modules[__name__])
