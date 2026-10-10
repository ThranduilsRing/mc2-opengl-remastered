<!-- doc-status: stale | reviewed:2026-10-11 -->
# RECON — TERRAIN-SUBPASS-RECON-1

Read-only recon mapping the **path-variable terrain solid pass** so the frame graph can model it
as distinct sub-passes BEFORE any modeling. The graph today carries ONE `RenderPassId::Terrain`
row (single reads[]/writes[]/ambient). Terrain solid is path-variable — a graph that models one
branch lies about the others.

> All `file:line` are nifty HEAD at recon time and DRIFT CHEAPLY. Re-grep
> (`repo_query.py slice-preflight`) before coding any slice derived from this doc.

Builds on `frame-graph-executor-staging-recon-1.md` §C (which framed terrain as a 3-way: Indirect /
LOD-chunk / Legacy MLR, with "capture takes legacy"). **This recon CORRECTS that framing**: it is a
**4-state machine**, the true *default* branch is **LOD-chunk** (not Indirect), and the four branches
do not even draw from the same call site or the same source file.

---

## 1. BRANCH ENUMERATION

### Two distinct dispatch sites, not one

The prior recon located one branch point (the txmmgr 3-way at `:2999/:3016/:3089`). There are
actually **TWO** terrain-solid draw sites in a frame, and which one fires depends on the gates:

1. **`code/gamecam.cpp:582`** — `Terrain::flushDrawCommands()`, BEFORE `renderLists()`. This is the
   LOD-chunk submit path (no-op unless `MC2_TERRAIN_LOD_CHUNK` chunk meta built). Routes to
   `gos_terrain_lod_chunk.cpp` `SubmitDrawCommands` → latch at `:1160`.
2. **`mclib/txmmgr.cpp:2999-3114`** — inside `renderLists()`, the 3-way branch (Indirect /
   PatchStream / Legacy MLR masterVertexNodes loop).

The two interact: when LOD-chunk is on (default), gamecam draws terrain at :508, then txmmgr's branch
at `:3011` sets `modernHandled=true` WITHOUT calling `DrawIndirect()` and the masterVertexNode loop
skips terrain (`:3057` resets `currentVertex`, `continue`). So under the default the txmmgr 3-way is
a **no-op for terrain**; the draw already happened in gamecam.

### The branches (4 states)

| Branch name | Draw call | Draw site | Latch site |
|---|---|---|---|
| **TerrainLODChunk** (DEFAULT) | chunk VBO `glDrawElements` | `gamecam.cpp:508` → `gos_terrain_lod_chunk.cpp` SubmitDrawCommands | `gos_terrain_lod_chunk.cpp:1160` |
| **TerrainIndirectBridge** | `glMultiDrawArraysIndirect` | `mclib/txmmgr_lightrow.cpp:645` → `GameOS/gameos/gos_terrain_indirect_pack.cpp:619 DrawIndirect` → `GameOS/gameos/gameos_graphics_bridge.cpp:175 gos_terrain_bridge_drawIndirect` | **NONE — see §4 (HIGH)** |
| **TerrainPatchStreamThin** | thin-record draw | `mclib/txmmgr_lightrow.cpp:650` → `gos_terrain_patch_stream.cpp flush()` | `gos_terrain_patch_stream.cpp:1500` |
| **TerrainLegacyMLR** | `gos_RenderIndexedArray` per masterVertexNode | `txmmgr.cpp:3089/3105` masterVertexNodes loop | `GameOS/gameos/gameos_graphics_tparams.cpp:187` (tess-draw funnel) |

### Activation conditions (gates + defaults + camera state)

- `mc2TerrainLodChunkEnabled()` — **DEFAULT ON** (`mclib/terrain.cpp:60`, cutover 2026-06-09; opt out
  `MC2_TERRAIN_LOD_CHUNK=0`). When on, LOD-chunk owns terrain via gamecam:508; txmmgr suppresses
  Indirect (`:3011-3012`).
