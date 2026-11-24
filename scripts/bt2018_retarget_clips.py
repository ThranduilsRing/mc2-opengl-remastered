#!/usr/bin/env python3
"""Retarget Atlas clips through model-space bind frames, preserving native clips.
Atlas keys come from the Atlas reference GLB, so the step is idempotent.

Usage:
  py -3 bt2018_retarget_clips.py <target.glb> <atlas_ref.glb> <out.glb>
"""
import os
import sys
import copy
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


ARM_CAP_LO, ARM_CAP_HI, ARM_CAP_MARGIN = -50.0, -10.0, 10.0
POD_LATERAL_RATIO, POD_MAX_ELEV = 1.5, 30.0
# Ready-pose match against the chassis' own BT2018 bind: arm joint offsets from the torso (fraction of
# chassis height) and arm joint rotation relative to the torso (degrees). error 1.0 = at the tolerance.
POSE_POS_TOL, POSE_ROT_TOL = 0.04, 25.0
POSE_BLENDS = (0.0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0)


def quat_mat(q):
    x, y, z, w = q
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                     [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                     [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])


def elevation(v):
    return np.degrees(np.arctan2(v[1], np.hypot(v[0], v[2])))


def _positions(tgt, parent, wq, trans=None):
    cache = {}
    def m(i):
        if i not in cache:
            n = tgt.nodes[i]
            loc = quat_mul(quat_conj(wq[parent[i]]), wq[i]) if i in parent else wq[i]
            t = np.eye(4)
            t[:3, :3] = quat_mat(loc) * np.array(n.scale or [1, 1, 1])
            t[:3, 3] = (trans or {}).get(i, n.translation or [0, 0, 0])
            cache[i] = (m(parent[i]) if i in parent else np.eye(4)) @ t
        return cache[i]
    return lambda i: m(i)[:3, 3]


def pod_arm(tgt, parent, bind_world, side):
    """Upper-arm node index when the 'arm' is a shoulder-mounted pod or launcher (Catapult): its bind
    hand points out sideways at a level bind. The BT2018 ready pose holds it upright beside the torso
    (owner 11-23 ref), so every Atlas clip leaves that subtree at bind. Else None."""
    names = name_to_index(tgt)
    ua, hand = names.get(f"j_{side}UpperArm"), names.get(f"j_{side}Hand")
    if ua is None or hand is None:
        return None
    pb = _positions(tgt, parent, bind_world)
    v = pb(hand) - pb(ua)
    if abs(v[0]) > POD_LATERAL_RATIO * abs(v[2]) and abs(elevation(v)) < POD_MAX_ELEV:
        return ua
    return None


def slerp(a, b, t):
    """Quaternion slerp, broadcasting over leading axes (a or b may be per-key arrays)."""
    a, b = np.broadcast_arrays(np.asarray(a, float), np.asarray(b, float))
    d = np.sum(a * b, axis=-1, keepdims=True)
    b = np.where(d < 0, -b, b)
    d = np.abs(d)
    th = np.arccos(np.clip(d, -1.0, 1.0))
    sn = np.sin(th)
    lin = d > 0.9995
    wa = np.where(lin, 1 - t, np.sin((1 - t) * th) / np.where(lin, 1, sn))
    wb = np.where(lin, t, np.sin(t * th) / np.where(lin, 1, sn))
    q = wa * a + wb * b
    return q / np.linalg.norm(q, axis=-1, keepdims=True)


def pose_error(tgt, parent, bind_world, idle_world, idle_trans=None):
    """Ready-pose error of idle_world against the chassis' own BT2018 bind (bind_world): per arm joint
    (clavicle, upper arm, forearm, hand/pod) the offset from the torso, as a fraction of chassis height,
    and the rotation relative to the torso. Returns (error, worst_joint); 1.0 = at POSE_*_TOL."""
    names = name_to_index(tgt)
    pb = _positions(tgt, parent, bind_world)
    pi = _positions(tgt, parent, idle_world, idle_trans)
    ys = [pb(i)[1] for i in range(len(tgt.nodes))]
    height = max(max(ys) - min(ys), 1e-9)
    worst, who = 0.0, ""
    for side in "LR":
        ua = names.get(f"j_{side}UpperArm")
        if ua is None:
            continue
        clav = parent.get(ua)
        torso = parent.get(clav) if clav is not None and "Clavicle" in tgt.nodes[clav].name else clav
        if torso is None:
            continue
        joints = [j for j in (clav if "Clavicle" in tgt.nodes[clav].name else None, ua, names.get(f"j_{side}Forearm"), names.get(f"j_{side}Hand")) if j is not None]
        rb, ri = quat_mat(bind_world[torso]).T, quat_mat(idle_world[torso]).T
        for j in joints:
            dp = np.linalg.norm(ri @ (pi(j) - pi(torso)) - rb @ (pb(j) - pb(torso))) / height
            qb = quat_mul(quat_conj(bind_world[torso]), bind_world[j])
            qi = quat_mul(quat_conj(idle_world[torso]), idle_world[j])
            ang = np.degrees(2 * np.arccos(min(1.0, abs(float(np.dot(qb, qi))))))
            e = max(dp / POSE_POS_TOL, ang / POSE_ROT_TOL)
            if e > worst:
                worst, who = e, f"{tgt.nodes[j].name} pos {dp:.3f}H rot {ang:.0f}deg"
    return worst, who


