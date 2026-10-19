// RenderCore/RenderPassContract.h
//
// Header-only DESCRIPTIVE registry of major render-pass lanes.
//
// Mirrors the table-of-facts style of RendererFeatureRegistry.h:
//   - feature registry = "what env-gated features exist"
//   - pipeline registry = "what GL state combos exist for a single draw"
//   - PASS contract (this file) = "what logical pass lanes exist, who owns them,
//     and how closed-up each one is on the snapshot / ViewUniforms / PipelineDesc
//     migration axes"
//
// NOT a scheduler. NOT a render graph. NO execute() callbacks. NO dispatch
// routing. The imperative frame loop continues to call each pass-owner's draw
// functions directly. This registry exists for inspection, audit, and
// migration tracking only.
//
// Firewall: header-only, no GL includes, no game-side includes. Pure POD +
// constexpr.
//
// Adding a pass:
//   1. Append a new RenderPassId enum value BEFORE _SentinelLast (never
//      renumber). The sentinel auto-tracks the count -- no hand-set COUNT
//      to forget to update.
//   2. Append a kRenderPassContracts[] entry with the same id, in the same
//      order. The static_assert at the bottom enforces parity.
//   3. Fill all fields with current shipped state -- DO NOT aspirational-flag.
//      If PipelineDesc is not yet registered for the pass, write false.

#pragma once

#include <cstdint>
#include "RenderResourceRegistry.h"

