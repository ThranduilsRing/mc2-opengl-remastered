#!/usr/bin/env python3
"""OBJECT-BURIAL-AUDIT-1 -- per-object authored-Z / ground / mesh-bottom table.

WHY THIS EXISTS
---------------
`a75339bb0` (CPU-PHONG-RETIRE-1) is DEFAULT-ON and changes gameplay elevation
by up to p99 6.4 wu. From the moment it landed, every in-game observation of
"the turrets are sunk" mixes the turret question with that change. This script
answers the question OFFLINE -- from the shipped mission `.pak` heightfield, the
shipped `object2.pak` type table, and the COMPILED `.tgl` meshes -- while the
two effects are still separable. It launches nothing.

USAGE
-----
    py -3 scripts/object_burial_audit.py                     # all 24 stock maps
    py -3 scripts/object_burial_audit.py --missions mc2_03
    py -3 scripts/object_burial_audit.py --missions mc2_03 --name-filter turret
    py -3 scripts/object_burial_audit.py --csv out.csv --json out.json
    py -3 scripts/object_burial_audit.py --selftest          # negative controls

WHAT EACH COLUMN IS, AND WHERE IT COMES FROM
--------------------------------------------
  authored_z    `ObjDataLoader.vector.z` from the mission `.pak` terrain-object
                packet (code/objmgr.cpp:1291-1304: objTypeNum,x,y,z,rot,damage,
                teamId,parentId,pad,pad = 40 bytes/record after an int count).

  ground_phong  `MapData::terrainElevation(Vector3D)` (mclib/mapdata.cpp:2189)
  ground_plain  ported line-by-line, run TWICE: `_phong` with the legacy CPU
                Phong term (mapdata.cpp:2489-2522, phongAlpha 0.5 default from
                gameos_graphics.cpp:2154), `_plain` without it (the current
                default since a75339bb0). The dirt-displacement sibling block is
                NOT ported: it is inert by default because f9c12b53 frees
                cpuColorMap/cpuDispAlpha after atlas upload.

  mesh_min_up   Minimum of the model's VERTICAL axis over every SHAPE node of
                the LOD0 compiled `.tgl`, in object-local world units, including
                each node's `nodeCenter` offset.

                THE VERTICAL AXIS IS `TG_TypeVertex.position.y`, NOT `.z`. Two
                independent source facts pin this:
                  * mclib/tgl.cpp:1068-1076 -- the ASE reader maps
                    `position.x = -ase.x`, `position.z = ase.y`, `position.y =
                    ase.z`. 3DS Max is Z-up, so ase.z (up) lands in `.y`.
                  * mclib/bdactor.cpp:4046-4048 -- the draw transform is
                    `xlatPosition.x=-position.x; xlatPosition.y=position.z;
                    xlatPosition.z=position.y`, i.e. WORLD Z (elevation) is fed
                    into the shape frame's Y.
                Object yaw is `Stuff::EulerAngles(0, yaw, 0)` and UP_AXIS is
                Y_Axis (mclib/stuff/vector3d.hpp:53), so rotation cannot change
                a vertex's vertical coordinate -- `mesh_min_up` is exact for any
                rotation, not an approximation.

  body_min_up   Same, but over only the SHALLOWEST-depth drawn node(s) -- the
  deepest_node  object's body. A whole-model `min_up` can be set by one tiny
                decorative sub-node; `L_TurretGenerator` reads -24.93 for the
                model but -0.64 for its 67-vertex `PowerGenerator` body, the
                -24.93 coming entirely from an 8-vertex `Coil` card that
                straddles the origin symmetrically (-24.93 .. +24.93).

  animated      The appearance `.ini` declares [Animation:N] blocks, so the
                compiled `.tgl` is a BIND POSE, not the only authored pose. Every
                `*Popup`/`*Camo` turret parks its `TurretRoot` at nodeCenter.y =
                -61.9 in the CLOSED state by design. A large negative extent on
                an animated prop is authoring, not a defect.

  burial_resnap             -mesh_min_up. Applies when the drawn base tracks the
                            current ground.
  burial_authored_{plain,   ground - (authored_z + mesh_min_up). Applies when the
    phong}                  drawn base is the frozen authored Z.
  burial_change_wu          How far this object's VISIBLE burial moved when
                            a75339bb0 landed: `+ground_delta` for a RE-SNAPPING
                            class, EXACTLY 0 for every other class. See
                            `burial_change()` for the derivation and for the
                            RESNAP-INVERSION-1 note on why the first cut of this
                            file had those two cases backwards. NOT
                            `burial_authored_plain - burial_authored_phong`:
                            that difference describes a world in which the DRAWN
                            ground was Phong-smoothed, which it has not been
                            since 75b8ebef (2026-06-29).

RE-SNAP BEHAVIOUR (verified at source, 2026-07-31)
--------------------------------------------------
`position.z = ...` appears in exactly TWO object update paths in the whole of
code/: `code/bldng.cpp:830` and `code/gate.cpp:361`, both
`position.z = TerrainRuntime::groundElevation(position)` under `if (inView)`.
`code/turret.cpp` and `code/terrobj.cpp` contain NO assignment to `position.z`
at all -- turret.cpp's four occurrences (:543, :760, :865, :911) are reads.
So BUILDING/TREEBUILDING/GATE are pulled onto the CURRENT terrainElevation on
their first in-view frame; TURRET / TREE / TERRAINOBJECT keep the authored Z for
the whole mission.

ONE PATH DOES NOT FOLLOW THAT RULE, and the table cannot see it: the GPU
static-prop recipe (`BldgAppearance::registerStatic` mclib/bdactor.cpp:4807 and
`TreeAppearance::registerStatic` :7857) bakes `xlatPosition.y =
TerrainRuntime::groundElevation(position)` -- the CURRENT ground -- instead of
`position.z`, for every class it admits (objmgr.cpp:1617-1660 feeds it terrain
objects, buildings, turrets AND gates). Whether a given object ends up on that
path or on the legacy `BldgAppearance::update` path (:4047, which uses
`position.z`) is a runtime decision. Both burial columns are therefore reported
per row rather than one being declared correct.

DATA AUTHORITY
--------------
Missions come from `tools/mc2data/stock_missions.py` (byte-verified against the
shipped `mission.fst`). `object2.pak` and the compiled `.tgl`/`.ini` are read
from a deploy lane; `.tgl` are ENGINE-ONLY (no source-tree counterpart -- see
tools/mc2data/stock_corpus.py), so the deployed archive IS the authority. The
resolver prints which lane it used and refuses to guess.
"""
from __future__ import annotations

import argparse
import csv
import json
import math
import re
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
sys.path.insert(0, str(ROOT / "tools" / "mc2data"))

from terrain_height_audit import (  # noqa: E402
    read_packets, decode_packet, POSTCOMP_VERTEX_SIZE, ELEV_OFFSET,
    WORLD_UNITS_PER_VERTEX, VALID_GRID_SIDES,
)
from stock_missions import (  # noqa: E402
    StockDataError, add_missions_dir_arg, resolve_from_args,
    fst_members, fst_blob,
)

# --- constants, all read FROM THE C++ ----------------------------------------

# mclib/terrain.cpp:111,116,121
VERTICES_BLOCK_SIDE = 20
# mclib/vertex.h:24-25
BOTTOMRIGHT, BOTTOMLEFT = 0, 1
# GameOS/gameos/gameos_graphics.cpp:2154 -- terrain_phong_alpha_ = 0.5f.
# Only writer is gos_SetTerrainPhongAlpha, whose only callers are the ImGui
# GraphicsOptionsWindow slider + its Reset button (which resets to 0.5f).
PHONG_ALPHA_DEFAULT = 0.5
# mclib/msl.cpp:83
CURRENT_SHAPE_VERSION = 0xBAFDECAF
# mclib/tgl.h:471-472
TYPE_NODE, SHAPE_NODE = 0, 1
# mclib/tgl.h:189
TG_NODE_ID = 25
# sizeof(TG_TypeVertex)   = Point3D(12) + Vector3D(12) + DWORD(4)
TG_TYPEVERTEX_SIZE = 28
# sizeof(TG_TypeTriangle) = DWORD[3] + DWORD + DWORD + TG_UVData(24) + Vector3D
TG_TYPETRIANGLE_SIZE = 56
# sizeof(TG_TinyTexture)  = DWORD + DWORD + bool -> 12 (4-byte align)
TG_TINYTEXTURE_SIZE = 12
# sizeof(TG_Texture)      = char[256] + DWORD + DWORD + bool -> 268
TG_TEXTURE_SIZE = 268
_TYPEVERT = struct.Struct("<3f3fI")

