"""
fk_bake.py — bake a skinned GLB's REST pose into a static assembled mesh (OBJ).

Pure numpy, no Blender. Reads POSITION/JOINTS_0/WEIGHTS_0 + skin inverseBind +
node hierarchy, computes each joint's global rest transform via FK, skins every
vertex once (glTF linear-blend skinning), and writes an assembled OBJ.

This is the "ingest the GLB directly" path: the parts live in bone-local space
(piled at origin); only the skeleton assembles them. Baking the rest pose gives
the correct standing mech as static geometry.

    py -3 fk_bake.py --src _fbx/marauder.glb --out _out/marauder_rest.obj [--drop blip,uix,_dmg,_explode]
"""
import argparse, json, struct
import numpy as np

CT = {5120: ("b", 1), 5121: ("B", 1), 5122: ("h", 2), 5123: ("H", 2),
      5125: ("I", 4), 5126: ("f", 4)}
NCOMP = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}


def load_glb(path):
    d = open(path, "rb").read()
    _, _, length = struct.unpack("<III", d[:12]); off = 12; js = None; bin_ = b""
    while off < length:
        clen, ctype = struct.unpack("<II", d[off:off+8]); off += 8
        chunk = d[off:off+clen]; off += clen
        if ctype == 0x4E4F534A: js = json.loads(chunk)
        elif ctype == 0x004E4942: bin_ = chunk
    return js, bin_


def read_accessor(js, bin_, idx):
    a = js["accessors"][idx]
    bv = js["bufferViews"][a["bufferView"]]
    comp, size = CT[a["componentType"]]; ncomp = NCOMP[a["type"]]
    base = bv.get("byteOffset", 0) + a.get("byteOffset", 0)
    stride = bv.get("byteStride", 0) or (size * ncomp)
    out = np.empty((a["count"], ncomp), dtype={"b":np.int8,"B":np.uint8,"h":np.int16,
        "H":np.uint16,"I":np.uint32,"f":np.float32}[comp])
    for i in range(a["count"]):
        o = base + i * stride
        out[i] = struct.unpack_from("<" + comp * ncomp, bin_, o)
    return out


def node_local(n):
    if "matrix" in n:
        return np.array(n["matrix"], dtype=np.float64).reshape(4, 4).T  # glTF col-major
    T = np.eye(4); R = np.eye(4); S = np.eye(4)
    if "translation" in n: T[:3, 3] = n["translation"]
    if "rotation" in n:
        x, y, z, w = n["rotation"]
        R[:3, :3] = np.array([
            [1-2*(y*y+z*z), 2*(x*y-z*w),   2*(x*z+y*w)],
            [2*(x*y+z*w),   1-2*(x*x+z*z), 2*(y*z-x*w)],
            [2*(x*z-y*w),   2*(y*z+x*w),   1-2*(x*x+y*y)]])
    if "scale" in n: S[:3, :3] = np.diag(n["scale"])
    return T @ R @ S


