// Indirect terrain solid path: env-gate readers, the N1 counters and the cost-split roll-ups. The recipe, atlas, cement,
// pack, dispatch and mine/decal stages live in gos_terrain_indirect_{diag,atlas,cement,pack,dispatch,mine_decal}.cpp;
// the public API is gos_terrain_indirect.h and what the pieces share is gos_terrain_indirect_internal.h.


#include "gos_terrain_indirect_internal.h"
#include "mc2fmt.h"



// Negative control: when MC2_LOAD_RACE_DELAY_ATLAS is set, BuildColormapAtlas()
// defers the real GL upload until scenarioTime >= 30s and ComputePreflight()
// pumps it each frame. This makes the atlas GENUINELY absent for ~30s so the
// reviewer can confirm the atlas/texel probes (a)/(b) actually FIRE when the
// atlas is the cause.
extern const bool s_loadRaceDelayAtlas = mc2::gate::get<"MC2_LOAD_RACE_DELAY_ATLAS">();

namespace gos_terrain_indirect_detail {}
using namespace gos_terrain_indirect_detail;

namespace gos_terrain_indirect {

// ---------------------------------------------------------------------------
// Env-gate readers
// ---------------------------------------------------------------------------

bool IsEnabled() {
    // Default-on (Stage C of plan v2.3). Literal "0" opts out; absent or
    // anything else = on. Default-on flip arrived bundled into e22fa3a
    // 2026-05-01; was previously deferred pending the gosFX/MLR white-
    // saturation bug (resolved 2026-05-01 in commit e9bf756 by
    // glDisable(GL_BLEND) before the postprocess composite). Verified
    // clean on tier1 5/5 with both MC2_TERRAIN_INDIRECT=0 (legacy
    // regression) and default-on configs 2026-05-01 sess 3 — no
    // destroys delta, FPS parity within noise.
    static const bool s = mc2::gate::get<"MC2_TERRAIN_INDIRECT">();
    return s;
}

bool IsParityCheckEnabled() {
    static const bool s = mc2::gate::get<"MC2_TERRAIN_INDIRECT_PARITY_CHECK">();
    return s;
}

bool IsTraceEnabled() {
    static const bool s = mc2::gate::get<"MC2_TERRAIN_INDIRECT_TRACE">();
    return s;
}

// Camera-windowed solid dispatch gate.  Default-ON (Approach A re-introduction
// after 08bd3b2 hard-wired the dispatch to the FULL recipe range every frame,
// camera-independent -> Terrain::IndirectDraw ~7.8ms zoomed-out on big maps).
// Same idiom as IsEnabled() above: literal "0" => OFF == current HEAD
// full-range behavior (the safety escape hatch); absent or anything else => ON.
bool SolidWindowEnabled() {
    static const bool s = mc2::gate::get<"MC2_TERRAIN_SOLID_NARROW">();
    return s;
}

// Parity-probe gate for the windowed dispatch (catastrophic-axis: a visible
// quad dropped from the window).  Default-OFF; literal "1" arms it.  When off
// the probe path does ZERO per-frame work (no ref build, no assert).
bool IsSolidWindowParityEnabled() {
    static const bool s = mc2::gate::get<"MC2_TERRAIN_SOLID_WINDOW_PARITY">();
    return s;
}

bool IsCostSplitEnabled() {
    static const bool s = mc2::gate::get<"MC2_TERRAIN_COST_SPLIT">();
    return s;
}

}  // namespace gos_terrain_indirect

