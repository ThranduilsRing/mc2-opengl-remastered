#pragma once
// FRAMEGRAPH-STATEPACK-SKELETON-1 — GL-free per-pass RenderStateDesc vocabulary.
//
// Unifies the already-declared/already-sampled state axes from three existing ledgers
// into a single per-pass descriptor:
//   - PipelineId (from PipelineRegistry.h) — owns blend/depth-test/cull/frontFace/
//     polygonOffset/program via applyPipeline(). StatePack reuses PipelineDesc; it does
//     NOT re-declare blend/cull/etc.
//   - ColorMaskState, DepthWriteState, DepthFuncState, ViewportKind (ambient_contract.h)
//   - RenderResourceId (FBO logical target) — fbo_ledger.h / kPassFboTarget[]
//
// SCHEMA / VALIDATION ONLY. This file is deliberately non-authoritative:
//   - It does NOT call any GL function.
//   - It does NOT drive the renderer.
//   - It does NOT replace applyPipeline, ambient_contract, or fbo_ledger — those remain
//     the per-axis source-of-truth. The RenderStateDesc struct is a UNION for inspection
//     and drift-detection only.
//   - validatePassRenderStateConsistency() is the primary gate: it cross-checks each
//     RenderStateDesc row against the per-axis ledgers it was derived from.
//
// Adding a new pass: append a row to kPassRenderState and update the
// static_assert count. Fill from the three source ledgers.

#include "RenderPassContract.h"      // RenderPassId, kRenderPassIdCount
#include "PipelineRegistry.h"        // PipelineId
#include "ambient_contract.h"        // ColorMaskState, DepthWriteState, DepthFuncState, ViewportKind, findAmbient
#include "fbo_ledger.h"              // RenderResourceId, declaredFboTarget
#include "RenderResourceRegistry.h"  // RenderResourceId

