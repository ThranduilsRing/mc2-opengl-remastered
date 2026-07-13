#!/usr/bin/env python3
"""make_placeholder_glb.py -- generate oak1_leafcard.glb, a placeholder tree
model for the tree-import example mod.

This is deliberately as simple as the building-import example's box: two
crossed alpha-tested quads (a classic "billboard cross" foliage card), NOT a
faithful recreation of the stock oak1 tree. The point of this example is the
model-override MECHANISM (registry entry, resolve seam, autoGround, texture
reuse), not tree art.

Coordinate convention (mclib/assimp_importer.cpp, MC2_GLTF_AXIS=0 default):
  mc2.x = -gltf.x ; mc2.y = -gltf.y ; mc2.z = gltf.z   (axisMap case 0)
  -> author glTF Y in [-HEIGHT, 0]  =>  MC2 Y in [0, HEIGHT] (canopy up).

UNLIKE the building-import box (a staticProp override, autoGround=false,
must be pivoted exactly), trees resolve with autoGround=true
(mclib/bdactor.cpp:4123 omits the 3rd ImportGeometryFromFile arg, so it
takes the header default `bool autoGround = true`,
mclib/assimp_importer.h:38). After import, assimp_importer.cpp:539-555
re-translates the whole mesh vertically so one end of its bounding box
lands on MC2 Y=0 (tuned by MC2_GLTF_GROUND, default 2) -- so even if this
placeholder's pivot is slightly off, the engine grounds it for you. A
staticProp override gets NO such correction; ship a wrong pivot and it
floats or sinks.

Material: one material named "a_oak1", alphaMode MASK, base-color image
named "a_oak1.png". The engine derives the MC2 texture name from that
image's file STEM ("a_oak1" + ".tga" -> "a_oak1.tga") and binds the STOCK
oak1 leaf texture already shipped in the game install
(data/tgl/128/a_oak1.tga, 128x128 RGBA -- confirmed to carry an alpha
channel). The .png itself never needs to exist -- only its NAME is read
(same trick as building-import's a_hangar.png).

Pure stdlib. Usage:
  py -3 make_placeholder_glb.py [out.glb]
"""
import json
import struct
import sys

# Card half-width and full height, in MC2 units. Loosely tree-sized; not
# measured against the real oak1 .ase (not available in this checkout --
# only the compiled .ini/.tga ship in a deploy tree, see README "Stock data
# reference").
HALF_W, HEIGHT = 5.0, 14.0


def cross_card_geometry():
    """Two crossed vertical quads (X-aligned and Z-aligned), each built
    front+back (4 tris/plane) so the alpha-cut leaf silhouette reads from
    any camera angle without depending on material double-sidedness being
    honored by the static-prop renderer.

    glTF space: Y in [-HEIGHT, 0] (canopy at -HEIGHT, base at 0)."""
    y0, y1 = -HEIGHT, 0.0

    def quad(corners, normal):
        """corners: 4 pts CCW seen from `normal`'s side. Returns
        (positions, normals, uvs, front_tri_idx_pairs, back_tri_idx_pairs)
        as flat vertex lists (front block then back block, 4 verts each)."""
        pos = list(corners)
        nrm_f = [normal] * 4
        nrm_b = [(-normal[0], -normal[1], -normal[2])] * 4
        uv = [(0, 1), (1, 1), (1, 0), (0, 0)]
        return pos, nrm_f, nrm_b, uv

    # Plane A: spans X, faces +Z/-Z. Plane B: spans Z, faces +X/-X.
    planes = [
        (((-HALF_W, y0, 0.0), (HALF_W, y0, 0.0), (HALF_W, y1, 0.0), (-HALF_W, y1, 0.0)), (0, 0, 1)),
        (((0.0, y0, -HALF_W), (0.0, y0, HALF_W), (0.0, y1, HALF_W), (0.0, y1, -HALF_W)), (1, 0, 0)),
    ]

    pos, nrm, uv, idx = [], [], [], []
    for corners, normal in planes:
        p, nf, nb, u = quad(corners, normal)
        base_f = len(pos)
        pos.extend(p); nrm.extend(nf); uv.extend(u)
        idx.extend([base_f, base_f + 1, base_f + 2, base_f, base_f + 2, base_f + 3])
        base_b = len(pos)
        pos.extend(p); nrm.extend(nb); uv.extend(u)
        # reversed winding for the back face
        idx.extend([base_b, base_b + 2, base_b + 1, base_b, base_b + 3, base_b + 2])
    return pos, nrm, uv, idx


