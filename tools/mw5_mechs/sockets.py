"""Local weapon fire points from attached exports and a resolved loadout table.

The table contains names only: mesh, bone, node, type and optional fires.
Blank rows have a null node. No hull geometry is used as a weapon muzzle.
"""
import json
import struct
from pathlib import Path

import numpy as np

if __package__:
    from .build_glb import load_recipe, read_accessor, trs_mat
else:
    from build_glb import load_recipe, read_accessor, trs_mat


def load_glb(path):
    data = Path(path).read_bytes()
    size = struct.unpack_from('<I', data, 12)[0]
    return json.loads(data[20:20 + size]), data[28 + size:]


def package_nodes(glb, loadout, recipe=None):
    """Return node manifest, local offsets, INI types and pre-weapon hull height."""
    recipe = load_recipe() if recipe is None else recipe
    rename = recipe["bone_rename"]
    g, buf = load_glb(glb)
    parent = {c: i for i, n in enumerate(g['nodes']) for c in n.get('children', [])}
    def world(i):
        return (world(parent[i]) if i in parent else np.eye(4)) @ trs_mat(g['nodes'][i])
    joints = {n['name']: i for i, n in enumerate(g['nodes'])}
    attached = g['extras']['mw5Attachments']
    nodes, offsets, types = dict(recipe["mc2"]["nodes"]), {}, {}
    for row in loadout:
        if not row.get('node'):
            continue
        name, mesh = row['node'], row['mesh']
        if name in nodes or len(name) > 24:
            raise ValueError(f'duplicate or overlong weapon node: {name}')
        matches = [a for a in attached if a['source'] == mesh]
        if len(matches) != 1:
            raise ValueError(f'weapon attachment not unique: {mesh}')
        a = matches[0]
        joint = rename.get(row['bone'], row['bone'])
        if a['joint'] != joint:
            raise ValueError(f'attachment joint mismatch: {mesh}')
        authored = [a['sockets'][s] for s in row.get('fires', []) if s in a['sockets']]
        if authored:
            # MC2 has one fire point per weapon node; use the first authored barrel.
            local = np.asarray(authored[0], dtype=float)
        else:
            # M1's verified forward: horizontal pelvis -> cockpit, in hull space.
            forward = world(joints[recipe['mc2']['nodes']['cockpit']])[:3, 3] - world(joints[rename.get('Pelvis', 'Pelvis')])[:3, 3]
            forward[1] = 0
            if np.linalg.norm(forward) < 1e-6:
                raise ValueError('cannot resolve hull forward axis')
            forward /= np.linalg.norm(forward)
            verts = np.concatenate([read_accessor(g, buf, pr['attributes']['POSITION'])
                                    for m in a['meshes'] for pr in g['meshes'][m]['primitives']])
            tip = verts[np.argmax(verts @ forward)]
            local = (np.linalg.inv(world(joints[joint])) @ np.r_[tip, 1])[:3]
        if not np.all(np.isfinite(local)):
            raise ValueError(f'nonfinite muzzle: {name}')
        nodes[name] = joint
        offsets[name] = ' '.join(f'{v:.4f}' for v in local)
        types[name] = int(row['type'])
        if types[name] not in (1, 2, 4):
            raise ValueError(f'unsupported weapon type: {types[name]}')
    height = float(g['extras']['trueHeightM'])
    if not np.isfinite(height) or height <= 0:
        raise ValueError('invalid pre-weapon hull height')
    return nodes, offsets, types, height