# code/dobjtype.h:32-59 -- enum ObjectTypeClass, written into every object2.pak
# packet as [ObjectClass] ObjectTypeNum.
OBJECT_TYPE_CLASS = {
    -1: "CRAPPY", 0: "TREE", 1: "BUILDING", 2: "BATTLEMECH", 3: "VEHICLE",
    4: "EXPLOSION", 5: "FIRE", 6: "LASER", 7: "SMOKES", 8: "PROJECTILE",
    9: "MECHARM", 10: "MAPICON", 11: "TERRAINOBJECT", 12: "ARTILLERY",
    13: "MINE", 14: "ELEMENTAL", 15: "BRIDGE", 16: "JET", 17: "PROJLASER",
    18: "TREEBUILDING", 19: "CAMERADRONE", 20: "TRAINCAR", 21: "TURRET",
    22: "GATE", 23: "KLIEG_LIGHT", 24: "WEAPONBOLT",
}
# Classes whose update() re-snaps position.z onto the terrain every in-view
# frame. Verified at source; see module docstring.
RESNAP_CLASSES = {"BUILDING", "TREEBUILDING", "GATE"}


def burial_change(resnaps: bool, g_plain: float, g_phong: float,
                  min_up: float) -> float:
    """How far an object's VISIBLE burial moved when a75339bb0 landed.

    RESNAP-INVERSION-1 (2026-07-31).  The first cut of this file had the two
    classes exactly backwards, because it assumed the DRAWN ground was Phong-
    smoothed before a75339bb0.  It never was.  a75339bb0 changed
    `MapData::terrainElevation()` -- GAMEPLAY elevation only.  The rasterized
    surface is `shaders/terrain_lod_chunk.vert/.frag`; its optional TCS/TES pair
    is a 20-line barycentric pass-through with no displacement, built only under
    the default-OFF cliff-tess gate (`gos_terrain_lod_chunk.cpp:1405-1418`).  The
    one terrain TES that DID Phong-smooth, `shaders/gos_terrain.tese`, has had no
    C++ loader since 75b8ebef deleted the tess-solid draw on 2026-06-29 -- which
    is the entire premise of a75339bb0's own commit message.  So the drawn ground
    is `g_plain` on BOTH sides of the change, and only the object's base moves:

      burial = drawn_ground - (base_z + mesh_min_up),  drawn_ground = g_plain

      re-snapping class (bldng.cpp:829, gate.cpp:361 assign position.z from
      TerrainRuntime::groundElevation -> ... -> MapData::terrainElevation, the
      exact function a75339bb0 edited):
          base_z = g_phong (pre) -> g_plain (post)
          change = (-min_up) - (-min_up - (g_phong - g_plain)) = +ground_delta

      non-re-snapping class (TURRET/TERRAINOBJECT/TREE keep the authored Z; no
      assignment to position.z exists in turret.cpp or terrobj.cpp):
          base_z = authored_z on both sides  ->  change = EXACTLY 0

    The delta is independent of how faithfully `g_plain` models the raster,
    because the drawn-ground term cancels.  Proven by NEGATIVE CONTROL 9.
    """
    if min_up != min_up:                 # NaN: mesh never parsed
        return float("nan")
    return (g_phong - g_plain) if resnaps else 0.0

DEPLOY_LANES = (
    "A:/Games/mc2-opengl/mc2-win64-v0.4c",
    "A:/Games/mc2-opengl/mc2-win64-v0.4",
    "A:/Games/mc2-opengl/releases/mc2-win64-v0.5.1",
    "A:/Games/mc2-opengl/releases/mc2-win64-v0.4d-rc1",
)

# mission .pak terrain-object record: code/objmgr.cpp:1291-1304
#   i32 objTypeNum ; f32 x,y,z ; f32 rotation ; i32 damage,teamId,parentId ;
#   i32 pad,pad  = 40 bytes
OBJREC = struct.Struct("<i4f5i")
assert OBJREC.size == 40


# =============================================================================
# Compiled .tgl binary parser  (mclib/msl.cpp:242 + mclib/tgl.cpp:472/:622)
# =============================================================================

class _Reader:
    __slots__ = ("b", "p")

    def __init__(self, b: bytes):
        self.b, self.p = b, 0

    def i(self) -> int:
        v = struct.unpack_from("<i", self.b, self.p)[0]
        self.p += 4
        return v

    def f(self) -> float:
        v = struct.unpack_from("<f", self.b, self.p)[0]
        self.p += 4
        return v

    def raw(self, n: int) -> bytes:
        if self.p + n > len(self.b):
            raise ValueError(f"tgl truncated: want {n} at {self.p}/{len(self.b)}")
        v = self.b[self.p:self.p + n]
        self.p += n
        return v


class TglParseError(RuntimeError):
    pass


def parse_tgl(blob: bytes) -> dict:
    """Parse a compiled `.tgl` (TG_TypeMultiShape::LoadBinaryCopy).

    Layout, straight off the SaveBinaryCopy writers -- NOT guessed:
      u32 version == 0xBAFDECAF                     (mclib/msl.cpp:83,255)
      i32 numTG_TypeShapes ; i32 numTextures        (msl.cpp:257-258)
      TG_Texture[numTextures]                       (msl.cpp:264, 268 B each)
      per node (msl.cpp:281-296):
        i32 nodeType
        if SHAPE_NODE:  i32 nVerts; TG_TypeVertex[nVerts]
                        i32 nTris ; TG_TypeTriangle[nTris]
                        i32 nTex  ; TG_TinyTexture[nTex]
        i32*6 nodeCenter,relativeNodeCenter ; char[25] nodeId ; char[25] parentId

    >>> THE TRAP <<<  For SHAPE_NODE the node header (`nodeCenter` /
    `relativeNodeCenter` / `nodeId` / `parentId`) is written AFTER the geometry,
    because TG_TypeShape::LoadBinaryCopy (mclib/tgl.cpp:622) does NOT call the
    TG_TypeNode base -- it inlines the tail at :731-745. A parser that assumes
    the node header comes first (as it does for TYPE_NODE) desynchronises on the
    FIRST shape node and recovers only the handful of vertices that happen to
    land in the resynced window. That is exactly the failure a prior lane hit
    (16 of 75 vertices on l_turretgenerator). The `consumed == len(blob)` check
    below is what makes the correct layout self-proving.
    """
    if len(blob) < 12:
        raise TglParseError("too short")
    ver = struct.unpack_from("<I", blob, 0)[0]
    if ver != CURRENT_SHAPE_VERSION:
        raise TglParseError(f"bad version {ver:#x} (want {CURRENT_SHAPE_VERSION:#x})")
    r = _Reader(blob)
    r.p = 4
    num_shapes = r.i()
    num_textures = r.i()
    if not (0 <= num_shapes <= 4096) or not (0 <= num_textures <= 4096):
        raise TglParseError(f"implausible counts shapes={num_shapes} tex={num_textures}")
    texblob = r.raw(TG_TEXTURE_SIZE * num_textures)
    textures = [texblob[t * TG_TEXTURE_SIZE:t * TG_TEXTURE_SIZE + 256]
                .split(b"\0", 1)[0].decode("latin-1")
                for t in range(num_textures)]
    nodes = []
    for i in range(num_shapes):
        nt = r.i()
        node = {"type": nt, "nverts": 0, "ntris": 0, "verts": []}
        if nt == SHAPE_NODE:
            nv = r.i()
            if nv < 0 or nv > 1_000_000:
                raise TglParseError(f"node {i}: implausible nVerts {nv}")
            vb = r.raw(TG_TYPEVERTEX_SIZE * nv)
            node["nverts"] = nv
            node["verts"] = [
                _TYPEVERT.unpack_from(vb, TG_TYPEVERTEX_SIZE * k)[:3]
                for k in range(nv)
            ]
            nt_tris = r.i()
            if nt_tris < 0 or nt_tris > 2_000_000:
                raise TglParseError(f"node {i}: implausible nTris {nt_tris}")
            r.raw(TG_TYPETRIANGLE_SIZE * nt_tris)
            node["ntris"] = nt_tris
            ntex = r.i()
            if ntex < 0 or ntex > 4096:
                raise TglParseError(f"node {i}: implausible nTex {ntex}")
            r.raw(TG_TINYTEXTURE_SIZE * ntex)
        elif nt != TYPE_NODE:
            # msl.cpp:292-315 -- the engine ABORTS the whole shape here too.
            raise TglParseError(f"unknown nodeType {nt} at node {i}/{num_shapes}")
        node["nodeCenter"] = (r.f(), r.f(), r.f())
        node["relCenter"] = (r.f(), r.f(), r.f())
        node["id"] = r.raw(TG_NODE_ID).split(b"\0", 1)[0].decode("latin-1")
        node["parent"] = r.raw(TG_NODE_ID).split(b"\0", 1)[0].decode("latin-1")
        nodes.append(node)
    if r.p != len(blob):
        raise TglParseError(
            f"trailing bytes: consumed {r.p} of {len(blob)} -- layout is wrong")
    return {"version": ver, "textures": textures, "nodes": nodes,
            "bytes": len(blob)}


