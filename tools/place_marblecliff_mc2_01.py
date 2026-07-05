#!/usr/bin/env python3
"""place_marblecliff_mc2_01.py -- P2 cliff-mesh silhouette dressing.

Places one MarbleCliff building-class mesh instance on the mc2_01 cliff face
at the `cliff_side_on` bookmark focus point, as a first placement pass.

Prereqs (installed by the P0 recon, tools/install_cliff_dressing.py):
  - data/objects/object2.pak packet 1188  = MarbleCliff FIT (ObjectTypeNum row)
  - data/tgl/marblecliff.ini               = appearance ini (Source="MarbleCliffGLB")
  - data/tgl/MarbleCliffGLB.glb            = the scanned marble-cliff mesh

What this tool does (two deploy-side edits, no C++):

1. GLB conditioning (the P0 GLB shipped at ~4.4 world-unit extent = a pebble in
   a 128-u/cell world, and its footprint tripped a load-time Fatal):
     a. Bakes a uniform scale into the POSITION vertex buffer so the mesh spans
        the cliff face (~700 world units). NOTE: the static building import path
        (assimp_importer.cpp:442-478) does NOT propagate node scale/rotation --
        only translation -- so the scale MUST be baked into vertices, not set as
        a node TRS. The base ends up well below the origin (self-sinking, per the
        P0 grounding-mitigation: single-sample + yaw-only grounding).
     b. Renames the mesh node to start with "_PAB". BldgAppearance::calcCellsCovered
        / markMoveMap (bdactor.cpp:4648 etc.) record one move-map cell PER VERTEX
        for every shape whose node id does NOT start with "_PAB", and overflow
        MAX_CELL_COORDS=5000 (move.h:192) -> Fatal "too many coords for cellList".
        A 53901-vertex scanned mesh always overflows. "_PAB" makes the footprint
        walk skip the shape entirely (0 cells, pass-through dressing) -- and it is
        checked ONLY in passability/footprint code, never in the render path, so
        the mesh still draws. This is the clean data-only fix for a high-poly
        cliff mesh authored as a building.

2. Mission object injection: appends one 40-byte terrain-object record to
   mc2_01.pak packet 1 (the terrain-objects layer, format per
   code/objmgr.cpp countTerrainObjects:1289-1302):
     int32 objTypeNum, float x,y,z, float rotation, int32 damage,
     int32 teamId, int32 parentId, int32 pad, int32 pad
   objTypeNum == the objects-pak packet index (objtype.cpp:355 seekPacket) == 1188.
   The leading int32 object count is bumped by 1. All other packets are re-emitted
   byte-for-byte (pak_append.write_pak).

Grounding: z is set to the sampled cliff elevation but the engine re-grounds
buildings to a single terrain sample at the origin with yaw-only rotation
(bdactor.cpp:3966/3969) -- expect the far edges to float/clip on the slope. That
is the documented P0 limitation, accepted for this first read.

Idempotent: makes .bak_* backups once; re-running re-bakes from the pristine GLB
backup and rewrites packet 1 from the pristine pak backup.
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

# --- placement constants -----------------------------------------------------
OBJ_TYPE_NUM = 1188            # object2.pak packet index for MarbleCliff
# cliff_side_on bookmark focus point (tests/visual/bookmarks/mc2_01.json).
# The bookmark `pos` is the ground focus the camera looks AT (camera.cpp:2960-2962
# builds the eye by offsetting back/up from this focus), so it IS the cliff XY.
# Independently confirmed as a real cliff: heightfield elevation 665 with a
# ~510-unit drop within ~1150 world units, max adjacent slope 65.9 deg.
CLIFF_X = 55.7989
CLIFF_Y = 628.6685
CLIFF_Z = 665.45               # sampled elevation; engine re-grounds anyway
CLIFF_YAW = -127.25            # face toward the cliff_side_on camera
GLB_WORLD_SCALE = 160.0        # native ~4.4u -> ~700u tall/wide


def bake_glb(glb_path: Path, scale: float) -> None:
    import pygltflib
    bak = glb_path.with_suffix(glb_path.suffix + ".bak_p2unscaled")
    if not bak.exists():
        bak.write_bytes(glb_path.read_bytes())
        print(f"[glb] backup -> {bak.name}")
    # Always re-bake from the pristine backup for idempotency. Use load_binary
    # explicitly: pygltflib.load() dispatches on file extension, and the backup's
    # ".glb.bak_p2unscaled" suffix would wrongly route to the JSON loader.
    g = pygltflib.GLTF2().load_binary(str(bak))
    blob = bytearray(g.binary_blob())
    acc = g.accessors[0]                       # POSITION VEC3 float
    bv = g.bufferViews[acc.bufferView]
    base = (bv.byteOffset or 0) + (acc.byteOffset or 0)
    stride = bv.byteStride or 12
    mn = [1e30] * 3
    mx = [-1e30] * 3
    for i in range(acc.count):
        off = base + i * stride
        x, y, z = struct.unpack_from('<fff', blob, off)
        x *= scale; y *= scale; z *= scale
        struct.pack_into('<fff', blob, off, x, y, z)
        for k, v in enumerate((x, y, z)):
            mn[k] = min(mn[k], v)
            mx[k] = max(mx[k], v)
    acc.min = [float(v) for v in mn]
    acc.max = [float(v) for v in mx]
    g.nodes[0].scale = None
    g.nodes[0].matrix = None
    g.nodes[0].name = "_PAB_marblecliff"       # skip building footprint walk
    if g.meshes[0].name is None:
        g.meshes[0].name = "_PAB_marblecliff"
    g.set_binary_blob(bytes(blob))
    g.save(str(glb_path))
    print(f"[glb] baked scale={scale} node=_PAB height={mx[1]-mn[1]:.1f} "
          f"width={mx[0]-mn[0]:.1f} baseY={mn[1]:.1f} (buried) -> {glb_path.name}")


def inject_object(pak_path: Path) -> None:
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from pak_dump import read_packets as rd_dump, decode_packet
    from pak_append import (read_packets as rd_app, PacketRecord,
                            make_zlib_payload, write_pak, ST_ZLIB)

    bak = pak_path.with_suffix(pak_path.suffix + ".bak_cliffp2")
    if not bak.exists():
        bak.write_bytes(pak_path.read_bytes())
        print(f"[pak] backup -> {bak.name}")

    # Rebuild packet 1 from the pristine backup for idempotency.
    src_pkts = rd_dump(bak)
    p1 = decode_packet(src_pkts[1])
    cnt = struct.unpack_from('<i', p1, 0)[0]
    if len(p1) != 4 + cnt * 40:
        raise SystemExit(f"[pak] packet1 not a plain object array "
                         f"(size {len(p1)} != {4 + cnt * 40})")

    rec = struct.pack('<i fff f i i i i i',
                      OBJ_TYPE_NUM, CLIFF_X, CLIFF_Y, CLIFF_Z, CLIFF_YAW,
                      0, 0, -1, 0, 0)
    assert len(rec) == 40
    newp1 = bytearray(p1)
    struct.pack_into('<i', newp1, 0, cnt + 1)
    newp1 += rec

    recs = rd_app(bak)
    recs[1] = PacketRecord(1, ST_ZLIB, make_zlib_payload(bytes(newp1)))
    write_pak(recs, pak_path)
    print(f"[pak] injected MarbleCliff objType={OBJ_TYPE_NUM} at "
          f"({CLIFF_X},{CLIFF_Y},{CLIFF_Z}) yaw={CLIFF_YAW} "
          f"count {cnt}->{cnt+1}, {len(recs)} packets -> {pak_path.name}")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--deploy-root", required=True,
                    help="deploy tree root, e.g. '.../mc2-win64-v0.5.0'")
    ap.add_argument("--scale", type=float, default=GLB_WORLD_SCALE)
    args = ap.parse_args(argv)
    root = Path(args.deploy_root)
    glb = root / "data" / "tgl" / "MarbleCliffGLB.glb"
    pak = root / "data" / "missions" / "mc2_01.pak"
    for p in (glb, pak):
        if not p.exists():
            raise SystemExit(f"missing prerequisite: {p}")
    bake_glb(glb, args.scale)
    inject_object(pak)
    print("done. capture with scripts/run_visual_capture.py --mission mc2_01")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