namespace RenderCore {

// ---------------------------------------------------------------------------
// Pass ids
// ---------------------------------------------------------------------------
// Values are stable -- never renumber, only append before _SentinelLast.
//
// Sentinel-after-last pattern: the count is derived from _SentinelLast's
// position, so appending a new pass id cannot silently desync a hand-set
// COUNT. The static_assert below still enforces that kRenderPassContracts[]
// length matches; this enum change closes the orthogonal "enum-value drift"
// hole.

enum class RenderPassId : uint32_t {
    None             = 0,   // null / dependsOn-terminator; never a contract row
    StaticPropOpaque = 1,
    Terrain          = 2,
    MechOpaque       = 3,
    Shadow           = 4,
    VFX              = 5,
    Water            = 6,
    PostProcess      = 7,
    VegetationCards  = 8,
    TerrainDecal     = 9,
    TerrainOverlay   = 10,
    UI               = 11,
    // KEEP _SentinelLast AT THE END. New pass ids must be added BEFORE it.
    _SentinelLast,
};

// Derived count of real (non-sentinel) pass ids. Pass ids start at 1 (None=0 is
// the null terminator and is NOT counted), so subtract 1 from the sentinel's
// underlying value.
constexpr uint32_t kRenderPassIdCount =
    static_cast<uint32_t>(RenderPassId::_SentinelLast) - 1u;

// ---------------------------------------------------------------------------
// Barrier kinds
// ---------------------------------------------------------------------------
// Bit flags for GL barriers needed after a pass's writes become visible to consumers.
// v0: metadata only -- runtime barrier calls are a future slice.
enum class BarrierKind : uint32_t {
    None          = 0,
    TextureFetch  = 1u << 0,  // GL_TEXTURE_FETCH_BARRIER_BIT
    ShaderStorage = 1u << 1,  // GL_SHADER_STORAGE_BARRIER_BIT
    ImageAccess   = 1u << 2,  // GL_SHADER_IMAGE_ACCESS_BARRIER_BIT
    Command       = 1u << 3,  // GL_COMMAND_BARRIER_BIT
    Framebuffer   = 1u << 4,  // GL_FRAMEBUFFER_BARRIER_BIT
};
constexpr inline BarrierKind operator|(BarrierKind a, BarrierKind b) {
    return static_cast<BarrierKind>(
        static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

// ---------------------------------------------------------------------------
// Vulkan-shaped attachment metadata (descriptive)
// ---------------------------------------------------------------------------
// v0: descriptive only -- no runtime load/store or layout-transition behaviour.
// Models the future RHI render-pass attachment description so a later Vulkan
// port has the per-pass intent already recorded.
enum class LoadOp  : uint8_t { Load = 0, Clear, DontCare };
enum class StoreOp : uint8_t { Store = 0, DontCare };
enum class ImageLayout : uint8_t {
    Undefined = 0, ColorAttachment, DepthStencilAttachment, ShaderReadOnly, Present
};

// ---------------------------------------------------------------------------
// Contract entry (descriptive)
// ---------------------------------------------------------------------------

struct RenderPassContract {
    RenderPassId id;
    const char*  name;                      // human-readable
    const char*  ownerSubsystem;            // e.g. "GpuStaticPropBatcher"
    bool         viewUniformsBound;         // consumes binding=3 ViewUniforms UBO
    bool         pipelineDescRegistered;    // routes through PipelineDesc/Registry
    bool         snapshotRowAuthoritative;  // RenderSnapshot is the authority
    const char*  inspectorSectionId;        // ImGui CollapsingHeader label (incl. ##tag if any)
    const char*  killSwitchEnv;             // env var; nullptr if none
    const char*  notes;                     // optional one-liner

    // Resources this pass samples (reads). RenderResourceId::Unknown (=0) terminates.
    RenderResourceId reads[4]  = {
        RenderResourceId::Unknown,
        RenderResourceId::Unknown,
        RenderResourceId::Unknown,
        RenderResourceId::Unknown
    };

    // Resources this pass writes. RenderResourceId::Unknown (=0) terminates.
    RenderResourceId writes[4] = {
        RenderResourceId::Unknown,
        RenderResourceId::Unknown,
        RenderResourceId::Unknown,
        RenderResourceId::Unknown
    };

    // GL barrier needed after this pass's writes, before consumers can read.
    // v0: informational only; runtime enforcement is a future slice.
    BarrierKind barrierAfter = BarrierKind::None;

    // v0 Vulkan render-pass metadata (descriptive; precise per-pass values are an
    // incremental follow-up to be filled from a RenderDoc capture). Defaults model
    // the common "load existing attachment, store result" opaque case.
    LoadOp      colorLoadOp      = LoadOp::Load;
    StoreOp     colorStoreOp     = StoreOp::Store;
    LoadOp      depthLoadOp      = LoadOp::Load;
    StoreOp     depthStoreOp     = StoreOp::Store;
    ImageLayout colorFinalLayout = ImageLayout::ColorAttachment;
    ImageLayout depthFinalLayout = ImageLayout::DepthStencilAttachment;
};

// ---------------------------------------------------------------------------
// Pass contract table
// ---------------------------------------------------------------------------
// Values reflect SHIPPED state at branch tip 1d7b9ea6. Update when a pass
// flips a closure axis (e.g. when terrain ViewUniforms ships, set
// viewUniformsBound=true here in the same slice).
//
// CONTRIBUTOR NOTE:
//   When a closure axis flips for an existing pass (e.g. terrain begins
//   consuming ViewUniforms, a pass gains a kill-switch env var, or a pass
//   migrates from live state to snapshot-authoritative dispatch), update
//   the corresponding row in kRenderPassContracts in the SAME commit that
//   makes the change. Stale booleans here will silently mis-report closure
//   state in the editor inspector and in docs/engine-closure-audit.md.
//   The static_assert below catches array-length drift but NOT field-value
//   staleness -- that is on you.

static constexpr RenderPassContract kRenderPassContracts[] = {
    {
        RenderPassId::StaticPropOpaque,
        "StaticPropOpaque",
        "GpuStaticPropBatcher",
        /*viewUniformsBound*/        true,
        /*pipelineDescRegistered*/   true,
        /*snapshotRowAuthoritative*/ true,
        "StaticProp",
        nullptr,
        "Reference path: snapshot-owned v6 DrawPacket+meta dispatch default-on (STATIC-PROP-V3-FLIP 2a88a5a8). "
        "POSTPROCESS-SCENEOBJECTID-RESOURCE-1: SceneObjectId write is conditional on RenderWorld::IsObjectIdBufferEnabled() "
        "(static_prop.frag layout location=2 out uint v_objectId; only emitted when MRT 3-entry draw-buffer set is active).",
        /* reads[4]    */ { },
        /* writes[4]   */ { RenderResourceId::MainColor, RenderResourceId::MainDepth, RenderResourceId::MainNormal, RenderResourceId::SceneObjectId },
        /* barrierAfter */ BarrierKind::Framebuffer
    },
    {
        RenderPassId::Terrain,
        "Terrain",
        "TerrainPatchStream",
        /*viewUniformsBound*/        false,
        /*pipelineDescRegistered*/   true,  // FRAMEGRAPH-STATEPACK-SKELETON-1: routed via applyPipeline(TerrainSolid)
                                            // in gos_terrain_lod_chunk.cpp:717 (LODChunk) and
                                            // gameos_graphics.cpp:4041 (PatchStreamThin). Was false — stale.
        /*snapshotRowAuthoritative*/ false,
        "Terrain Pass##tp",
        nullptr,
        "TerrainPassFacts row landed in RenderSnapshot at 1d7b9ea6 as passive recorder; not yet authoritative.",
        /* reads[4]    */ { },
        /* writes[4]   */ { RenderResourceId::MainColor, RenderResourceId::MainDepth, RenderResourceId::MainNormal },
        /* barrierAfter */ BarrierKind::Framebuffer
    },
    {
        RenderPassId::MechOpaque,
        "MechOpaque",
        "GpuMechBatcher",
        /*viewUniformsBound*/        true,
        /*pipelineDescRegistered*/   true,
        /*snapshotRowAuthoritative*/ true,
        "Mech",
        "MC2_SNAPSHOT_MECH_EXTRACT",
        "Wired through PipelineId::MechOpaque (MECH-PIPELINEDESC-1, applyPipeline) "
        "+ ViewUniforms UBO consumer default-on (MECH-VIEWUNIFORMS). "
        "POSTPROCESS-SCENEOBJECTID-RESOURCE-1: SceneObjectId write is conditional on RenderWorld::IsObjectIdBufferEnabled() "
        "(mech.frag layout location=2 out uint v_objectId; only emitted when MRT 3-entry draw-buffer set is active).",
        /* reads[4]    */ { },
        /* writes[4]   */ { RenderResourceId::MainColor, RenderResourceId::MainDepth, RenderResourceId::MainNormal, RenderResourceId::SceneObjectId },
        /* barrierAfter */ BarrierKind::Framebuffer
    },
    {
        RenderPassId::Shadow,
        "Shadow",
        "gosPostProcess + per-lane shadow programs",
        /*viewUniformsBound*/        false,
        /*pipelineDescRegistered*/   false,  // FRAMEGRAPH-STATEPACK-SKELETON-1: three PipelineIds
                                             // (ShadowTerrain/ShadowMech/ShadowStaticProp) are
                                             // DESCRIPTIVE ONLY per PipelineRegistry.h comment;
                                             // the pass itself does NOT have a single registered
                                             // pipeline routed via applyPipeline. Left false.
        /*snapshotRowAuthoritative*/ false,
        "Shadow Pass##sp",
        nullptr,
        "Three shadow lanes (terrain/mech/static-prop); counters live-read from inspectors.",
        /* reads[4]    */ {},
        /* writes[4]   */ { },
        /* barrierAfter */ BarrierKind::TextureFetch,
        // Depth-only pass: clears its depth target, and the resulting depth map
        // is SAMPLED by later geometry/post passes -> ShaderReadOnly final layout.
        /* colorLoadOp      */ LoadOp::Load,
        /* colorStoreOp     */ StoreOp::Store,
        /* depthLoadOp      */ LoadOp::Clear,
        /* depthStoreOp     */ StoreOp::Store,
        /* colorFinalLayout */ ImageLayout::ColorAttachment,
        /* depthFinalLayout */ ImageLayout::ShaderReadOnly
    },
    {
        RenderPassId::VFX,
        "VFX",
        "mc2::particles::Batcher",
        /*viewUniformsBound*/        false,
        /*pipelineDescRegistered*/   true,  // FRAMEGRAPH-STATEPACK-SKELETON-1: multiple sub-pipelines
                                            // (VfxBillboard/Tube/MeshAlpha/Additive) all route through
                                            // applyPipeline (gos_particle_bridge.cpp:847/1263,
                                            // gos_vfx_mesh_bridge.cpp:315). Was false — stale.
                                            // Note: no single PipelineId (StatePack uses Invalid).
        /*snapshotRowAuthoritative*/ false,
        "VFX Pass##vfx",
        nullptr,
        "Object-ID PROHIBITED. GpuTrailKind {None, MissileSmoke, PpcBolt}. "
        "POSTPROCESS-SCENEDEPTHCOPY-RESOURCE-1: produces SceneDepthCopy "
        "(copySceneDepthForParticles in gos_particle_bridge.cpp:1068, VFX flush path); "
        "consumed downstream by PostProcess BoxDecals (SUBGRAPH-2) for soft-depth reject. "
        "REGISTRY-SCENECOLORCOPY-PRODUCER-1: also produces SceneColorCopy "
        "(copySceneColorForVfx in gos_particle_bridge.cpp:1097, same VFX flush window, "
        "gated MC2_VFX_SCENECOLOR_GRAB); consumed by PostprocessComputeBlur (SUBGRAPH-2, "
        "isCompute, default-OFF). Closes the id-without-producer gap.",
        /* reads[4]    */ { RenderResourceId::MainDepth },
        /* writes[4]   */ { RenderResourceId::MainColor, RenderResourceId::SceneDepthCopy, RenderResourceId::SceneColorCopy },
        /* barrierAfter */ BarrierKind::None
    },
    {
        RenderPassId::Water,
        "Water",
        "quad.cpp / renderWaterFastPath",
        /*viewUniformsBound*/        false,
        /*pipelineDescRegistered*/   false, // native Vulkan water; GL pipeline retired
                                            // routes through applyPipeline(WaterArmed) in
                                            // gameos_graphics.cpp:3287. Was false — stale.
        /*snapshotRowAuthoritative*/ false,
        "Water Pass##water",
        nullptr,
        "Intentional projected path (Bucket B1); legacy quad water + optional MDI fastpath.",
        /* reads[4]    */ { RenderResourceId::MainDepth },
        /* writes[4]   */ { RenderResourceId::MainColor },
        /* barrierAfter */ BarrierKind::Framebuffer
    },
    {
        RenderPassId::PostProcess,
        "PostProcess",
        "gos_postprocess",
        /*viewUniformsBound*/        false,
        /*pipelineDescRegistered*/   true,  // FRAMEGRAPH-STATEPACK-SKELETON-1: multiple sub-pipelines
                                            // all route applyPipeline (PostProcessComposite at :2542,
                                            // PostProcessScreenShadow :2050, PostProcessCloudShadow :2192,
                                            // PostProcessShoreline :2248, PostProcessSsaoApply :1988,
                                            // PostProcessEdgeFog :2302, PostProcessFogOob :2358 in
                                            // gos_postprocess.cpp). Was false — stale.
                                            // Note: no single PipelineId (StatePack uses Invalid).
        /*snapshotRowAuthoritative*/ false,
        "PostProcess##pp",
        nullptr,
        "Composite chain after gos_RendererEndFrame: HZB/SSAO/screen-shadow/shoreline/godrays/composite. "
        "Samples GBuffer normal (sceneNormalTex_ / MainNormal) for SSAO, Shoreline, ScreenShadow, BoxDecals sub-stages.",
        /* reads[4]    */ { RenderResourceId::MainColor, RenderResourceId::MainDepth, RenderResourceId::MainNormal },
        /* writes[4]   */ { RenderResourceId::MainColor },
        /* barrierAfter */ BarrierKind::Framebuffer,
        // Final on-screen pass -> color attachment ends in Present layout.
        /* colorLoadOp      */ LoadOp::Load,
        /* colorStoreOp     */ StoreOp::Store,
        /* depthLoadOp      */ LoadOp::Load,
        /* depthStoreOp     */ StoreOp::Store,
        /* colorFinalLayout */ ImageLayout::Present,
        /* depthFinalLayout */ ImageLayout::DepthStencilAttachment
    },
    {
        RenderPassId::VegetationCards,
        "VegetationCards",
        "VegetationAdapter / gos_vegetation",
        /*viewUniformsBound*/        false,
        /*pipelineDescRegistered*/   false,
        /*snapshotRowAuthoritative*/ false,
        "Vegetation##veg",
        "MC2_VEGETATION_CARDS",
        "Instanced crossed-quad billboards, alpha-discard; post-renderLists.",
        /* reads[4]    */ { RenderResourceId::MainDepth, },
        /* writes[4]   */ { RenderResourceId::MainColor, RenderResourceId::MainDepth },
        /* barrierAfter */ BarrierKind::Framebuffer
    },
    {
        RenderPassId::TerrainDecal,
        "TerrainDecal",
        "craterManager / quad.cpp",
        /*viewUniformsBound*/        false,
        /*pipelineDescRegistered*/   true,  // FRAMEGRAPH-STATEPACK-SKELETON-1: routes applyPipeline(TerrainDecal)
                                            // at gameos_graphics.cpp:10005. Was false — stale.
        /*snapshotRowAuthoritative*/ false,
        "TerrainDecal##td",
        nullptr,
        "Craters, footprints, scorch; coplanar decals blended onto terrain.",
        /* reads[4]    */ { RenderResourceId::MainDepth },
        /* writes[4]   */ { RenderResourceId::MainColor },
        /* barrierAfter */ BarrierKind::Framebuffer
    },
    {
        RenderPassId::TerrainOverlay,
        "TerrainOverlay",
        "quad.cpp M2d producer",
        /*viewUniformsBound*/        false,
        /*pipelineDescRegistered*/   true,  // FRAMEGRAPH-STATEPACK-SKELETON-1: routes applyPipeline(TerrainOverlay)
                                            // at gameos_graphics.cpp:9810 (and again at 9930 for the
                                            // static-decal path within drawTerrainOverlays). Was false — stale.
        /*snapshotRowAuthoritative*/ false,
        "TerrainOverlay##to",
        nullptr,
        "Perimeter cement / transitions; mostly colormap-baked, residual overlay quads.",
        /* reads[4]    */ { RenderResourceId::MainDepth },
        /* writes[4]   */ { RenderResourceId::MainColor },
        /* barrierAfter */ BarrierKind::Framebuffer
    },
    {
        RenderPassId::UI,
        "UI",
        "GameOS 2D / HUD",
        /*viewUniformsBound*/        false,
        /*pipelineDescRegistered*/   false,
        /*snapshotRowAuthoritative*/ false,
        "UI##ui",
        nullptr,
        "HUD, text, menu; screen-space, drawn after scene before post-composite.",
        /* reads[4]    */ {},
        /* writes[4]   */ { RenderResourceId::MainColor },
        /* barrierAfter */ BarrierKind::None
    },
};

static constexpr int kRenderPassContractCount =
    sizeof(kRenderPassContracts) / sizeof(kRenderPassContracts[0]);

static_assert(
    kRenderPassContractCount == static_cast<int>(kRenderPassIdCount),
    "kRenderPassContracts length must match kRenderPassIdCount "
    "(append the new RenderPassContract row when you append a RenderPassId).");

// ---------------------------------------------------------------------------
// Logical dependency skeleton (RENDER-PASS-DAG-CONTRACT-1)
// ---------------------------------------------------------------------------
// kFramePassOrder encodes logical observation order rather than the draw timeline: the 2026-06-09 LOD-chunk terrain hoist moved
// the default terrain draw to gamecam.cpp:508 BEFORE renderLists(), so Terrain
// actually fires before StaticPropOpaque at runtime (even though it is listed after
// it here). This is a config-variable draw-site: LOD-chunk=Gamecam (pre-renderLists),
// the other three branches=RenderLists. The draw-site is modeled in
// TerrainSubPass::drawSite (terrain_subpass_contract.h), and the dry-run kernel
// suppresses the resulting apparent out-of-order event via knownEarlyDrawSite
// (frame_pass_trace.h markEntryKnownEarly / DryRunReport::knownEarlySuppressed).
// Do NOT reorder this array to match the LOD-chunk runtime sequence — that would
// break the other three non-default branches whose draw-site IS renderLists.
//
// Native shadow image barriers belong to the Vulkan island. Remaining resource
// edges are derived from reads[]/writes[] against this order
// (see render_pass_table_harness), not stored per-row, to avoid a second source
// of truth that can rot.
static constexpr RenderPassId kFramePassOrder[] = {
    RenderPassId::Shadow,
    RenderPassId::StaticPropOpaque, // GpuStaticPropBatcher::flush fires at renderLists preamble (~txmmgr:3250)
                                    // MECHOPAQUE-ORDER-FIX-2: Static flush PRECEDES Mech GPU draw
    RenderPassId::MechOpaque,       // noteRenderPass + GpuMechBatcher::flush fires after Static flush (~txmmgr:3271)
                                    // (MECHOPAQUE-NOTE-RELOCATE-1, SAME-ORDER-EXECUTOR-SLICE-2)
    RenderPassId::Terrain,          // LODChunk fires pre-renderLists (Gamecam site, see comment above)
    RenderPassId::TerrainOverlay,   // gos_DrawTerrainOverlays fires before gos_DrawDecals in renderLists
    RenderPassId::TerrainDecal,     // gos_DrawDecals follows overlays (txmmgr.cpp:3275/3311)
    RenderPassId::Water,
    RenderPassId::VegetationCards,
    RenderPassId::VFX,
    RenderPassId::UI,
    RenderPassId::PostProcess,
};
static constexpr int kFramePassOrderCount =
    sizeof(kFramePassOrder) / sizeof(kFramePassOrder[0]);
static_assert(kFramePassOrderCount == static_cast<int>(kRenderPassIdCount),
    "kFramePassOrder must list every real RenderPassId exactly once.");

} // namespace RenderCore
