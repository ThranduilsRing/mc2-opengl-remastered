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
    HzbReduce,        // sub-stage: Hi-Z pyramid build (draw, hzbFBO_); NOT executor-owned; gated MC2_HZB_BUILD default-OFF
    HzbProbe,         // sub-stage: HZB CPU diagnostic readback (no draw); NOT executor-owned; gated MC2_HZB_PROBE default-OFF
    ClusterDepthPyramid, // sub-stage: per-tile (min,max) depth image compute dispatch; NOT executor-owned; gated MC2_CLUSTER_DEPTH_PYRAMID default-OFF; isCompute=true
    LightgridBuild,   // sub-stage: per-tile light-bin grid compute dispatch; NOT executor-owned; gated MC2_LIGHTGRID_BUILD default-OFF; isCompute=true
    PostprocessComputeBlur, // sub-stage: GPU compute downsample+Gaussian blur substrate; NOT executor-owned; gated MC2_POSTPROCESS_COMPUTE_BLUR default-OFF; isCompute=true
    ScreenShadow,     // sub-stage: screen-space shadow (draw, sceneFBO_); EXECUTOR-ISLAND-SCREENSHADOW-1 (validate-only); tex-unit leak fixed by a0b4189b; gated screenShadowEnabled_+shadowsEnabled_ default-ON in-mission
    Ssao,             // sub-stage: GTAO-lite AO (two-pass: ssaoFBO_ then sceneFBO_); NOT executor-owned; gated MC2_SSAO default-OFF
    BoxDecals,        // sub-stage: screen-space box decal (draw, sceneFBO_); NOT executor-owned; reads SceneDepthCopy cross-boundary; gated MC2_PROJECTED_DECALS default-OFF
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
        /*requiresProgramValid*/    true,
        /*requiresSceneColorTex*/   true,
        /*requiresSceneDepthTex*/   false,
        /*warnIfNoTerrainLatch*/    true,   // sub-stages bail gracefully; WARN not FATAL
        /*postRequiresDefaultFbo*/  true,   // composite blit explicitly binds FBO 0 at end
        /*postRequiresBlendDisabled*/false, // PostProcess end-state not asserted (composite varies)
        /*postRequiresActiveTexture0*/false,
    },
    {
        /*id*/                      ExecutorIslandId::EdgeFog,
        /*requiresProgramValid*/    true,
        /*requiresSceneColorTex*/   false,
        /*requiresSceneDepthTex*/   true,   // EdgeFog reads sceneDepthTex_, not sceneColorTex_
        /*warnIfNoTerrainLatch*/    true,   // runEdgeFog() bails on !sceneHasTerrain_
        /*postRequiresDefaultFbo*/  false,  // stays on sceneFBO_, not FBO 0
        /*postRequiresBlendDisabled*/true,  // runEdgeFog() calls glDisable(GL_BLEND) on exit
        /*postRequiresActiveTexture0*/true, // runEdgeFog() calls glActiveTexture(GL_TEXTURE0) on exit
    },
    {
        /*id*/                      ExecutorIslandId::FogOob,
        /*requiresProgramValid*/    true,
        /*requiresSceneColorTex*/   false,
        /*requiresSceneDepthTex*/   true,   // FogOob reads sceneDepthTex_
        /*warnIfNoTerrainLatch*/    true,   // runFogOob() bails on !sceneHasTerrain_
        /*postRequiresDefaultFbo*/  false,  // stays on sceneFBO_, not FBO 0
        /*postRequiresBlendDisabled*/true,  // runFogOob() calls glDisable(GL_BLEND) on exit
        /*postRequiresActiveTexture0*/true, // runFogOob() calls glActiveTexture(GL_TEXTURE0) on exit
    },
    // ISLAND-3: Shoreline foam pass (reads sceneDepthTex_ + sceneNormalTex_, stays sceneFBO_)
    {
        /*id*/                      ExecutorIslandId::Shoreline,
        /*requiresProgramValid*/    true,
        /*requiresSceneColorTex*/   false,
        /*requiresSceneDepthTex*/   true,   // Shoreline reads sceneDepthTex_ (unit 0) + sceneNormalTex_ (unit 1)
        /*warnIfNoTerrainLatch*/    true,   // runShoreline() bails on !sceneHasTerrain_
        /*postRequiresDefaultFbo*/  false,  // stays on sceneFBO_, not FBO 0
        /*postRequiresBlendDisabled*/true,  // runShoreline() calls glDisable(GL_BLEND) on exit
        /*postRequiresActiveTexture0*/true, // runShoreline() calls glActiveTexture(GL_TEXTURE0) on exit
    },
    // ISLAND-3: CloudShadow pass (reads sceneDepthTex_ only, stays sceneFBO_)
    {
        /*id*/                      ExecutorIslandId::CloudShadow,
        /*requiresProgramValid*/    true,
        /*requiresSceneColorTex*/   false,
        /*requiresSceneDepthTex*/   true,   // CloudShadow reads sceneDepthTex_ (unit 0 only)
        /*warnIfNoTerrainLatch*/    true,   // runCloudShadow() bails on !sceneHasTerrain_
        /*postRequiresDefaultFbo*/  false,  // stays on sceneFBO_, not FBO 0
        /*postRequiresBlendDisabled*/true,  // runCloudShadow() calls glDisable(GL_BLEND) on exit
        /*postRequiresActiveTexture0*/true, // runCloudShadow() calls glActiveTexture(GL_TEXTURE0) on exit
    },
    // EXECUTOR-ISLAND-SCREENSHADOW-1: ScreenShadow — validate-only (body unchanged).
    // Unblocked by SCREENSHADOW-TEX-RESTORE-1 (a0b4189b): unit-3 2D_ARRAY now unbound at exit.
    // Exit state (ground-truthed from runScreenShadow() lines 2149-2160):
    //   glDisable(GL_BLEND)          at line 2150 — postRequiresBlendDisabled=true
    //   glActiveTexture(GL_TEXTURE0) at line 2160 — postRequiresActiveTexture0=true
    //   stays on sceneFBO_           — postRequiresDefaultFbo=false
    // Gates: screenShadowEnabled_ && sceneHasTerrain_ && screenShadowProg_ valid && shadowsEnabled_
    {
        /*id*/                       ExecutorIslandId::ScreenShadow,
        /*requiresProgramValid*/     true,
        /*requiresSceneColorTex*/    false,
        /*requiresSceneDepthTex*/    true,   // reads sceneDepthTex_(unit0)+sceneNormalTex_(unit1)
        /*warnIfNoTerrainLatch*/     true,   // runScreenShadow() bails on !sceneHasTerrain_
        /*postRequiresDefaultFbo*/   false,  // stays on sceneFBO_, not FBO 0
        /*postRequiresBlendDisabled*/true,   // glDisable(GL_BLEND) at line 2150
        /*postRequiresActiveTexture0*/true,  // glActiveTexture(GL_TEXTURE0) at line 2160
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

// ---------------------------------------------------------------------------
// FRAMEGRAPH-APPLY-STATE-ISLAND-1: SubStageStateDesc — GL-free descriptor for
// the executor's pre-apply of a sub-stage's declared GL state.
//
// The table is the vocabulary; only EdgeFog is WIRED in this slice (slice 2
// removes the body's redundant apply once all 4 are wired).
// Fields:
//   id         — sub-stage island id
//   pipelineId — PipelineId used at the top of the body (passed to applyPipeline)
//   fboTarget  — logical resource the body binds as the draw FBO (MainColor=sceneFBO_)
//   viewport   — viewport scope (MainScene = glViewport(0,0,width_,height_))
// Pure, constexpr-compatible, no GL.
// ---------------------------------------------------------------------------
struct SubStageStateDesc {
    ExecutorIslandId         id;
    RenderCore::PipelineId       pipelineId;
    RenderCore::RenderResourceId fboTarget;
    ViewportKind                 viewport;    // RenderCore::framegraph::ViewportKind (same namespace)
};

static constexpr SubStageStateDesc kSubStageState[] = {
    // EdgeFog — runEdgeFog(): bindFB(sceneFBO_) + SingleColor + viewport + applyPipeline(PostProcessEdgeFog)
    {
        /*id*/         ExecutorIslandId::EdgeFog,
        /*pipelineId*/ RenderCore::PipelineId::PostProcessEdgeFog,
        /*fboTarget*/  RenderCore::RenderResourceId::MainColor,
        /*viewport*/   ViewportKind::MainScene,
    },
    // FogOob — same entry state as EdgeFog (validate-only this slice; not wired to apply yet)
    {
        /*id*/         ExecutorIslandId::FogOob,
        /*pipelineId*/ RenderCore::PipelineId::PostProcessFogOob,
        /*fboTarget*/  RenderCore::RenderResourceId::MainColor,
        /*viewport*/   ViewportKind::MainScene,
    },
    // Shoreline (validate-only this slice)
    {
        /*id*/         ExecutorIslandId::Shoreline,
        /*pipelineId*/ RenderCore::PipelineId::PostProcessShoreline,
        /*fboTarget*/  RenderCore::RenderResourceId::MainColor,
        /*viewport*/   ViewportKind::MainScene,
    },
    // CloudShadow (validate-only this slice)
    {
        /*id*/         ExecutorIslandId::CloudShadow,
        /*pipelineId*/ RenderCore::PipelineId::PostProcessCloudShadow,
        /*fboTarget*/  RenderCore::RenderResourceId::MainColor,
        /*viewport*/   ViewportKind::MainScene,
    },
    // ScreenShadow — runScreenShadow(): bindFB(sceneFBO_) + SingleColor + viewport + applyPipeline(PostProcessScreenShadow)
    // APPLY-STATE-SCREENSHADOW-1: 6th apply-state island (matches CloudShadow shape).
    {
        /*id*/         ExecutorIslandId::ScreenShadow,
        /*pipelineId*/ RenderCore::PipelineId::PostProcessScreenShadow,
        /*fboTarget*/  RenderCore::RenderResourceId::MainColor,
        /*viewport*/   ViewportKind::MainScene,
    },
};
static constexpr unsigned kSubStageStateCount =
    sizeof(kSubStageState) / sizeof(kSubStageState[0]);

// Find the SubStageStateDesc for the given island id, or nullptr if not in table.
// Pure, constexpr-compatible, no GL.
inline const SubStageStateDesc* findSubStageState(ExecutorIslandId id) {
    for (unsigned i = 0; i < kSubStageStateCount; ++i) {
        if (kSubStageState[i].id == id)
            return &kSubStageState[i];
    }
    return nullptr;
}

// PER-PASS-APPLY-COUNTERS-1 — independent observability id per apply-state path.
// Replaces the coarse aggregate g_applyStatePasses (which made decal "rise"
// look like variance) with one counter slot per apply site, so each path is
// observable on its own in the render-state dump. The aggregate is now derived
// as the SUM over these ids (back-compat: the total is byte-stable).
// 5 PostProcess sub-stage applies + 5 top-level applies (TerrainDecal,
// TerrainOverlay, StaticPropOpaque, MechOpaque, Water). Shadow left with the GL dynamic shadow pass (s61).
enum class ApplyPassId : uint8_t {
    PostProcessEdgeFog = 0,
    PostProcessFogOob,
    PostProcessShoreline,
    PostProcessCloudShadow,
    PostProcessScreenShadow,
    TerrainDecal,
    TerrainOverlay,
    StaticPropOpaque,
    MechOpaque,   // APPLY-STATE-MECHOPAQUE-1: top-level mech-opaque pipeline apply
    Water,        // APPLY-STATE-WATER-1: top-level water pipeline apply (body-site post-compute-dispatch)
    Count,
};

// Exact stable string names for the dump keys (must match the dump map keys).
inline const char* applyPassName(ApplyPassId id) {
    switch (id) {
        case ApplyPassId::PostProcessEdgeFog:      return "PostProcessEdgeFog";
        case ApplyPassId::PostProcessFogOob:       return "PostProcessFogOob";
        case ApplyPassId::PostProcessShoreline:    return "PostProcessShoreline";
        case ApplyPassId::PostProcessCloudShadow:  return "PostProcessCloudShadow";
        case ApplyPassId::PostProcessScreenShadow: return "PostProcessScreenShadow";
        case ApplyPassId::TerrainDecal:            return "TerrainDecal";
        case ApplyPassId::TerrainOverlay:          return "TerrainOverlay";
        case ApplyPassId::StaticPropOpaque:        return "StaticPropOpaque";
        case ApplyPassId::MechOpaque:              return "MechOpaque";
        case ApplyPassId::Water:                   return "Water";
        default:                                   return "Unknown";
    }
}

}} // namespace RenderCore::framegraph
