// vulkan_terrain_island.cpp — VK-P10 TERRAIN TIER-1: LIVE LOD0 terrain opaque prefix
// co-written IN-PLACE into the SHARED scene GBuffer.
// =============================================================================
// The terrain analogue of the prop Tier-1 island (vulkan_color_prop_island.cpp,
// tier1 path). It REUSES that proven plumbing VERBATIM:
//   - Borrows the shared engine VulkanContext (no second device).
//   - Fetches the SAME shared exportable color(MRT0)+normal(MRT1)+depth images the
//     GL scene renders terrain into (mc2SharedSceneColorGetResource /
//     mc2SharedSceneDepthGetResource), builds a LOAD-preserve renderpass +
//     framebuffer over them (2 color LOAD + shared depth LOAD, no clears).
//   - Pipeline state COPIED from the GL terrain contract (gos_terrain_lod_chunk.cpp
//     :869-884), which is byte-identical to the prop tier1 pipeline: cullMode=NONE
//     (terrainMVP bakes the GL-NDC X-flip -> double-sided), frontFace=CCW,
//     depthTestEnable=TRUE, depthWriteEnable=TRUE, depthCompareOp=GREATER_OR_EQUAL
//     (reverse-Z), NO blend on either color attachment (opaque in-place overwrite).
//
// TERRAIN differences from props:
//   - Geometry: NO shared-prop VBO. The island REBUILDS the LOD0 patch grid itself
//     (a deterministic int16 (lx,ly) grid + uint16 indices from quadCountX/Y — the
//     same shape getOrBuildPatch() builds for lodStep==1), cached per (qcX,qcY).
//   - Feed: the height SSBO (GL binding 23) is snapshot-copied ONCE PER CHANGE into
//     a VK-owned storage buffer via glGetBufferSubData (map-load pattern, coherent
//     snapshot — NOT a per-frame handoff readback; endorsed by the recon). CPU-side
//     LOD0 chunk selection stays on GL/CPU (the feed hands over the LOD0 patch list).
//   - GBuffer1 = vec4(0.5,0.5,1.0,1.0), a=1.0 (terrain shadow-class; props use 0.25).
//   - Per-patch placement (blockOrigin/quadCount) is fed via a DYNAMIC UBO offset
//     (one UBO slot per LOD0 patch), replacing the GL per-block u_blockOrigin*
//     uniforms.
//
// SCOPE = LOD0 (lodStep==1) MAIN triangle draw ONLY. Skirts, coarser bands, stitch,
// visual-displace are DEFERRED. The GL LOD-chunk path draws everything else.
//
// The whole TU compiles to nothing unless MC2_VULKAN_ISLAND is defined.
// =============================================================================

#include "vulkan_terrain_island_internal.h"

#ifdef MC2_VULKAN_ISLAND



namespace vk_terrain_detail {

TerrainIslandHealth g_health;

InteropForbidden g_forbidden;

void setFallback(std::string_view reason) {
    const size_t n = (std::min)(reason.size(), sizeof(g_health.fallbackReason) - 1);
    std::memcpy(g_health.fallbackReason, reason.data(), n);
    g_health.fallbackReason[n] = '\0';
}

void vlog(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vk_island_util::vlogv("[VK_TERRAIN_ISLAND] ", fmt, ap);
    va_end(ap);
}

// VK-TERRAIN-FENCE-DEFER-1: defer the
// island's retire wait to the parity slot's next vkImageResourceBeginFrame instead
// of waiting the submit fence inline. Default-ON (the per-parity UBO staging +
// per-parity descriptor sets make it correct by construction);
// MC2_VK_TERRAIN_FENCE_DEFER=0 is the killswitch restoring the inline wait — the
// per-parity staging holds either way (superset-safe).
bool terrainFenceDeferEnabled() {
    static int state = -1;
    if (state == -1)
        state = mc2::gate::get<"MC2_VK_TERRAIN_FENCE_DEFER">() ? 1 : 0;
    return state == 1;
}

// MC2_VK_TERRAIN_PERF — per-frame microsecond breakdown receipt of the VK terrain
// island ([VK_TERRAIN_PERF] line: stage/record-submit/fence-wait/total). Same
// diagnostic class + shape as MC2_VK_WATER_PERF (additive stderr trace, default OFF).
bool terrainPerfGateRequested() {
    static int state = -1;
    if (state == -1) {
        state = false;
    }
    return state == 1;
}

float g_prefixClearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};

