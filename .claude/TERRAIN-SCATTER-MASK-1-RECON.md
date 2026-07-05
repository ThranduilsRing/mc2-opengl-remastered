# TERRAIN-SCATTER-MASK-1 — RECON

**Slice:** TERRAIN-SCATTER-MASK-1 (mask-driven foliage/rock scatter, cook-offline)
**Worktree:** A:/Games/mc2-controlmap-sample-1 (branch claude/controlmap-sample-1, HEAD 5ded0f19) — read-only recon.
**Date:** 2026-07-01

## Executive summary

The cheapest, engine-change-free cook path already exists in pieces and just needs wiring:
**offline Python reads authored/analyzer masks → runs mask-weighted blue-noise placement →
appends the chosen instances into the mission `.pak` packet-1 (terrain-objects packet), reusing
the same 40-byte record the game already loads.** This is a *pure data-pack edit* — zero C++,
zero shader, zero runtime gate — and it **structurally sidesteps the black-tree landmine**: we are
not skipping or frame-stamp-gating any registered `TG_MultiShape` at runtime; we are authoring more
real placed objects, which the engine registers/prewarms/culls through its normal, proven path.

Object type to scatter = an existing **BUILDING/TREEBUILDING-class dressing prop** (the MarbleCliff
FitID 1188 pattern, already installed via `tools/pak_append.py` + a `data/tgl/*.ini` + a FIT packet
with `SetImpassable=0`/`blockLineOfFire=FALSE` for pass-through dressing). TREE-class also works and
is the only class that gets pitch/roll tilt, but has a harder catalog wiring; BUILDING-class is the
proven P0 vehicle. Per-instance the record carries **yaw only** (`rotation` float) — **no per-instance
scale** (hard constraint; see landmines).

## Today's tree/prop entry points (verified)

- **Terrain-objects packet = per-mission `.pak` packet 1.** Editor writes it via
  `EditorObjectMgr::save(file,1)` (editor/EditorData.cpp:2183, per
  docs/recon/terrain-runtime-api-recon-1-pak-format.md:21). Game reads it in
  `GameObjectManager::countTerrainObjects` (**code/objmgr.cpp:1259-1334**): `readInt()` count, then
  per object `objTypeNum, x, y, z, rotation(float), damage, teamId, parentId, pad, pad` =
  **40 bytes/record, little-endian** (objmgr.cpp:1289-1302). This is byte-identical to the analyzer's
  `OBJREC_SIZE=40` reader (mission_terrain_analyzer.py:58-59, 322-327) — one canonical format, both
  directions proven.
- **objTypeNum → class** in `countObject` (objmgr.cpp:1336-1380): TREE/TERRAINOBJECT/BUILDING/
  TREEBUILDING/TURRET/GATE/BRIDGE. Object *types* (chassis/building defs) live in the **global
  `object2.pak`** keyed by objTypeNum (objtype.cpp:355 seekPacket), NOT the per-mission `.pak`.
- **Trees vs buildings tilt asymmetry (bdactor.cpp):** trees get `EulerAngles(pitch, yaw, 0)`
  (:6744 — CAN tilt to slope); buildings get `EulerAngles(0, yaw, 0)` (:3966 — yaw only). Both ground
  to a **single** bilinear terrain sample at origin (`TerrainRuntime::groundElevation`,
  bdactor.cpp:1306/3969/6747) — no per-instance z-offset field exists.
- **World→grid transform** (analyzer:325-326, matches terrain.h:446 `worldToTile`):
  `col=(x-tlx)/128, row=(tly-y)/128`, `tlx=-side*128/2, tly=+side*128/2`, `WORLD_UNITS_PER_VERTEX=128`.
- **Prewarm/registration path** already correct for placed BUILDING/TREE: editor
  `Bldg_ForceRenderShapeTexturesResident` on place (EditorObjectMgr.cpp:2427/2457 → bdactor.cpp:747);
  runtime registers into GPU static-prop substrate normally.

## Recommended cook design (RECOMMENDED: packet-1 writer)

**Chosen path: offline emit into packet-1** (vs a new runtime instance list). Rationale:
- Zero engine change / zero gate (data-only). The instances ARE ordinary editor objects afterward.
- Reuses proven tooling: `tools/pak_append.py` (PacketFile append/replace, pytest-covered) and the
  analyzer's `read_object_footprints` reader (parses the existing packet-1) — we need a small
  **packet-1 *rewriter*** (read count+N*40, append M new records, rewrite count, replace packet)
  ~40 lines, mirroring pak_append's `replace` + `write_pak`.
- Editor round-trip is free (see below) — cooked props appear as normal, editable, deletable objects.
- A "new runtime instance list" would need a C++ loader, a gate, its own registration/prewarm/cull
  wiring, and would re-open the exact static-registry currentness surface the landmine lives in.
  **Rejected.**

