#pragma once
// SAME-ORDER-EXECUTOR-VALIDATE-1 — pure/constexpr top-level pass executor descriptor.
//
// Declares WHICH top-level (frame-order) passes the executor validate-owns and what
// preconditions to check. Contains NO GL, NO runtime state. The GL-touching runtime
// wrapper lives in mclib/render_contract.cpp (executorOwnBeginTopLevel /
// executorOwnEndTopLevel). This header is offline-testable (tests/unit/test_frame_graph.cpp).
//
// Design note: VALIDATE-ONLY. The body still sets its own state; the executor wraps,
// validates preconditions + postconditions, and counts. NO glApplyState, NO reorder,
// NO scheduling.
//
// Gate: MC2_FRAMEGRAPH_EXECUTOR (default-OFF). When unset -> executorOwnBeginTopLevel /
// executorOwnEndTopLevel are early-return no-ops -> byte-identical to unwrapped draw.
//
// DEFERRED (beyond slice 2): Water, VFX, UI (ambient/FBO gaps per same-order recon).
// Shadow and MechOpaque are NOW OWNED in this slice (SAME-ORDER-EXECUTOR-SLICE-2):
//   Shadow   — dynamic-shadow seam: gos_BeginDynamicShadowPass / gos_EndDynamicShadowPass
//              (txmmgr.cpp); noteRenderPass(ShadowCaster) fires after FBO bind in
//              gosRenderer::beginDynamicShadowPass (SHADOW-OBSERVE-3).
//   MechOpaque — GpuMechBatcher::flush() seam in renderLists(); noteRenderPass(OpaqueObject)
//              relocated from preamble to the real draw site (MECHOPAQUE-NOTE-RELOCATE-1).

#include "RenderCore/RenderPassContract.h"  // RenderPassId
#include "RenderCore/PipelineRegistry.h"      // PipelineId (APPLY-STATE-TERRAINDECAL-1)
#include "RenderCore/ambient_contract.h"      // ViewportKind (APPLY-STATE-TERRAINDECAL-1)
#include "RenderCore/RenderResourceRegistry.h" // RenderResourceId (APPLY-STATE-TERRAINDECAL-1)

