// RenderCore/RenderRegionId.h
//
// RENDER-BACKEND-REGION-IFACE-1 (Layer-6 ENTRY). GL-FREE contract for a
// SELECTABLE render region routed through IRenderBackend. First (and only)
// region in this slice: PostprocessFog — the fused EdgeFog + OOB-fog composite
// that already runs as a gated Vulkan subgraph.
//
// HARD RULE (same as IRenderBackend.h): no GL / no Vulkan includes, no raw
// GLuint / VkImage in this file. The region CONTEXT is BACKEND-NEUTRAL — it
// names resources by RenderResourceId and carries params + dims only. Each
// backend impl translates RenderResourceId -> its concrete handle via an
// IResourceResolver (defined here, also GL-free: a resolver is a translation
// SERVICE, not a handle).
//
// The engine-facing selector is an ENUM, never a string. Strings appear only in
// logs/config (see region_impl_to_string()).

#pragma once

#include "RenderResourceRegistry.h"   // RenderResourceId (GL-free enum)

namespace RenderCore {

// The set of render regions that can be routed through IRenderBackend::runRegion.
// Grows one region at a time (same discipline as IRenderBackend's op list).
enum class RenderRegionId {
    PostprocessFog = 0,   // fused EdgeFog + OOB fog composite
    PostprocessSSAO = 1,  // half-res AO occlusion + multiplicative apply (VK-REGION-SSAO-1)
    PostprocessHzb = 2,   // reverse-Z Hi-Z depth-pyramid reduce chain (VK-REGION-HZB-1)
    ShadowDepth = 4,      // depth-only instanced shadow-caster producer (VK-REGION-SHADOW-1)
};

// Which backend implementation actually ran the region this frame. Surfaced in
// the health dump as region_impl and used to prove no double-apply.
enum class RenderRegionImpl {
    None        = 0,   // region did not run (gates/preconditions skipped it)
    GLInline    = 1,   // ran through the GL backend (the extracted GL fog wrapper)
    VulkanSubgraph = 2, // ran through the Vulkan subgraph
    FallbackGL  = 3,    // Vulkan requested but failed/unavailable -> GL wrapper ran
};

inline const char* region_id_to_string(RenderRegionId id) {
    switch (id) {
        case RenderRegionId::PostprocessFog:  return "PostprocessFog";
        case RenderRegionId::PostprocessSSAO: return "PostprocessSSAO";
        case RenderRegionId::PostprocessHzb:  return "PostprocessHzb";
        case RenderRegionId::ShadowDepth:      return "ShadowDepth";
    }
    return "Unknown";
}

inline const char* region_impl_to_string(RenderRegionImpl impl) {
    switch (impl) {
        case RenderRegionImpl::None:           return "None";
        case RenderRegionImpl::GLInline:       return "GLInline";
        case RenderRegionImpl::VulkanSubgraph: return "VulkanSubgraph";
        case RenderRegionImpl::FallbackGL:     return "FallbackGL";
    }
    return "Unknown";
}

// Backend-neutral fog parameters. Identical numeric content to the GL
// runEdgeFog()/runFogOob() uploads; NO handles. invViewProj is the same 16
// floats the GL path uploads (row-major, uploaded with GL_FALSE by the GL path).
struct FogParams {
    float invViewProj[16] = {0};
    float color[3]        = {0, 0, 0};
    // EdgeFog-only fields (ignored by OOB):
    float halfExtent      = 0.0f;
    float fogStart        = 0.0f;
    float fogHeight       = 0.0f;
    float fogMax          = 0.0f;
    float waterElevation  = 0.0f;
    // OOB-only fields (ignored by EdgeFog):
    float opacity         = 0.0f;
    float time            = 0.0f;
};

// BACKEND-NEUTRAL region context. color/depth name resources by id; the backend
// resolves them to concrete handles via `resolver` (never a raw handle here).
struct PostprocessFogRegionContext {
    RenderResourceId color = RenderResourceId::MainColor;
    RenderResourceId depth = RenderResourceId::MainDepth;
    int width  = 0;
    int height = 0;
    FogParams edgeFog;
    FogParams oobFog;
    // Translation service: RenderResourceId -> concrete backend handle. GL-free
    // interface (see below). Owned by the caller; must outlive the runRegion call.
    const struct IResourceResolver* resolver = nullptr;
};

// What runRegion reports back: which impl ran, why it fell back (if it did), and
// how many GL fog passes actually executed (equivalence-counter cross-check).
struct RegionOutput {
    RenderRegionImpl impl = RenderRegionImpl::None;
    const char* fallbackReason = "";   // "" when no fallback
    int glPassesRun = 0;               // GL fog passes executed (0 or 2)
    int vkDraws     = 0;               // Vulkan fog draws executed (0 or 2)
};

// GL-FREE resolver interface. A backend impl calls resolve() to turn a
// RenderResourceId into its concrete handle (a plain uint — GL name / any
// backend-native handle that fits 64 bits). The IMPLEMENTATION lives with the
// resource owner (gosPostProcess owns sceneColorTex_/sceneDepthTex_), so the
// constexpr registry keeps NO handles. Returns 0 for an unresolvable id.
struct IResourceResolver {
    virtual ~IResourceResolver() = default;
    virtual unsigned int resolve(RenderResourceId id) const = 0;
};

// Backend-neutral SSAO parameters. Identical numeric content to the GL
// runSSAO() uploads (ssaoProg_ occlusion pass + ssaoApplyProg_ apply pass);
// NO handles. invViewProj/viewProj are the same 16 floats the GL path
// uploads (row-major, GL_FALSE direct upload — same convention as fog).
struct SSAOParams {
    float invViewProj[16] = {0};
    float viewProj[16]    = {0};
    float screenWidth     = 0.0f;
    float screenHeight    = 0.0f;
    float aoRadius        = 0.0f;
    float aoBias          = 0.0f;
    float aoStrength      = 0.0f;
    float aoPower         = 0.0f;
    int   debugMode       = 0;
};

// BACKEND-NEUTRAL region context for the SSAO region (VK-REGION-SSAO-1).
// depth/normal/color/occlusion name resources by id; the backend resolves
// them to concrete handles via `resolver` (never a raw handle here).
// occlusion is the half-res intermediate (SsaoOcclusion) the region writes
// in pass 1 and reads back in pass 2 — a Vulkan impl keeps it as a
// Vulkan-owned intermediate image (no GL round-trip mid-region), matching
// the fog subgraph's "one Vulkan-owned intermediate" precedent.
struct PostprocessSSAORegionContext {
    RenderResourceId depth     = RenderResourceId::MainDepth;
    RenderResourceId normal    = RenderResourceId::MainNormal;
    RenderResourceId color     = RenderResourceId::MainColor;
    RenderResourceId occlusion = RenderResourceId::SsaoOcclusion;
    int width  = 0;   // full-res width  (screenWidth/Height in SSAOParams)
    int height = 0;   // full-res height
    int halfWidth  = 0;  // occlusion intermediate width  (ssaoW_)
    int halfHeight = 0;  // occlusion intermediate height (ssaoH_)
    SSAOParams params;
    // Translation service: RenderResourceId -> concrete backend handle. GL-free
    // interface (see below). Owned by the caller; must outlive the runRegion call.
    const struct IResourceResolver* resolver = nullptr;
};

// BACKEND-NEUTRAL region context for the HZB region (VK-REGION-HZB-1).
// depth/pyramid name resources by id (HzbPyramid is a MULTI-LEVEL resource --
// one ceil-ladder R32F texture per level in GL; the resolver returns the
// level-0 representative handle, and the backend impl owns the per-level
// mapping, same discipline as the SSAO region's member access). All the GL
// pass's uniforms (uSrcTexel per level, uReduce) are DERIVED from
// width/height/mipCount -- no extra params struct needed.
struct PostprocessHzbRegionContext {
    RenderResourceId depth   = RenderResourceId::MainDepth;
    RenderResourceId pyramid = RenderResourceId::HzbPyramid;
    int width    = 0;   // pyramid level-0 width  (hzbW_ == full-res scene width)
    int height   = 0;   // pyramid level-0 height (hzbH_)
    int mipCount = 0;   // ceil-ladder level count (hzbMipCount_)
    // Translation service: RenderResourceId -> concrete backend handle. GL-free
    // interface (see below). Owned by the caller; must outlive the runRegion call.
    const struct IResourceResolver* resolver = nullptr;
};

// Maximum cascade count the ShadowDepth region carries in a fixed inline array
// (keeps the context a POD -- no heap / no std::vector in the neutral contract).
// The GL CSM path (the ShadowCSM option) uses a small ladder; 4 covers it with slack.
static const int kMaxShadowCascades = 4;

// BACKEND-NEUTRAL region context for the ShadowDepth region (VK-REGION-SHADOW-1).
// This is the FIRST GEOMETRY region: it WRITES a shadow map (depth-only instanced
// producer) rather than READing one (comparesampler/csmarray proved the read).
//
// It names by RenderResourceId the per-frame INSTANCE ring (the source of the
// per-instance world transforms), the companion BONE/COLOR ring, and the MATERIAL
// table. It carries the cascade LIGHT-SPACE matrices (one per cascade), the cascade
// COUNT, and the shadow-map DIMENSIONS. NO VkImage / GLuint here -- the backend
// resolves ids -> concrete handles via `resolver`, and OWNS the depth target it
// renders into (the shadow map is backend-internal, like the fog subgraph's
// Vulkan-owned intermediate; it is NOT named in this neutral context).
//
// ★ Two geometry FAMILIES cast shadows (mech + static prop). Each family has its
// own instance ring + companion ring; a caller runs the region once per family
// (or a backend batches both), selecting rings by the ids below. bone == the
// mech GPU-skinning bone ring (id 40); color == the static-prop per-instance
// color ring (id 42). A given draw uses the pair matching `instance`.
struct ShadowDepthRegionContext {
    // Per-instance world transforms live here (STORAGE_BUFFER). MechInstanceRingSsbo
    // (39) for the mech family; StaticPropInstanceRingSsbo (41) for props.
    RenderResourceId instance = RenderResourceId::MechInstanceRingSsbo;
    // Companion ring: MechBoneRingSsbo (40, GPU-skinning bones) for the mech family;
    // StaticPropColorRingSsbo (42, per-instance color) for props. Meaning depends
    // on which family `instance` selects.
    RenderResourceId companion = RenderResourceId::MechBoneRingSsbo;
    // Material table (id 5 = mech MaterialGpuBuffer; id 28 = static-prop table). A
    // depth-only pass rarely samples material, but the id is carried so an
    // alpha-tested caster variant can bind it without widening the contract later.
    RenderResourceId material = RenderResourceId::MaterialGpuBuffer;

