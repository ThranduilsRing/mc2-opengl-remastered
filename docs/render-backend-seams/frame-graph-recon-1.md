<!-- doc-status: stale | reviewed:2026-10-11 -->
# RECON — FRAME-GRAPH-RECON-1

Read-only input for **FRAME-GRAPH-SKELETON-1** (declarative `declarePass().reads().writes().forbids()`
wrapping the EXISTING `kRenderPassContracts[]`). We are building a frame-graph **CONTRACT** (declaration +
validation), NOT an executor. This doc reconciles the shipped contract tables against the real frame and
states honestly what blocks a future executor.

**Reuses, does not re-derive:**
- `RenderCore/RenderPassContract.h` — `RenderPassId` (11), `kRenderPassContracts[11]`, `kFramePassOrder[11]`, per-row `reads[4]`/`writes[4]`/`barrierAfter` + load/store/layout.
- `RenderCore/RenderResourceRegistry.h` — `RenderResourceId` (MainColor/Depth, Shadow{Static,Dynamic}Map, TerrainHeight, MaterialGpuBuffer, WaterReflection{Color,Depth}).
- `mclib/render_contract.{h,cpp}` — `PassIdentity` (15 callsite tags), `PassStateContract`, `noteRenderPass`, `beginPass/endPass` (CONTRACT-3 order audit), `beginPassScope` (scope stack), `toRenderPassId()` lossy collapse.
- `docs/render-backend-seams/render-frame-plan-recon-1.md` (§A/§B/§C/§E/§F — canonical frame topology) and `render-contract-index-1.md` (RENDERPASS-ENUM-RECON-1 + DAG-CONTRACT-1 results).
- Currency layer (shipped): `GameOS/gameos/gos_object_draw_mvp.h`, `gos_frame_context.h`; `scripts/check-object-mvp-currency.py`.

> Line numbers drift cheaply. The prior recon docs were against `0390b805`/`1d7b9ea6`; re-grep before relying on any `file:line` below.

---

## 1. PASS CATALOG

One row per `kFramePassOrder[]` entry (`RenderPassContract.h:353-365`), in execution order. `reads`/`writes`/`barrierAfter`
columns reference the **already-encoded** table (`kRenderPassContracts[]`, `RenderPassContract.h:163-334`) — not re-invented.
"View dep" = which camera matrix the pass projects through. "Temporal" = consumes N-1 data.

