#!/usr/bin/env python3
"""make_placeholder_glb.py -- generate hangar_demo_box.glb, a placeholder
building model for the building-import example mod.

Produces a simple textured box in the coordinate convention the MC2 override
importer expects (assimp_importer.cpp default env, axis map 0: X=-x, Y=-y, Z=z,
NO auto-ground for staticprop overrides):

  * author glTF Y in [-HEIGHT, 0]  ->  MC2 Y in [0, HEIGHT]  (base sits on the
    ground without any runtime grounding shift)
  * one material named "a_hangar", alphaMode MASK, whose base-color texture
    image is named "a_hangar.png". The engine derives the MC2 texture name
    from that image file name (stem + ".tga" -> "a_hangar.tga") and binds the
    STOCK hangar texture already shipped in the game install
    (data/tgl/128/a_hangar.tga). The .png file itself never needs to exist --
    only its NAME is read.

Pure stdlib. Usage:
  py -3 make_placeholder_glb.py [out.glb]
"""
import json
import struct
import sys

# Box dimensions in MC2 units (roughly hangar-sized; stock hangar is a large prop).
HX, HEIGHT, HZ = 8.0, 10.0, 6.0   # half-width X, full height, half-depth Z


def box_geometry():
    """24 verts (4 per face), outward CCW winding, per-face 0..1 UVs.
    glTF space: x in [-HX,HX], y in [-HEIGHT,0], z in [-HZ,HZ]."""
    y0, y1 = -HEIGHT, 0.0
    faces = [
        # (normal, 4 corners CCW seen from outside)
        ((0, 0, 1),  [(-HX, y0,  HZ), ( HX, y0,  HZ), ( HX, y1,  HZ), (-HX, y1,  HZ)]),  # +Z
        ((0, 0, -1), [( HX, y0, -HZ), (-HX, y0, -HZ), (-HX, y1, -HZ), ( HX, y1, -HZ)]),  # -Z
        ((1, 0, 0),  [( HX, y0,  HZ), ( HX, y0, -HZ), ( HX, y1, -HZ), ( HX, y1,  HZ)]),  # +X
        ((-1, 0, 0), [(-HX, y0, -HZ), (-HX, y0,  HZ), (-HX, y1,  HZ), (-HX, y1, -HZ)]),  # -X
        ((0, 1, 0),  [(-HX, y1,  HZ), ( HX, y1,  HZ), ( HX, y1, -HZ), (-HX, y1, -HZ)]),  # +Y (glTF top)
        ((0, -1, 0), [(-HX, y0, -HZ), ( HX, y0, -HZ), ( HX, y0,  HZ), (-HX, y0,  HZ)]),  # -Y (glTF bottom)
    ]
    pos, nrm, uv, idx = [], [], [], []
    for n, corners in faces:
        base = len(pos)
        pos.extend(corners)
        nrm.extend([n] * 4)
        uv.extend([(0, 1), (1, 1), (1, 0), (0, 0)])
        idx.extend([base, base + 1, base + 2, base, base + 2, base + 3])
    return pos, nrm, uv, idx


def main() -> int:
    out = sys.argv[1] if len(sys.argv) > 1 else "hangar_demo_box.glb"
    pos, nrm, uv, idx = box_geometry()

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
        "asset": {"version": "2.0", "generator": "make_placeholder_glb.py (building-import example)"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "name": "hangar_demo_box"}],
        "meshes": [{
            "name": "hangar_demo_box",
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
        #   image "a_hangar.png" -> derived MC2 name "a_hangar.tga" -> stock tile.
        # alphaMode MASK marks the material as alpha-tested (matches the a_ prefix).
        "materials": [{
            "name": "a_hangar",
            "alphaMode": "MASK",
            "alphaCutoff": 0.5,
            "pbrMetallicRoughness": {
                "baseColorTexture": {"index": 0},
                "metallicFactor": 0.0,
                "roughnessFactor": 1.0,
            },
        }],
        "textures": [{"source": 0}],
        "images": [{"uri": "a_hangar.png"}],
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
          f"glTF bounds {mins}..{maxs} (MC2 Y will be 0..{HEIGHT})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
