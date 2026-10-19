#pragma once
// FRAME-GRAPH-EXECUTOR-ISLAND-1 — pure/constexpr IslandContract descriptor.
// FRAMEGRAPH-APPLY-STATE-ISLAND-1 — adds SubStageStateDesc vocabulary table.
//
// Declares WHAT the executor validates for each owned island; contains no GL
// includes and no game-side runtime state. The GL-touching wrapper lives in
// gos_postprocess.cpp (executorOwnBegin / executorOwnEnd) where it can access
// gosPostProcess members. This header is offline-testable (tests/unit/test_frame_graph.cpp).
//
// Design note: deliberately NOT in gos_FrameCtx (read-only mirror, no executor
// state may be injected). Counters are standalone process-statics exposed via
// extern "C" mc2_framegraph_executor_owned_passes() / _validation_failures().
//
// Gate: MC2_FRAMEGRAPH_EXECUTOR (default-OFF). When unset -> executorOwnBegin /
// executorOwnEnd are early-return no-ops -> byte-identical to the raw pp->endScene() call.
//
// FRAME-GRAPH-EXECUTOR-ISLAND-2: re-keyed IslandContract to ExecutorIslandId (owns
// sub-stage islands EdgeFog + FogOob in addition to PostProcess).
// FRAME-GRAPH-EXECUTOR-ISLAND-3: adds Shoreline + CloudShadow sub-stage islands.
// EXECUTOR-ISLAND-SCREENSHADOW-1: adds ScreenShadow (6th validate-only island).
//   ScreenShadow now owned: SCREENSHADOW-TEX-RESTORE-1 (a0b4189b) fixed the unit-3
//   GL_TEXTURE_2D_ARRAY leak; glActiveTexture(GL_TEXTURE0) was always restored.

#include "RenderCore/PipelineRegistry.h"         // PipelineId
#include "RenderCore/RenderResourceRegistry.h"    // RenderResourceId
#include "RenderCore/ambient_contract.h"           // ViewportKind

namespace RenderCore { namespace framegraph {

// Dedicated island id — independent of RenderPassId so sub-stage islands
// (which live inside a single RenderPassId::PostProcess frame slot) can each
// have their own contract row.
enum class ExecutorIslandId : uint8_t {
    PostProcess = 0,  // outer composite blit (ISLAND-1)
    EdgeFog,          // sub-stage: map-boundary edge fog (ISLAND-2)
    FogOob,           // sub-stage: out-of-bounds ground fog (ISLAND-2)
    Shoreline,        // sub-stage: shoreline foam brightening (ISLAND-3)
    CloudShadow,      // sub-stage: procedural cloud shadow darkening (ISLAND-3)
    Composite,        // POSTPROCESS-SUBGRAPH-1: outer fullscreen blit sceneFBO_->FBO0 (owned, always runs)
    ShadowDebugOverlay, // POSTPROCESS-SUBGRAPH-1: debug shadow-map overlay on FBO0 (not owned, default-OFF)
    // POSTPROCESS-SUBGRAPH-2: remaining 8 sub-stage ids (append-only, modeling-only, no GL touched)
    ClusterDepthPyramid, // sub-stage: per-tile (min,max) depth image compute dispatch; NOT executor-owned; gated MC2_CLUSTER_DEPTH_PYRAMID default-OFF; isCompute=true
    LightgridBuild,   // sub-stage: per-tile light-bin grid compute dispatch; NOT executor-owned; gated MC2_LIGHTGRID_BUILD default-OFF; isCompute=true
    PostprocessComputeBlur, // sub-stage: GPU compute downsample+Gaussian blur substrate; NOT executor-owned; gated MC2_POSTPROCESS_COMPUTE_BLUR default-OFF; isCompute=true
    ScreenShadow,     // sub-stage: screen-space shadow (draw, sceneFBO_); EXECUTOR-ISLAND-SCREENSHADOW-1 (validate-only); tex-unit leak fixed by a0b4189b; gated screenShadowEnabled_+shadowsEnabled_ default-ON in-mission
    Count,
};

// IslandContract — what the executor validates for a single owned island.
// All fields are plain bool; no GL, no runtime state.
struct IslandContract {
    ExecutorIslandId id;
    bool requiresProgramValid;      // assert program is compiled+valid before call
    bool requiresSceneColorTex;     // assert sceneColorTex_ != 0 before call
    bool requiresSceneDepthTex;     // assert sceneDepthTex_ != 0 before call (EdgeFog/FogOob)
    bool warnIfNoTerrainLatch;      // warn (non-fatal) if sceneHasTerrain_==false
    bool postRequiresDefaultFbo;    // assert GL_DRAW_FRAMEBUFFER_BINDING==0 after call
    bool postRequiresBlendDisabled; // assert GL_BLEND disabled after call
    bool postRequiresActiveTexture0;// assert active texture unit == GL_TEXTURE0 after call
};

// Compile-time table of owned islands.
static constexpr IslandContract kExecutorIslands[] = {
    {
        /*id*/                      ExecutorIslandId::PostProcess,
        /*requiresProgramValid*/    false, // native pipelines are created on demand
        /*requiresSceneColorTex*/   true,
        /*requiresSceneDepthTex*/   false,
        /*warnIfNoTerrainLatch*/    true,   // sub-stages bail gracefully; WARN not FATAL
        /*postRequiresDefaultFbo*/  true,   // composite blit explicitly binds FBO 0 at end
        /*postRequiresBlendDisabled*/false, // PostProcess end-state not asserted (composite varies)
        /*postRequiresActiveTexture0*/false,
    },
};
static constexpr unsigned kExecutorIslandCount =
    sizeof(kExecutorIslands) / sizeof(kExecutorIslands[0]);

// Find the IslandContract for the given island id, or nullptr if not owned.
// Pure, constexpr-compatible, no GL.
inline const IslandContract* findIslandContract(ExecutorIslandId id) {
    for (unsigned i = 0; i < kExecutorIslandCount; ++i) {
        if (kExecutorIslands[i].id == id)
            return &kExecutorIslands[i];
    }
    return nullptr;
}

// PER-PASS-APPLY-COUNTERS-1 — independent observability id per apply-state path.
// Replaces the coarse aggregate g_applyStatePasses (which made decal "rise"
// look like variance) with one counter slot per apply site, so each path is
// observable on its own in the render-state dump. The aggregate is now derived
// as the SUM over these ids (back-compat: the total is byte-stable).
// 4 top-level applies (TerrainDecal, TerrainOverlay, StaticPropOpaque, MechOpaque); Water left with the GL water draw (s62). Shadow left with the GL dynamic shadow pass (s61); the
// 5 PostProcess sub-stage applies (EdgeFog, FogOob, Shoreline, CloudShadow, ScreenShadow) left with the GL passes (s62).
enum class ApplyPassId : uint8_t {
    TerrainDecal = 0,
    TerrainOverlay,
    StaticPropOpaque,
    MechOpaque,   // APPLY-STATE-MECHOPAQUE-1: top-level mech-opaque pipeline apply
    Count,
};

// Exact stable string names for the dump keys (must match the dump map keys).
inline const char* applyPassName(ApplyPassId id) {
    switch (id) {
        case ApplyPassId::TerrainDecal:            return "TerrainDecal";
        case ApplyPassId::TerrainOverlay:          return "TerrainOverlay";
        case ApplyPassId::StaticPropOpaque:        return "StaticPropOpaque";
        case ApplyPassId::MechOpaque:              return "MechOpaque";
        default:                                   return "Unknown";
    }
}

}} // namespace RenderCore::framegraph
