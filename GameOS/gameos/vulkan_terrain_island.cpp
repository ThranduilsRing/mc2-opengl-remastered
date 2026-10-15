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

// VK-NOINTEROP-STRICT-REVIEW (H2b): record a PER-FRAME render-path GL depth readback.
// The terrain island's per-frame shadow fallback (copy_shadow_slot for the dynamic
// single-map slot) does a glGetTexImage when the VK direct producer declined -- exactly
// the fallback that
// caused the 126 ms/frame CSM readback (killed by direct-chain in 62707331). Before this
// fix the g_forbidden.glGetTexImage counter was DECLARED and printed but NEVER
// incremented, so the run()-scope f0b/f0c snapshot check and the health-line counter were
// always 0 (a realized false-pass). Copy-ONCE asset-feed imports (import_colormap /
// import_cement / import_matnormal, each guarded to fire once at
// map/mission load) must NOT call this -- they are the allowed GL-dependent feed layer
// documented in .claude/METAL-GATE-AUDIT-2.md. This bumps the counter the health line
// asserts == 0 AND emits a one-time PERFRAME_READBACK trip-wire the hardened checker greps.
// True under MC2_VK_NO_GL_INTEROP_STRICT: a per-frame render-path GL readback must NEVER
// fire (the whole point of the strict proof). The shadow direct-chain producer covers the
// steady state; on the ~2 warmup frames before it is ready, the per-frame fallback would
// otherwise do a GL depth readback. Under strict we fail-soft (skip the readback -> that
// frame's terrain is unshadowed via the legacy path) instead of violating the invariant.
bool terrainNoInteropStrict() {
    static const int s = mc2::gate::get<"MC2_VK_NO_GL_INTEROP_STRICT">();
    return s != 0;
}

void terrainPerFrameReadback(std::string_view fn) {
    g_forbidden.glGetTexImage++;
    static bool s_first = true;
    if (s_first) {
        s_first = false;
        vlog("[VK_TERRAIN_PERFRAME_READBACK v1] fn=%s (forbidden per-frame GL depth "
             "readback in render path -- direct-chain declined this frame)", std::string(fn).c_str());
    }
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
        state = mc2::gate::get<"MC2_VK_TERRAIN_PERF">();
    }
    return state == 1;
}

// Gate: MC2_VK_TERRAIN_TIER1==1 AND MC2_VK_SHARED_SCENE_DEPTH==1 (the shared depth
// the Tier-1 pass depth-tests against). Mirrors propColorTier1Requested().
bool terrainTier1Requested() {
    static int state = -1;
    if (state == -1) {
        // Umbrella member: MC2_VK_SHARED_GBUFFER=1 arms it.
        state = mc2vkgate::umbrellaOn() ? 1 : 0;   // tier-1 island AND shared scene depth
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
        state = terrainTier1Requested() ? 1 : 0;
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

// VK-TERRAIN-VISUALDISPLACE-FAR-1 (rung 5): far-band visual-displace mode-2 Z-swap
// sub-gate. Requires ALL_LODS (mode-2 only exists on lodStep>1 bands). An explicit
// opt-in (an Exact1 row: only "1" arms it; unset -> OFF regardless of the
// umbrella, unlike SKIRTS which is now an umbrella member) so the oracle can isolate "VK bands undisplaced (GL owns displaced)" from
// "VK bands displaced" -- if the Z-swap diverges, flip this OFF alone and keep rung 3/4.
// VK-FIELD-ENV-CONTENT-STACK-ARM-1 (2026-07-11): DELIBERATELY stays an explicit opt-in
// (NOT an umbrella family member, unlike ALL_LODS/SKIRTS). It is a far-band visual
// ENHANCEMENT, not base content -- when OFF the coarse bands still render fully (via
// ALL_LODS) merely undisplaced, so nothing VANISHES (the layer's "VK fallback" is the
// undisplaced-but-present VK band). It also depends on visual_height_4x.r32 bakes the
// 0.4c field lane does not ship, so arming it by default would risk a field regression.
bool terrainVisualDisplaceFarRequested() {
    static int state = -1;
    if (state == -1) {
        state = (terrainAllLodsRequested() && mc2::gate::get<"MC2_VK_TERRAIN_VISUALDISPLACE_FAR">()) ? 1 : 0;
    }
    return state == 1;
}

std::string spv_dir() {
    std::string d = mc2::gate::str<"MC2_VULKAN_SPV_DIR">();   // row default "shaders/vulkan" (also for an empty value)
    if (!d.empty() && d.back() != '/' && d.back() != '\\') d += '/';
    return d;
}

mc2vk::OwnedShaderModule load_spv(VkDevice device, std::string_view path) {
    return mc2vk::OwnedShaderModule(device, vk_island_util::loadSpv(device, path, vlog));
}

std::unique_ptr<mc2vk::IslandState>& islandSlot() {
    return mc2vk::getOrCreateEngineVulkanContext().islandSlot(mc2vk::IslandSlot::Terrain);
}
TerrainIsland* parkedIsland() { return static_cast<TerrainIsland*>(islandSlot().get()); }

// VK-UMBRELLA-DIRECT-CHAIN-DEFAULT-1 item-2 (boot-level native-or-decline):
// distinguish a PERMANENT init failure (spv/pipeline/renderpass/resource creation --
// won't recover this session) from a TRANSIENT one (engine VK context / shared GBuffer
// not born YET at boot -- recovers a few frames later, keep the item-9 fail-hard-with-
// retry). g_terrainInitReachedBuild flips to 1 once init passes the context/GBuffer
// readiness gates and starts creating permanent VK objects; if init then fails, the
// island is declined for the SESSION (one receipt line, GL owns terrain from frame 0)
// instead of re-newing + re-init'ing the island every frame.
bool g_terrainSessionDeclined  = false;
int  g_terrainInitReachedBuild = 0;

} // namespace

#endif // MC2_VULKAN_ISLAND
