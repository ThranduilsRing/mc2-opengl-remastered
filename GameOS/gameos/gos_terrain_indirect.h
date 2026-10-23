// GameOS/gameos/gos_terrain_indirect.h
//
// Indirect terrain SOLID-only PR1 — Stage 0 scaffolding.
//
// Architectural endpoint of the SOLID-arc CPU->GPU offload. SOLID-only PR1
// retires the per-frame Terrain::quadSetupTextures SOLID main-emit setup
// loop in favour of:
//   - Static dense TerrainQuadRecipe SSBO  (Stage 2; existing 144 B / 9-vec4
//     schema from gos_terrain_patch_stream.h:87 verbatim — no growth)
//   - Per-frame TerrainQuadThinRecord SSBO (Stage 3; existing 32 B M2 schema)
//   - Per-frame DrawArraysIndirectCommand  (Stage 3; one cmd for SOLID-PR1,
//     headroom for 16 future buckets)
// Detail (MC2_DRAWALPHA), overlay (gos_PushTerrainOverlay), and mine paths
// stay legacy in this slice. Detail/overlay/mine consolidation = follow-up.
//
// Pattern template: gos_terrain_water_stream.{h,cpp} (renderWater Stage 1+2+3
// shipped 2026-04-30); preflight-arming hazard analysis from advisor stop-the-
// line on plan v1.
//
// Plan: docs/superpowers/plans/2026-04-30-indirect-terrain-draw-plan.md
// Design: docs/superpowers/specs/2026-04-30-indirect-terrain-draw-design.md
//
// ---------------------------------------------------------------------------
// Tracy zone name reservations (consumed across stages — keep names verbatim)
// ---------------------------------------------------------------------------
//   Stage 1 zones (cost-split capture; gated under MC2_TERRAIN_COST_SPLIT=1):
//     "Terrain::SetupSolidBranch"          per-frame, around SOLID admit clusters
//     "Terrain::SetupDetailOverlayBranch"  per-frame, around DRAWALPHA / mine /
//                                          overlay clusters
//   Stage 2 zones:
//     "Terrain::IndirectRecipeBuild"       one-shot @ primeMissionTerrainCache
//     "Terrain::IndirectRecipeReset"       one-shot @ Terrain::destroy /
//                                          mission teardown
//   Stage 3 zones:
//     "Terrain::IndirectPreflight"         per-frame @ ComputePreflight()
//     "Terrain::ThinRecordPack"            per-frame @ packer entry
//     "Terrain::IndirectDraw"              per-frame @ glMultiDrawArraysIndirect
//   Aggregator (Stage 3, optional):
//     "Terrain::TotalCPU"                  per-frame @ outer-most caller scope

#pragma once

#include <array>
#include <cstdint>

// Forward-declare so callers of RecipeForVertexNum don't need to include
// gos_terrain_patch_stream.h just for the return type.
struct TerrainQuadRecipe;

