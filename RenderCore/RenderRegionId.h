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

}  // namespace RenderCore
