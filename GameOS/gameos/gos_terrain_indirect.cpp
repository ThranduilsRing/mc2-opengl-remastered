// GameOS/gameos/gos_terrain_indirect.cpp
//
// Indirect terrain SOLID-only PR1 — Stage 0 + Stage 2 implementation.
//
// See gos_terrain_indirect.h for the slice overview, the Tracy zone name
// reservations, and the public API contract (counter units = per-quad,
// parity-printer schema, env-gate semantics).
//
// Stage 0: env-gate readers, N1 counters, parity-printer skeleton.
// Stage 2: dense TerrainQuadRecipe SSBO build/reset/invalidate/flush,
//          parity body comparing recipe against live quadList values.
// Stage 3 wires the per-frame thin-record packer, indirect-command builder,
// preflight-arming, and bridge entry.

#include "gos_terrain_indirect.h"

// ROAD-PBR-FAILSOFT-1 (defined in gameos_graphics.cpp)
extern bool gos_TerrainRoadMaterialReady(int matId);
#include "gos_gpu_sync.h"               // GPU-SYNC-CONTRACT typed barrier helper
#include "gos_terrain_patch_stream.h"  // TerrainQuadRecipe
#include "gpu_driven_common.h"         // gpu_driven::IsTerrainSolidEnabled
#include "gos_terrain_lighting.h"      // gos_terrain_lighting::GetOutputSsbo()
#include "gos_static_prop_killswitch.h" // gos_GetTerrainMVPMat4()
#include "gos_postprocess.h"            // WATER-TERRAIN-REFLECTION-1: reflection FBO
#include "../../RenderCore/terrain_path_telemetry.h"  // TERRAIN-PATH-TELEMETRY-1
#include "../../RenderCore/RenderResourceRegistry.h"   // REGISTRY-TERRAIN-SSBO-1: recipe/thin/cement/mask ids
// WATER-TERRAIN-REFLECTION-1: install a mirror MVP into the terrain_mvp_ cache
// (defined __stdcall in gameos_graphics.cpp; same cache gos_GetTerrainMVPMat4 reads).
void __stdcall gos_SetTerrainMVP(const float* matrix16);

// [MVP_EARLY v1] MVP-PUBLISH-EARLY-HOIST proof counters. Both are global-scope
// (defined in gameos_graphics.cpp), so the externs MUST live OUTSIDE namespace
// gos_terrain_indirect or the linker mangles them as namespace-scoped symbols.
extern long g_mvpDiagFrame;
extern long g_viewContentEpoch;   // VIEW-EPOCH-DEDUPE-1 semantic view-content epoch
extern long g_mvpEarlyPublishSeq;

#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>  // memcpy
#include <cmath>    // std::abs (clipPos ULP-bounded parity comparator)
#include <chrono>   // MC2_RING_TRACE wait-time measurement (probe-only)
#include <ctime>    // RING_SINK timestamp on probe-sink open

#include <intrin.h>   // __rdtsc -- [LIGHT_COST_SPLIT v1]

#include <GL/glew.h>

// MC2 types — resolve relative path from GameOS/gameos/
#include "../../mclib/terrain.h"
#include "../../mclib/quad.h"
#include "../../mclib/vertex.h"
#include "../../mclib/mapdata.h"
#include "../../mclib/move.h"       // PR2c Stage 1c — MissionMap, GameMap, MAPCELL_DIM
#include "../../mclib/terrtxm.h"    // TERRAIN_TXM_SIZE (extern int)
#include "../../mclib/txmmgr.h"     // MC_MAXTEXTURES (cement node-index space)
#include "gos_terrain_bridge.h"     // gos_terrain_bridge_glTextureForGosHandle (cement readback)
#include "../../RenderCore/KtxLoader.h"  // COLORMAP-BC7-KTX2-1: ktxLoadRgba8 for BC7 atlas upload
#include "gos_terrain_arm_logic.h"        // shared GPU-terrain arm predicate (editor↔game)
#include "gos_terrain_lod_chunk.h"        // Step 5c: push cement words to the LOD chunk path

#include "../gameos/gos_profiler.h"
#include "mc2_hitch_trace.h"

// CEMENT_DIAG: file-scope extern for the global mission name buffer
// (defined in code/mechcmd2.cpp:180 and code/logmain.cpp:88 as
// `char missionName[1024]`).  Used by BuildCementCatalogAtlas summary line.
extern char missionName[];

// [DEPTH_TRANSITION v1] CPU-water REAL screen-z sample - DEFINED at global
// scope in mclib/quad.cpp (writer) and reset in mclib/terrain.cpp. Declared
// here at GLOBAL file scope (NOT block-scope inside namespace
// gos_terrain_indirect) so the dump's unqualified use binds the global
// ::g_cpuWaterProbe* (a block-scope extern inside the namespace would bind
// gos_terrain_indirect:: and not link - C++ [basic.link] innermost-ns rule).
extern float              g_cpuWaterProbeZ;
extern bool               g_cpuWaterProbeAny;
extern unsigned long long g_cpuWaterProbeStamp;

namespace gos_terrain_indirect {

// ---------------------------------------------------------------------------
// Env-gate readers
// ---------------------------------------------------------------------------

bool IsEnabled() {
    static const bool s = []() {
        const char* v = getenv("MC2_TERRAIN_INDIRECT");
        // Default-on (Stage C of plan v2.3). Literal "0" opts out; absent or
        // anything else = on. Default-on flip arrived bundled into e22fa3a
        // 2026-05-01; was previously deferred pending the gosFX/MLR white-
        // saturation bug (resolved 2026-05-01 in commit e9bf756 by
        // glDisable(GL_BLEND) before the postprocess composite). Verified
        // clean on tier1 5/5 with both MC2_TERRAIN_INDIRECT=0 (legacy
        // regression) and default-on configs 2026-05-01 sess 3 — no
        // destroys delta, FPS parity within noise.
        if (v && v[0] == '0' && v[1] == '\0') return false;
        return true;
    }();
    return s;
}

bool IsParityCheckEnabled() {
    static const bool s = []() {
        const char* v = getenv("MC2_TERRAIN_INDIRECT_PARITY_CHECK");
        return v && v[0] == '1' && v[1] == '\0';
    }();
    return s;
}

bool IsTraceEnabled() {
    static const bool s = []() {
        const char* v = getenv("MC2_TERRAIN_INDIRECT_TRACE");
        return v && v[0] == '1' && v[1] == '\0';
    }();
    return s;
}

// Camera-windowed solid dispatch gate.  Default-ON (Approach A re-introduction
// after 08bd3b2 hard-wired the dispatch to the FULL recipe range every frame,
// camera-independent -> Terrain::IndirectDraw ~7.8ms zoomed-out on big maps).
// Same idiom as IsEnabled() above: literal "0" => OFF == current HEAD
// full-range behavior (the safety escape hatch); absent or anything else => ON.
bool SolidWindowEnabled() {
    static const bool s = []() {
        const char* v = getenv("MC2_TERRAIN_SOLID_NARROW");
        if (v && v[0] == '0' && v[1] == '\0') return false;
        return true;
    }();
    return s;
}

// Parity-probe gate for the windowed dispatch (catastrophic-axis: a visible
// quad dropped from the window).  Default-OFF; literal "1" arms it.  When off
// the probe path does ZERO per-frame work (no ref build, no assert).
bool IsSolidWindowParityEnabled() {
    static const bool s = []() {
        const char* v = getenv("MC2_TERRAIN_SOLID_WINDOW_PARITY");
        return v && v[0] == '1' && v[1] == '\0';
    }();
    return s;
}

bool IsCostSplitEnabled() {
    static const bool s = []() {
        const char* v = getenv("MC2_TERRAIN_COST_SPLIT");
        return v && v[0] == '1' && v[1] == '\0';
    }();
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
    static const bool s = []() {
        const char* v = getenv("MC2_TERRAIN_INDIRECT_MINE");
        if (v && v[0] == '0' && v[1] == '\0') return false;
        return true;
    }();
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
    static const bool s = []() {
        const char* v = getenv("MC2_TERRAIN_INDIRECT_OVERLAY");
        if (v && v[0] == '0' && v[1] == '\0') return false;
        return true;
    }();
    return s;
}

bool IsOverlayParityCheckEnabled() {
    static const bool s = []() {
        const char* v = getenv("MC2_TERRAIN_INDIRECT_OVERLAY_PARITY_CHECK");
        return v && v[0] == '1' && v[1] == '\0';
    }();
    return s;
}

// MC2_THIN_CANARY=1 enables the Probe-6 thin-record canary readback+compare
// (diagnostic for the corrupt-recipeIdx grey-triangle bug). Default OFF — it is a
// per-frame ~80KB glGetBufferSubData readback + CPU compare and must not run in
// production. Kill-switch restores it for diagnosis.
static const bool s_thinCanaryEnabled = []() -> bool {
    const char* v = std::getenv("MC2_THIN_CANARY");
    return v && v[0] == '1';
}();

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
        g_lcsOn   = (getenv("MC2_LIGHT_COST_SPLIT") != nullptr);
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
    const double f = (double)600.0;
    fprintf(stderr,
        "[LIGHT_COST_SPLIT v1] event=summary frames=600 "
        "c2_cyc_per_frame=%.0f c2_calls_per_frame=%.1f "
        "c6_cyc_per_frame=%.0f c6_calls_per_frame=%.1f "
        "c5_cyc_per_frame=%.0f c5_calls_per_frame=%.1f\n",
        (double)g_lcsC2Cyc/f, (double)g_lcsC2Call/f,
        (double)g_lcsC6Cyc/f, (double)g_lcsC6Call/f,
        (double)g_lcsC5Cyc/f, (double)g_lcsC5Call/f);
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

// ---------------------------------------------------------------------------
// Parity-printer + 600-frame summary
// ---------------------------------------------------------------------------
namespace {
int       s_parityMismatchesThisFrame = 0;
long long s_paritySummaryFrames       = 0;
long long s_paritySummaryQuads        = 0;
long long s_paritySummaryMismatches   = 0;
}  // namespace

namespace gos_terrain_indirect {

void ParityPrintMismatch(int frame, int quad, const char* layer, int tri,
                         int vert, const char* field,
                         uint32_t legacy, uint32_t fast) {
    if (s_parityMismatchesThisFrame >= 16) return;  // throttle 16/frame
    ++s_parityMismatchesThisFrame;
    fprintf(stderr,
            "[TERRAIN_INDIRECT_PARITY v1] event=mismatch frame=%d quad=%d "
            "layer=%s tri=%d vert=%d field=%s legacy=0x%08X fast=0x%08X\n",
            frame, quad, layer ? layer : "?", tri, vert, field ? field : "?",
            legacy, fast);
    fflush(stderr);
}

void ParityFrameTick(int quadsCheckedThisFrame) {
    ++s_paritySummaryFrames;
    s_paritySummaryQuads      += quadsCheckedThisFrame;
    s_paritySummaryMismatches += s_parityMismatchesThisFrame;
    s_parityMismatchesThisFrame = 0;
    if (s_paritySummaryFrames % 600 == 0) {
        // Cost-split columns are appended only when MC2_TERRAIN_COST_SPLIT
        // is set — otherwise the all-zero noise confuses readers.
        const int    csFrames = CostSplit_GetFramesObserved();
        const bool   csOn     = (csFrames > 0);
        const long long csSolidNs   = csOn ? CostSplit_GetSolidNanosTotal()         : 0;
        const long long csDetailNs  = csOn ? CostSplit_GetDetailOverlayNanosTotal() : 0;
        const long long csMineEnqNs = csOn ? CostSplit_GetMineEnqueueNanosTotal()   : 0;
        const long long csMineDrwNs = csOn ? CostSplit_GetMineDrawNanosTotal()      : 0;
        const long long csOverlayNs = csOn ? CostSplit_GetOverlayNanosTotal()       : 0;
        const long long csWvpNs     = csOn ? CostSplit_GetWaterVertProjNanosTotal() : 0;
        const long long csRecipeNs  = csOn ? CostSplit_GetRecipeCacheNanosTotal()   : 0;
        const long long csSetupNs   = csOn ? CostSplit_GetSetupTotalNanosTotal()    : 0;
        const long long csResNs     = csOn ? CostSplit_GetCacheResidentNanosTotal() : 0;
        const long long csSolidPerFrame    = csOn ? csSolidNs   / csFrames : 0;
        const long long csDetailPerFrame   = csOn ? csDetailNs  / csFrames : 0;
        const long long csMineEnqPerFrame  = csOn ? csMineEnqNs / csFrames : 0;
        const long long csMineDrwPerFrame  = csOn ? csMineDrwNs / csFrames : 0;
        const long long csOverlayPerFrame  = csOn ? csOverlayNs / csFrames : 0;
        const long long csWvpPerFrame      = csOn ? csWvpNs     / csFrames : 0;
        const long long csRecipePerFrame   = csOn ? csRecipeNs  / csFrames : 0;
        const long long csSetupPerFrame    = csOn ? csSetupNs   / csFrames : 0;
        const long long csResPerFrame      = csOn ? csResNs     / csFrames : 0;
        if (csOn) {
            fprintf(stderr,
                    "[TERRAIN_INDIRECT_PARITY v1] event=summary frames=%lld "
                    "quads_checked=%lld total_mismatches=%lld "
                    "legacy_solid_setup_quads=%lld "
                    "indirect_solid_packed_quads=%lld "
                    "legacy_detail_overlay_quads=%lld "
                    "legacy_mine_enqueue_quads=%lld "
                    "legacy_mine_draw_quads=%lld "
                    "indirect_mine_drawn_cells=%lld "
                    "m2c_detail_emit_quads=%lld "
                    "legacy_m2d_overlay_emit_quads=%lld "
                    "indirect_overlay_packed_quads=%lld "
                    "gos_push_overlay_calls=%lld "
                    "solid_branch_ns_per_frame=%lld "
                    "detail_overlay_branch_ns_per_frame=%lld "
                    "mine_enqueue_ns_per_frame=%lld "
                    "mine_draw_ns_per_frame=%lld "
                    "overlay_ns_per_frame=%lld "
                    "water_vert_proj_ns_per_frame=%lld "
                    "recipe_cache_ns_per_frame=%lld "
                    "setup_total_ns_per_frame=%lld "
                    "cache_resident_ns_per_frame=%lld "
                    "shape_c_invisible_quads=%lld "
                    "frames_observed=%d\n",
                    s_paritySummaryFrames,
                    s_paritySummaryQuads,
                    s_paritySummaryMismatches,
                    Counters_GetLegacySolidSetupQuads(),
                    Counters_GetIndirectSolidPackedQuads(),
                    0LL,  // Arc 2: counter retired; field kept for log schema stability (remove in Arc 5)
                    Counters_GetLegacyMineEnqueueQuads(),
                    Counters_GetLegacyMineDrawQuads(),
                    Counters_GetIndirectMineDrawnCells(),
                    Counters_GetM2cDetailEmitQuads(),
                    Counters_GetM2dOverlayEmitQuads(),
                    Counters_GetIndirectOverlayPackedQuads(),
                    Counters_GetGosPushOverlayCalls(),
                    csSolidPerFrame,
                    csDetailPerFrame,
                    csMineEnqPerFrame,
                    csMineDrwPerFrame,
                    csOverlayPerFrame,
                    csWvpPerFrame,
                    csRecipePerFrame,
                    csSetupPerFrame,
                    csResPerFrame,
                    Counters_GetShapeCInvisibleQuads(),
                    csFrames);
        } else {
            fprintf(stderr,
                    "[TERRAIN_INDIRECT_PARITY v1] event=summary frames=%lld "
                    "quads_checked=%lld total_mismatches=%lld "
                    "legacy_solid_setup_quads=%lld "
                    "indirect_solid_packed_quads=%lld "
                    "legacy_detail_overlay_quads=%lld "
                    "legacy_mine_enqueue_quads=%lld "
                    "legacy_mine_draw_quads=%lld "
                    "indirect_mine_drawn_cells=%lld "
                    "m2c_detail_emit_quads=%lld "
                    "legacy_m2d_overlay_emit_quads=%lld "
                    "indirect_overlay_packed_quads=%lld "
                    "gos_push_overlay_calls=%lld "
                    "shape_c_invisible_quads=%lld\n",
                    s_paritySummaryFrames,
                    s_paritySummaryQuads,
                    s_paritySummaryMismatches,
                    Counters_GetLegacySolidSetupQuads(),
                    Counters_GetIndirectSolidPackedQuads(),
                    0LL,  // Arc 2: counter retired; field kept for log schema stability (remove in Arc 5)
                    Counters_GetLegacyMineEnqueueQuads(),
                    Counters_GetLegacyMineDrawQuads(),
                    Counters_GetIndirectMineDrawnCells(),
                    Counters_GetM2cDetailEmitQuads(),
                    Counters_GetM2dOverlayEmitQuads(),
                    Counters_GetIndirectOverlayPackedQuads(),
                    Counters_GetGosPushOverlayCalls(),
                    Counters_GetShapeCInvisibleQuads());
        }
        fflush(stderr);
    }
}

}  // namespace gos_terrain_indirect

// ---------------------------------------------------------------------------
// Stage 2 — dense recipe SSBO storage and helpers
// ---------------------------------------------------------------------------

namespace {

// Module-private: dense recipe array indexed by vertexNum (= mx + my * mapSide).
// Sized mapSide² when built; cleared on ResetDenseRecipe.
std::vector<TerrainQuadRecipe> g_denseRecipes;
std::vector<bool>              g_denseRecipeDirty;

// ---------------------------------------------------------------------------
// Camera-windowed solid dispatch (Approach A: GPU windowed-index buffer).
// Structural twin of WaterStream's narrow path (gos_terrain_water_stream.cpp
// BuildQuadWindowSSBO / g_quadWindowSsbo / g_quadWindowSsboCapacity).  std430
// plain uint[] (4 B stride): each entry is a recipe index (= top-left
// vertexNum).
//
// LIFECYCLE (LAG-FREE): terrain.cpp calls BeginFrameSolidWindow() then fills
// g_solidWindowStaging in the SLIM LOOP (Terrain::geometry slimReduce), which
// runs BEFORE gos_terrain_indirect::ComputeDispatch() consumes it later in
// the SAME geometry() call.  Frame N's dispatch consumes frame N's window —
// no 1-frame lag, so a moving camera cannot transiently vanish edge terrain.
// Declared here (not with the other Stage-3 GLuint state) so the per-mission
// ResetDenseRecipe() below can clear the staging + high-water mark (MAJOR-1).
std::vector<uint32_t> g_solidWindowStaging;
GLuint   g_solidQuadWindowSsbo     = 0;   // per-frame: recipe indices in camera window
uint32_t g_solidQuadWindowCapacity = 0;   // CPU-side mirror of allocated size (bytes); NO readback
uint32_t g_solidWindowMaxSeen      = 0;   // high-water mark for reserve sizing

bool                           g_denseRecipeAnyDirty = false;
GLuint                         g_recipeSSBO          = 0;
int32_t                        g_recipeMapSide       = 0;
bool                           g_recipeReady         = false;

// VPL parity-infra retirement (cpu-pack-retirement plan §7 OQ-2, full
// delete): s_packParityMask / kParityMaskWords removed. The last consumer
// (gos_terrain_mask_dispatch accessor read) was retired in Step 4 (2e11617);
// the txmmgr ComputeDispatchParity_Check caller is removed in this same
// commit. See memory/mc_texture_manager_dual_queue_legacy_retirement_debt.md.

// Mission-latch for trace reset
static bool s_firstDrawPrintedThisMission = false;

// Cached trace flag (avoids repeated getenv calls in hot paths)
static bool s_indirectTrace = false;
static bool s_indirectTraceKnown = false;
static bool traceOn() {
    if (!s_indirectTraceKnown) {
        const char* v = getenv("MC2_TERRAIN_INDIRECT_TRACE");
        s_indirectTrace = (v && v[0] == '1' && v[1] == '\0');
        s_indirectTraceKnown = true;
    }
    return s_indirectTrace;
}

// Map MC2 terrain type enum (0-20) to PBR material index (0-3).
// Mirrors quad.cpp terrainTypeToMaterial (file-static, not exported) and
// gos_terrain_water_stream.cpp terrainTypeToMaterialLocal.
// MUST stay in sync. Any drift shows up immediately in the parity check
// _wp0 comparison the next time MC2_TERRAIN_INDIRECT_PARITY_CHECK runs.
// 0=Rock, 1=Grass, 2=Dirt, 3=Concrete
inline uint8_t terrainTypeToMaterialLocal(uint32_t terrainType) {
    switch (terrainType) {
        case 3:  case 8:  case 9:  case 12:           return 1; // Grass
        case 2:  case 4:                              return 2; // Dirt
        case 10: case 13: case 14: case 15: case 16:
        case 17: case 18: case 19: case 20:           return 3; // Concrete
        default:                                      return 0; // Rock
    }
}

// Populate one slot in the dense recipe array from MapData.
// Called both from BuildDenseRecipe (full build at primeMissionTerrainCache)
// and from InvalidateRecipeForVertexNum (in-gameplay precise rebuild).
//
// Judgment call on UV defaults during in-gameplay invalidation:
//   setTerrain() calls invalidateTerrainFaceCache(), which frees the entire
//   Shape C cache. After that, getTerrainFaceCacheEntry returns nullptr until
//   the next primeMissionTerrainCache call (mission reload). For in-gameplay
//   mutations we therefore fall back to the default UV values from
//   quad.cpp:1734-1737 — the same fallback legacy uses once the cache is gone.
//   This is correct: default UVs are the pre-Shape-C half-texel padding values.
void buildRecipeSlot(int32_t vn, TerrainQuadRecipe& out) {
    const long mapSide = Terrain::realVerticesMapSide;
    if (mapSide <= 0) { memset(&out, 0, sizeof(TerrainQuadRecipe)); return; }

    // vn = top-left corner vertexNum = mx + my * mapSide
    const long mx = vn % mapSide;
    const long my = vn / mapSide;

    // Edge vertex: no valid quad. Zero out.
    if (mx >= mapSide - 1 || my >= mapSide - 1) {
        memset(&out, 0, sizeof(TerrainQuadRecipe));
        return;
    }

    const PostcompVertexPtr blocks = Terrain::mapData ? Terrain::mapData->getBlocks() : nullptr;
    if (!blocks) { memset(&out, 0, sizeof(TerrainQuadRecipe)); return; }

    const long halfSide = Terrain::halfVerticesMapSide;
    const float wupv    = Terrain::worldUnitsPerVertex;

    // Corner layout (matches water stream and quad.cpp):
    //   v0 = (mx,   my)     top-left
    //   v1 = (mx+1, my)     top-right
    //   v2 = (mx+1, my+1)   bottom-right
    //   v3 = (mx,   my+1)   bottom-left
    const PostcompVertex& p0 = blocks[mx       + my       * mapSide];
    const PostcompVertex& p1 = blocks[(mx + 1) + my       * mapSide];
    const PostcompVertex& p2 = blocks[(mx + 1) + (my + 1) * mapSide];
    const PostcompVertex& p3 = blocks[mx       + (my + 1) * mapSide];

    // World X/Y from map indices (gos_terrain_water_stream.cpp:125-130)
    const float wx0 = float(mx     - halfSide) * wupv;
    const float wy0 = float(halfSide - my    ) * wupv;
    const float wx1 = float(mx + 1 - halfSide) * wupv;
    const float wy1 = wy0;
    const float wx2 = wx1;
    const float wy2 = float(halfSide - (my + 1)) * wupv;
    const float wx3 = wx0;
    const float wy3 = wy2;

    out.wx0 = wx0; out.wy0 = wy0; out.wz0 = p0.elevation; out._wp0 = 0.f;
    out.wx1 = wx1; out.wy1 = wy1; out.wz1 = p1.elevation; out._wp1 = 0.f;
    out.wx2 = wx2; out.wy2 = wy2; out.wz2 = p2.elevation; out._wp2 = 0.f;
    out.wx3 = wx3; out.wy3 = wy3; out.wz3 = p3.elevation; out._wp3 = 0.f;

    out.nx0 = p0.vertexNormal.x; out.ny0 = p0.vertexNormal.y; out.nz0 = p0.vertexNormal.z; out._np0 = 0.f;
    out.nx1 = p1.vertexNormal.x; out.ny1 = p1.vertexNormal.y; out.nz1 = p1.vertexNormal.z; out._np1 = 0.f;
    out.nx2 = p2.vertexNormal.x; out.ny2 = p2.vertexNormal.y; out.nz2 = p2.vertexNormal.z; out._np2 = 0.f;
    out.nx3 = p3.vertexNormal.x; out.ny3 = p3.vertexNormal.y; out.nz3 = p3.vertexNormal.z; out._np3 = 0.f;

    // UV extents — mirror quad.cpp:1734-1748 logic.
    // Default: half-texel padding. Override from Shape C cache when available.
    float minU = 0.5f / TERRAIN_TXM_SIZE;
    float maxU = 1.0f - 0.5f / TERRAIN_TXM_SIZE;
    float minV = minU;
    float maxV = maxU;

    if (Terrain::terrainTextures2 && Terrain::mapData) {
        const MapData::WorldQuadTerrainCacheEntry* entry =
            Terrain::mapData->getTerrainFaceCacheEntry(my, mx);  // (tileR=my, tileC=mx)
        if (entry && entry->isValid()) {
            // Mirror quad.cpp:1743: only use uvData when NOT (overlayHandle==0xffffffff && isCement)
            const bool isCement  = entry->isCement();
            const bool noOverlay = (entry->overlayHandle == 0xffffffffu);
            if (!(noOverlay && isCement)) {
                minU = entry->uvData.minU;
                minV = entry->uvData.minV;
                maxU = entry->uvData.maxU;
                maxV = entry->uvData.maxV;
            }
            // Bake terrain texture nodeId into _wp2 for GPU-direct dispatch.
            // _wp3 (cementWord) is baked in PopulateRecipeCementWords() after
            // BuildCementCatalogAtlas() because g_cementLayerIndexBySlot isn't
            // ready yet at buildRecipeSlot() time.
            const uint32_t nodeId = (uint32_t)entry->terrainHandle;
            memcpy(&out._wp2, &nodeId, 4);
        }
        // If entry is NULL or !isValid(): _wp2 remains 0 → GPU skips quad.
    }

    out.minU = minU; out.minV = minV; out.maxU = maxU; out.maxV = maxV;

    // Pack 4 corner material types into _wp0 (bit-preserving; shader reads
    // via floatBitsToUint per gos_terrain_thin.vert:122).
    {
        const uint32_t m0 = terrainTypeToMaterialLocal(p0.terrainType);
        const uint32_t m1 = terrainTypeToMaterialLocal(p1.terrainType);
        const uint32_t m2 = terrainTypeToMaterialLocal(p2.terrainType);
        const uint32_t m3 = terrainTypeToMaterialLocal(p3.terrainType);
        const uint32_t tpacked = m0 | (m1 << 8) | (m2 << 16) | (m3 << 24);
        memcpy(&out._wp0, &tpacked, 4);
    }

    // NOTE: _wp3 (cement word) is NOT baked here.  Reasons:
    //   * First-init from BuildDenseRecipe: g_cementLayerIndexBySlot is empty
    //     (BuildCementCatalogAtlas hasn't run yet) — PopulateRecipeCementWords
    //     bakes it post-atlas-build.
    //   * Later callers (InvalidateRecipeForVertexNum / InvalidateAllRecipes):
    //     they MUST re-bake cement after calling buildRecipeSlot, otherwise
    //     out._wp3 = 0.f above silently wipes the cement layer for every
    //     subsequent FlushDirtyRecipeSlotsToGPU upload.
}


}  // anonymous namespace

// ---------------------------------------------------------------------------
// Colormap atlas — single GL texture covering the full merged colormap.
//
// cpuColorMap (terrtxm2.h:93) already holds the entire RGBA atlas in CPU
// memory, retained at mission load.  We upload it once as a plain
// GL_TEXTURE_2D so gos_terrain_bridge_drawIndirect can bind it at unit 0
// instead of per-bucket tile binds.
//
// Atlas UV formula (mirrors terrtxm2.cpp:resolveTextureHandle):
//   posX  = (worldX - mapTopLeft3d.x) * oneOverWorldUnitsMapSide
//   posY  = (mapTopLeft3d.y - worldY) * oneOverWorldUnitsMapSide
//   tileX = floor((posX + 0.0005) * numTexturesAcross)
//   tileY = floor((posY + 0.0005) * numTexturesAcross)
//   atlasUV = (vec2(tileX, tileY) + perTileUV) / numTexturesAcross
//
// The per-tile UV (from recipe.uvData) is already in [0,1] within a tile.
// Dividing by numTexturesAcross converts tile-local to atlas-absolute.
// ---------------------------------------------------------------------------

namespace {

static GLuint  g_atlasGLTex              = 0;
static int     g_atlasSize               = 0;
static float   g_atlasNumTexturesAcross  = 0.f;
static float   g_atlasMapTopLeftX        = 0.f;
static float   g_atlasMapTopLeftY        = 0.f;
static float   g_atlasOneOverWorldUnits  = 0.f;

// ---------------------------------------------------------------------------
// Cement catalog atlas — single GL_TEXTURE_2D, packed grid of N cement tile
// textures.  Built once per mission at BuildDenseRecipe() time via GPU
// readback from already-resident catalog textures (textureData[0] in
// tileRAMHeap is dead in stock gameplay — quickLoad gates the RAM path at
// terrtxm.cpp:561).  Bound at unit 3 by gos_terrain_bridge_drawIndirect.
//
// LAYER MAP: keyed by mcTextureNodeIndex (NOT textures[] slot).
//   q.terrainHandle returned by quad.cpp:546 (getTextureHandle) is the
//   nodeIdx, NOT the slot — see V22 in Verification Appendix.
// ---------------------------------------------------------------------------
static GLuint  g_cementAtlasGLTex          = 0;
static int     g_cementAtlasGridSide       = 0;   // cells per row/col (power of 2)
static int     g_cementAtlasTileCount      = 0;   // distinct cement entries enumerated
static bool    g_cementLayerMapReady       = false;
static int     g_cementCatalogTruncated    = 0;   // 1 if N>=255 cap hit (Gate A FAIL)

// Dense lookup: mcTextureNodeIndex → atlas layer-index (0..N-1).
// Sized MC_MAXTEXTURES = 4096 (mclib/txmmgr.h:44) — node-index space.
// 0xFFFF = "not cement / not in atlas".
static uint16_t g_cementLayerIndexByNodeIdx[MC_MAXTEXTURES];

// Slot-keyed lookup: textures[] slot index → atlas layer-index (0..N-1).
// SLOT IS THE STABLE KEY (persistent across frames).  nodeIdx (mcTextureManager
// handle) mutates per-frame per memory/mc2_texture_handle_is_live.md, so a
// nodeIdx-keyed lookup miss-hits per frame → cement validity bit flickers
// → visible concrete flicker.  Slot is allocated once by initTexture and is
// stable for the mission lifetime.
//
// Sized MC_MAX_TERRAIN_TXMS = 3000 (terrtxm.h:34) — the textures[] cap.
// 0xFFFF = "not cement / not in atlas".
static uint16_t g_cementLayerIndexBySlot[MC_MAX_TERRAIN_TXMS];

// Stage B — cement transitions in recipe.
static constexpr uint32_t kCementTransitionBit = 0x40000000u;  // IS_TRANSITION in cementWord
static constexpr uint32_t kCementMaskIdShift    = 24u;         // bits 29:24 = maskId
static constexpr uint32_t kCementMaskIdMask     = 0x3Fu;       // 6 bits (14 shapes used)
static constexpr int      kTransitionMaskLayers = 14;
static constexpr int      kTransitionMaskSize   = 256;

static GLuint g_transitionMaskArrayGL = 0;
static bool   g_transitionMaskReady   = false;

// binNumber = (c0?8:0)|(c1?4:0)|(c2?2:0)|(c3?1:0); 0 and 15 are degenerate.
// maskId = binNumber - 1 (0..13).
static int deriveMaskIdFromCorners(bool c0, bool c1, bool c2, bool c3) {
    const uint32_t bn = (c0 ? 8u : 0u) | (c1 ? 4u : 0u) | (c2 ? 2u : 0u) | (c3 ? 1u : 0u);
    if (bn == 0u || bn == 15u) return -1;
    return (int)(bn - 1u);
}

// Build 14-layer R8 GL_TEXTURE_2D_ARRAY procedurally (bilinear weights + smoothstep).
// Called once at end of BuildCementCatalogAtlas().
static void BuildTransitionMaskArray() {
    const int N = kTransitionMaskLayers;
    const int S = kTransitionMaskSize;
    std::vector<uint8_t> buf((size_t)N * S * S, 0u);
    for (int k = 0; k < N; ++k) {
        const uint32_t bn = (uint32_t)(k + 1);
        const bool c0 = (bn & 8u) != 0u;
        const bool c1 = (bn & 4u) != 0u;
        const bool c2 = (bn & 2u) != 0u;
        const bool c3 = (bn & 1u) != 0u;
        uint8_t* layer = &buf[(size_t)k * S * S];
        for (int py = 0; py < S; ++py) {
            for (int px = 0; px < S; ++px) {
                const float u  = (float(px) + 0.5f) / float(S);
                const float v  = (float(py) + 0.5f) / float(S);
                const float w0 = (1.f - u) * (1.f - v);  // corner 0: UV(0,0)
                const float w1 =        u  * (1.f - v);  // corner 1: UV(1,0)
                const float w2 =        u  *        v;   // corner 2: UV(1,1)
                const float w3 = (1.f - u) *        v;   // corner 3: UV(0,1)
                float cov = (c0 ? w0 : 0.f) + (c1 ? w1 : 0.f)
                          + (c2 ? w2 : 0.f) + (c3 ? w3 : 0.f);
                cov = cov * cov * (3.f - 2.f * cov);  // smoothstep sharpening
                layer[py * S + px] = (uint8_t)(std::min(cov, 1.f) * 255.f + 0.5f);
            }
        }
    }
    // TEX-CLASS: asset-pool -- terrain transition-mask 2D_ARRAY (content)
    if (g_transitionMaskArrayGL == 0) glGenTextures(1, &g_transitionMaskArrayGL);
    glBindTexture(GL_TEXTURE_2D_ARRAY, g_transitionMaskArrayGL);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_R8, S, S, N, 0, GL_RED, GL_UNSIGNED_BYTE, buf.data());
    glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    g_transitionMaskReady = true;

