<!-- doc-status: current | reviewed:2026-10-11 -->
# Terrain Height Audit (TERRAIN-HEIGHT-AUDIT-0)

Recon variant per slice spec. The `.pak` mission format is parseable
but not trivial, and this session is read-only-except-artifacts (another
session owns engine debug edits). So this document describes WHERE the
data lives, WHAT an audit needs to extract, and HOW to extract it,
without shipping a script that would race the parallel session.

Pairs with [docs/terrain-rv-arc-recon.md](terrain-rv-arc-recon.md)
(Slice 1) and [docs/terrain-resample-plan.md](terrain-resample-plan.md)
(Slice 5).

## 1. What the audit must report

For each tier1 mission (mc2_01, mc2_03, mc2_10, mc2_17, mc2_24):

| Metric | Why it matters |
|---|---|
| `realVerticesMapSide` (60 / 80 / 100 / 120) | Direct visual ceiling |
| World units / vertex (always 128.0 by code; assert) | Cross-check vs binary |
| Map extent in world units | Sanity for camera distance / LOD |
| `min/max/mean/stdev` elevation | Calibrates fog & shadow tunables |
| Adjacent-delta histogram (Δh per 128 wu step, N, S, E, W) | Detects whether interesting slopes ever exist |
| Slope distribution (atan(Δh / 128)) in degrees | Drives reasonable normal generation |
| Second-difference variance ("blockiness") | Confirms low-frequency content of source |
| Per-mission storage type (RAW / LZD / ZLIB) | Tells script which decompressor it needs |
| Per-mission raw bytes of MapData packet | `bytes / 28 == realVerticesMapSide²` invariant |

Optional extension (post-Slice-5 plan):
- Distribution of `PostcompVertex.vertexNormal` deviation from
  flat-up (`(0,0,1)`) — measures how much of the lit surface comes
  from baked normals vs from the geometry.

## 2. Data locations

### Mission files

Path convention: `data/missions/<mission>.pak` plus `<mission>.fit`.

Live source: **resolved and byte-verified** by
[`tools/mc2data/stock_missions.py`](../tools/mc2data/stock_missions.py) — do
not hard-code one. This doc previously named
`/a/Games/Carver5-feasibility/data/missions/` "per recon"; that tree differs
from the shipped `mission.fst` on **all 24 campaign maps**, so audits run
against it graded terrain the engine never loads. See
[data-provenance.md](data-provenance.md) § "Which copy of the mission data is
authoritative". `scripts/terrain_height_audit.py` now has no `--missions-dir`
default and refuses unverifiable data.

Deploy: `A:/Games/mc2-opengl/mc2-win64-v0.4/data/missions/` holds only loose
ABL/brain fixtures and custom maps; the `mc2_*` maps live in that lane's
`mission.fst`, which is the authority.

### Terrain elevation packet

`MapData::newInit()` reads ONE packet — the "current packet" — into
a flat buffer of `PostcompVertex` records.

- [mclib/mapdata.cpp:205-211](../mclib/mapdata.cpp) — `MapData::newInit(PacketFile* newFile, long numVertices)` → `newFile->readPacket(newFile->getCurrentPacket(), (MemoryPtr)blocks)`.
- [mclib/mapdata.cpp:215-218](../mclib/mapdata.cpp) — save side: `realVerticesMapSide² * sizeof(PostcompVertex)`.
- [mclib/terrain.cpp:322](../mclib/terrain.cpp) — at load: `realVerticesMapSide = sqrt(packetSize / sizeof(PostcompVertex))`, validated against `{60, 80, 100, 120}` (lines 325-328).

### PostcompVertex record (28 bytes)

[mclib/vertex.h:32-47](../mclib/vertex.h):

```
offset  size  field
   0    12    Stuff::Vector3D vertexNormal   (3× float32, normal)
  12     4    float           elevation      (world height, single precision)
  16     4    DWORD           textureData    (overlay + base texture layers, packed)
  20     4    DWORD           localRGBLight  (pre-baked RGB)
  24     4    (padding / per-build extras — verify via static_assert(sizeof(PostcompVertex)==28))
```

If `sizeof(PostcompVertex) != 28` on a future build, the audit MUST
abort and re-emit. The script's first job is to read the source header
and assert.

### PacketFile binary format

[mclib/packet.h:29-48](../mclib/packet.h), [mclib/packet.cpp:106-345](../mclib/packet.cpp).
Summary (full spec in recon traces; reproducing only what the audit
needs):

- Bytes 0–3: checksum OR magic `0xFEEDFACE` (uint32 LE).
- Bytes 4–7: `firstPacketOffset` (uint32 LE) → `numPackets = (firstPacketOffset / 4) - 2`.
- Bytes 8 .. firstPacketOffset: `numPackets` × uint32 entries.
- Each entry: top 3 bits = storage type (`RAW=0`, `FWF=1`, `LZD=2`, `HF=3`, `ZLIB=4`, `NUL=7`), low 29 bits = file offset of packet body.
- Packet size: delta between consecutive entry offsets; last packet runs to EOF.
- For `LZD` and `ZLIB`: first 4 bytes at packet body = uint32 LE unpacked size; payload starts at offset+4.