- `IsFrameSolidArmed()` (`GameOS/gameos/gos_terrain_indirect_dispatch.cpp:12` = `s_frameSolidArmed && !s_processArmingDisabled`)
  — **CAMERA-WINDOWED**: armed by `ComputePreflight` only when the camera frames a SOLID-terrain
  expanse (header `gos_terrain_indirect.h:409-417`). Reset by `gosRenderer::endFrame()` each frame.
  In-mission with a normal camera it is usually true; deterministic capture/smoke poses usually do
  NOT arm it (the bookmark `tests/visual/bookmarks/mc2_01_terrain_solid.json` exists *specifically*
  to force it true).
- `MC2_TERRAIN_INDIRECT` (`mclib/terrtxm2_internal.h:157`) — default ON (gates colormap-atlas skip / recipe
  build). The Indirect bridge only actually DRAWS when armed AND chunk-path OFF.
- PatchStream branch (`mclib/txmmgr_render_passes.cpp:535`): `TerrainPatchStream::isReady() && !isOverflowed()` AND
  not armed AND not chunk — the un-armed-but-filled middle state.
- Legacy MLR (`mclib/txmmgr_lightrow.cpp:720`): the `else` fall-through — `!modernHandled` (nothing else handled
  terrain). The masterVertexNodes loop with `MC2_DRAWSOLID|MC2_ISTERRAIN`.

### WHICH branch SMOKE/CAPTURE actually takes — confirm/correct prior recon

The prior recon (executor-staging §C, frame-graph-recon-1 :78) said **"capture takes the legacy MLR
branch"** and **"in-game free-cam exercises INDIRECT"**. Both are now STALE/wrong for the default
config:

- **Default in-game = TerrainLODChunk**, NOT Indirect. `MC2_TERRAIN_LOD_CHUNK` is default ON since
  2026-06-09; `mclib/txmmgr_lightrow.cpp:642-643` explicitly suppresses DrawIndirect when chunk is on. Indirect
  only draws if a run sets `MC2_TERRAIN_LOD_CHUNK=0`.
- **Capture/smoke** runs with chunk-path default ON too → capture *also* takes **TerrainLODChunk**,
  not Legacy MLR — UNLESS the run forces `MC2_TERRAIN_LOD_CHUNK=0`, in which case (with no armed
  camera) it falls to PatchStream or Legacy MLR. The "capture takes legacy" claim is only true on
  the chunk-OFF opt-out path, which `terrain.cpp:145` calls "increasingly vestigial."
- The `RENDER-FRAME-PLAN-SCAFFOLD-1` tattle trace (`mclib/txmmgr_lightrow.cpp:657-668`) only observes the txmmgr
  branch — it CANNOT see the gamecam chunk draw at all, so its "TerrainLegacyMLR" verdict in a
  default (chunk-on) run is misleading: terrain was actually drawn by the chunk path upstream and
  txmmgr just fell through. **The scaffold trace under-reports the real default branch.**

**Blunt:** the real default (in-game AND capture) is **LOD-chunk**. Indirect is a non-default
opt-out branch. Legacy MLR is reachable only on the vestigial chunk-OFF path. The prior recon's
"Indirect default / Legacy capture" dichotomy is wrong post-cutover.

---

## 2. PER-BRANCH CONTRACT DELTA

