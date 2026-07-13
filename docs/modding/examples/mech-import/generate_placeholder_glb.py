#!/usr/bin/env python3
"""generate_placeholder_glb.py -- generate warhammer_glb_demo.glb, a
placeholder mech mesh for the mech-import example mod.

This is deliberately a crude 6-box "toy soldier" silhouette (two legs, a
torso, two arms, a head), NOT a faithful Warhammer recreation. The point of
this example is the [Import] MESH mechanism (mclib/mech3d.cpp:346-360 opt-in
+ mclib/msl.cpp:438-479 .glb/.fbx/.ase probe), not mech art. For a real
asset pipeline (BattleTech-2018 rip -> assembled, textured GLB) see
tools/mech_import/README.md.

Coordinate convention (mclib/assimp_importer.cpp, MC2_GLTF_AXIS=0 default):
  mc2.x = -gltf.x ; mc2.y = -gltf.y ; mc2.z = gltf.z   (axisMap case 0)
  -> author glTF Y in [-HEIGHT, 0]  =>  MC2 Y in [0, HEIGHT] (head up).
This is the SAME convention already verified for the tree-import example
(docs/modding/examples/tree-import/mod/source/make_placeholder_glb.py) --
base/feet at glTF Y=0, top/head at glTF Y=-HEIGHT.

Mech import uses autoGround=true (the default 3rd argument of
ImportGeometryFromFile; mclib/msl.cpp:463 does not override it), so after
import assimp_importer.cpp:539-555 re-translates the whole mesh vertically
so one end of its bounding box lands on MC2 Y=0 (tuned by MC2_GLTF_GROUND,
default 2) -- a second safety net on top of the authoring convention above.

Material: untextured (no baseColorTexture -> DeriveMC2TextureName returns
false -> the multi-shape's texture slot gets the "NULLTXM" sentinel, same
fallback ParseASEFile uses for an untextured ASE material,
mclib/assimp_importer.cpp:263-266). This keeps the placeholder fully
self-contained -- no .tga/.ktx2 has to ship alongside it. A real mech GLB
would embed a textured material instead (see tools/mech_import/README.md).

Pure stdlib. Usage:
  py -3 generate_placeholder_glb.py [out.glb]
"""
import json
import struct
import sys

# All sizes in MC2 units. Loosely mech-sized (Warhammer-class, ~12.5 units
# tall); not measured against the real stock Warhammer mesh (not present as
# loose geometry in this checkout -- see README "Stock data reference").
LEG_TOP = -6.0        # legs run glTF Y in [0, LEG_TOP]  (toward -Y = up)
TORSO_TOP = -10.5      # torso runs [LEG_TOP, TORSO_TOP]
HEAD_TOP = -12.5       # head runs [TORSO_TOP, HEAD_TOP]  -> HEIGHT = 12.5
ARM_BOTTOM, ARM_TOP = -6.0, -10.0


def box(cx, cz, y_bottom, y_top, hx, hz):
    """One box, 24 verts (4 per face), outward CCW winding, per-face 0..1
    UVs. y_bottom/y_top are glTF Y values (both usually <= 0 here except the
    legs' y_bottom = 0)."""
    y0, y1 = y_bottom, y_top  # y0 nearer the ground (larger), y1 higher (more negative)
    x0, x1 = cx - hx, cx + hx
    z0, z1 = cz - hz, cz + hz
    faces = [
        ((0, 0, 1),  [(x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)]),   # +Z
        ((0, 0, -1), [(x1, y0, z0), (x0, y0, z0), (x0, y1, z0), (x1, y1, z0)]),   # -Z
        ((1, 0, 0),  [(x1, y0, z1), (x1, y0, z0), (x1, y1, z0), (x1, y1, z1)]),   # +X
        ((-1, 0, 0), [(x0, y0, z0), (x0, y0, z1), (x0, y1, z1), (x0, y1, z0)]),   # -X
        ((0, -1, 0), [(x0, y1, z1), (x1, y1, z1), (x1, y1, z0), (x0, y1, z0)]),   # top (toward -Y = up)
        ((0, 1, 0),  [(x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1)]),   # bottom
    ]
    pos, nrm, uv, idx = [], [], [], []
    for n, corners in faces:
        base = len(pos)
        pos.extend(corners)
        nrm.extend([n] * 4)
        uv.extend([(0, 1), (1, 1), (1, 0), (0, 0)])
        idx.extend([base, base + 1, base + 2, base, base + 2, base + 3])
    return pos, nrm, uv, idx


def mech_geometry():
    parts = [
        # legs (feet at glTF Y=0, ground)
        (-1.5, 0.0, 0.0, LEG_TOP, 1.0, 1.0),
        (1.5, 0.0, 0.0, LEG_TOP, 1.0, 1.0),
        # torso
        (0.0, 0.0, LEG_TOP, TORSO_TOP, 3.0, 2.0),
        # arms (attached to torso sides)
        (-4.2, 0.0, ARM_BOTTOM, ARM_TOP, 0.8, 0.8),
        (4.2, 0.0, ARM_BOTTOM, ARM_TOP, 0.8, 0.8),
        # head
        (0.0, 0.0, TORSO_TOP, HEAD_TOP, 1.0, 1.0),
    ]
    pos, nrm, uv, idx = [], [], [], []
    for cx, cz, y0, y1, hx, hz in parts:
        p, n, u, i = box(cx, cz, y0, y1, hx, hz)
        base = len(pos)
        pos.extend(p); nrm.extend(n); uv.extend(u)
        idx.extend([base + k for k in i])
    return pos, nrm, uv, idx


def main() -> int:
    out = sys.argv[1] if len(sys.argv) > 1 else "warhammer_glb_demo.glb"
    pos, nrm, uv, idx = mech_geometry()

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
        "asset": {"version": "2.0", "generator": "generate_placeholder_glb.py (mech-import example)"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "name": "warhammer_glb_demo"}],
        "meshes": [{
            "name": "warhammer_glb_demo",
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
        # No baseColorTexture -> DeriveMC2TextureName() returns false ->
        # NULLTXM sentinel (mclib/assimp_importer.cpp:167-168, 263-266).
        # baseColorFactor only affects the flat-shaded tint, not the
        # texture-name derivation.
        "materials": [{
            "name": "warhammer_glb_demo_mat",
            "pbrMetallicRoughness": {
                "baseColorFactor": [0.35, 0.38, 0.42, 1.0],
                "metallicFactor": 0.0,
                "roughnessFactor": 1.0,
            },
        }],
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
          f"glTF bounds {mins}..{maxs} (MC2 Y will be 0..{-HEAD_TOP} after axis map + grounding)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