def tgl_node_extents(parsed: dict) -> list[dict]:
    """Per-SHAPE-node vertical extent, in object-local world units.

    UP = TG_TypeVertex.position.y (index 1). See module docstring for the two
    source facts that pin the axis. Each SHAPE node's vertices are node-local;
    the node's absolute offset is its own `nodeCenter` (verified: for every
    stock node, relCenter == nodeCenter_child - nodeCenter_parent, and
    baseRotation is identity at load -- msl.cpp:170-173 initialises it to the
    identity quaternion and only the runtime SetNodeRotation at msl.cpp:1320
    ever changes it).

    NOT APPLIED, and it matters: per-node ANIMATION offsets (`TG_Animation.pos`
    / `.quat`, shipped separately in the `.agl`). This is the BIND POSE. For an
    animated prop -- e.g. every `*Popup` / `*Camo` turret, whose `TurretRoot`
    node is parked at nodeCenter.y = -61.9 in the closed state -- the bind pose
    is the CLOSED pose and a large negative extent is the authored design, not
    a defect. The `animated` column flags those rows.
    """
    by_id = {n["id"]: n for n in parsed["nodes"]}

    def depth(node, seen=None):
        seen = seen or set()
        p = node["parent"]
        if p in ("None", "", "none") or p not in by_id or p in seen:
            return 0
        seen.add(node["id"])
        return 1 + depth(by_id[p], seen)

    out = []
    for node in parsed["nodes"]:
        if node["type"] != SHAPE_NODE or not node["verts"]:
            continue
        off = node["nodeCenter"][1]
        ys = [off + v[1] for v in node["verts"]]
        out.append({"id": node["id"], "parent": node["parent"],
                    "depth": depth(node),
                    "nverts": len(ys), "min_up": min(ys), "max_up": max(ys)})
    return out


def tgl_vertical_extent(parsed: dict):
    """(min_up, max_up, total_verts, deepest_node_id, deepest_node_nverts,
        root_min_up).

    `root_min_up` is the extent of the SHALLOWEST-DEPTH drawn node(s) -- the
    object's BODY. It is reported separately because a whole-model `min_up` can
    be dominated by one tiny sub-node: `L_TurretGenerator` reads -24.93 over the
    model, but that is entirely an 8-vertex / 4-triangle `Coil` card straddling
    the origin symmetrically (-24.93 .. +24.93); the 67-vertex `PowerGenerator`
    body only reaches -0.64. Reporting one number there would have said
    "half-buried building" about a decoration.
    """
    nodes = tgl_node_extents(parsed)
    if not nodes:
        return (float("nan"), float("nan"), 0, "", 0, float("nan"))
    lo = min(n["min_up"] for n in nodes)
    hi = max(n["max_up"] for n in nodes)
    tot = sum(n["nverts"] for n in nodes)
    deepest = min(nodes, key=lambda n: n["min_up"])
    d0 = min(n["depth"] for n in nodes)
    root_lo = min(n["min_up"] for n in nodes if n["depth"] == d0)
    return (lo, hi, tot, deepest["id"], deepest["nverts"], root_lo)


# =============================================================================
# Deploy-lane data access (object2.pak, tgl.fst, loose data/tgl)
# =============================================================================

@dataclass
class GameData:
    lane: Path
    obj_types: dict           # objTypeNum -> dict(class,name,appearance)
    tgl_fst: dict             # "name.tgl" (lowercase) -> bytes
    ini_text: dict            # "name.ini" (lowercase) -> str
    loose_ini: set = field(default_factory=set)
    notes: list = field(default_factory=list)


def _parse_fitini_kv(text: str) -> list[tuple[str, dict]]:
    """[(block, {KEY_lower: value})] for a FitIni text buffer.

    FIRST occurrence of a duplicated key wins, because that is what the engine
    does: FitIniFile::readIdString (mclib/inifile.cpp:1404-1434) seeks to the
    top of the block and scans FORWARD, returning the first line that matches.
    It matters: `dunebuggyprop.ini` declares `st FileName` TWICE (the LOD1 line
    was never renamed to `FileName1`), and last-wins resolves it to
    `dunebuggypropL1.tgl`, which does not exist in tgl.fst -- 19 phantom
    "absent shape" rows. The engine loads `dunebuggyprop.tgl` and is fine.
    """
    out, cur, blk = [], {}, None
    for raw in text.splitlines():
        line = raw.split("//", 1)[0].strip()
        if not line:
            continue
        if line.startswith("[") and line.endswith("]"):
            if blk is not None:
                out.append((blk, cur))
            blk, cur = line[1:-1], {}
            continue
        if blk is None:
            continue
        m = re.match(r"^(?:[A-Za-z]{1,3}\s+)?([A-Za-z_]\w*)\s*=\s*(.*)$", line)
        if m:
            val = m.group(2).strip().strip('"')
            cur.setdefault(m.group(1).lower(), val)
    if blk is not None:
        out.append((blk, cur))
    return out


def load_game_data(lane: Path | None = None) -> GameData:
    lanes = [Path(lane)] if lane else [Path(p) for p in DEPLOY_LANES]
    chosen = None
    for p in lanes:
        if (p / "data/objects/object2.pak").is_file() and (p / "tgl.fst").is_file():
            chosen = p
            break
    if chosen is None:
        raise StockDataError(
            "no deploy lane with BOTH data/objects/object2.pak and tgl.fst.\n"
            "  searched: " + ", ".join(str(p) for p in lanes) + "\n"
            "  pass --deploy-lane <dir> explicitly.")

    notes = []

    # --- object2.pak: objTypeNum -> FitIni text
    obj_types: dict[int, dict] = {}
    for idx, t, payload in read_packets(chosen / "data/objects/object2.pak"):
        dec = decode_packet(idx, t, payload)
        if not dec or not dec.lstrip().startswith(b"FITini"):
            continue
        blocks = dict(_parse_fitini_kv(dec.decode("latin-1")))
        oc = blocks.get("ObjectClass", {})
        ot = blocks.get("ObjectType", {})
        try:
            cls_num = int(oc.get("objecttypenum", "-999"))
        except ValueError:
            continue
        obj_types[idx] = {
            "class_num": cls_num,
            "class": OBJECT_TYPE_CLASS.get(cls_num, f"UNKNOWN({cls_num})"),
            "name": ot.get("name", ""),
            "appearance": ot.get("appearancename", ""),
        }

    # --- tgl.fst: compiled shapes + the .ini that names them.
    tgl_fst: dict[str, bytes] = {}
    ini_text: dict[str, str] = {}
    data, members = fst_members(chosen / "tgl.fst")
    for nm, off, size, real in members:
        if not nm.startswith("data/tgl/"):
            continue
        base = nm[len("data/tgl/"):]
        if base.endswith(".tgl"):
            blob = fst_blob(data, off, size, real)
            if blob is not None:
                tgl_fst[base] = blob
        elif base.endswith(".ini"):
            blob = fst_blob(data, off, size, real)
            if blob is not None:
                ini_text[base] = blob.decode("latin-1")

    # --- loose data/tgl SHADOWS the FST (mclib/file.cpp resolution ladder:
    #     mod overlay -> loose file on disk -> FastFile). Record and honour it.
    loose = set()
    loose_dir = chosen / "data/tgl"
    if loose_dir.is_dir():
        for p in loose_dir.iterdir():
            if not p.is_file():
                continue
            b = p.name.lower()
            if b.endswith(".ini"):
                loose.add(b)
                ini_text[b] = p.read_text(errors="ignore")
            elif b.endswith(".tgl"):
                loose.add(b)
                tgl_fst[b] = p.read_bytes()
    if loose:
        notes.append(f"{len(loose)} loose data/tgl file(s) SHADOW tgl.fst in {chosen}")

    return GameData(lane=chosen, obj_types=obj_types, tgl_fst=tgl_fst,
                    ini_text=ini_text, loose_ini=loose, notes=notes)