    // REGISTRY-TERRAIN-SSBO-1: register the transition-mask 2D_ARRAY (observe-only).
    {
        RenderCore::RenderResourceDesc d;
        d.id        = RenderCore::RenderResourceId::TransitionMaskArray;
        d.kind      = RenderCore::RenderResourceKind::Texture2DArray;
        d.lifetime  = RenderCore::RenderResourceLifetime::Mission;  // rebuilt per mission load
        d.format    = RenderCore::RenderResourceFormat::RGBA8;  // R8 storage; closest enum slot
        d.debugName = "TransitionMaskArray";
        d.width     = static_cast<uint32_t>(S);
        d.height    = static_cast<uint32_t>(S);
        d.layers    = static_cast<uint32_t>(N);
        d.glName    = static_cast<uint32_t>(g_transitionMaskArrayGL);
        d.sizeBytes = static_cast<uint64_t>(N) * static_cast<uint64_t>(S) * static_cast<uint64_t>(S);
        d.valid     = true;
        RenderCore::registerOrUpdateRenderResource(d);
    }

    printf("[CEMENT_ATLAS v1] event=transition_mask_array_built layers=%d size=%dx%d\n", N, S, S);
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// PopulateRecipeCementWords — bake cementWord into recipe._wp3 for every slot.
// Must be called AFTER BuildCementCatalogAtlas() because g_cementLayerIndexBySlot
// is not populated until that function runs.
// Marks all modified slots dirty so FlushDirtyRecipeSlotsToGPU uploads them.
// ---------------------------------------------------------------------------
static void PopulateRecipeCementWords() {
    if (!Terrain::mapData) return;
    const long mapSide = Terrain::realVerticesMapSide;
    if (mapSide <= 0) return;
    const size_t N = g_denseRecipes.size();
    constexpr uint32_t kCementLayerValidBit = 0x80000000u;

    // Per-vertex terrain type array for corner lookup in transition handling.
    PostcompVertexPtr blocks = Terrain::mapData->getBlocks();
    auto* tt = Terrain::terrainTextures;

    long cementQuadCount    = 0;
    long transitionQuadCount = 0;
    // Step 5c: also collect a full per-vn cement-word array for the terrain LOD
    // chunk path (indexed vn = mx + my*mapSide, matching its heightfield SSBO).
    std::vector<uint32_t> chunkCementWords((size_t)mapSide * (size_t)mapSide, 0u);
    for (size_t vn = 0; vn < N; ++vn) {
        const long mx = (long)vn % mapSide;
        const long my = (long)vn / mapSide;
        if (mx >= mapSide - 1 || my >= mapSide - 1) continue;

        uint32_t cementWord = 0u;
        if (g_cementLayerMapReady) {
            const DWORD texData = Terrain::mapData->getTexture(my, mx);
            const DWORD slot    = texData & 0xFFFFu;
            if (slot < (DWORD)MC_MAX_TERRAIN_TXMS) {
                const uint16_t idx = g_cementLayerIndexBySlot[slot];
                if (idx != 0xFFFFu) {
                    // Solid cement quad — existing path, unchanged.
                    cementWord = kCementLayerValidBit | ((uint32_t)idx & 0xFFFFu);
                    ++cementQuadCount;
                } else if (blocks && tt
                           && tt->isCement((DWORD)slot)
                           && tt->isAlpha((DWORD)slot)) {
                    // Transition quad: read per-vertex terrain types from PostcompVertex.
                    // v0=(my,mx) v1=(my,mx+1) v2=(my+1,mx+1) v3=(my+1,mx) — matches
                    // mapdata.cpp setTexture() corner ordering and createTransition() packing.
                    auto cemLayerForVertex = [&](long row, long col) -> uint16_t {
                        const size_t vi = (size_t)row * (size_t)mapSide + (size_t)col;
                        const DWORD  vt = blocks[vi].terrainType;
                        return (vt < (DWORD)MC_MAX_TERRAIN_TXMS)
                               ? g_cementLayerIndexBySlot[vt] : 0xFFFFu;
                    };
                    const uint16_t l0 = cemLayerForVertex(my,     mx);
                    const uint16_t l1 = cemLayerForVertex(my,     mx + 1);
                    const uint16_t l2 = cemLayerForVertex(my + 1, mx + 1);
                    const uint16_t l3 = cemLayerForVertex(my + 1, mx);
                    const bool c0 = (l0 != 0xFFFFu);
                    const bool c1 = (l1 != 0xFFFFu);
                    const bool c2 = (l2 != 0xFFFFu);
                    const bool c3 = (l3 != 0xFFFFu);
                    const int maskId = deriveMaskIdFromCorners(c0, c1, c2, c3);
                    // Cement atlas layer from first cement corner.
                    const uint16_t cementLayerIdx = c0 ? l0 : (c1 ? l1 : (c2 ? l2 : l3));
                    if (maskId >= 0 && cementLayerIdx != 0xFFFFu) {
                        cementWord = kCementLayerValidBit
                                   | kCementTransitionBit
                                   | ((uint32_t)(maskId & (int)kCementMaskIdMask) << kCementMaskIdShift)
                                   | ((uint32_t)cementLayerIdx & 0xFFFFu);
                        ++transitionQuadCount;
                    }
                }
            }
        }
        memcpy(&g_denseRecipes[vn]._wp3, &cementWord, 4);
        g_denseRecipeDirty[vn]  = true;
        if (vn < chunkCementWords.size()) chunkCementWords[vn] = cementWord;
    }
    g_denseRecipeAnyDirty = true;
    // Push to the chunk renderer (no-op if its SSBO/Init has not run yet).
    gos_TerrainLodChunk_UploadCementWordsFull(
        chunkCementWords.data(), (int)chunkCementWords.size(), (int)mapSide);
    printf("[CEMENT_ATLAS v1] event=cement_words_baked cement_solid=%ld cement_transition=%ld total_vn=%zu\n",
           cementQuadCount, transitionQuadCount, N);
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// CollectUniqueNodeIds — build list of unique terrain texture nodeIds from
// the baked _wp2 fields. Called once per mission after recipes are fully
// populated. Used by UploadTerrainHandleLUT() to resolve handles per frame
// without iterating the full quadList.
// ---------------------------------------------------------------------------
static std::vector<uint32_t> g_uniqueTerrainNodeIds;

static void CollectUniqueNodeIds() {
    g_uniqueTerrainNodeIds.clear();
    static bool seen[MC_MAXTEXTURES];
    memset(seen, 0, sizeof(seen));
    for (const TerrainQuadRecipe& rec : g_denseRecipes) {
        uint32_t nodeId;
        memcpy(&nodeId, &rec._wp2, 4);
        if (!gos_terrain_arm::IsTileHandle(nodeId, (uint32_t)MC_MAXTEXTURES)) continue;
        if (!seen[nodeId]) {
            seen[nodeId] = true;
            g_uniqueTerrainNodeIds.push_back(nodeId);
        }
    }
}

// COLORMAP-CPU-RETIRE-1: free cpuColorMap + cpuDispAlpha after atlas upload.
// Default ON. Kill-switch MC2_COLORMAP_CPU_RETIRE=0 reverts (keeps CPU copy alive
// for legacy displacement; needed when MC2_COLORMAP_DISPLACE_PROBE=1).
static bool retireCpuColorMap() {
    static const bool s = []() {
        const char* v = getenv("MC2_COLORMAP_CPU_RETIRE");
        return !v || v[0] != '0';
    }();
    return s;
}

// Per-frame counter — incremented when the packer sees a quad whose
// q.terrainHandle is non-zero AND maps to no cement layer.  A non-zero count
// after Stage A.4 is wired indicates an enumeration miss (debug discipline).
static uint32_t g_cementPackUnmappedCount = 0;

// Diagnostic Test 1 — per-frame cement classification flip detection.
// Reset at the start of PackThinRecordsForFrame, emitted every 60 frames
// when MC2_TERRAIN_INDIRECT_TRACE is on.
static uint32_t g_cementMappedThisFrame       = 0;  // valid cement layer found
static uint32_t g_concreteAllCornersThisFrame = 0;  // _wp0 == 3,3,3,3 (genuine pure-cement quad)


void BuildColormapAtlas() {
    ZoneScopedN("Terrain::IndirectAtlasUpload");
    if (!Terrain::terrainTextures2) {
        if (traceOn()) printf("[TERRAIN_INDIRECT v1] event=atlas_skip reason=no_terrainTextures2\n");
        return;
    }
    auto* tcm = Terrain::terrainTextures2;

    int atlasSizeCapture = 0;
    bool atlasBuilt = false;

    // --- KTX2 / BC7 path (COLORMAP-BC7-KTX2-1) ---
    // Precedes the cpuColorMap early-return so the KTX2 path fires even when
    // cpuColorMap was never allocated (e.g. future "skip-cpu-alloc" optimization).
    // cpuColorMap is always populated as RGBA8 fallback (in case BPTC cap absent).
    if (tcm->ktx2ColormapPath[0] != '\0' && GLEW_ARB_texture_compression_bptc) {
        RenderCore::KtxImage img;
        if (RenderCore::ktxLoadRgba8(tcm->ktx2ColormapPath, img) &&
            img.isCompressed && (img.vkFormat == 145 || img.vkFormat == 146)) {

            GLenum glIF = (img.vkFormat == 146) ? GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM
                                                 : GL_COMPRESSED_RGBA_BPTC_UNORM;
            // Mip 0 only: colormap is sampled at a fixed world-space scale, no LOD.
            size_t mip0Bytes = (img.mipCount > 1 && img.mipByteOffsets.size() > 1)
                ? (size_t)img.mipByteOffsets[1]
                : img.pixels.size();

            // TEX-CLASS: asset-pool -- terrain colormap atlas (content)
            if (g_atlasGLTex == 0) glGenTextures(1, &g_atlasGLTex);
            MC2_GL_BindTexture(GL_TEXTURE_2D, g_atlasGLTex);
            glCompressedTexImage2D(GL_TEXTURE_2D, 0, glIF,
                                   img.width, img.height, 0,
                                   (GLsizei)mip0Bytes, img.pixels.data());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            MC2_GL_BindTexture(GL_TEXTURE_2D, 0);

            atlasSizeCapture = img.width;
            atlasBuilt = true;

            printf("[COLORMAP] COLORMAP-BC7-KTX2-1: uploaded %dx%d BC7 fmt=0x%x "
                   "bytes=%zu gltex=%u\n",
                   img.width, img.height, (unsigned)glIF, mip0Bytes,
                   (unsigned)g_atlasGLTex);

            // KTX2 path succeeded — retire CPU copies (same as RETIRE-1, but
            // also clear the path so we don't attempt a second upload).
            tcm->ktx2ColormapPath[0] = '\0';
            if (retireCpuColorMap()) {
                free(tcm->cpuColorMap);   tcm->cpuColorMap    = nullptr;
                tcm->cpuColorMapSize = 0;
                free(tcm->cpuDispAlpha);  tcm->cpuDispAlpha   = nullptr;
                tcm->cpuDispAlphaSize = 0;
            }
        } else {
            // KTX2 load/format check failed — clear path, fall through to RGBA8.
            printf("[COLORMAP] COLORMAP-BC7-KTX2-1: KTX2 load failed or wrong format "
                   "(%s), falling back to RGBA8\n", tcm->ktx2ColormapPath);
            tcm->ktx2ColormapPath[0] = '\0';
        }
    } else if (tcm->ktx2ColormapPath[0] != '\0' && !GLEW_ARB_texture_compression_bptc) {
        // BPTC cap absent — log once and fall through to RGBA8.
        printf("[COLORMAP] COLORMAP-BC7-KTX2-1: BPTC unsupported, using RGBA8 fallback\n");
        tcm->ktx2ColormapPath[0] = '\0';
    }

    // --- RGBA8 fallback (pre-existing path) ---
    if (!atlasBuilt) {
        if (!tcm->cpuColorMap || tcm->cpuColorMapSize <= 0) {
            if (traceOn()) printf("[TERRAIN_INDIRECT v1] event=atlas_skip reason=no_cpuColorMap\n");
            return;
        }

        // TEX-CLASS: asset-pool -- terrain colormap atlas (content, alt path)
        if (g_atlasGLTex == 0) glGenTextures(1, &g_atlasGLTex);
        MC2_GL_BindTexture(GL_TEXTURE_2D, g_atlasGLTex);
        // cpuColorMap is BGRA-in-memory (mc2_argb_packing memory note: MC2's
        // textures are BGRA). Upload format param = GL_BGRA so the driver swizzles
        // to RGBA8 storage at upload time.
        MC2_GL_TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                     tcm->cpuColorMapSize, tcm->cpuColorMapSize,
                     0, GL_BGRA, GL_UNSIGNED_BYTE, tcm->cpuColorMap);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        MC2_GL_BindTexture(GL_TEXTURE_2D, 0);

        atlasSizeCapture = tcm->cpuColorMapSize;

        if (retireCpuColorMap()) {
            // Atlas is on GPU — release the CPU copies (COLORMAP-CPU-RETIRE-1).
            free(tcm->cpuColorMap);   tcm->cpuColorMap    = nullptr;
            tcm->cpuColorMapSize = 0;
            free(tcm->cpuDispAlpha);  tcm->cpuDispAlpha   = nullptr;
            tcm->cpuDispAlphaSize = 0;
            if (traceOn())
                printf("[COLORMAP] COLORMAP-CPU-RETIRE-1: released cpuColorMap+cpuDispAlpha "
                       "(%dx%d BGRA) after RGBA8 atlas upload\n",
                       atlasSizeCapture, atlasSizeCapture);
        }
    }

    g_atlasSize              = atlasSizeCapture;
    g_atlasNumTexturesAcross = tcm->getNumTexturesAcross();
    g_atlasMapTopLeftX       = Terrain::mapTopLeft3d.x;
    g_atlasMapTopLeftY       = Terrain::mapTopLeft3d.y;
    g_atlasOneOverWorldUnits = Terrain::oneOverWorldUnitsMapSide;

    if (traceOn()) {
        printf("[TERRAIN_INDIRECT v1] event=atlas_built size=%d numTilesAcross=%.4f "
               "mapTopLeftX=%.3f mapTopLeftY=%.3f oneOverWorldUnits=%.9f gltex=%u "
               "bc7=%s\n",
               g_atlasSize, g_atlasNumTexturesAcross,
               g_atlasMapTopLeftX, g_atlasMapTopLeftY, g_atlasOneOverWorldUnits,
               (unsigned)g_atlasGLTex, atlasBuilt ? "yes" : "no");
        fflush(stdout);
    }
}

// Bridge accessor: gosHandle → GLuint texture name (declared in
// gos_terrain_bridge.h, included above).  Implemented at
// GameOS/gameos/gameos_graphics.cpp:1775-1781.

// BuildCementCatalogAtlas — GPU readback path (B1 of plan v2.1).
// Runs at BuildDenseRecipe() time.  Walks textures[0..nextAvailable-1],
// filters by isCement(slot), reads each cement tile via glGetTexImage,
// blits into a packed grid atlas, uploads as a single GL_TEXTURE_2D.
//
// quickLoad-safe: does not depend on tileRAMHeap textureData[0] (which
// is NULL in stock gameplay per terrtxm.cpp:561 enclosing gate).
//
// NO MIPMAPS: cement atlas cells are packed without inter-cell gutters,
// so glGenerateMipmap would bleed neighboring cells.  Sampler is GL_LINEAR
// min/mag; potential shimmer at distance/oblique angles is accepted —
// Gate A includes a distance/oblique screenshot to surface this.  Per-cell
// mip generation with gutters is a follow-up slice.
void BuildCementCatalogAtlas() {
    ZoneScopedN("Terrain::IndirectCementAtlasUpload");

    if (!Terrain::terrainTextures) {
        if (traceOn()) printf("[TERRAIN_INDIRECT v1] event=cement_atlas_skip reason=no_terrainTextures\n");
        return;
    }
    auto* tt = Terrain::terrainTextures;

    const int txmSize = TERRAIN_TXM_SIZE;  // extern int, typically 64 (terrtxm.cpp:51)
    const long lastSlot = tt->getNextAvailableSlot();
    if (lastSlot <= 0) {
        if (traceOn()) printf("[TERRAIN_INDIRECT v1] event=cement_atlas_skip reason=no_slots\n");
        return;
    }

    // Pass 1: enumerate cement slots, resolve each to (nodeIdx, GLuint).
    std::vector<int>    cementSlots;
    std::vector<DWORD>  cementNodeIndices;
    std::vector<GLuint> cementGLTextures;
    cementSlots.reserve(64);
    cementNodeIndices.reserve(64);
    cementGLTextures.reserve(64);
    bool truncated = false;

    // CEMENT_DIAG (temporary, gated by MC2_TERRAIN_INDIRECT_TRACE).
    // Walk the FULL slot range without the 255-cap break, classify each
    // cement-flagged slot as base (< firstTransition) or transition,
    // and emit a per-slot line + summary.  See MEMORY: cement_diag breakdown.
    const long firstTransition = tt->getFirstTransition();
    long diagTotal = 0, diagBase = 0, diagTransition = 0;

    for (long slot = 0; slot < lastSlot; ++slot) {
        if (!tt->isCement((DWORD)slot)) continue;
        if (tt->isAlpha((DWORD)slot)) continue;  // pure-cement only (brainstorm Q1)
        const DWORD nodeIdx = tt->peekTextureHandle((DWORD)slot);

        // CEMENT_DIAG per-slot line.
        const int isBase = (firstTransition < 0 || slot < firstTransition) ? 1 : 0;
        ++diagTotal;
        if (isBase) ++diagBase; else ++diagTransition;
        if (traceOn()) {
            printf("[CEMENT_DIAG] slot=%ld nodeIdx=%u isBase=%d (firstTransition=%ld nextAvailable=%ld)\n",
                   slot, (unsigned)nodeIdx, isBase,
                   firstTransition, lastSlot);
        }

        if (nodeIdx == 0xffffffffu) continue;
        if (nodeIdx >= (DWORD)MC_MAXTEXTURES) {
            if (traceOn()) {
                printf("[TERRAIN_INDIRECT v1] event=cement_atlas_nodeidx_oob "
                       "slot=%ld nodeIdx=%u cap=%d\n",
                       slot, (unsigned)nodeIdx, (int)MC_MAXTEXTURES);
                fflush(stdout);
            }
            continue;
        }
        const DWORD gosHandle = tex_resolve(nodeIdx);
        if (gosHandle == 0u) continue;
        const GLuint glTex = gos_terrain_bridge_glTextureForGosHandle((unsigned)gosHandle);
        if (glTex == 0) continue;
        // Record-but-don't-break past atlas budget cap: we still want the
        // diag totals to reflect TRUE counts for tier1.  Atlas allocation
        // gated on size below to avoid memory blowup.
        // Cap = 1024 = atlas budget cap (gridSide=32, 2048x2048 = 16 MB).
        // NOT the _pad0 encoding cap (16-bit field, max 65535).  If anyone
        // ever exceeds 1024, the atlas budget needs revisiting.
        if (cementNodeIndices.size() < 1024) {
            cementSlots.push_back((int)slot);
            cementNodeIndices.push_back(nodeIdx);
            cementGLTextures.push_back(glTex);
        } else {
            truncated = true;
        }
    }

    // CEMENT_DIAG summary — always emit when trace on, regardless of cap.
    if (traceOn()) {
        printf("[CEMENT_DIAG] summary mission=%s total_cement=%ld base=%ld transitions=%ld nextAvailable=%ld firstTransition=%ld\n",
               (::missionName[0] ? ::missionName : "unknown"),
               diagTotal, diagBase, diagTransition,
               lastSlot, firstTransition);
        fflush(stdout);
    }

    const int N = (int)cementNodeIndices.size();
    // Always-on summary — not trace-gated so the count is visible without MC2_TERRAIN_INDIRECT_TRACE.
    printf("[CEMENT_ATLAS v1] event=build_result N=%d diagTotal=%ld mission=%s\n",
           N, diagTotal, (::missionName[0] ? ::missionName : "unknown"));
    fflush(stdout);
    if (N == 0) {
        if (traceOn()) printf("[TERRAIN_INDIRECT v1] event=cement_atlas_skip reason=no_cement_tiles count=0\n");
        return;
    }

    // Build nodeIdx → layer-index map.
    memset(g_cementLayerIndexByNodeIdx, 0xFF, sizeof(g_cementLayerIndexByNodeIdx));
    for (int k = 0; k < N; ++k) {
        g_cementLayerIndexByNodeIdx[cementNodeIndices[k]] = (uint16_t)k;
    }

    // Build slot → layer-index map.  Slot is the STABLE key (nodeIdx mutates
    // per-frame per memory/mc2_texture_handle_is_live.md).  This is the lookup
    // the per-frame packer uses; the nodeIdx map is kept for one-commit dead-code.
    memset(g_cementLayerIndexBySlot, 0xFF, sizeof(g_cementLayerIndexBySlot));
    for (int k = 0; k < N; ++k) {
        if (cementSlots[k] >= 0 && cementSlots[k] < MC_MAX_TERRAIN_TXMS) {
            g_cementLayerIndexBySlot[cementSlots[k]] = (uint16_t)k;
        }
    }

    // Grid: smallest power-of-2 side fitting N cells in a square.
    int gridSide = 1;
    while (gridSide * gridSide < N) gridSide <<= 1;
    const int atlasPixelSide = gridSide * txmSize;

    std::vector<uint32_t> atlasBuf((size_t)atlasPixelSide * atlasPixelSide, 0u);
    std::vector<uint32_t> tileBuf((size_t)txmSize * txmSize, 0u);

    // Save GL state (V24).
    GLint savedActive = GL_TEXTURE0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &savedActive);
    glActiveTexture(GL_TEXTURE0);
    GLint savedTex0Binding = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTex0Binding);
    GLint savedPackAlign = 4;
    glGetIntegerv(GL_PACK_ALIGNMENT, &savedPackAlign);
    GLint savedUnpackAlign = 4;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &savedUnpackAlign);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    for (int k = 0; k < N; ++k) {
        MC2_GL_BindTexture(GL_TEXTURE_2D, cementGLTextures[k]);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_BGRA, GL_UNSIGNED_BYTE, tileBuf.data());

        const int col  = k % gridSide;
        const int row  = k / gridSide;
        const int dstX = col * txmSize;
        const int dstY = row * txmSize;
        for (int py = 0; py < txmSize; ++py) {
            const uint32_t* srcRow = &tileBuf[(size_t)py * txmSize];
            uint32_t*       dstRow = &atlasBuf[(size_t)(dstY + py) * atlasPixelSide + dstX];
            memcpy(dstRow, srcRow, (size_t)txmSize * sizeof(uint32_t));
        }
    }

    // TEX-CLASS: asset-pool -- cement/road overlay atlas (content)
    if (g_cementAtlasGLTex == 0) glGenTextures(1, &g_cementAtlasGLTex);
    MC2_GL_BindTexture(GL_TEXTURE_2D, g_cementAtlasGLTex);
    MC2_GL_TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                 atlasPixelSide, atlasPixelSide, 0,
                 GL_BGRA, GL_UNSIGNED_BYTE, atlasBuf.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);  // no mips — see header comment
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    // Restore state (V24).
    glPixelStorei(GL_PACK_ALIGNMENT, savedPackAlign);
    glPixelStorei(GL_UNPACK_ALIGNMENT, savedUnpackAlign);
    MC2_GL_BindTexture(GL_TEXTURE_2D, (GLuint)savedTex0Binding);
    glActiveTexture((GLenum)savedActive);

    g_cementAtlasGridSide    = gridSide;
    g_cementAtlasTileCount   = N;
    g_cementCatalogTruncated = truncated ? 1 : 0;
    g_cementLayerMapReady    = true;

    // REGISTRY-TERRAIN-SSBO-1: register the cement catalog atlas (observe-only).
    {
        RenderCore::RenderResourceDesc d;
        d.id        = RenderCore::RenderResourceId::CementAtlas;
        d.kind      = RenderCore::RenderResourceKind::Texture2D;
        d.lifetime  = RenderCore::RenderResourceLifetime::Mission;  // rebuilt per mission load
        d.format    = RenderCore::RenderResourceFormat::RGBA8;
        d.debugName = "CementAtlas";
        d.width     = static_cast<uint32_t>(atlasPixelSide);
        d.height    = static_cast<uint32_t>(atlasPixelSide);
        d.glName    = static_cast<uint32_t>(g_cementAtlasGLTex);
        d.sizeBytes = static_cast<uint64_t>(atlasPixelSide) * static_cast<uint64_t>(atlasPixelSide) * 4ull;
        d.valid     = true;
        RenderCore::registerOrUpdateRenderResource(d);
    }

    if (traceOn()) {
        printf("[TERRAIN_INDIRECT v1] event=cement_catalog_built tile_count=%d "
               "atlas_size=%dx%d grid_side=%d gltex=%u truncated=%d "
               "unmapped_pack_count=%u\n",
               N, atlasPixelSide, atlasPixelSide, gridSide,
               (unsigned)g_cementAtlasGLTex,
               g_cementCatalogTruncated,
               g_cementPackUnmappedCount);
        if (truncated) {
            printf("[TERRAIN_INDIRECT v1] event=cement_catalog_truncated count=1024\n");
        }
        fflush(stdout);
    }

    // Stage B: build procedural 14-layer transition mask array alongside the atlas.
    BuildTransitionMaskArray();
}

}  // anonymous namespace (atlas helpers)

namespace gos_terrain_indirect {

}  // namespace gos_terrain_indirect

// Bridge accessors — declared extern in gameos_graphics.cpp.
GLuint gos_terrain_indirect_getAtlasGLTex()            { return g_atlasGLTex; }
float  gos_terrain_indirect_getNumTexturesAcross()     { return g_atlasNumTexturesAcross; }
float  gos_terrain_indirect_getAtlasMapTopLeftX()      { return g_atlasMapTopLeftX; }
float  gos_terrain_indirect_getAtlasMapTopLeftY()      { return g_atlasMapTopLeftY; }
float  gos_terrain_indirect_getAtlasOneOverWorldUnits(){ return g_atlasOneOverWorldUnits; }

GLuint gos_terrain_indirect_getCementAtlasGLTex()    { return g_cementAtlasGLTex; }
int    gos_terrain_indirect_getCementAtlasGridSide() { return g_cementAtlasGridSide; }
bool   gos_terrain_indirect_isCementAtlasReady()     { return g_cementLayerMapReady && g_cementAtlasGLTex != 0; }
float  gos_terrain_indirect_getWorldUnitsPerVertex() { return Terrain::worldUnitsPerVertex; }

GLuint gos_terrain_indirect_getTransitionMaskArrayGL() { return g_transitionMaskArrayGL; }
bool   gos_terrain_indirect_isTransitionMaskReady()    { return g_transitionMaskReady && g_transitionMaskArrayGL != 0; }

