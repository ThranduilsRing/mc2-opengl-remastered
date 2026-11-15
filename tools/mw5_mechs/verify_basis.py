#!/usr/bin/env python3
"""Offline numeric gates. Inputs stay outside git; stdout contains numbers/names only.

Trigger paths: tools/mw5_mechs/{build_glb,ue_basis,verify_basis}.py.
Stand is an animated pose, not a bind pose: its FK difference is measured,
not required to be zero. Root track ranges describe full PSA motion even
though the current engine conversion deliberately emits rotations only.
"""
import argparse
import json
from pathlib import Path

import numpy as np

if __package__:
    from .build_glb import CLIPS, read_accessor, read_psa, read_psk, trs_mat
    from .ue_basis import psa_pos_to_gltf, psa_quat_to_gltf
else:
    from build_glb import CLIPS, read_accessor, read_psa, read_psk, trs_mat
    from ue_basis import psa_pos_to_gltf, psa_quat_to_gltf


def angle(a, b):
    a, b = np.asarray(a), np.asarray(b)
    dot = np.sum(a * b, axis=-1) / (np.linalg.norm(a, axis=-1) * np.linalg.norm(b, axis=-1))
    return np.degrees(2 * np.arccos(np.clip(np.abs(dot), 0, 1)))


def fk(nodes, pose=None):
    parents = {c: i for i, n in enumerate(nodes) for c in n.get("children", [])}
    cache = {}

    def visit(i):
        if i not in cache:
            local = trs_mat((pose or {}).get(i, nodes[i]))
            cache[i] = visit(parents[i]) @ local if i in parents else local
        return cache[i]

    return np.array([visit(i) for i in range(len(nodes))])


