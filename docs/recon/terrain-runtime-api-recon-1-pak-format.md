<!-- doc-status: stale | reviewed:2026-10-11 -->
# TERRAIN-RUNTIME-API-RECON-1 - On-disk .pak terrain format and load path

Read-only recon (2026-06-25, worktree nifty-mendeleev). All file:line verified this pass.

## 1. .pak packet layout (PacketFile container)

Container = PacketFile, magic 0xFEEDFACE (tools/terrain_gen/pak_exporter.py:14;
spec docs/terrain-height-audit.md:76-81; reader mclib/packet.h / mclib/packet.cpp):
- Bytes 0-3: magic/checksum 0xFEEDFACE.
- Bytes 4-7: firstPacketOffset; numPackets = firstPacketOffset/4 - 2.
- Bytes 8..firstPacketOffset: numPackets x uint32 seek-table entries; each entry =
  storageType<<29 | offset (RAW=0, FWF=1, LZD=2, HF=3, ZLIB=4, NUL=7).
- Packet body size = delta between consecutive entry offsets; last runs to EOF.
- LZD/ZLIB bodies: first 4 bytes = uint32 unpacked size, payload at +4.

Per-mission <mission>.pak packet indices (editor save, editor/EditorData_save.cpp:441-478):
- Packet 0 - terrain land grid. land->save(file,0,...) (editor/EditorData_save.cpp:444). Raw
  PostcompVertex[side*side] row-major (y outer). Python PakExporter.build_packet0 /
  patch_pak writes this (pak_exporter.py:64-91,114-144).
- Packet 1 - objects/buildings. EditorObjectMgr::save(file,1) (editor/EditorData_save.cpp:446);
  load EditorObjectMgr::load(pFile,1) (editor/EditorData_terrain.cpp:183, def editor/EditorObjectMgr_io.cpp:77).
- Packet 3 - TacMap (saveTacMap(file,3), editor/EditorData_save.cpp:451).
- Packet 4 - MOVE / pathfinding data (MOVE_saveData(file,4), editor/EditorData_save.cpp:456).
- Last packet - GUID multiplayer version stamp (editor/EditorData_save.cpp:474-478).
- numPackets = 6 (or 5+movePacketCount), editor/EditorData_save.cpp:447.

NOTE: object TYPES (chassis/building defs) live in a SEPARATE global object2.pak keyed by
objTypeNum (code/objtype.cpp:355 seekPacket), NOT the per-mission .pak. Packet 1 holds
object instances/placements.

## 2. Load sequence

Game (code/mission.cpp:2759-2797):
1. terrainFileName.init(missionPath, missionName, .pak) - .pak named off mission stem,
   sibling to the mission .fit (missionFile).
2. PacketFile pakFile; pakFile.open(terrainFileName) (code/mission_movers.cpp:548-550).
3. land->getColorMapName(missionFile) - colormap name from .fit ColorMap block (terrain.cpp:529).
4. land->init(pakFile, 0, ...) (code/mission_movers.cpp:566) -> Terrain::init (terrain.cpp:495):
   seekPacket(0), getPacketSize(), realVerticesMapSide = sqrt(size/sizeof(PostcompVertex))
   (terrain.cpp:505-506), range-check [60,2048] (terrain.cpp:510), then init(...) (terrain.cpp:523,575).
5. land->load(missionFile) (code/mission_movers.cpp:581, def mclib/terrain_geometry.cpp:133) - reads mission .fit, NOT .pak.
- Save-game variant: identical land->init(pakFile,0,...) at code/saveload.cpp:1135.

In-memory: Terrain (mclib/terrain.cpp/.h) owns mapData (MapData, mclib/mapdata.*) holding the
PostcompVertex block array (getBlocks(), terrain.cpp:375).

### Dimensions / world-size / elevation / water
- Grid side: derived from packet-0 byte size (terrain.cpp:505-506) - NOT stored explicitly.
- Spacing: worldUnitsPerVertex = 128 wu; world coord tables terrain.cpp:480-491.
- Per-vertex elevation: float in each PostcompVertex; python elevation = h*max_elev+min_elev
  (pak_exporter.py:82-85).
- Elevation slider range: [Terrain] UserMin/UserMax in mission .fit (mclib/terrain_geometry.cpp:159-160;
  python terrain_gen.py:100-101).
- Water: mission .fit [Water] block - Elevation (authoritative water test), Frequency,
  Ampliture, AlphaShallow/Middle/Deep, AlphaDepth, ShallowDepth (mclib/terrain_geometry.cpp:136-156). NOT .pak.

## 3. Per-vertex on-disk attributes (PostcompVertex, packet 0)