// ---------------------------------------------------------------------------
// Stage 2 public API
// ---------------------------------------------------------------------------

namespace gos_terrain_indirect {

void BuildDenseRecipe() {
    ZoneScopedN("Terrain::IndirectRecipeBuild");
    if (!Terrain::mapData) return;

    g_recipeMapSide = Terrain::realVerticesMapSide;
    const size_t N  = (size_t)g_recipeMapSide * (size_t)g_recipeMapSide;

    g_denseRecipes.assign(N, TerrainQuadRecipe{});
    g_denseRecipeDirty.assign(N, false);
    g_denseRecipeAnyDirty        = false;
    g_recipeReady                = false;
    s_firstDrawPrintedThisMission = false;

    // BuildDenseRecipe is called AFTER mapData->buildTerrainFaceCache (terrain.cpp:585),
    // so the Shape C cache is populated and getTerrainFaceCacheEntry returns valid entries.
    for (int32_t vn = 0; vn < (int32_t)N; ++vn) {
        buildRecipeSlot(vn, g_denseRecipes[vn]);
    }

    // Upload the merged colormap atlas for the indirect draw bridge.
    // Must run after recipe build so terrainTextures2 is ready.
    BuildColormapAtlas();

    // Build cement catalog atlas via GPU readback — populates g_cementLayerIndexBySlot.
    BuildCementCatalogAtlas();

    // Bake cementWord into _wp3 now that g_cementLayerIndexBySlot is ready.
    // Must run BEFORE glBufferData so the cement words are included in the initial
    // GPU upload and don't require a FlushDirtyRecipeSlotsToGPU on frame 1.
    PopulateRecipeCementWords();

    // Full GPU upload on mission load — includes _wp3 cement words from above.
    // TIER2-EXCLUDED: dead-path
    if (g_recipeSSBO == 0) glGenBuffers(1, &g_recipeSSBO);
    // Pre-upload audit: dumps how many recipes have a non-zero _wp3 (i.e.,
    // valid cement word) in the CPU memory glBufferData is about to upload.
    // Gated on MC2_TERRAIN_INDIRECT_TRACE; left in-tree as a tripwire because
    // a mismatch between this count and the subsequent `cement_words_baked`
    // count (also gated) is the canary for cement-bake regressions like the
    // InvalidateAllRecipes bug that motivated this audit.
    if (traceOn()) {
        long preUploadNonzero = 0;
        uint32_t firstNonzeroWp3 = 0;
        size_t firstNonzeroVn = (size_t)-1;
        for (size_t vn = 0; vn < N; ++vn) {
            uint32_t w;
            memcpy(&w, &g_denseRecipes[vn]._wp3, 4);
            if (w != 0u) {
                ++preUploadNonzero;
                if (firstNonzeroVn == (size_t)-1) {
                    firstNonzeroVn = vn;
                    firstNonzeroWp3 = w;
                }
            }
        }
        printf("[CEMENT_DIAG] event=pre_upload_wp3_audit "
               "nonzero=%ld firstVn=%lld firstWp3=0x%08x sizeof_recipe=%zu wp3_offset=%zu\n",
               preUploadNonzero, (long long)firstNonzeroVn, firstNonzeroWp3,
               sizeof(TerrainQuadRecipe),
               (size_t)((const char*)&g_denseRecipes[0]._wp3 - (const char*)&g_denseRecipes[0]));
        fflush(stdout);
    }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_recipeSSBO);
    MC2_GL_BufferData(GL_SHADER_STORAGE_BUFFER,
                 (GLsizeiptr)(N * sizeof(TerrainQuadRecipe)),
                 g_denseRecipes.data(),
                 GL_DYNAMIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    // REGISTRY-TERRAIN-SSBO-1: register the dense recipe SSBO (observe-only).
    {
        RenderCore::RenderResourceDesc d;
        d.id        = RenderCore::RenderResourceId::TerrainRecipeBuffer;
        d.kind      = RenderCore::RenderResourceKind::Buffer;
        d.lifetime  = RenderCore::RenderResourceLifetime::Mission;  // rebuilt per mission load
        d.format    = RenderCore::RenderResourceFormat::BufferRaw;
        d.debugName = "TerrainRecipeBuffer";
        d.glName    = static_cast<uint32_t>(g_recipeSSBO);
        d.sizeBytes = static_cast<uint64_t>(N) * sizeof(TerrainQuadRecipe);
        d.valid     = true;
        RenderCore::registerOrUpdateRenderResource(d);
    }

    // Clear dirty flags — data already in GPU from glBufferData above.
    g_denseRecipeDirty.assign(N, false);
    g_denseRecipeAnyDirty = false;

    g_recipeReady = true;

    if (traceOn()) {
        printf("[TERRAIN_INDIRECT v1] event=recipe_build mapSide=%d entries=%zu "
               "bytes=%zu ssbo=%u\n",
               g_recipeMapSide, N, N * sizeof(TerrainQuadRecipe),
               (unsigned)g_recipeSSBO);
        fflush(stdout);
    }

    // Collect unique terrain texture nodeIds for per-frame LUT upload.
    CollectUniqueNodeIds();
}

void ResetDenseRecipe() {
    ZoneScopedN("Terrain::IndirectRecipeReset");

    if (traceOn()) {
        printf("[TERRAIN_INDIRECT v1] event=recipe_reset ssbo=%u\n",
               (unsigned)g_recipeSSBO);
        fflush(stdout);
    }

    g_denseRecipes.clear();
    g_denseRecipeDirty.clear();
    // MAJOR-1 (Approach A): the camera-window staging + its monotonic high-
    // water mark are per-mission state — clear them here alongside the recipe
    // reset.  Without this, g_solidWindowMaxSeen carries a previous (larger)
    // map's mark into a smaller map's reserve, and stale staging contents
    // could be uploaded on the first frame before the slim loop runs.
    g_solidWindowStaging.clear();
    g_solidWindowMaxSeen         = 0;
    g_denseRecipeAnyDirty        = false;
    g_recipeMapSide              = 0;
    g_recipeReady                = false;
    s_firstDrawPrintedThisMission = false;
    // g_recipeSSBO stays allocated — reused by next mission's BuildDenseRecipe.
    // Mirrors WaterStream::Reset() pattern.

    // Tear down the atlas GL texture (per-mission; rebuilt by BuildColormapAtlas).
    if (g_atlasGLTex != 0) {
        glDeleteTextures(1, &g_atlasGLTex);
        g_atlasGLTex = 0;
    }
    g_atlasSize              = 0;
    g_atlasNumTexturesAcross = 0.f;
    g_atlasMapTopLeftX       = 0.f;
    g_atlasMapTopLeftY       = 0.f;
    g_atlasOneOverWorldUnits = 0.f;
    // Clear stale nodeIds so the GPU path guard doesn't fire from a prior mission.
    g_uniqueTerrainNodeIds.clear();

    // Cement catalog atlas teardown — mirror g_atlasGLTex pattern.
    if (g_cementAtlasGLTex != 0) {
        glDeleteTextures(1, &g_cementAtlasGLTex);
        g_cementAtlasGLTex = 0;

        // REGISTRY-TERRAIN-SSBO-1: mark the cement atlas slot unavailable on teardown.
        RenderCore::RenderResourceDesc invalid;
        invalid.id = RenderCore::RenderResourceId::CementAtlas;
        RenderCore::registerOrUpdateRenderResource(invalid);
    }
    g_cementAtlasGridSide    = 0;
    g_cementAtlasTileCount   = 0;
    g_cementLayerMapReady    = false;
    g_cementCatalogTruncated = 0;
    g_cementPackUnmappedCount = 0;
    memset(g_cementLayerIndexByNodeIdx, 0xFF, sizeof(g_cementLayerIndexByNodeIdx));
    memset(g_cementLayerIndexBySlot,    0xFF, sizeof(g_cementLayerIndexBySlot));

    if (traceOn()) {
        printf("[TERRAIN_INDIRECT v1] event=cement_catalog_reset\n");
        fflush(stdout);
    }
}

bool IsDenseRecipeReady() {
    return g_recipeReady && g_recipeSSBO != 0;
}

const TerrainQuadRecipe* RecipeForVertexNum(int32_t vn) {
    if (vn < 0) return nullptr;
    if (static_cast<size_t>(vn) >= g_denseRecipes.size()) return nullptr;
    return &g_denseRecipes[vn];
}

void InvalidateRecipeForVertexNum(int32_t vn) {
    if (!IsEnabled() && !IsParityCheckEnabled()) return;
    if (g_denseRecipes.empty()) return;
    if (vn < 0 || static_cast<size_t>(vn) >= g_denseRecipes.size()) return;
    buildRecipeSlot(vn, g_denseRecipes[vn]);
    // buildRecipeSlot zeroes _wp3.  Re-bake cement words from the layer map
    // so the next FlushDirtyRecipeSlotsToGPU doesn't upload a zeroed _wp3
    // and nuke this quad's cement render.  PopulateRecipeCementWords is
    // whole-grid; cheap (~10k simple iterations) and only fires on the rare
    // invalidate events (shadow recalc, light-dir change, normal recompute).
    PopulateRecipeCementWords();
    g_denseRecipeDirty[vn] = true;
    g_denseRecipeAnyDirty  = true;
    if (traceOn()) {
        printf("[TERRAIN_INDIRECT v1] event=invalidate vn=%d\n", vn);
        fflush(stdout);
    }
}

void InvalidateAllRecipes() {
    if (!IsEnabled() && !IsParityCheckEnabled()) return;
    if (g_denseRecipes.empty()) return;
    const size_t N = g_denseRecipes.size();
    for (size_t vn = 0; vn < N; ++vn) {
        buildRecipeSlot((int32_t)vn, g_denseRecipes[vn]);
        g_denseRecipeDirty[vn] = true;
    }
    // Re-bake cement words: buildRecipeSlot zeroed _wp3 on every slot above.
    // Without this, FlushDirtyRecipeSlotsToGPU uploads zeroed _wp3 for all
    // cement quads and the GPU compute shader's read of r.pos3.w returns 0,
    // making cement pads render as bare colormap (the original cement bug).
    PopulateRecipeCementWords();
    g_denseRecipeAnyDirty = true;
    if (traceOn()) {
        printf("[TERRAIN_INDIRECT v1] event=invalidate_all entries=%zu\n", N);
        fflush(stdout);
    }
}

void FlushDirtyRecipeSlotsToGPU() {
    if (!g_denseRecipeAnyDirty || g_recipeSSBO == 0) return;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_recipeSSBO);
    const size_t N = g_denseRecipes.size();
    for (size_t vn = 0; vn < N; ++vn) {
        if (!g_denseRecipeDirty[vn]) continue;
        MC2_GL_BufferSubData(GL_SHADER_STORAGE_BUFFER,
                        (GLintptr)(vn * sizeof(TerrainQuadRecipe)),
                        sizeof(TerrainQuadRecipe),
                        &g_denseRecipes[vn]);
        g_denseRecipeDirty[vn] = false;
    }
    g_denseRecipeAnyDirty = false;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

// ---------------------------------------------------------------------------
// Stage 2 parity body
// ---------------------------------------------------------------------------
//
// Walks the live quadList, looks up the dense recipe by top-left vertexNum,
// and byte-compares recipe input fields against the legacy-equivalent values
// derived directly from the live quad vertex pointers.
//
// Skip set (mirrors water_ssbo_pattern.md):
//   - null pointer guards on q.vertices[0..3] and ->pVertex
//   - blank-vertex skip: any vertexNum < 0
//   - recipe coverage gate: RecipeForVertexNum returns nullptr → skip
//
// Compared fields: corner positions (wx*/wy*/wz*), corner normals (nx*/ny*/nz*),
// UV extents (minU/minV/maxU/maxV), _wp0 terrainType-pack bits.
// NOT compared: _wp1.._wp3, _np0.._np3 (always 0.f padding),
//               post-projection data (per water_ssbo_pattern.md — sub-ULP drift).
//
int ParityCompareRecipeFrame() {
    if (!IsParityCheckEnabled()) return 0;
    if (!g_recipeReady || g_denseRecipes.empty()) return 0;
    if (!land) return 0;

    const long total            = land->getNumQuads();
    const TerrainQuadPtr quads  = land->getQuadList();
    if (!quads || total <= 0) return 0;

    static long long s_parityFrameIdx = 0;
    const long long frame = ++s_parityFrameIdx;

    const long  mapSide  = Terrain::realVerticesMapSide;
    const long  halfSide = Terrain::halfVerticesMapSide;
    const float wupv     = Terrain::worldUnitsPerVertex;

    int quadsChecked = 0;

    for (long qi = 0; qi < total; ++qi) {
        const TerrainQuad& q = quads[qi];

        // Null-pointer guards
        if (!q.vertices[0] || !q.vertices[1] || !q.vertices[2] || !q.vertices[3]) continue;
        if (!q.vertices[0]->pVertex || !q.vertices[1]->pVertex ||
            !q.vertices[2]->pVertex || !q.vertices[3]->pVertex) continue;

        // Blank-vertex skip
        if (q.vertices[0]->vertexNum < 0 || q.vertices[1]->vertexNum < 0 ||
            q.vertices[2]->vertexNum < 0 || q.vertices[3]->vertexNum < 0) continue;

        const int32_t vn0 = (int32_t)q.vertices[0]->vertexNum;

        // Recipe coverage gate
        const TerrainQuadRecipe* rec = RecipeForVertexNum(vn0);
        if (!rec) continue;

        ++quadsChecked;

        // Derive expected corner positions from map index arithmetic
        const long  mx = vn0 % mapSide;
        const long  my = vn0 / mapSide;
        const float e_wx0 = float(mx     - halfSide) * wupv;
        const float e_wy0 = float(halfSide - my    ) * wupv;
        const float e_wx1 = float(mx + 1 - halfSide) * wupv;
        const float e_wy1 = e_wy0;
        const float e_wx2 = e_wx1;
        const float e_wy2 = float(halfSide - (my + 1)) * wupv;
        const float e_wx3 = e_wx0;
        const float e_wy3 = e_wy2;

        // Expected elevations and normals from live pVertex
        const float e_wz0 = q.vertices[0]->pVertex->elevation;
        const float e_wz1 = q.vertices[1]->pVertex->elevation;
        const float e_wz2 = q.vertices[2]->pVertex->elevation;
        const float e_wz3 = q.vertices[3]->pVertex->elevation;

        const float e_nx0 = q.vertices[0]->pVertex->vertexNormal.x;
        const float e_ny0 = q.vertices[0]->pVertex->vertexNormal.y;
        const float e_nz0 = q.vertices[0]->pVertex->vertexNormal.z;
        const float e_nx1 = q.vertices[1]->pVertex->vertexNormal.x;
        const float e_ny1 = q.vertices[1]->pVertex->vertexNormal.y;
        const float e_nz1 = q.vertices[1]->pVertex->vertexNormal.z;
        const float e_nx2 = q.vertices[2]->pVertex->vertexNormal.x;
        const float e_ny2 = q.vertices[2]->pVertex->vertexNormal.y;
        const float e_nz2 = q.vertices[2]->pVertex->vertexNormal.z;
        const float e_nx3 = q.vertices[3]->pVertex->vertexNormal.x;
        const float e_ny3 = q.vertices[3]->pVertex->vertexNormal.y;
        const float e_nz3 = q.vertices[3]->pVertex->vertexNormal.z;

        // Expected UV extents: mirror buildRecipeSlot exactly — read from the
        // terrain face cache entry (NOT from q.uvData, which is only set by the
        // Shape C hot-path and stays zero for quads that bypass it).
        // This comparison is apples-to-apples: recipe was built from the cache;
        // parity derives the expected value from the same cache.
        float e_minU = 0.5f / TERRAIN_TXM_SIZE;
        float e_maxU = 1.0f - 0.5f / TERRAIN_TXM_SIZE;
        float e_minV = e_minU;
        float e_maxV = e_maxU;
        if (Terrain::terrainTextures2 && Terrain::mapData) {
            const MapData::WorldQuadTerrainCacheEntry* entry =
                Terrain::mapData->getTerrainFaceCacheEntry(my, mx);
            if (entry && entry->isValid()) {
                const bool isCement  = entry->isCement();
                const bool noOverlay = (entry->overlayHandle == 0xffffffffu);
                if (!(noOverlay && isCement)) {
                    e_minU = entry->uvData.minU;
                    e_minV = entry->uvData.minV;
                    e_maxU = entry->uvData.maxU;
                    e_maxV = entry->uvData.maxV;
                }
            }
        }

        // Expected _wp0 (terrainType pack)
        const uint32_t e_m0 = terrainTypeToMaterialLocal(q.vertices[0]->pVertex->terrainType);
        const uint32_t e_m1 = terrainTypeToMaterialLocal(q.vertices[1]->pVertex->terrainType);
        const uint32_t e_m2 = terrainTypeToMaterialLocal(q.vertices[2]->pVertex->terrainType);
        const uint32_t e_m3 = terrainTypeToMaterialLocal(q.vertices[3]->pVertex->terrainType);
        const uint32_t e_tpacked = e_m0 | (e_m1 << 8) | (e_m2 << 16) | (e_m3 << 24);

        uint32_t g_tpacked = 0;
        memcpy(&g_tpacked, &rec->_wp0, 4);

        // Helper: bit-cast float to uint32 for exact mismatch comparison
        // (avoids NaN != NaN false positives and keeps hex output informative).
#define FCMP(fname, got_f, exp_f) \
        do { \
            uint32_t _g = 0, _e = 0; \
            float _gf = (got_f), _ef = (exp_f); \
            memcpy(&_g, &_gf, 4); memcpy(&_e, &_ef, 4); \
            if (_g != _e) { \
                ParityPrintMismatch((int)frame, (int)qi, "recipe", 0, 0, fname, _e, _g); \
            } \
        } while(0)

        FCMP("wx0", rec->wx0, e_wx0);
        FCMP("wy0", rec->wy0, e_wy0);
        FCMP("wz0", rec->wz0, e_wz0);
        FCMP("wx1", rec->wx1, e_wx1);
        FCMP("wy1", rec->wy1, e_wy1);
        FCMP("wz1", rec->wz1, e_wz1);
        FCMP("wx2", rec->wx2, e_wx2);
        FCMP("wy2", rec->wy2, e_wy2);
        FCMP("wz2", rec->wz2, e_wz2);
        FCMP("wx3", rec->wx3, e_wx3);
        FCMP("wy3", rec->wy3, e_wy3);
        FCMP("wz3", rec->wz3, e_wz3);

        FCMP("nx0", rec->nx0, e_nx0);
        FCMP("ny0", rec->ny0, e_ny0);
        FCMP("nz0", rec->nz0, e_nz0);
        FCMP("nx1", rec->nx1, e_nx1);
        FCMP("ny1", rec->ny1, e_ny1);
        FCMP("nz1", rec->nz1, e_nz1);
        FCMP("nx2", rec->nx2, e_nx2);
        FCMP("ny2", rec->ny2, e_ny2);
        FCMP("nz2", rec->nz2, e_nz2);
        FCMP("nx3", rec->nx3, e_nx3);
        FCMP("ny3", rec->ny3, e_ny3);
        FCMP("nz3", rec->nz3, e_nz3);

        FCMP("minU", rec->minU, e_minU);
        FCMP("minV", rec->minV, e_minV);
        FCMP("maxU", rec->maxU, e_maxU);
        FCMP("maxV", rec->maxV, e_maxV);

        if (g_tpacked != e_tpacked) {
            ParityPrintMismatch((int)frame, (int)qi, "recipe", 0, 0,
                                "_wp0", e_tpacked, g_tpacked);
        }

#undef FCMP
    }

    return quadsChecked;
}

}  // namespace gos_terrain_indirect  // Stage 2 block

// ---------------------------------------------------------------------------
// Stage 3 — per-frame thin-record packer, indirect-command builder,
//            preflight arming, and DrawIndirect thin executor.
//
// Bridge function (gos_terrain_bridge_drawIndirect) lives in gameos_graphics.cpp
// where gosRenderer state is accessible.  This file owns the CPU-side logic;
// the bridge is called by DrawIndirect() after arming.
// ---------------------------------------------------------------------------

// Forward-declare the bridge (defined in gameos_graphics.cpp).
// Signature uses unsigned int (not GLuint) to match gos_terrain_bridge.h
// without pulling in GL headers there.
bool gos_terrain_bridge_drawIndirect(int cmdCount, unsigned int recipeSSBO,
                                     unsigned int thinRecordSSBO,
                                     unsigned int indirectCmdBuffer);

// Include pVertex via PostcompVertex — already available through mapdata.h.
// (terrain.h already included above, which pulls mapdata.h.)
#include "../../mclib/vertex.h"     // ScreenVertex, vertexNum
// tex_resolve — lazy per-frame memoization. Header includes txmmgr.h.
#include "../../mclib/tex_resolve_table.h"

// TERRAIN_DEPTH_FUDGE for the per-tri pz check: single source of truth is
// mclib/terrain_depth_bias.h (was a re-stated local constexpr; the old
// #ifndef guard defended against a quad.cpp MACRO collision that the
// constexpr header eliminates). Consumer below uses it unqualified.
#include "../../mclib/terrain_depth_bias.h"
using mc2depth::TERRAIN_DEPTH_FUDGE;

namespace {

// ---------------------------------------------------------------------------
// Thin-record SSBO (triple-buffered, GPU_STREAM_DRAW).
// Mirrors the M2 thin-record ring in gos_terrain_patch_stream.cpp.
// ---------------------------------------------------------------------------
static constexpr int    kThinRingFrames    = 3;
static constexpr size_t kMaxThinRecords    = 65536u;  // ≥ max visible quads
static constexpr size_t kThinRecordBytes   = kMaxThinRecords * sizeof(TerrainQuadThinRecord);

static GLuint g_thinRecordSSBO              = 0;
static int    g_thinRingSlot                = 0;
static GLsync g_thinRingFences[kThinRingFrames] = { 0, 0, 0 };

// Indirect command buffer — driver-only, GL_DRAW_INDIRECT_BUFFER.
// Sized for 16 commands (256 B); PR1 emits exactly 1.
struct DrawArraysIndirectCommand {
    GLuint count;
    GLuint instanceCount;
    GLuint first;
    GLuint baseInstance;
};
static_assert(sizeof(DrawArraysIndirectCommand) == 16,
    "DrawArraysIndirectCommand is 4 GLuints = 16 B per GL spec");

static constexpr size_t kIndirectCmdBufferBytes = 16 * sizeof(DrawArraysIndirectCommand);
static GLuint g_indirectCmdBuffer = 0;

// Per-frame arming state (reset each frame by ComputePreflight).
static bool  s_frameSolidArmed           = false;
static int   s_frameSolidPackedThinCount = 0;
static int   s_frameSolidCmdCount        = 0;

// Process-sticky hard-failure latch.
static bool  s_processArmingDisabled     = false;

// Process-sticky "intro pan complete observed" latch. Set on the first
// frame where ComputePreflight() arms (path=gpu or path=cpu); never
// resets within a process. Consumed by VisualDiff::onFrameTick to gate
// frame counting on the engine's own intro-complete signal -- the
// IsFrameSolidArmed() per-frame flag is reset by gosRenderer::endFrame()
// before VisualDiff's post-PP hook fires, so a sticky cousin is needed.
static bool  s_everFrameSolidArmed       = false;

// first_draw lifecycle latch (reset by ResetDenseRecipe / mission teardown).
// Declared extern in the anonymous ns of the Stage 2 block; re-stated here
// via file-scope bool below.  Use the existing s_firstDrawPrintedThisMission
// that's already declared in the Stage 2 anonymous namespace above.

// ResourcesReady lazy-alloc state.
static bool  s_resourcesAllocated = false;
static bool  s_resourcesReady     = false;

// ---------------------------------------------------------------------------
// Phase C Stage 2 — SOLID GPU compute resources.
// Lazy-allocated on first ComputeDispatch(); destroyed on ResetDenseRecipe.
// ---------------------------------------------------------------------------
static GLuint  g_solidComputeProgram    = 0;
// Step 2b (VPL retirement): cmd-patch program retired.  Primary compute is
// the sole writer of cmds[0].count via atomicAdd.  Bucket-header SSBO is
// demoted behind MC2_BUCKET_HEADER_TRACE=1 (default-off) per plan
// amendment C1; when off the SSBO is not allocated, not bound, not cleared,
// and no diagnostic atomicAdds run in the shader.
static GLuint  g_solidBucketHeaderSsbo  = 0;
static GLuint  g_terrainHandleLutSSBO   = 0;
// Camera-windowed solid dispatch (Approach A) state — DECLARED EARLY (next to
// g_denseRecipes) so the per-mission reset in ResetDenseRecipe() can clear it
// (MAJOR-1).  Definitions live near g_denseRecipes; see the comment there.
static GLuint  g_thinCanarySSBO         = 0;   // probe 6: separate buffer, never bound by bridge
// Probe 8: MVP fingerprint at compute dispatch time, read by bridge at draw time.
static uint32_t g_dispatchMvpFp      = 0;
static uint64_t g_dispatchMvpFrameIdx = 0;
// RENDER-VIEW-CURRENCY-1: the VIEW EPOCH (g_mvpDiagFrame, bumped on every
// gos_SetWorldToClipGL = every authoritative camera-matrix publish) observed at
// the moment this snapshot's MVP was sourced. Object/mech consumers compare this
// against the CURRENT g_mvpDiagFrame to reject a snapshot taken under a different
// camera — even within the same engine frame (e.g. an early publish followed by a
// mid-frame zoom-anchor camera change). -1 = never published. Engine-frame is NOT
// sufficient here: same frame, different camera must be detectable.
static long     g_dispatchMvpViewEpoch = -1;
// Probe 8b: verification — also stash first 4 floats from compute-time MVP.
// Bridge logs both sets on mismatch so we can see byte-level difference.
static float    g_dispatchMvpFloats[4] = { 0, 0, 0, 0 };
// Water-consistency fix (2026-05-17): full 16-float snapshot of the MVP
// terrain-solid baked its Fix-B clipPos with THIS frame. The water compute
// reads this (via gos_terrain_indirect_getDispatchMvp16) so the drawn water
// is bit-consistent with the drawn terrain. [WATER_DEPTHPROBE v1] proved an
// exact 1-frame divergence (terrain-solid stale vs water fresh) was the
// shoreline recede/flicker/vanish root cause. ring_slot_state_must_travel_
// with_slot.md. Unconditional 64 B/frame; written only when ComputeDispatch
// actually runs (terrain-solid armed), so a consumer that gates on
// IsFrameSolidArmed() always sees this-frame-fresh data.
static float    g_dispatchMvp16[16] = { 0 };

// Fix A (2026-05-14): per-ring-slot MVP stash for the intentional 1-frame
// compute->bridge lag.  Compute writes thin records to slot S using MVP_X;
// the bridge consumes slot S one frame later when terrain_mvp_ has rotated
// to MVP_Y.  Without this snapshot the VS projects records with MVP_Y while
// their pzOk gates assume MVP_X — producing the giant grey-banded terrain
// triangle under fast camera rotation documented in
// docs/superpowers/progress/2026-05-14-raster-triangle-handoff.md.
// Both the GPU path (ComputeDispatch) and the CPU path (PackThinRecordsForFrame)
// stash here for invariant uniformity, even though the CPU path is not lagged.
//
// VPL retirement step 9 (2026-05-15): DEMOTED behind MC2_RING_TRACE.  The VPL
// body was deleted in step 8c-2 (commit 63c023f) and Fix B re-homed projection
// into tr.clipPos[] (the thin VS no longer declares a terrainMVP uniform —
// shaders/gos_terrain_thin.vert:56-61), so this per-slot MVP snapshot is no
// longer load-bearing defense-in-depth.  Per the Debug-instrumentation rule
// (demote-not-silently-delete; memory/debug_instrumentation_rule.md) the array
// + the Probe 8 [RING_MVP_DELTA v1] snapshot/compare path are KEPT but gated:
// when g_envRingTrace is false the writers below are skipped and the
// gos_terrain_indirect_getRingSlotMvp() accessor returns nullptr, so the
// snapshot does zero per-frame work.  Net cost when off: ~192 bytes of static
// state.  Re-armable for any future temporal-misalignment regression via
// MC2_RING_TRACE=1.  See docs/superpowers/plans/
// 2026-05-14-vertex-project-loop-retirement.md §"Step 9" and
// memory/ring_slot_state_must_travel_with_slot.md (Fix B is the surviving
// instance of that rule; Fix A's snapshot is the retired mechanism).
static const bool g_envRingTrace = (getenv("MC2_RING_TRACE") != nullptr);
static float    g_thinSlotMVP[kThinRingFrames][16] = { { 0 } };
static bool     g_thinSlotMVPValid[kThinRingFrames] = { false, false, false };

// Cached uniform locations — populated once when programs are compiled.
// Per-frame varying uniforms (u_windowCount, u_terrainMVP, u_alphaOverride)
// are still uploaded every frame; only the locations themselves are cached.
static GLint g_locSolidWC  = -1;   // u_windowCount   (per-frame)
static GLint g_locSolidAO  = -1;   // u_alphaOverride (per-frame, may be absent)
static GLint g_locSolidMVP = -1;   // u_terrainMVP    (per-frame)
// Step 2b (VPL retirement): cmd-patch uniform-location caches retired
// (g_locCmdVPE, g_locCmdCC removed) — cmd-patch program is no longer compiled.
// u_bucketHeaderTrace gates the demoted bucket-header SSBO writes in the
// primary compute shader (MC2_BUCKET_HEADER_TRACE).
static GLint g_locSolidBHT = -1;   // u_bucketHeaderTrace (per-frame int gate)
static GLint g_locSolidUW  = -1;   // u_useWindow (per-frame: 1=windowed, 0=full-range identity)
static GLint g_locSolidMS  = -1;   // u_mapSide (PER-FRAME: changes when a different-size map loads)
// WATER-REFLECTION-CLIP-1: u_reflectionPass (per-frame int gate; see .comp for
// rationale). Set true only for the duration of RenderWaterReflectionPass()'s
// ComputeDispatch() call, false otherwise -- so the main SOLID dispatch is
// unaffected (byte-identical pzOk behavior).
static GLint g_locSolidReflPass = -1;
static bool  s_solidReflectionPassActive = false;


// Flag: whether ComputeDispatch() ran the GPU path this frame.
static bool s_solidGpuDispatchRanThisFrame = false;

// ---------------------------------------------------------------------------
// ResourcesReady() — lazy-allocate thin-record SSBO + indirect buffer.
// Called from ComputePreflight; returns true once both are allocated.
// ---------------------------------------------------------------------------
static bool ResourcesReady() {
    if (s_resourcesReady) {
        // Between-mission check: atlas is deleted by ResetDenseRecipe but
        // s_resourcesReady isn't visible from that public function. Re-arm
        // the lazy setup path so the atlas guard re-runs next mission.
        if (g_atlasGLTex == 0) {
            s_resourcesReady     = false;
            s_resourcesAllocated = false;
        } else {
            return true;
        }
    }
    if (s_resourcesAllocated) return false;  // already tried and failed

    // Thin-record SSBO: triple-buffered, GL_STREAM_DRAW.
    if (g_thinRecordSSBO == 0) {
        // TIER2-EXCLUDED: dead-path
        glGenBuffers(1, &g_thinRecordSSBO);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_thinRecordSSBO);
        MC2_GL_BufferData(GL_SHADER_STORAGE_BUFFER,
                     (GLsizeiptr)(kThinRingFrames * kThinRecordBytes),
                     nullptr, GL_STREAM_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        if (g_thinRecordSSBO == 0) {
            if (traceOn()) {
                fprintf(stderr, "[TERRAIN_INDIRECT v1] event=resources_alloc_fail reason=thin_ssbo\n");
                fflush(stderr);
            }
            s_resourcesAllocated = true;
            return false;
        }

        // REGISTRY-TERRAIN-SSBO-1: register the triple-buffered thin-record SSBO
        // (observe-only metadata; never read by the draw path).
        {
            RenderCore::RenderResourceDesc d;
            d.id        = RenderCore::RenderResourceId::TerrainThinBuffer;
            d.kind      = RenderCore::RenderResourceKind::Buffer;
            d.lifetime  = RenderCore::RenderResourceLifetime::Mission;  // rebuilt per mission load (triple-buffered thin records)
            d.format    = RenderCore::RenderResourceFormat::BufferRaw;
            d.debugName = "TerrainThinBuffer";
            d.glName    = static_cast<uint32_t>(g_thinRecordSSBO);
            d.sizeBytes = static_cast<uint64_t>(kThinRingFrames) * static_cast<uint64_t>(kThinRecordBytes);
            d.valid     = true;
            RenderCore::registerOrUpdateRenderResource(d);
        }
    }

    // Indirect command buffer.
    if (g_indirectCmdBuffer == 0) {
        // TIER2-EXCLUDED: dead-path
        glGenBuffers(1, &g_indirectCmdBuffer);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, g_indirectCmdBuffer);
        // Step 2b (VPL retirement, C2): pre-initialize {count=0,
        // instanceCount=1, first=0, baseInstance=0} once at allocation.
        // The primary compute touches only cmds[0].count (via atomicAdd) so
        // the other three fields must be correct at allocation.  Per-frame
        // count clear is done in ComputeDispatch via glClearBufferSubData of
        // the 4-byte count slot before the primary dispatch fires.
        DrawArraysIndirectCommand initCmd[16];
        for (size_t i = 0; i < 16; ++i) {
            initCmd[i].count         = 0;
            initCmd[i].instanceCount = 1;
            initCmd[i].first         = 0;
            initCmd[i].baseInstance  = 0;
        }
        MC2_GL_BufferData(GL_DRAW_INDIRECT_BUFFER,
                     (GLsizeiptr)kIndirectCmdBufferBytes,
                     initCmd, GL_STREAM_DRAW);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
        if (g_indirectCmdBuffer == 0) {
            if (traceOn()) {
                fprintf(stderr, "[TERRAIN_INDIRECT v1] event=resources_alloc_fail reason=indirect_buf\n");
                fflush(stderr);
            }
            s_resourcesAllocated = true;
            return false;
        }
    }

