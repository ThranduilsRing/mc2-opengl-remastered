"""P10: muzzle sockets for the pack model.

The BT model has joints, not muzzles: firing from a forearm or clavicle joint centre puts the
bolt inside the arm. For every weapon socket a chassis names, this finds the geometry skinned
to the socket's joint (intact parts only) and takes its most forward rest-pose vertex -- the
barrel tip for arm guns, the leading edge of the torso mount -- then stores it in that joint's
LOCAL frame. The engine evaluates actor x posed joint x this offset (assimp_importer.cpp
GetImportedNodeWorld), so the muzzle follows the same pose as the visible model and shadow.

Forward is the model's +Z (glTF), up is +Y. Torso sockets pick the most forward vertex in
the upper half of their joint's geometry (dorsal/shoulder mounts), arm sockets the most
forward overall.
"""
from __future__ import annotations

from pathlib import Path

import numpy as np

from . import render_icons as R


def load_skinned(glb: Path):
    """joint name -> (joint world rest 4x4, intact vertices skinned to it in world space)."""
    from pygltflib import GLTF2
    g = GLTF2().load(str(glb))
    blob = g.binary_blob()
    parent = {c: i for i, n in enumerate(g.nodes) for c in (n.children or [])}
    world = {}

    def wm(i):
        if i not in world:
            world[i] = (wm(parent[i]) if i in parent else np.eye(4)) @ R._local(g.nodes[i])
        return world[i]

    out = {}
    for node in g.nodes:
        if node.mesh is None or node.skin is None:
            continue
        if R.region_of(node.name or "") is None:
            continue
        skin = g.skins[node.skin]
        ibm = R._accessor(g, blob, skin.inverseBindMatrices).reshape(-1, 4, 4).transpose(0, 2, 1)
        for prim in g.meshes[node.mesh].primitives:
            if prim.attributes.JOINTS_0 is None:
                continue
            pos = R._accessor(g, blob, prim.attributes.POSITION).astype(np.float64)
            j0 = R._accessor(g, blob, prim.attributes.JOINTS_0)[:, 0].astype(int)
            for j in np.unique(j0):
                jn = skin.joints[j]
                name = g.nodes[jn].name
                m = wm(jn) @ ibm[j]
                v = (np.c_[pos[j0 == j], np.ones(int((j0 == j).sum()))] @ m.T)[:, :3]
                prev = out.get(name, (wm(jn), np.zeros((0, 3))))
                out[name] = (wm(jn), np.vstack([prev[1], v]))
    return out


def socket_offsets(glb: Path, socket_joints: dict[str, str]) -> dict[str, list[float]]:
    """socket name -> [x, y, z] in its joint's local frame."""
    skinned = load_skinned(glb)
    out = {}
    for socket, joint in sorted(socket_joints.items()):
        if joint not in skinned or len(skinned[joint][1]) == 0:
            continue
        jw, verts = skinned[joint]
        cand = verts
        mid = np.median(verts[:, 1])
        if socket[-1:].isdigit():
            # a second mount on the same joint: the leading edge of the LOWER half (under-barrel)
            lower = verts[verts[:, 1] < mid]
            cand = lower if len(lower) else verts
        elif "torso" in socket:
            upper = verts[verts[:, 1] >= mid]
            cand = upper if len(upper) else verts
        tip = cand[np.argmax(cand[:, 2])]
        local = np.linalg.inv(jw) @ np.r_[tip, 1.0]
        out[socket] = [round(float(c), 4) for c in local[:3]]
    return out