The MapData packet's index in the table is mission-specific
(`currentPacket` in `MapData::newInit`). The audit cannot know that
index without running the engine, so it must **scan all packets** and
pick the one whose unpacked size matches `S² × 28` for some
`S ∈ {60,80,100,120}`. If multiple match, the heuristic is to pick
the largest, since other packets are smaller game-data structures.

## 3. Existing parser to leverage

`dev/squelch/fix_squelch.py` (lines 38–201, per recon) is a production
PacketFile reader for `.pak` files with:

- `read_pak(path)` — header parse + entry extraction
- `lzw_decompress()` — 9–12-bit LZD decoder
- LZW table matches the mclib LZ implementation (validated)

The Slice 3 audit script can import from / vendor that reader rather
than reimplementing. Two pre-conditions:

1. Confirm `fix_squelch.py` understands `STORAGE_TYPE_ZLIB` (if any
   tier1 mission uses zlib instead of LZD, you'll need to add a
   `zlib.decompress` branch — trivial).
2. Confirm `STORAGE_TYPE_RAW` is the common case for MapData packets —
   if yes, the LZ path is optional for tier1 coverage.

## 4. Recommended script shape (not authored this slice)

```
scripts/terrain_height_audit.py [mission1 mission2 ...]
  for each mission:
    open .pak
    walk all packets, locate the one with unpacked_size matching S²·28
    decode into PostcompVertex array (struct: <fff f I I 4x>)
    compute metrics in §1
    emit tests/terrain/terrain_height_audit_<timestamp>.md
```

Output: a single Markdown report per run, committed (or written into
`tests/terrain/` and reviewed before commit). No engine binary
required.

## 5. Constraints honored

- No gameplay data mutation — the script is read-only against `.pak`.
- No render behavior changes — script lives in `tools/`, not in the
  engine.
- No displacement implementation — that is Slice 5 + a future
  TERRAIN-RESAMPLE-1.
- No terrain object identity — audit aggregates samples, never
  identifies per-tile objects.

## 6. Why this slice ships as a recon doc, not the script

1. **Read-only-except-artifacts mode** for this session.
2. **Parallel session active in same worktree** (debug substrate edits
   to `gos_static_prop_batcher.{cpp,h}`, `gameosmain.cpp`,
   `RendererFeatureRegistry.h`, `scripts/run_smoke.py`,
   `docs/tier1_env_vars.md`, plus new `debug_state_dump.{cpp,h}`).
   Adding `scripts/terrain_height_audit.py` + `tests/terrain/`
   output dir does not collide with their files, BUT executing the
   script would write a timestamped artifact while their session may
   be commit-staging tools/ or tests/ — clean-isolation risk.
3. **Authored script needs cross-validation against ground truth.**
   First validation should be: pick one mission, run script, compare
   reported `(min, max, mean, S)` against a live game-side dump
   (`MC2_TERRAIN_HEIGHT_DUMP=1` style). That live dump is an engine
   edit — out of scope for read-only mode and racing the other
   session's edits.

## 7. Acceptance criteria for the script when authored

When TERRAIN-HEIGHT-AUDIT-0 is upgraded from doc to script:

1. Script reads `.pak`, asserts `sizeof PostcompVertex == 28`.
2. Locates MapData packet via size heuristic.
3. Produces a tier1 report with all metrics in §1.
4. Cross-validated on ONE mission against an engine-side
   `getTerrainElevation()` dump — `min/max/mean` match within FP
   rounding.
5. Committed under `scripts/terrain_height_audit.py` (script)
   and `tests/terrain/terrain_height_audit_<timestamp>.md` (sample
   output).
6. No engine changes required to RUN the script after authorship
   (engine-side dump only needed for the one-shot validation).

## 8. Expected findings (predicted, to be confirmed)

Based on the recon (128 wu/vertex, 60–120 sample side, classic RTS
campaign maps):

- Tier1 mode: `realVerticesMapSide` = 120 for most missions.
- Elevation range: typically a few hundred world units; mc2_17 / mc2_24
  may have wider range (combined-arms / final).
- Slope distribution: heavily peaked < 10°; tail to ~30°. Almost no
  cliff geometry — confirming that the visual flatness is data, not
  shader, in origin.
- Blockiness: high — second-difference variance dominated by axis-
  aligned per-vertex steps; expected, since the gameplay grid is what
  the level designer worked with.

If these predictions hold, the case for visual-only resample
(Slice 5) is empirically grounded; if they fail (e.g., one mission
has a 60-side grid with very steep terrain), Slice 5 must address
per-mission scaling.

## 9. Open questions for the user

- Do you want the audit script authored in a follow-up session (with
  the engine-side validation dump) or do you want me to author the
  script-only side now in a separate worktree to avoid the parallel-
  session race?
- Should tier2 (full 24-mission) be in scope when the script lands,
  or tier1 only?

---

**Status:** docs-only artifact. No code, no script, no build, no
deploy. Slice 3 of approved batch (TERRAIN-HEIGHT-AUDIT-0).