**Cook pipeline (all Python, deterministic):**
1. Read mission `.pak`: locate packet-0 MapData (analyzer `locate_mapdata`) → `side`, per-vertex
   elevation/overlay/water; locate packet-1 terrain-objects (analyzer `read_object_footprints`).
2. Read masks (see Masks below) → per-cell weight + hard-exclusion grids.
3. Sample slope/water/overlay/footprint gates from the pak height itself (analyzer `derive_masks`
   already yields slope_deg, water, shoreline, cliff, flat_playable, protected_hard).
4. Run mask-weighted blue-noise (seeded) → world (x,y,z) placements + yaw.
5. Emit each as a 40-byte record with the dressing prop's objTypeNum, damage=0, teamId=-1,
   parentId=-1, pad=0. Append to packet-1's list, rewrite count, write NEW `.pak` (baseHash-gated,
   `.beauty`/`.terrain2`-style reversibility — do NOT mutate stock in place).
6. Preview: render a workbench PNG (mask + placed points) via existing
   `tools/terrain_beautify/terrain_workbench.py` conventions.

## Placement algorithm

- **Blue-noise / Poisson-disk with mask weighting.** Per candidate cell: accept probability ∝
  `veg_density_mask` (or rock mask), rejected if inside any exclusion. Deterministic: fixed `--seed`,
  hashed per-cell RNG (mirror `foliage_generator.py` determinism; guard PYTHONHASHSEED like
  tools/campaign_gen tests). Min-distance disk radius scales inversely with local density.
- **Exclusion (hard) masks — subtract from candidate set:** `protected_hard = overlay(roads/concrete)
  | building_footprints | water` (analyzer:381); plus **slope gate** (`slope_deg > CLIFF_SLOPE_DEG`
  for trees, or a veg-max-slope threshold); **shoreline band** (`masks["shoreline"]`) as an *include*
  band for beach/rock scatter, exclude for forest.
- **Gates from pak height** (already computed by analyzer `derive_masks`): slope, water depth,
  flat_playable. No new geometry sampling needed.
- Rocks vs trees: two mask channels + two prop objTypeNums; rock scatter may *include* the cliff/slope
  mask that trees exclude.

## Masks wanted → sources (all already producible)

| Mask | Source |
|---|---|
| vegetation density | authored raster (paint) OR analyzer `flat_playable` × inverse-slope. control_map channel is a candidate carrier. |
| rock scatter | analyzer `cliff_candidates` / slope band; or authored channel. |
| exclusion (road/concrete/water) | analyzer `roads_overlays` (overlay hi16), `water`, `building_footprints` → `protected_hard`. |
| slope gate | analyzer `slope.png` / `slope_deg`. |
| shoreline band | analyzer `shoreline.png` (`land & dilate(water) & ~water`). |

control_map precedent: `data/missions/<stem>.beauty/control_map.png` loaded at
**mclib/terrain.cpp:856** (stb_image decode wrapper terrain.cpp:65). A `<stem>.beauty/` sidecar is the
natural home for authored scatter masks + a cook manifest (baseHash gate, mission_sidecar.py:42-46,95).

## Caps / perf

- Each instance = 1 static-prop substrate record; frustum-culled + LOD-switched normally
  (bdactor.cpp:621; TERRAIN-CLIFF-MESH-DRESSING recon:49). **No "distant props never culled" hard
  rule found** — they honor `Distance` LOD, so distant instances get the cheap LOD.
- Real cost axes to respect: (a) per-object **prewarm** on load (texture residency); (b) GOM
  TerrainObjects `ObjUpdate` + per-frame `BldgAppearance::touch` light resubmit (MEMORY note). So the
  cap is driven by *update/light-resubmit*, not draw. Recommend a **conservative first cap (~300–800
  props/map)** with a `--max-instances` cook arg + per-cell density clamp; measure prewarm+frame cost
  before raising. Stock missions carry far fewer objects (analyzer object_count typically tens).
- Share ONE prop type (one objTypeNum) so all instances batch into the same substrate record set.

## Editor round-trip

- Cooked records ARE terrain-objects packet-1 entries → the editor loads them via
  `EditorObjectMgr::load(pFile,1)` (EditorData.cpp:501) as **normal placed objects: selectable,
  movable, deletable** (delete de-draw already handled, EDITOR-STATIC-DEDRAW). No special marker
  needed for basic editability.