namespace RenderCore { namespace framegraph {

// ---------------------------------------------------------------------------
// Per-pass state descriptor
// ---------------------------------------------------------------------------
// PipelineId encapsulates blend(semantic), depthTest, depthWrite, depthFunc, cull,
// frontFace, polygonOffset, program — do NOT add those axes here.
// The remaining axes (colorMask, viewport, fboTarget) are not owned by PipelineDesc.

struct RenderStateDesc {
    RenderPassId     id;
    RenderCore::PipelineId pipelineId;   // Invalid = no single representative pipeline (UI, Shadow, VFX-multi, Vegetation)
    ColorMaskState   colorMask;          // from ambient_contract kPassAmbient[].colorMaskOnEntry; Inherit = undeclared
    DepthWriteState  depthWrite;         // from ambient_contract kPassAmbient[].depthWrite; Inherit = undeclared
    DepthFuncState   depthFunc;          // from ambient_contract kPassAmbient[].depthFunc; Inherit = undeclared
    ViewportKind     viewport;           // from ambient_contract kPassAmbient[].viewport; Inherit = undeclared
    RenderResourceId fboTarget;          // from fbo_ledger declaredFboTarget(); Unknown = undeclared
};

// ---------------------------------------------------------------------------
// Per-pass table — ONE row per top-level RenderPassId (kRenderPassIdCount rows).
// Order matches kFramePassOrder logical ordering.
//
// Source derivation rules:
//   pipelineId     = PipelineId used by the pass via applyPipeline(); Invalid if none/multi
//   colorMask      = findAmbient(pass)->colorMaskOnEntry      (else Inherit)
//   depthWrite     = findAmbient(pass)->depthWrite            (else Inherit)
//   depthFunc      = findAmbient(pass)->depthFunc             (else Inherit)
//   viewport       = findAmbient(pass)->viewport              (else Inherit)
//   fboTarget      = declaredFboTarget(pass)                  (else Unknown)
//
// IMPORTANT: Do NOT aspirational-fill. Use Inherit/Unknown if the ledger does not
// declare a value. The consistency validator enforces that declared values match.
// ---------------------------------------------------------------------------
static constexpr RenderStateDesc kPassRenderState[] = {
    // Shadow: three sub-caster pipelines (ShadowTerrain/ShadowMech/ShadowStaticProp),
    // each DESCRIPTIVE-only (pipelineDescRegistered=false in contract, explicitly
    // noted in PipelineRegistry.h as "NOT routed through applyPipeline"). No single
    // representative PipelineId -> Invalid.
    // ambient_contract: colorMask=Inherit, depthWrite=On, depthFunc=ShadowLess, viewport=ShadowMap.
    // fbo: not declared in kPassFboTarget (shadow FBO is its own; not the scene MainColor).
    {
        RenderPassId::Shadow,
        RenderCore::PipelineId::Invalid,
        ColorMaskState::Inherit,
        DepthWriteState::On,
        DepthFuncState::ShadowLess,
        ViewportKind::ShadowMap,
        RenderResourceId::Unknown
    },
    // MechOpaque: pipelineDescRegistered=true (MECH-PIPELINEDESC-1). applyPipeline routes
    // through PipelineId::MechOpaque (gos_mech_batcher.cpp:2152).
    // ambient_contract: colorMask=Inherit, depthWrite=On, depthFunc=SceneGEqual, viewport=MainScene.
    // fbo: not in kPassFboTarget (probe uncertain) -> Unknown.
    {
        RenderPassId::MechOpaque,
        RenderCore::PipelineId::MechOpaque,
        ColorMaskState::Inherit,
        DepthWriteState::On,
        DepthFuncState::SceneGEqual,
        ViewportKind::MainScene,
        RenderResourceId::Unknown
    },
    // StaticPropOpaque: pipelineDescRegistered=true. applyPipeline routes through
    // PipelineId::StaticPropOpaque (gos_static_prop_batcher.cpp:5252/5492/7788/7941).
    // ambient_contract: colorMask=Inherit, depthWrite=On, depthFunc=SceneGEqual, viewport=MainScene.
    // fbo: kPassFboTarget declares MainColor.
    {
        RenderPassId::StaticPropOpaque,
        RenderCore::PipelineId::StaticPropOpaque,
        ColorMaskState::Inherit,
        DepthWriteState::On,
        DepthFuncState::SceneGEqual,
        ViewportKind::MainScene,
        RenderResourceId::MainColor
    },
    // Native terrain owns its pipeline; keep the render-state vocabulary for ordering checks.
    {
        RenderPassId::Terrain,
        RenderCore::PipelineId::Invalid,
        ColorMaskState::AllOn,
        DepthWriteState::On,
        DepthFuncState::SceneGEqual,
        ViewportKind::MainScene,
        RenderResourceId::MainColor
    },
    // TerrainOverlay: routed via applyPipeline(TerrainOverlay) in gameos_graphics.cpp:9810.
    // pipelineDescRegistered = true (FRAMEGRAPH-STATEPACK-SKELETON-1).
    // ambient_contract: not declared (Inherit/Inherit/Inherit/Inherit).
    // fbo: kPassFboTarget declares MainColor.
    {
        RenderPassId::TerrainOverlay,
        RenderCore::PipelineId::TerrainOverlay,
        ColorMaskState::Inherit,
        DepthWriteState::Inherit,
        DepthFuncState::Inherit,
        ViewportKind::Inherit,
        RenderResourceId::MainColor
    },
    // TerrainDecal: routed via applyPipeline(TerrainDecal) in gameos_graphics.cpp:10005.
    // pipelineDescRegistered = true (FRAMEGRAPH-STATEPACK-SKELETON-1).
    // ambient_contract: not declared.
    // fbo: kPassFboTarget declares MainColor.
    {
        RenderPassId::TerrainDecal,
        RenderCore::PipelineId::TerrainDecal,
        ColorMaskState::Inherit,
        DepthWriteState::Inherit,
        DepthFuncState::Inherit,
        ViewportKind::Inherit,
        RenderResourceId::MainColor
    },
    // Water: routed via applyPipeline(WaterArmed) in gameos_graphics.cpp:3287.
    // pipelineDescRegistered = true (FRAMEGRAPH-STATEPACK-SKELETON-1).
    // ambient_contract: not declared (Water has no kPassAmbient row).
    // fbo: not in kPassFboTarget -> Unknown.
    {
        RenderPassId::Water,
        RenderCore::PipelineId::Invalid,
        ColorMaskState::Inherit,
        DepthWriteState::Inherit,
        DepthFuncState::Inherit,
        ViewportKind::Inherit,
        RenderResourceId::Unknown
    },
    // VegetationCards: no applyPipeline callsite found (gos_vegetation.cpp,
    // VegetationAdapter.cpp — zero hits). pipelineDescRegistered=false confirmed correct.
    // ambient_contract: not declared.
    // fbo: not in kPassFboTarget -> Unknown.
    {
        RenderPassId::VegetationCards,
        RenderCore::PipelineId::Invalid,
        ColorMaskState::Inherit,
        DepthWriteState::Inherit,
        DepthFuncState::Inherit,
        ViewportKind::Inherit,
        RenderResourceId::Unknown
    },
    // VFX: multiple pipelines per draw (VfxBillboard/Tube/Mesh x Alpha/Additive) — no single
    // representative PipelineId -> Invalid. All sub-pipelines do route applyPipeline
    // (gos_particle_bridge.cpp:847/1263, gos_vfx_mesh_bridge.cpp:315).
    // pipelineDescRegistered = true (FRAMEGRAPH-STATEPACK-SKELETON-1; multiple pipelines, so
    // the representative PipelineId is Invalid, but the lane does route applyPipeline).
    // ambient_contract: not declared.
    // fbo: not in kPassFboTarget -> Unknown.
    {
        RenderPassId::VFX,
        RenderCore::PipelineId::Invalid,
        ColorMaskState::Inherit,
        DepthWriteState::Inherit,
        DepthFuncState::Inherit,
        ViewportKind::Inherit,
        RenderResourceId::Unknown
    },
    // UI: runtime-dynamic blend per draw; no applyPipeline routing; HUD/text/menu.
    // pipelineDescRegistered=false confirmed correct. PipelineId::Invalid.
    // ambient_contract: not declared.
    // fbo: not in kPassFboTarget -> Unknown.
    {
        RenderPassId::UI,
        RenderCore::PipelineId::Invalid,
        ColorMaskState::Inherit,
        DepthWriteState::Inherit,
        DepthFuncState::Inherit,
        ViewportKind::Inherit,
        RenderResourceId::Unknown
    },
    // PostProcess: multiple sub-pipelines (PostProcessComposite/ScreenShadow/CloudShadow/
    // Shoreline/EdgeFog/FogOob) all route applyPipeline in gos_postprocess.cpp.
    // pipelineDescRegistered = true (FRAMEGRAPH-STATEPACK-SKELETON-1; multiple sub-pipelines, so
    // the representative PipelineId is Invalid, but the lane does route applyPipeline).
    // No single representative -> Invalid (the executor-island model in frame_executor.h
    // models sub-pipelines individually; that is the right granularity for PostProcess).
    // ambient_contract: colorMask=Inherit, depthFunc=Inherit, viewport=MainScene, consumesTerrainLatch.
    // fbo: not declared in kPassFboTarget (timing-uncertain at sample seam, per fbo_ledger.h comment).
    {
        RenderPassId::PostProcess,
        RenderCore::PipelineId::Invalid,
        ColorMaskState::Inherit,
        DepthWriteState::Inherit,
        DepthFuncState::Inherit,
        ViewportKind::MainScene,
        RenderResourceId::Unknown
    },
};
static constexpr int kPassRenderStateCount =
    static_cast<int>(sizeof(kPassRenderState) / sizeof(kPassRenderState[0]));
static_assert(kPassRenderStateCount == static_cast<int>(kRenderPassIdCount),
    "kPassRenderState must have one row per RenderPassId (update both when adding a pass).");

// ---------------------------------------------------------------------------
// Lookup
// ---------------------------------------------------------------------------
inline const RenderStateDesc* findPassRenderState(RenderPassId id) {
    for (int i = 0; i < kPassRenderStateCount; ++i)
        if (kPassRenderState[i].id == id) return &kPassRenderState[i];
    return nullptr;
}

// True iff the pass has a statically-declarable single pipeline (i.e., pipelineId != Invalid).
// False for UI (runtime-dynamic blend), Shadow (multi-caster sub-pipelines, descriptive only),
// PostProcess (multi-sub-pipeline), VFX (multi-pipeline per draw), VegetationCards (none).
// An executor uses this to know which passes can have their pipeline state pre-declared.
inline bool passHasStaticPipeline(RenderPassId id) {
    const RenderStateDesc* r = findPassRenderState(id);
    return r != nullptr && r->pipelineId != RenderCore::PipelineId::Invalid;
}

// ---------------------------------------------------------------------------
// Consistency validator
// ---------------------------------------------------------------------------
// Cross-checks each RenderStateDesc row against the per-axis ledger it was derived from.
// This proves the unified struct AGREES with kPassAmbient[] and kPassFboTarget[] — no drift.
//
// Axes compared:
//   colorMask  : RenderStateDesc::colorMask  vs findAmbient(pass)->colorMaskOnEntry
//   depthWrite : RenderStateDesc::depthWrite vs findAmbient(pass)->depthWrite
//   depthFunc  : RenderStateDesc::depthFunc  vs findAmbient(pass)->depthFunc
//   viewport   : RenderStateDesc::viewport   vs findAmbient(pass)->viewport
//   fboTarget  : RenderStateDesc::fboTarget  vs declaredFboTarget(pass)
//
// Convention: if EITHER side is Inherit/Unknown, the axis is SKIPPED (no false positive).
// Only when BOTH sides have a concrete value and they differ is it counted as a violation.
enum class StatePackAxis : uint8_t {
    None = 0,
    ColorMask,
    DepthWrite,
    DepthFunc,
    Viewport,
    FboTarget,
};

struct StatePackConsistencyResult {
    bool          ok            = true;
    RenderPassId  offendingPass = RenderPassId::None;
    StatePackAxis axis          = StatePackAxis::None;
};

inline StatePackConsistencyResult validatePassRenderStateConsistency() {
    StatePackConsistencyResult res;
    for (int i = 0; i < kPassRenderStateCount; ++i) {
        const RenderStateDesc& row = kPassRenderState[i];
        const RenderPassId pid = row.id;

        // --- fboTarget: compare against declaredFboTarget() ---
        {
            const RenderResourceId declared = row.fboTarget;
            const RenderResourceId ledger   = declaredFboTarget(pid);
            // Skip if either side is Unknown (undeclared or not in sparse table).
            if (declared != RenderResourceId::Unknown && ledger != RenderResourceId::Unknown) {
                if (declared != ledger) {
                    res.ok = false; res.offendingPass = pid; res.axis = StatePackAxis::FboTarget;
                    return res;
                }
            }
        }

        // --- ambient axes: compare against findAmbient() ---
        const AmbientContract* amb = findAmbient(pid);
        if (amb == nullptr) continue; // pass has no ambient row; Inherit on all sides is fine

        // colorMask
        {
            const ColorMaskState rowVal = row.colorMask;
            const ColorMaskState ambVal = amb->colorMaskOnEntry;
            if (rowVal != ColorMaskState::Inherit && ambVal != ColorMaskState::Inherit) {
                if (rowVal != ambVal) {
                    res.ok = false; res.offendingPass = pid; res.axis = StatePackAxis::ColorMask;
                    return res;
                }
            }
        }
        // depthWrite
        {
            const DepthWriteState rowVal = row.depthWrite;
            const DepthWriteState ambVal = amb->depthWrite;
            if (rowVal != DepthWriteState::Inherit && ambVal != DepthWriteState::Inherit) {
                if (rowVal != ambVal) {
                    res.ok = false; res.offendingPass = pid; res.axis = StatePackAxis::DepthWrite;
                    return res;
                }
            }
        }
        // depthFunc
        {
            const DepthFuncState rowVal = row.depthFunc;
            const DepthFuncState ambVal = amb->depthFunc;
            if (rowVal != DepthFuncState::Inherit && ambVal != DepthFuncState::Inherit) {
                if (rowVal != ambVal) {
                    res.ok = false; res.offendingPass = pid; res.axis = StatePackAxis::DepthFunc;
                    return res;
                }
            }
        }
        // viewport
        {
            const ViewportKind rowVal = row.viewport;
            const ViewportKind ambVal = amb->viewport;
            if (rowVal != ViewportKind::Inherit && ambVal != ViewportKind::Inherit) {
                if (rowVal != ambVal) {
                    res.ok = false; res.offendingPass = pid; res.axis = StatePackAxis::Viewport;
                    return res;
                }
            }
        }
    }
    return res;  // res.ok == true
}

}} // namespace RenderCore::framegraph