    // Atlas guard: refuse to arm if BuildColormapAtlas didn't produce a texture.
    // This keeps ResourcesReady() idempotent-false until BuildDenseRecipe has run
    // (and thus BuildColormapAtlas has run). Without this, ComputePreflight could
    // arm and the bridge would draw with no texture bound — reproducing the grey bug.
    if (g_atlasGLTex == 0) {
        if (traceOn()) {
            fprintf(stderr, "[TERRAIN_INDIRECT v1] event=resources_not_ready reason=atlas_not_built\n");
            fflush(stderr);
        }
        // Don't mark s_resourcesAllocated — retry next frame once atlas is ready.
        return false;
    }

    s_resourcesAllocated = true;
    s_resourcesReady     = true;
    if (traceOn()) {
        fprintf(stderr,
            "[TERRAIN_INDIRECT v1] event=resources_ready "
            "thinSSBO=%u indirectBuf=%u ringFrames=%d maxThin=%zu atlasGLTex=%u\n",
            (unsigned)g_thinRecordSSBO, (unsigned)g_indirectCmdBuffer,
            kThinRingFrames, kMaxThinRecords, (unsigned)g_atlasGLTex);
        fflush(stderr);
    }
    return true;
}

// ---------------------------------------------------------------------------
// InMissionTransition() — simple stub; returns false until needed.
// Stage 4's quintuple soak gate validates this boundary.
// ---------------------------------------------------------------------------
static inline bool InMissionTransition() { return false; }

// ---------------------------------------------------------------------------
// PackThinRecordsForFrame() — walks live quadList, packs thin-record SSBO
// ring slot for this frame.  Returns count of packed quads (≥ 0).
//
// Skip set (per memory/water_ssbo_pattern.md):
//   1. Pointer guards: q.vertices[0..3] and ->pVertex null check
//   2. Map-edge blank-vertex: vertexNum < 0 sentinel
//   3. Recipe coverage: RecipeForVertexNum returns nullptr
//   4. Per-tri pz check (drives flags bits 1, 2)
// ---------------------------------------------------------------------------
static int PackThinRecordsForFrame() {
    // VPL retirement step 1 (2026-05-14): CPU thin-record pack demoted behind
    // MC2_TERRAIN_INDIRECT_CPU_FALLBACK. Default-off — the GPU compute path in
    // shaders/gpu_driven_terrain_solid.comp is the sole projection authority
    // post Fix-B (commit 005ebc7). The CPU body is retained as the GPU-arm-
    // failure safety net per memory/stock_install_must_remain_playable.md;
    // future plan retires the declaration itself once telemetry confirms the
    // env-gate is never set in production. See
    // docs/superpowers/plans/2026-05-14-cpu-pack-retirement.md §3.
    static const bool s_cpuFallback =
        (getenv("MC2_TERRAIN_INDIRECT_CPU_FALLBACK") != nullptr);
    if (!s_cpuFallback) {
        // One-shot demotion log so smoke artifacts capture the gate state.
        static bool s_loggedDemote = false;
        if (!s_loggedDemote) {
            s_loggedDemote = true;
            printf("[TERRAIN_INDIRECT v1] event=cpu_pack_demoted "
                   "path=stock_solid gate=MC2_TERRAIN_INDIRECT_CPU_FALLBACK\n");
            fflush(stdout);
        }
        return 0;  // no records this frame — indirect pipeline disarms
    }

    ZoneScopedN("Terrain::ThinRecordPack");

    // Diagnostic Test 1 — reset per-frame cement classification counters.
    g_cementMappedThisFrame       = 0;
    g_concreteAllCornersThisFrame = 0;

    if (!land) return 0;
    const long total          = land->getNumQuads();
    const TerrainQuadPtr quads = land->getQuadList();
    if (!quads || total <= 0) return 0;

    // Advance ring slot — wait on the fence for this slot before overwriting.
    g_thinRingSlot = (g_thinRingSlot + 1) % kThinRingFrames;
    if (g_thinRingFences[g_thinRingSlot]) {
        glClientWaitSync(g_thinRingFences[g_thinRingSlot],
                         GL_SYNC_FLUSH_COMMANDS_BIT, 10000000u /* 10ms */);
        glDeleteSync(g_thinRingFences[g_thinRingSlot]);
        g_thinRingFences[g_thinRingSlot] = 0;
    }

    // Fix A: snapshot current MVP for this slot.  The CPU pack path is not
    // frame-lagged (records are produced and drawn in the same frame), so this
    // stash equals the bridge's terrain_mvp_ at draw time — the override is a
    // no-op for CPU-packed slots.  Stash anyway to keep the "every slot has
    // a valid MVP" invariant uniform across CPU and GPU paths.
    // Step 9 (2026-05-15): demoted behind MC2_RING_TRACE — skipped by default.
    if (g_envRingTrace) {
        if (const float* curMvp = gos_GetTerrainMVPMat4()) {
            memcpy(g_thinSlotMVP[g_thinRingSlot], curMvp, sizeof(float) * 16);
            g_thinSlotMVPValid[g_thinRingSlot] = true;
        }
    }

    // Stage area: up to kMaxThinRecords records into a stack-local shadow.
    // glBufferSubData one shot at the end.
    static TerrainQuadThinRecord s_shadow[kMaxThinRecords];
    int packed = 0;

    for (long qi = 0; qi < total && (size_t)packed < kMaxThinRecords; ++qi) {
        const TerrainQuad& q = quads[qi];

        // 1. Pointer guards (pVertex not required here — we only need
        //    vertexNum + pz for the skip set; lightRGB comes from
        //    vertices[c]->lightRGB which only requires ScreenVertex).
        if (!q.vertices[0] || !q.vertices[1] ||
            !q.vertices[2] || !q.vertices[3]) continue;

        // 2. Map-edge blank-vertex skip (vertexNum == -1 sentinel)
        const int32_t vn0 = q.vertices[0]->vertexNum;
        if (vn0 < 0 ||
            q.vertices[1]->vertexNum < 0 ||
            q.vertices[2]->vertexNum < 0 ||
            q.vertices[3]->vertexNum < 0) continue;

        // 3. Recipe coverage gate
        const TerrainQuadRecipe* rec = gos_terrain_indirect::RecipeForVertexNum(vn0);
        if (!rec) continue;

        // terrainHandle: use tex_resolve for per-frame handle indirection
        // (memory/mc2_texture_handle_is_live.md — never cache raw handle).
        const uint32_t th = static_cast<uint32_t>(
            tex_resolve(static_cast<DWORD>(q.terrainHandle)));
        // Skip quads with no base terrain texture (detail-only quads).
        if (th == 0 || th == 0xffffffffu) continue;

        // 4. Per-tri pz check — mirrors quad.cpp:1836-1845 logic.
        //    vertices[c]->pz is pre-projected by vertexProjectLoop.
        bool pzc[4];
        for (int c = 0; c < 4; c++) {
            float pz_adj = q.vertices[c]->pz + TERRAIN_DEPTH_FUDGE;
            pzc[c] = (pz_adj >= 0.0f) && (pz_adj < 1.0f);
        }

        const int uvMode = q.uvMode;
        bool pzTri1, pzTri2;
        if (uvMode == 1 /*BOTTOMLEFT*/) {
            pzTri1 = pzc[0] && pzc[1] && pzc[3];
            pzTri2 = pzc[1] && pzc[2] && pzc[3];
        } else {
            // BOTTOMRIGHT / TOPRIGHT diagonal
            pzTri1 = pzc[0] && pzc[1] && pzc[2];
            pzTri2 = pzc[0] && pzc[2] && pzc[3];
        }
        if (!pzTri1 && !pzTri2) continue;  // both culled

        // lightRGB — mirrors quad.cpp:1860-1865 (lightRGBc lambda).
        // alphaOverride (whitens lighting when terrainTextures2 is active
        // and not pure-cement-with-no-overlay) is baked into the existing
        // M2 thin path.  We mirror the exact same logic here.
        // The recipe already carries terrainHandle; we just need per-corner light.
        // Judgment call: use the same simplified lightRGBc pattern the M2 thin
        // path uses — terrainTextures2 check for alphaOverride, selection check.
        const bool alphaOverride = (Terrain::terrainTextures2 != nullptr);
        auto lightRGBc = [&](int c) -> uint32_t {
            DWORD lc = q.vertices[c]->lightRGB;
            if (alphaOverride) lc = 0xffffffffu;
            if (q.vertices[c]->pVertex && q.vertices[c]->pVertex->selected)
                lc = static_cast<DWORD>(0xffff7fffu /*SELECTION_COLOR*/);
            return static_cast<uint32_t>(lc);
        };

        TerrainQuadThinRecord& tr = s_shadow[packed];
        tr.recipeIdx     = static_cast<uint32_t>(vn0);
        tr.terrainHandle = th;
        tr.flags         = static_cast<uint32_t>((uvMode == 1 ? 1u : 0u)
                         | (pzTri1 ? 2u : 0u)
                         | (pzTri2 ? 4u : 0u));
        // Cement layer-index lookup, keyed by textures[] SLOT (stable across
        // frames).  nodeIdx (q.terrainHandle) mutates per-frame per
        // memory/mc2_texture_handle_is_live.md, so a nodeIdx-keyed lookup
        // intermittently misses → cement validity bit flickers → visible
        // concrete flicker on indirect base + perceived flicker on alpha-cement
        // overlay composite.  Slot is allocated once by initTexture and stable
        // for the mission lifetime.  Re-derive the slot at packer time from
        // pVertex->textureData & 0xFFFFu (the same expression quad.cpp:546
        // passes to getTextureHandle).
        //
        // Encoding (V23, widened in V27):
        //   bit 31     = CEMENT_LAYER_VALID — disambiguates "layer 0" from "not cement"
        //   bits 30:16 = reserved for future layers (decals, scorch)
        //   bits 15:0  = cement atlas layer index (0..65535 encoding cap;
        //                practically capped at 1024 by atlas budget — see
        //                BuildCementCatalogAtlas)
        constexpr uint32_t kCementLayerValidBit = 0x80000000u;
        uint32_t cementWord = 0u;
        uint16_t idx = 0xFFFFu;
        if (g_cementLayerMapReady && q.vertices[0] && q.vertices[0]->pVertex) {
            const DWORD slot = q.vertices[0]->pVertex->textureData & 0xFFFFu;
            if (slot < (DWORD)MC_MAX_TERRAIN_TXMS) {
                idx = g_cementLayerIndexBySlot[slot];
                if (idx != 0xFFFFu) {
                    cementWord = kCementLayerValidBit | ((uint32_t)idx & 0xFFFFu);
                    ++g_cementMappedThisFrame;
                }
            }
            // Lifecycle counter: count quads that EXPECT a cement layer
            // (all 4 corner materials in recipe._wp0 == Concrete=3),
            // independent of whether the layer lookup succeeded.
            if (rec) {
                uint32_t tpacked = 0u;
                memcpy(&tpacked, &rec->_wp0, 4);
                const bool allConcrete =
                    ((tpacked        & 0xFFu) == 3u) &&
                    (((tpacked >> 8) & 0xFFu) == 3u) &&
                    (((tpacked >>16) & 0xFFu) == 3u) &&
                    (((tpacked >>24) & 0xFFu) == 3u);
                if (allConcrete) {
                    ++g_concreteAllCornersThisFrame;
                    if (idx == 0xFFFFu) ++g_cementPackUnmappedCount;
                }
            }
        }
        // Old nodeIdx-based lookup — kept as dead code for one commit; the
        // slot-keyed lookup above is what drives correctness now.  Slot is
        // stable across frames; nodeIdx is not.
        // const DWORD nodeIdx = (DWORD)q.terrainHandle;
        // if (nodeIdx < (DWORD)MC_MAXTEXTURES) {
        //     const uint16_t idx2 = g_cementLayerIndexByNodeIdx[nodeIdx];
        //     ...
        // }
        tr.cementWord    = cementWord;
        tr.lightRGB0     = lightRGBc(0);
        tr.lightRGB1     = lightRGBc(1);
        tr.lightRGB2     = lightRGBc(2);
        tr.lightRGB3     = lightRGBc(3);

        // Fix B 2026-05-14: per-corner clip-space positions from the recipe's
        // worldPos0..3 multiplied by the current terrainMVP.  Mirrors the
        // compute shader's projectClip() at gpu_driven_terrain_solid.comp:174.
        //
        // Matrix convention (memory/terrain_mvp_gl_false.md): terrain_mvp_ is
        // stored row-major on CPU and uploaded with GL_FALSE.  GLSL interprets
        // bytes as column-major, so GPU's `m * v` reads as a dot of v with the
        // i-th *column* of our row-major storage — i.e. out[i] = sum_j m[j*4+i]
        // * v[j].  CPU mirror uses the same formula so values match modulo
        // FMA-fusion / rounding-mode drift (parity comparator allows ~4 ULP).
        const float* mvp = gos_GetTerrainMVPMat4();
        if (mvp) {
            const float* wp[4] = {
                &rec->wx0, &rec->wx1, &rec->wx2, &rec->wx3
            };
            for (int c = 0; c < 4; ++c) {
                const float vx = wp[c][0], vy = wp[c][1], vz = wp[c][2];
                for (int i = 0; i < 4; ++i) {
                    tr.clipPos[c * 4 + i] =
                        mvp[0 * 4 + i] * vx +
                        mvp[1 * 4 + i] * vy +
                        mvp[2 * 4 + i] * vz +
                        mvp[3 * 4 + i];
                }
            }
        } else {
            // No MVP this frame (impossible during normal play, defensive).
            // Zero clipPos so VS produces a degenerate triangle behind near
            // clip — same fail-safe shape as the pzOk culled path in the VS.
            for (int k = 0; k < 16; ++k) tr.clipPos[k] = 0.0f;
        }

        gos_terrain_indirect::Counters_AddIndirectSolidPackedQuad();
        ++packed;
    }

    if (packed == 0) return 0;

    // Upload to the current ring slot.
    const GLintptr slotOffset = (GLintptr)(g_thinRingSlot * kThinRecordBytes);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_thinRecordSSBO);
    MC2_GL_BufferSubData(GL_SHADER_STORAGE_BUFFER,
                    slotOffset,
                    (GLsizeiptr)(packed * sizeof(TerrainQuadThinRecord)),
                    s_shadow);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    // Diagnostic Test 1 — per-frame cement classification trace (1/sec at 60fps).
    {
        static uint32_t s_frameCount = 0;
        ++s_frameCount;
        if (traceOn() && (s_frameCount % 60u == 0u)) {
            printf("[CEMENT_FRAME v1] frame=%u packed_total=%u cement_mapped=%u concrete_all_corners=%u\n",
                   (unsigned)s_frameCount, (unsigned)packed,
                   (unsigned)g_cementMappedThisFrame,
                   (unsigned)g_concreteAllCornersThisFrame);
            fflush(stdout);
        }
    }

    return packed;
}

// ---------------------------------------------------------------------------
// BuildIndirectCommands() — builds 1 DrawArraysIndirectCommand (PR1 SOLID-only).
// Returns 1 on success, 0 on error.
// ---------------------------------------------------------------------------
static int BuildIndirectCommands(int thinCount) {
    if (thinCount <= 0) return 0;

    DrawArraysIndirectCommand cmd{};
    cmd.count         = static_cast<GLuint>(thinCount * 6);
    cmd.instanceCount = 1u;
    cmd.first         = 0u;
    cmd.baseInstance  = 0u;

    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, g_indirectCmdBuffer);
    MC2_GL_BufferSubData(GL_DRAW_INDIRECT_BUFFER, 0,
                    (GLsizeiptr)sizeof(cmd), &cmd);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
    return 1;
}

// ---------------------------------------------------------------------------
// UploadTerrainHandleLUT — Phase C Stage 2 (GPU-direct replacement for
// BuildSolidQuadWindowSSBO). Builds a uint[MC_MAXTEXTURES] LUT mapping
// nodeId → resolved gosHandle (tex_resolve result), then uploads it as a
// 16KB SSBO on binding 2. The GPU shader reads nodeId from recipe._wp2 and
// indexes into this LUT — eliminating the per-frame quadList CPU walk.
// Returns g_denseRecipes.size() as the GPU dispatch count (full recipe range).
// ---------------------------------------------------------------------------
static uint32_t UploadTerrainHandleLUT() {
    constexpr GLsizeiptr kLutBytes = MC_MAXTEXTURES * sizeof(uint32_t);

    if (g_terrainHandleLutSSBO == 0) {
        // TIER2-EXCLUDED: dead-path
        glGenBuffers(1, &g_terrainHandleLutSSBO);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_terrainHandleLutSSBO);
        MC2_GL_BufferData(GL_SHADER_STORAGE_BUFFER, kLutBytes, nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }

    // Resolve unique nodeIds from the mission-static set — typically 20-50
    // entries regardless of camera position. O(uniqueTextures), not O(quads).
    static uint32_t lut[MC_MAXTEXTURES];
    memset(lut, 0, sizeof(lut));
    for (uint32_t nodeId : g_uniqueTerrainNodeIds) {
        if (nodeId > 0u && nodeId < (uint32_t)MC_MAXTEXTURES) {
            const uint32_t th = static_cast<uint32_t>(tex_resolve((DWORD)nodeId));
            lut[nodeId] = (th == 0u || th == 0xffffffffu) ? 0u : th;
        }
    }

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_terrainHandleLutSSBO);
    MC2_GL_BufferSubData(GL_SHADER_STORAGE_BUFFER, 0, kLutBytes, lut);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    {
        static bool s_lutFirstPrinted = false;
        if (!s_lutFirstPrinted) {
            s_lutFirstPrinted = true;
            int nonZero = 0;
            for (uint32_t nodeId : g_uniqueTerrainNodeIds) {
                if (nodeId > 0u && nodeId < (uint32_t)MC_MAXTEXTURES && lut[nodeId] != 0u)
                    ++nonZero;
            }
            fprintf(stderr, "[TERRAIN_INDIRECT v1] event=lut_upload unique=%d nonzero=%d\n",
                    (int)g_uniqueTerrainNodeIds.size(), nonZero);
            fflush(stderr);
        }
    }

    return (uint32_t)g_denseRecipes.size();
}

// ---------------------------------------------------------------------------
// BuildSolidQuadWindowSSBO — Approach A.  Structural twin of WaterStream's
// BuildQuadWindowSSBO (gos_terrain_water_stream.cpp:1109).  Uploads the
// per-frame camera-windowed list of recipe indices collected by
// AppendSolidWindowCandidate() into a std430 plain-uint[] SSBO (4 B stride,
// NOT the old 16 B SolidQuadWindowEntry — no struct, no static_assert
// lockstep; the GPU re-derives thSlot/cementWord/uvMode from the recipe).
//
// Lazy-grow capped at kMaxThinRecords using the CPU-side capacity mirror
// (g_solidQuadWindowCapacity) — NO glGetBufferSubData / glMapBuffer.
//
// Return value == GPU dispatch count:
//   * narrow ON  && staging non-empty: window count; *outUseWindow = 1.
//   * narrow OFF || staging empty:     full g_denseRecipes.size() fallback;
//     *outUseWindow = 0 (shader uses identity vn0 = id == current HEAD path).
// The empty-on-armed-frame case (e.g. the 1-frame lag's very first armed
// frame, before the loop has run once) falls back to full-range so terrain
// is never blank.
static uint32_t BuildSolidQuadWindowSSBO(int* outUseWindow) {
    const bool narrowOn = gos_terrain_indirect::SolidWindowEnabled();

    if (!narrowOn || g_solidWindowStaging.empty()) {
        if (outUseWindow) *outUseWindow = 0;
        return (uint32_t)g_denseRecipes.size();
    }

    // Cap the window at the thin-record capacity — the GPU output array is
    // kMaxThinRecords entries; dispatching more invocations than that cannot
    // admit more records, and over-large windows waste GPU threads.
    uint32_t windowCount = (uint32_t)g_solidWindowStaging.size();
    if (windowCount > (uint32_t)kMaxThinRecords)
        windowCount = (uint32_t)kMaxThinRecords;
    if (windowCount == 0) {
        if (outUseWindow) *outUseWindow = 0;
        return (uint32_t)g_denseRecipes.size();
    }

    const uint32_t bytesNeeded = windowCount * (uint32_t)sizeof(uint32_t);

    // Lazy-grow via the CPU-side capacity mirror (no GPU round-trip).  Cap the
    // allocation at kMaxThinRecords entries (the hard upper bound).
    if (g_solidQuadWindowSsbo != 0 && g_solidQuadWindowCapacity < bytesNeeded) {
        glDeleteBuffers(1, &g_solidQuadWindowSsbo);
        g_solidQuadWindowSsbo     = 0;
        g_solidQuadWindowCapacity = 0;
    }
    if (g_solidQuadWindowSsbo == 0) {
        const uint32_t cap =
            (uint32_t)(kMaxThinRecords * sizeof(uint32_t));
        // TIER2-EXCLUDED: dead-path
        glGenBuffers(1, &g_solidQuadWindowSsbo);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_solidQuadWindowSsbo);
        MC2_GL_BufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)cap, nullptr,
                     GL_DYNAMIC_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        g_solidQuadWindowCapacity = cap;
    }

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_solidQuadWindowSsbo);
    MC2_GL_BufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)bytesNeeded,
                    g_solidWindowStaging.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    // -----------------------------------------------------------------------
    // PARITY PROBE (catastrophic-axis).  Active ONLY under
    // MC2_TERRAIN_SOLID_WINDOW_PARITY=1 — zero hot cost when off.
    //
    // Non-tautological by construction: `win` is built from
    // g_solidWindowStaging (produced by terrain.cpp's slim-loop predicate:
    // clipInfo cull-active + RecipeForVertexNum).  `ref` is built by an
    // INDEPENDENT producer here — iterating g_denseRecipes indices and
    // applying: Terrain::getObjVertexActive(vn) (the slim loop's dilated
    // visible-cull superset, the SAME source-of-truth the corrected collector
    // rides) AND the GPU shader keep predicate MINUS pz (edge-skip +
    // recipe-resolvable + _wp2!=0 + LUT-resolvable-or-cement).  Two different
    // producers (g_denseRecipes/objVertexActive vs g_solidWindowStaging), two
    // different data sources — ref does NOT read g_solidWindowStaging.
    //
    // Catastrophic direction only: a recipe that is cull-active AND the GPU
    // WOULD keep (ref-member) AND that is a live drawable quad
    // (RecipeForVertexNum non-null) but is ABSENT from `win` == a dropped
    // visible quad == terrain-vanish.  With identical source-of-truth and
    // same-frame consumption, dropped MUST be 0 steady-state AND under camera
    // motion; any nonzero is logged (gated stderr, NOT assert — assert is a
    // no-op under RelWithDebInfo's /DNDEBUG).
    if (gos_terrain_indirect::IsSolidWindowParityEnabled()) {
        const long mapSide = Terrain::realVerticesMapSide;
        const size_t N = g_denseRecipes.size();

        // win membership set (recipe idx -> present this frame's window).
        static std::vector<uint8_t> winSet;
        winSet.assign(N, 0u);
        for (uint32_t i = 0; i < windowCount; ++i) {
            const uint32_t v = g_solidWindowStaging[i];
            if (v < (uint32_t)N) winSet[v] = 1u;
        }

        uint64_t refVisible = 0;
        uint64_t dropped    = 0;
        if (mapSide > 0) {
            for (size_t vn = 0; vn < N; ++vn) {
                const long mx = (long)vn % mapSide;
                const long my = (long)vn / mapSide;
                // GPU edge-skip (gpu_driven_terrain_solid.comp:244).
                if (mx >= mapSide - 1 || my >= mapSide - 1) continue;

                // MAJOR-2: ref must add the cull-active term so it matches the
                // corrected (slim-loop / dilated-cull) collector's source-of-
                // truth.  Without this the probe would false-alarm on every
                // off-window-but-GPU-keepable recipe.  This is the SAME
                // objVertexActive[] the slim loop writes via
                // setObjVertexActive(rv->vertexNum,true) — read independently
                // here, NOT via g_solidWindowStaging.
                if (!Terrain::getObjVertexActive((long)vn)) continue;

                const TerrainQuadRecipe& rec = g_denseRecipes[vn];
                uint32_t nodeId = 0u, cementWord = 0u;
                memcpy(&nodeId,     &rec._wp2, 4);
                memcpy(&cementWord, &rec._wp3, 4);
                const bool cementQuad = (cementWord & 0x80000000u) != 0u;

                // GPU thSlot gate (comp:285-294): non-cement quads need a
                // resolvable nodeId; cement quads pass regardless.
                bool gpuKeep;
                if (nodeId == 0u || nodeId >= (uint32_t)MC_MAXTEXTURES) {
                    gpuKeep = cementQuad;
                } else {
                    const uint32_t th =
                        static_cast<uint32_t>(tex_resolve((DWORD)nodeId));
                    const bool resolvable =
                        (th != 0u && th != 0xffffffffu);
                    gpuKeep = resolvable || cementQuad;
                }
                if (!gpuKeep) continue;

                // Restrict to quads that are actually live drawables this
                // frame (a recipe slot with no current quad cannot vanish).
                if (!gos_terrain_indirect::RecipeForVertexNum((int32_t)vn)) continue;

                ++refVisible;
                if (vn < N && winSet[vn] == 0u) ++dropped;
            }
        }

        static int      s_parityFrame    = 0;
        static uint32_t s_parityMaxWin   = 0;
        static uint64_t s_parityTotDrop  = 0;
        const int frame = s_parityFrame++;
        if (windowCount > s_parityMaxWin) s_parityMaxWin = windowCount;
        s_parityTotDrop += dropped;

        if (frame < 64) {
            fprintf(stderr,
                "[TERRAIN_SOLID_WINDOW v1] event=frame frame=%d window=%u "
                "ref_visible=%u dropped=%u\n",
                frame, windowCount, (unsigned)refVisible,
                (unsigned)dropped);
            fflush(stderr);
        }
        if (frame > 0 && (frame % 600) == 0) {
            fprintf(stderr,
                "[TERRAIN_SOLID_WINDOW v1] event=summary frames=%u "
                "max_window=%u total_dropped=%llu\n",
                (unsigned)frame, s_parityMaxWin,
                (unsigned long long)s_parityTotDrop);
            fflush(stderr);
        }
        // CRITICAL-1: catastrophic-axis tripwire — gated unconditional stderr,
        // NOT assert.  The project mandates --config RelWithDebInfo, under
        // which MSVC injects /DNDEBUG (CMakeLists does not strip it), so
        // assert() compiles to a no-op and a dropped visible quad would pass
        // silently.  fprintf always fires regardless of NDEBUG.
        if (dropped) {
            fprintf(stderr,
                "[TERRAIN_SOLID_WINDOW v1] event=catastrophic dropped=%llu "
                "frame=%d\n",
                (unsigned long long)dropped, frame);
            fflush(stderr);
        }
        (void)dropped;
    }

    if (outUseWindow) *outUseWindow = 1;
    return windowCount;
}

}  // anonymous namespace (Stage 3 helpers)