| # | Pass (RenderPassId) | Producer (owner fn / file) | Reads (table) | Writes (table) | View dep | Temporal | Phase | Global-state deps it reads |
|---|---|---|---|---|---|---|---|---|
| 1 | **Shadow** | `gosPostProcess` + per-lane shadow programs; dynamic pass `txmmgr.cpp:2569-2820` (R6); static build R4 `:2472` | `{}` | `ShadowDynamicMap` | light/shadow view (own ortho), NOT main cam | static atlas built once/mission (pre-seeded `frameBegin()` `mclib/render_contract.cpp:924`) | shadow | `g_numShadowShapes` (clamp `std::min(...,MAX_SHADOW_SHAPES)`); `glColorMaski(FALSE)` on attach 1/2 `gos_postprocess.cpp:2798-2799` |
| 2 | **StaticPropOpaque** | `GpuStaticPropBatcher` `glMultiDrawElementsIndirect` `gos_static_prop_batcher.cpp:6051`; R8 `txmmgr.cpp:3012-3120` | `ShadowDynamicMap` | `MainColor`, `MainDepth` | **ViewUniforms UBO (binding=3)** + `gos_GetObjectDrawMVP()` (currency-checked) | no | static-prop-opaque | `g_useGpuStaticProps`/`g_useGpuObjects` `txmmgr.cpp:3136-3138`; ShadowDynamicMap tex unit |
| 3 | **Terrain** | `TerrainPatchStream` / legacy MLR `txmmgr.cpp:2931-2993` (R7 `gos_RenderIndexedArray`) vs indirect bridge `:2999` | `ShadowDynamicMap` | `MainColor`, `MainDepth` | **PATH-VARIABLE** (see §3): live `terrainMVP` (legacy) vs dispatch snapshot (indirect) | no | terrain | `IsFrameSolidArmed()` `txmmgr.cpp:2999`; **publishes dispatch-MVP snapshot + bumps `g_viewContentEpoch`**; **calls `markTerrainDrawn()` → sets `sceneHasTerrain_`** (latch, §2) |
| 4 | **MechOpaque** | `GpuMechBatcher::submitActor` `glDrawElementsInstancedBaseVertex` `gos_mech_batcher.cpp:1004/2454`; R9 `mclib/txmmgr_lightrow.cpp:748` | `ShadowDynamicMap` | `MainColor`, `MainDepth` | **ViewUniforms UBO** + `gos_GetObjectDrawMVP()` (PipelineId::MechOpaque, applyPipeline) | no | mech | `g_useGpuObjects`/`g_useGpuMechs` `txmmgr.cpp:2675-2677,3238`; ShadowDynamicMap unit |
| 5 | **TerrainDecal** | `craterManager` / `quad.cpp`; gameos_graphics `:9893/9967`; R11/R13 `txmmgr.cpp:3152/3184` | `MainDepth` | `MainColor` | terrain-phase: raw dispatch snapshot (sanctioned, §5) | no | terrain-decal | coplanar w/ terrain depth; `glColorMaski` re-set `gos_postprocess.cpp:3022-3024` |
| 6 | **TerrainOverlay** | `quad.cpp` M2d producer; gameos_graphics `:9772`; R11 `mclib/txmmgr_render.cpp:146` | `MainDepth` | `MainColor` | terrain-phase: raw dispatch snapshot (sanctioned, §5) | no | terrain-overlay | overlay alpha; cement atlas |
| 7 | **Water** | `quad.cpp` / `renderWaterFastPath` (MDI) `GameOS/gameos/gameos_graphics_water.cpp:251` WaterStream; R14 legacy loop `txmmgr.cpp:3207-3337` | `MainDepth` | `MainColor` | raw GL + `gos_InvalidateRenderStateCache` after | **YES if `MC2_WATER_REFLECTION_RT`** reads `WaterReflection{Color,Depth}` (own RT, built `gamecam.cpp:522`) | water | `WaterArmed`; reflection RT gate |
| 8 | **VegetationCards** | `VegetationAdapter` / `gos_vegetation`; `gamecam.cpp:554` | `MainDepth`, `ShadowDynamicMap` | `MainColor`, `MainDepth` | live MVP (alpha-discard cards) | no | vegetation | `MC2_VEGETATION_CARDS` (killSwitchEnv); ShadowDynamicMap unit |
| 9 | **VFX** | `mc2::particles::Batcher`; particles `gamecam.cpp:585/594` | `MainDepth` | `MainColor` | live MVP | no | vfx | **Object-ID PROHIBITED** (`check-vfx-no-objectid.sh`); additive blend |
| 10 | **UI** | GameOS 2D / HUD batch replay `gameos_graphics.cpp:7584/7653` | `{}` | `MainColor` | screen-space (no world MVP) | no | ui | `renderStates_` snapshot replay; **rebinds `gVAO`** (§2); no depth |
| 11 | **PostProcess** | `gos_postprocess::endScene()` `GameOS/gameos/gos_postprocess.cpp:816` (after `gos_RendererEndFrame`, `gameosmain.cpp:594`) | `MainColor`, `MainDepth`, `ShadowDynamicMap` | `MainColor` | fullscreen quad; no world MVP | reads this-frame color/depth | post-process | **`sceneHasTerrain_`/`prevFrameHadTerrain_` gate 4 sub-passes** (`gos_postprocess.cpp:1303/1936/2030/2173/2234/2284/2341`); reverse-Z clear; `glColorMask(TRUE)` re-assert `:1418` |