def appearance_is_animated(gd: GameData, appearance: str) -> bool:
    """True if the appearance .ini declares any [Animation:N] / [AnimationNode]
    block (mclib/bdactor.cpp bdAnimData / rotationalNodeId). For those the
    compiled `.tgl` bind pose is only ONE of several authored poses."""
    txt = gd.ini_text.get((appearance or "").lower() + ".ini")
    if not txt:
        return False
    return bool(re.search(r"^\s*\[\s*Animation", txt, re.M | re.I))


def resolve_lod0_shape(gd: GameData, appearance: str) -> tuple[str, str]:
    """appearance name -> (tgl member basename, why).

    mclib/bdactor.cpp:843-996: `<tglPath>/<appearance>.ini`, then [TGLData]
    `FileName` (single-LOD) or `FileName0` (multi-LOD, LOD0 = the close-range
    shape). The engine then calls LoadTGMultiShapeFromASE("<name>.ase") which,
    with no loose .ase on disk (0 in every deploy lane -- measured), falls
    through to LoadBinaryCopy("<name>.tgl") at mclib/msl.cpp:727.
    """
    if not appearance:
        return ("", "no AppearanceName")
    ini_key = appearance.lower() + ".ini"
    txt = gd.ini_text.get(ini_key)
    if txt is None:
        return ("", f"no {ini_key}")
    blocks = dict(_parse_fitini_kv(txt))
    tgl = blocks.get("TGLData", {})
    for key in ("filename", "filename0"):
        if tgl.get(key):
            return (tgl[key].lower() + ".tgl", f"[TGLData] {key}")
    return ("", f"{ini_key} has no FileName/FileName0")


# =============================================================================
# MapData::terrainElevation port (mclib/mapdata.cpp:2189)
# =============================================================================

class Heightfield:
    """Decoded PostcompVertex grid + a line-by-line port of terrainElevation."""

    def __init__(self, side: int, blocks: bytes):
        self.side = side
        self.wupv = WORLD_UNITS_PER_VERTEX
        self.one_over_wupv = 1.0 / WORLD_UNITS_PER_VERTEX
        self.world_units_map_side = side * WORLD_UNITS_PER_VERTEX
        self.map_top_left_x = -self.world_units_map_side / 2.0
        self.map_top_left_y = self.world_units_map_side / 2.0
        n = side * side
        self.elev = [0.0] * n
        self.nrm = [(0.0, 0.0, 0.0)] * n
        for i in range(n):
            base = i * POSTCOMP_VERTEX_SIZE
            self.elev[i] = struct.unpack_from("<f", blocks, base + ELEV_OFFSET)[0]
            self.nrm[i] = struct.unpack_from("<3f", blocks, base)

    # mclib/terrain.cpp:1996
    def is_valid(self, x: float, y: float) -> bool:
        m = self.world_units_map_side / 2.0
        return (-m < x < m) and (-m < y < m)

    def elevation(self, x: float, y: float, phong: bool) -> float:
        if not self.is_valid(x, y):
            return 0.0
        side = self.side
        wupv, oow = self.wupv, self.one_over_wupv

        ul_x = math.floor(x * oow) * wupv
        uly = math.floor(y * oow)
        if _f32(y * oow) != _f32(uly):
            uly += 1.0
        ul_y = uly * wupv

        _ulx = ul_x * oow
        _uly = ul_y * oow
        mo_x = int(math.floor(_ulx))
        mo_y = int(math.floor(_uly))

        vms = side  # verticesBlockSide * blocksMapSide == realVerticesMapSide
        mo_x += vms >> 1
        mo_y = (vms >> 1) - mo_y
        if not (0 <= mo_x < vms - 1):
            return 0.0
        if not (0 <= mo_y < vms - 1):
            return 0.0

        i1 = mo_x + mo_y * side
        i2 = (mo_x + 1) + mo_y * side
        i3 = (mo_x + 1) + (mo_y + 1) * side
        i4 = mo_x + (mo_y + 1) * side
        e1, e2, e3, e4 = self.elev[i1], self.elev[i2], self.elev[i3], self.elev[i4]

        tri_pos = (wupv * math.floor(_ulx), wupv * math.floor(_uly), e1)

        # topLeftVertex is a CAMERA-derived value (mapdata.cpp:987-1000), but it
        # CANCELS: xby2 = ((mo.x - TLV.x) & 1) ^ (TLV.x & 1) == mo.x & 1 for
        # either parity of TLV.x (two's-complement low bit of a difference is
        # the XOR of the low bits). Same for y. So uvMode is camera-independent
        # and this offline port is exact, not an approximation.
        xby2 = mo_x & 1
        yby2 = mo_y & 1
        uv_mode = BOTTOMRIGHT if (bool(xby2) == bool(yby2)) else BOTTOMLEFT

        if uv_mode == BOTTOMRIGHT:
            dx = abs(x - ul_x)
            dy = abs(ul_y - y)
            if dx > dy:
                t0 = tri_pos
                t1 = (t0[0] + wupv, t0[1], e2)
                t2 = (t1[0], t0[1] - wupv, e3)
                tv0 = _sub(t1, t0)
                tv1 = _sub(t2, t0)
                pv = (i1, i2, i3)
            else:
                t0 = tri_pos
                t1 = (t0[0] + wupv, t0[1] - wupv, e3)
                t2 = (t0[0], t1[1], e4)
                tv0 = _sub(t2, t0)
                tv1 = _sub(t1, t0)
                pv = (i1, i3, i4)
        else:
            dx = abs((ul_x + wupv) - x)
            dy = abs(ul_y - y)
            if dx > dy:
                t1 = tri_pos
                t0 = (tri_pos[0] + wupv, tri_pos[1], e2)
                t2 = (t1[0], t1[1] - wupv, e4)
                tv0 = _sub(t1, t0)
                tv1 = _sub(t2, t0)
                pv = (i2, i1, i4)
            else:
                t0 = (tri_pos[0] + wupv, tri_pos[1], e2)
                t1 = (tri_pos[0], t0[1] - wupv, e4)
                t2 = (t0[0], t1[1], e3)
                tv0 = _sub(t2, t0)
                tv1 = _sub(t1, t0)
                pv = (i2, i4, i3)
            dx = -dx

        tv0 = _norm(tv0)
        tv1 = _norm(tv1)
        perp = _cross(tv0, tv1)

        result = 0.0
        if perp[2] != 0.0:
            if perp[2] < 0.0:
                perp = (-perp[0], -perp[1], -perp[2])
            result = dx * (perp[0] / perp[2])
            result += (-dy) * (perp[1] / perp[2])
            result = -result
            result += t0[2]

        if not phong or PHONG_ALPHA_DEFAULT <= 0.0:
            return result

        # --- legacy CPU Phong term (mapdata.cpp:2489-2522) ------------------
        tri = (t0, t1, t2)
        x0, y0 = t0[0], t0[1]
        x1, y1 = t1[0], t1[1]
        x2, y2 = t2[0], t2[1]
        det = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2)
        b0 = b1 = b2 = 0.333
        if abs(det) > 1e-6:
            b0 = ((y1 - y2) * (x - x2) + (x2 - x1) * (y - y2)) / det
            b1 = ((y2 - y0) * (x - x2) + (x0 - x2) * (y - y2)) / det
            b2 = 1.0 - b0 - b1
        pz = result
        proj = []
        for k in range(3):
            n = self.nrm[pv[k]]
            d = ((x - tri[k][0]) * n[0] + (y - tri[k][1]) * n[1]
                 + (pz - tri[k][2]) * n[2])
            proj.append(pz - d * n[2])
        phong_z = b0 * proj[0] + b1 * proj[1] + b2 * proj[2]
        return result + PHONG_ALPHA_DEFAULT * (phong_z - result)