def pose_table(rows):
    """rows: (chassis, error_before, error_after, iterations, converged, blend)."""
    out = [f"{'chassis':14s} {'err_before':>10s} {'err_after':>9s} {'iters':>5s} {'blend':>5s} converged"]
    out += [f"{n:14s} {b:10.2f} {a:9.2f} {i:5d} {bl:5.2f} {'yes' if c else 'NO'}" for n, b, a, i, c, bl in rows]
    return "\n".join(out)


def arm_lift_fix(tgt, parent, bind_world, idle_world, side):
    """World rotation about the shoulder that stops the Atlas ready-idle lifting a hand above a
    natural ready pose. The Atlas delta raises forearms ~55 deg, so chassis whose bind arms are
    level or raised end with hands above the shoulder. Cap the idle hand elevation (hand - shoulder)
    at the chassis' bind elevation + margin, clamped to [-50, -10] deg. One constant rotation,
    applied to the whole arm subtree of every Atlas clip, so walk/run swing is kept.
    bind_world/idle_world are per-node world quaternions. Returns (upper_idx, quat) or None."""
    names = name_to_index(tgt)
    ua, hand = names.get(f"j_{side}UpperArm"), names.get(f"j_{side}Hand")
    if ua is None or hand is None or pod_arm(tgt, parent, bind_world, side) is not None:
        return None
    pb, pi = _positions(tgt, parent, bind_world), _positions(tgt, parent, idle_world)
    v_bind, v = pb(hand) - pb(ua), pi(hand) - pi(ua)
    hz = np.array([v[0], 0.0, v[2]])
    if np.linalg.norm(hz) < 1e-9:
        return None
    cap = float(np.clip(elevation(v_bind) + ARM_CAP_MARGIN, ARM_CAP_LO, ARM_CAP_HI))
    delta = min(elevation(v), cap) - elevation(v)
    if delta > -1e-6:
        return None
    axis = np.cross([0.0, 1.0, 0.0], hz / np.linalg.norm(hz))
    axis /= np.linalg.norm(axis)
    # a positive turn about (up x horizontal-dir) lowers the arm, so turn by -delta
    half = np.radians(-delta) / 2
    print(f"[retarget] {side} arm idle hand elev {elevation(v):.0f} -> cap {cap:.0f} (bind {elevation(v_bind):.0f})")
    return ua, np.append(axis * np.sin(half), np.cos(half))