Notes on table fidelity:
- `kFramePassOrder[]` lists Shadow FIRST even though shadow-caster *enqueue* happens after geometry; `frameBegin()` pre-seeds `ShadowStaticMap`+`TerrainHeightTexture` (`render_contract.cpp:769-770`) so the order audit doesn't false-flag. Shadow-before-geometry is a **position-encoded** edge, not a stored `dependsOn[]` (comment `RenderPassContract.h:344-352`).
- The table's `MechOpaque` row covers vehicles + legacy buildings too (lossy `toRenderPassId()`, `render_contract.cpp:550`). `AlphaObject` and `OpaqueObject` PassIdentity tags BOTH collapse to `MechOpaque`.
- 10 `PassIdentity` tags have a `RenderPassId` lane; `Unknown` and `DebugOverlay` collapse to `RenderPassId::None` (no lane).

---

## 2. AMBIENT-STATE LEDGER  *(the executor's main blocker — first-class)*

State NOT owned/restored by `applyPipeline` (which sets only depth/blend/cull/frontFace/polyOffset per
`render-frame-plan-recon-1.md §F`). Each row: who sets, who assumes, self-repairs?, order-dependent?

| Ambient state | Set by | Assumed by | Self-repairs? | Order-dependent? |
|---|---|---|---|---|
| **colorMask (per-attachment)** | Shadow lanes `glColorMaski(1/2,FALSE)` `gos_postprocess.cpp:2798-2799,3022-3024,3206-3208`; shadow-blob `glColorMask(FALSE)` `:3470/3492`; **terrain re-asserts `glColorMask(TRUE)`** `:1418/3501` (the load-bearing shadow-leak repair) | every color-writing pass after shadow | YES — terrain re-assert papers over shadow's FALSE | **YES — fatal.** Reorder/wrap so terrain doesn't re-assert → color stranded FALSE (invisible scene) |
| **MRT / glDrawBuffers mask** | `glDrawBuffers(1/3/2)` vs `sceneFBO_` `gos_postprocess.cpp:225/235/241`; GBuffer1 sentinel cleared `gameosmain.cpp:569` | passes declaring `location=1` (GBuffer1) / `location=2` (objectId) | partial (re-set per pass group) | YES — objectId/thermal viewmode depends on `sceneObjectIdTex_` bound |
| **viewport** | raw everywhere: `gameosmain` + every post helper + `txmmgr.cpp:2407/2453`; FORCE-43 re-set before composite | all passes | NO owner — purely inherited/re-set ad hoc | YES |
| **scissor / stencil** | no per-pass owner; stencil cleared once `gameosmain.cpp:563` | nobody re-establishes per pass | NO — inherited across whole frame | YES (silent) |
| **depth func / write** | `applyPipeline` for routed passes; raw elsewhere; reverse-Z scene = `GL_GEQUAL`, shadow = `GL_LESS` (`render_contract.cpp:493-506`) | every depth-tested pass | partial (applyPipeline owns routed only) | YES |
| **blend state** | `applyPipeline` (routed); raw for legacy/post | opaque passes assume OFF | partial | YES |
| **bound FBO** | `endScene` saves/restores; `glDrawBuffers` callers; bind FB0 before composite | every pass | partial (endScene restores caller FBO/VAO) | YES |
| **active program** | per-draw `apply()` / `glUseProgram` | each draw sets own | YES (each pass binds own program) | no |
| **texture-unit latches** | only composite unit-0/2 guard-wrapped (`GlScopedTextureUnit`); `gos_InvalidateRenderStateCache` does NOT track tex units | samplers assume their unit still bound | NO for non-guarded units (NVIDIA 2D_ARRAY residual leak, open) | YES |
| **clip control / reverse-Z** | reverse-Z clear `clearDepth=0` `gameosmain.cpp` (`render-frame-plan-recon-1.md §A row 0`) | all scene depth passes assume reverse-Z | NO — frame-global assumption | YES |
| **VAO** | `endScene` saves/restores caller VAO; **HUD rebinds `gVAO`** `gameos_graphics.cpp:7653` | HUD + post quads | partial (endScene restores) | YES |
| **`markTerrainDrawn()` / `sceneHasTerrain_`** | set during terrain draw; reset each frame `gos_postprocess.cpp:1405`; `prevFrameHadTerrain_` saved `:1404` | **screenShadow/cloudShadow/shoreline/edgeFog/fogOob + clear color** bail if false (`:1303/1936/2030/2173/2234/2284/2341`) | NO — terrain taking a path that skips the latch silently kills 4 post passes (documented landmine) | **YES — cross-phase latch** |
| **`g_viewContentEpoch` / dispatch-MVP snapshot** | published by terrain solid draw; `gos_object_draw_mvp.h:29,45` | object/mech/prop draw via `gos_GetObjectDrawMVP()` epoch-match | falls back to live MVP on epoch mismatch (safe) | YES (must publish before object phase) |
| **`gos_InvalidateRenderStateCache()`** | after water fast path `gamecam.cpp:530`; `GameOS/gameos/gos_postprocess_diag.cpp:219` | legacy `gos_SetRenderState` consumers | YES (forces re-apply) but does NOT track tex units / colorMask | YES |