namespace gos_terrain_indirect {

// ---------------------------------------------------------------------------
// Stage 3 public API
// ---------------------------------------------------------------------------

bool IsFrameSolidArmed() {
    return s_frameSolidArmed && !s_processArmingDisabled;
}

// Process-sticky variant: true once ComputePreflight() has armed at least
// one frame in this process. Used by VisualDiff to latch "intro pan
// complete" -- the per-frame arm is cleared by gosRenderer::endFrame()
// before the VisualDiff post-PP hook reads it, so a sticky-once cousin
// is required. Never resets within a process.
bool WasEverFrameSolidArmed() {
    return s_everFrameSolidArmed && !s_processArmingDisabled;
}

void ForceDisableArmingForProcess() {
    if (!s_processArmingDisabled) {
        // Loud-fail (T16/T19): a hard GL/init failure has PERMANENTLY disabled the GPU
        // terrain path for this process. The only backstop is the legacy setupTextures
        // fallback, which Phase 8z removes -- after 8z this condition renders NO terrain
        // (black) with no recovery. State it prominently; the preceding event=error line
        // carries the actual cause.
        fprintf(stderr,
            "[TERRAIN_INDIRECT v1] FATAL event=arming_disabled_process_sticky "
            "msg=GPU_terrain_path_PERMANENTLY_DISABLED_after_hard_GL_or_init_failure "
            "(post-8z: terrain will NOT render -- see preceding event=error line)\n");
        fflush(stderr);
    }
    s_processArmingDisabled = true;
}

void BeginFrame() {
    // Reset armed flag unconditionally every frame so mech-bay / menu frames
    // don't inherit the armed state from the last gameplay frame.
    // ComputePreflight() re-arms when terrain is actually present.
    static int s_beginFrameCount = 0;
    ++s_beginFrameCount;
    if (s_solidGpuDispatchRanThisFrame && !s_frameSolidArmed) {
        // Dispatch ran this frame but arming is already false — double BeginFrame call.
        fprintf(stderr, "[TERRAIN_INDIRECT v1] event=double_beginframe count=%d\n",
                s_beginFrameCount);
        fflush(stderr);
    }
    s_frameSolidArmed = false;
    // BeginFrame() is the true end-of-frame boundary; clear BOTH per-frame latches here
    // so they can't desync on idle (un-preflighted) frames. On logistics / mech-bay /
    // menu screens ComputePreflight() stops running, so without this the dispatch latch
    // stays stale-true and the guard above fires every frame (false-positive spam).
    // ComputePreflight() re-sets it next in-mission frame.
    s_solidGpuDispatchRanThisFrame = false;
}

// ---------------------------------------------------------------------------
// BeginFrameSolidWindow / AppendSolidWindowCandidate — camera-windowed solid
// dispatch collector.  Structural twin of WaterStream::BeginFrameNarrow /
// AppendNarrowCandidate (gos_terrain_water_stream.cpp:149/161).  Called from
// terrain.cpp's existing setupTextures loop; NO new walk is introduced.
// ---------------------------------------------------------------------------
void BeginFrameSolidWindow() {
    if (!SolidWindowEnabled()) return;
    // Reserve last-frame max + 10% slack + 64, mirroring water :149-158.
    // vector::clear on a trivially-destructible uint32_t is a size reset; no
    // allocation occurs in the hot loop once the high-water mark is reached.
    const size_t reserve =
        (size_t)(g_solidWindowMaxSeen + (g_solidWindowMaxSeen / 10) + 64);
    if (g_solidWindowStaging.capacity() < reserve)
        g_solidWindowStaging.reserve(reserve);
    g_solidWindowStaging.clear();
}

void AppendSolidWindowCandidate(int32_t vn0) {
    // Caller (terrain.cpp) asserts the quad already passed a predicate that is
    // STRICTLY LOOSER than the GPU keep-set: corners non-null, vertexNum >= 0,
    // and RecipeForVertexNum(vn0) != nullptr.  It applies NO pz, NO
    // terrainHandle, NO _wp2 filter — the GPU shader still does all of those
    // per-thread.  Looseness == guaranteed superset == no terrain-vanish.
    if (vn0 < 0) return;
    g_solidWindowStaging.push_back((uint32_t)vn0);
    if (g_solidWindowStaging.size() > g_solidWindowMaxSeen)
        g_solidWindowMaxSeen = (uint32_t)g_solidWindowStaging.size();
}

// Phase 8b A/B: read-only view of the legacy staging window. No mutation.
const uint32_t* SolidWindowStagingData(uint32_t* outCount) {
    if (outCount) *outCount = (uint32_t)g_solidWindowStaging.size();
    return g_solidWindowStaging.empty() ? nullptr : g_solidWindowStaging.data();
}

bool ComputePreflight() {
    ZoneScopedN("Terrain::IndirectPreflight");
    {
        // VPL parity-infra retirement lifecycle marker (Debug-instrumentation
        // rule): one-shot, grep-visible in smoke artifacts. s_packParityMask /
        // kParityMaskWords / gos_terrain_indirect_getPackParityMask /
        // ComputeDispatchParity_Check fully removed; txmmgr call removed.
        static bool s_loggedParityRetired = false;
        if (!s_loggedParityRetired) {
            s_loggedParityRetired = true;
            printf("[TERRAIN_INDIRECT v1] event=parity_infra_retired "
                   "path=gos_terrain_indirect scope=full "
                   "removed=s_packParityMask,ComputeDispatchParity_Check,getPackParityMask,txmmgr_call\n");
            fflush(stdout);
        }
    }
    s_frameSolidArmed                = false;
    s_frameSolidPackedThinCount      = 0;
    s_frameSolidCmdCount             = 0;
    s_solidGpuDispatchRanThisFrame   = false;

    if (s_processArmingDisabled) {
        // Loud-fail (T16/T19): the one-shot disable line is easy to miss, and post-8z
        // this is a silent black-terrain state with no fallback. Re-warn periodically so
        // the condition is unmissable in logs for the whole session it persists.
        static unsigned long s_disabledTicks = 0;
        if ((s_disabledTicks++ % 300u) == 0u) {
            fprintf(stderr,
                "[TERRAIN_INDIRECT v1] FATAL arming still disabled (tick %lu) -- "
                "GPU terrain not rendering; no fallback post-8z.\n", s_disabledTicks);
            fflush(stderr);
        }
        return false;  // ForceDisableArmingForProcess already printed the initial cause
    }
    if (!IsEnabled()) {
        static bool s_warnedEnabled = false;
        if (!s_warnedEnabled) {
            s_warnedEnabled = true;
            fprintf(stderr, "[TERRAIN_INDIRECT v1] event=preflight_fail reason=not_enabled\n");
            fflush(stderr);
        }
        return false;
    }
    if (!IsDenseRecipeReady()) {
        static bool s_warnedRecipe = false;
        if (!s_warnedRecipe) {
            s_warnedRecipe = true;
            fprintf(stderr, "[TERRAIN_INDIRECT v1] event=preflight_fail reason=recipe_not_ready\n");
            fflush(stderr);
        }
        return false;
    }
    if (!ResourcesReady()) {
        static bool s_warnedResources = false;
        if (!s_warnedResources) {
            s_warnedResources = true;
            fprintf(stderr, "[TERRAIN_INDIRECT v1] event=preflight_fail reason=resources_not_ready\n");
            fflush(stderr);
        }
        return false;
    }
    if (InMissionTransition())    return false;

    FlushDirtyRecipeSlotsToGPU();

    // Arm the GPU terrain path when this is a colormap (terrainTextures2) map.
    // Two independent signals indicate that:
    //   (a) g_uniqueTerrainNodeIds non-empty — the map has cement/overlay quads
    //       whose recipe._wp2 is a real tile node index (needs the handle LUT); OR
    //   (b) g_atlasGLTex != 0 — BuildColormapAtlas produced the merged colormap
    //       atlas. The bulk of a colormap map's quads carry the 0xFFFFFFFF
    //       sentinel in _wp2 and sample this atlas directly by world position,
    //       NOT via the node LUT, so they render even with an empty LUT.
    //
    // The legacy code armed on (a) alone. That silently fails for a map made
    // ENTIRELY of colormap quads with no cement/overlay tiles — e.g. a freshly
    // generated flat editor map: every quad's _wp2 is 0xFFFFFFFF, which
    // CollectUniqueNodeIds() drops (>= MC_MAXTEXTURES), leaving the node list
    // empty. The GPU path never armed, the frame fell through to the CPU thin
    // path which packed zero records (zero_thin), and the terrain rendered as
    // the black clear color. Adding (b) fixes that without changing behaviour
    // for any existing map: stock maps always have both an atlas AND some tile
    // quads, and legacy non-colormap maps build no atlas (terrainTextures2 NULL)
    // and leave _wp2=0 — so neither signal fires and they still take the CPU
    // path. g_atlasGLTex is zeroed per-mission on unload, so it can't go stale.
    if (gos_terrain_arm::ShouldArmGpuTerrain(
            gpu_driven::IsTerrainSolidEnabled(),
            !g_uniqueTerrainNodeIds.empty(),
            g_atlasGLTex != 0)) {
        // GPU path: arm immediately. ComputeDispatch() (called after Phase 1
        // PackAndDispatch) will build the window SSBO and issue the primary
        // cull/pack compute dispatch.  Per VPL step 2 (commit 2b), the primary
        // dispatch is the sole writer of cmds[0].count (atomicAdd of 6u per
        // admitted record); the cmd-patch shader dispatch is retired.
        // DrawIndirect() fires glMultiDrawArraysIndirect with cmdCount=1.
        s_frameSolidPackedThinCount = -1;   // sentinel: GPU path (logged only)
        s_frameSolidCmdCount        = 1;    // PR1: always 1 indirect draw command
        s_frameSolidArmed           = true;
        s_everFrameSolidArmed       = true;
        {
            static bool s_firstArm = false;
            if (!s_firstArm) {
                s_firstArm = true;
                fprintf(stderr, "[TERRAIN_INDIRECT v1] event=first_arm path=gpu nodeIds=%zu atlasTex=%u\n",
                        g_uniqueTerrainNodeIds.size(), (unsigned)g_atlasGLTex);
                fflush(stderr);
            }
        }
        return true;
    }

    // CPU path (legacy — demote-don't-delete per CLAUDE.md debug instrumentation rule).
    const int thinCount = PackThinRecordsForFrame();
    if (thinCount == 0) {
        if (traceOn())
            printf("[TERRAIN_INDIRECT v1] event=preflight_skip reason=zero_thin\n");
        return false;
    }

    const int cmdCount = BuildIndirectCommands(thinCount);
    if (cmdCount == 0) {
        if (traceOn())
            printf("[TERRAIN_INDIRECT v1] event=preflight_skip reason=zero_cmd\n");
        return false;
    }

    s_frameSolidPackedThinCount = thinCount;
    s_frameSolidCmdCount        = cmdCount;
    s_frameSolidArmed           = true;
    s_everFrameSolidArmed       = true;
    {
        static bool s_firstArm = false;
        if (!s_firstArm) {
            s_firstArm = true;
            fprintf(stderr, "[TERRAIN_INDIRECT v1] event=first_arm path=cpu thin=%d cmd=%d\n",
                    thinCount, cmdCount);
            fflush(stderr);
        }
    }
    return true;
}

void ComputeDispatch() {
    // Phase C Stage 2. Called AFTER gos_terrain_lighting::PackAndDispatch() so
    // Phase 1's lighting SSBO is populated with same-frame data.
    if (!s_frameSolidArmed)                    return;
    if (!gpu_driven::IsTerrainSolidEnabled())  return;

    ZoneScopedN("Terrain::SolidComputeDispatch");
    mc2_hitch::HitchScope _hitchTerrain(mc2_hitch::HitchSpanKind::TerrainSolidDispatch);

    // Guard: Phase 1 lighting SSBO must be ready.
    const GLuint lightSsbo = gos_terrain_lighting::GetOutputSsbo();
    if (lightSsbo == 0) {
        static bool s_warnedLight = false;
        if (!s_warnedLight) {
            printf("[TERRAIN_INDIRECT v1] event=warn msg=solid_lighting_ssbo_not_ready\n");
            fflush(stdout);
            s_warnedLight = true;
        }
        return;
    }

    // Lazy-build compute programs and cache uniform locations + constant uniforms.
    if (g_solidComputeProgram == 0) {
        g_solidComputeProgram = gpu_driven::BuildComputeProgramFromFile(
            "shaders/gpu_driven_terrain_solid.comp", nullptr, 0, "gpu_driven_terrain_solid");
        if (g_solidComputeProgram == 0) {
            fprintf(stderr, "[TERRAIN_INDIRECT v1] event=error msg=solid_compute_compile_failed\n");
            fflush(stderr);
            ForceDisableArmingForProcess();
            return;
        }
        g_locSolidWC  = glGetUniformLocation(g_solidComputeProgram, "u_windowCount");
        g_locSolidAO  = glGetUniformLocation(g_solidComputeProgram, "u_alphaOverride");
        g_locSolidMVP = glGetUniformLocation(g_solidComputeProgram, "u_worldToClipGL");
        // Step 2b: u_bucketHeaderTrace — gate for hdr.{visibleCount,pad1_,pad2_}
        // writes in the primary compute shader.
        g_locSolidBHT = glGetUniformLocation(g_solidComputeProgram, "u_bucketHeaderTrace");
        // Approach A: window-vs-identity gate for the solid index buffer.
        g_locSolidUW  = glGetUniformLocation(g_solidComputeProgram, "u_useWindow");
        // u_mapSide is NOT a constant: it changes whenever a different-size map
        // loads. The program is compiled once per process, so uploading it here
        // baked in the FIRST map's side and a later, larger map (e.g. editor blank
        // 120 -> generated 260) had the shader skip ~79% of quads (edge + index
        // decompose use the stale side) -> all-black terrain. Cache the location;
        // upload the live value per frame in ComputeDispatch().
        g_locSolidMS  = glGetUniformLocation(g_solidComputeProgram, "u_mapSide");
        // WATER-REFLECTION-CLIP-1: may be optimized out if unused by the
        // driver in some future shader revision; -1 is handled below (skip
        // upload, shader-side uniform defaults to 0 = main-pass behavior).
        g_locSolidReflPass = glGetUniformLocation(g_solidComputeProgram, "u_reflectionPass");
        // Upload the genuinely-constant uniforms once.
        MC2_GL_UseProgram(g_solidComputeProgram);
        const GLint locMTR  = glGetUniformLocation(g_solidComputeProgram, "u_maxThinRecords");
        const GLint locWUPV = glGetUniformLocation(g_solidComputeProgram, "u_worldUnitsPerVertex");
        if (locMTR  >= 0) glUniform1i(locMTR, (int)kMaxThinRecords);
        if (locWUPV >= 0) glUniform1f(locWUPV, Terrain::worldUnitsPerVertex);
        MC2_GL_UseProgram(0);
    }
    // Step 2b (VPL retirement): cmd-patch program compile retired -- the
    // primary compute writes cmds[0].count directly via atomicAdd.

    // Step 2b: bucket-header SSBO demoted behind MC2_BUCKET_HEADER_TRACE.
    // Cached env lookup, used at alloc / bind / clear / readback / teardown.
    static const bool s_bucketHeaderTrace =
        (getenv("MC2_BUCKET_HEADER_TRACE") != nullptr);
    // Lazy-allocate bucket header SSBO (16 B GpuDrivenBucketHeader) ONLY when
    // diagnostic tracing is requested.  Default-off: zero allocation, zero
    // per-frame work, zero binding.
    if (s_bucketHeaderTrace && g_solidBucketHeaderSsbo == 0) {
        // TIER2-EXCLUDED: dead-path
        glGenBuffers(1, &g_solidBucketHeaderSsbo);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_solidBucketHeaderSsbo);
        MC2_GL_BufferData(GL_SHADER_STORAGE_BUFFER, 16, nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }

    // Probe 6 + 7a: lazy-allocate canary SSBO — 2*kMaxThinRecords uints,
    // never bound by the bridge.  Compute writes [recipeIdx, flags] per record.
    if (g_thinCanarySSBO == 0) {
        // TIER2-EXCLUDED: dead-path
        glGenBuffers(1, &g_thinCanarySSBO);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_thinCanarySSBO);
        MC2_GL_BufferData(GL_SHADER_STORAGE_BUFFER,
                     (GLsizeiptr)(2u * kMaxThinRecords * sizeof(uint32_t)),
                     nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }

    // Upload per-frame terrain handle LUT (nodeId → gosHandle, ~20-50 entries).
    // KEPT from 08bd3b2: the shader still resolves thSlot via LUT[nodeId] from
    // recipe._wp2 and cementWord from _wp3 regardless of windowing.  The LUT
    // upload is O(uniqueTextures); its return value (full recipe count) is
    // discarded — the dispatch count now comes from the camera window.
    (void)UploadTerrainHandleLUT();

    // Approach A: build the per-frame camera-windowed recipe-index SSBO from
    // the list collected by AppendSolidWindowCandidate() during terrain.cpp's
    // setupTextures loop (NO new walk).  s_solidUseWindow=1 when the window is
    // active; 0 == fall back to full-range identity (current HEAD behavior /
    // safety escape hatch / first armed frame before the loop has run).
    int s_solidUseWindow = 0;
    const uint32_t windowCount = BuildSolidQuadWindowSSBO(&s_solidUseWindow);
    if (windowCount == 0) {
        // Zero visible quads this frame; leave armed — DrawIndirect fires with
        // cmd.count=0 (after bucket-clear + cmd-patch). No visual artifact.
        // Still need to run both dispatches to keep cmd buffer in defined state.
    }

    // Advance the thin-record ring slot (same ring as CPU path).
    g_thinRingSlot = (g_thinRingSlot + 1) % kThinRingFrames;

    // ── Probe sink — writes tripwires to a file next to mc2.exe regardless of
    // how the process was launched.  Solves the "stderr disappears under
    // double-click on Windows" problem so raw user repros are captured.
    // Append-mode; line-buffered via fflush after each write.  Opened once,
    // never closed (process-lifetime).  Stderr writes are kept too — the
    // file is additive, not a redirect.
    static FILE* s_probeSink = []{
        FILE* f = fopen("ring_trace.log", "a");
        if (f) {
            fprintf(f, "\n[RING_SINK v1] event=open time=%lld\n",
                    (long long)time(nullptr));
            fflush(f);
        }
        return f;
    }();
#define PROBE_LOG(fmt, ...) do { \
        fprintf(stderr, fmt, ##__VA_ARGS__); \
        fflush(stderr); \
        if (s_probeSink) { fprintf(s_probeSink, fmt, ##__VA_ARGS__); fflush(s_probeSink); } \
    } while (0)

    // ── Ring-hazard probe (MC2_RING_TRACE) ────────────────────────────────
    // Default-off per-frame trace; ALWAYS-on tripwire when the wait actually
    // times out or the fence is missing on a non-warmup frame. Either signal
    // proves the ring discipline is broken — the CPU is about to write a
    // thin-record slot whose prior GPU consumer may still be reading it.
    // Symptom we are chasing: huge raster triangles under fast camera
    // rotation; the bug is masked when the camera is still because the
    // thin-record content is frame-to-frame identical (atomicAdd reorder
    // notwithstanding) and the race produces the same bytes either way.
    // Fingerprint of the camera MVP rotation rows lets offline analysis
    // correlate fence misses with rotation magnitude.
    static const bool s_ringTrace        = (getenv("MC2_RING_TRACE") != nullptr);
    static uint64_t   s_ringTraceFrameIdx = 0;
    const uint64_t  ringFrameIdx = ++s_ringTraceFrameIdx;
    const int       probedSlot   = g_thinRingSlot;
    const bool      fencePresent = (g_thinRingFences[probedSlot] != 0);

    uint32_t mvpFp = 0u;
    if (s_ringTrace) {
        const float* mvpProbe = gos_GetTerrainMVPMat4();
        if (mvpProbe) {
            // FNV-1a over rotation/translation rows (skip projection row mvp[12..15])
            // — gives a stable fingerprint that changes proportionally to camera
            // rotation rate but is insensitive to projection-only mutation.
            mvpFp = 2166136261u;
            for (int k = 0; k < 12; ++k) {
                uint32_t bits = 0;
                memcpy(&bits, &mvpProbe[k], sizeof(bits));
                mvpFp ^= bits;
                mvpFp *= 16777619u;
            }
        }
    }

    GLenum waitResult = GL_ALREADY_SIGNALED;
    uint64_t waitUs   = 0;
    if (g_thinRingFences[probedSlot]) {
        // C-02: use timeout=0 (non-blocking) instead of 10ms so the CPU never
        // stalls waiting for the GPU.  If the fence is not yet signaled the slot
        // is still in use by the GPU; skip this dispatch entirely (return below)
        // so we never overwrite an in-flight ring slot.  Ring-overwrite safety is
        // preserved: the fence is only deleted when it has signaled.
        const auto t0 = std::chrono::steady_clock::now();
        waitResult = glClientWaitSync(g_thinRingFences[probedSlot],
                         GL_SYNC_FLUSH_COMMANDS_BIT, 0u /*non-blocking*/);
        const auto t1 = std::chrono::steady_clock::now();
        waitUs = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        if (waitResult != GL_TIMEOUT_EXPIRED && waitResult != GL_WAIT_FAILED) {
            // Fence signaled (GL_ALREADY_SIGNALED or GL_CONDITION_SATISFIED).
            glDeleteSync(g_thinRingFences[probedSlot]);
            g_thinRingFences[probedSlot] = 0;
        }
        // If GL_TIMEOUT_EXPIRED: fence NOT deleted; slot still in-flight.
        // The tripwire block below will log it, then we return early.
    }

    // Tripwire #1: missing fence past warmup OR fence timeout.
    // Warmup window = 2*kThinRingFrames frames; first slot 1 wrap can miss
    // a fence if frame-1's DrawIndirect didn't issue (recipe not ready /
    // MVP not yet valid).  This is benign startup, not a bug.
    {
        // When MC2_TERRAIN_LOD_CHUNK=1 the chunk path owns terrain rendering and
        // DrawIndirect is suppressed — no fences are ever written, so every frame
        // past warmup triggers fenceMissedAfterWarmup. Gate it out to silence the
        // flood and let useful diagnostics from the LOD chunk path show up in logs.
        const bool s_lodChunkActive = mc2TerrainLodChunkEnabled();  // default-on aware
        const bool fenceMissedAfterWarmup =
            (!fencePresent) && (ringFrameIdx > (uint64_t)(2 * kThinRingFrames))
            && !s_lodChunkActive;
        const bool timeoutFired =
            (waitResult == GL_TIMEOUT_EXPIRED || waitResult == GL_WAIT_FAILED);
        if (fenceMissedAfterWarmup || timeoutFired) {
            static uint32_t s_tripwireMissCount    = 0;
            static uint32_t s_tripwireTimeoutCount = 0;
            uint32_t totalMiss    = (fenceMissedAfterWarmup ? ++s_tripwireMissCount    : s_tripwireMissCount);
            uint32_t totalTimeout = (timeoutFired           ? ++s_tripwireTimeoutCount : s_tripwireTimeoutCount);
            PROBE_LOG(
                "[RING_TRIPWIRE v1] frame=%llu slot=%d fence=%d wait=0x%x wait_us=%llu "
                "totalMiss=%u totalTimeout=%u mvpFp=0x%08x\n",
                (unsigned long long)ringFrameIdx, probedSlot, (int)fencePresent,
                (unsigned)waitResult, (unsigned long long)waitUs,
                (unsigned)totalMiss, (unsigned)totalTimeout, (unsigned)mvpFp);
        }
    }

    // Tripwire #2: prev-frame visibleCount overshoot.
    // The cull compute shader does atomicAdd(visibleCount, 1) unconditionally,
    // then early-returns if outSlot >= kMaxThinRecords.  So visibleCount can
    // climb past the cap; in that case the records that ran the atomicAdd
    // LAST get dropped — atomicAdd ordering is race-dependent → different
    // frames drop different records → thin-SSBO content varies between
    // compute write and VS read EVEN AT THE SAME RING SLOT.  This is the
    // candidate root cause for the "huge garbage triangle on fast rotation"
    // signature: cmd-patch clamps the count to 65536*6 but the records past
    // the kept records are race-dependent.
    //
    // Cost: one 4-byte readback per frame.  The fence wait above guarantees
    // all prior GPU commands are complete, so glGetBufferSubData is stall-
    // free here on AMD/NV (driver fast-path for already-signaled buffers).
    if (g_solidBucketHeaderSsbo != 0 && ringFrameIdx > 1) {
        // Read all 16 bytes of GpuDrivenBucketHeader:
        //   [0]=visibleCount, [1]=pad0_ (unused), [2]=pad1_ (near-zero clip.w count,
        //   probe 4), [3]=pad2_ (recipe-spread corruption count, probe 5).
        //
        // C-04 (non-blocking): only issue glGetBufferSubData when the fence for
        // probedSlot already signaled (GPU-done confirmed by the timeout=0 wait
        // above).  If the fence was absent / still in-flight, reuse the last
        // cached header values — a 1-frame-stale overshoot LOG is harmless.
        // This prevents a per-frame CPU stall on the default-ON GPU-driven path.
        static uint32_t s_hdr4Cached[4] = { 0, 0, 0, 0 };
        const bool gpuDone = (waitResult == GL_ALREADY_SIGNALED ||
                              waitResult == GL_CONDITION_SATISFIED);
        if (gpuDone) {
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_solidBucketHeaderSsbo);
            glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                               (GLsizeiptr)sizeof(s_hdr4Cached), s_hdr4Cached);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        }
        const uint32_t prevVisible = s_hdr4Cached[0];
        const uint32_t prevNearW   = s_hdr4Cached[2];
        const uint32_t prevSpread  = s_hdr4Cached[3];

        static uint32_t s_peakVisible    = 0;
        static uint32_t s_overshootCount = 0;
        static uint32_t s_peakNearW      = 0;
        static uint32_t s_nearWFrames    = 0;
        static uint64_t s_lastSummary    = 0;
        if (prevVisible > s_peakVisible) s_peakVisible = prevVisible;
        if (prevNearW   > s_peakNearW  ) s_peakNearW   = prevNearW;
        if (prevNearW > 0) ++s_nearWFrames;
        const bool overshot = (prevVisible >= (uint32_t)kMaxThinRecords);
        if (overshot) {
            ++s_overshootCount;
            PROBE_LOG(
                "[RING_OVERSHOOT v1] frame=%llu prev_visible=%u cap=%zu "
                "overshootCount=%u peak=%u mvpFp=0x%08x\n",
                (unsigned long long)ringFrameIdx, (unsigned)prevVisible,
                kMaxThinRecords, (unsigned)s_overshootCount,
                (unsigned)s_peakVisible, (unsigned)mvpFp);
        }
        // Probe 6 (FULL COVERAGE): compare thin[i].recipeIdx vs canary[i]
        // for EVERY i in [0, visibleCount).  Compute writes both at the same
        // outSlot in the SAME dispatch; canary buffer is never bound by the
        // bridge.  If for any i the two disagree, the thin SSBO has been
        // clobbered between compute write and our readback.
        //
        // User-reported symptom: giant grey-banded triangle that appears in
        // varied screen locations (skybox / mid-screen / behind terrain at
        // water level) frame-to-frame.  Only mechanism that explains location
        // variance is ONE thin record per frame having a corrupt recipeIdx
        // pointing to different recipes each occurrence.  Sample-based probe
        // (5 indices) almost certainly missed it; full coverage will catch it.
        //
        // Cost: visibleCount * 4 bytes for canary + visibleCount * 4 bytes for
        // thin.recipeIdx = ~80KB/frame at peak visibleCount=10000. PCIe stall
        // possible but acceptable for diagnostic.
        if (s_thinCanaryEnabled && g_thinCanarySSBO != 0 && g_thinRecordSSBO != 0 && prevVisible > 0) {
            const uint32_t vis = prevVisible < (uint32_t)kMaxThinRecords
                                ? prevVisible : (uint32_t)kMaxThinRecords;
            const int priorSlot = (g_thinRingSlot + kThinRingFrames - 1) % kThinRingFrames;
            const GLintptr thinSlotByteOffset = (GLintptr)(priorSlot * kThinRecordBytes);

            // Reuse heap buffers across frames to avoid alloc churn.
            static std::vector<uint32_t> s_canaryBuf;
            static std::vector<uint32_t> s_thinBlock;
            if (s_canaryBuf.size() < (size_t)vis * 2u) s_canaryBuf.resize((size_t)vis * 2u);
            const size_t thinDwords = (size_t)vis * 8u;
            if (s_thinBlock.size() < thinDwords) s_thinBlock.resize(thinDwords);

            // 1) Bulk-read canary[0..2*vis-1] (single readback, vis*8 bytes).
            //    Layout: [recipeIdx_0, flags_0, recipeIdx_1, flags_1, ...]
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_thinCanarySSBO);
            glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                               (GLsizeiptr)(vis * 2u * sizeof(uint32_t)),
                               s_canaryBuf.data());

            // 2) Read full thin records as packed block (vis * 32 bytes).
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_thinRecordSSBO);
            glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, thinSlotByteOffset,
                               (GLsizeiptr)(vis * 32u),
                               s_thinBlock.data());
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

            // 3) Compare BOTH recipeIdx AND flags.
            //    Thin record layout: dword 0=recipeIdx, dword 1=terrainHandle,
            //    dword 2=flags, dword 3=cementWord, dwords 4..7=lightRGB0..3.
            //    Canary layout: word 0=recipeIdx, word 1=flags (paired per record).
            static uint32_t s_recipeIdxMismatchFrames = 0;
            static uint64_t s_recipeIdxMismatchTotal  = 0;
            static uint32_t s_flagsMismatchFrames     = 0;
            static uint64_t s_flagsMismatchTotal      = 0;
            static uint32_t s_nextRecipeReportAt      = 1;
            static uint32_t s_nextFlagsReportAt       = 1;
            uint32_t recipeMismatchInFrame = 0, flagsMismatchInFrame = 0;
            uint32_t firstBadIdxR = 0, firstBadCanaryR = 0, firstBadThinR = 0;
            uint32_t firstBadIdxF = 0, firstBadCanaryF = 0, firstBadThinF = 0;
            for (uint32_t i = 0; i < vis; ++i) {
                const uint32_t canaryR = s_canaryBuf[i * 2u];
                const uint32_t canaryF = s_canaryBuf[i * 2u + 1u];
                const uint32_t thinR   = s_thinBlock[i * 8u];        // recipeIdx
                const uint32_t thinF   = s_thinBlock[i * 8u + 2u];   // flags
                if (canaryR != thinR) {
                    if (recipeMismatchInFrame == 0) {
                        firstBadIdxR    = i;
                        firstBadCanaryR = canaryR;
                        firstBadThinR   = thinR;
                    }
                    ++recipeMismatchInFrame; ++s_recipeIdxMismatchTotal;
                }
                if (canaryF != thinF) {
                    if (flagsMismatchInFrame == 0) {
                        firstBadIdxF    = i;
                        firstBadCanaryF = canaryF;
                        firstBadThinF   = thinF;
                    }
                    ++flagsMismatchInFrame; ++s_flagsMismatchTotal;
                }
            }
            if (recipeMismatchInFrame > 0) {
                ++s_recipeIdxMismatchFrames;
                if (s_recipeIdxMismatchFrames == s_nextRecipeReportAt) {
                    PROBE_LOG(
                        "[RING_CANARY_RECIPE v1] frame=%llu visible=%u prior_slot=%d "
                        "mismatch_in_frame=%u first_bad_idx=%u "
                        "first_bad_canary=%u first_bad_thin=%u "
                        "frames=%u total=%llu mvpFp=0x%08x\n",
                        (unsigned long long)ringFrameIdx, (unsigned)vis, priorSlot,
                        (unsigned)recipeMismatchInFrame, (unsigned)firstBadIdxR,
                        (unsigned)firstBadCanaryR, (unsigned)firstBadThinR,
                        (unsigned)s_recipeIdxMismatchFrames,
                        (unsigned long long)s_recipeIdxMismatchTotal, (unsigned)mvpFp);
                    s_nextRecipeReportAt = s_nextRecipeReportAt < 100
                        ? (s_nextRecipeReportAt + 1) : (s_nextRecipeReportAt * 10);
                }
            }
            if (flagsMismatchInFrame > 0) {
                ++s_flagsMismatchFrames;
                if (s_flagsMismatchFrames == s_nextFlagsReportAt) {
                    PROBE_LOG(
                        "[RING_CANARY_FLAGS v1] frame=%llu visible=%u prior_slot=%d "
                        "mismatch_in_frame=%u first_bad_idx=%u "
                        "first_bad_canary=0x%x first_bad_thin=0x%x "
                        "frames=%u total=%llu mvpFp=0x%08x\n",
                        (unsigned long long)ringFrameIdx, (unsigned)vis, priorSlot,
                        (unsigned)flagsMismatchInFrame, (unsigned)firstBadIdxF,
                        (unsigned)firstBadCanaryF, (unsigned)firstBadThinF,
                        (unsigned)s_flagsMismatchFrames,
                        (unsigned long long)s_flagsMismatchTotal, (unsigned)mvpFp);
                    s_nextFlagsReportAt = s_nextFlagsReportAt < 100
                        ? (s_nextFlagsReportAt + 1) : (s_nextFlagsReportAt * 10);
                }
            }
        }

        // Probe 5 tripwire: any frame where pad2_ > 0 means at least one
        // RECIPE entry has worldPos corners spanning more than 2.5 cells —
        // ground-truth evidence that the recipe SSBO is corrupt.  If this
        // fires AND the bug is visible, recipe data is the root cause.
        // If silent AND bug visible, recipe is fine — corruption is in the
        // VS or downstream.
        if (prevSpread > 0) {
            static uint32_t s_spreadFrames = 0;
            static uint32_t s_peakSpread   = 0;
            static uint32_t s_nextSpreadReportAt = 1;
            ++s_spreadFrames;
            if (prevSpread > s_peakSpread) s_peakSpread = prevSpread;
            if (s_spreadFrames == s_nextSpreadReportAt) {
                PROBE_LOG(
                    "[RING_SPREAD v1] frame=%llu spread_count=%u spread_frames=%u "
                    "peak_spread=%u prev_visible=%u mvpFp=0x%08x\n",
                    (unsigned long long)ringFrameIdx, (unsigned)prevSpread,
                    (unsigned)s_spreadFrames, (unsigned)s_peakSpread,
                    (unsigned)prevVisible, (unsigned)mvpFp);
                s_nextSpreadReportAt = s_nextSpreadReportAt < 100 ? (s_nextSpreadReportAt + 1)
                                     : (s_nextSpreadReportAt * 10);
            }
        }
        // Probe 4 tripwire: any frame where pad1_ > 0 means at least one quad
        // passed the pzOk gate while having a corner with |clip.w| < 0.05.
        // Strong candidate for the "huge garbage triangle" bug per
        // memory/clip_w_sign_trap.md.  Rate-limit by printing only on first
        // event AND on every 100x increase in s_nearWFrames so we don't
        // flood the log when the bug is continuous.
        if (prevNearW > 0) {
            static uint32_t s_nextNearWReportAt = 1;
            if (s_nearWFrames == s_nextNearWReportAt) {
                PROBE_LOG(
                    "[RING_NEARW v1] frame=%llu near_w_count=%u total_near_w_frames=%u "
                    "peak_near_w=%u prev_visible=%u mvpFp=0x%08x\n",
                    (unsigned long long)ringFrameIdx, (unsigned)prevNearW,
                    (unsigned)s_nearWFrames, (unsigned)s_peakNearW,
                    (unsigned)prevVisible, (unsigned)mvpFp);
                s_nextNearWReportAt = s_nextNearWReportAt < 100 ? (s_nextNearWReportAt + 1)
                                    : (s_nextNearWReportAt * 10);
            }
        }
        if (s_ringTrace) {
            printf("[RING v1] frame=%llu slot=%d fence=%d wait=0x%x wait_us=%llu "
                   "prev_visible=%u near_w=%u peak=%u peakNearW=%u mvpFp=0x%08x\n",
                (unsigned long long)ringFrameIdx, probedSlot, (int)fencePresent,
                (unsigned)waitResult, (unsigned long long)waitUs,
                (unsigned)prevVisible, (unsigned)prevNearW,
                (unsigned)s_peakVisible, (unsigned)s_peakNearW, (unsigned)mvpFp);
            fflush(stdout);
        }
        // Periodic peak summary even when MC2_RING_TRACE is off so manual
        // runs without env-var still surface the peak visible count.
        if (ringFrameIdx - s_lastSummary >= 600) {
            s_lastSummary = ringFrameIdx;
            PROBE_LOG(
                "[RING_PEAK v1] frame=%llu peak_visible=%u cap=%zu "
                "overshootCount=%u peak_near_w=%u near_w_frames=%u\n",
                (unsigned long long)ringFrameIdx, (unsigned)s_peakVisible,
                kMaxThinRecords, (unsigned)s_overshootCount,
                (unsigned)s_peakNearW, (unsigned)s_nearWFrames);
        }
    } else if (s_ringTrace) {
        printf("[RING v1] frame=%llu slot=%d fence=%d wait=0x%x wait_us=%llu mvpFp=0x%08x\n",
            (unsigned long long)ringFrameIdx, probedSlot, (int)fencePresent,
            (unsigned)waitResult, (unsigned long long)waitUs, (unsigned)mvpFp);
        fflush(stdout);
    }
    // ── end probe ─────────────────────────────────────────────────────────

    // C-02: if the fence for this slot was not signaled (timeout=0 above), the
    // prior GPU consumer is still reading it.  Skip the dispatch to preserve ring
    // safety; the slot fence is left intact so the next frame can retry.
    if (waitResult == GL_TIMEOUT_EXPIRED || waitResult == GL_WAIT_FAILED) {
        static uint32_t s_ringSkipCount = 0;
        ++s_ringSkipCount;
        PROBE_LOG("[RING_SKIP v1] frame=%llu slot=%d wait=0x%x skip_total=%u\n",
            (unsigned long long)ringFrameIdx, probedSlot,
            (unsigned)waitResult, (unsigned)s_ringSkipCount);
        return;
    }

    const GLintptr thinSlotOffset = (GLintptr)(g_thinRingSlot * kThinRecordBytes);

    // Step 2b: zero the indirect-cmd buffer's count slot (offset 0, 4 bytes)
    // every frame before the primary dispatch.  instanceCount / first /
    // baseInstance were initialized to {1,0,0} at allocation and are never
    // re-written (cmd-patch dispatch retired).
    {
        const uint32_t zero = 0u;
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, g_indirectCmdBuffer);
        glClearBufferSubData(GL_DRAW_INDIRECT_BUFFER, GL_R32UI, 0, 4,
                             GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
    }

    // Step 2b: bucket-header SSBO (when traced) gets the same per-frame clear
    // it always had.  No-op when MC2_BUCKET_HEADER_TRACE is unset because
    // g_solidBucketHeaderSsbo is 0.
    if (g_solidBucketHeaderSsbo != 0) {
        const uint32_t zero = 0u;
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_solidBucketHeaderSsbo);
        glClearBufferSubData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, 0, 16,
                             GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }

    // GPU-SYNC-CONTRACT: order the cmd-count clear (and the recipe/window/LUT
    // glBufferSubData uploads earlier this frame) BEFORE the cull/pack dispatch
    // below, which atomicAdds into cmds[0].count. Without this, NVIDIA may reorder
    // the clear after the dispatch -> atomicAdd accumulates onto a stale count ->
    // garbage/giant terrain triangles on fast camera motion. AMD tolerated the
    // omission. (Routed through the typed helper, not a hand-placed barrier.)
    gpuSyncBarrier(GpuProducer::ClearBuffer, GpuConsumer::ComputeShader,
                   "terrain_indirect_count_clear");

    // ------------------------------------------------------------------
    // DISPATCH 1: cull/pack (gpu_driven_terrain_solid.comp)
    // Bindings: 0=recipe, 1=lighting, 2=terrainHandleLut, 3=thin, 6=header,
    //           7=canary, 8=cmdbuf, 9=solid window (Approach A).
    // NOTE: the plan named binding 7 for the window SSBO, but on this branch
    // bindings 7 (canary) and 8 (cmdbuf) are already occupied (post-VPL
    // retirement additions that did not exist when 08bd3b2 freed binding 2).
    // Binding 9 is the first free slot — chosen to avoid the collision the
    // plan explicitly warned against.
    // ------------------------------------------------------------------
    MC2_GL_UseProgram(g_solidComputeProgram);

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, g_recipeSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, lightSsbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2,
                     g_terrainHandleLutSSBO ? g_terrainHandleLutSSBO : 0);
    gpuBindSsboRange(3, g_thinRecordSSBO,
                     (long long)thinSlotOffset, (long long)kThinRecordBytes,
                     "terrain.thinRecord");
    // Step 2b: bucket-header SSBO only bound when MC2_BUCKET_HEADER_TRACE
    // armed.  When unbound, the shader sees u_bucketHeaderTrace=0 and skips
    // all hdr.* writes (including the slot-counter atomicAdd on visibleCount).
    if (g_solidBucketHeaderSsbo != 0)
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, g_solidBucketHeaderSsbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, g_thinCanarySSBO);  // probe 6
    // Step 2b: bind the indirect-cmd buffer as SSBO at slot 8.  The primary
    // compute atomicAdds 6u into cmds[0].count per admitted record and uses
    // the returned-before-increment value as the thin-record slot index when
    // the bucket-header trace path is off.  Buffer is double-bound
    // (GL_DRAW_INDIRECT_BUFFER for the draw, GL_SHADER_STORAGE_BUFFER for
    // the atomicAdd).  Coherent qualifier on the shader-side declaration
    // covers cross-target visibility within the frame.
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 8, g_indirectCmdBuffer);
    // Approach A: camera-windowed recipe-index buffer.  Only meaningful when
    // s_solidUseWindow==1; bound unconditionally (when allocated) so the
    // shader's solidWin[] declaration always resolves.  When the window is
    // inactive the shader uses identity (vn0 = id) and never reads solidWin[].
    if (g_solidQuadWindowSsbo != 0)
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 9, g_solidQuadWindowSsbo);

    if (g_locSolidWC < 0 || g_locSolidMVP < 0) {
        fprintf(stderr,
            "[TERRAIN_INDIRECT v1] event=error msg=solid_compute_missing_uniform "
            "u_windowCount=%d u_terrainMVP=%d\n",
            g_locSolidWC, g_locSolidMVP);
        fflush(stderr);
        MC2_GL_UseProgram(0);
        ForceDisableArmingForProcess();
        return;
    }

    glUniform1i(g_locSolidWC, (int)windowCount);
    // Per-frame: the current map's side. Stale-from-first-map here = all-black on
    // any later different-size map (the shader's edge cull + index decompose use it).
    if (g_locSolidMS >= 0)
        glUniform1i(g_locSolidMS, (int)Terrain::realVerticesMapSide);
    if (g_locSolidAO >= 0)
        glUniform1i(g_locSolidAO, Terrain::terrainTextures2 != nullptr ? 1 : 0);
    // Step 2b: upload u_bucketHeaderTrace gate every frame.  Uniform values
    // reset across glUseProgram cycles on some drivers, so per-frame upload
    // is safer than once-at-compile.
    if (g_locSolidBHT >= 0)
        glUniform1i(g_locSolidBHT, s_bucketHeaderTrace ? 1 : 0);
    // Approach A: 1 == read vn0 from solidWin[id]; 0 == identity vn0 = id
    // (full-range fallback == current HEAD behavior).  CPU-side value, no
    // GPU readback.
    if (g_locSolidUW >= 0)
        glUniform1i(g_locSolidUW, s_solidUseWindow);
    // WATER-REFLECTION-CLIP-1: armed ONLY for the duration of the reflection
    // pass's own ComputeDispatch() call (see RenderWaterReflectionPass). 0 on
    // every other call site (main SOLID dispatch) -> byte-identical pzOk.
    if (g_locSolidReflPass >= 0)
        glUniform1i(g_locSolidReflPass, s_solidReflectionPassActive ? 1 : 0);

    {
        const float* mvp = gos_GetTerrainMVPMat4();
        if (!mvp) { MC2_GL_UseProgram(0); return; }
        glUniformMatrix4fv(g_locSolidMVP, 1, GL_FALSE, mvp);

        // ── [MVP_EARLY v1] MVP-PUBLISH-EARLY-HOIST proof line ──────────────
        // Gated MC2_MVP_EARLY_TRACE (default OFF), throttled. terrainDispatchSeq
        // is g_mvpDiagFrame observed HERE (at the compute snapshot's MVP read);
        // publishSeq is the same counter captured at the early publish in
        // Mission::update. ACCEPTANCE: with MC2_MVP_PUBLISH_EARLY=1 the early
        // publish is the LAST set_mvp before this read -> dispatch==publish
        // (current frame). With it OFF (and indirect armed) the only set_mvp
        // this frame happens later in the RENDER phase -> dispatch trails the
        // PRIOR frame's publish -> dispatch==publish-1 (the stale frame). Pure
        // reads of two extern longs; printf only on the throttled frames.
        {
            static const bool s_mvpEarlyTrace =
                (getenv("MC2_MVP_EARLY_TRACE") != nullptr);
            if (s_mvpEarlyTrace) {
                // Both externs are declared global-scope at file top.
                const long dispatchSeq = ::g_mvpDiagFrame;
                if (dispatchSeq == 1 || dispatchSeq == 5 ||
                    dispatchSeq == 30 || (dispatchSeq % 120) == 0) {
                    fprintf(stderr,
                        "[MVP_EARLY v1] frame=%ld publishSeq=%ld "
                        "terrainDispatchSeq=%ld armed=%d\n",
                        dispatchSeq, g_mvpEarlyPublishSeq, dispatchSeq,
                        IsFrameSolidArmed() ? 1 : 0);
                    fflush(stderr);
                }
            }
        }
        // Probe 8: fingerprint the MVP the COMPUTE uploaded (FNV-1a over rotation
        // + translation rows = first 12 floats).  Bridge compares against MVP
        // at draw time via gos_terrain_indirect_getDispatchMvpFp().
        uint32_t fp = 2166136261u;
        for (int k = 0; k < 12; ++k) {
            uint32_t bits = 0;
            memcpy(&bits, &mvp[k], sizeof(bits));
            fp ^= bits; fp *= 16777619u;
        }
        g_dispatchMvpFp       = fp;
        g_dispatchMvpFrameIdx = ringFrameIdx;
        // Water-consistency fix: full MVP snapshot (see decl). This is the
        // exact matrix terrain-solid's Fix-B clipPos bake uses this frame.
        memcpy(g_dispatchMvp16, mvp, sizeof(float) * 16);
        // RENDER-VIEW-CURRENCY-1 / VIEW-EPOCH-DEDUPE-1: stamp the snapshot with the
        // semantic VIEW-CONTENT epoch it was sourced under (bumps only on real camera
        // change, NOT on the redundant early+gamecam republish), so object/mech
        // consumers prove same-view currency without false-staling every frame.
        g_dispatchMvpViewEpoch = ::g_viewContentEpoch;

        // ── [DEPTH_TRANSITION v1] zoom-step depth-pop diagnostic ───────────
        // Env-gated MC2_DEPTH_TRANSITION_PROBE (cached static const bool;
        // silent default; matches the [WATER_DEPTHPROBE v2] idiom at
        // gos_terrain_water_stream.cpp:1429 — raw printf+fflush). HIGH STAKES:
        // this zoom-step depth-pop root-cause has been WRONG 3x. The probe
        // dumps each flat consumer's ACTUAL resulting screen-z for a FIXED
        // world point ONLY on the MVP-transition (zoom-step) frame so the
        // ~0.1-unit 1-frame pop is numerically visible. [WATER_DEPTHPROBE v2]
        // is structurally blind (it FNV-hashes the MVP, not the downstream
        // per-consumer depth-fudge). RenderDoc cannot capture a 1-frame
        // transient. ZERO behavior change: pure reads of g_dispatchMvp16 (the
        // exact matrix terrain-solid's Fix-B clipPos bake + the armed GPU
        // water fast path both project with this frame), live terrain_mvp_
        // (the matrix the decal overlay binds), and the cross-TU sampled CPU
        // water value; O(1)/frame; printf only on transition frames (latched).
        {
            static const bool s_depthTransProbe =
                (getenv("MC2_DEPTH_TRANSITION_PROBE") != nullptr);
            if (s_depthTransProbe) {
                // --- Transition detector -----------------------------------
                // maxDelta = max_i |g_dispatchMvp16[i] - prev[i]|. kEps chosen
                // so a smooth pan does NOT trip but a DISCRETE zoom-level step
                // does. Justification: a per-frame camera pan changes the MVP
                // translation row by a small continuous increment (sub-1e-4 in
                // the normalized clip-matrix entries per frame at normal pan
                // speed); MC2's zoom is a DISCRETE camera-distance step (the
                // bug only reproduces on zoom/elevation-change, NOT pan — see
                // CLAUDE.md Known issues), which jumps several matrix entries
                // (projection scale + translation) by >>1e-4 in one frame. A
                // smooth same-magnitude pan would also trip a too-small kEps,
                // so kEps=1e-3f is used: large enough that normalized
                // per-frame pan deltas (empirically ~1e-5..1e-4 in these
                // entries) stay below it, small enough that a discrete zoom
                // step (whole-number camera-distance change -> matrix entry
                // jumps ~1e-2+) trips it decisively. This is a DIAGNOSTIC
                // gate, not a control path; over/under-trip only changes which
                // frames print, never rendering.
                static float g_prevDispatchMvp16[16] = { 0.0f };
                static bool  s_prevValid = false;
                static unsigned long long s_dtFrame = 0;
                const unsigned long long f = ++s_dtFrame;
                const float kEps = 1.0e-3f;
                float maxDelta = 0.0f;
                for (int i = 0; i < 16; ++i) {
                    const float d =
                        std::abs(g_dispatchMvp16[i] - g_prevDispatchMvp16[i]);
                    if (d > maxDelta) maxDelta = d;
                }
                const bool transition = s_prevValid && (maxDelta > kEps);

                // --- Fixed probe point P -----------------------------------
                // Deterministic mid-map XY at the water plane. FRAME-STABLE
                // (map geometry constants + waterElevation, NOT camera): the
                // dz across the transition is therefore attributable purely to
                // the per-consumer depth-fudge x MVP interaction, not point
                // motion. MC2 world layout (terrain.h:367-368): X increases
                // from mapTopLeft3d.x, Y decreases from mapTopLeft3d.y.
                const float Px = Terrain::mapTopLeft3d.x
                                 + Terrain::worldUnitsMapSide * 0.5f;
                const float Py = Terrain::mapTopLeft3d.y
                                 - Terrain::worldUnitsMapSide * 0.5f;
                const float Pz = Terrain::waterElevation;

                // Project P with a given GL_FALSE row-major MVP using the
                // file's authoritative convention (see :1881-1898 and
                // memory/terrain_mvp_gl_false.md): GLSL reads our row-major
                // bytes column-major, so clip[i] = sum_j m[j*4+i]*P[j]. This
                // is byte-identical to gpu_driven_terrain_solid.comp's
                // projectClip() (terrain-solid's clipPos source, read verbatim
                // by gos_terrain_thin.vert:210) and to the `terrainMVP*vec4`
                // in gos_terrain_water_fast_mdi.vert:283 and
                // terrain_overlay.vert:31 (same upload convention).
                auto projZ = [](const float* m,
                                 float x, float y, float z,
                                 float& outZ, float& outW) {
                    const float cz =
                        m[0 * 4 + 2] * x + m[1 * 4 + 2] * y +
                        m[2 * 4 + 2] * z + m[3 * 4 + 2];
                    const float cw =
                        m[0 * 4 + 3] * x + m[1 * 4 + 3] * y +
                        m[2 * 4 + 3] * z + m[3 * 4 + 3];
                    outZ = cz; outW = cw;
                };

                // terrain SOLID: mirror gos_terrain_thin.vert:219 exactly.
                // clip = (dispatch MVP) * P (the compute bakes tr.clipPos with
                // EXACTLY g_dispatchMvp16 -- see :1521 decl + :1894 bake);
                // screen.z = clip.z/clip.w + TERRAIN_DEPTH_FUDGE (0.002,
                // single-sourced mc2depth::TERRAIN_DEPTH_FUDGE).
                float czT, cwT;
                projZ(g_dispatchMvp16, Px, Py, Pz, czT, cwT);
                const float z_terr =
                    (czT / cwT) + mc2depth::TERRAIN_DEPTH_FUDGE;

                // GPU WATER (fast MDI): mirror
                // gos_terrain_water_fast_mdi.vert:291 exactly. The armed water
                // fast path binds gos_terrain_indirect_getDispatchMvp16()
                // (== g_dispatchMvp16; gos_terrain_water_stream.cpp:1409-1413)
                // and ComputeDispatch only runs armed, so this is the live
                // GPU-water producer's exact matrix this frame. screen.z =
                // clip.z/clip.w + WATER_DEPTH_FUDGE_FAST (0.0025).
                const float z_gpuw =
                    (czT / cwT) + mc2depth::WATER_DEPTH_FUDGE_FAST;

                // DECAL (post-Fix B, probe==producer): the armed overlay/
                // decal callers (drawTerrainOverlays / drawDecalStaticBatch /
                // drawDecals) bind the symmetric-mirror MVP -- armed ->
                // gos_terrain_indirect_getDispatchMvp16() == g_dispatchMvp16,
                // the SAME matrix z_terr/z_gpuw use here (uploadOverlayUniforms_
                // in gameos_graphics.cpp) -- and terrain_overlay.vert adds
                // OVERLAY_DEPTH_BIAS (single-sourced mc2depth). This code is
                // armed-only (ComputeDispatch), so the faithful producer model
                // is czT/cwT + OVERLAY_DEPTH_BIAS, co-planar with z_terr/z_gpuw
                // by construction (Fix B). Retired the pre-Fix-B model (stale
                // liveMvp gos_GetTerrainMVPMat4 + kPolyOffsetNdcApprox modelling
                // the REMOVED glPolygonOffset(-1,-1)); that produced a
                // counterfactual constant dz_decal blind to the fix
                // (parity_probe_100pct: probe must equal producer). z_decal is
                // now exact, not an approximation.
                const float z_decal =
                    (czT / cwT) + mc2depth::OVERLAY_DEPTH_BIAS;
                const int decalOk = 1;

                // CPU WATER: SAMPLED real produced value (NOT a CPU re-derive;
                // the Stuff eye->projectForTerrainAdmission pipeline is not
                // reachable here). TerrainQuad::drawWater (mclib/quad.cpp
                // ~:3345) latches vertices[k]->wz + WATER_DEPTH_FUDGE (raster
                // 0.0025) for the water vertex nearest the SAME fixed P.
                // CRITICAL: CPU water and the GPU fast path are mutually
                // exclusive per frame (terrain.cpp:1225-1229). ComputeDispatch
                // (this code) only runs ARMED, i.e. exactly when CPU water is
                // SKIPPED -- so g_cpuWaterProbeZ is the LAST un-armed steady
                // sample, NOT this-frame data. We emit cpuw_stamp + its delta
                // vs the previous dump so a stale CPU value is VISIBLE and
                // never silently trusted (we have been wrong 3x trusting
                // models over real data; a flagged-stale real value is honest
                // evidence, a fabricated this-frame CPU value would be false).
                // (declared at global file scope above - unqualified lookup
                // here binds the global ::g_cpuWaterProbe* defined in quad.cpp)
                const float z_cpuw = g_cpuWaterProbeZ;
                const int   cpuwAny = g_cpuWaterProbeAny ? 1 : 0;
                static unsigned long long s_prevCpuStamp = 0;
                const unsigned long long cpuStampNow = g_cpuWaterProbeStamp;

                // --- Emit (transition frames only, latched) ----------------
                // Also emit the 1 steady frame immediately BEFORE a transition
                // (one-frame-back latch) and the 1 steady frame AFTER, so the
                // transition-frame dz can be compared to adjacent steady dz.
                // s_emitTail counts down post-transition steady frames to
                // print; s_havePrevSteady stashes the immediately-preceding
                // steady line so we can print it retroactively when a
                // transition is detected (cheap: 4 floats).
                struct DtLine {
                    unsigned long long f; float md; float zt, zc, zg, zd;
                    int cAny; unsigned long long cStamp;
                };
                static DtLine s_prevSteady = {0,0,0,0,0,0,0,0};
                static bool   s_havePrevSteady = false;
                static int    s_emitTail = 0;

                DtLine cur;
                cur.f = f; cur.md = maxDelta;
                cur.zt = z_terr; cur.zc = z_cpuw;
                cur.zg = z_gpuw; cur.zd = z_decal;
                cur.cAny = cpuwAny; cur.cStamp = cpuStampNow;

                auto emitLine = [&](const DtLine& L, const char* kind) {
                    const long long cpuStampDelta =
                        (long long)L.cStamp - (long long)s_prevCpuStamp;
                    // cpuw_fresh=1 only if the CPU sampler advanced its stamp
                    // since the previous emitted line AND it found a vertex.
                    const int cpuwFresh =
                        (L.cAny && cpuStampDelta != 0) ? 1 : 0;
                    printf("[DEPTH_TRANSITION v1] kind=%s f=%llu "
                           "maxMvpDelta=%.7f "
                           "z_terr=%.7f z_cpuw=%.7f z_gpuw=%.7f z_decal=%.7f "
                           "dz_cpuw=%.7f dz_gpuw=%.7f dz_decal=%.7f "
                           "cpuw_fresh=%d cpuw_any=%d cpuw_stamp=%llu "
                           "cpuw_stamp_delta=%lld decal_ok=%d decal_z_approx=0\n",
                           kind,
                           (unsigned long long)L.f,
                           (double)L.md,
                           (double)L.zt, (double)L.zc,
                           (double)L.zg, (double)L.zd,
                           (double)(L.zc - L.zt),
                           (double)(L.zg - L.zt),
                           (double)(L.zd - L.zt),
                           cpuwFresh, L.cAny,
                           (unsigned long long)L.cStamp,
                           cpuStampDelta, decalOk);
                    fflush(stdout);
                    s_prevCpuStamp = L.cStamp;
                };

                if (transition) {
                    if (s_havePrevSteady)
                        emitLine(s_prevSteady, "pre");
                    emitLine(cur, "trans");
                    s_emitTail = 1;  // print 1 steady frame after
                } else if (s_emitTail > 0) {
                    emitLine(cur, "post");
                    --s_emitTail;
                }
                // Always stash the latest (steady or not) for the next
                // transition's retroactive "pre" line.
                s_prevSteady = cur;
                s_havePrevSteady = true;

                // Maintain prev MVP snapshot AFTER the diff (copy current ->
                // prev). s_prevValid gates the first frame (no prior to diff).
                memcpy(g_prevDispatchMvp16, g_dispatchMvp16,
                       sizeof(float) * 16);
                s_prevValid = true;
            }
        }
        // ── end [DEPTH_TRANSITION v1] ──────────────────────────────────────

        // Probe 8b: stash first 4 floats so bridge can log byte-level delta.
        g_dispatchMvpFloats[0] = mvp[0];
        g_dispatchMvpFloats[1] = mvp[1];
        g_dispatchMvpFloats[2] = mvp[2];
        g_dispatchMvpFloats[3] = mvp[3];
        // Fix A: full 16-float snapshot for the bridge to re-upload at draw
        // time.  Indexed by the current ring slot — compute is about to write
        // thin records to g_thinRingSlot using this MVP, so the bridge must
        // project them with the same MVP one frame later.
        // Step 9 (2026-05-15): demoted behind MC2_RING_TRACE.  Default-off ⇒
        // the snapshot is not written; gos_terrain_indirect_getRingSlotMvp()
        // returns nullptr and the (already inert post-Fix-B) bridge override
        // is skipped.  One-shot lifecycle line per the Debug-instrumentation
        // rule, matching the step 8c-2 event=retired one-shots.
        if (g_envRingTrace) {
            memcpy(g_thinSlotMVP[g_thinRingSlot], mvp, sizeof(float) * 16);
            g_thinSlotMVPValid[g_thinRingSlot] = true;
        } else {
            static bool s_loggedFixADemote = false;
            if (!s_loggedFixADemote) {
                s_loggedFixADemote = true;
                printf("[RING_MVP v1] event=fixA_demoted gate=MC2_RING_TRACE\n");
                fflush(stdout);
            }
        }
    }

    const uint32_t groups = (windowCount + 63u) / 64u;
    if (groups > 0) glDispatchCompute(groups, 1, 1);
    // Step 2b (VPL retirement): single barrier replaces the prior pair.  Both
    // bits are required:
    //   - GL_SHADER_STORAGE_BARRIER_BIT: fences thin[] + (when traced) hdr[]
    //     writes against the VS that reads thin[] at draw time.
    //   - GL_COMMAND_BARRIER_BIT: fences cmds[0].count atomicAdd writes
    //     against the subsequent glMultiDrawElementsIndirect's read of the
    //     indirect-cmd buffer.  Without this bit, AMD drivers can re-order
    //     the indirect-cmd fetch ahead of the compute write (NVIDIA tolerant,
    //     AMD failing per plan §"Load-bearing constraints").
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);

    // ── Probe 7 (MC2_RING_FORCE_FINISH) — directed barrier-hypothesis test ──
    // Forces full GPU pipeline drain after the compute dispatches.
    // If the giant-triangle bug DISAPPEARS with this on, the barrier is
    // insufficient on AMD — VS reads stale thin records via L1$ even though
    // CPU readback sees correct data in VRAM (probe 6 full coverage silent).
    // Severe perf cost (every-frame stall); diagnostic-only, default OFF.
    static const bool s_ringForceFinish = (getenv("MC2_RING_FORCE_FINISH") != nullptr);
    if (s_ringForceFinish) {
        glFinish();
    }
    // ── end probe 7 ────────────────────────────────────────────────────────

    // ── Probe 3 (MC2_RING_TRACE) — read back the indirect cmd's count after patch ──
    // If a cmd-patch race is occurring under fast rotation, cmd.count may not equal
    // min(visibleCount, kMaxThinRecords) * 6.  Reading it here after the
    // GL_COMMAND_BARRIER_BIT means the GPU has finished cmd-patch; no stall.
    // Cost: one 16-byte readback per frame (one DrawArraysIndirectCommand).
    if (s_ringTrace && g_indirectCmdBuffer != 0) {
        uint32_t cmdQuad[4] = { 0, 0, 0, 0 };
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, g_indirectCmdBuffer);
        glGetBufferSubData(GL_DRAW_INDIRECT_BUFFER, 0, (GLsizeiptr)sizeof(cmdQuad), cmdQuad);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
        // Step 2b: with cmd-patch retired the [RING_CMDPATCH] tripwire can no
        // longer recompute expected from hdr.visibleCount unless the bucket-
        // header SSBO is also armed (MC2_BUCKET_HEADER_TRACE).  When it is
        // off, cmdQuad[0] IS the authoritative count (primary's atomicAdd
        // accounting) so there is nothing to cross-check; we still surface
        // the raw cmd values via the existing RING_PEAK summary.
        uint32_t vis = 0;
        uint32_t expectedCount = cmdQuad[0];
        if (g_solidBucketHeaderSsbo != 0) {
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_solidBucketHeaderSsbo);
            glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)sizeof(vis), &vis);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
            expectedCount =
                (vis < (uint32_t)kMaxThinRecords ? vis : (uint32_t)kMaxThinRecords) * 6u;
        }
        if (g_solidBucketHeaderSsbo != 0 && cmdQuad[0] != expectedCount) {
            static uint32_t s_cmdMismatchCount = 0;
            ++s_cmdMismatchCount;
            PROBE_LOG(
                "[RING_CMDPATCH v1] frame=%llu cmd_count=%u expected=%u "
                "visible=%u inst=%u first=%u base=%u mismatchCount=%u\n",
                (unsigned long long)ringFrameIdx, (unsigned)cmdQuad[0],
                (unsigned)expectedCount, (unsigned)vis,
                (unsigned)cmdQuad[1], (unsigned)cmdQuad[2], (unsigned)cmdQuad[3],
                (unsigned)s_cmdMismatchCount);
        }
    }
    // ── end probe 3 ─────────────────────────────────────────────────────────

    // Step 2b: parity probe retired with cmd-patch (commit 2a's safety net no
    // longer needed once cmd-patch is gone -- there is nothing to compare
    // against).  The surviving MC2_RING_TRACE probe below still reads
    // cmds[0].count for the [RING_CMDPATCH] tripwire; its expected-value
    // calculation now derives from the primary's atomicAdd accounting
    // (see comment in that block).

    MC2_GL_UseProgram(0);

    // Restore compute-only slots to clean state.
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, 0);
    // Step 2b: unbind the indirect-cmd SSBO at slot 8 to keep the SSBO
    // bind-points clean for the next compute pipeline (water-stream cmd-patch
    // uses different bindings but symmetry helps debug).
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 8, 0);
    // Step 2b: unbind slot 6 only if we bound it (bucket-header SSBO trace path).
    if (g_solidBucketHeaderSsbo != 0)
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, 0);
    // Leave slot 3 as the thin-record range (DrawIndirect's bridge reads it via
    // gos_terrain_bridge_drawIndirect which re-binds by recipeSSBO/thinRecordSSBO
    // arguments — no leak risk).

    s_solidGpuDispatchRanThisFrame = true;
}

