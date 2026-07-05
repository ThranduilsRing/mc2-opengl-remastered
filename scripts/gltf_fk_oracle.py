#!/usr/bin/env python3
"""
gltf_fk_oracle.py — BT2018-GLTF-FK-ORACLE-1

A SPEC-PURE, engine-independent forward-kinematics reference for a glTF/GLB
skinned mesh. Parses the GLB binary chunk + JSON directly (no Assimp, no
mc2skel, no pygltflib). Implements glTF 2.0 animation + node-hierarchy
semantics exactly:

  * A sampled animation channel REPLACES that node-local TRS component at time t.
  * A component with NO channel uses the node's bind/default TRS component
    (node.translation / node.rotation / node.scale), defaulting to
    (0,0,0)/(0,0,0,1)/(1,1,1) only when the node field is truly absent.
  * LINEAR keyframe interpolation. Quaternions are normalized and
    hemisphere-corrected (shortest arc) before nlerp.
  * local = T * R * S ; global = parentGlobal * local ; walk from scene roots.
  * NO axis conversion inside FK — pure glTF native space.

Usage:
  python scripts/gltf_fk_oracle.py <model.glb> --clip <name> --frame N [--frame M ...]
                                   [--json out.json] [--validate]
"""
import sys, json, struct, argparse
import numpy as np

# ---------- GLB parse ----------

def load_glb(path):
    with open(path, "rb") as f:
        data = f.read()
    magic, version, length = struct.unpack_from("<III", data, 0)
    assert magic == 0x46546C67, "not a GLB"
    off = 12
    gltf_json = None
    bin_chunk = None
    while off < length:
        clen, ctype = struct.unpack_from("<II", data, off)
        off += 8
        chunk = data[off:off + clen]
        off += clen
        if ctype == 0x4E4F534A:      # 'JSON'
            gltf_json = json.loads(chunk.decode("utf-8"))
        elif ctype == 0x004E4942:    # 'BIN\0'
            bin_chunk = chunk
    return gltf_json, bin_chunk

_COMP = {5120: ("b", 1), 5121: ("B", 1), 5122: ("h", 2), 5123: ("H", 2),
         5125: ("I", 4), 5126: ("f", 4)}
_NUMC = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}

def read_accessor(gltf, bin_chunk, idx):
    acc = gltf["accessors"][idx]
    bv = gltf["bufferViews"][acc["bufferView"]]
    comp_fmt, comp_size = _COMP[acc["componentType"]]
    ncomp = _NUMC[acc["type"]]
    count = acc["count"]
    base = bv.get("byteOffset", 0) + acc.get("byteOffset", 0)
    stride = bv.get("byteStride", comp_size * ncomp)
    out = np.empty((count, ncomp), dtype=np.float64)
    for i in range(count):
        elem_off = base + i * stride
        vals = struct.unpack_from("<" + comp_fmt * ncomp, bin_chunk, elem_off)
        out[i] = vals
    return out if ncomp > 1 else out.reshape(-1)

# ---------- math (T*R*S, glTF quaternion = (x,y,z,w)) ----------

def quat_norm(q):
    n = np.linalg.norm(q)
    return q / n if n > 0 else np.array([0.0, 0.0, 0.0, 1.0])

def quat_to_mat(q):
    x, y, z, w = q
    return np.array([
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w),     2 * (x * z + y * w),     0],
        [2 * (x * y + z * w),     1 - 2 * (x * x + z * z), 2 * (y * z - x * w),     0],
        [2 * (x * z - y * w),     2 * (y * z + x * w),     1 - 2 * (x * x + y * y), 0],
        [0, 0, 0, 1]], dtype=np.float64)

def trs_matrix(t, r, s):
    T = np.eye(4); T[:3, 3] = t
    R = quat_to_mat(r)
    S = np.eye(4); S[0, 0], S[1, 1], S[2, 2] = s
    return T @ R @ S

def lerp_vec(keys_t, keys_v, t):
    if len(keys_t) == 1:
        return keys_v[0]
    if t <= keys_t[0]:
        return keys_v[0]
    if t >= keys_t[-1]:
        return keys_v[-1]
    i = np.searchsorted(keys_t, t, side="right") - 1
    i = max(0, min(i, len(keys_t) - 2))
    t0, t1 = keys_t[i], keys_t[i + 1]
    f = (t - t0) / (t1 - t0) if t1 > t0 else 0.0
    return keys_v[i] * (1 - f) + keys_v[i + 1] * f