**Punchline:** `applyPipeline` owns ~5 of ~13 ambient axes. The rest are inherited, ad-hoc-re-set, or
self-repaired-by-side-effect. The single most dangerous is **colorMask** (shadow sets FALSE, terrain re-asserts
TRUE — an implicit ordering contract invisible to every contract table).

---

## 3. PATH-VARIABLE PASSES

| Pass | Branch axis | Capture/smoke hits | Citation |
|---|---|---|---|
| **Terrain solid** | `if (IsFrameSolidArmed()) DrawIndirect()  else if PatchStream::isReady() flush()  else → LEGACY MLR gos_RenderIndexedArray` | **LEGACY MLR branch** — `IsFrameSolidArmed()` is CAMERA-WINDOWED; deterministic capture/smoke does NOT arm | `mclib/txmmgr_lightrow.cpp:526-630`; `render-frame-plan-recon-1.md §E` |
| **StaticPropOpaque** | `g_useGpuStaticProps \|\| g_useGpuObjects` gate | GPU path default-on (STATIC-PROP-V3-FLIP) | `mclib/txmmgr_lightrow.cpp:764-766` |
| **MechOpaque** | `g_useGpuObjects \|\| g_useGpuMechs` gate (`MC2_GPU_MECHS`) | GPU path | `txmmgr.cpp:2675-2677,3238` |
| **Water** | `WaterArmed` MDI fast path vs legacy quad loop | variable | `GameOS/gameos/gameos_graphics_water.cpp:682`; `mclib/txmmgr_render_phases.cpp:251` |
| **Vegetation** | `MC2_VEGETATION_CARDS` (default off) | off in capture | killSwitchEnv |

**Structural smell (from prior recon, still true):** Any frame-graph that models only the modern indirect terrain
bridge will NOT match capture, which exercises the legacy MLR branch. The contract MUST encode BOTH terrain
branches and which one capture hits. This is the #1 capture/smoke divergence.

---

## 4. DEPTH-FRAGILE / PINNED ORDERINGS  *(must NOT be reordered)*