bool DrawIndirect() {
    if (!IsFrameSolidArmed()) {
        static bool s_warnedOnce = false;
        if (!s_warnedOnce) {
            s_warnedOnce = true;
            fprintf(stderr, "[TERRAIN_INDIRECT v1] event=draw_indirect_skip "
                   "armed=%d process_disabled=%d\n",
                   (int)s_frameSolidArmed, (int)s_processArmingDisabled);
            fflush(stderr);
        }
        return false;
    }
    ZoneScopedN("Terrain::SolidDrawIndirect");
    TracyGpuZone("Terrain::SolidDrawIndirect");

    const bool ok = gos_terrain_bridge_drawIndirect(
        s_frameSolidCmdCount,
        static_cast<unsigned int>(g_recipeSSBO),
        static_cast<unsigned int>(g_thinRecordSSBO),
        static_cast<unsigned int>(g_indirectCmdBuffer));

    if (!ok) {
        // Hard failure post-arming: gate-off already fired, cannot recover.
        static bool s_hardFailureLogged = false;
        if (!s_hardFailureLogged) {
            s_hardFailureLogged = true;
            fprintf(stderr,
                "[TERRAIN_INDIRECT v1] event=hard_failure reason=bridge_returned_false "
                "thin_count=%d cmd_count=%d "
                "advice=set MC2_TERRAIN_INDIRECT=0 to fall back to M2 legacy SOLID\n",
                s_frameSolidPackedThinCount, s_frameSolidCmdCount);
            fflush(stderr);
        }
        ForceDisableArmingForProcess();
        return false;
    }

    // first_draw lifecycle print — once per mission via the mission-latch.
    // s_firstDrawPrintedThisMission is declared in the Stage 2 anonymous ns;
    // it's reset by ResetDenseRecipe() at mission teardown.
    if (!s_firstDrawPrintedThisMission) {
        s_firstDrawPrintedThisMission = true;
        fprintf(stderr, "[TERRAIN_INDIRECT v1] event=first_draw "
               "thin_count=%d cmd_count=%d ring_slot=%d\n",
               s_frameSolidPackedThinCount, s_frameSolidCmdCount, g_thinRingSlot);
        fflush(stderr);
    }

    // Ring fence for the slot just drawn.
    if (g_thinRingFences[g_thinRingSlot]) {
        glDeleteSync(g_thinRingFences[g_thinRingSlot]);
    }
    g_thinRingFences[g_thinRingSlot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

    // TERRAIN-INDIRECT-LATCH-FIX-1: like the chunk/patch-stream/legacy branches, the
    // Indirect terrain draw MUST set the terrain-drawn latch. sceneHasTerrain_ gates 5
    // post passes (screenShadow / cloudShadow / shoreline / edgeFog-godrays / clear-color);
    // without this, MC2_TERRAIN_LOD_CHUNK=0 + armed-camera + indirect draws terrain but
    // silently kills those passes (the dead-cloud-shadow bug the other branches were fixed
    // for; TERRAIN-SUBPASS-RECON-1 §4). Gated on commands actually issued so a terrainless
    // frame doesn't enable post (mirrors the legacy extras>0 caution). Suppressed by the
    // chunk default today -> byte-identical on the default path.
    if (s_frameSolidCmdCount > 0) {
        if (gosPostProcess* pp = getGosPostProcess()) pp->markTerrainDrawn();
        RenderCore::framegraph::noteTerrainPath(RenderCore::framegraph::TerrainPath::IndirectBridge);  // TERRAIN-PATH-TELEMETRY-1
    }

    return true;
}

// WATER-TERRAIN-REFLECTION-1 (Phase C1): render mirrored terrain-only into the
// quarter-res water reflection RT. Gate MC2_WATER_REFLECTION_RT default OFF ->
// no-op -> byte-identical. Called from gamecam AFTER renderLists (main SOLID
// DrawIndirect already consumed its ring slot; atlases warm) and BEFORE water.
// Installs a mirror MVP, re-dispatches the SOLID compute (advances to a fresh
// ring slot + refills the shared cmd buffer), draws into the reflection FBO,
// then RESTORES the production MVP (LOAD-BEARING: ~15 same-frame consumers read
// gos_GetTerrainMVPMat4 after this — water fast path, decals, overlays, cull).
// Terrain only: no water/props/mechs. No clip plane (-> WATER-REFLECTION-CLIP-1).
void RenderWaterReflectionPass() {
    static const bool s_rtEnabled = [](){
        const char* v = getenv("MC2_WATER_REFLECTION_RT");
        return v && v[0] && v[0] != '0';
    }();
    if (!s_rtEnabled)          return;   // gate OFF -> byte-identical
    if (!IsFrameSolidArmed())  return;   // no terrain this frame
    const float* M = gos_GetTerrainMVPMat4();
    if (!M)                    return;
    gosPostProcess* pp = getGosPostProcess();
    if (!pp)                   return;
    GLuint fbo = (GLuint)pp->getWaterReflectionFBO();
    int rw = pp->getWaterReflectionWidth();
    int rh = pp->getWaterReflectionHeight();
    if (fbo == 0 || rw < 1 || rh < 1) return;

    // Mirror MVP: clip = G * R * world, where R reflects MC2 world Z across the
    // water plane (z' = 2*we - z). With G[r][c] = M[c*4+r] (M uploaded GL_FALSE),
    // R only touches columns 2,3: negate column 2; column3 += 2*we * orig col2.
    const float we2 = 2.0f * (float)Terrain::waterElevation;
    float saved[16]; for (int i = 0; i < 16; ++i) saved[i] = M[i];
    float mir[16];   for (int i = 0; i < 16; ++i) mir[i]   = M[i];
    for (int r = 0; r < 4; ++r) {
        mir[12 + r] = saved[12 + r] + we2 * saved[8 + r];  // col3 += 2we*col2(orig)
        mir[8  + r] = -saved[8 + r];                        // col2 negated
    }

    // Save all GL state we touch BEFORE any state change (FBO/viewport/clear
    // color). glClearDepth(0.0) == the engine's reverse-Z default, so no restore.
    GLint prevFBO = 0; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);
    GLint prevVP[4];   glGetIntegerv(GL_VIEWPORT, prevVP);
    GLfloat prevCC[4]; glGetFloatv(GL_COLOR_CLEAR_VALUE, prevCC);

    // LOAD-BEARING: ComputeDispatch() also bakes the MVP into the shared
    // g_dispatchMvp16 snapshot, which the GPU WATER fast path reads as its
    // u_worldToClipGL (gameos_graphics.cpp getDispatchMvp16). Restoring only
    // terrain_mvp_ (gos_SetTerrainMVP) is NOT enough -> water would be drawn with
    // the MIRROR matrix and vanish/flicker. Save + restore the snapshot too.
    float    savedDispatchMvp[16]; memcpy(savedDispatchMvp, g_dispatchMvp16, sizeof(savedDispatchMvp));
    uint32_t savedDispatchFp       = g_dispatchMvpFp;
    uint64_t savedDispatchFrameIdx = g_dispatchMvpFrameIdx;

    gos_SetTerrainMVP(mir);   // install mirror -> compute bakes mirrored clip
    // WATER-REFLECTION-CLIP-1: arm the reflection-pass pzOk relaxation ONLY for
    // this ComputeDispatch() call, then disarm immediately after -- the flag is
    // read once per dispatch (uploaded inside ComputeDispatch itself) so there is
    // no window where the main SOLID pass could see it armed.
    s_solidReflectionPassActive = true;
    ComputeDispatch();        // fresh ring slot + refills shared cmd buffer
    s_solidReflectionPassActive = false;

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, rw, rh);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);   // alpha=0 = SH-sky fallback marker (C2)
    glClearDepth(0.0);                        // reverse-Z: far plane = 0 (GEQUAL)
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // WATER-REFLECTION-CLIP-1: cheap mirrored-draw-count proof. Read back just
    // the indirect cmd's DrawArraysIndirectCommand.count (16 bytes) rather than
    // the whole RT -- this is the count of mirrored VERTICES the CLIP-1-relaxed
    // pzOk gate admitted this dispatch (0 == the pre-fix bug: everything culled).
    // Still a GL sync point (glGetBufferSubData waits for the write), but is
    // ~4-5 orders of magnitude less data than the recon's landmine-6
    // whole-RT glReadPixels(GL_RGBA, GL_FLOAT) stall, which is now opt-in only
    // (MC2_WATER_REFL_RT_PIXELPROOF=1) for deep debugging.
    uint32_t mirroredCmdCount = 0;
    {
        GLint prevDIB = 0; glGetIntegerv(GL_DRAW_INDIRECT_BUFFER_BINDING, &prevDIB);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, g_indirectCmdBuffer);
        glGetBufferSubData(GL_DRAW_INDIRECT_BUFFER, 0, (GLsizeiptr)sizeof(mirroredCmdCount), &mirroredCmdCount);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, (GLuint)prevDIB);
    }

    // WATER-REFLECTION-CLIP-1: pzOk (compute-side) now ADMITS far-mirrored quads
    // (see u_reflectionPass), but hardware near/far clipping happens again at
    // the rasterizer regardless of what the compute gate decided -- a quad with
    // clip.z > clip.w still gets clipped/discarded by the GPU, not just culled
    // by our own gate. GL_DEPTH_CLAMP disables that hardware far-plane clip
    // (clamps depth to [0,1] instead of discarding), matching the recon's
    // "relax pzOk + GL_DEPTH_CLAMP" interim-fix pairing. Scoped to this pass
    // only; restored immediately after (was disabled engine-wide before this).
    const GLboolean depthClampWasEnabled = glIsEnabled(GL_DEPTH_CLAMP);
    if (!depthClampWasEnabled) glEnable(GL_DEPTH_CLAMP);

    const bool drew = DrawIndirect();          // bridge inherits this FBO+viewport

    if (!depthClampWasEnabled) glDisable(GL_DEPTH_CLAMP);

    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFBO);
    glViewport(prevVP[0], prevVP[1], prevVP[2], prevVP[3]);
    glClearColor(prevCC[0], prevCC[1], prevCC[2], prevCC[3]);  // restore clear color

    gos_SetTerrainMVP(saved);   // RESTORE production MVP (load-bearing)
    // RESTORE the dispatch snapshot the water fast path consumes (see above).
    memcpy(g_dispatchMvp16, savedDispatchMvp, sizeof(g_dispatchMvp16));
    g_dispatchMvpFp       = savedDispatchFp;
    g_dispatchMvpFrameIdx = savedDispatchFrameIdx;

    // Throttled diagnostics. Default: cheap count-only proof (mirroredCmdCount,
    // read back above -- BEFORE the restore, since g_indirectCmdBuffer is
    // per-dispatch-shared and DrawIndirect()/the next frame's main dispatch may
    // reuse the slot). Opt-in MC2_WATER_REFL_RT_PIXELPROOF=1 additionally does
    // the old whole-RT glReadPixels coverage/alpha_cov breakdown for deep debug.
    static long s_frame = 0; ++s_frame;
    if (s_frame == 1 || s_frame == 5 || s_frame == 30 || s_frame == 120 ||
        (s_frame % 600) == 0) {
        static const bool s_pixelProof = [](){
            const char* v = getenv("MC2_WATER_REFL_RT_PIXELPROOF");
            return v && v[0] && v[0] != '0';
        }();
        if (!s_pixelProof) {
            fprintf(stderr, "[WATER_REFL_RT v1] frame=%ld gate=1 fbo=%u dims=%dx%d drew=%d "
                    "mirrored_cmd_count=%u\n",
                    s_frame, (unsigned)fbo, rw, rh, (int)drew, (unsigned)mirroredCmdCount);
            fflush(stderr);
        } else {
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            std::vector<float> px((size_t)rw * rh * 4, 0.0f);
            glReadPixels(0, 0, rw, rh, GL_RGBA, GL_FLOAT, px.data());
            glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFBO);
            int n = rw * rh, colorHits = 0, alphaHits = 0; float maxL = 0.0f; double acc = 0.0;
            for (int i = 0; i < n; ++i) {
                float L = 0.2126f*px[i*4] + 0.7152f*px[i*4+1] + 0.0722f*px[i*4+2];
                if (L > 0.001f)        ++colorHits;
                if (px[i*4+3] > 0.001f) ++alphaHits;
                if (L > maxL) maxL = L;
                acc += L;
            }
            GLenum err = glGetError();
            fprintf(stderr, "[WATER_REFL_RT v1] frame=%ld gate=1 fbo=%u dims=%dx%d drew=%d "
                    "mirrored_cmd_count=%u coverage=%.1f%% alpha_cov=%.1f%% max_luma=%.4f "
                    "avg_luma=%.5f gl_err=0x%x\n",
                    s_frame, (unsigned)fbo, rw, rh, (int)drew, (unsigned)mirroredCmdCount,
                    100.0*colorHits/(n?n:1), 100.0*alphaHits/(n?n:1), maxL,
                    (n?acc/n:0.0), (unsigned)err);
            fflush(stderr);
        }
    }
}

