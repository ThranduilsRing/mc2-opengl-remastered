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
// inverts IsEnabled() to default-on (only literal "0" opts out); other
// gates stay default-off.
bool IsEnabled();              // MC2_TERRAIN_INDIRECT
bool IsParityCheckEnabled();   // MC2_TERRAIN_INDIRECT_PARITY_CHECK
bool IsTraceEnabled();         // MC2_TERRAIN_INDIRECT_TRACE — gates
                                // [TERRAIN_INDIRECT v1] event=... lifecycle prints
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
// Call once per frame at the close of the per-quad setupTextures loop
// (terrain.cpp:1684 boundary). Internally gated on IsCostSplitEnabled() —
// safe to call unconditionally.
void      CostSplit_RollFrame();
long long CostSplit_GetSolidNanosTotal();
long long CostSplit_GetDetailOverlayNanosTotal();
int       CostSplit_GetFramesObserved();

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
void Counters_AddLegacyDetailOverlayQuad();   // legacy DRAWALPHA / detail / mine /
                                              // overlay cluster (passive — never gated)

long long Counters_GetLegacySolidSetupQuads();
long long Counters_GetIndirectSolidPackedQuads();
long long Counters_GetLegacyDetailOverlayQuads();

// ---------------------------------------------------------------------------
// Stage 2: dense recipe SSBO build / lifecycle / per-entry invalidation.
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

// Internal helper used by Stage 3's preflight too
void FlushDirtyRecipeSlotsToGPU();  // glBufferSubData per dirty slot

// Stage 2 parity body — walks live quadList, byte-compares recipe against
// per-quad legacy-equivalent computation. Returns quads_checked count.
int  ParityCompareRecipeFrame();

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
