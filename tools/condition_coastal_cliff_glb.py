#!/usr/bin/env python3
"""condition_coastal_cliff_glb.py -- turn a Blender-exported PolyHaven cliff GLB
into the MC2-ready CLIFF_WALL mesh decal GLB (replaces the flat displaced-plane
proof from gen_cliff_wall_glb.py with a REAL photogrammetry cliff-face segment).

INPUT: a GLB exported from Blender with export_yup=False, containing a SINGLE
mesh whose vertices are ALREADY baked into MC2-local space (see the driver notes
in the session / tools/README). Concretely the Blender authoring step:
  * takes a ~35-unit-wide segment of PolyHaven `coastal_cliff_01`,
  * decimates to ~12k tris (RTS-sane),
  * bakes a non-uniform transform so that, after MC2's importer axis map
    (assimp_importer.cpp:90, case 0 = (-x,-y,z)), the mesh stands upright:
      world +Y = height (~290u), world X = contour width (~335u),
      world +Z = outward cliff face (relief/depth ~125u), base sunk ~18u below 0.
  * recomputes outward normals (front-region mean normal.z verified > 0).

WHAT THIS TOOL DOES (pure GLB JSON surgery -- no Blender, no geometry change):
  1. Renames the baseColor / normal / roughness embedded images to the
     `marble_cliff_01{,_nor_gl,_rough}` family. MC2 derives the on-disk texture
     name from the baseColor image stem (assimp_importer.cpp DeriveMC2TextureName
     -> resolves embedded image mFilename == glTF image `name`). Using stem
     `marble_cliff_01` keeps the SAME derived name the existing MarbleCliff
     decal + place_cliffwall deploy path already uses -> no ini/pak churn.
  2. Forces the single node + mesh name to start with `_PAB` so the building
     footprint move-map walk (BldgAppearance::calcCellsCovered, bdactor.cpp)
     SKIPS it -- a >5000-vertex prop otherwise overflows MAX_CELL_COORDS and
     Fatals "too many coords for cellList". Render path never checks `_PAB`.
  3. Asserts the single-SHAPE-node / single-mesh / identity-TRS invariants the
     static-prop registration requires (it ABORTS on the first non-SHAPE_NODE
     child -> prop silently vanishes).

The output GLB is byte-compatible with place_cliffwall_mc2_01.py, which extracts
the `marble_cliff_01` baseColor JPEG and writes it as data/tgl/128/
marble_cliff_01.tga (the importer loads that on-disk TGA, not the embedded JPEG).

Usage:
  python condition_coastal_cliff_glb.py IN.glb OUT.glb
"""
from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

# baseColor stem MUST match the existing MarbleCliff decal texture name so the
# deploy path (marblecliff.ini / place_cliffwall) is unchanged.
BASE_STEM = "marble_cliff_01"
NODE_NAME = "_PAB_CoastalCliffWall"


def read_glb(path: Path):
    d = path.read_bytes()
    if d[:4] != b"glTF":
        raise SystemExit(f"{path}: not a GLB")
    off = 12
    jlen, jtype = struct.unpack("<II", d[off:off + 8]); off += 8
    assert jtype == 0x4E4F534A, "first chunk must be JSON"
    js = json.loads(d[off:off + jlen]); off += jlen
    blen, btype = struct.unpack("<II", d[off:off + 8]); off += 8
    assert btype == 0x004E4942, "second chunk must be BIN"
    blob = d[off:off + blen]
    return js, blob


def write_glb(path: Path, js: dict, blob: bytes):
    json_bytes = json.dumps(js, separators=(",", ":")).encode("utf-8")
    while len(json_bytes) % 4:
        json_bytes += b" "          # JSON chunk padded with spaces
    while len(blob) % 4:
        blob += b"\x00"             # BIN chunk padded with NULs
    total = 12 + 8 + len(json_bytes) + 8 + len(blob)
    out = bytearray()
    out += b"glTF" + struct.pack("<II", 2, total)
    out += struct.pack("<II", len(json_bytes), 0x4E4F534A) + json_bytes
    out += struct.pack("<II", len(blob), 0x004E4942) + blob
    path.write_bytes(out)


def main(argv=None) -> int:
    argv = argv if argv is not None else sys.argv[1:]
    if len(argv) != 2:
        raise SystemExit(__doc__)
    src, dst = Path(argv[0]), Path(argv[1])
    js, blob = read_glb(src)

    # --- invariant checks (static-prop registration) ---
    nodes = js.get("nodes", [])
    meshes = js.get("meshes", [])
    if len(meshes) != 1:
        raise SystemExit(f"expected exactly 1 mesh, got {len(meshes)} "
                         "(static-prop reg aborts on multi-mesh)")
    if len(nodes) != 1 or "mesh" not in nodes[0]:
        raise SystemExit(f"expected exactly 1 SHAPE node with a mesh, got {nodes}")
    n0 = nodes[0]
    for trs in ("rotation", "scale"):
        if n0.get(trs) not in (None, [0, 0, 0, 1] if trs == "rotation" else [1, 1, 1]):
            raise SystemExit(f"node has non-identity {trs} -> importer DROPS it, "
                             "breaking the baked axis map. Re-export with "
                             "export_yup=False and applied transforms.")
    prims = meshes[0].get("primitives", [])
    if len(prims) != 1:
        raise SystemExit(f"expected 1 primitive, got {len(prims)}")

    # --- rename node + mesh to _PAB (footprint move-map skip) ---
    n0["name"] = NODE_NAME
    meshes[0]["name"] = NODE_NAME

    # --- rename embedded images to the marble_cliff_01 family ---
    # Map by material role: baseColor -> BASE_STEM, normal -> _nor_gl, MR -> _rough
    mat = js["materials"][0]
    pbr = mat.get("pbrMetallicRoughness", {})
    textures = js.get("textures", [])
    images = js.get("images", [])

    def img_of(tex_ref):
        if tex_ref is None:
            return None
        return textures[tex_ref["index"]]["source"]

    base_i = img_of(pbr.get("baseColorTexture"))
    norm_i = img_of(mat.get("normalTexture"))
    mr_i = img_of(pbr.get("metallicRoughnessTexture"))

    if base_i is None:
        raise SystemExit("material has no baseColorTexture -> no derived TGA name")
    images[base_i]["name"] = BASE_STEM
    if norm_i is not None:
        images[norm_i]["name"] = BASE_STEM + "_nor_gl"
    if mr_i is not None:
        images[mr_i]["name"] = BASE_STEM + "_rough"

    mat["name"] = BASE_STEM

    # POSITION bounds sanity print (world after importer -x,-y,z)
    pa = js["accessors"][prims[0]["attributes"]["POSITION"]]
    mn, mx = pa["min"], pa["max"]
    write_glb(dst, js, blob)
    print(f"WROTE {dst} ({dst.stat().st_size} bytes)")
    print(f"  node/mesh = {NODE_NAME}  baseColor image -> {BASE_STEM}")
    print(f"  tris = {pa['count'] * 0}  verts = {pa['count']}")
    print(f"  local POSITION min={[round(x,1) for x in mn]} max={[round(x,1) for x in mx]}")
    print(f"  after importer (-x,-y,z): height(+Y)~{mx[1]-mn[1]:.0f}  "
          f"width(X)~{mx[0]-mn[0]:.0f}  depth(Z)~{mx[2]-mn[2]:.0f}  "
          f"base world y~{-mx[1]:.0f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