| Ordering | Why pinned | Citation (code comment) |
|---|---|---|
| **Shadow before geometry** | shadows enqueue AFTER geometry but resolve FIRST inside flush; `frameBegin()` pre-seeds ShadowDynamicMap to paper over | `RenderPassContract.h:344-352`; `render_contract.cpp:769` |
| **R3 (hardware-queue objects) before R7 (TerrainSolid)** | TerrainSolid overwrote building pixels otherwise; relocated flushes at `:2444-2447,3006` | `render-frame-plan-recon-1.md §B` ("DEPTH-FRAGILE explicit orderings — pin, don't infer") |
| **TerrainDecal/Overlay after Terrain** | coplanar decals blended onto terrain depth; need terrain depth present | table `barrierAfter=Framebuffer`; `kFramePassOrder` positions 3<5,6 |
| **colorMask: terrain re-assert after shadow FALSE** | reorder strands colorMask FALSE → invisible scene | `gos_postprocess.cpp:1418/3501` (§2) |
| **markTerrainDrawn before post-process** | 4 post sub-passes bail on `!sceneHasTerrain_` | `GameOS/gameos/gos_postprocess_fbo.cpp:47` etc. (§2) |
| **compute → GL_COMMAND_BARRIER → indirect** | mis-order → zero/stale instances in MDI | `barrierAfter=Command/Framebuffer`; `render-frame-plan-recon-1.md §H.6` |
| **Terrain publishes dispatch-MVP/epoch before object phase** | object draw epoch-match else falls to live MVP | `gos_object_draw_mvp.h` |

These are currently **encoded only as `kFramePassOrder[]` array position + prose comments** — NOT as machine-checkable
hard constraints. SKELETON-1 should encode them as `forbids()`/edge rules.

---

## 5. RAW-RESOURCE-ACCESS SITES (dispatch-MVP snapshot)

`gos_terrain_indirect_getDispatchMvp16()` is PHASE-PRIVATE to terrain-coupled passes; enforced by
`scripts/check-object-mvp-currency.py`.

| Site | Sanctioned? | How |
|---|---|---|
| `gos_terrain_indirect.cpp/.h` | yes | defines snapshot + getter (FULL_ALLOW) |
| `gos_terrain_lod_chunk.cpp` | yes | terrain solid draw (FULL_ALLOW) |
| `gos_terrain_water_stream.cpp` | yes | pure water fast path (FULL_ALLOW) |
| `gos_object_draw_mvp.h` | yes | the ONLY sanctioned object-phase reader (epoch-checked accessor) |
| `gameos_graphics.cpp` | **MIXED** | raw read allowed ONLY on a line carrying `TERRAIN-PHASE-RAW-MVP-OK` tag; object-phase reads (e.g. `gos_SetupObjectShadows`) must use `gos_GetObjectDrawMVP()` |
| `debug_renderer.cpp` | yes | diagnostics dump |

**Object-phase raw reads remaining: NONE.** The OBJECT-SHADOW-MVP-CURRENCY-1 hole (a whole-file allowlist that let
`gos_SetupObjectShadows` read the raw snapshot blind) was closed by the per-line `MARKER_REQUIRED` mechanism
(`check-object-mvp-currency.py:46-49,80`). Static-prop/mech/vehicle/UI/editor draw all go through
`gos_GetObjectDrawMVP()`, which returns the snapshot only when `getDispatchMvpViewEpoch() == g_viewContentEpoch`,
else live MVP (`gos_object_draw_mvp.h:43-54`).

---

## 6. EXECUTOR BLOCKERS  *(the punchline — what must be true before FRAME-GRAPH-EXECUTOR-1 is safe)*

An executor REORDERS / OWNS passes. None of these hold today:

1. **Ambient state is NOT owned or declared.** `applyPipeline` owns ~5 of ~13 axes (§2). colorMask, viewport,
   scissor/stencil, tex-unit latches, clip-control/reverse-Z, VAO, and the `sceneHasTerrain_`/`markTerrainDrawn`
   latches are inherited or self-repaired by side effect. An executor that reorders passes strands colorMask FALSE
   (invisible scene) and kills 4 post passes (terrain-latch). **This is the dominant blocker.**
2. **Terrain solid is path-variable and capture exercises the path the contract under-models** (§3). Legacy MLR vs
   indirect bridge produce DIFFERENT GL state ownership and DIFFERENT MVP sources. Until unified (or both branches
   are first-class in the graph with the capture branch identified), graph validation lies.