// ---------------------------------------------------------------------------
// N1 counter storage (private to this TU; cross-TU access via public Add/Get)
// ---------------------------------------------------------------------------
namespace {
long long s_legacy_solid_setup_quads     = 0;
long long s_indirect_solid_packed_quads  = 0;
// PR2c Stage 0c — independent mine counters.
long long s_legacy_mine_enqueue_quads    = 0;
long long s_legacy_mine_draw_quads       = 0;
long long s_indirect_mine_drawn_cells    = 0;
// PR2a Stage 0a — M2c detail-emit counter (drops to zero post Stage 1a).
long long s_m2c_detail_emit_quads        = 0;
// PR2b Stage 0b — overlay counters (drop to zero post Stage 3b gate-off).
long long s_m2d_overlay_emit_quads       = 0;
long long s_indirect_overlay_packed_quads = 0;
long long s_gos_push_overlay_calls       = 0;  // probe at producer body
// Slice A — cement-overlay static-bake counters (mirror s_indirect_mine_drawn_cells
// + the legacy_detail_overlay split rationale). decal_static_tris_drawn is the
// substitutive analog of indirect_mine_drawn_cells; legacy_drawalpha_detail_quads
// is the A2 dead-confirmation counter (the existing legacy_detail_overlay_quads
// conflates mine/overlay/detail — this one is DRAWALPHA-detail-only).
long long s_decal_static_tris_drawn      = 0;
// #4 recon: count Shape-C quads that fail isTerrainQuadVisible each frame.
// Always-on (not gated on MC2_TERRAIN_COST_SPLIT). Running lifetime total;
// divide by summary frames to get per-frame rate.
long long s_shape_c_invisible_quads      = 0;
long long s_legacy_drawalpha_detail_quads = 0;
}  // namespace