def _f32(v: float) -> float:
    return struct.unpack("<f", struct.pack("<f", v))[0]


def _sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def _norm(v):
    n = math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])
    return v if n == 0.0 else (v[0] / n, v[1] / n, v[2] / n)


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])


# =============================================================================
# Mission parsing
# =============================================================================

def load_mission(pak: Path) -> tuple[Heightfield, list[dict]]:
    packets = read_packets(pak)
    hf = None
    objs = []
    for idx, t, payload in packets:
        dec = decode_packet(idx, t, payload)
        if dec is None or not dec:
            continue
        if hf is None and len(dec) % POSTCOMP_VERTEX_SIZE == 0:
            cells = len(dec) // POSTCOMP_VERTEX_SIZE
            side = math.isqrt(cells)
            if side * side == cells and side in VALID_GRID_SIDES:
                hf = Heightfield(side, dec)
                continue
        if not objs and len(dec) >= 4:
            count = struct.unpack_from("<i", dec, 0)[0]
            if 0 < count and 4 + count * OBJREC.size == len(dec):
                for i in range(count):
                    (ot, x, y, z, rot, dmg, team, parent, _p0, _p1) = \
                        OBJREC.unpack_from(dec, 4 + i * OBJREC.size)
                    objs.append(dict(idx=i, obj_type=ot, x=x, y=y, z=z,
                                     rot=rot, damage=dmg, team=team,
                                     parent=parent))
    if hf is None:
        raise StockDataError(f"{pak.name}: no MapData heightfield packet found")
    return hf, objs


# =============================================================================
# The table
# =============================================================================

COLUMNS = [
    "mission", "obj_index", "obj_type_num", "class", "type_name",
    "appearance", "tgl", "x", "y", "authored_z", "rot",
    "ground_plain", "ground_phong", "ground_delta",
    "authored_minus_plain", "authored_minus_phong",
    "mesh_min_up", "mesh_max_up", "mesh_verts",
    "body_min_up", "deepest_node", "deepest_node_verts", "animated",
    "resnaps", "burial_resnap", "burial_authored_plain",
    "burial_authored_phong", "burial_change_wu", "parse_note",
]


def build_rows(mission: str, hf: Heightfield, objs: list[dict],
               gd: GameData, tgl_cache: dict) -> list[dict]:
    rows = []
    for o in objs:
        ot = gd.obj_types.get(o["obj_type"])
        cls = ot["class"] if ot else "UNRESOLVED"
        name = ot["name"] if ot else ""
        appearance = ot["appearance"] if ot else ""

        tgl_name, why = ("", "no object type")
        if ot:
            tgl_name, why = resolve_lod0_shape(gd, appearance)

        nan = float("nan")
        if tgl_name and tgl_name in tgl_cache:
            min_up, max_up, nv, dnode, dnv, body_up, note = tgl_cache[tgl_name]
        elif tgl_name:
            blob = gd.tgl_fst.get(tgl_name)
            if blob is None:
                min_up = max_up = body_up = nan
                nv, dnode, dnv = 0, "", 0
                note = f"{tgl_name} absent from tgl.fst and loose dir"
            else:
                try:
                    parsed = parse_tgl(blob)
                    (min_up, max_up, nv, dnode, dnv, body_up) = \
                        tgl_vertical_extent(parsed)
                    note = "" if nv else "no SHAPE_NODE geometry"
                except (TglParseError, ValueError) as exc:
                    min_up = max_up = body_up = nan
                    nv, dnode, dnv = 0, "", 0
                    note = f"tgl parse failed: {exc}"
            tgl_cache[tgl_name] = (min_up, max_up, nv, dnode, dnv, body_up, note)
        else:
            min_up = max_up = body_up = nan
            nv, dnode, dnv, note = 0, "", 0, why
        animated = appearance_is_animated(gd, appearance) if ot else False

        g_plain = hf.elevation(o["x"], o["y"], phong=False)
        g_phong = hf.elevation(o["x"], o["y"], phong=True)
        resnaps = cls in RESNAP_CLASSES

        burial_resnap = -min_up if min_up == min_up else float("nan")
        b_plain = (g_plain - (o["z"] + min_up)) if min_up == min_up else float("nan")
        b_phong = (g_phong - (o["z"] + min_up)) if min_up == min_up else float("nan")
        change = burial_change(resnaps, g_plain, g_phong, min_up)

        rows.append({
            "mission": mission, "obj_index": o["idx"],
            "obj_type_num": o["obj_type"], "class": cls, "type_name": name,
            "appearance": appearance, "tgl": tgl_name,
            "x": round(o["x"], 4), "y": round(o["y"], 4),
            "authored_z": round(o["z"], 6), "rot": round(o["rot"], 3),
            "ground_plain": round(g_plain, 6), "ground_phong": round(g_phong, 6),
            "ground_delta": round(g_phong - g_plain, 6),
            "authored_minus_plain": round(o["z"] - g_plain, 6),
            "authored_minus_phong": round(o["z"] - g_phong, 6),
            "mesh_min_up": _r(min_up), "mesh_max_up": _r(max_up),
            "mesh_verts": nv,
            "body_min_up": _r(body_up), "deepest_node": dnode,
            "deepest_node_verts": dnv, "animated": int(animated),
            "resnaps": int(resnaps),
            "burial_resnap": _r(burial_resnap),
            "burial_authored_plain": _r(b_plain),
            "burial_authored_phong": _r(b_phong),
            "burial_change_wu": _r(change),
            "parse_note": note,
        })
    return rows


def _r(v, nd=6):
    return "" if (v != v) else round(v, nd)


# =============================================================================
# Self-test / negative controls
# =============================================================================

def _parse_tgl_header_first(blob: bytes) -> tuple[int, str | None]:
    """DELIBERATELY WRONG parser: assumes the TG_TypeNode header precedes the
    geometry for SHAPE_NODE too. This is the exact mistake a prior lane made
    (it recovered 16 of l_turretgenerator's 75 vertices). Kept as a live
    mutation so the byte-exact layout claim is a demonstrated property, not an
    assertion. Returns (vertices_recovered, error_or_None)."""
    try:
        r = _Reader(blob)
        r.p = 4
        ns, nt = r.i(), r.i()
        r.raw(TG_TEXTURE_SIZE * nt)
        total = 0
        for _ in range(ns):
            typ = r.i()
            r.raw(24)                      # nodeCenter + relativeNodeCenter
            r.raw(TG_NODE_ID * 2)          # nodeId + parentId
            if typ == SHAPE_NODE:
                nv = r.i()
                if nv < 0 or nv > 1_000_000:
                    return (total, f"implausible nVerts {nv}")
                r.raw(TG_TYPEVERTEX_SIZE * nv)
                total += nv
                ntri = r.i()
                if ntri < 0 or ntri > 2_000_000:
                    return (total, f"implausible nTris {ntri}")
                r.raw(TG_TYPETRIANGLE_SIZE * ntri)
                ntex = r.i()
                if ntex < 0 or ntex > 4096:
                    return (total, f"implausible nTex {ntex}")
                r.raw(TG_TINYTEXTURE_SIZE * ntex)
            elif typ != TYPE_NODE:
                return (total, f"unknown nodeType {typ}")
        if r.p != len(blob):
            return (total, f"trailing bytes: consumed {r.p} of {len(blob)}")
        return (total, None)
    except Exception as exc:      # noqa: BLE001 -- any failure is the point
        return (-1, f"{type(exc).__name__}: {exc}")