// VPL parity-infra retirement (cpu-pack-retirement plan §7 OQ-2, full
// delete): gos_terrain_indirect::ComputeDispatchParity_Check() removed.
// Both retirement gates clear (Step 4 2e11617 retired the last accessor
// consumer; soak-waiver) and the txmmgr coupling is resolved by removing
// that call in this same commit. WaterStream::ComputeDispatchParity_Check
// (gos_terrain_water_stream.cpp) is a SEPARATE symbol and is unaffected.
// See memory/mc_texture_manager_dual_queue_legacy_retirement_debt.md.

}  // namespace gos_terrain_indirect  // Stage 3 block

// Bridge accessor for the current thin-record ring slot (used by
// gos_terrain_bridge_drawIndirect to compute glBindBufferRange offset).
int gos_terrain_indirect_getRingSlot() {
    return g_thinRingSlot;
}

// Fix A: returns the per-slot MVP snapshot for the current ring slot, or
// nullptr if no MVP has been stashed yet (early frames before any pack /
// dispatch).  Bridge re-uploads this over terrain_mvp_ so the thin VS
// projects records with the MVP they were culled by.
// Step 9 (2026-05-15): demoted behind MC2_RING_TRACE.  When off (default) the
// writers above never populate g_thinSlotMVPValid[], so this would return
// nullptr regardless; the explicit early-out makes the demote contract
// self-documenting and guarantees zero snapshot work on the default render
// path (the gameos_graphics.cpp:2532 caller handles nullptr by skipping the
// terrainOverrideThinMVP override — itself a no-op post-Fix-B since the thin
// VS declares no terrainMVP uniform; see gos_terrain_thin.vert:56-61).
const float* gos_terrain_indirect_getRingSlotMvp() {
    if (!g_envRingTrace) return nullptr;
    const int slot = g_thinRingSlot;
    if (slot < 0 || slot >= kThinRingFrames) return nullptr;
    if (!g_thinSlotMVPValid[slot]) return nullptr;
    return g_thinSlotMVP[slot];
}

// Slice B4 Stage 1b — C-linkage accessors used by gos_terrain_mask_dispatch::DrawMaskSolid.
// Probe 8: MVP fingerprint accessors also live here (C linkage for cross-TU call from bridge).
extern "C" {

unsigned int gos_terrain_indirect_getRecipeSSBO() {
    return (unsigned int)g_recipeSSBO;
}

int gos_terrain_indirect_getRecipeMapSide() {
    return (int)g_recipeMapSide;
}

int gos_terrain_indirect_getRecipeQuadCount() {
    // The dense recipe is indexed by vertexNum = mx + my*mapSide, sized mapSide^2.
    return (int)g_recipeMapSide * (int)g_recipeMapSide;
}

uint32_t gos_terrain_indirect_getDispatchMvpFp() {
    return g_dispatchMvpFp;
}

// Water-consistency fix (2026-05-17): the full MVP terrain-solid baked its
// Fix-B clipPos with this frame. Callers MUST gate on IsFrameSolidArmed()
// (only then did ComputeDispatch run + refresh this); otherwise it is stale.
const float* gos_terrain_indirect_getDispatchMvp16() {
    return g_dispatchMvp16;
}

uint64_t gos_terrain_indirect_getDispatchMvpFrameIdx() {
    return g_dispatchMvpFrameIdx;
}

// RENDER-VIEW-CURRENCY-1: view epoch (g_mvpDiagFrame) the live dispatch snapshot
// was sourced under. Consumers compare against the current g_mvpDiagFrame.
long gos_terrain_indirect_getDispatchMvpViewEpoch() {
    return g_dispatchMvpViewEpoch;
}

void gos_terrain_indirect_getDispatchMvpFloats4(float out[4]) {
    out[0] = g_dispatchMvpFloats[0];
    out[1] = g_dispatchMvpFloats[1];
    out[2] = g_dispatchMvpFloats[2];
    out[3] = g_dispatchMvpFloats[3];
}

}  // extern "C"

// ---------------------------------------------------------------------------
// PR2c Stage 1c — mine static-bake infrastructure.
//
// Builds a per-mission mine VBO + 2-layer mine/blown texture-array. Both are
// populated lazily on first dirty event (i.e., first MissionMap::setMine
// call after Reset, including the per-cell init-time setMine loop). Per spec
// at 2026-05-08-pr2c-mine-static-bake-design.md.
//
// Stage 1c is build-and-invalidate scaffolding only — RebuildMineStaticVBOIfDirty
// is callable but not yet wired into per-frame from the bridge. Stage 2c
// adds the bridge + draw + legacy gate-off (single-PR per N2 partial-landing
// rule).
// ---------------------------------------------------------------------------

namespace {

GLuint g_mineStaticVBO_GL         = 0;
int    g_mineVertCount            = 0;
bool   g_mineVBODirty             = true;   // first build triggers via setMine init loop
bool   g_mineVBOFirstBuildPending = true;   // gates lazy texture-array build (R7)
GLuint g_mineTextureArrayGL       = 0;
bool   g_mineTextureArrayReady    = false;

// Vertex format: 3 float pos (world space) + 2 float uv (sprite 0..1) +
// 1 uint layer (0=mine, 1=blown). 24 bytes/vert; 6 verts/cell (2 tris).
struct MineVert {
    float    pos[3];
    float    uv[2];
    uint32_t layer;
};

}  // namespace

namespace gos_terrain_indirect {

void MarkMineDirty() {
    g_mineVBODirty = true;
}

void ResetMineStaticVBO() {
    g_mineVertCount            = 0;
    g_mineVBODirty             = true;
    g_mineVBOFirstBuildPending = true;
    // Keep g_mineStaticVBO_GL allocation across missions; reused by next
    // BuildMineStaticVBO via glBufferData (which orphans + reallocates the
    // backing store).
}

void ResetMineTextureArray() {
    g_mineTextureArrayReady = false;
    // Keep g_mineTextureArrayGL allocation across missions. Source TGAs
    // (defaults/mine_00.tga, defaults/minescorch_00.tga) are process-stable;
    // first BuildMineTextureArray call after reset re-marks ready without
    // re-allocating the GL texture.
}

// [TEMP MINE_GLPROBE] eager-drain probe — env MC2_DECAL_GLPROBE=1 (shared
// gate with the decal probe). Removed once root cause is pinned.
#define MINE_GLPROBE(tag) do { \
    static const bool s_p = (getenv("MC2_DECAL_GLPROBE") && \
                             getenv("MC2_DECAL_GLPROBE")[0] == '1'); \
    if (s_p) { GLenum e; while ((e = glGetError()) != GL_NO_ERROR) \
        printf("[MINE_GLPROBE] at=%s err=0x%x\n", tag, (unsigned)e); \
        fflush(stdout); } } while(0)

void BuildMineTextureArray() {
    // Texture-array contents are process-stable; re-read only if first build
    // or invalidated.
    if (g_mineTextureArrayReady) return;
    MINE_GLPROBE("atlas_entry");

    // R7: handles must be loaded by setupTextures before this fires. If still
    // 0xffffffff, bail — the next dirty event retries. (Stage 2c invokes
    // RebuildMineStaticVBOIfDirty post first-paint via the bridge; setupTextures
    // runs per-quad each frame and lazy-loads the handles at quad.cpp:520-531
    // before any per-frame mine work happens.)
    const DWORD mineSlot  = TerrainQuad::mineTextureHandle;
    const DWORD blownSlot = TerrainQuad::blownTextureHandle;
    if (mineSlot == 0xffffffffu || blownSlot == 0xffffffffu) {
        if (IsTraceEnabled()) {
            printf("[TERRAIN_INDIRECT v1] event=mine_atlas_skip "
                   "reason=handles_not_loaded mine=%u blown=%u\n",
                   (unsigned)mineSlot, (unsigned)blownSlot);
            fflush(stdout);
        }
        return;
    }

    const DWORD mineGosHandle  = tex_resolve(mineSlot);
    const DWORD blownGosHandle = tex_resolve(blownSlot);
    if (mineGosHandle == 0u || blownGosHandle == 0u) return;

    const GLuint mineGLTex  = gos_terrain_bridge_glTextureForGosHandle((unsigned)mineGosHandle);
    const GLuint blownGLTex = gos_terrain_bridge_glTextureForGosHandle((unsigned)blownGosHandle);
    if (mineGLTex == 0 || blownGLTex == 0) return;

    constexpr int kW = 16;
    constexpr int kH = 16;
    std::vector<uint32_t> mineBuf((size_t)kW * kH, 0u);
    std::vector<uint32_t> blownBuf((size_t)kW * kH, 0u);

    // Save GL state (mirrors PR1 BuildCementCatalogAtlas pattern).
    GLint savedActive = GL_TEXTURE0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &savedActive);
    glActiveTexture(GL_TEXTURE0);
    GLint savedTex0 = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTex0);
    GLint savedPackAlign = 4;
    glGetIntegerv(GL_PACK_ALIGNMENT, &savedPackAlign);
    GLint savedUnpackAlign = 4;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &savedUnpackAlign);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    MINE_GLPROBE("before_getteximage");
    MC2_GL_BindTexture(GL_TEXTURE_2D, mineGLTex);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_BGRA, GL_UNSIGNED_BYTE, mineBuf.data());
    MINE_GLPROBE("after_getteximage_mine");
    MC2_GL_BindTexture(GL_TEXTURE_2D, blownGLTex);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_BGRA, GL_UNSIGNED_BYTE, blownBuf.data());
    MINE_GLPROBE("after_getteximage_blown");

    // TEX-CLASS: asset-pool -- minefield 2D_ARRAY (content)
    if (g_mineTextureArrayGL == 0) glGenTextures(1, &g_mineTextureArrayGL);
    MC2_GL_BindTexture(GL_TEXTURE_2D_ARRAY, g_mineTextureArrayGL);
    // Allocate 2-layer storage. glTexImage3D with NULL data orphans / reserves.
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, kW, kH, 2,
                 0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, /*layer=*/0, kW, kH, 1,
                    GL_BGRA, GL_UNSIGNED_BYTE, mineBuf.data());
    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, /*layer=*/1, kW, kH, 1,
                    GL_BGRA, GL_UNSIGNED_BYTE, blownBuf.data());
    // NEAREST filter, no mips — matches gosHint_DisableMipmap | DontShrink at
    // quad.cpp:524, :531. CLAMP_TO_EDGE: sprite UVs are 0..1, no edge-bleed risk.
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    MC2_GL_BindTexture(GL_TEXTURE_2D_ARRAY, 0);
    MINE_GLPROBE("after_teximage3d");

    // Restore state.
    glPixelStorei(GL_PACK_ALIGNMENT, savedPackAlign);
    glPixelStorei(GL_UNPACK_ALIGNMENT, savedUnpackAlign);
    MC2_GL_BindTexture(GL_TEXTURE_2D, (GLuint)savedTex0);
    glActiveTexture((GLenum)savedActive);

    g_mineTextureArrayReady = true;
    MINE_GLPROBE("atlas_exit");

    if (IsTraceEnabled()) {
        printf("[TERRAIN_INDIRECT v1] event=mine_atlas_built "
               "mineSlot=%u blownSlot=%u gltex=%u dim=%dx%d\n",
               (unsigned)mineSlot, (unsigned)blownSlot,
               (unsigned)g_mineTextureArrayGL, kW, kH);
        fflush(stdout);
    }
}

