#!/usr/bin/env python3
"""Count triangles with mirrored UV winding (and GLBs carrying TANGENT) in mech GLBs.

usage: glb_uv_mirror_stats.py <dir-with-glbs>
A triangle is mirrored when sign(uv-area) disagrees with sign(pos-space handedness
reference), approximated per mesh by the majority sign of uv-area (glTF V is flipped
relative to tangent space, but the majority sign is the unmirrored convention).
"""
import json, struct, sys, glob, os
import numpy as np

CT = {5120: np.int8, 5121: np.uint8, 5122: np.int16, 5123: np.uint16, 5125: np.uint32, 5126: np.float32}
NC = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}


def load(path):
    d = open(path, "rb").read()
    _, _, _ = struct.unpack_from("<III", d, 0)
    off, js, bin_ = 12, None, b""
    while off < len(d):
        n, t = struct.unpack_from("<II", d, off)
        body = d[off + 8: off + 8 + n]
        if t == 0x4E4F534A:
            js = json.loads(body)
        elif t == 0x004E4942 and not bin_:
            bin_ = body
        off += 8 + n
    return js, bin_


def acc(js, bin_, i):
    a = js["accessors"][i]
    bv = js["bufferViews"][a["bufferView"]]
    nc, dt = NC[a["type"]], CT[a["componentType"]]
    stride = bv.get("byteStride") or nc * np.dtype(dt).itemsize
    base = bv.get("byteOffset", 0) + a.get("byteOffset", 0)
    out = np.empty((a["count"], nc), dt)
    isz = np.dtype(dt).itemsize
    for k in range(nc):
        out[:, k] = np.ndarray(a["count"], dt, bin_, base + k * isz, (stride,))
    return out


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: glb_uv_mirror_stats.py <dir-with-glbs>  (e.g. <install>/data/tgl)")
    root = sys.argv[1]
    tot = mir = withtan = n = 0
    rows = []
    for p in sorted(glob.glob(os.path.join(root, "*.glb"))):
        js, b = load(p)
        t = m = 0
        tan = False
        for mesh in js.get("meshes", []):
            for pr in mesh["primitives"]:
                at = pr["attributes"]
                tan |= "TANGENT" in at
                if "TEXCOORD_0" not in at or "indices" not in pr or pr.get("mode", 4) != 4:
                    continue
                uv = acc(js, b, at["TEXCOORD_0"]).astype(np.float64)
                ix = acc(js, b, pr["indices"])[:, 0].astype(np.int64).reshape(-1, 3)
                u0, u1, u2 = uv[ix[:, 0]], uv[ix[:, 1]], uv[ix[:, 2]]
                ar = (u1[:, 0] - u0[:, 0]) * (u2[:, 1] - u0[:, 1]) - (u2[:, 0] - u0[:, 0]) * (u1[:, 1] - u0[:, 1])
                ar = ar[np.abs(ar) > 1e-12]
                if ar.size == 0:
                    continue
                pos = (ar > 0).sum()
                neg = (ar < 0).sum()
                # minority sign = mirrored relative to the dominant convention
                m += min(pos, neg)
                t += ar.size
        rows.append((os.path.basename(p), t, m, tan))
        tot += t; mir += m; withtan += tan; n += 1
    for r in rows:
        print("%-40s tris=%7d mirrored=%6d (%.2f%%) TANGENT=%s" % (r[0], r[1], r[2], 100.0 * r[2] / max(r[1], 1), r[3]))
    print("TOTAL glbs=%d tris=%d mirrored=%d (%.3f%%) glbs_with_TANGENT=%d" % (n, tot, mir, 100.0 * mir / max(tot, 1), withtan))


if __name__ == "__main__":
    main()
