"""Blender: assemble read-only MW5 exports from explicit socket recipes.

blender --background --python tools/mw5_mechs/assemble_vehicles.py -- RECIPE OUT
Only the output directory receives assets; recipes and source caches stay read-only.
"""
import ast
import json
import math
from pathlib import Path
import re
import sys
import struct

import bmesh
import bpy
import numpy as np
from mathutils import Euler, Matrix, Quaternion, Vector


def empty(name, matrix, parent=None, animated=False):
    ob = bpy.data.objects.new(name, None)
    bpy.context.collection.objects.link(ob)
    ob.parent = parent
    ob.matrix_world = matrix
    if animated:
        ob['mc2_anim_node'] = name
    return ob


def import_part(path, parent, prefix, moving):
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=str(path))
    added = set(bpy.data.objects) - before
    # Blender adjusts display-bone axes on import. Attach in the authored glTF
    # node frames instead, converting Y-up metres to Blender Z-up metres.
    doc = (json.loads(path.read_bytes()[20:20 + struct.unpack_from('<I', path.read_bytes(), 12)[0]])
           if path.suffix == '.glb' else json.loads(path.read_text()))
    nodes = doc['nodes']
    parents = {c: i for i, n in enumerate(nodes) for c in n.get('children', [])}
    basis = Matrix(((1, 0, 0, 0), (0, 0, -1, 0), (0, 1, 0, 0), (0, 0, 0, 1)))
    def world(i):
        n = nodes[i]
        if 'matrix' in n:
            m = Matrix([n['matrix'][j:j+4] for j in range(0, 16, 4)]).transposed()
        else:
            x, y, z, w = n.get('rotation', (0, 0, 0, 1))
            m = Matrix.LocRotScale(Vector(n.get('translation', (0, 0, 0))), Quaternion((w, x, y, z)),
                                   Vector(n.get('scale', (1, 1, 1))))
        return world(parents[i]) @ m if i in parents else m
    sockets = {n['name']: basis @ world(i) @ basis.inverted()
               for i, n in enumerate(nodes) if 'mesh' not in n}
    helpers = {b.custom_shape for o in added if o.type == 'ARMATURE' for b in o.pose.bones if b.custom_shape}
    meshes = [o for o in added if o.type == 'MESH' and o not in helpers]
    result = []
    for ob in meshes:
        world = ob.matrix_world.copy()
        # The source bind pose is the assembled pose; partition only moving rotors.
        groups = {g.index: g.name for g in ob.vertex_groups}
        owners = {}
        for v in ob.data.vertices:
            group = max(v.groups, key=lambda g: g.weight, default=None)
            owners[v.index] = moving.get(groups.get(group.group)) if group else None
        targets = set(owners.values())
        for target in sorted(targets, key=str):
            mesh = ob.data.copy()
            bm = bmesh.new()
            bm.from_mesh(mesh)
            bm.verts.ensure_lookup_table()
            remove = [f for f in bm.faces if owners[f.verts[0].index] != target]
            bmesh.ops.delete(bm, geom=remove, context='FACES')
            bmesh.ops.delete(bm, geom=[v for v in bm.verts if not v.link_faces], context='VERTS')
            bm.to_mesh(mesh)
            bm.free()
            if not mesh.polygons:
                bpy.data.meshes.remove(mesh)
                continue
            dst = bpy.data.objects.new(prefix + '_mesh_' + str(len(result)), mesh)
            bpy.context.collection.objects.link(dst)
            dst.parent = parent
            dst.matrix_world = parent.matrix_world @ world
            if target:
                bone = next(b for b, name in moving.items() if name == target)
                pivot = parent.matrix_world @ sockets[bone]
                rotor = empty(target, pivot, parent, True)
                rotor['source_bone'] = bone
                rotor['source_bind_rotation'] = nodes[next(i for i,n in enumerate(nodes) if n.get('name') == bone)].get('rotation', [0,0,0,1])
                old = dst.matrix_world.copy()
                dst.parent = rotor
                dst.matrix_world = old
            result.append(dst)
    for ob in added:
        bpy.data.objects.remove(ob, do_unlink=True)
    return result, {name: parent.matrix_world @ m for name, m in sockets.items()}