def selftest(gd: GameData, hf: Heightfield | None) -> int:
    """Negative controls. A measurement that has never returned "no difference"
    cannot be trusted when it returns "difference"."""
    fails = 0

    def check(label, ok, detail=""):
        nonlocal fails
        print(f"  [{'PASS' if ok else 'FAIL'}] {label}" + (f"  {detail}" if detail else ""))
        if not ok:
            fails += 1

    print("NEGATIVE CONTROL 1 -- flat terrain: Phong term must be EXACTLY zero.")
    side = 60
    flat = bytearray()
    for _ in range(side * side):
        flat += struct.pack("<3f", 0.0, 0.0, 1.0)   # vertexNormal straight up
        flat += struct.pack("<f", 12.5)             # elevation
        flat += b"\0" * 16
    fhf = Heightfield(side, bytes(flat))
    worst = 0.0
    for xi in range(-1500, 1501, 137):
        for yi in range(-1500, 1501, 149):
            a = fhf.elevation(xi, yi, phong=False)
            b = fhf.elevation(xi, yi, phong=True)
            worst = max(worst, abs(a - b), abs(a - 12.5))
    check("flat plane: |phong-plain| and |plain-12.5| == 0", worst == 0.0,
          f"max |delta| = {worst:g}")

    print("NEGATIVE CONTROL 2 -- uniform slope: still EXACTLY zero (planar).")
    ramp = bytearray()
    nx, nz = 0.2, 1.0
    ln = math.sqrt(nx * nx + nz * nz)
    for r in range(side):
        for c in range(side):
            ramp += struct.pack("<3f", -nx / ln, 0.0, nz / ln)
            ramp += struct.pack("<f", c * WORLD_UNITS_PER_VERTEX * 0.2)
            ramp += b"\0" * 16
    rhf = Heightfield(side, bytes(ramp))
    worst = 0.0
    for xi in range(-1500, 1501, 137):
        for yi in range(-1500, 1501, 149):
            worst = max(worst, abs(rhf.elevation(xi, yi, phong=False)
                                   - rhf.elevation(xi, yi, phong=True)))
    check("uniform slope: |phong-plain| == 0", worst == 0.0,
          f"max |delta| = {worst:g}")

    print("NEGATIVE CONTROL 3 -- curved terrain: the SAME measurement must "
          "report a NONZERO difference (else it can only ever say 'no').")
    # Elevation AND its analytic normal, so the surface is genuinely curved.
    # (A curved heightfield with all-up normals is still locally planar as far
    # as the Phong projection is concerned -- that variant returns 0 and is a
    # useful reminder that the term is driven by NORMALS, not by height.)
    def _h(c, r):
        return 40.0 * math.sin(c * 0.7) * math.cos(r * 0.5)

    bump = bytearray()
    for r in range(side):
        for c in range(side):
            # world x grows with c, world y DECREASES with r (row 0 = +y edge)
            dhdx = 40.0 * 0.7 * math.cos(c * 0.7) * math.cos(r * 0.5) / WORLD_UNITS_PER_VERTEX
            dhdy = 40.0 * 0.5 * math.sin(c * 0.7) * math.sin(r * 0.5) / WORLD_UNITS_PER_VERTEX
            n = _norm((-dhdx, -dhdy, 1.0))
            bump += struct.pack("<3f", n[0], n[1], n[2])
            bump += struct.pack("<f", _h(c, r))
            bump += b"\0" * 16
    bhf = Heightfield(side, bytes(bump))
    worst = 0.0
    for xi in range(-1500, 1501, 137):
        for yi in range(-1500, 1501, 149):
            worst = max(worst, abs(bhf.elevation(xi, yi, phong=False)
                                   - bhf.elevation(xi, yi, phong=True)))
    check("curved terrain: max |phong-plain| > 0.1 wu", worst > 0.1,
          f"max |delta| = {worst:.4f} wu")

    print("NEGATIVE CONTROL 4 -- a mesh KNOWN to sit on its origin must report "
          "mesh_min_up ~ 0, not a spurious burial.")
    # Every .tgl in the corpus, bucketed. If the measurement were broken we
    # would see a degenerate distribution (all-zero or all-negative).
    zeroish, negative, positive, failed, empty = 0, 0, 0, 0, 0
    for nm, blob in gd.tgl_fst.items():
        try:
            mn, _mx, nv, _dn, _dnv, _body = tgl_vertical_extent(parse_tgl(blob))
        except Exception:
            failed += 1
            continue
        if nv == 0 or mn != mn:
            empty += 1
        elif abs(mn) < 0.5:
            zeroish += 1
        elif mn < 0:
            negative += 1
        else:
            positive += 1
    total = zeroish + negative + positive
    check("population is NOT degenerate (all three buckets non-empty)",
          zeroish > 0 and negative > 0 and positive > 0,
          f"n={total} zeroish={zeroish} below-origin={negative} "
          f"above-origin={positive} geometry-free={empty} unparsed={failed}")

    print("NEGATIVE CONTROL 5 -- byte-exactness of the .tgl layout: EVERY "
          "member must parse and consume its whole byte range (parse_tgl "
          "raises on a single trailing or missing byte).")
    check("0 compiled .tgl rejected",
          failed == 0,
          f"{failed} of {total + empty + failed} rejected")

    print("NEGATIVE CONTROL 6 -- MUTATION: re-parse with the header-FIRST "
          "layout (the real prior-lane bug) and require it to be caught.")
    probe = "l_turretgenerator.tgl"
    blob = gd.tgl_fst.get(probe)
    if blob is None:
        check(f"{probe} present", False)
    else:
        good = parse_tgl(blob)
        gv = sum(len(n["verts"]) for n in good["nodes"])
        wrong_v, wrong_err = _parse_tgl_header_first(blob)
        check(f"correct layout recovers all vertices of {probe}", gv == 75,
              f"{gv} vertices (expected 75)")
        check("header-FIRST layout is REJECTED or under-recovers",
              wrong_err is not None or wrong_v < gv,
              f"header-first -> {'error: ' + wrong_err if wrong_err else str(wrong_v) + ' vertices'}")

    print("NEGATIVE CONTROL 7 -- AXIS: using `.z` instead of `.y` as 'up' must "
          "give a DIFFERENT, wrong answer on a turret known to sit on grade.")
    ref = gd.tgl_fst.get("mediumautocannonturret.tgl")
    if ref is None:
        check("mediumautocannonturret.tgl present", False)
    else:
        p = parse_tgl(ref)
        up_y = min(n["nodeCenter"][1] + v[1]
                   for n in p["nodes"] if n["type"] == SHAPE_NODE and n["verts"]
                   for v in n["verts"])
        up_z = min(n["nodeCenter"][2] + v[2]
                   for n in p["nodes"] if n["type"] == SHAPE_NODE and n["verts"]
                   for v in n["verts"])
        check("UP=.y gives min_up == 0 (turret base ON grade)", abs(up_y) < 1e-4,
              f"min over .y = {up_y:.6f}")
        check("UP=.z (wrong axis) gives a clearly different number",
              abs(up_z - up_y) > 1.0,
              f"min over .z = {up_z:.6f}  (delta {abs(up_z - up_y):.3f} wu)")

    if hf is not None:
        print("NEGATIVE CONTROL 8 -- real mission heightfield: elevation() must "
              "reproduce grid vertices exactly at their own XY.")
        worst, n = 0.0, 0
        s = hf.side
        for r in range(4, s - 4, 7):
            for c in range(4, s - 4, 7):
                wx = (c - (s >> 1)) * WORLD_UNITS_PER_VERTEX
                wy = ((s >> 1) - r) * WORLD_UNITS_PER_VERTEX
                got = hf.elevation(wx + 1e-3, wy - 1e-3, phong=False)
                want = hf.elev[c + r * s]
                worst = max(worst, abs(got - want))
                n += 1
        check("vertex round-trip |delta| < 0.02 wu", worst < 0.02,
              f"max |delta| = {worst:.6f} wu over {n} vertices")

    print("\nNEGATIVE CONTROL 9 -- RESNAP-INVERSION-1: burial_change_wu must "
          "attribute the a75339bb0 move to the RE-SNAPPING class and to no "
          "other, and must report zero when there was nothing to move.")
    # Curvature present (g_phong != g_plain): only the re-snapping class moves.
    gp, gf, mu = 100.0, 100.75, -0.5
    check("re-snapping class moves by exactly +ground_delta",
          burial_change(True, gp, gf, mu) == (gf - gp),
          f"got {burial_change(True, gp, gf, mu):+.6f}, want {gf - gp:+.6f}")
    check("non-re-snapping class (TURRET) moves by EXACTLY 0",
          burial_change(False, gp, gf, mu) == 0.0,
          f"got {burial_change(False, gp, gf, mu):+.6f}")
    # The mutation this control exists to catch: the pre-fix model, which
    # returned -ground_delta for a turret and 0 for a building -- both classes
    # wrong, and inverted with respect to each other.
    check("the INVERTED model is rejected (turret != -ground_delta)",
          burial_change(False, gp, gf, mu) != -(gf - gp),
          f"inverted model would have said {-(gf - gp):+.6f}")
    check("the INVERTED model is rejected (building != 0 under curvature)",
          burial_change(True, gp, gf, mu) != 0.0)
    # Planar terrain: the Phong term is identically zero (controls 1-2), so
    # NOTHING moved, for either class. A measurement that always reports a
    # change is as useless as one that never does.
    check("flat/planar terrain: re-snapping class reports EXACTLY 0",
          burial_change(True, 100.0, 100.0, mu) == 0.0)
    check("unparsed mesh propagates NaN rather than a fake 0",
          burial_change(True, gp, gf, float("nan")) != burial_change(True, gp, gf, mu))

    print(f"\nselftest: {'ALL PASS' if fails == 0 else str(fails) + ' FAILURE(S)'}")
    return 0 if fails == 0 else 1