3. **Pinned orderings are prose + array-position only** (§4), not machine constraints. shadow-before-geometry,
   R3-before-R7, decal-after-terrain, terrain-colorMask-reassert-after-shadow, markTerrainDrawn-before-post,
   compute→barrier→indirect — an executor free to reorder will violate these silently.
4. **`reads[]`/`writes[]` model GPU resources only**, not ambient GL state. The CONTRACT-3 order audit
   (`beginPass/endPass`, `render_contract.cpp:774-829`) checks reads-satisfied-by-prior-writes over RESOURCES but is
   blind to colorMask/viewport/latches — so it would PASS a frame that an executor reorder broke.

**Honest gap statement:** the contract tables are ~90% of the *resource-flow* catalog but ~0% of the *ambient-state*
catalog. An executor is blocked primarily on ambient state, not on resource edges. FRAME-GRAPH-SKELETON-1 (declaration
+ validation) is the right next step *precisely because* it can encode the ambient/ordering constraints as
declarative `forbids()` rules WITHOUT taking execution ownership — closing the gap incrementally and safely.

---

## 7. SKELETON HANDOFF  *(exact instructions for FRAME-GRAPH-SKELETON-1)*

**What to wrap:** `RenderCore::kRenderPassContracts[]` (`RenderPassContract.h:163-334`) — the existing 11-row table.
Do NOT introduce a second source of truth; `declarePass()` must READ this table, not duplicate it. Iterate it in
`kFramePassOrder[]` order (`:353-365`).

**Producer/consumer fields to populate** (from §1, derived not re-invented):
- `producer` = owner fn/file already in `ownerSubsystem` + the §1 citations.
- `consumers` = DERIVE from `writes[]` cross-referenced against later passes' `reads[]` over `kFramePassOrder[]`
  (edges are derived, not stored — matches the existing comment `:351-352`).

**`forbids()` rules to encode** (these are the executor blockers, declared not executed):
- Object passes (StaticPropOpaque, MechOpaque, VegetationCards, VFX, UI) `.forbids(rawDispatchMvpRead)` — mirror
  `check-object-mvp-currency.py`; they must use `gos_GetObjectDrawMVP()`.
- VFX `.forbids(objectIdWrite)` — mirror `check-vfx-no-objectid.sh` (`location=2`).
- Terrain `.forbids(reorderBeforeShadow)` and StaticProp/Mech `.forbids(runBeforeTerrainMvpPublish)`.
- Encode the pinned orderings (§4) as edge constraints: Shadow→geometry, R3-objects→TerrainSolid,
  Terrain→{Decal,Overlay}, Terrain.colorMaskReassert→after Shadow.colorMaskFalse,
  markTerrainDrawn→PostProcess sub-passes.

**Validations to add** (declaration-time / per-frame audit, NOT execution):
1. **reads-satisfied-by-prior-writes** over `kFramePassOrder[]` — already exists in spirit as CONTRACT-3
   (`beginPass`, `render_contract.cpp:774-788`); promote to a static graph check at declare time so it runs without
   the engine.
2. **forbidden raw access** — static-source check folding in `check-object-mvp-currency.py` + `check-vfx-no-objectid.sh`
   verdicts against the declared `forbids()`.
3. **stale-resource** — flag a `reads[]` whose only writer is later in `kFramePassOrder[]` (would be N-1 / stale);
   legitimate cases: Water reflection RT (temporal), ShadowDynamic pre-seed.
4. **ambient-state declaration (NEW, the real value-add)** — extend each row with an `ambientPre`/`ambientPost`
   descriptor (colorMask, viewport, drawBuffers, sceneHasTerrain latch) so the colorMask-reassert and terrain-latch
   contracts (§2) become *declared* and a future executor can diff them. This is the field SKELETON-1 should ADD that
   does not exist anywhere today.

Per-slice discipline: run `repo_query.py slice-preflight` against nifty HEAD before coding; all `file:line` here are
drift-prone — re-grep.