// VK-TERRAIN-ALL-LODS sub-slice (rung 3): extend VK terrain ownership beyond LOD0
// (lodStep==1) to all coarse bands (lodStep>1), non-displaced, non-stitched only.
// Requires TIER1 (no coarse bands without the LOD0 island already live). Skirts and
// skirts remain GL-owned until the rung-4 SKIRTS sub-gate. NOTE (VK-TERRAIN-EDGE-
// STITCH-1, commit 0aae0337): edge-stitch is NO LONGER GL-owned -- the capture-
// predicate exclusion was removed and the corner-pin (+ mode-2 re-stitch) vertex
// math is ported into terrain_tier1.vert / _skirt.vert with edgeStitch/quadCountX/Y
// carried through the patch UBO tail. Stitched patches are VK-captured; the GL-
// fallback audit's edge_stitch counter is 0 by construction, captured_stitch>0.
//
// VK-FIELD-ENV-CONTENT-STACK-ARM-1 (2026-07-11): coarse LOD bands are BASE terrain
// content, not an enhancement. Post-VK-RUNG8-ITEM8 the native VK composite is the sole
// present-image producer under the umbrella and there is NO per-layer GL fallback: an
// unarmed content layer VANISHES (the field bug -- only LOD0/1 terrain rendered, coarse
// bands GL-drawn into the discarded GL backbuffer). So ALL_LODS is now an umbrella FAMILY
// member: MC2_VK_SHARED_GBUFFER=1 arms it. The strict proof armed ALL_LODS=1 and passed
// its parity check, so this is the
// proven-complete scene config. Still gated on TIER1 (no coarse bands without LOD0).
bool terrainAllLodsRequested() {
    static int state = -1;
    if (state == -1) {
        state = 1;
    }
    return state == 1;
}

// VK-TERRAIN-SKIRTS sub-slice (rung 4): extend VK terrain ownership to skirt/edge-
// stitch geometry for VK-owned patches. Requires ALL_LODS (skirts only matter once
// coarse bands are VK-owned -- LOD0-only skirts were already excluded from GL draw
// by the TIER1 capture, see the "LOD0 skirts excluded from probe" comment at the
// capture site). Independent explicit opt-in (own env var, NOT chained silently onto
// ALL_LODS) so the oracle can isolate "VK bands, GL skirts" (today's shipped state)
// from "VK bands, VK skirts" (this rung) -- if skirts crack, flip this OFF alone and
// keep the rung-3 band win. VK-FIELD-ENV-CONTENT-STACK-ARM-1: now an umbrella FAMILY
// member like ALL_LODS -- without VK skirts the coarse-band LOD seams crack
// open (visible gaps), and there is no per-layer GL fallback post-item-8. The strict
// proof armed SKIRTS=1 and passed. Still gated on ALL_LODS.
bool terrainSkirtsRequested() {
    static int state = -1;
    if (state == -1) {
        state = terrainAllLodsRequested() ? 1 : 0;
    }
    return state == 1;
}


std::string spv_dir() { return vk_island_util::spvDir(); }

mc2vk::OwnedShaderModule load_spv(VkDevice device, std::string_view path) {
    return mc2vk::OwnedShaderModule(device, vk_island_util::loadSpv(device, path, vlog));
}

std::unique_ptr<mc2vk::IslandState>& islandSlot() {
    return mc2vk::getOrCreateEngineVulkanContext().islandSlot(mc2vk::IslandSlot::Terrain);
}
TerrainIsland* parkedIsland() { return static_cast<TerrainIsland*>(islandSlot().get()); }

// g_terrainInitReachedBuild flips to 1 once init passes the context/GBuffer readiness gates and starts creating VK objects: a failure
// before that retries next frame, a failure after it is fatal (vk_island_fail_policy.h).
int  g_terrainInitReachedBuild = 0;

} // namespace

#endif // MC2_VULKAN_ISLAND