# =============================================================================

def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description="Per-object authored-Z / terrain / mesh-bottom burial table.")
    add_missions_dir_arg(ap)
    ap.add_argument("--missions", nargs="*", default=None,
                    help="mission stems (default: every mc2_* .pak found)")
    ap.add_argument("--deploy-lane", default=None,
                    help="deploy dir holding data/objects/object2.pak + tgl.fst")
    ap.add_argument("--class-filter", default=None,
                    help="only rows whose class matches (substring, case-insens)")
    ap.add_argument("--name-filter", default=None,
                    help="only rows whose class/type/appearance/tgl matches "
                         "(substring, case-insensitive)")
    ap.add_argument("--buried-only", action="store_true",
                    help="only rows with a nonzero burial under the CURRENT default")
    ap.add_argument("--csv", type=Path, default=None,
                    help="full per-object table (~15 MB for all 37 maps)")
    ap.add_argument("--json", type=Path, default=None)
    ap.add_argument("--kinds-csv", type=Path, default=None,
                    help="compact one-row-per-(class,appearance,tgl) rollup -- "
                         "small enough to commit; the full table stays "
                         "regenerable from this script")
    ap.add_argument("--top", type=int, default=40, help="rows to print (0 = all)")
    ap.add_argument("--selftest", action="store_true",
                    help="run negative controls and exit")
    ap.add_argument("--nodes", nargs="*", default=None, metavar="APPEARANCE",
                    help="dump per-node vertical extents for these appearance "
                         "names (or bare .tgl names) and exit -- this is how a "
                         "whole-model min_up gets attributed to a real body or "
                         "to one decorative sub-node")
    args = ap.parse_args(argv)

    gd = load_game_data(args.deploy_lane)
    if args.nodes is not None:
        return _dump_nodes(gd, args.nodes)
    print(f"[deploy-lane] {gd.lane}")
    print(f"[object2.pak] {len(gd.obj_types)} object types")
    print(f"[tgl] {len(gd.tgl_fst)} compiled shapes, {len(gd.ini_text)} appearance .ini")
    for n in gd.notes:
        print(f"[note] {n}")

    res = resolve_from_args(args, missions=args.missions)
    print(res.banner())
    missions_dir = Path(res.dir)

    if args.missions:
        stems = list(args.missions)
    else:
        stems = sorted(p.stem for p in missions_dir.glob("mc2_*.pak"))
    if not stems:
        print("no missions found", file=sys.stderr)
        return 2

    if args.selftest:
        hf0, _ = load_mission(missions_dir / f"{stems[0]}.pak")
        return selftest(gd, hf0)

    tgl_cache: dict = {}
    rows: list[dict] = []
    for stem in stems:
        pak = missions_dir / f"{stem}.pak"
        if not pak.is_file():
            print(f"  !! {stem}: no .pak in {missions_dir}", file=sys.stderr)
            continue
        hf, objs = load_mission(pak)
        rows += build_rows(stem, hf, objs, gd, tgl_cache)
        print(f"  {stem}: side={hf.side} objects={len(objs)}")

    def keep(r):
        if args.class_filter and args.class_filter.lower() not in r["class"].lower():
            return False
        if args.name_filter:
            f = args.name_filter.lower()
            # class is part of the haystack on purpose: `--name-filter turret`
            # must catch BOTH the TURRET-class objects (some of which are named
            # `guntower` / `Spotlight`) and the turret-NAMED BUILDING-class
            # objects (`L_TurretGenerator`, `BTurretControl`). Splitting those
            # into two runs is how the two complaints got conflated.
            blob = f"{r['class']} {r['type_name']} {r['appearance']} {r['tgl']}".lower()
            if f not in blob:
                return False
        if args.buried_only:
            b = r["burial_resnap"] if r["resnaps"] else r["burial_authored_plain"]
            if b == "" or abs(b) < 1e-6:
                return False
        return True

    sel = [r for r in rows if keep(r)]

    if args.csv:
        args.csv.parent.mkdir(parents=True, exist_ok=True)
        with args.csv.open("w", newline="", encoding="utf-8") as fh:
            w = csv.DictWriter(fh, fieldnames=COLUMNS)
            w.writeheader()
            w.writerows(sel)
        print(f"\n[csv] {len(sel)} row(s) -> {args.csv}")
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(sel, indent=1), encoding="utf-8")
        print(f"[json] {len(sel)} row(s) -> {args.json}")
    if args.kinds_csv:
        n = _write_kinds(sel, args.kinds_csv)
        print(f"[kinds-csv] {n} kind(s) -> {args.kinds_csv}")

    _summarise(rows, sel, args.top)
    return 0


KIND_COLUMNS = [
    "class", "appearance", "tgl", "instances", "missions",
    "mesh_min_up", "mesh_max_up", "body_min_up",
    "deepest_node", "deepest_node_verts", "mesh_verts", "animated", "resnaps",
    "burial_current_default",
    "max_abs_authored_minus_plain", "max_abs_authored_minus_phong",
    "max_abs_burial_change_wu",
]


def _write_kinds(rows: list[dict], path: Path) -> int:
    """One row per (class, appearance, tgl). The full per-object table is ~15 MB;
    this rollup keeps every fact that does not vary within a kind, plus the
    per-kind extremes of the facts that do."""
    agg: dict = {}
    for r in rows:
        k = (r["class"], r["appearance"], r["tgl"])
        e = agg.get(k)
        if e is None:
            e = agg[k] = {
                "class": r["class"], "appearance": r["appearance"],
                "tgl": r["tgl"], "instances": 0, "_missions": set(),
                "mesh_min_up": r["mesh_min_up"], "mesh_max_up": r["mesh_max_up"],
                "body_min_up": r["body_min_up"],
                "deepest_node": r["deepest_node"],
                "deepest_node_verts": r["deepest_node_verts"],
                "mesh_verts": r["mesh_verts"], "animated": r["animated"],
                "resnaps": r["resnaps"],
                "burial_current_default":
                    r["burial_resnap"] if r["resnaps"]
                    else r["burial_authored_plain"],
                "max_abs_authored_minus_plain": 0.0,
                "max_abs_authored_minus_phong": 0.0,
                "max_abs_burial_change_wu": 0.0,
            }
        e["instances"] += 1
        e["_missions"].add(r["mission"])
        for src, dst in (("authored_minus_plain", "max_abs_authored_minus_plain"),
                         ("authored_minus_phong", "max_abs_authored_minus_phong"),
                         ("burial_change_wu", "max_abs_burial_change_wu")):
            v = r[src]
            if v != "":
                e[dst] = max(e[dst], abs(v))
    out = []
    for e in agg.values():
        e["missions"] = len(e.pop("_missions"))
        for k2 in ("max_abs_authored_minus_plain", "max_abs_authored_minus_phong",
                   "max_abs_burial_change_wu"):
            e[k2] = round(e[k2], 6)
        out.append({c: e[c] for c in KIND_COLUMNS})
    out.sort(key=lambda e: (e["mesh_min_up"] if e["mesh_min_up"] != "" else 0.0,
                            e["class"], e["appearance"]))
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as fh:
        w = csv.DictWriter(fh, fieldnames=KIND_COLUMNS)
        w.writeheader()
        w.writerows(out)
    return len(out)