def flight_clip(path):
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from build_glb import read_psa, psa_quat_to_gltf
    bones, keys, rate, count = read_psa(path)
    basis = Quaternion((math.sqrt(.5), math.sqrt(.5), 0, 0))
    bpy.context.scene.render.fps = round(rate)
    bpy.context.scene.render.fps_base = round(rate) / rate
    for ob in list(bpy.data.objects):
        if 'source_bone' not in ob:
            continue
        bone = bones.index(ob['source_bone'])
        quats = psa_quat_to_gltf(keys[:, bone, 3:7], False)
        x,y,z,w = ob['source_bind_rotation']
        bind = basis @ Quaternion((w,x,y,z)) @ basis.inverted()
        rest = ob.matrix_local.to_quaternion()
        ob.rotation_mode = 'QUATERNION'
        for frame, (x,y,z,w) in enumerate(quats):
            pose = basis @ Quaternion((w,x,y,z)) @ basis.inverted()
            ob.rotation_quaternion = rest @ bind.inverted() @ pose
            ob.keyframe_insert('rotation_quaternion', frame=frame)
        action = ob.animation_data.action
        ob.animation_data.action = None
        track = ob.animation_data.nla_tracks.new()
        track.name = 'atlas_moveCoreIdle'
        track.strips.new(action.name, 0, action)
    bpy.context.scene.frame_set(0)


def decoder(path):
    # Reuse the exact first-layer material interpretation used by the inventory.
    tree = ast.parse(Path(path).read_text())
    names = {'material_maps', 'material_colors', 'build_material'}
    module = ast.Module(body=[n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in names],
                        type_ignores=[])
    scope = dict(bpy=bpy, re=re, Path=Path)
    exec(compile(module, str(path), 'exec'), scope)
    return scope


def render(out, meshes):
    points = [o.matrix_world @ Vector(c) for o in meshes for c in o.bound_box]
    lo = Vector([min(p[i] for p in points) for i in range(3)])
    hi = Vector([max(p[i] for p in points) for i in range(3)])
    center, extent = (lo + hi) / 2, max(hi - lo)
    scene = bpy.context.scene
    scene.render.engine = 'CYCLES'
    scene.cycles.samples = 16
    scene.render.resolution_x = 720
    scene.render.resolution_y = 720
    scene.render.resolution_percentage = 100
    scene.world.color = (.22, .22, .22)
    scene.view_settings.view_transform = 'AgX'
    for name, loc, power, size in [('key', (1, -2, 3), 1800, 12), ('fill', (-2, 1, 2), 1200, 10)]:
        light = bpy.data.lights.new(name, 'AREA')
        light.energy, light.shape, light.size = power * max(extent, 1), 'DISK', size
        ob = bpy.data.objects.new(name, light)
        bpy.context.collection.objects.link(ob)
        ob.location = center + Vector(loc) * extent
        ob.rotation_euler = (center - ob.location).to_track_quat('-Z', 'Y').to_euler()
    camera = bpy.data.objects.new('camera', bpy.data.cameras.new('camera'))
    bpy.context.collection.objects.link(camera)
    scene.camera = camera
    camera.data.type = 'ORTHO'
    camera.data.ortho_scale = extent * 1.3
    for name, direction in [('top', (0, 0, 3)), ('quarter', (2, -3, 2.3))]:
        camera.location = center + Vector(direction) * extent
        camera.rotation_euler = (center - camera.location).to_track_quat('-Z', 'Y').to_euler()
        scene.render.filepath = str(out / (name + '.png'))
        bpy.ops.render.render(write_still=True)
    return [list(lo), list(hi)]