| Axis | TerrainLODChunk (default) | TerrainIndirectBridge | TerrainPatchStreamThin | TerrainLegacyMLR |
|---|---|---|---|---|
| Draw call | chunk `glDrawElements` | `glMultiDrawArraysIndirect` (`ggfx.cpp:4338`) | thin-record draw | `gos_RenderIndexedArray` ×N |
| Draw site | `gamecam.cpp:508` | `mclib/txmmgr_lightrow.cpp:645` (in renderLists) | `mclib/txmmgr_lightrow.cpp:650` | `mclib/txmmgr_lightrow.cpp:720` |
| **writes[]** | MainColor, MainDepth | MainColor, MainDepth | MainColor, MainDepth | MainColor, MainDepth |
| **reads[] base** | ShadowDynamicMap | ShadowDynamicMap | ShadowDynamicMap | ShadowDynamicMap |
| **reads[] extra** | height SSBO (`TERRAIN_HEIGHT_SSBO`), colormap atlas | **recipe SSBO (slot1, `ggfx.cpp:4227`), thin-record SSBO (slot2, `:4250`), colormap atlas (unit0), cement atlas (unit3, `:4172`), transition-mask 2D_ARRAY (unit4, `:4213`)** | thin-record SSBO, atlas | per-node bound texture (`tex_resolve`, `:3088`) |
| **MVP source** | `IsFrameSolidArmed()?getDispatchMvp16():live` (`GameOS/gameos/gos_terrain_lod_chunk_matalbedo.cpp:36`) | **dispatch-snapshot ring-slot MVP** override (`ggfx.cpp:4080 getRingSlotMvp` / `terrainOverrideThinMVP`) + `g_viewContentEpoch` | live MVP | live `terrainMVP` per-draw upload |
| **barrier** | none | **`GL_SHADER_STORAGE_BARRIER_BIT \| GL_COMMAND_BARRIER_BIT`** (`GameOS/gameos/gos_terrain_indirect_pack.cpp:539`, compute→draw) | none | none |
| **ambient: colorMask reassert** | yes (chunk repair) | yes (`ggfx.cpp:4100`, M5 LOAD-BEARING) | yes | yes (via applyRenderStates) |
| **ambient: depthFunc/write** | GEQUAL reverse-Z, write ON | GEQUAL via `applyPipeline(TerrainSolid)` (`ggfx.cpp:4093`) | GEQUAL | GEQUAL |
| **markTerrainDrawn latch** | **SET** (`:1160`) | **NOT SET — HIGH bug class, §4** | **SET** (`:1500`) | **SET** (`ggfx.cpp:7292`, conditional on tess-draw block) |
| **Extra ambient** | — | publishes dispatch-MVP snapshot + bumps epoch BEFORE object phase; binds 5 extra resources; many save/restore | — | sets `gos_State_Terrain`/`TextureClamp` per node |

Notes:
- The Indirect bridge does **far** more binding than any other branch (5 extra resources + sampler
  objects + heavy save/restore at `ggfx.cpp:4039-4050` etc.), and is the only one needing a barrier.
- Legacy MLR's `markTerrainDrawn` is **indirect and conditional**: `gos_RenderIndexedArray` funnels
  into `gosRenderer` whose tess-draw block (`ggfx.cpp:7280`) fires the latch at `:7292` ONLY if
  `gos_State_Terrain && !gos_State_Overlay && terrain_material_ && terrain_batch_extras_count_>0 &&
  terrain_draw_enabled_`. If extras count is 0, the latch does NOT fire on the legacy path (a
  secondary latch-skip risk, lower confidence than the Indirect one).

---

## 3. SHARED vs DIVERGENT

**Invariant across all 4 branches (single Terrain row is partially right):**
- `writes[]` = {MainColor, MainDepth}. Same FBO target (sceneFBO_), same attachments.
- `reads[]` base = ShadowDynamicMap (all four read the dynamic shadow map).
- Depth contract: reverse-Z, GEQUAL compare, depth-write ON.
- colorMask(TRUE) re-assert after the shadow pass (each branch does it, by its own mechanism).
- Intent: every branch is *supposed* to set the markTerrainDrawn latch.