def _dump_nodes(gd: GameData, names: list[str]) -> int:
    rc = 0
    for raw in names:
        tgl, why = resolve_lod0_shape(gd, raw)
        if not tgl:
            tgl = raw.lower()
            if not tgl.endswith(".tgl"):
                tgl += ".tgl"
            why = "treated as a bare .tgl name"
        blob = gd.tgl_fst.get(tgl)
        print(f"\n== {raw}  ->  {tgl}   ({why})")
        if blob is None:
            print("   ABSENT from tgl.fst and the loose data/tgl dir")
            rc = 1
            continue
        try:
            parsed = parse_tgl(blob)
        except (TglParseError, ValueError) as exc:
            print(f"   PARSE FAILED: {exc}")
            rc = 1
            continue
        lo, hi, tot, dn, dnv, body = tgl_vertical_extent(parsed)
        print(f"   {parsed['bytes']} bytes, {len(parsed['nodes'])} node(s), "
              f"{tot} vertices, animated={appearance_is_animated(gd, raw)}")
        print("   %-22s %-22s %2s %6s %10s %10s"
              % ("node", "parent", "d", "nverts", "min_up", "max_up"))
        for n in tgl_node_extents(parsed):
            print("   %-22s %-22s %2d %6d %10.3f %10.3f"
                  % (n["id"], n["parent"], n["depth"], n["nverts"],
                     n["min_up"], n["max_up"]))
        print(f"   MODEL min_up={lo:.3f} max_up={hi:.3f}   "
              f"BODY(shallowest drawn node) min_up={body:.3f}   "
              f"deepest node={dn!r} ({dnv} verts)")
    return rc


def _summarise(all_rows, sel, top):
    print(f"\n================ SUMMARY ({len(all_rows)} objects) ================")
    by_class: dict[str, int] = {}
    for r in all_rows:
        by_class[r["class"]] = by_class.get(r["class"], 0) + 1
    print("class histogram: " + ", ".join(
        f"{k}={v}" for k, v in sorted(by_class.items(), key=lambda kv: -kv[1])))

    unresolved = [r for r in all_rows if r["parse_note"]]
    print(f"rows with a parse/resolve note: {len(unresolved)}")
    notes: dict[str, int] = {}
    for r in unresolved:
        notes[r["parse_note"]] = notes.get(r["parse_note"], 0) + 1
    for k, v in sorted(notes.items(), key=lambda kv: -kv[1])[:8]:
        print(f"   {v:5d}  {k}")

    # ---- THE decisive statistic: which elevation were the objects AUTHORED on?
    print("\nWHICH ELEVATION WAS THE SHIPPED DATA AUTHORED ON?")
    print(f"{'class':<15s} {'n':>7s} | {'vs PLAIN (post-a75339bb0)':^34s} | "
          f"{'vs PHONG (pre-a75339bb0)':^34s}")
    print(f"{'':<15s} {'':>7s} | {'max|d|':>10s} {'mean|d|':>10s} {'frac>1e-3':>11s} | "
          f"{'max|d|':>10s} {'mean|d|':>10s} {'frac>1e-3':>11s}")
    order = sorted({r["class"] for r in all_rows})
    for cls in order:
        rs = [r for r in all_rows if r["class"] == cls]
        a = [abs(r["authored_minus_plain"]) for r in rs]
        b = [abs(r["authored_minus_phong"]) for r in rs]
        if not a:
            continue
        print(f"{cls:<15s} {len(rs):>7d} | {max(a):>10.4f} {sum(a)/len(a):>10.4f} "
              f"{sum(1 for v in a if v > 1e-3)/len(a):>11.4f} | "
              f"{max(b):>10.4f} {sum(b)/len(b):>10.4f} "
              f"{sum(1 for v in b if v > 1e-3)/len(b):>11.4f}")

    moved = [r for r in all_rows
             if r["burial_change_wu"] != "" and abs(r["burial_change_wu"]) > 1e-6]
    # RESNAP-INVERSION-1 label fix (2026-07-31).  632122f7d corrected
    # burial_change() but left THIS label reading "non-re-snapping classes
    # only" -- the old, inverted model.  Anyone pasting this output into a
    # status doc would have re-seeded the exact error the commit fixed, and
    # that is precisely how the wrong figures reached the review pack the
    # first time.  The movers are the RE-SNAPPING classes; everything else
    # changes by exactly zero (see burial_change's docstring).
    print(f"\nobjects whose APPARENT burial moves when a75339bb0 lands "
          f"(re-snapping classes only -- {', '.join(sorted(RESNAP_CLASSES))}; "
          f"every other class changes by exactly 0): {len(moved)}")
    if moved:
        deeper = [r for r in moved if r["burial_change_wu"] > 0]
        shallow = [r for r in moved if r["burial_change_wu"] < 0]
        print(f"   settles DOWN (was floating): {len(deeper)}   "
              f"rises UP (was sunk): {len(shallow)}")
        print("   In BOTH directions the post-retire value is exactly the "
              "geometric burial -mesh_min_up,\n"
              "   because the re-snap assigns base_z = ground_plain after the "
              "retire (bldng.cpp:829,\n"
              "   gate.cpp:361) -- NOT because authored_z equals anything. "
              "The move is a correction,\n"
              "   not a new error -- but it IS user-visible relative to the "
              "immediately preceding build.")
        worst = sorted(moved, key=lambda r: -abs(r["burial_change_wu"]))[:10]
        print("   worst |move| (wu):")
        for r in worst:
            print(f"     {r['mission']:8s} #{r['obj_index']:<4d} {r['class']:<12s} "
                  f"{(r['appearance'] or r['type_name'])[:26]:<26s} "
                  f"{r['burial_change_wu']:+.4f}")

    # ---- genuinely buried geometry, under the CURRENT default
    buried: dict = {}
    for r in all_rows:
        mu = r["mesh_min_up"]
        if mu == "" or mu >= -0.5:
            continue
        k = (r["class"], r["appearance"], bool(r["animated"]))
        e = buried.setdefault(k, {"n": 0, "min_up": mu, "body": r["body_min_up"],
                                  "node": r["deepest_node"],
                                  "nv": r["deepest_node_verts"]})
        e["n"] += 1
    print(f"\nPLACED object kinds whose LOD0 bind-pose mesh reaches >0.5 wu "
          f"below its own origin: {len(buried)} kind(s), "
          f"{sum(e['n'] for e in buried.values())} instance(s)")
    print(f"{'n':>6s}  {'class':<14s} {'appearance':<24s} {'model':>9s} "
          f"{'body':>9s} {'anim':>5s}  deepest node")
    for k, e in sorted(buried.items(), key=lambda kv: kv[1]["min_up"])[:25]:
        body = e["body"] if e["body"] != "" else float("nan")
        print(f"{e['n']:6d}  {k[0]:<14s} {k[1][:24]:<24s} {e['min_up']:>9.3f} "
              f"{body:>9.3f} {('Y' if k[2] else 'n'):>5s}  "
              f"{e['node']} ({e['nv']} verts)")

    if not sel:
        return
    print(f"\n---- {min(len(sel), top) if top else len(sel)} of {len(sel)} "
          f"selected rows ----")
    hdr = (f"{'mission':8s} {'#':>4s} {'class':<12s} {'appearance':<26s} "
           f"{'authZ':>9s} {'gnd_pl':>9s} {'gnd_ph':>9s} {'minUp':>9s} "
           f"{'burial':>9s} {'chg':>8s} rs")
    print(hdr)
    print("-" * len(hdr))
    ordered = sorted(sel, key=lambda r: (r["mission"], r["obj_index"]))
    for r in (ordered[:top] if top else ordered):
        b = r["burial_resnap"] if r["resnaps"] else r["burial_authored_plain"]
        print(f"{r['mission']:8s} {r['obj_index']:>4d} {r['class']:<12s} "
              f"{(r['appearance'] or r['type_name'])[:26]:<26s} "
              f"{r['authored_z']:>9.3f} {r['ground_plain']:>9.3f} "
              f"{r['ground_phong']:>9.3f} "
              f"{(r['mesh_min_up'] if r['mesh_min_up'] != '' else float('nan')):>9.3f} "
              f"{(b if b != '' else float('nan')):>9.3f} "
              f"{(r['burial_change_wu'] if r['burial_change_wu'] != '' else float('nan')):>+8.3f} "
              f"{'Y' if r['resnaps'] else 'n'}")


if __name__ == "__main__":
    try:
        sys.exit(main())
    except StockDataError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(2)