def main() -> int:
    out = sys.argv[1] if len(sys.argv) > 1 else "oak1_leafcard.glb"
    pos, nrm, uv, idx = cross_card_geometry()

    def fbytes(rows):
        return b"".join(struct.pack("<" + "f" * len(r), *r) for r in rows)

    pos_b, nrm_b = fbytes(pos), fbytes(nrm)
    uv_b = fbytes(uv)
    idx_b = b"".join(struct.pack("<H", i) for i in idx)
    if len(idx_b) % 4:
        idx_b += b"\x00\x00"

    blobs, views, offset = [], [], 0
    for b, target in ((pos_b, 34962), (nrm_b, 34962), (uv_b, 34962), (idx_b, 34963)):
        views.append({"buffer": 0, "byteOffset": offset, "byteLength": len(b), "target": target})
        blobs.append(b)
        offset += len(b)
    binblob = b"".join(blobs)

    mins = [min(p[i] for p in pos) for i in range(3)]
    maxs = [max(p[i] for p in pos) for i in range(3)]

    gltf = {
        "asset": {"version": "2.0", "generator": "make_placeholder_glb.py (tree-import example)"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "name": "oak1_leafcard"}],
        "meshes": [{
            "name": "oak1_leafcard",
            "primitives": [{
                "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
                "indices": 3,
                "material": 0,
                "mode": 4,
            }],
        }],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": len(pos), "type": "VEC3",
             "min": mins, "max": maxs},
            {"bufferView": 1, "componentType": 5126, "count": len(nrm), "type": "VEC3"},
            {"bufferView": 2, "componentType": 5126, "count": len(uv), "type": "VEC2"},
            {"bufferView": 3, "componentType": 5123, "count": len(idx), "type": "SCALAR"},
        ],
        "bufferViews": views,
        "buffers": [{"byteLength": len(binblob)}],
        # Material name + image name drive the engine-side texture binding:
        #   image "a_oak1.png" -> derived MC2 name "a_oak1.tga" -> stock tile.
        # alphaMode MASK marks the material as alpha-tested (matches the a_
        # prefix / leaf-card convention documented in mechanism-map.md).
        "materials": [{
            "name": "a_oak1",
            "alphaMode": "MASK",
            "alphaCutoff": 0.5,
            "doubleSided": True,
            "pbrMetallicRoughness": {
                "baseColorTexture": {"index": 0},
                "metallicFactor": 0.0,
                "roughnessFactor": 1.0,
            },
        }],
        "textures": [{"source": 0}],
        "images": [{"uri": "a_oak1.png"}],
    }

    js = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
    js += b" " * (-len(js) % 4)
    binpad = binblob + b"\x00" * (-len(binblob) % 4)
    total = 12 + 8 + len(js) + 8 + len(binpad)
    with open(out, "wb") as f:
        f.write(struct.pack("<4sII", b"glTF", 2, total))
        f.write(struct.pack("<I4s", len(js), b"JSON"))
        f.write(js)
        f.write(struct.pack("<I4s", len(binpad), b"BIN\x00"))
        f.write(binpad)
    print(f"wrote {out}: {len(pos)} verts, {len(idx)//3} tris, "
          f"glTF bounds {mins}..{maxs} (MC2 Y will be 0..{HEIGHT} after axis map)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