def global_transforms(js):
    nodes = js["nodes"]; n = len(nodes)
    parent = [-1] * n
    for i, nd in enumerate(nodes):
        for c in nd.get("children", []): parent[c] = i
    locals_ = [node_local(nd) for nd in nodes]
    glob = [None] * n
    def comp(i):
        if glob[i] is not None: return glob[i]
        glob[i] = locals_[i] if parent[i] < 0 else comp(parent[i]) @ locals_[i]
        return glob[i]
    for i in range(n): comp(i)
    return glob


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True)
    ap.add_argument("--out", help="OBJ output (geometry only, for QA render)")
    ap.add_argument("--out_glb", help="textured GLB output (deployable/validatable)")
    ap.add_argument("--drop", default="blip,uix,_dmg,_explode")
    ap.add_argument("--target_height", type=float, default=25.44, help="MC2 units (combined.glb Y)")
    ap.add_argument("--albedo", help="PNG to embed as baseColor + its name drives the engine .tga lookup")
    ap.add_argument("--normal", help="PNG normal map -> normalTexture")
    ap.add_argument("--emissive", help="PNG emissive map -> emissiveTexture")
    ap.add_argument("--embed_max", type=int, default=512, help="downscale embedded maps to this max edge (GLB size)")
    args = ap.parse_args()
    drop = [t.lower() for t in args.drop.split(",") if t]

    js, bin_ = load_glb(args.src)
    glob = global_transforms(js)

    # cache inverse-bind matrices per skin
    skin_ibm = {}
    for si, sk in enumerate(js["skins"]):
        raw = read_accessor(js, bin_, sk["inverseBindMatrices"]).astype(np.float64)
        skin_ibm[si] = [raw[k].reshape(4, 4).T for k in range(raw.shape[0])]  # col-major

    verts = []; uvs = []; tris = []; voff = 0; kept = 0
    for nd in js["nodes"]:
        if "mesh" not in nd or "skin" not in nd: continue
        mname = js["meshes"][nd["mesh"]].get("name", "")
        if any(t in mname.lower() for t in drop): continue
        skin = js["skins"][nd["skin"]]; joints = skin["joints"]; ibm = skin_ibm[nd["skin"]]
        for pr in js["meshes"][nd["mesh"]]["primitives"]:
            attr = pr["attributes"]
            P = read_accessor(js, bin_, attr["POSITION"]).astype(np.float64)
            J = read_accessor(js, bin_, attr["JOINTS_0"]).astype(np.int64)
            W = read_accessor(js, bin_, attr["WEIGHTS_0"]).astype(np.float64)
            UV = (read_accessor(js, bin_, attr["TEXCOORD_0"]).astype(np.float32)
                  if "TEXCOORD_0" in attr else np.zeros((P.shape[0], 2), np.float32))
            idx = read_accessor(js, bin_, pr["indices"]).astype(np.int64).ravel()
            jm = [glob[joints[k]] @ ibm[k] for k in range(len(joints))]
            out = np.zeros((P.shape[0], 3))
            ph = np.hstack([P, np.ones((P.shape[0], 1))])
            for k in range(4):
                w = W[:, k]
                if not w.any(): continue
                for jj in np.unique(J[:, k]):
                    m = J[:, k] == jj
                    out[m] += (w[m, None]) * (ph[m] @ jm[jj].T)[:, :3]
            verts.append(out); uvs.append(UV)
            tris.append(idx.reshape(-1, 3) + voff)
            voff += P.shape[0]
        kept += 1

    V = np.vstack(verts); UVall = np.vstack(uvs).astype(np.float32); F = np.vstack(tris).astype(np.uint32)
    mn, mx = V.min(0), V.max(0)
    # Scale to MC2 size (tallest axis = height); rest pose is glTF Y-up like combined.glb.
    cur_h = float((mx - mn).max())
    s = args.target_height / cur_h if cur_h > 0 else 1.0
    V = (V - (mn + mx) / 2.0) * s        # center + scale
    V[:, 1] += (mx[1] - mn[1]) * s / 2.0  # feet ~ y=0
    print(f"[FK_BAKE] kept {kept} meshes, {V.shape[0]} verts, {F.shape[0]} tris; scaled x{s:.1f} -> dims {np.round((mx-mn)*s,2).tolist()}")

    import os
    if args.out:
        os.makedirs(os.path.dirname(args.out), exist_ok=True)
        with open(args.out, "w") as f:
            for v in V: f.write(f"v {v[0]:.5f} {v[1]:.5f} {v[2]:.5f}\n")
            for a, b, c in F + 1: f.write(f"f {a} {b} {c}\n")
        print(f"[FK_BAKE] wrote {args.out}")
    if args.out_glb:
        write_glb(args.out_glb, V.astype(np.float32), UVall, F,
                  args.albedo, args.normal, args.emissive, args.embed_max)
        print(f"[FK_BAKE] wrote {args.out_glb}")