void BuildMineStaticVBO() {
    MINE_GLPROBE("vbo_entry");
    if (!GameMap || !land) {
        g_mineVertCount = 0;
        return;
    }

    // Empirically tiny in stock content — start with 64 cell capacity.
    std::vector<MineVert> verts;
    verts.reserve(64 * 6);

    const int W = (int)GameMap->width;
    const int H = (int)GameMap->height;
    const int TW = W / MAPCELL_DIM;
    const int TH = H / MAPCELL_DIM;
    const float halfCell = Terrain::worldUnitsPerCell * 0.5f;

    for (int tR = 0; tR < TH; ++tR) {
        for (int tC = 0; tC < TW; ++tC) {
            // tileHasMines short-circuits ~97% of tiles per recon-5.
            if (!GameMap->tileHasMines(tR, tC)) continue;
            for (int cR = 0; cR < MAPCELL_DIM; ++cR) {
                for (int cC = 0; cC < MAPCELL_DIM; ++cC) {
                    const int row = tR * MAPCELL_DIM + cR;
                    const int col = tC * MAPCELL_DIM + cC;
                    if (!GameMap->inBounds(row, col)) continue;
                    const unsigned long mine = GameMap->getMine(row, col);
                    if (mine == 0) continue;
                    const uint32_t layer = (mine == 2) ? 1u : 0u;

                    Stuff::Vector3D center;
                    land->tileCellToWorld(tR, tC, cR, cC, center);

                    // Four corners around cell center. tileCellToWorld returns
                    // cell center XY (with half-cell offset baked in per
                    // terrain.h:416-417); we expand to the 4 cell corners.
                    Stuff::Vector3D c00 = center, c10 = center;
                    Stuff::Vector3D c11 = center, c01 = center;
                    c00.x -= halfCell; c00.y += halfCell;  // top-left
                    c10.x += halfCell; c10.y += halfCell;  // top-right
                    c11.x += halfCell; c11.y -= halfCell;  // bottom-right
                    c01.x -= halfCell; c01.y -= halfCell;  // bottom-left
                    c00.z = land->getTerrainElevation(c00);
                    c10.z = land->getTerrainElevation(c10);
                    c11.z = land->getTerrainElevation(c11);
                    c01.z = land->getTerrainElevation(c01);

                    auto pushVert = [&](const Stuff::Vector3D& p, float u, float v) {
                        MineVert mv;
                        mv.pos[0] = p.x; mv.pos[1] = p.y; mv.pos[2] = p.z;
                        mv.uv[0]  = u;   mv.uv[1]  = v;
                        mv.layer  = layer;
                        verts.push_back(mv);
                    };
                    // Tri 1: c00 → c10 → c11
                    pushVert(c00, 0.0f, 0.0f);
                    pushVert(c10, 1.0f, 0.0f);
                    pushVert(c11, 1.0f, 1.0f);
                    // Tri 2: c00 → c11 → c01
                    pushVert(c00, 0.0f, 0.0f);
                    pushVert(c11, 1.0f, 1.0f);
                    pushVert(c01, 0.0f, 1.0f);
                }
            }
        }
    }

    g_mineVertCount = (int)verts.size();

    if (g_mineStaticVBO_GL == 0) glGenBuffers(1, &g_mineStaticVBO_GL);
    glBindBuffer(GL_ARRAY_BUFFER, g_mineStaticVBO_GL);
    // glBufferData with size>0 always uploads even if vector is empty
    // (verts.data() returns a non-null sentinel on empty MSVC vectors, but
    // we pass nullptr-on-empty defensively to match Khronos expectations).
    MC2_GL_BufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)(verts.size() * sizeof(MineVert)),
                 verts.empty() ? nullptr : verts.data(),
                 GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    MINE_GLPROBE("vbo_after_bufferdata");

    if (IsTraceEnabled()) {
        printf("[TERRAIN_INDIRECT v1] event=mine_vbo_built cells=%d verts=%d "
               "vbo_gl=%u\n",
               g_mineVertCount / 6, g_mineVertCount,
               (unsigned)g_mineStaticVBO_GL);
        fflush(stdout);
    }
}

void RebuildMineStaticVBOIfDirty() {
    if (!g_mineVBODirty) return;
    if (g_mineVBOFirstBuildPending) {
        BuildMineTextureArray();
        // Mark first-build done regardless of texture-array ready state —
        // BuildMineTextureArray handles its own retry via g_mineTextureArrayReady.
        g_mineVBOFirstBuildPending = false;
    }
    BuildMineStaticVBO();
    g_mineVBODirty = false;
}

unsigned int GetMineStaticVBO_GL()      { return g_mineStaticVBO_GL; }
int          GetMineVertCount()         { return g_mineVertCount; }
unsigned int GetMineTextureArrayGL()    { return g_mineTextureArrayGL; }
bool         IsMineTextureArrayReady()  { return g_mineTextureArrayReady; }

// PR2c Stage 2c — armed iff MC2_TERRAIN_INDIRECT_MINE=1 (env gate).
//
// We do NOT gate on g_mineTextureArrayReady here — that creates a bootstrap
// circular dependency: the texture-array can only be built inside
// BuildMineTextureArray, which is only called from RebuildMineStaticVBOIfDirty,
// which is only called from DrawMineStatic, which is only called from the
// Render.TerrainMines zone IF IsFrameMineArmed() returns true. Gating on
// readiness here means the first build never happens.
//
// Instead: arming = env on. DrawMineStatic handles the texture-not-ready
// case internally (returns true no-op until the next dirty event lazy-builds).
// First paint cycle after mission load: gate-offs fire, legacy paths skipped,
// DrawMineStatic invokes Rebuild* which lazy-builds the texture-array on
// first call (handles loaded by setupTextures on the SAME frame's earlier
// per-quad loop — R7 timing trap mitigated by paint-order: setupTextures
// runs before Render.TerrainMines).
bool IsFrameMineArmed() {
    return IsMineEnabled();
}

// PR2c Stage 2c — invoked from txmmgr.cpp's Render.TerrainMines zone. Lazy-builds
// VBO + texture-array on first dirty event; issues one glDrawArrays via the
// bridge if there's anything to draw.
bool DrawMineStatic() {
    RebuildMineStaticVBOIfDirty();
    if (g_mineVertCount <= 0) {
        // Successful zero-emit frame (mission has no mines yet). Legacy paths
        // are still gated off — that's the whole point.
        return true;
    }
    if (!g_mineTextureArrayReady) {
        // Build retry pending; the texture-array build couldn't complete (e.g.,
        // handles still 0xffffffff at first call). Treat as no-op until next
        // dirty event re-attempts.
        return true;
    }
    const bool ok = gos_terrain_bridge_drawMineStatic(
        g_mineVertCount,
        g_mineStaticVBO_GL,
        g_mineTextureArrayGL);
    if (ok) {
        // 6 verts per cell (2 tris * 3 verts).
        Counters_AddIndirectMineDrawnCells((long long)(g_mineVertCount / 6));
    }
    return ok;
}

}  // namespace gos_terrain_indirect

// C-linkage forwarder for mclib/move.h's inline setMine — avoids pulling
// the full gos_terrain_indirect.h into the widely-included move.h. The
// symbol is forward-declared at the call site in move.h:
//     extern void gos_terrain_indirect_MarkMineDirty();
// and resolved at link time.
void gos_terrain_indirect_MarkMineDirty() {
    gos_terrain_indirect::MarkMineDirty();
}

// ---------------------------------------------------------------------------
// Slice A — cement-overlay (decal) static-bake infrastructure.
//
// STRUCTURAL MIRROR of the PR2c mine static-bake (BuildMineStaticVBO /
// RebuildMineStaticVBOIfDirty / MarkMineDirty / ResetMineStaticVBO /
// IsFrameMineArmed / DrawMineStatic, ~line 3119-3398 above). Same lifetime
// discipline: one GL_STATIC_DRAW buffer kept across missions, CPU state reset
// on mission load + destroy, lazy rebuild-if-dirty on first armed draw (R7
// timing-trap mitigation: do NOT eager-build at primeMissionTerrainCache —
// the overlay tex handles lazy-load in TerrainQuad::setupTextures during the
// first paint cycle, before Render.TerrainOverlaysStatic fires).
//
// What it bakes: the per-quad M2d cement-overlay producer (mclib/quad.cpp
// `if (useOverlayTexture && overlayHandle != 0xffffffff)`). Decal inputs are
// map-stable — corner world XYZ, overlayHandle, uvMode are all derivable from
// the map-immutable Shape-C terrain face cache (MapData::buildTerrainFaceCache)
// + MapData::blocks. The bake reproduces M2d's 4-corner WorldOverlayVert build
// + per-uvMode tri emit UNCONDITIONALLY (no pzTri camera cull — plan's pz-cull
// policy; cement is sparse, GPU clips, mirrors DrawMineStatic's unconditional
// static draw). Vertex argb/fog are forced to 0xffffffff / 1.0: shaders/
// terrain_overlay.frag never reads the Color/FogValue varyings (explicit
// comment at terrain_overlay.frag:48 — it recomputes lighting/fog from WorldPos
// + live uniforms), so a geometry-only bake is lighting-identical to M2d, the
// same way MineVert is pos/uv/layer-only.
// ---------------------------------------------------------------------------

namespace {

// WorldOverlayVert layout MUST byte-match GameOS/include/gameos.hpp's struct
// (28-byte stride) — the bridge VAO (drawDecalStaticBatch / makeOverlayVAO)
// reads attribs 0..3 at offsets 0/12/20/24.
struct DecalVert {
    float        wx, wy, wz;   // offset  0  — MC2 world (x=east, y=north, z=elev)
    float        u, v;         // offset 12
    float        fog;          // offset 20  — forced 1.0 (frag discards it)
    uint32_t     argb;         // offset 24  — forced 0xffffffff (frag discards it)
};

// Per-overlayTexId draw range (mirror gameos_graphics.cpp OverlayBatchEntry_
// and gos_terrain_indirect's mine 6-verts-per-cell grouping idea).
struct DecalDrawRange {
    uint32_t texHandle;
    uint32_t firstVert;
    uint32_t vertCount;
};

GLuint                       g_decalStaticVBO_GL  = 0;
int                          g_decalVertCount     = 0;
bool                         g_decalVBODirty      = true;  // first build triggers via Rebuild on first armed draw
std::vector<DecalDrawRange>  g_decalDrawRanges;

// Mirrors quad.cpp's OVERLAY_ELEV_OFFSET (0.15f) + the fixed overlay UV
// constants (oldminU/oldmaxU/oldminV/oldmaxV at quad.cpp:2054-2057). The M2d
// overlay path uses these CONSTANT UVs (not uvData), so they are fully
// map-stable.
constexpr float kOverlayElevOffset = 0.15f;
constexpr float kOldMinU = 0.0078125f;
constexpr float kOldMaxU = 0.9921875f;
constexpr float kOldMinV = 0.0078125f;
constexpr float kOldMaxV = 0.9921875f;

}  // namespace

namespace gos_terrain_indirect {

// Public forwarder: calls the anonymous-namespace BuildCementCatalogAtlas() from
// editor code (e.g. EditorData::refreshTerrainAfterEdit) that needs to rebuild the
// cement atlas after overlay painting creates new texture slots not present at
// mission-load atlas-build time. Editor uses only; game path goes through BuildDenseRecipe.
void RebuildCementAtlas() {
    BuildCementCatalogAtlas();
}

// Deferred cement-atlas rebuild (mirror MarkDecalDirty/RebuildDecalStaticVBOIfDirty
// debounce discipline). RebuildCementAtlas() does a glGetTexImage readback, which
// breaks in-flight GL draw state if called MID-FRAME (the editor used to call it
// straight from refreshTerrainAfterEdit and that reset normal/detail tiling
// map-wide). So instead a cement-affecting brush stroke MARKS the atlas dirty here,
// and RebuildCementAtlasIfDirty() fires the actual rebuild at FRAME-START — before
// any terrain/GL draws — where the readback is safe. BuildCementCatalogAtlas()
// itself save/restores GL_ACTIVE_TEXTURE + GL_TEXTURE_BINDING_2D + pack/unpack
// alignment (gos_terrain_indirect.cpp:1231-1273) and only touches GL_TEXTURE0, so
// the rebuild does not intrinsically disturb normal/detail bindings.
static bool g_cementAtlasDirty = false;

void MarkCementAtlasDirty() {
    g_cementAtlasDirty = true;
}

void RebuildCementAtlasIfDirty() {
    if (!g_cementAtlasDirty) return;
    g_cementAtlasDirty = false;
    RebuildCementAtlas();
}

// Mirror MarkMineDirty — idempotent; multiple cement mutations between paints
// debounce to one rebuild via the dirty flag.
void MarkDecalDirty() {
    g_decalVBODirty = true;
}

// Mirror ResetMineStaticVBO — CPU-clear only; keep g_decalStaticVBO_GL
// allocation across missions (next BuildDecalStaticVBO reuses it via
// glBufferData orphan+realloc).
void ResetDecalStaticVBO() {
    g_decalVertCount = 0;
    g_decalVBODirty  = true;
    g_decalDrawRanges.clear();
    if (IsTraceEnabled()) {
        printf("[TERRAIN_OVERLAY v1] event=decal_vbo_reset "
               "vbo_gl=%u kept=1\n", (unsigned)g_decalStaticVBO_GL);
        fflush(stdout);
    }
}

// Mirror BuildMineStaticVBO. Iterates the map-immutable Shape-C terrain face
// cache exactly as MapData::buildTerrainFaceCache does (mapdata.cpp:262-309):
// same tile range, same 4-corner block layout, same worldQuadUVMode formula.
void BuildDecalStaticVBO() {
    g_decalVertCount = 0;
    g_decalDrawRanges.clear();

    if (!Terrain::mapData) return;
    PostcompVertexPtr blocks = Terrain::mapData->getBlocks();
    if (!blocks) return;

    const long mapSide  = Terrain::realVerticesMapSide;
    const long half     = Terrain::halfVerticesMapSide;
    const float wupv    = Terrain::worldUnitsPerVertex;
    const long cacheW   = mapSide - 1;   // == worldQuadCacheWidth() (mapdata.cpp:107)
    if (cacheW <= 0) return;

    std::vector<DecalVert> verts;
    verts.reserve(64 * 6);

    // First collect (texId -> tri verts) so draws can be grouped per texId
    // (mirror OverlayBatch_'s verts + draws split / pushToOverlayBatch_).
    // overlay tile count is empirically tiny — a flat scan + run-grouping is
    // fine (cement sparse, matches DrawMineStatic's tileHasMines short-circuit
    // in spirit).
    struct PendingTri { uint32_t texId; DecalVert v[3]; };
    std::vector<PendingTri> pending;

    for (long tileR = 0; tileR < cacheW; ++tileR) {
        for (long tileC = 0; tileC < cacheW; ++tileC) {
            const MapData::WorldQuadTerrainCacheEntry* e =
                Terrain::mapData->getTerrainFaceCacheEntry(tileR, tileC);
            if (!e || !e->isValid()) continue;
            // M2d gate equivalent: useOverlayTexture && overlayHandle !=
            // 0xffffffff. buildTerrainFaceCache only sets overlayHandle for
            // the alpha-cement case (mapdata.cpp:296-302); pure-cement and
            // non-cement leave it 0xffffffff. Use the handle directly so this
            // matches the live M2d predicate byte-for-byte.
            const DWORD overlayHandle = e->overlayHandle;
            if (overlayHandle == 0xffffffffu) continue;

            // CEMENT-HARD-EDGE-1 (Slice 2, default-on; re-apply of reverted 322ef8b4):
            // Cement transition edges are now drawn analytically in the chunk frag
            // (terrain_lod_chunk.frag, neighbor-derived hard mask). Skip the legacy
            // gray cement-transition decal so it does not double-draw / seam over the
            // new frag edge.
            //
            // CEMENT-ROAD-SPLIT-1: the earlier "isCement()&&isAlpha() is concrete-only"
            // assumption was WRONG. isCement() is a MISNOMER for "uses the cement/
            // transition texture machinery": terrtxm.cpp:1493 sets CEMENT_FLAG on EVERY
            // createTransition — roads/runways/bridges included (see the explicit comment
            // terrtxm.cpp:1501-1504 "If we are a road ... ALPHA is TRUE"). So &Overlays
            // road/runway/bridge tiles ALSO satisfy isCement()&&isAlpha() and were being
            // wrongly skipped, making dirt/regular roads vanish. The frag cement-word
            // edge system only ever covers cement TERRAIN-TYPE tiles (10, 13-20), never
            // the &Overlays road system, so skip the legacy decal ONLY for a true
            // concrete transition: isCement()&&isAlpha() AND no road overlay on the tile.
            Overlays decalOverlayType = INVALID_OVERLAY;
            DWORD    decalOverlayOffset = 0;
            Terrain::mapData->getOverlay(tileR, tileC, decalOverlayType, decalOverlayOffset);
            if (e->isCement() && e->isAlpha() && decalOverlayType == INVALID_OVERLAY) continue;

            const DWORD overlayTexId = tex_resolve(overlayHandle);
            if (overlayTexId == 0) continue;

            // OVERLAY-TILE-HIRES-1 (gated, default OFF => kOld* constants used
            // verbatim). Mirror quad.cpp's per-overlay half-texel inset: a
            // resynced hi-res overlay tile sampled with the 64px inset (0.5/64)
            // crops its outer texel ring and breaks road continuity at seams.
            // physical edge = logical * uvScale (disk 4x-upscale TGA convention).
            float tileMinU = kOldMinU, tileMaxU = kOldMaxU;
            float tileMinV = kOldMinV, tileMaxV = kOldMaxV;
            if (MC2_OverlayTileHiresSize() > 0)
            {
                DWORD ovLogicalW = 0, ovLogicalH = 0;
                if (mcTextureManager->tryGetTextureLogicalSize(overlayHandle, ovLogicalW, ovLogicalH))
                {
                    const DWORD ovEdge = ovLogicalW * mcTextureManager->getUVScale(overlayHandle);
                    if (ovEdge > 0)
                    {
                        tileMinU = tileMinV = 0.5f / (float)ovEdge;
                        tileMaxU = tileMaxV = 1.0f - 0.5f / (float)ovEdge;
                    }
                }
            }

            // 4 corners — mirror buildTerrainFaceCache (mapdata.cpp:266-275)
            // and fillWorldCacheVertex (mapdata.cpp:125-132). Quad
            // vertices[0..3] == cache worldVertices[0..3] (verified vs
            // MapData::makeLists v0=v(x,y) v1=v(x+1,y) v2=v(x+1,y+1)
            // v3=v(x,y+1)).
            const long idx0 = tileC + tileR * mapSide;
            PostcompVertexPtr p0 = &blocks[idx0];
            PostcompVertexPtr p1 = p0 + 1;
            PostcompVertexPtr p2 = p0 + mapSide + 1;
            PostcompVertexPtr p3 = p0 + mapSide;

            auto cornerXY = [&](long tR, long tC, float& wx, float& wy) {
                wx = float(tC - half) * wupv;     // fillWorldCacheVertex vx
                wy = float(half - tR) * wupv;     // fillWorldCacheVertex vy
            };

            DecalVert c[4];
            cornerXY(tileR,     tileC,     c[0].wx, c[0].wy);
            cornerXY(tileR,     tileC + 1, c[1].wx, c[1].wy);
            cornerXY(tileR + 1, tileC + 1, c[2].wx, c[2].wy);
            cornerXY(tileR + 1, tileC,     c[3].wx, c[3].wy);
            c[0].wz = p0->elevation + kOverlayElevOffset;  // M2d quad.cpp:2406
            c[1].wz = p1->elevation + kOverlayElevOffset;
            c[2].wz = p2->elevation + kOverlayElevOffset;
            c[3].wz = p3->elevation + kOverlayElevOffset;

            // TERRAIN-OVERLAY-V2-DECAL-SUPPRESS-1: when the authored OVERLAY_V2
            // sidecar covers this tile it already draws the runway/road/cement
            // here (composited in the LOD-chunk frag). Baking the legacy decal
            // too draws it TWICE (visible double-draw at e.g. the mc2_01 runway).
            // Skip the legacy decal for tiles FULLY inside the sidecar's world
            // bounds so cement/runway draws once. Byte-identical when OVERLAY_V2
            // is off (sidecar not loaded -> guard false, no tile skipped) and
            // for tiles OUTSIDE the bounds (fall through -> baked normally, so
            // no gaps where the sidecar doesn't reach). Bounds-rect v0 (per the
            // decal recon): assumes the sidecar authors the overlay content
            // within its bounds; alpha-accurate per-tile suppression is a
            // follow-up. Env MC2_TERRAIN_OVERLAY_V2_DECAL_SUPPRESS=0 reverts.
            {
                static const bool s_decalSuppress = []() {
                    const char* v = getenv("MC2_TERRAIN_OVERLAY_V2_DECAL_SUPPRESS");
                    return !(v && v[0] == '0');   // default ON
                }();
                if (s_decalSuppress && gos_TerrainLodChunk_OverlaySidecarLoaded()) {
                    const float* ob = gos_TerrainLodChunk_OverlayBounds();
                    const float bMinX = ob[0];
                    const float bMaxY = ob[1];
                    const float bMaxX = ob[0] + ob[2];
                    const float bMinY = ob[1] - ob[3];
                    bool allInside = true;
                    for (int k = 0; k < 4; ++k) {
                        if (c[k].wx < bMinX || c[k].wx > bMaxX ||
                            c[k].wy < bMinY || c[k].wy > bMaxY) { allInside = false; break; }
                    }
                    if (allInside) continue;   // sidecar owns this tile's overlay
                }
            }

            // [TERRAIN_OVERLAY v1] decal_corner_probe — env-gated
            // (MC2_TERRAIN_INDIRECT_TRACE), first few cement tiles only,
            // SILENT by default. Retained dormant diagnostic (demote-not-
            // delete per the debug-instrumentation rule): prints the bake's
            // per-corner world coords + the Terrain globals it derived them
            // from + an ELEV_IDENTICAL flag. The raster-sheet bug it was
            // built for was root-caused (unconditional all-map draw vs the
            // non-clip-safe terrain_overlay.vert) and FIXED 2026-05-17 (the
            // px.z in [0,1) guard) — the bake coords were proven correct, so
            // this stays as a coord-sanity check for any future bake-source
            // change, not an active investigation. Background:
            // memory/drawpass_retirement_decal_bake_state_and_raster_sheet_trap.md
            if (IsTraceEnabled()) {
                static int s_probeCount = 0;
                if (s_probeCount < 6) {
                    ++s_probeCount;
                    const bool elevIdentical =
                        (p0->elevation == p1->elevation) &&
                        (p1->elevation == p2->elevation) &&
                        (p2->elevation == p3->elevation);
                    printf("[TERRAIN_OVERLAY v1] event=decal_corner_probe "
                           "tileR=%ld tileC=%ld mapSide=%ld half=%ld "
                           "wupv=%.6f elev_identical=%d "
                           "c0=(%.3f,%.3f,%.5f) c1=(%.3f,%.3f,%.5f) "
                           "c2=(%.3f,%.3f,%.5f) c3=(%.3f,%.3f,%.5f) "
                           "elev_raw=(%.5f,%.5f,%.5f,%.5f)\n",
                           tileR, tileC, (long)mapSide, (long)half,
                           (double)wupv, elevIdentical ? 1 : 0,
                           c[0].wx, c[0].wy, c[0].wz,
                           c[1].wx, c[1].wy, c[1].wz,
                           c[2].wx, c[2].wy, c[2].wz,
                           c[3].wx, c[3].wy, c[3].wz,
                           (double)p0->elevation, (double)p1->elevation,
                           (double)p2->elevation, (double)p3->elevation);
                    fflush(stdout);
                }
            }
            // ROAD-PBR-ASPHALT-1: repurpose the previously-discarded argb field to
            // carry a per-tile material id to terrain_overlay.frag. The bridge VAO
            // reads attrib 3 as GL_UNSIGNED_BYTE/NORMALIZED in memory order
            // (byte0=B, byte1=G, byte2=R, byte3=A); the vert shader swizzles
            // colorIn.bgra so Color.a == (argb>>24)&0xff normalized. We encode the
            // material id in the ALPHA byte: asphalt (PAVED_ROAD/RUNWAY) -> alpha
            // byte 1 (Color.a == 1/255 ~= 0.0039), all other tiles -> alpha byte
            // 0xff (Color.a == 1.0). The frag thresholds Color.a < 0.5 to select
            // the asphalt material; every non-asphalt tile is byte-identical to the
            // pre-slice behaviour (frag ignores the rest of Color).
            // ROAD-MATERIAL-GRAVEL-1: 3-way material id in the alpha byte. asphalt
            // (PAVED_ROAD/RUNWAY) -> 0x01 (Color.a ~= 0.0039), gravel (DIRT_ROAD) ->
            // 0x02 (Color.a ~= 0.0078), all other tiles -> 0xff (Color.a == 1.0).
            // The vert decodes matByte<250 as the id (else 0 = no material), so the
            // 0xff tiles stay byte-identical to pre-slice behaviour.
            // ROAD-PBR-FAILSOFT-1: only bake a PBR material id when its albedo
            // actually loaded (gos_TerrainRoadMaterialReady). Installs without
            // the hand-placed asphalt/gravel TGAs previously got matId anyway ->
            // the frag sampled texture 0 -> BLACK roads (looked like the roads
            // were gone). matId 0xff = legacy tgl/64 road tile path, untouched.
            uint32_t tileArgb;
            if ((decalOverlayType == PAVED_ROAD || decalOverlayType == RUNWAY)
                && ::gos_TerrainRoadMaterialReady(1))
                tileArgb = 0x01000000u;       // asphalt (v_matId==1)
            else if (decalOverlayType == DIRT_ROAD && ::gos_TerrainRoadMaterialReady(2))
                tileArgb = 0x02000000u;       // gravel  (v_matId==2)
            else
                tileArgb = 0xffffffffu;       // no overlay material (legacy tile)
            for (int k = 0; k < 4; ++k) {
                c[k].fog  = 1.0f;          // frag discards FogValue (terrain_overlay.frag:48)
                c[k].argb = tileArgb;      // ROAD material id (alpha byte)
            }

            // uvMode: mirror worldQuadUVMode (mapdata.cpp:115-118). The cache's
            // uvData was resolved with this; M2d's diagonal/UV choice keys off
            // the live quad uvMode, which equals worldQuadUVMode(absRow,absCol)
            // (MapData::makeLists parity == absolute-tile parity).
            const long uvMode =
                ((tileR & 1) == (tileC & 1)) ? BOTTOMRIGHT : BOTTOMLEFT;

            auto emit = [&](const DecalVert& a, float au, float av,
                            const DecalVert& b, float bu, float bv,
                            const DecalVert& d, float du, float dv) {
                PendingTri t;
                t.texId = (uint32_t)overlayTexId;
                t.v[0] = a; t.v[0].u = au; t.v[0].v = av;
                t.v[1] = b; t.v[1].u = bu; t.v[1].v = bv;
                t.v[2] = d; t.v[2].u = du; t.v[2].v = dv;
                pending.push_back(t);
            };

            // EXACT reproduction of M2d's per-uvMode tri emit
            // (quad.cpp:2412-2443), but UNCONDITIONAL (no pzTri1/pzTri2).
            // tileMin/Max == kOld* constants unless OVERLAY-TILE-HIRES is on.
            if (uvMode == BOTTOMLEFT) {
                // tri1: corners 0,1,3
                emit(c[0], tileMinU, tileMinV,
                     c[1], tileMaxU, tileMinV,
                     c[3], tileMinU, tileMaxV);
                // tri2: corners 1,2,3
                emit(c[1], tileMaxU, tileMinV,
                     c[2], tileMaxU, tileMaxV,
                     c[3], tileMinU, tileMaxV);
            } else {
                // BOTTOMRIGHT — tri1: corners 0,1,2
                emit(c[0], tileMinU, tileMinV,
                     c[1], tileMaxU, tileMinV,
                     c[2], tileMaxU, tileMaxV);
                // tri2: corners 0,2,3
                emit(c[0], tileMinU, tileMinV,
                     c[2], tileMaxU, tileMaxV,
                     c[3], tileMinU, tileMaxV);
            }
        }
    }

    // Group pending tris into contiguous per-texId draw ranges (mirror
    // pushToOverlayBatch_'s run-coalescing: consecutive same-texHandle tris
    // extend the last range).
    for (const PendingTri& t : pending) {
        if (!g_decalDrawRanges.empty() &&
            g_decalDrawRanges.back().texHandle == t.texId) {
            g_decalDrawRanges.back().vertCount += 3;
        } else {
            DecalDrawRange r;
            r.texHandle = t.texId;
            r.firstVert = (uint32_t)verts.size();
            r.vertCount = 3;
            g_decalDrawRanges.push_back(r);
        }
        verts.push_back(t.v[0]);
        verts.push_back(t.v[1]);
        verts.push_back(t.v[2]);
    }

    g_decalVertCount = (int)verts.size();

    if (g_decalStaticVBO_GL == 0) glGenBuffers(1, &g_decalStaticVBO_GL);
    glBindBuffer(GL_ARRAY_BUFFER, g_decalStaticVBO_GL);
    MC2_GL_BufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)(verts.size() * sizeof(DecalVert)),
                 verts.empty() ? nullptr : verts.data(),
                 GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    if (IsTraceEnabled()) {
        printf("[TERRAIN_OVERLAY v1] event=decal_vbo_built tris=%d verts=%d "
               "ranges=%d vbo_gl=%u\n",
               g_decalVertCount / 3, g_decalVertCount,
               (int)g_decalDrawRanges.size(),
               (unsigned)g_decalStaticVBO_GL);
        fflush(stdout);
    }
}

// Mirror RebuildMineStaticVBOIfDirty (no texture-array first-build step —
// cement overlay uses the existing per-mission terrain texture nodes via
// tex_resolve, not a dedicated 2-layer array).
void RebuildDecalStaticVBOIfDirty() {
    if (!g_decalVBODirty) return;
    BuildDecalStaticVBO();
    g_decalVBODirty = false;
}

unsigned int GetDecalStaticVBO_GL() { return g_decalStaticVBO_GL; }
int          GetDecalVertCount()    { return g_decalVertCount; }

// Mirror DrawMineStatic. Lazy rebuild-if-dirty on first armed draw, single
// bridge dispatch, NO clear (static buffer persists). Returns true on a
// successful zero-emit frame (mission has no cement overlay) — the M2d gate-
// off is still the point.
bool DrawDecalStatic() {
    RebuildDecalStaticVBOIfDirty();
    if (g_decalVertCount <= 0 || g_decalDrawRanges.empty()) {
        return true;  // no cement overlay this mission — successful no-op
    }
    static_assert(sizeof(DecalDrawRange) == sizeof(GosDecalStaticDraw),
                  "DecalDrawRange must layout-match GosDecalStaticDraw");
    const bool ok = gos_terrain_bridge_drawDecalStatic(
        g_decalStaticVBO_GL,
        reinterpret_cast<const GosDecalStaticDraw*>(g_decalDrawRanges.data()),
        (int)g_decalDrawRanges.size());
    if (ok) {
        Counters_AddDecalStaticTrisDrawn((long long)(g_decalVertCount / 3));
        if (IsTraceEnabled()) {
            static bool s_firstDraw = true;
            if (s_firstDraw) {
                printf("[TERRAIN_OVERLAY v1] event=decal_first_draw tris=%d\n",
                       g_decalVertCount / 3);
                fflush(stdout);
                s_firstDraw = false;
            }
        }
    } else if (IsTraceEnabled()) {
        printf("[TERRAIN_OVERLAY v1] event=decal_draw_fallback "
               "reason=bridge_returned_false verts=%d\n", g_decalVertCount);
        fflush(stdout);
    }
    return ok;
}

}  // namespace gos_terrain_indirect

// C-linkage forwarder for the cement-mutation invalidation sites (code/
// bldng.cpp bridge-destroy + mclib/terrain.cpp Terrain::setOverlay). Mirrors
// gos_terrain_indirect_MarkMineDirty: forward-declared `extern void
// gos_terrain_indirect_MarkDecalDirty();` at the call sites so widely-included
// headers do not need gos_terrain_indirect.h. Idempotent — chain mutations
// debounce to one rebuild/paint via the dirty flag.
void gos_terrain_indirect_MarkDecalDirty() {
    gos_terrain_indirect::MarkDecalDirty();
}