def main(recipe, output):
    recipe = json.loads(Path(recipe).read_text())
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    paths = {key: {p.stem: p for p in Path(root).rglob('*') if p.suffix in ('.gltf', '.glb')}
             for key, root in recipe['exports'].items()}
    roots = list(map(Path, recipe['exports'].values()))
    props = {p.name.removesuffix('.props.txt'): p for root in roots for p in root.rglob('*.props.txt')}
    pngs = {p.stem: p for root in roots for p in root.rglob('*.png')}
    dec = decoder(recipe['material_decoder'])
    receipt = []
    for vehicle in recipe['vehicles']:
        bpy.ops.object.select_all(action='SELECT')
        bpy.ops.object.delete(use_global=False)
        for mat in list(bpy.data.materials):
            bpy.data.materials.remove(mat)
        frames, sockets, meshes, sources = {}, {}, [], []
        for component in vehicle['components']:
            path = paths[component['export']][component['mesh']] if component.get('mesh') else None
            anchor = component.get('at')
            parent = frames.get(component.get('parent'))
            matrix = sockets[anchor].copy() if anchor else (parent.matrix_world.copy() if parent else Matrix.Identity(4))
            matrix @= Matrix.Translation(Vector(component.get('offset_cm', [0, 0, 0])) * .01)
            pitch, yaw, roll = component.get('rotation_deg', [0, 0, 0])
            matrix @= Euler(tuple(math.radians(v) for v in (roll, -pitch, yaw)), 'XYZ').to_matrix().to_4x4()
            matrix @= Matrix.Diagonal((*component.get('scale', [1, 1, 1]), 1))
            node = empty(component['node'], matrix, parent, component.get('animated', False))
            objects, bones = import_part(path, node, component['node'], component.get('rotors', {})) if path else ([], {})
            frames[component['node']] = node
            sockets.update({component['node'] + ':' + k: v for k, v in bones.items()})
            for name, offset in component.get('sockets_cm', {}).items():
                sockets[component['node'] + ':' + name] = matrix @ Matrix.Translation(Vector(offset) * .01)
            meshes.extend(objects)
            if path:
                sources.append(str(path))
        for mat in list(bpy.data.materials):
            name = re.sub(r'\.\d{3}$', '', mat.name)
            params = dec['material_maps'](name, props, pngs)
            params.update({k: str(pngs[v]) for k, v in vehicle.get('material_maps', {}).get(name, {}).items()})
            # These are unbound master-material placeholders, not vehicle maps.
            params = {k: v for k, v in params.items() if Path(v).stem not in ('Noise_VeryStrong_NRM', 'Flat_White')}
            if not params:
                continue
            maps = dec['build_material'](mat, params, pngs, dec['material_colors'](name, props))
            if maps['normal']:
                # Reconstruct BC5 Z and flip DirectX Y, as the mech texture cooker
                # does. A direct texture -> Normal Map link also survives glTF export.
                image = bpy.data.images.load(maps['normal'], check_existing=False)
                image.colorspace_settings.name = 'Non-Color'
                if max(image.size) > 1024:
                    ratio = 1024 / max(image.size)
                    image.scale(*(max(1, round(n * ratio)) for n in image.size))
                rgba = np.empty(len(image.pixels), dtype=np.float32)
                image.pixels.foreach_get(rgba)
                rgba = rgba.reshape(-1, 4)
                x, y = rgba[:, 0] * 2 - 1, 1 - rgba[:, 1] * 2
                z = np.sqrt(np.clip(1 - x*x - y*y, 0, 1))
                rgba[:, :3] = np.stack((x, y, z), axis=1) * .5 + .5
                corrected = bpy.data.images.new(name + '_normal_gl', *image.size)
                corrected.colorspace_settings.name = 'Non-Color'
                corrected.pixels.foreach_set(rgba.ravel())
                tex = mat.node_tree.nodes.new('ShaderNodeTexImage')
                tex.image = corrected
                normal = next(n for n in mat.node_tree.nodes if n.type == 'NORMAL_MAP')
                mat.node_tree.links.new(tex.outputs['Color'], normal.inputs['Color'])
        target = output / vehicle['id']
        target.mkdir(exist_ok=True)
        triangles = sum(sum(len(p.vertices) - 2 for p in o.data.polygons) for o in meshes)
        if vehicle.get('flight_clip'):
            flight_clip(vehicle['flight_clip'])
        bpy.ops.export_scene.gltf(filepath=str(target/'assembled.glb'), export_format='GLB',
                                  export_extras=True, export_animations=bool(vehicle.get('flight_clip')), export_skins=False,
                                  export_animation_mode='NLA_TRACKS',
                                  export_cameras=False, export_lights=False)
        bounds = render(target, meshes)
        receipt.append(dict(id=vehicle['id'], family=vehicle['family'], replaces=vehicle['replaces'],
                            triangles=triangles, sources=sources, bounds_m=bounds,
                            nodes=[o.name for o in bpy.data.objects if o.get('mc2_anim_node')]))
        (output/'assembly-receipt.json').write_text(json.dumps(receipt, indent=2))


if __name__ == '__main__':
    main(*sys.argv[sys.argv.index('--') + 1:])