def write_glb(path, V, UV, F, albedo_png, normal_png, emissive_png, embed_max):
    """Textured GLB: one mesh, POSITION+TEXCOORD_0, one PBR material with
    baseColor (+ optional normal/emissive). The baseColor image is NAMED after
    albedo_png stem so the MC2 importer derives the matching <stem>.tga."""
    import io, os
    from PIL import Image
    # UVs come straight from the source glTF; same convention on write -> no flip.
    pos = V.astype("<f4").tobytes(); uv = UV.astype("<f4").tobytes(); idx = F.astype("<u4").ravel().tobytes()

    def load_png(p):
        if not (p and os.path.exists(p)): return None, None
        im = Image.open(p).convert("RGBA")
        if max(im.size) > embed_max: im.thumbnail((embed_max, embed_max), Image.LANCZOS)
        b = io.BytesIO(); im.save(b, "PNG")
        return b.getvalue(), os.path.splitext(os.path.basename(p))[0]

    # role -> (png bytes, name)
    maps = []
    for role, p in (("base", albedo_png), ("normal", normal_png), ("emissive", emissive_png)):
        data, nm = load_png(p)
        if data: maps.append((role, data, nm))

    def pad(b): return b + b"\x00" * ((4 - len(b) % 4) % 4)
    geo_parts = [pos, uv, idx]
    parts = geo_parts + [m[1] for m in maps]
    blob = b""; views = []; o = 0
    for p in parts:
        pp = pad(p); views.append((o, len(p))); blob += pp; o += len(pp)

    nv = len(V)
    accs = [
        {"bufferView": 0, "componentType": 5126, "count": nv, "type": "VEC3", "min": V.min(0).tolist(), "max": V.max(0).tolist()},
        {"bufferView": 1, "componentType": 5126, "count": nv, "type": "VEC2"},
        {"bufferView": 2, "componentType": 5125, "count": int(F.size), "type": "SCALAR"},
    ]
    bvs = [{"buffer": 0, "byteOffset": views[0][0], "byteLength": views[0][1], "target": 34962},
           {"buffer": 0, "byteOffset": views[1][0], "byteLength": views[1][1], "target": 34962},
           {"buffer": 0, "byteOffset": views[2][0], "byteLength": views[2][1], "target": 34963}]
    pbr = {"baseColorFactor": [1, 1, 1, 1]}
    mat = {"name": "marauder_body", "pbrMetallicRoughness": pbr, "doubleSided": True}
    images, textures, samplers = [], [], [{}]
    for i, (role, data, nm) in enumerate(maps):
        bvs.append({"buffer": 0, "byteOffset": views[3 + i][0], "byteLength": views[3 + i][1]})
        images.append({"bufferView": len(bvs) - 1, "mimeType": "image/png", "name": nm})
        textures.append({"source": i, "sampler": 0})
        if role == "base": pbr["baseColorTexture"] = {"index": i}
        elif role == "normal": mat["normalTexture"] = {"index": i}
        elif role == "emissive":
            mat["emissiveTexture"] = {"index": i}; mat["emissiveFactor"] = [1, 1, 1]
    js = {"asset": {"version": "2.0", "generator": "mc2 fk_bake"},
          "scene": 0, "scenes": [{"nodes": [0]}],
          "nodes": [{"mesh": 0, "name": "marauder"}],
          "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "TEXCOORD_0": 1}, "indices": 2, "material": 0}]}],
          "materials": [mat], "buffers": [{"byteLength": 0}],
          "bufferViews": bvs, "accessors": accs}
    if images:
        js["images"] = images; js["textures"] = textures; js["samplers"] = samplers

    jbytes = json.dumps(js).encode("utf-8")
    jsb = jbytes + b"\x20" * ((4 - len(jbytes) % 4) % 4)  # JSON chunk pads with SPACES
    blob = pad(blob)
    js["buffers"][0]["byteLength"] = len(blob)
    # re-serialize with correct buffer length
    jbytes = json.dumps(js).encode("utf-8")
    jsb = jbytes + b"\x20" * ((4 - len(jbytes) % 4) % 4)
    glb = struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(jsb) + 8 + len(blob))
    glb += struct.pack("<II", len(jsb), 0x4E4F534A) + jsb
    glb += struct.pack("<II", len(blob), 0x004E4942) + blob
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, "wb").write(glb)


if __name__ == "__main__":
    main()