namespace gos_terrain_indirect {

void Counters_AddLegacySolidSetupQuad()      { ++s_legacy_solid_setup_quads; }
void Counters_AddIndirectSolidPackedQuad()   { ++s_indirect_solid_packed_quads; }
void Counters_AddLegacyMineEnqueueQuad()     { ++s_legacy_mine_enqueue_quads; }
void Counters_AddLegacyMineDrawQuad()        { ++s_legacy_mine_draw_quads; }
void Counters_AddIndirectMineDrawnCells(long long n) { s_indirect_mine_drawn_cells += n; }

long long Counters_GetLegacySolidSetupQuads()    { return s_legacy_solid_setup_quads; }
long long Counters_GetIndirectSolidPackedQuads() { return s_indirect_solid_packed_quads; }
long long Counters_GetLegacyMineEnqueueQuads()   { return s_legacy_mine_enqueue_quads; }
long long Counters_GetLegacyMineDrawQuads()      { return s_legacy_mine_draw_quads; }
long long Counters_GetIndirectMineDrawnCells()   { return s_indirect_mine_drawn_cells; }
// PR2a Stage 0a — M2c detail-emit counter.
void      Counters_AddM2cDetailEmitQuad()        { ++s_m2c_detail_emit_quads; }
long long Counters_GetM2cDetailEmitQuads()       { return s_m2c_detail_emit_quads; }
// PR2b Stage 0b — overlay counters.
void      Counters_AddM2dOverlayEmitQuad()       { ++s_m2d_overlay_emit_quads; }
void      Counters_AddIndirectOverlayPackedQuad(){ ++s_indirect_overlay_packed_quads; }
void      Counters_AddGosPushOverlayCall()       { ++s_gos_push_overlay_calls; }
long long Counters_GetM2dOverlayEmitQuads()      { return s_m2d_overlay_emit_quads; }
long long Counters_GetIndirectOverlayPackedQuads(){ return s_indirect_overlay_packed_quads; }
void      Counters_AddShapeCInvisibleQuad()      { ++s_shape_c_invisible_quads; }
long long Counters_GetShapeCInvisibleQuads()     { return s_shape_c_invisible_quads; }
long long Counters_GetGosPushOverlayCalls()      { return s_gos_push_overlay_calls; }
// Slice A — cement-overlay static-bake counters.
void      Counters_AddDecalStaticTrisDrawn(long long n) { s_decal_static_tris_drawn += n; }
long long Counters_GetDecalStaticTrisDrawn()     { return s_decal_static_tris_drawn; }
void      Counters_AddLegacyDrawAlphaDetailQuad(){ ++s_legacy_drawalpha_detail_quads; }
long long Counters_GetLegacyDrawAlphaDetailQuads(){ return s_legacy_drawalpha_detail_quads; }

// PR2c — Stage 4 default-on flip 2026-05-08.
// Static-bake mine path retires ~932 µs/frame (mc2_01 baseline:
// enqueueTerrainMineState ~54 µs + drawMine ~886 µs → 0). Tier1 5/5 PASS
// with arming verified across PR2c Stages 0c/1c/2c. Literal "0" opts out
// for bisection; any other value (including unset) opts in.
bool IsMineEnabled() {
    static const bool s = mc2::gate::get<"MC2_TERRAIN_INDIRECT_MINE">();
    return s;
}

// PR2b Stage 0b — env-gate readers.
// 2026-05-17 Stage-6 default-ON flip (drawPass-retirement campaign endpoint):
// the Slice-A decal static-bake + Slice-B drawPass-skip are substitutively
// proven (drawPass self-time ~1.7ms -> ~20us armed, total frame dropped) and
// raster-sheet-fixed + user-visual-confirmed. Mirror IsMineEnabled() EXACTLY:
// literal "0" opts out (bisection / code-proof fallback escape hatch), any
// other value INCLUDING UNSET opts in. Stock play now gets the retirement;
// `MC2_TERRAIN_INDIRECT_OVERLAY=0` is the built-in revert.
bool IsOverlayEnabled() {
    static const bool s = mc2::gate::get<"MC2_TERRAIN_INDIRECT_OVERLAY">();
    return s;
}

bool IsOverlayParityCheckEnabled() {
    static const bool s = mc2::gate::get<"MC2_TERRAIN_INDIRECT_OVERLAY_PARITY_CHECK">();
    return s;
}

// MC2_THIN_CANARY=1 enables the Probe-6 thin-record canary readback+compare
// (diagnostic for the corrupt-recipeIdx grey-triangle bug). Default OFF — it is a
// per-frame ~80KB glGetBufferSubData readback + CPU compare and must not run in
// production. Kill-switch restores it for diagnosis.
extern const bool s_thinCanaryEnabled = mc2::gate::get<"MC2_THIN_CANARY">();

// Slice A — wire the real predicate (replaces the Stage 0b/1b/2b always-false
// stub). Mirrors IsFrameMineArmed() == IsMineEnabled() EXACTLY: arming is
// purely the env gate. The cement-overlay static bake (DrawDecalStatic) lazy-
// builds on first armed draw and handles the empty/not-yet-built case
// internally, so there is no readiness gate here (same bootstrap-cycle
// reasoning as the IsFrameMineArmed comment further down).
//
// MC2_TERRAIN_INDIRECT_OVERLAY default ON since the 2026-05-17 Stage-6 flip
// (IsOverlayEnabled: only literal "0" opts out): unset => IsFrameOverlay
// Armed()==true => Slice-A decal static-bake is the producer and Slice-B
// skips the drawPass per-quad loop. `MC2_TERRAIN_INDIRECT_OVERLAY=0` reverts
// to the legacy M2d per-quad emit (the code-proof fallback).
bool IsFrameOverlayArmed() { return IsOverlayEnabled(); }

// IsFrameMineArmed is defined further down (after Stage 1c's
// g_mineTextureArrayReady storage). Forward decl is in the header.

}  // namespace gos_terrain_indirect