def verify(skm, psk, anim_dir, expected_bones=121):
    g = json.loads(Path(skm).read_text())
    nodes = g["nodes"]
    joints = g["skins"][0]["joints"]
    by_name = {nodes[i]["name"]: i for i in joints}
    parents = {c: i for i, n in enumerate(nodes) for c in n.get("children", [])}
    roots = {i for i in joints if parents.get(i) not in joints}
    rest = fk(nodes)
    errors = []
    buf = (Path(skm).parent / g["buffers"][0]["uri"]).read_bytes()
    ibms = read_accessor(g, buf, g["skins"][0]["inverseBindMatrices"])
    drift = max(float(np.max(np.abs(np.linalg.inv(rest[i]) - m.reshape(4, 4).T))) for i, m in zip(joints, ibms))
    print(f"ibm_drift {drift:.9f}")
    if not np.isfinite(drift) or drift >= 1e-4:
        errors.append("ibm_drift")
    if psk:
        bones = read_psk(psk)
        if len(bones) != expected_bones or {b["name"] for b in bones} != set(by_name):
            raise ValueError("PSK/glTF bone set/count mismatch")
        print("bone rotation_deg translation_m alternate_w_deg")
        for b in bones:
            i = by_name[b["name"]]
            is_root = i in roots
            if not is_root and by_name[bones[b["parent"]]["name"]] != parents.get(i):
                errors.append(b["name"] + "_parent")
            q = psa_quat_to_gltf(b["quat"], is_root)
            rot = float(angle(q, nodes[i].get("rotation", [0, 0, 0, 1])))
            alt = float(angle(psa_quat_to_gltf(b["quat"], not is_root), nodes[i].get("rotation", [0, 0, 0, 1])))
            pos = float(np.linalg.norm(psa_pos_to_gltf(b["pos"]) - nodes[i].get("translation", [0, 0, 0])))
            print(f"{b['name']} {rot:.9f} {pos:.9f} {alt:.9f}")
            if not np.isfinite(rot + pos) or rot >= .05 or pos >= 1e-4:
                errors.append(b["name"])
            if is_root:
                print(f"root_sign_identifiable {int(abs(rot - alt) > .05)}")
        if errors:
            raise ValueError("reference residual failure: " + ",".join(errors))
    else:
        print("root_rule_unproven 1")

    def poses(names, keys):
        if not set(by_name).issubset(names) or len(names) != len(set(names)):
            raise ValueError("PSA/glTF bone set mismatch")
        for frame in keys:
            yield {by_name[name]: {"rotation": psa_quat_to_gltf(k[3:7], by_name[name] in roots),
                                   "translation": psa_pos_to_gltf(k[:3])} for name, k in zip(names, frame) if name in by_name}

    stand = Path(anim_dir) / "TBR_Stand_ANI.psa"
    names, keys, _, _ = read_psa(stand)
    stand_fk = fk(nodes, next(poses(names, keys)))
    delta = float(np.max(np.linalg.norm(stand_fk[joints, :3, 3] - rest[joints, :3, 3], axis=1)))
    print(f"Stand_fk_max_delta_m {delta:.9f}")
    if not np.isfinite(delta):
        errors.append("Stand_fk")
    if not psk:
        for name, k in zip(names, keys[0]):
            if name not in by_name:
                continue
            i = by_name[name]
            r = float(angle(psa_quat_to_gltf(k[3:7], i in roots), nodes[i].get("rotation", [0, 0, 0, 1])))
            t = float(np.linalg.norm(psa_pos_to_gltf(k[:3]) - nodes[i].get("translation", [0, 0, 0])))
            print(f"{name} {r:.9f} {t:.9f}")
            if r >= .05 or t >= 1e-4:
                errors.append(name)

    pelvis, cockpit = by_name["Pelvis"], by_name["Cockpit"]
    left, right = by_name["Foot0_Left"], by_name["Foot0_Right"]
    forward = stand_fk[cockpit, :3, 3] - stand_fk[pelvis, :3, 3]
    forward[1] = 0
    forward /= np.linalg.norm(forward)
    left_axis = np.cross([0, 1, 0], forward)
    side = float((stand_fk[left, :3, 3] - stand_fk[right, :3, 3]) @ left_axis)
    print(f"Foot0_Left_side_m {side:.9f}")
    if not np.isfinite(side) or side <= 0:
        errors.append("Foot0_Left_mirror")

    print("clip root_rotation_deg root_translation_m root_motion")
    for clip in CLIPS:
        names, keys, _, nf = read_psa(Path(anim_dir) / f"TBR_{clip}_ANI.psa")
        for root in roots:
            track = keys[:, names.index(nodes[root]["name"])]
            q = psa_quat_to_gltf(track[:, 3:7], True)
            # Pairwise angular diameter, sign-insensitive; translation AABB diagonal.
            r = max(float(np.max(angle(a, q))) for a in q)
            t = float(np.linalg.norm(np.ptp(psa_pos_to_gltf(track[:, :3]), axis=0)))
            print(f"{clip} {r:.9f} {t:.9f} {int(r > .05 or t > 1e-4)}")
        if clip == "WalkForward_Straight":
            walk = np.array([fk(nodes, pose) for pose in poses(names, keys)])
            feet = (walk[:, :, :3, 3][:, [left, right]] - walk[:, pelvis, :3, 3][:, None]) @ forward
            ranges = np.ptp(feet, axis=0)
            corr = float(np.corrcoef(feet.T)[0, 1])
            ratio = float(min(ranges) / max(ranges)) if max(ranges) else 0
            # Best cyclic correlation lag in cycles: antiphase must be near half a cycle.
            lag = int(np.argmax([np.corrcoef(feet[:, 0], np.roll(feet[:, 1], k))[0, 1] for k in range(nf)])) / nf
            print(f"Walk_foot_ranges_m {ranges[0]:.9f} {ranges[1]:.9f}")
            print(f"Walk_range_ratio_correlation_phase {ratio:.9f} {corr:.9f} {lag:.9f}")
            if not np.isfinite(ratio + corr + lag) or ratio < .9 or corr >= -.8 or not .4 <= lag <= .6:
                errors.append("Walk_symmetry_antiphase")
    if errors:
        raise ValueError("basis gates: " + ",".join(errors))


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--skm", required=True)
    ap.add_argument("--psk")
    ap.add_argument("--anim-dir", required=True)
    args = ap.parse_args()
    verify(args.skm, args.psk, args.anim_dir)