namespace RenderCore { namespace framegraph {

// What the executor validates at begin/end of a top-level pass.
// All fields are plain bool; no GL, no runtime state.
struct TopLevelPassContract {
    RenderPassId id;
    bool         validateAmbient; // check ambient ledger (colorMask/depthFunc/depthWrite) on begin
    bool         validateFbo;     // check FBO ledger (declared vs live draw FBO) on begin
    const char*  note;            // human-readable label (for log/debug only)
};

// Compile-time table of top-level passes the executor validate-owns this slice.
// Order is documentary; the executor does NOT reorder.
//
// VegetationCards: gated MC2_VEGETATION_CARDS default-OFF -> legitimately fires 0 times
// per frame in default smoke. That is correct and NOT a validation failure.
// Native shadow/water passes remain observed, without GL state or FBO validation.
static constexpr TopLevelPassContract kTopLevelExecutorPasses[] = {
    {
        /*id*/              RenderPassId::Shadow,
        /*validateAmbient*/ false,   // native Vulkan owns the shadow state
        /*validateFbo*/     false,   // native Vulkan owns the shadow images
        /*note*/            "Shadow/gosRenderer_beginDynamicShadowPass",
    },
    {
        /*id*/              RenderPassId::MechOpaque,
        /*validateAmbient*/ true,   // AmbientContract row: SceneGEqual depthFunc, MainScene viewport
        /*validateFbo*/     true,   // FBO ledger: MainColor (scene FBO, established by terrain)
        /*note*/            "MechOpaque/GpuMechBatcher_flush",
    },
    {
        /*id*/              RenderPassId::StaticPropOpaque,
        /*validateAmbient*/ true,   // AmbientContract row exists (SceneGEqual, depthWrite=On)
        /*validateFbo*/     true,   // FBO ledger declares MainColor target
        /*note*/            "StaticPropOpaque/GpuStaticPropBatcher_flush",
    },
    {
        /*id*/              RenderPassId::Terrain,
        /*validateAmbient*/ true,   // AmbientContract row exists (SceneGEqual, depthWrite=On, AllOn colorMask)
        /*validateFbo*/     true,   // FBO ledger declares MainColor target
        /*note*/            "Terrain/gos_TerrainLodChunk_SubmitDrawCommands",
    },
    {
        /*id*/              RenderPassId::TerrainOverlay,
        /*validateAmbient*/ false,  // no AmbientContract row for TerrainOverlay
        /*validateFbo*/     true,   // FBO ledger declares MainColor target
        /*note*/            "TerrainOverlay/gosRenderer_drawTerrainOverlays",
    },
    {
        /*id*/              RenderPassId::TerrainDecal,
        /*validateAmbient*/ false,  // no AmbientContract row for TerrainDecal
        /*validateFbo*/     true,   // FBO ledger declares MainColor target
        /*note*/            "TerrainDecal/gosRenderer_drawDecals",
    },
    {
        /*id*/              RenderPassId::Water,
        /*validateAmbient*/ false,   // AmbientContract row: SceneGEqual depthFunc, depthWrite On, MainScene viewport
        /*validateFbo*/     false,   // FBO ledger declares MainColor target (scene FBO)
        /*note*/            "Water/gosRenderer_renderWaterFastPath",
    },
    {
        /*id*/              RenderPassId::VegetationCards,
        /*validateAmbient*/ false,  // DRYRUN-OBSERVE-COVERAGE-1: no AmbientContract row
        /*validateFbo*/     false,  // no declared FBO target for VegetationCards
        /*note*/            "VegetationCards/GosVegetation_flush",
    },
    {
        /*id*/              RenderPassId::VFX,
        /*validateAmbient*/ false,  // VFX-FBO-ONLY-VALIDATE-1: note seam (gamecam:594) fires
                                    // pre-body in a different TU -> ambient not honestly declarable
        /*validateFbo*/     true,   // FBO ledger declares MainColor (scene HDR FBO bound across whole flush window)
        /*note*/            "VFX/gamecam_particlesFlush",
    },
    {
        /*id*/              RenderPassId::UI,
        /*validateAmbient*/ false,  // UI-PASS-MODEL-1: UI now HAS an AmbientContract row and is
                                    // verified OBSERVE-ONLY at UI entry (mc2_ui_pass_check_ambient,
                                    // own non-fatal counter). Deliberately NOT routed through this
                                    // executor validateAmbient path yet — that shares the default-ON
                                    // FATAL g_ambientMismatchCount. Flip to true only after the UI
                                    // observe counter is verified 0 across all tier1 (measure-first).
        /*validateFbo*/     true,   // FBO ledger declares Backbuffer (default FBO 0, post-composite)
        /*note*/            "UI/gosRenderer_flushHUDBatch",
    },
};
static constexpr unsigned kTopLevelExecutorPassCount =
    sizeof(kTopLevelExecutorPasses) / sizeof(kTopLevelExecutorPasses[0]);

// Find the TopLevelPassContract for the given pass id, or nullptr if not executor-owned.
// Pure, constexpr-compatible, no GL.
inline const TopLevelPassContract* findTopLevelExecutorPass(RenderPassId id) {
    for (unsigned i = 0; i < kTopLevelExecutorPassCount; ++i) {
        if (kTopLevelExecutorPasses[i].id == id)
            return &kTopLevelExecutorPasses[i];
    }
    return nullptr;
}

// Count of deferred top-level passes (not executor-owned this slice).
// none = 0. (UI now owned FBO-only in UI-SAME-ORDER-VALIDATE-1 -> EVERY top-level pass is
// executor-owned; VFX in VFX-FBO-ONLY-VALIDATE-1; Water in WATER-SAME-ORDER-VALIDATE-1;
// Shadow + MechOpaque in SLICE-2.)
// Called by the dump to populate executor_skipped_deferred_passes.
static constexpr unsigned kTopLevelDeferredPassCount = 0u;

// ---- APPLY-STATE-TERRAINDECAL-1: top-level APPLY-STATE descriptor table -----
//
// The executor's top-level apply-state table (the PostProcess sub-stage analog left with the GL post passes). This table is
// the FIRST step toward the north-star: executor applies declared pipeline state,
// the pass body stops self-setting it.
//
// SCOPE THIS SLICE: only the pipeline is lifted. fboTarget/viewport are recorded
// as Inherit/Unknown to be HONEST that they are NOT applied here — TerrainDecal
// inherits Terrain's scene FBO / drawBuffers / viewport (recon ground-truth:
// drawDecals() at gameos_graphics.cpp does no FBO/viewport setup at entry; its
// only entry render-state is applyPipeline(TerrainDecal)). pipelineId is reused
// from the authoritative kPassRenderState[] row (render_state_desc.h), NOT
// duplicated — see findTopLevelStateDesc().
//
// Pure / constexpr / GL-free. The GL-touching apply lives in the gosRenderer TU
// (gameos_graphics.cpp executorApplyTerrainDecalState()) where applyPipeline /
// getPipelineDesc are already used. This header is offline-testable.
// FRAMEGRAPH-APPLY-STATE-EXTEND-1: optional depth-clear axis applied by the generic-axes
// helper. None = skip (every existing consumer; default). DepthForwardZ = forward-Z depth
// clear (glClearDepth(1)/glClear(DEPTH)/glClearDepth(0)) — NOT used this slice; implemented
// so the upcoming Shadow slice can declare it. depthFunc/depthWrite are deliberately OMITTED:
// the ambient ledger owns those axes.
enum class ClearSpec : uint8_t { None = 0, DepthForwardZ };

struct TopLevelStateDesc {
    RenderPassId           id;
    RenderCore::PipelineId pipelineId;  // lifted this slice: executor applies via applyPipeline()
    RenderResourceId       fboTarget;   // Unknown = NOT applied (body inherits it); else applied
    ViewportKind           viewport;    // Inherit = NOT applied (body inherits it); else applied
    // FRAMEGRAPH-APPLY-STATE-EXTEND-1: trailing optional axis. Defaults to None (skip) so the
    // 4 unchanged rows below remain byte-identical (aggregate init leaves it at the member
    // initializer). Only StaticPropOpaque sets fboTarget/viewport explicitly this slice.
    ClearSpec              clear = ClearSpec::None;
};

// Compile-time table of top-level passes whose render-state the executor APPLIES.
// Consumers: TerrainDecal, TerrainOverlay, StaticPropOpaque, MechOpaque, Water (pipeline lifts;
// StaticProp also self-proofs explicit FBO/viewport) + Shadow (APPLY-STATE-SHADOW-1: first
// render-target-MODE consumer — pipeline + DepthForwardZ clear; FBO/viewport deferred to SHADOW-2).
static constexpr TopLevelStateDesc kTopLevelStateDesc[] = {
    // APPLY-STATE-MECHOPAQUE-1: MechOpaque — GpuMechBatcher::flush(): the SOLE entry
    // render-state is applyPipeline(getPipelineDesc(MechOpaque)) (~gos_mech_batcher.cpp:2152).
    // Like StaticPropOpaque, MechOpaque runs EVERY tier1 frame, so the ON-path body-skip is
    // runtime-exercised (provable via the per-pass apply counter). flush() has no
    // glBindFramebuffer/glDrawBuffers/glViewport — FBO/MRT/objectId(loc2)/viewport are all
    // inherited from the preceding Terrain/StaticProp passes. objectId is an SSBO per-instance
    // + shader #define, not a body drawBuffers call. MVP is a view-UBO uniform (binding=3),
    // NOT pipeline state -> pin-safe. flushShadow() (the SEPARATE Shadow pass) is NOT lifted.
    // FBO/MRT/viewport deliberately recorded as Unknown/Inherit (same honesty shape as above).
    {
        /*id*/         RenderPassId::MechOpaque,
        /*pipelineId*/ RenderCore::PipelineId::MechOpaque,
        /*fboTarget*/  RenderResourceId::Unknown,   // inherit (not applied)
        /*viewport*/   ViewportKind::Inherit,       // inherit (not applied)
    },
};
static constexpr unsigned kTopLevelStateDescCount =
    sizeof(kTopLevelStateDesc) / sizeof(kTopLevelStateDesc[0]);

// Find the TopLevelStateDesc for the given pass id, or nullptr if the executor does
// not apply state for it. Pure, constexpr-compatible, no GL.
inline const TopLevelStateDesc* findTopLevelStateDesc(RenderPassId id) {
    for (unsigned i = 0; i < kTopLevelStateDescCount; ++i) {
        if (kTopLevelStateDesc[i].id == id)
            return &kTopLevelStateDesc[i];
    }
    return nullptr;
}

}} // namespace RenderCore::framegraph