// ---------------------------------------------------------------------------
// Stage 1 cost-split accumulators
// ---------------------------------------------------------------------------
namespace {
long long s_solidBranchNanosThisFrame   = 0;
long long s_detailOverlayNanosThisFrame = 0;
long long s_mineEnqueueNanosThisFrame   = 0;  // PR2c Stage 0c
long long s_mineDrawNanosThisFrame      = 0;  // PR2c Stage 0c
long long s_overlayNanosThisFrame       = 0;  // PR2b Stage 0b
long long s_waterVertProjNanosThisFrame = 0;  // 1A-alt Slice 0
long long s_recipeCacheNanosThisFrame   = 0;  // 1A-alt Slice 0
long long s_setupTotalNanosThisFrame    = 0;  // 1A-alt Slice 0 follow-up
long long s_cacheResidentNanosThisFrame = 0;  // 1A-alt Slice 0 follow-up
long long s_solidBranchNanosTotal       = 0;
long long s_detailOverlayNanosTotal     = 0;
long long s_mineEnqueueNanosTotal       = 0;  // PR2c Stage 0c
long long s_mineDrawNanosTotal          = 0;  // PR2c Stage 0c
long long s_overlayNanosTotal           = 0;  // PR2b Stage 0b
long long s_waterVertProjNanosTotal     = 0;  // 1A-alt Slice 0
long long s_recipeCacheNanosTotal       = 0;  // 1A-alt Slice 0
long long s_setupTotalNanosTotal        = 0;  // 1A-alt Slice 0 follow-up
long long s_cacheResidentNanosTotal     = 0;  // 1A-alt Slice 0 follow-up
int       s_costSplitFramesObserved     = 0;
}  // namespace

// ---------------------------------------------------------------------------
// [LIGHT_COST_SPLIT v1] -- RDTSC, distinct gate. See header rationale.
// ---------------------------------------------------------------------------
namespace {
    bool                g_lcsInit   = false;
    bool                g_lcsOn     = false;
    unsigned long long  g_lcsC2Cyc  = 0, g_lcsC6Cyc = 0, g_lcsC5Cyc = 0;
    unsigned long long  g_lcsC2Call = 0, g_lcsC6Call = 0, g_lcsC5Call = 0;
    unsigned long long  g_lcsFrames = 0;
}  // namespace

namespace gos_terrain_indirect {

bool IsLightCostSplitEnabled() {
    if (!g_lcsInit) {                       // cache once -- getenv per-call is slow
        g_lcsOn   = mc2::gate::get<"MC2_LIGHT_COST_SPLIT">();
        g_lcsInit = true;
    }
    return g_lcsOn;
}

void LightCostSplit_AddC2DirectCycles(unsigned long long c)  { g_lcsC2Cyc  += c; }
void LightCostSplit_AddC6ResubmitCycles(unsigned long long c){ g_lcsC6Cyc  += c; }
void LightCostSplit_AddC5PerActorCycles(unsigned long long c){ g_lcsC5Cyc  += c; }
void LightCostSplit_AddC2DirectCall()                        { ++g_lcsC2Call; }
void LightCostSplit_AddC6ResubmitCall()                      { ++g_lcsC6Call; }
void LightCostSplit_AddC5PerActorCall()                      { ++g_lcsC5Call; }

void LightCostSplit_RollFrameAndMaybeEmit() {
    if (!IsLightCostSplitEnabled()) return;
    ++g_lcsFrames;
    if (g_lcsFrames % 600ULL != 0ULL) return;
    const double f = static_cast<double>(600.0);
    mc2::fmt::print_err("[LIGHT_COST_SPLIT v1] event=summary frames=600 c2_cyc_per_frame={:.0f} c2_calls_per_frame={:.1f} c6_cyc_per_frame={:.0f} c6_calls_per_frame={:.1f} c5_cyc_per_frame={:.0f} c5_calls_per_frame={:.1f}\n", static_cast<double>(g_lcsC2Cyc)/f, static_cast<double>(g_lcsC2Call)/f, static_cast<double>(g_lcsC6Cyc)/f, static_cast<double>(g_lcsC6Call)/f, static_cast<double>(g_lcsC5Cyc)/f, static_cast<double>(g_lcsC5Call)/f);
    g_lcsC2Cyc=g_lcsC6Cyc=g_lcsC5Cyc=0;
    g_lcsC2Call=g_lcsC6Call=g_lcsC5Call=0;
}

}  // namespace gos_terrain_indirect