Struct mclib/vertex.h:33-64, 32 bytes (python _VERTEX 3f f I I I 4B, pak_exporter.py:12):
- off 0  Vector3D vertexNormal (3f) - lighting (python finite-diff pak_exporter.py:32-62)
- off 12 float elevation - height (vertex.h:44,56)
- off 16 DWORD textureData - hi16 Overlay TXM handle, lo16 Base TXM (vertex.h:46); overlay
         codes e.g. DAMAGED_BRIDGE per tools/terrain_beautify/README.md:34,47
- off 20 DWORD localRGBLight - pre-baked aRGB lighting (vertex.h:48); python 0x00CCCCCC
- off 24 DWORD terrainType - terrainType number / material class (vertex.h:50); python from
         masks.terrain_type (pak_exporter.py:89)
- off 28 BYTE selected (vertex.h:51)
- off 29 BYTE water flag (vertex.h:52)
- off 30 BYTE shadow (vertex.h:53)
- off 31 BYTE highlighted whole-face (vertex.h:54)

- Passability / area id are NOT vertex fields. Passability is derived (terrainType +
  elevation/slope); authoritative movement graph = separate MOVE packet 4. Per-vertex
  terrainType dominated by one code in practice - weak material signal (README.md:40-41).
- textureData written 0 by python generator (pak_exporter.py:87); TXM ids authored in-editor.

## 4. Existing sidecar precedent (the model for terrain2)

B2/B7 mission beauty sidecar - package <mission>.beauty/:
- sidecar.json - metadata + baseHash (sha256 of source .pak), grid_side,
  world_units_per_vertex, delta_file, delta_dtype (mission_sidecar.py:91-104).
- height_delta.r32 - float32 [side*side] row-major world-unit ELEVATION deltas (mission_sidecar.py:89).
- (B7c) optional protected.r8 - per-cell level 2=structural/1=water/0=editable (BeautySidecarPreview.h:51-55).

Attach to a mission TWO ways:
- Offline (python) apply_sidecar (mission_sidecar.py:116): reads packet-0 RAW range
  (_locate_packet0_range), reshapes to PostcompVertex grid, adds delta to elevation@12,
  recomputes Z-up normal@0 for changed cells only, writes a NEW patched .pak. Reversible:
  only touches elevation+normal of nonzero-delta cells.
- Live editor preview BeautySidecarPreview::Apply() (BeautySidecarPreview.cpp; header
  BeautySidecarPreview.h:23): loads height_delta.r32, snapshots original elevations
  (s_origElev), applies via HeightBrush edit path (setVertexHeight+calcLight+refreshTerrainAfterEdit);
  Restore() swaps snapshot back. Dir named by Terrain::terrainName (mission stem), NOT the
  .fit display title (BeautySidecarPreview.cpp:37-46). Never writes a file; never checks
  baseHash (grep clean) - the hash gate is python-only.

## 5. Reversibility / baseHash validation

- baseHash = sha256_file(pak_path) captured at sidecar write (mission_sidecar.py:42-46,95).
- apply_sidecar recomputes cur_hash and REFUSES if cur_hash != meta[baseHash] unless
  allow_hash_mismatch (mission_sidecar.py:119-124) - blocks applying to wrong/already-modified terrain.
- Reversibility proven by _cmd_verify_roundtrip (mission_sidecar.py:165-183): apply a
  zero/identity sidecar -> output sha256 == source. Patch byte-localized (changed cells only,
  elevation+normal only), RAW-packet-0 only (mission_sidecar.py:128-130).

## How a terrain2 sidecar could attach (reuse this precedent)

- Package dir <mission>.terrain2/ (sibling to .fit/.pak, mirroring .beauty/).
- terrain2.json with format, grid_side, and baseHash = sha256 of stock .pak - same gate to
  refuse mismatched/modified base terrain.
- Loose binary planes (.r32/.r8/.u32) row-major side*side, one per modern attribute (material
  splat ids, normal/PBR aux, overlay codes), NEVER mutating the stock .pak.
- Engine load: after land->init(pakFile,0,...) (code/mission_movers.cpp:566) and land->load
  (code/mission_movers.cpp:581), add an opt-in (MC2_* gate) sidecar pass that recomputes baseHash of the
  open pakFile and applies only on match - degrade to stock generation on miss/absent
  (stock-must-be-playable). Mirror BeautySidecarPreview snapshot/restore for live use; mirror
  python apply for offline bake.
- Stock coexistence: .pak stays authoritative; sidecar additive, hash-gated, reversible, not
  required for gameplay correctness, never referenced by save games.
