#!/usr/bin/env python3
"""Retarget Atlas clips through model-space bind frames, preserving native clips.
Recipes may lock identified gun-arm chains to their model-space bind orientation.

Usage:
  py -3 bt2018_retarget_clips.py <target.glb> <atlas_ref.glb> <out.glb>
"""
import sys
import copy
import json
from pathlib import Path
import numpy as np
from pygltflib import GLTF2

FLOAT = 5126


def quat_mul(a, b):
    # Hamilton product, [x,y,z,w]
    ax, ay, az, aw = np.moveaxis(np.asarray(a), -1, 0)
    bx, by, bz, bw = np.moveaxis(np.asarray(b), -1, 0)
    return np.stack([
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by - ax * bz + ay * bw + az * bx,
        aw * bz + ax * by - ay * bx + az * bw,
        aw * bw - ax * bx - ay * by - az * bz,
    ], axis=-1)


def quat_conj(q):
    return np.asarray(q) * [-1, -1, -1, 1]


def node_bind_rot(gltf, node):
    r = gltf.nodes[node].rotation
    return np.array(r if r else [0.0, 0.0, 0.0, 1.0], dtype=np.float64)


def name_to_index(gltf):
    return {n.name: i for i, n in enumerate(gltf.nodes) if n.name}


def accessor_view(gltf, blob, acc_idx):
    acc = gltf.accessors[acc_idx]
    assert acc.componentType == FLOAT and acc.type == "VEC4", \
        f"accessor {acc_idx} not FLOAT VEC4 ({acc.componentType},{acc.type})"
    bv = gltf.bufferViews[acc.bufferView]
    base = (bv.byteOffset or 0) + (acc.byteOffset or 0)
    stride = bv.byteStride or 16
    assert stride == 16, f"unexpected byteStride {stride}"
    n = acc.count
    arr = np.frombuffer(blob, dtype=np.float32, count=n * 4, offset=base)
    return arr.reshape(n, 4), base, n


def retarget_file(target_path, atlas_path, out_path):
    """Retarget Atlas rotation clips in target_path from the Atlas-skeleton bind
    into target_path's own bind frame; write to out_path. Returns (channels, keys)."""
    tgt = GLTF2().load(target_path)
    atl = GLTF2().load(atlas_path)
    blob = bytearray(tgt.binary_blob())

    atl_names = name_to_index(atl)
    def parents(g):
        return {c: i for i, n in enumerate(g.nodes) for c in (n.children or [])}
    def globals_for(g, local):
        parent, result = parents(g), {}
        def visit(i):
            if i not in result:
                result[i] = quat_mul(visit(parent[i]), local[i]) if i in parent else local[i]
            return result[i]
        return [visit(i) for i in range(len(g.nodes))]
    target_bind = [node_bind_rot(tgt, i) for i in range(len(tgt.nodes))]
    atlas_bind = [node_bind_rot(atl, i) for i in range(len(atl.nodes))]
    target_world, atlas_world = globals_for(tgt, target_bind), globals_for(atl, atlas_bind)
    recipe = json.loads((Path(__file__).resolve().parents[1] / 'tools/bt2018_mechs/data/retarget.json').read_text())
    locks = recipe.get(Path(target_path).stem, {}).get('bind_lock', [])
    parent = parents(tgt)
    tgt_names_rev = {i: n.name for i, n in enumerate(tgt.nodes)}

    n_chan = 0
    n_keys = 0
    missing = []
    bone_report = {}
    for anim in tgt.animations:
        if not anim.name.startswith('atlas_'):
            continue
        rotation_channels = [c for c in anim.channels if c.target.path == 'rotation']
        def times(ch):
            acc = tgt.accessors[anim.samplers[ch.sampler].input]
            view = tgt.bufferViews[acc.bufferView]
            return np.frombuffer(blob, '<f4', acc.count, (view.byteOffset or 0) + (acc.byteOffset or 0))
        grid = np.unique(np.concatenate([times(c) for c in rotation_channels]))
        local = [np.broadcast_to(q, (len(grid), 4)) for q in atlas_bind]
        for ch in rotation_channels:
            bone = tgt.nodes[ch.target.node].name
            if bone not in atl_names:
                continue
            keys, _, _ = accessor_view(tgt, blob, anim.samplers[ch.sampler].output)
            keys = keys.copy()
            for k in range(1, len(keys)):
                if np.dot(keys[k-1], keys[k]) < 0: keys[k] *= -1
            q = np.stack([np.interp(grid, times(ch), keys[:, k]) for k in range(4)], axis=-1)
            local[atl_names[bone]] = q / np.linalg.norm(q, axis=-1, keepdims=True)
        source_world = globals_for(atl, local)
        posed = {}
        def target_pose(i):
            if i not in posed:
                bone = tgt.nodes[i].name
                inherited = target_pose(parent[i]) if i in parent else np.array([0., 0., 0., 1.])
                posed[i] = (np.broadcast_to(target_world[i], (len(grid), 4)) if bone in locks else
                    quat_mul(quat_mul(source_world[atl_names[bone]], quat_conj(atlas_world[atl_names[bone]])), target_world[i])
                    if bone in atl_names else quat_mul(inherited, target_bind[i]))
            return posed[i]
        for ch in anim.channels:
            if ch.target.path != "rotation":
                continue
            node_idx = ch.target.node
            bone = tgt_names_rev.get(node_idx, "")
            if bone not in atl_names:
                missing.append(bone)
                continue
            sampler = anim.samplers[ch.sampler]
            data, base, count = accessor_view(tgt, blob, sampler.output)
            inherited = target_pose(parent[node_idx]) if node_idx in parent else np.array([0., 0., 0., 1.])
            new = quat_mul(quat_conj(inherited), target_pose(node_idx))[np.searchsorted(grid, times(ch))]
            new /= np.linalg.norm(new, axis=-1, keepdims=True)
            # write back into blob
            raw = new.astype("<f4").tobytes()
            blob[base:base + len(raw)] = raw
            n_chan += 1
            n_keys += count
            if bone not in bone_report:
                # angle between first key before/after, for sanity
                bone_report[bone] = count

    # Native family clips win over Atlas compatibility names, including mission bindings.
    for anim in list(tgt.animations):
        if not anim.name.startswith('atlas_') and '_moveCore' in anim.name:
            alias = 'atlas_' + anim.name.split('_', 1)[1]
            tgt.animations = [a for a in tgt.animations if a.name != alias]
            native = copy.deepcopy(anim)
            native.name = alias
            tgt.animations.append(native)
    tgt.set_binary_blob(bytes(blob))
    tgt.save(out_path)
    print(f"[retarget] {out_path}: channels={n_chan} keys={n_keys} bones={len(bone_report)}")
    if missing:
        print(f"[retarget] WARN no atlas bind for (left as-is): {sorted(set(missing))}")
    return n_chan, n_keys


if __name__ == "__main__":
    retarget_file(sys.argv[1], sys.argv[2], sys.argv[3])