def nlerp_quat(keys_t, keys_v, t):
    if len(keys_t) == 1:
        return quat_norm(keys_v[0])
    if t <= keys_t[0]:
        return quat_norm(keys_v[0])
    if t >= keys_t[-1]:
        return quat_norm(keys_v[-1])
    i = np.searchsorted(keys_t, t, side="right") - 1
    i = max(0, min(i, len(keys_t) - 2))
    t0, t1 = keys_t[i], keys_t[i + 1]
    f = (t - t0) / (t1 - t0) if t1 > t0 else 0.0
    q0 = quat_norm(keys_v[i]); q1 = quat_norm(keys_v[i + 1])
    if np.dot(q0, q1) < 0:        # hemisphere / shortest-arc
        q1 = -q1
    return quat_norm(q0 * (1 - f) + q1 * f)

# ---------- node defaults ----------

def node_default_trs(node):
    if "matrix" in node:
        # column-major 16 -> decompose. Rare for animated nodes; handle anyway.
        m = np.array(node["matrix"], dtype=np.float64).reshape(4, 4).T
        t = m[:3, 3].copy()
        sx = np.linalg.norm(m[:3, 0]); sy = np.linalg.norm(m[:3, 1]); sz = np.linalg.norm(m[:3, 2])
        s = np.array([sx, sy, sz])
        rot = m[:3, :3] / np.array([sx, sy, sz])
        # matrix->quat
        tr = rot[0, 0] + rot[1, 1] + rot[2, 2]
        if tr > 0:
            S = np.sqrt(tr + 1.0) * 2; w = 0.25 * S
            x = (rot[2, 1] - rot[1, 2]) / S; y = (rot[0, 2] - rot[2, 0]) / S; z = (rot[1, 0] - rot[0, 1]) / S
        else:
            i = np.argmax([rot[0, 0], rot[1, 1], rot[2, 2]])
            if i == 0:
                S = np.sqrt(1.0 + rot[0, 0] - rot[1, 1] - rot[2, 2]) * 2
                w = (rot[2, 1] - rot[1, 2]) / S; x = 0.25 * S
                y = (rot[0, 1] + rot[1, 0]) / S; z = (rot[0, 2] + rot[2, 0]) / S
            elif i == 1:
                S = np.sqrt(1.0 + rot[1, 1] - rot[0, 0] - rot[2, 2]) * 2
                w = (rot[0, 2] - rot[2, 0]) / S; x = (rot[0, 1] + rot[1, 0]) / S
                y = 0.25 * S; z = (rot[1, 2] + rot[2, 1]) / S
            else:
                S = np.sqrt(1.0 + rot[2, 2] - rot[0, 0] - rot[1, 1]) * 2
                w = (rot[1, 0] - rot[0, 1]) / S; x = (rot[0, 2] + rot[2, 0]) / S
                y = (rot[1, 2] + rot[2, 1]) / S; z = 0.25 * S
        return t, quat_norm(np.array([x, y, z, w])), s
    t = np.array(node.get("translation", [0, 0, 0]), dtype=np.float64)
    r = np.array(node.get("rotation", [0, 0, 0, 1]), dtype=np.float64)
    s = np.array(node.get("scale", [1, 1, 1]), dtype=np.float64)
    return t, r, s

# ---------- FK ----------