**Genuinely divergent (forces a split):**
1. **reads[]** — Indirect adds recipe SSBO + thin SSBO + cement atlas + transition-mask array (4-5
   resources the other branches don't touch). A graph validating reads[] against producers cannot
   use one row.
2. **MVP source** — dispatch-snapshot+epoch (Indirect, and chunk when armed) vs live (PatchStream,
   Legacy). Ties to RENDER-VIEW-CURRENCY: the snapshot path reads a frame-N-1-ish MVP; the live
   path reads this-frame. An executor reasoning about view currency must know which.
3. **barrier** — only Indirect needs `GL_COMMAND_BARRIER_BIT` (compute→indirect). Mis-modeling this
   as "none" for all = the stale-instance hazard the bridge comment warns about.
4. **draw site / ordering** — LOD-chunk draws in gamecam BEFORE renderLists; the other three draw
   INSIDE renderLists. The single Terrain row implies one ordering slot; the default branch is
   actually at a different point in the frame.
5. **latch reliability** — see §4.

---

## 4. THE markTerrainDrawn LATCH ACROSS BRANCHES  — **HIGH FINDING**

The latch `sceneHasTerrain_` (`gos_postprocess.h:226/228`) is reset each frame
(`GameOS/gameos/gos_postprocess_fbo.cpp:150`) and gates **5 post sub-passes** that bail on `!sceneHasTerrain_`:
screenShadow `:1939`, cloudShadow `:2033/:2176`, shoreline `:2237`, edgeFog/godrays `:2287`, clear-
color decision `:2344`. A branch that draws terrain WITHOUT setting the latch silently kills all 5.

Caller inventory (grep `markTerrainDrawn`):
- `gos_terrain_lod_chunk.cpp:1160` — **TerrainLODChunk sets it.** ✔ (unconditional in SubmitDrawCommands)
- `gos_terrain_patch_stream.cpp:1500` — **TerrainPatchStreamThin sets it.** ✔ (unconditional in flush)
- `GameOS/gameos/gameos_graphics_tparams.cpp:187` — **TerrainLegacyMLR sets it** ✔ but **CONDITIONAL** on the tess-draw
  block guard (`:7280`: needs `terrain_batch_extras_count_>0` etc.). Latch-skip possible if a legacy
  terrain node has zero extras. LOW-MED confidence; legacy path is vestigial.
- `gos_terrain_bridge_drawIndirect` (`GameOS/gameos/gameos_graphics_bridge.cpp:371-~4400`) — **NO markTerrainDrawn
  call.** `DrawIndirect` (`GameOS/gameos/gos_terrain_indirect_dispatch.cpp:1173`) does not call it. `mclib/txmmgr_lightrow.cpp:645`
  (caller) does not call it. **The Indirect branch draws terrain and NEVER sets the latch.**

**HIGH:** if a run takes the **TerrainIndirectBridge** branch as the terrain-solid draw
(`MC2_TERRAIN_LOD_CHUNK=0` + camera armed + `MC2_TERRAIN_INDIRECT` on), terrain renders but
`sceneHasTerrain_` stays false → screenShadow / cloudShadow / shoreline / edgeFog/godrays all
silently skip, and the clear-color decision flips. This is exactly the "dead cloud-shadow pass" bug
the chunk-path comment (`gos_terrain_lod_chunk.cpp:1152-1158`) describes — except the Indirect path
was never given the fix the chunk path got. It is masked today ONLY because chunk is default-on and
suppresses Indirect. The moment someone runs chunk-OFF + indirect (the documented opt-out, e.g. a
perf A/B or the editor GPU path), the latch silently drops.

Recommended follow-up fix slice (out of scope here): add `getGosPostProcess()->markTerrainDrawn()`
at the end of the successful `DrawIndirect()` in `GameOS/gameos/gos_terrain_indirect_dispatch.cpp:1173` (or in the txmmgr
caller after `modernHandled = DrawIndirect()` returns true), mirroring the chunk-path fix. Flag as a
real bug class for the frame-graph executor: **the latch is a per-branch responsibility with one
branch missing it.**

---

## 5. MODELING PROPOSAL (not built)

Three options were posed; recommendation below.

**(a) 3 (really 4) sub-pass rows + mutual-exclusion group + runtime active-branch probe.**
**(b) one Terrain row with conditional per-branch fields.**
**(c) a `capturePath` flag so validation knows which branch capture exercises.**

### Recommendation: **(a) + (c) combined** — sub-pass rows with a `producesTerrainLatch` audit and a runtime active-branch probe.

Rationale: the four branches diverge on reads[] (Indirect's +4 SSBO/atlas resources), barrier, MVP
source, AND draw site — too much for option (b)'s conditional fields to stay honest; a reviewer
cannot tell from one row that Indirect needs a COMMAND barrier and reads a cement atlas. Option (c)
alone doesn't fix the reads[]/barrier modeling. So model 4 rows, share the invariant via a trait,
and add a runtime probe (mirroring the ambient/FBO guards) that asserts exactly one fired and that
the active branch's declared latch matches reality.

Concrete changes implied:

- **`RenderCore/RenderPassContract.h`** — append 4 rows (renumber-safe append):
  `TerrainLODChunk`, `TerrainIndirectBridge`, `TerrainPatchStreamThin`, `TerrainLegacyMLR`. Keep the
  existing `Terrain` id as a deprecated alias OR a "terrain family" parent. All four: writes
  {MainColor, MainDepth}, reads += ShadowDynamicMap. Per-row deltas: Indirect reads +=
  {TerrainRecipeBuffer, TerrainThinBuffer, CementAtlas, TransitionMaskArray} and `barrierAfter =
  Command`; chunk reads += {TerrainHeightSsbo}. Add a `mutuallyExclusive` group id so the validator
  knows only one fires/frame.
- **NEW RenderResourceIds** (RenderResourceRegistry): `TerrainRecipeBuffer`, `TerrainThinBuffer`,
  `CementAtlas`, `TransitionMaskArray`, `TerrainHeightSsbo` — none exist today. (Same "new id"
  shape Stage A's FBO ledger needs.)
- **`RenderCore/ambient_contract.h`** — the `producesTerrainLatch` field already exists (`:69`).
  Set it `true` on ALL FOUR terrain rows (it documents intent); then a runtime guard can FLAG the
  Indirect row whose draw does not actually set the latch (turns §4's HIGH bug into a tested
  invariant). Add a `capturePath` bool on each row, set true on whichever branch the active config
  takes (probe-driven, not hardcoded — see below).
- **`RenderCore/fbo_ledger.h`** (Stage A) — all four terrain rows map to sceneFBO_ → {MainColor,
  MainDepth}; no per-branch FBO divergence, so the FBO ledger is shared. Good: the FBO axis is the
  one place a single row IS correct.
- **`tests/unit/test_frame_graph.cpp`** — extend the existing latch test (`:96`) to assert every
  terrain row with `producesTerrainLatch=true` has a real `markTerrainDrawn` caller (static check
  against the grep inventory in §4) — this would have caught the Indirect miss.

### Sizing / risk

- **Modeling-only, zero behavior change.** MED effort: 4 rows + 5 resource ids + mutual-exclusion
  group concept + capturePath field + extending the latch test.
- **Runtime active-branch probe IS needed** (do not hardcode "capture = legacy"). The active branch
  is a function of `mc2TerrainLodChunkEnabled()` × `IsFrameSolidArmed()` × `MC2_TERRAIN_INDIRECT` ×
  PatchStream readiness — all runtime. A probe (mirror `noteRenderPass` / the ambient compare) reads
  those four and emits which branch drew, so validation compares declared-active vs actually-active.
  Without it the `capturePath` flag would be a stale hardcode (exactly the trap the scaffold trace
  fell into). Cheap: the inputs are all already-exposed predicates.
- **Risk LOW** for the modeling; the only real-world risk is the §4 latch bug, which modeling
  *surfaces* but does not fix.

---

## 6. EXECUTOR IMPACT

An executor cannot schedule "Terrain" until it knows, per frame, **which** of the four sub-passes is
live and that sub-pass's reads / writes / ambient / barriers / draw-site. What must be modeled first:

1. **The 4-branch enumeration with a runtime active-branch probe** (§1/§5). The executor must query
   the active branch each frame — it is not static. Until the probe exists, the executor would
   schedule a fictional single Terrain pass.
2. **Per-branch reads[]** — especially Indirect's +4 SSBO/atlas resources, so a resource-DAG dry-run
   doesn't flag them as unproduced (or miss that they must be ready before the draw).
3. **The COMMAND barrier on the Indirect branch** — the executor must emit/verify the compute→draw
   barrier; modeling it as "none" (the single-row default) corrupts the Indirect schedule.
4. **MVP-source per branch** (snapshot+epoch vs live) — tie-in to RENDER-VIEW-CURRENCY; the executor
   must know the snapshot branches publish dispatch-MVP + bump `g_viewContentEpoch` before the object
   phase, so downstream same-frame consumers (water fast path, decals, cull) read the right matrix.
5. **The markTerrainDrawn latch as a per-branch produces-edge** (§4). The executor must treat the 5
   post sub-passes as consumers of a latch *produced by whichever terrain branch fired* — and must
   know the Indirect branch currently fails to produce it (HIGH). This is the cross-phase edge that
   makes pass reordering dangerous and that a dry-run must validate.
6. **The draw-site split** — LOD-chunk (default) draws in gamecam BEFORE renderLists; the other
   three draw inside renderLists. The executor's pass-ordering model must place the terrain pass at
   the correct frame point for the active branch, not assume the renderLists slot.

Until 1-6 are modeled, a dry-run that walks `kFramePassOrder[]` with a single Terrain row validates a
branch that, in the default config, **isn't even the one that draws** (it's the suppressed Indirect
slot, while the real draw happened upstream in the chunk path).

---

## SUMMARY (TL;DR)

- Terrain solid is a **4-state machine**, not 3: **TerrainLODChunk (DEFAULT, draws in
  `gamecam.cpp:508`), TerrainIndirectBridge, TerrainPatchStreamThin, TerrainLegacyMLR** (latter
  three in `txmmgr.cpp:2999-3114`).
- **Prior recon CORRECTED:** the default branch (in-game AND capture) is **LOD-chunk**, not Indirect.
  `MC2_TERRAIN_LOD_CHUNK` default ON since 2026-06-09 (`terrain.cpp:139`) and `mclib/txmmgr_lightrow.cpp:642`
  suppresses Indirect. Legacy MLR is reachable only on the vestigial chunk-OFF path. "Indirect
  default / Legacy capture" is stale.
- Branches diverge on **reads[]** (Indirect +4: recipe SSBO, thin SSBO, cement atlas, transition
  mask array), **barrier** (only Indirect needs `GL_COMMAND_BARRIER_BIT`, `gos_terrain_indirect.cpp:3606`),
  **MVP source** (snapshot+epoch vs live), and **draw site** (gamecam vs renderLists).
- Invariant (single-row-correct part): all write {MainColor,MainDepth} to sceneFBO_, all read
  ShadowDynamicMap, all reverse-Z GEQUAL, all reassert colorMask(TRUE).
- **HIGH — latch-skip bug class:** `markTerrainDrawn()` (sets `sceneHasTerrain_`, gates 5 post
  passes) is set by chunk (`:1160`), patch-stream (`:1500`), and legacy (`ggfx.cpp:7292`, conditional)
  — but **the Indirect bridge NEVER sets it**. Masked today only because chunk-default suppresses
  Indirect; a chunk-OFF + armed + indirect run silently kills screenShadow/cloudShadow/shoreline/
  edgeFog/clear-color. Fix: add `markTerrainDrawn()` after successful `DrawIndirect()`.
- **Modeling rec:** 4 sub-pass rows + shared "terrain family" trait + mutual-exclusion group + a
  **runtime active-branch probe** (active branch is runtime, not static) + `producesTerrainLatch`
  audit + a unit test that every latch-producing row has a real caller. MED effort, modeling-only,
  zero behavior change; surfaces (not fixes) the HIGH latch bug.
- **Executor blocker:** cannot schedule "Terrain" until the active branch is probed per-frame and
  each branch's reads/barrier/MVP/latch/draw-site is modeled — today's single row validates the
  suppressed Indirect slot while the chunk path actually drew upstream.

Doc: `docs/render-backend-seams/terrain-subpass-recon-1.md`