namespace gos_terrain_indirect {

// ---------------------------------------------------------------------------
// Env-gate readers (boot-time once; cached in function-scope statics so
// subsequent calls are a single branch-predicted bool load).
// ---------------------------------------------------------------------------
//
// Stage 0..3 default OFF: only literal "1" turns the path on. Stage 4
// flip shipped (this commit): IsEnabled() defaults on (only literal
// "0" opts out); other gates stay default-off.
bool IsEnabled();              // MC2_TERRAIN_INDIRECT
bool IsParityCheckEnabled();   // MC2_TERRAIN_INDIRECT_PARITY_CHECK
bool IsTraceEnabled();         // MC2_TERRAIN_INDIRECT_TRACE — gates
                                // [TERRAIN_INDIRECT v1] event=... lifecycle prints
bool SolidWindowEnabled();        // MC2_TERRAIN_SOLID_NARROW — default-ON;
                                  // literal "0" => OFF = full-range HEAD path
                                  // (the safety escape hatch).  Approach A.
bool IsSolidWindowParityEnabled();// MC2_TERRAIN_SOLID_WINDOW_PARITY — default
                                  // OFF; "1" arms the catastrophic-axis probe.
bool IsCostSplitEnabled();     // MC2_TERRAIN_COST_SPLIT — gates Stage 1
                                // per-frame steady_clock accumulators in quad.cpp.
                                // When unset, the RAII timer scopes are zero-cost
                                // no-ops (single branch-predicted bool load each).

// ---------------------------------------------------------------------------
// Stage 1 cost-split accumulators — used by RAII timers in quad.cpp.
//
// Per-frame steady_clock nanosecond totals split between SOLID admit clusters
// (target of this slice) and DRAWALPHA detail/mine/overlay clusters (out of
// scope here). Reported via the existing 600-frame summary line, suppressed
// when MC2_TERRAIN_COST_SPLIT is unset to avoid all-zero noise.
//
// Per-quad ZoneScopedN() rejected for this measurement: 8 clusters x 14K
// quads x 60 fps would saturate Tracy's queue and ZoneScopedN overhead would
// become comparable to the work being measured. Per-frame summation matches
// slice 2b's mine-counter convention.
// ---------------------------------------------------------------------------
void      CostSplit_AddSolidNanos(long long n);
void      CostSplit_AddDetailOverlayNanos(long long n);
// PR2c Stage 0c — mine-specific accumulators. Independent of solid /
// detail-overlay buckets; same RollFrame cadence.
void      CostSplit_AddMineEnqueueNanos(long long n);
void      CostSplit_AddMineDrawNanos(long long n);
// PR2b Stage 0b — overlay-specific accumulator. Brackets ONLY the M2d
// fast-path block (CostSplitDetailOverlayScope brackets the legacy
// Shape-C fallback at quad.cpp:485-503 which fires ~0 times in tier1
// steady-state). Same RollFrame cadence.
void      CostSplit_AddOverlayNanos(long long n);
// 1A-alt Slice 0 — recon buckets for the unmeasured sub-tasks inside
// TerrainQuad::setupTextures. Path 2 onion-peel uses these to identify
// which sub-task to port to GPU compute first. Same RollFrame cadence.
//   WaterVertProj — water-elevation vertex projection block at
//     quad.cpp:833-1140 (4 per-vertex projectForTerrainAdmission calls
//     + per-vertex leastZ/mostZ reductions + waterHandle resolution +
//     addTriangleBulk water/waterDetail). Expected hot at wolfman zoom
//     on water-heavy missions (mc2_10).
//   Lighting — per-vertex lighting block at quad.cpp:1147-1771
//     (4 vertices × numTerrainLights × falloff + RGB accumulation +
//     lightRGB pack). Expected hot at wolfman zoom on lit missions.
//   RecipeCache — Shape-C recipe lookup + member assignments +
//     addTerrainTriangles at quad.cpp:781-818. Expected small post
//     Shape-C cache flip; instrumented to confirm.
void      CostSplit_AddWaterVertProjNanos(long long n);
void      CostSplit_AddRecipeCacheNanos(long long n);
// 1A-alt Slice 0 follow-up — close the "missing 8ms" gap between Tracy
// outer-zone (~11ms) and sum of named sub-buckets (~3.5ms).
//   SetupTotal     — brackets the entire TerrainQuad::setupTextures body.
//                    sum_setup_total - (recipe + water + lighting + cache_resident)
//                    = unmeasured residual inside setupTextures (function-entry
//                    preamble, isTerrainQuadVisible, dispatch, mine state gate).
//   CacheResident  — brackets the cache-fetch + residency check between
//                    cold-path tex loads and the recipe-cache RAII block
//                    (rowCol math + getTerrainFaceCacheEntry +
//                    ensureTerrainFaceCacheEntryResident).
//   tracy_zone - sum_setup_total = outer for-loop overhead + per-frame setup
//   (ComputePreflight, BeginFrameNarrow, AppendNarrowCandidate gate, etc.)
void      CostSplit_AddSetupTotalNanos(long long n);
void      CostSplit_AddCacheResidentNanos(long long n);
// 1A-alt Slice 0 follow-up #2 — VisibilityCheck wraps isTerrainQuadVisible(*this)
// + the invisible-quad-handle-reset branch. Prime suspect for the remaining
// ~3ms inside setup_total that's unaccounted for by recipe + water + lighting +
// cache_resident. At wolfman zoom ~14K quads are iterated and ~8K are visible
// per frame counter inference, so ~6K quads pay this cost without entering
// any other measured block.
// Call once per frame at the close of the per-quad setupTextures loop
// (terrain.cpp:1684 boundary). Internally gated on IsCostSplitEnabled() —
// safe to call unconditionally.
void      CostSplit_RollFrame();
long long CostSplit_GetSolidNanosTotal();
long long CostSplit_GetDetailOverlayNanosTotal();
long long CostSplit_GetMineEnqueueNanosTotal();
long long CostSplit_GetMineDrawNanosTotal();
long long CostSplit_GetOverlayNanosTotal();
long long CostSplit_GetWaterVertProjNanosTotal();
long long CostSplit_GetRecipeCacheNanosTotal();
long long CostSplit_GetSetupTotalNanosTotal();
long long CostSplit_GetCacheResidentNanosTotal();
int       CostSplit_GetFramesObserved();

// --- [LIGHT_COST_SPLIT v1] -------------------------------------------------
// Separate gate from MC2_TERRAIN_COST_SPLIT (that gate's chrono scopes are
// observer-effect-dominated and the terrain-CPU campaign is terminal --
// memory/cost_split_instrumentation_is_observer_effect_dominated.md).
// These buckets use __rdtsc() and a distinct env var so they can run on a
// legacy-object mission without the falsified terrain scopes attached.
// Three buckets isolate the three addLightDataStructure callsites the Tracy
// "addLightDataStructure scan" zone conflates:
//   C2-direct  : mclib/tgl.cpp  (!eligibleForGpuObjects legacy leaf)
//   C6-resubmit: mclib/tgl.cpp  (TG_Shape::ResubmitCachedLightData)
//   C5-peractor: mclib/tgl.cpp  (TG_Shape::GatherGpuObjectLightDataOnly)
bool IsLightCostSplitEnabled();              // MC2_LIGHT_COST_SPLIT
void LightCostSplit_AddC2DirectCycles(unsigned long long c);
void LightCostSplit_AddC6ResubmitCycles(unsigned long long c);
void LightCostSplit_AddC5PerActorCycles(unsigned long long c);
void LightCostSplit_AddC2DirectCall();
void LightCostSplit_AddC6ResubmitCall();
void LightCostSplit_AddC5PerActorCall();
void LightCostSplit_RollFrameAndMaybeEmit();  // call from CostSplit_RollFrame()

// ---------------------------------------------------------------------------
// N1 counters — units = per-quad (per cluster), NOT per-triangle.
//
// Each call increments by exactly ONE quad. Callers wrap a paired addTriangle
// admit cluster (e.g. quad.cpp:466-467 — two MC2_DRAWSOLID admits) and call
// the matching Add* helper ONCE per cluster, not once per addTriangle. Detail/
// overlay clusters bump the legacy_detail_overlay counter; SOLID clusters bump
// either legacy_solid_setup (un-armed legacy admit) or indirect_solid_packed
// (armed packer iteration).
//
// Without these counters, Stage 3 Gate B can pass on "renderer time went
// down" while completely missing the CPU-offload goal — see plan v2 N1.
// ---------------------------------------------------------------------------
void Counters_AddLegacySolidSetupQuad();      // un-armed legacy SOLID admit cluster
void Counters_AddIndirectSolidPackedQuad();   // armed indirect packer per packed quad
long long Counters_GetLegacySolidSetupQuads();
long long Counters_GetIndirectSolidPackedQuads();

// PR2c Stage 0c — mine-specific counters (independent of legacy_detail_overlay).
//   legacy_mine_enqueue_quads — incremented once per call to
//     enqueueTerrainMineState (mclib/quad.cpp:251). Drops to zero post Stage 2c
//     gate-off.
//   legacy_mine_draw_quads    — incremented once per call to TerrainQuad::drawMine
//     (mclib/quad.cpp:4240). Drops to zero post Stage 2c gate-off.
//   indirect_mine_drawn_cells — Stage 2c wires; placeholder here.
void      Counters_AddLegacyMineEnqueueQuad();
void      Counters_AddLegacyMineDrawQuad();
void      Counters_AddIndirectMineDrawnCells(long long n);
long long Counters_GetLegacyMineEnqueueQuads();
long long Counters_GetLegacyMineDrawQuads();
long long Counters_GetIndirectMineDrawnCells();

// PR2a Stage 0a — M2c detail-emit counter. Confidence guard for the
// upcoming Stage 1a delete: pre-delete this should be non-zero on
// water-interest tiles (~5800/frame on mc2_01 normal zoom per
// m2_thin_record_cpu_reduction_results.md). Post-Stage-1a it MUST be
// zero (the call site is deleted along with the M2c block at
// quad.cpp:2001-2070). Counter declaration STAYS post-delete per the
// "demote, don't delete" debug-instrumentation rule (worktree
// CLAUDE.md "Debug Instrumentation Rule"); body becomes orphan no-op.
void      Counters_AddM2cDetailEmitQuad();
long long Counters_GetM2cDetailEmitQuads();

// PR2b Stage 0b — overlay-specific counters.
//   legacy_m2d_overlay_emit_quads — incremented once per call to the M2d
//     overlay block at quad.cpp:2035-2083 (the
//     `if (useOverlayTexture && overlayHandle != 0xffffffff)`).
//     Drops to zero post Stage 3b legacy gate-off.
//   indirect_overlay_packed_quads — Stage 2b wires per-frame thin-record
//     packer; placeholder here.
//   gos_push_overlay_calls — incremented once per call to
//     gos_PushTerrainOverlay regardless of which producer (M2d fast-path
//     OR Shape-C fallback). Diagnostic probe: confirms whether the
//     overlay-producer pipeline is live anywhere on this mission. If
//     zero across tier1, PR2b reframes from indirect-draw to delete-slice.
void      Counters_AddM2dOverlayEmitQuad();
void      Counters_AddIndirectOverlayPackedQuad();
void      Counters_AddGosPushOverlayCall();
long long Counters_GetM2dOverlayEmitQuads();
long long Counters_GetIndirectOverlayPackedQuads();
long long Counters_GetGosPushOverlayCalls();

// #4 recon: Shape-C invisible quad counter.
// Always-on (not MC2_TERRAIN_COST_SPLIT-gated). Lifetime running total;
// divide by summary frames to get per-frame rate.
void      Counters_AddShapeCInvisibleQuad();
long long Counters_GetShapeCInvisibleQuads();

// PR2c Stage 0c — env gate readers.
//   IsMineEnabled()       — MC2_TERRAIN_INDIRECT_MINE (default OFF until Stage 4
//                           default-on flip after Stage 2c soak).
//   IsFrameMineArmed()    — Stage 2c will wire to a real preflight latch; Stage 0c
//                           stub returns false unconditionally so callers compile
//                           and the gate-off sites can be staged ahead of arming.
bool IsMineEnabled();
bool IsFrameMineArmed();

// PR2b Stage 0b — env gate readers.
//   IsOverlayEnabled()        — MC2_TERRAIN_INDIRECT_OVERLAY (DEFAULT-ON
//                               since the 60f2ef8 Stage-6 flip; only
//                               literal "0" opts out / reverts).
//   IsOverlayParityCheckEnabled() — MC2_TERRAIN_INDIRECT_OVERLAY_PARITY_CHECK.
//   IsFrameOverlayArmed()     — Stage 3b wires the real predicate; Stage 0b
//                               stub returns false so gate-off sites can be
//                               staged ahead of the live draw.
bool IsOverlayEnabled();
bool IsOverlayParityCheckEnabled();
bool IsFrameOverlayArmed();

// ---------------------------------------------------------------------------
// Native mine bake; legacy VBO/array names remain for source compatibility.
// Terrain mission setup/destruction resets the CPU vertices and texture readiness.
// MissionMap mine mutations mark the bake dirty; the next terrain-mine pass rebuilds
// six vertices per occupied cell. Shared GOS handles supply the two sprite textures.
// The GL object accessors return zero; no mine GL buffer or array is allocated.
void  ResetMineStaticVBO();
void  ResetMineTextureArray();
void  MarkMineDirty();
void  RebuildMineStaticVBOIfDirty();
void  BuildMineStaticVBO();
void  BuildMineTextureArray();
unsigned int GetMineStaticVBO_GL();
int          GetMineVertCount();
unsigned int GetMineTextureArrayGL();
bool         IsMineTextureArrayReady();

// Rebuild dirty CPU geometry and submit through the native overlay owner.
// Counts submitted cells; no mines is a successful zero-emit frame. Returns false
// when the native owner or texture handles are unavailable.
bool DrawMineStatic();

// ---------------------------------------------------------------------------
// Slice A — cement-overlay (decal) static-bake infrastructure.
//
// STRUCTURAL MIRROR of the PR2c mine static-bake above. Same lifetime
// discipline:
//   ResetDecalStaticVBO()         — Terrain::primeMissionTerrainCache +
//                                   Terrain::destroy (same chokepoints as
//                                   ResetMineStaticVBO). CPU-clears state,
//                                   keeps the GL_STATIC_DRAW buffer.
//   MarkDecalDirty()              — cement-mutation chokepoint hook. Wired at
//                                   bldng.cpp bridge-destroy setOverlay(
//                                   DAMAGED_BRIDGE) + Terrain::setOverlay
//                                   public wrapper (mirrors MarkMineDirty at
//                                   the setMine sites). Idempotent.
//   RebuildDecalStaticVBOIfDirty()— fires from DrawDecalStatic before draw
//                                   (mirror RebuildMineStaticVBOIfDirty).
//   BuildDecalStaticVBO()         — walks the map-immutable Shape-C terrain
//                                   face cache; for each alpha-cement quad,
//                                   reproduces M2d's 4-corner + per-uvMode
//                                   tri emit UNCONDITIONALLY (no pz cull).
//   GetDecalVertCount()           — int; 0 if mission has no cement overlay.
//
// IsFrameOverlayArmed() (declared above) is the env gate, DEFAULT-ON
// since the 60f2ef8 Stage-6 flip (only MC2_TERRAIN_INDIRECT_OVERLAY=0
// reverts). DrawDecalStatic is the Render.TerrainOverlaysStatic hook:
// lazy first-build + single bridge dispatch + decal_static_tris_drawn
// counter. Default (armed) => the static decal bake is the producer and
// the drawPass per-quad draw() loop is skipped. The =0 revert restores
// the M2d per-quad emit (code-proof fallback), bake inert.
void  ResetDecalStaticVBO();
void  MarkDecalDirty();
void  RebuildDecalStaticVBOIfDirty();
void  BuildDecalStaticVBO();
int          GetDecalVertCount();
// VEG-CEMENT-1: sub-cell hard-surface (cement/pad/road) coverage in [0,1] at a
// world XY, rasterised from the same face-cache walk as the decal bake (overlay
// alpha for alpha-cement tiles, 1.0 for pure-cement tiles). Lazy, once per mission.
void  InvalidateHardSurfaceField();
void  MarkDecalDirtyTile(long row, long col);   // MarkDecalDirty() that names the changed tile (incremental hard-surface update)
struct HardSurfaceDirty {
    bool all = false;                          // recomposite everything
    int count = 0;                             // else: this many world rects {x0, y0, x1, y1}
    std::array<std::array<float, 4>, 64> rects{};
};
HardSurfaceDirty TakeHardSurfaceDirty();
float HardSurfaceCoverage(float wx, float wy);
void  HardSurfaceStats(int* alpha, int* pure, int* noImage);
bool  DrawDecalStatic();

// Slice A counters.
//   decal_static_tris_drawn      — substitutive analog of
//                                  indirect_mine_drawn_cells; nonzero when
//                                  the static bake draws (gos_push_overlay_calls
//                                  -> 0 on the same armed frame).
//   legacy_drawalpha_detail_quads— A2 dead-confirmation. Incremented at the 4
//                                  post-return DRAWALPHA detail addVertices
//                                  sites (quad.cpp ~2656/2799/2971/3112). The
//                                  existing legacy_detail_overlay_quads
//                                  conflates mine/overlay/detail; this one is
//                                  DRAWALPHA-detail-only. Expected == 0.
void      Counters_AddDecalStaticTrisDrawn(long long n);
long long Counters_GetDecalStaticTrisDrawn();
void      Counters_AddLegacyDrawAlphaDetailQuad();
long long Counters_GetLegacyDrawAlphaDetailQuads();

// ---------------------------------------------------------------------------
// Stage 2: CPU recipe cache build / lifecycle / per-entry invalidation.
//
// Dense recipe indexing convention (Option A):
//   vn (vertexNum) ∈ [0, mapSide²)  → g_denseRecipes[vn] is the slot.
//   vn == -1 (blankVertex)          → no recipe; lookup returns nullptr.
//   vn ≥ mapSide²                   → out-of-range; lookup returns nullptr.
// All references (parity-check, GLSL shader-side indexing through
// TerrainQuadThinRecord.recipeIdx in Stage 3) consume vn DIRECTLY.
// There is no +1 offset.
// ---------------------------------------------------------------------------

// Recipe-build / lifecycle
void BuildDenseRecipe();           // called from primeMissionTerrainCache
void ResetDenseRecipe();           // called from Terrain::destroy + start of Build
bool IsDenseRecipeReady();
const ::TerrainQuadRecipe* RecipeForVertexNum(int32_t vn);  // nullptr for vn<0 or out-of-range
void InvalidateRecipeForVertexNum(int32_t vn);             // precise; CPU recompute + mark dirty
void InvalidateAllRecipes();                               // whole-map; rebuild all slots + mark dirty
void RebuildCementAtlas();                                 // (re)build GPU cement atlas from textures[]
void MarkCementAtlasDirty();                                // schedule a deferred cement-atlas rebuild (debounced)
void RebuildCementAtlasIfDirty();                           // frame-start safe point: rebuild if marked dirty (no mid-frame readback)

// Stage 2 parity body — walks live quadList, byte-compares recipe against
// per-quad legacy-equivalent computation. Returns quads_checked count.
int  ParityCompareRecipeFrame();

// ---------------------------------------------------------------------------
// Native frame readiness and the shared camera snapshot.
void BeginFrame();              // reset armed flag; call unconditionally once per frame
// Approach A: camera-windowed solid dispatch collector.  Structural twin of
// WaterStream::BeginFrameNarrow / AppendNarrowCandidate.  BeginFrameSolidWindow
// resets the per-frame recipe-index window (no-op when MC2_TERRAIN_SOLID_NARROW
// =0); AppendSolidWindowCandidate is called from terrain.cpp's existing
// setupTextures loop with vn0 = quad.vertices[0]->vertexNum (caller guarantees
// the loose superset predicate — see terrain.cpp).  NO new walk.
void BeginFrameSolidWindow();
void AppendSolidWindowCandidate(int32_t vn0);
// Phase 8b A/B: read-only snapshot of this frame's legacy solid-window staging
// (the vns slimReduce appended). Returns data ptr + count; ptr null when empty.
// CPU diagnostic view; valid until the next BeginFrameSolidWindow call.
const uint32_t* SolidWindowStagingData(uint32_t* outCount);
bool ComputePreflight();
// Capture the CPU camera snapshot used by the native terrain and water feeds.
void CaptureFrameInputs();
bool IsFrameSolidArmed();
// Process-sticky cousin of IsFrameSolidArmed(); true once preflight has
// armed at least one frame this process. Consumed by VisualDiff to gate
// the visual-diff capture on the engine's "intro pan complete" signal --
// the per-frame arm is reset by gosRenderer::endFrame() before the
// VisualDiff post-PP hook fires, so a sticky-once cousin is needed.
bool WasEverFrameSolidArmed();
// Single-source water fast-path gate: true iff all conditions that allow
// renderWater() to skip the legacy loop are satisfied. Defined in
// terrain.cpp (the only TU that sees WaterStream + gpu_driven symbols).
// quad.cpp calls this via the opaque bool return; no new includes needed.
bool WaterFastPathOwnsArmedDraw();

// ---------------------------------------------------------------------------
// Parity-check printer + 600-frame summary cadence.
//
// Stage 0 lands the printer skeleton; Stage 2 plugs in the actual recipe-
// content comparisons. Throttled to 16 mismatch prints per frame to keep
// logs bounded when an early-frame cascade fires.
//
// Schema (grep-friendly):
//   [TERRAIN_INDIRECT_PARITY v1] event=mismatch frame=N quad=Q layer=<name>
//                                tri=T vert=V field=<name> legacy=0xHEX fast=0xHEX
//   [TERRAIN_INDIRECT_PARITY v1] event=summary frames=N quads_checked=Q
//                                total_mismatches=K
//                                legacy_solid_setup_quads=N
//                                indirect_solid_packed_quads=N
//                                legacy_detail_overlay_quads=N
// ---------------------------------------------------------------------------
void ParityPrintMismatch(int frame, int quad, const char* layer, int tri,
                         int vert, const char* field,
                         uint32_t legacy, uint32_t fast);

// Call once per frame at the close of the per-quad setupTextures loop
// (terrain.cpp:1684 boundary). Resets the per-frame mismatch throttle and
// emits the summary line every 600 frames. Internally a no-op when neither
// IsParityCheckEnabled nor any counter has been bumped — cheap to call
// unconditionally.
void ParityFrameTick(int quadsCheckedThisFrame);

}  // namespace gos_terrain_indirect
