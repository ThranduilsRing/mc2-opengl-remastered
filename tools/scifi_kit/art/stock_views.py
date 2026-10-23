"""Render the decoded STOCK mesh with its stock texture from front/left/rear/right/top.

Reference only: the decoded mesh (round2/stock/<name>.json, authoring metres, Z up,
front -Y) is drawn for count/silhouette review and never enters any output asset.
Camera directions match blend.assembly_job's remake count views, so stock-side-N
sits next to remake-side-N on the review sheets.
"""
import json
from pathlib import Path

import bpy

STOCK = Path('G:/mc2-assets/out/scifi-kit/round2/stock')
SIDES = [('front', (0, -1, .08)), ('left', (-1, 0, .08)), ('rear', (0, 1, .08)),
         ('right', (1, 0, .08)), ('top', (0.0001, -0.0001, 1))]


def stock_mesh(name):
    d = json.loads((STOCK / f'{name}.json').read_text())
    verts = [tuple(v) for v in d['vertices_m']]
    faces = [(i, i + 1, i + 2) for i in range(0, len(verts), 3)]
    me = bpy.data.meshes.new('stock_' + name)
    me.from_pydata(verts, [], faces)
    uv = me.uv_layers.new(name='UVMap')
    for poly in me.polygons:
        for loop in poly.loop_indices:
            u, v = d['uv'][me.loops[loop].vertex_index]
            uv.data[loop].uv = (u, 1 - v)
    for i, path in enumerate(d['texture_paths']):
        mat = bpy.data.materials.new(f'stock_{name}_{i}')
        mat.use_nodes = True
        nt = mat.node_tree
        bs = nt.nodes.get('Principled BSDF')
        bs.inputs['Roughness'].default_value = .8
        if path and Path(path).is_file():
            tex = nt.nodes.new('ShaderNodeTexImage')
            tex.image = bpy.data.images.load(path, check_existing=True)
            tex.interpolation = 'Closest'
            nt.links.new(tex.outputs['Color'], bs.inputs['Base Color'])
            if d['texture_alpha'][i]:
                nt.links.new(tex.outputs['Alpha'], bs.inputs['Alpha'])
                try:
                    mat.surface_render_method = 'DITHERED'
                except AttributeError:
                    pass
        me.materials.append(mat)
    for poly, mi in zip(me.polygons, d['materials']):
        poly.material_index = mi
    ob = bpy.data.objects.new('stock_' + name, me)
    bpy.context.scene.collection.objects.link(ob)
    return ob


def run(job, stage):
    out = Path(job['out'])
    out.mkdir(parents=True, exist_ok=True)
    for name in job['assemblies']:
        stage.reset()
        ob = stock_mesh(job.get('stock', {}).get(name, name))
        cam = stage.look_scene(bpy.context.scene)
        bpy.data.objects['ground'].hide_render = True
        for side, direction in SIDES:
            stage.frame_camera(cam, [ob], direction=direction, margin=1.14, aspect=1.4)
            stage.render(out / f'{name}_stock_{side}.png', (700, 500), 16)