namespace gos_terrain_indirect {

void CostSplit_AddSolidNanos(long long n)         { s_solidBranchNanosThisFrame  += n; }
void CostSplit_AddDetailOverlayNanos(long long n) { s_detailOverlayNanosThisFrame += n; }
void CostSplit_AddMineEnqueueNanos(long long n)   { s_mineEnqueueNanosThisFrame  += n; }
void CostSplit_AddMineDrawNanos(long long n)      { s_mineDrawNanosThisFrame     += n; }
void CostSplit_AddOverlayNanos(long long n)       { s_overlayNanosThisFrame      += n; }
void CostSplit_AddWaterVertProjNanos(long long n) { s_waterVertProjNanosThisFrame += n; }
void CostSplit_AddRecipeCacheNanos(long long n)   { s_recipeCacheNanosThisFrame   += n; }
void CostSplit_AddSetupTotalNanos(long long n)    { s_setupTotalNanosThisFrame   += n; }
void CostSplit_AddCacheResidentNanos(long long n) { s_cacheResidentNanosThisFrame += n; }

void CostSplit_RollFrame() {
    LightCostSplit_RollFrameAndMaybeEmit();   // [LIGHT_COST_SPLIT v1] -- MUST be
                                              // above the next line; self-gates.
    if (!IsCostSplitEnabled()) return;        // existing MC2_TERRAIN_COST_SPLIT gate
    s_solidBranchNanosTotal       += s_solidBranchNanosThisFrame;
    s_detailOverlayNanosTotal     += s_detailOverlayNanosThisFrame;
    s_mineEnqueueNanosTotal       += s_mineEnqueueNanosThisFrame;
    s_mineDrawNanosTotal          += s_mineDrawNanosThisFrame;
    s_overlayNanosTotal           += s_overlayNanosThisFrame;
    s_waterVertProjNanosTotal     += s_waterVertProjNanosThisFrame;
    s_recipeCacheNanosTotal       += s_recipeCacheNanosThisFrame;
    s_setupTotalNanosTotal        += s_setupTotalNanosThisFrame;
    s_cacheResidentNanosTotal     += s_cacheResidentNanosThisFrame;
    ++s_costSplitFramesObserved;
    s_solidBranchNanosThisFrame    = 0;
    s_detailOverlayNanosThisFrame  = 0;
    s_mineEnqueueNanosThisFrame    = 0;
    s_mineDrawNanosThisFrame       = 0;
    s_overlayNanosThisFrame        = 0;
    s_waterVertProjNanosThisFrame  = 0;
    s_recipeCacheNanosThisFrame    = 0;
    s_setupTotalNanosThisFrame     = 0;
    s_cacheResidentNanosThisFrame  = 0;
}

long long CostSplit_GetSolidNanosTotal()           { return s_solidBranchNanosTotal; }
long long CostSplit_GetDetailOverlayNanosTotal()   { return s_detailOverlayNanosTotal; }
long long CostSplit_GetMineEnqueueNanosTotal()     { return s_mineEnqueueNanosTotal; }
long long CostSplit_GetMineDrawNanosTotal()        { return s_mineDrawNanosTotal; }
long long CostSplit_GetOverlayNanosTotal()         { return s_overlayNanosTotal; }
long long CostSplit_GetWaterVertProjNanosTotal()   { return s_waterVertProjNanosTotal; }
long long CostSplit_GetRecipeCacheNanosTotal()     { return s_recipeCacheNanosTotal; }
long long CostSplit_GetSetupTotalNanosTotal()      { return s_setupTotalNanosTotal; }
long long CostSplit_GetCacheResidentNanosTotal()   { return s_cacheResidentNanosTotal; }
int       CostSplit_GetFramesObserved()            { return s_costSplitFramesObserved; }

}  // namespace gos_terrain_indirect