- **Re-cook without duplication:** the cook is idempotent only if it can tell "its own" instances from
  hand-placed. Options (recommend #1): (1) reserve a dedicated scatter objTypeNum(s) — the re-cook
  first *deletes all records of that type*, then re-emits (hand-placed buildings use other types, so
  they survive). (2) A sidecar list of cooked (x,y,z) to diff. #1 is simplest and marker-free.

## Files / tools to touch

- **NEW** `tools/terrain_beautify/cook_scatter.py` — the cook driver (reads masks + pak, places,
  emits). Reuse analyzer functions (import, don't fork).
- **NEW/extend** a packet-1 rewriter — ~40 lines modeled on `tools/pak_append.py`
  `cmd_replace`+`write_pak` (append records to the decoded packet-1 payload, rewrite in place).
- Masks: author under `data/missions/<stem>.beauty/` (control_map channels or new `scatter_*.png`);
  reuse `mission_terrain_analyzer.py` to derive slope/water/shoreline/exclusion.
- Dressing prop asset: reuse the MarbleCliff FitID-1188 install pattern (data/tgl/*.ini + object2.pak
  FIT packet + Buildings.csv line) for rock; a tree prop analogously (TREE class for tilt) or a
  broadleaf building-class prop. No new C++.
- **NEW** `tests/…/test_cook_scatter.py` (pytest) + a workbench preview image.

## Landmines

1. **Black-tree static-registry landmine (user-pinned) — SIDESTEPPED, do not reintroduce.** Root
   cause was a per-frame skip freezing `cachedFrame_` on a registered `TG_MultiShape`, so the registry
   `flush()` dropped it (07a1f8ac; docs/frame-contracts/skip-safety-audit-1.md; objmgr.cpp:173
   `MC2_SKIP_STATIC_TREES`). **The cook approach must NOT add any runtime veg-suppress / shader-skip /
   frame-stamp-gate.** Emitting real packet-1 objects avoids this entirely — they ride the proven
   register/prewarm/cull path. Any "hide veg under roads at runtime" temptation → do it in the COOK
   (don't emit there), never at runtime.
2. **No per-instance scale.** The 40-byte record has yaw (`rotation` float) but **no scale field**
   (objmgr.cpp:1289-1302). Size variety must come from multiple prop *types* or mesh-authored variants,
   not per-instance. Do not invent a scale column — the game reader would misparse.
3. **Single-point grounding + yaw-only (buildings).** A wide rock/cliff prop pins to one height and
   won't match slope (bdactor.cpp:3966/3969). Keep scatter props small; self-sink the mesh (author base
   below origin). Trees CAN tilt (bdactor.cpp:6744) — prefer TREE class for slope-hugging veg.
4. **Prewarm/light-resubmit cost, not draw, is the cap.** Over-scatter → load-time prewarm stall +
   per-frame `BldgAppearance::touch` light resubmit. Cap conservatively.
5. **Static-prop registration aborts on first non-SHAPE_NODE child** — the dressing GLB must import
   clean or the whole prop silently vanishes (TERRAIN-CLIFF-MESH-DRESSING recon:44,71).
6. **fitID must pre-exist / append discipline.** New object *type* needs a real appended packet in
   object2.pak (seekPacket asserts, EditorObjectMgr.cpp:448); reuse the proven `pak_append.py` flow and
   round-trip-verify byte-identity of untouched packets.
7. **baseHash gate for reversibility.** Cook must refuse a mismatched/already-modified base `.pak`
   (mission_sidecar.py:119-124 precedent) and write a NEW pak, never mutate stock in place.

## Acceptance

- **Offline pytest:** cook is deterministic (same seed → byte-identical output pak); untouched packets
  byte-identical pre/post (round-trip like tools/test_pak_append.py); re-cook is idempotent (delete-
  by-type + re-emit yields same instance set); exclusion masks respected (0 instances in
  protected_hard cells).
- **Preview image:** workbench PNG overlaying density/exclusion masks + placed points (visual sanity).
- **One smoke:** deploy a cooked mission, `run_smoke.py --tier tier1` (or a single `--mission`) — exit
  0, no crash, no black-prop regression.
- **Gate story:** **data-only → no engine gate needed.** The cook is opt-in per-mission (only cooked
  missions carry the scatter); stock missions untouched and byte-identical. If a future *runtime*
  toggle is ever wanted, it belongs on the asset/catalog side, never as a runtime skip.

## Open questions (need user ruling)

1. **Mask authoring source:** paint new `scatter_*.png` channels under `<stem>.beauty/`, or derive
   purely from analyzer masks (slope/flat/shoreline) with no hand-authoring for v0? (Affects whether an
   authoring/import step is in-scope.)
2. **Prop asset(s):** reuse MarbleCliff (building, yaw-only) for rock + author one tree/broadleaf prop
   (TREE class, tilts) for veg? Or single prop type for v0? Which GLB sources.
3. **Instance cap** for v0 (recommend ≤ ~500 until prewarm/frame cost measured) — user ceiling?
4. **Re-cook identity:** reserve dedicated scatter objTypeNum(s) for delete-and-re-emit (recommended),
   acceptable? (No marker on the record itself.)
5. **Sidecar vs in-pak:** emit into the mission `.pak` directly (editable in-editor immediately), or
   keep a `.beauty`-style patched-pak alongside stock? (Recommend patched-new-pak, hash-gated.)
