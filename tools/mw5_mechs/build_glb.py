#!/usr/bin/env python3
"""Convert a umodel-exported MechWarrior 5 mech (skeletal mesh glTF + PSA clips) into the rigid-part skinned GLB
the engine's mech importer takes (one mesh per bone, one single-joint skin per mesh, atlas_* clip names).

Code only: inputs and outputs are the owner's own extracted files and stay outside git.

  py -3 tools/mw5_mechs/build_glb.py --skm Timberwolf_SKM.gltf --anim-dir <dir with *.psa> --out madcat.glb \
      --image mw5_madcat_alb.png [--scale 1.0]
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path

import numpy as np

if __package__:
    from .ue_basis import psa_quat_to_gltf
else:
    from ue_basis import psa_quat_to_gltf

# Rest FK: Thigh -> Calf -> Foot0 is 3.483 m + 5.176 m on both sides;
# Knee and Calf share a pivot (within 0.2 micrometres). Preserve foot descendants.
RENAME = {"Thigh_Left": "j_LThigh", "Calf_Left": "j_LCalf", "Foot0_Left": "j_LFoot",
          "Thigh_Right": "j_RThigh", "Calf_Right": "j_RCalf", "Foot0_Right": "j_RFoot",
          "Torso_Pitch": "j_Spine2", "Cockpit": "j_" + "COCKPIT"}
KEEP_ALWAYS ={"Root", "Pelvis", "Torso_Twist", "Torso_Animation", "Torso_Pitch", "Cockpit"}
# MW5 clip (TimberWolf/Animations/TBR_<name>_ANI) -> the engine's hard-coded clip name (mclib/mech_clip_state.h)
CLIPS = {
    "Stand": "atlas_" + "moveCoreIdle", "WalkForward_Straight": "atlas_" + "moveCoreWalkFwd",
    "RunForward_Straight": "atlas_" + "moveCoreRunFwd", "WalkBackward_Straight": "atlas_" + "moveCoreWalkBwd",
    "WalkForward_LeftLimp": "atlas_" + "moveCoreWalkLimpFwdLeft", "WalkForward_RightLimp": "atlas_" + "moveCoreWalkLimpFwdRight",
    "Stand_Turn_Left": "atlas_" + "moveCoreTurnLeftIdle", "Stand_Turn_Right": "atlas_" + "moveCoreTurnRightIdle",
    "Jumpjetting_Neutral": "atlas_moveJumpUpIdle",
}
# Reverse selects WalkBwd; the engine has no RunBackward slot. Ragdoll/additive
# gestures have no full-body source: holds prevent continuing the previous loop.
# Destination -> (MW5 source, sampled frame); these are poses, not transitions.
HOLD_CLIPS = {
    **{"atlas_" + name: ("Falling", -1) for name in (
        "hitReactKnockdownInpl", "downedIdle", "deathKnockdown", "deathIdle")},
    **{"atlas_" + name: ("Stand", 0) for name in (
        "downedGetup", "hitReactLgtFwd", "hitReactLgtBwd", "hitReactLgtLeft", "hitReactLgtRight")},
    "atlas_" + "moveJumpUpStart": ("Crouch", 0),
    "atlas_" + "moveJumpDownLand": ("Crouch", -1),
}
NODE_MAP = {"joint_torso": "j_Spine2", "weapon_rightarm": "Forearm_Right", "weapon_leftarm": "Forearm_Left",
            "weapon_righttorso": "Clavicle_Right", "weapon_lefttorso": "Clavicle_Left", "weapon_centertorso": "j_Spine2",
            "cockpit": "j_" + "COCKPIT", "hit_left": "Clavicle_Left", "hit_right": "Clavicle_Right"}


def quat_to_mat(q):
    x, y, z, w = q
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), 0],
                     [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w), 0],
                     [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y), 0],
                     [0, 0, 0, 1]])


def trs_mat(n):
    m = quat_to_mat(n.get("rotation", [0, 0, 0, 1]))
    m[:3, 3] = n.get("translation", [0, 0, 0])
    return m


def read_accessor(g, buf, i):
    a = g["accessors"][i]
    bv = g["bufferViews"][a["bufferView"]]
    n = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}[a["type"]]
    dt = {5126: "<f4", 5123: "<u2", 5121: "u1", 5125: "<u4"}[a["componentType"]]
    off = bv.get("byteOffset", 0) + a.get("byteOffset", 0)
    arr = np.frombuffer(buf, dtype=dt, count=a["count"] * n, offset=off)
    return arr.reshape(a["count"], n) if n > 1 else arr


def actorx_chunks(path):
    d = Path(path).read_bytes()
    off, ch = 0, {}
    while off < len(d):
        if len(d) - off < 32:
            raise ValueError("truncated ActorX chunk header")
        name, _flags, size, cnt = struct.unpack_from("<20sIII", d, off)
        off += 32
        ch[name.split(b"\0")[0].decode()] = (off, size, cnt)
        off += size * cnt
        if off > len(d):
            raise ValueError("truncated ActorX chunk data")
    return d, ch


def read_psk(path):
    """Read REFSKELT (FNamedBoneBinary): name, parent, xyzw, centimetres."""
    d, ch = actorx_chunks(path)
    o, size, cnt = ch["REFSKELT"]
    if size != 120:
        raise ValueError(f"unsupported REFSKELT record size {size}")
    bones = []
    for i in range(cnt):
        name, _flags, _children, parent, *pose = struct.unpack_from("<64sIii4f3ff3f", d, o + i * size)
        bones.append({"name": name.split(b"\0")[0].decode(), "parent": parent,
                      "quat": pose[:4], "pos": pose[4:7]})
    return bones


def read_psa(path):
    d, ch = actorx_chunks(path)
    o, size, cnt = ch["BONENAMES"]
    names = [struct.unpack_from("<64s", d, o + i * size)[0].split(b"\0")[0].decode() for i in range(cnt)]
    o, _, _ = ch["ANIMINFO"]
    _n, _g, nb, _ri, _ks, _kq, _kr, _tt, rate, _sb, _fr, nframes = struct.unpack_from("<64s64siiiifffiii", d, o)
    o, size, cnt = ch["ANIMKEYS"]
    if size != 32 or cnt != nframes * nb or nb != len(names) or rate <= 0:
        raise ValueError("inconsistent PSA animation dimensions/rate")
    keys = np.frombuffer(d, dtype="<f4", count=cnt * 8, offset=o).reshape(nframes, nb, 8)
    return names, keys, float(rate), nframes


def build(skm_path, anim_dir, out_path, image_name, clips=None):
    skm = Path(skm_path)
    g = json.loads(skm.read_text())
    buf = (skm.parent / g["buffers"][0]["uri"]).read_bytes()
    nodes = g["nodes"]
    skin = g["skins"][0]
    joint_nodes = skin["joints"]
    parent = {}
    for i, n in enumerate(nodes):
        for c in n.get("children", []):
            parent[c] = i
    glob = {}

    def gmat(i):
        if i not in glob:
            m = trs_mat(nodes[i])
            glob[i] = (gmat(parent[i]) @ m) if i in parent else m
        return glob[i]

    # rigid parts: every triangle goes to the dominant bone of its first vertex
    parts = {}
    total_triangles, mixed_triangles, pairs = 0, 0, {}
    for mesh in g["meshes"]:
        for pr in mesh["primitives"]:
            at = pr["attributes"]
            pos = read_accessor(g, buf, at["POSITION"]).astype(np.float32)
            nrm = read_accessor(g, buf, at["NORMAL"]).astype(np.float32)
            uv = read_accessor(g, buf, at["TEXCOORD_0"]).astype(np.float32)
            J = read_accessor(g, buf, at["JOINTS_0"]).astype(int)
            W = read_accessor(g, buf, at["WEIGHTS_0"])
            dom = J[np.arange(len(J)), np.argmax(W, axis=1)]
            idx = read_accessor(g, buf, pr["indices"]).astype(np.int64).reshape(-1, 3)
            tb = dom[idx[:, 0]]
            total_triangles += len(idx)
            for corners in dom[idx]:
                bones = sorted(set(map(int, corners)))
                if len(bones) > 1:
                    mixed_triangles += 1
                    for a in range(len(bones)):
                        for c in range(a + 1, len(bones)):
                            pair = (bones[a], bones[c])
                            pairs[pair] = pairs.get(pair, 0) + 1
            for b in np.unique(tb):
                tri = idx[tb == b]
                used, inv = np.unique(tri.ravel(), return_inverse=True)
                parts.setdefault(int(b), []).append((pos[used], nrm[used], uv[used], inv.reshape(-1, 3)))

    for (a, b), count in sorted(pairs.items()):
        print(f"mixed_pair {nodes[joint_nodes[a]]['name']} {nodes[joint_nodes[b]]['name']} {count}")
    fraction = mixed_triangles / max(total_triangles, 1)
    print(f"mixed_triangles {mixed_triangles} {total_triangles} {fraction:.8f}")
    if fraction > 0.01:  # rigid split assigns seam triangles to the dominant bone; 0.65% on the Mad Cat is cosmetic (orchestrator 11-16)
        raise ValueError("mixed dominant-bone triangles exceed 1%")

    keep = set()
    for b in parts:
        i = joint_nodes[b]
        while i is not None:
            keep.add(i)
            i = parent.get(i)
    for i, n in enumerate(nodes):
        if n["name"] in KEEP_ALWAYS or n["name"] in NODE_MAP.values() or n["name"].startswith(("Forearm_", "Clavicle_", "Upperarm_")):
            j = i
            while j is not None:
                keep.add(j)
                j = parent.get(j)
    keep.discard(0)  # the umodel mesh wrapper node
    order = sorted(keep)
    remap = {old: k for k, old in enumerate(order)}

    def nm(i):
        return RENAME.get(nodes[i]["name"], nodes[i]["name"])

    out_nodes = []
    for old in order:
        n = nodes[old]
        e = {"name": nm(old), "translation": list(map(float, n.get("translation", [0, 0, 0]))),
             "rotation": list(map(float, n.get("rotation", [0, 0, 0, 1])))}
        ch = [remap[c] for c in n.get("children", []) if c in remap]
        if ch:
            e["children"] = ch
        out_nodes.append(e)
        if len(e["name"]) > 24:
            sys.exit("node name over 24 chars: " + e["name"])
    root_nodes = [remap[i] for i in order if i not in parent or parent[i] not in remap]

    blob = bytearray()
    views, accs = [], []

    def add(arr, ctype, typ, target=None, minmax=False):
        while len(blob) % 4:
            blob.append(0)
        arr = np.ascontiguousarray(arr)
        views.append({"buffer": 0, "byteOffset": len(blob), "byteLength": arr.nbytes, **({"target": target} if target else {})})
        blob.extend(arr.tobytes())
        a = {"bufferView": len(views) - 1, "componentType": ctype, "count": int(arr.shape[0]), "type": typ}
        if minmax:
            a["min"], a["max"] = np.atleast_1d(arr.min(axis=0)).tolist(), np.atleast_1d(arr.max(axis=0)).tolist()
        accs.append(a)
        return len(accs) - 1

    meshes, skins, mesh_nodes = [], [], []
    drift = 0.0
    for b, plist in sorted(parts.items()):
        jn = joint_nodes[b]
        ibm = np.linalg.inv(gmat(jn))
        orig = read_accessor(g, buf, skin["inverseBindMatrices"])[b].reshape(4, 4).T
        drift = max(drift, float(np.abs(ibm - orig).max()))
        if not np.isfinite(drift) or drift >= 1e-4:
            raise ValueError(f"ibm_drift {drift:.9g} >= 1e-4")
        p = [np.concatenate([q[k] for q in plist]) for k in range(3)]
        base, tris = 0, []
        for q in plist:
            tris.append(q[3] + base)
            base += len(q[0])
        tris = np.concatenate(tris).astype(np.uint32)
        nv = len(p[0])
        attrs = {"POSITION": add(p[0], 5126, "VEC3", 34962, True), "NORMAL": add(p[1], 5126, "VEC3", 34962),
                 "TEXCOORD_0": add(p[2], 5126, "VEC2", 34962),
                 "JOINTS_0": add(np.zeros((nv, 4), np.uint16), 5123, "VEC4", 34962),
                 "WEIGHTS_0": add(np.tile(np.array([1, 0, 0, 0], np.float32), (nv, 1)), 5126, "VEC4", 34962)}
        mname = nm(jn).replace("_DM0", "")
        meshes.append({"name": mname, "primitives": [{"attributes": attrs, "indices": add(tris.ravel(), 5125, "SCALAR", 34963), "material": 0}]})
        skins.append({"joints": [remap[jn]], "inverseBindMatrices": add(ibm.T.astype(np.float32)[None].reshape(1, 16), 5126, "MAT4")})
        mesh_nodes.append({"name": mname + "_n", "mesh": len(meshes) - 1, "skin": len(skins) - 1})
        if len(mesh_nodes[-1]["name"]) > 24:
            raise ValueError("node name over 24 chars: " + mesh_nodes[-1]["name"])
    first = len(out_nodes)
    out_nodes += mesh_nodes
    root_nodes += list(range(first, len(out_nodes)))

    # Rotation-only clips: shared ActorX basis accounts for the root/child convention.
    name_to_node = {nm(o): remap[o] for o in order}
    anims = []
    clip_specs = [(src, clip, None) for src, clip in CLIPS.items()]
    clip_specs += [(src, clip, frame) for clip, (src, frame) in HOLD_CLIPS.items()]
    for src, clip, hold_frame in clip_specs:
        if clips is not None and src not in clips:
            continue
        f = Path(anim_dir) / f"TBR_{src}_ANI.psa"
        if not f.is_file():
            if hold_frame is not None:
                print(f"fallback_missing {clip} source={f.name}")
            continue
        bones, keys, rate, nf = read_psa(f)
        if hold_frame is not None:
            print(f"fallback_hold {clip} source={f.name} frame={hold_frame % nf}")
            keys = keys[[hold_frame]]
            nf = 1
        t = (np.arange(nf) / rate).astype(np.float32)
        samplers, channels = [], []
        tin = add(t, 5126, "SCALAR", None, True)
        for bi, bname in enumerate(bones):
            node = name_to_node.get(RENAME.get(bname, bname))
            if node is None:
                continue
            old = order[node]
            q = psa_quat_to_gltf(keys[:, bi, 3:7], parent.get(old) not in joint_nodes)
            for k in range(1, nf):
                if np.dot(q[k], q[k - 1]) < 0:
                    q[k] = -q[k]
            samplers.append({"input": tin, "output": add(q.astype(np.float32), 5126, "VEC4"), "interpolation": "LINEAR"})
            channels.append({"sampler": len(samplers) - 1, "target": {"node": node, "path": "rotation"}})
        anims.append({"name": clip, "samplers": samplers, "channels": channels})

    doc = {"asset": {"version": "2.0", "generator": "tools/mw5_mechs/build_glb.py"},
           "scene": 0, "scenes": [{"nodes": root_nodes}], "nodes": out_nodes, "meshes": meshes, "skins": skins,
           "materials": [{"name": "mw5_mech", "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}, "metallicFactor": 0.3, "roughnessFactor": 0.55}}],
           "textures": [{"source": 0}], "images": [{"uri": image_name}],
           "accessors": accs, "bufferViews": views, "buffers": [{"byteLength": len(blob)}], "animations": anims}
    js = json.dumps(doc, separators=(",", ":")).encode()
    js += b" " * (-len(js) % 4)
    blob.extend(b"\0" * (-len(blob) % 4))
    Path(out_path).write_bytes(struct.pack("<4sII", b"glTF", 2, 28 + len(js) + len(blob)) + struct.pack("<I4s", len(js), b"JSON") + js
                               + struct.pack("<I4s", len(blob), b"BIN\0") + bytes(blob))
    print(f"parts={len(meshes)} nodes={len(out_nodes)} clips={[a['name'] for a in anims]} ibm_drift={drift:.5f}")
    return doc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--skm", required=True)
    ap.add_argument("--anim-dir", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--image", required=True)
    ap.add_argument("--clips", default=None, help="comma list of MW5 clip names to include (default all, empty = none)")
    a = ap.parse_args()
    build(a.skm, a.anim_dir, a.out, a.image, None if a.clips is None else [c for c in a.clips.split(",") if c])


if __name__ == "__main__":
    main()