def retarget_file(target_path, atlas_path, out_path, own_clips=None):
    """Bake own clips when supplied, otherwise retarget Atlas through the bind frames.
    Write out_path; return (rotation channels, rotation keys)."""
    if own_clips:
        from tools.bt2018_import.bake_clips import add_to_glb
        retarget_file.last_report = None
        add_to_glb(target_path, out_path, own_clips)
        return (sum(len(rots) for _, rots in own_clips.values()),
                sum(len(q) for _, rots in own_clips.values() for q in rots.values()))
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
    parent = parents(tgt)
    tgt_names_rev = {i: n.name for i, n in enumerate(tgt.nodes)}
    atl_blob = atl.binary_blob()
    atl_anims = {a.name: a for a in atl.animations}

    def children_of(i):
        out = [i]
        for c in tgt.nodes[i].children or []:
            out += children_of(c)
        return out

    hold = set()                      # pod subtrees stay at bind rotation in every Atlas clip
    keep_t = set()                    # clavicle + arm subtrees keep their own bind translation (Atlas keys are Atlas-sized)
    for side in "LR":
        ua = name_to_index(tgt).get(f"j_{side}UpperArm")
        if ua is None:
            continue
        clav = parent.get(ua)
        root = clav if clav is not None and "Clavicle" in tgt.nodes[clav].name else ua
        keep_t.update(children_of(root))
        if pod_arm(tgt, parent, target_world, side) is not None:
            hold.update(children_of(root))

    def make_pose(source_world, fix, blend=0.0):
        posed = {}
        def target_pose(i):
            # Model space: the target bone turns in the world exactly as the Atlas bone turns
            # away from its own bind (Wt = Wa * conj(WaBind) * WtBind). A local-frame copy
            # rotates arms about the wrong axes wherever clavicle/arm bind frames differ.
            if i not in posed:
                bone = tgt.nodes[i].name
                inherited = target_pose(parent[i]) if i in parent else np.array([0., 0., 0., 1.])
                posed[i] = (quat_mul(quat_mul(source_world[atl_names[bone]], quat_conj(atlas_world[atl_names[bone]])), target_world[i])
                    if bone in atl_names and i not in hold else quat_mul(inherited, target_bind[i]))
                if i in fix:
                    posed[i] = quat_mul(fix[i], posed[i])
                if blend and i in keep_t and i not in hold:
                    posed[i] = slerp(posed[i], quat_mul(inherited, target_bind[i]), blend)
            return posed[i]
        return target_pose

    # Arm lift fix from the Atlas ready-idle (first key), shared by every Atlas clip, then a closed
    # loop over the arm blend toward the chassis' own bind until the ready pose matches it.
    arm_fix = {}
    arm_blend = 0.0
    report = None
    idle = atl_anims.get("atlas_moveCoreIdle")
    if idle is not None:
        loc0 = list(atlas_bind)
        for c in idle.channels:
            if c.target.path == "rotation":
                loc0[c.target.node] = accessor_view(atl, atl_blob, idle.samplers[c.sampler].output)[0][0].astype(np.float64)
        idle_src = globals_for(atl, loc0)
        pose0 = make_pose(idle_src, {})
        idle_world = [pose0(i) for i in range(len(tgt.nodes))]
        # error before = the old behaviour: Atlas idle rotations and the clip's Atlas-sized translation keys
        old_idle = next((a for a in tgt.animations if a.name == "atlas_moveCoreIdle"), None)
        old_trans = {}
        for c in (old_idle.channels if old_idle else []):
            if c.target.path == "translation" and c.target.node in keep_t:
                tacc = tgt.accessors[old_idle.samplers[c.sampler].output]
                off = (tgt.bufferViews[tacc.bufferView].byteOffset or 0) + (tacc.byteOffset or 0)
                old_trans[c.target.node] = np.frombuffer(bytes(blob), "<f4", 3, off).astype(np.float64)
        err_before = pose_error(tgt, parent, target_world, idle_world, old_trans)[0]
        for side in "LR":
            r = arm_lift_fix(tgt, parent, target_world, idle_world, side)
            if r:
                for i in children_of(r[0]):
                    arm_fix[i] = r[1]
        for it, b in enumerate(POSE_BLENDS, 1):
            tp = make_pose(idle_src, arm_fix, b)
            err_after, worst = pose_error(tgt, parent, target_world, [tp(i) for i in range(len(tgt.nodes))])
            arm_blend = b
            if os.environ.get("BT2018_POSE_TRACE"):
                print(f"[pose-trace] blend {b:.2f} error {err_after:.2f} ({worst})")
            if err_after <= 1.0:
                break
        report = dict(error_before=err_before, error_after=err_after, iterations=it, blend=arm_blend,
                      converged=err_after <= 1.0, worst=worst)
        print(f"[pose] {os.path.basename(out_path)}: error {err_before:.2f} -> {err_after:.2f} blend {arm_blend:.2f} "
              f"iters {it} converged={'yes' if report['converged'] else 'NO'} ({worst})")
    retarget_file.last_report = report

    n_chan = 0
    n_keys = 0
    missing = []
    bone_report = {}
    for anim in tgt.animations:
        # Native family clips are authored for this skeleton; Atlas clips are re-derived from
        # atlas.glb's own keys, so re-running on an already retargeted GLB gives the same bytes.
        src = atl_anims.get(anim.name)
        if not anim.name.startswith('atlas_') or src is None:
            continue
        def times(g, b, a, ch):
            acc = g.accessors[a.samplers[ch.sampler].input]
            view = g.bufferViews[acc.bufferView]
            return np.frombuffer(b, '<f4', acc.count, (view.byteOffset or 0) + (acc.byteOffset or 0))
        src_rot = [c for c in src.channels if c.target.path == 'rotation']
        grid = np.unique(np.concatenate([times(atl, atl_blob, src, c) for c in src_rot]))
        local = [np.broadcast_to(q, (len(grid), 4)) for q in atlas_bind]
        for ch in src_rot:
            keys = accessor_view(atl, atl_blob, src.samplers[ch.sampler].output)[0].astype(np.float64)
            for k in range(1, len(keys)):
                if np.dot(keys[k-1], keys[k]) < 0: keys[k] *= -1
            q = np.stack([np.interp(grid, times(atl, atl_blob, src, ch), keys[:, k]) for k in range(4)], axis=-1)
            local[ch.target.node] = q / np.linalg.norm(q, axis=-1, keepdims=True)
        target_pose = make_pose(globals_for(atl, local), arm_fix, arm_blend)
        for ch in anim.channels:
            if ch.target.path == "translation" and ch.target.node in keep_t:
                # the clip's translation keys are Atlas-sized; a held pod keeps its own bind offset
                tacc = tgt.accessors[anim.samplers[ch.sampler].output]
                tbase = (tgt.bufferViews[tacc.bufferView].byteOffset or 0) + (tacc.byteOffset or 0)
                tcount = tacc.count
                raw = np.tile(np.array(tgt.nodes[ch.target.node].translation or [0, 0, 0], "<f4"), (tcount, 1)).tobytes()
                blob[tbase:tbase + len(raw)] = raw
                continue
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
            at = np.clip(np.searchsorted(grid, times(tgt, blob, anim, ch) - 1e-6), 0, len(grid) - 1)
            new = quat_mul(quat_conj(inherited), target_pose(node_idx))[at]
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