    // Cascade light-space matrices (view*proj per cascade), row-major, uploaded with
    // the SAME convention the GL shadow producer uses (GL_FALSE direct upload). Only
    // the first `cascadeCount` entries are meaningful.
    float lightSpace[kMaxShadowCascades][16] = {{0}};
    int   cascadeCount = 1;   // 1 == single shadow map; >1 == CSM 2D_ARRAY (one layer/cascade)

    // Shadow-map dimensions (square in the GL path; kept as w/h for generality).
    int shadowWidth  = 0;
    int shadowHeight = 0;

    // Dynamic-offset selector into the instance ring: byte offset =
    // slot * instanceCapacity * instanceStride (inventory hard-case #2, the
    // 3-frames-in-flight ring). The backend binds STORAGE_BUFFER_DYNAMIC with this.
    int instanceSlot     = 0;   // frame-in-flight ring slot
    int instanceCapacity = 0;   // max instances per slot (element count)
    int instanceStride   = 0;   // per-instance stride in bytes
    int instanceCount    = 0;   // instances to draw this invocation

    // Translation service: RenderResourceId -> concrete backend handle. GL-free
    // interface (see above). Owned by the caller; must outlive the runRegion call.
    const struct IResourceResolver* resolver = nullptr;
};

}  // namespace RenderCore
