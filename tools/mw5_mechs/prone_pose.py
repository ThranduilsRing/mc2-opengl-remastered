#!/usr/bin/env python3
"""Give the MW5 Mad Cat pack a real fallen pose.

MW5's `Falling` clip is the airborne dangle (the mech stays upright), so the downed/dead clips built from its last frame
left a destroyed mech standing. This rewrites the four fall clips of a built pack GLB: the airborne pose pitched forward
about the pelvis onto the chest, and for the one-shot clips a stand -> airborne -> prone ramp.

  py -3 tools/mw5_mechs/prone_pose.py in.glb out.glb [--pitch 85]
"""
import argparse
import struct
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_glb import read_accessor  # noqa: E402
from sockets import load_glb  # noqa: E402

HOLD = ("atlas_downedIdle", "atlas_deathIdle")
ONE_SHOT = {"atlas_hitReactKnockdownInpl": 0.9, "atlas_deathKnockdown": 0.9}
PIVOT = "Pelvis"
STAND = "atlas_moveCoreIdle"


def qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return np.array([aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
                     aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz])


def pitch_quat(deg):
    """Nose down (the mech's forward is +X in the pack's Y-up space): rotation about +Z by -deg."""
    h = np.radians(-deg) / 2
    return np.array([0.0, 0.0, np.sin(h), np.cos(h)])


def clip_rotations(doc, buf, name):
    """node -> first-key rotation of a clip (xyzw)."""
    clip = next(a for a in doc["animations"] if a["name"] == name)
    return {c["target"]["node"]: read_accessor(doc, buf, clip["samplers"][c["sampler"]]["output"])[0].astype(np.float64)
            for c in clip["channels"] if c["target"]["path"] == "rotation"}


def build_clips(doc, buf, pitch_deg=85.0):
    """name -> {node: [key rotations]}, times for the four fall clips."""
    pivot = next(i for i, n in enumerate(doc["nodes"]) if n.get("name") == PIVOT)
    air = clip_rotations(doc, buf, "atlas_deathIdle")
    stand = clip_rotations(doc, buf, STAND)
    bind = {i: np.array(n.get("rotation", [0, 0, 0, 1]), np.float64) for i, n in enumerate(doc["nodes"])}
    nodes = sorted(set(air) | set(stand) | {pivot})
    pose = lambda src, node: src.get(node, bind[node])  # noqa: E731
    prone = dict(air)
    prone[pivot] = qmul(pitch_quat(pitch_deg), pose(air, pivot))
    out = {name: ([0.0], {n: [pose(prone, n)] for n in nodes}) for name in HOLD}
    for name, seconds in ONE_SHOT.items():
        out[name] = ([0.0, 0.3 * seconds, seconds],
                     {n: [pose(stand, n), pose(air, n), pose(prone, n)] for n in nodes})
    return out


def rewrite(glb_in, glb_out, pitch_deg=85.0):
    doc, buf = load_glb(glb_in)
    if doc.get("extras", {}).get("pronePitchDeg"):
        raise ValueError("fall clips are already prone: run on the build_pack GLB, not on its own output")
    doc.setdefault("extras", {})["pronePitchDeg"] = pitch_deg
    blob = bytearray(buf)

    def add(arr, typ):
        while len(blob) % 4:
            blob.append(0)
        arr = np.ascontiguousarray(arr, np.float32)
        doc["bufferViews"].append({"buffer": 0, "byteOffset": len(blob), "byteLength": arr.nbytes})
        blob.extend(arr.tobytes())
        acc = {"bufferView": len(doc["bufferViews"]) - 1, "componentType": 5126, "count": int(arr.shape[0]), "type": typ}
        if typ == "SCALAR":
            acc["min"], acc["max"] = [float(arr.min())], [float(arr.max())]
        doc["accessors"].append(acc)
        return len(doc["accessors"]) - 1

    for name, (times, keys) in build_clips(doc, buf, pitch_deg).items():
        clip = next(a for a in doc["animations"] if a["name"] == name)
        tin = add(np.array(times), "SCALAR")
        clip["samplers"], clip["channels"] = [], []
        for node, rots in keys.items():
            q = [np.asarray(r, np.float64) for r in rots]
            for k in range(1, len(q)):
                if np.dot(q[k], q[k - 1]) < 0:
                    q[k] = -q[k]   # shortest-path slerp between keys
            clip["samplers"].append({"input": tin, "output": add(np.array(q), "VEC4"), "interpolation": "LINEAR"})
            clip["channels"].append({"sampler": len(clip["samplers"]) - 1, "target": {"node": node, "path": "rotation"}})
    doc["buffers"][0]["byteLength"] = len(blob)
    import json
    js = json.dumps(doc, separators=(",", ":")).encode()
    js += b" " * (-len(js) % 4)
    blob.extend(b"\0" * (-len(blob) % 4))
    Path(glb_out).write_bytes(struct.pack("<4sII", b"glTF", 2, 28 + len(js) + len(blob)) + struct.pack("<I4s", len(js), b"JSON")
                              + js + struct.pack("<I4s", len(blob), b"BIN\0") + bytes(blob))


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("glb_in")
    ap.add_argument("glb_out")
    ap.add_argument("--pitch", type=float, default=85.0)
    a = ap.parse_args()
    rewrite(a.glb_in, a.glb_out, a.pitch)