class Oracle:
    def __init__(self, path):
        self.gltf, self.bin = load_glb(path)
        self.nodes = self.gltf["nodes"]
        # parent map
        self.parent = [-1] * len(self.nodes)
        for ni, n in enumerate(self.nodes):
            for c in n.get("children", []):
                self.parent[c] = ni
        # roots: scene nodes
        scene = self.gltf.get("scene", 0)
        self.roots = self.gltf["scenes"][scene]["nodes"]
        self.name2idx = {}
        for ni, n in enumerate(self.nodes):
            nm = n.get("name", "node%d" % ni)
            self.name2idx[nm] = ni

    def channels_for(self, clip_name):
        anim = None
        for a in self.gltf.get("animations", []):
            if a.get("name") == clip_name:
                anim = a
                break
        if anim is None:
            raise SystemExit("clip not found: " + clip_name)
        # node -> {path: (times, values)}
        chans = {}
        interp_set = set()
        for ch in anim["channels"]:
            samp = anim["samplers"][ch["sampler"]]
            tgt = ch["target"]
            node = tgt["node"]
            path = tgt["path"]
            times = read_accessor(self.gltf, self.bin, samp["input"])
            vals = read_accessor(self.gltf, self.bin, samp["output"])
            interp_set.add(samp.get("interpolation", "LINEAR"))
            chans.setdefault(node, {})[path] = (np.asarray(times, dtype=np.float64), vals)
        self.interp_set = interp_set
        # duration = max time
        dur = 0.0
        for nd in chans.values():
            for (tt, vv) in nd.values():
                if len(tt):
                    dur = max(dur, tt[-1])
        return chans, dur

    def local_trs(self, node_idx, chans, t):
        node = self.nodes[node_idx]
        dt, dr, ds = node_default_trs(node)
        nd = chans.get(node_idx, {})
        if "translation" in nd:
            tt, vv = nd["translation"]; dt = lerp_vec(tt, vv, t)
        if "rotation" in nd:
            tt, vv = nd["rotation"]; dr = nlerp_quat(tt, vv, t)
        if "scale" in nd:
            tt, vv = nd["scale"]; ds = lerp_vec(tt, vv, t)
        return dt, dr, ds

    def evaluate(self, clip_name, t):
        chans, dur = self.channels_for(clip_name)
        globals_ = {}
        locals_trs = {}

        def walk(ni, parent_g):
            dt, dr, ds = self.local_trs(ni, chans, t)
            L = trs_matrix(dt, dr, ds)
            G = parent_g @ L
            globals_[ni] = G
            locals_trs[ni] = (dt, dr, ds, L)
            for c in self.nodes[ni].get("children", []):
                walk(c, G)

        for r in self.roots:
            walk(r, np.eye(4))
        return globals_, locals_trs, dur

    def inv_bind_for(self, node_idx):
        """Return inverse-bind matrices keyed by joint node from skins (first match)."""
        for sk in self.gltf.get("skins", []):
            joints = sk["joints"]
            if node_idx in joints and "inverseBindMatrices" in sk:
                ibm = read_accessor(self.gltf, self.bin, sk["inverseBindMatrices"])
                j = joints.index(node_idx)
                return ibm[j].reshape(4, 4).T  # column-major -> row-major numpy
        return None


def frame_to_time(dur_ticks_or_sec, frame, fps=30.0):
    # glTF times are in SECONDS. Engine uses frame/30*ticksPerSec where Assimp
    # exposes ticksPerSecond. To compare at the SAME pose we sample at the same
    # wall-clock instant: t_sec = frame/30 (engine 30fps frame convention),
    # clamped to clip duration.
    t = frame / fps
    return min(t, dur_ticks_or_sec)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("glb")
    ap.add_argument("--clip", required=True)
    ap.add_argument("--frame", type=int, action="append", required=True)
    ap.add_argument("--json")
    ap.add_argument("--validate", action="store_true")
    ap.add_argument("--fps", type=float, default=30.0)
    args = ap.parse_args()

    orc = Oracle(args.glb)
    # peek duration
    _, dur = orc.channels_for(args.clip)
    print("clip=%s duration_sec=%.4f interpolations=%s nodes=%d roots=%s"
          % (args.clip, dur, sorted(orc.interp_set), len(orc.nodes), orc.roots))

    out = {"clip": args.clip, "duration_sec": dur, "frames": {}}
    for fr in args.frame:
        t = frame_to_time(dur, fr, args.fps)
        g, l, _ = orc.evaluate(args.clip, t)
        frec = {"frame": fr, "t_sec": t, "bones": {}}
        for nm, ni in orc.name2idx.items():
            if ni not in g:
                continue
            dt, dr, ds, L = l[ni]
            frec["bones"][nm] = {
                "node": ni,
                "parent": orc.parent[ni],
                "localT": dt.tolist(),
                "localR": dr.tolist(),
                "localS": ds.tolist(),
                "global": g[ni].tolist(),
            }
        out["frames"][str(fr)] = frec

    if args.validate:
        validate(orc, args.clip, args.fps)

    if args.json:
        json.dump(out, open(args.json, "w"), indent=1)
        print("wrote", args.json)


def validate(orc, clip, fps):
    """Reverse-joint check: knee-minus-ankle along forward axis across the gait.
    Marauder is reverse-jointed; viewer shows knee BEHIND ankle through plant.
    We report knee(j_LCalf) minus ankle(j_LFoot) global translation for each axis
    across frames 0..27 so the reverse stance is visible regardless of which
    glTF axis is 'forward'."""
    _, dur = orc.channels_for(clip)
    kn = orc.name2idx.get("j_LCalf")
    an = orc.name2idx.get("j_LFoot")
    print("\n=== ORACLE VALIDATION: knee(j_LCalf) - ankle(j_LFoot) global, frames 0..27 ===")
    print("frame  dX        dY        dZ")
    for fr in range(0, 28):
        t = min(fr / fps, dur)
        g, _, _ = orc.evaluate(clip, t)
        d = g[kn][:3, 3] - g[an][:3, 3]
        print("%5d  %+.5f  %+.5f  %+.5f" % (fr, d[0], d[1], d[2]))


if __name__ == "__main__":
    main()
