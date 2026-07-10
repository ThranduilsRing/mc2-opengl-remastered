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

#ifdef MC2_VULKAN_ISLAND

#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <volk.h>
#include <GL/glew.h>

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include <vk_mem_alloc.h>

#include "vk_image_resource.h"
#include "gl_interop_view.h"
#include "vulkan_context.h"
#include "vulkan_terrain_island.h"
#include "vk_shared_gbuffer_gate.h"     // VK-P14 GATE-CONSOLIDATION-1 umbrella resolver
#define MC2_VULKAN_SHARED_SCENE_COLOR_IMPL_ISLAND
#include "vulkan_shared_scene_color.h"
#define MC2_VULKAN_SHARED_SCENE_DEPTH_IMPL_ISLAND
#include "vulkan_shared_scene_depth.h"
#include "vulkan_shadow_island_live.h"  // VK-SHADOW-DIRECT-CONSUME-1: direct dyn-shadow view accessors

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cstdint>
#include <vector>
#include <string>
#include <chrono>
#include <unordered_map>
#include <tracy/Tracy.hpp>   // VK-FLIP-FIELD-REPORT-1: coarse per-region frame zones

#ifndef GL_COMPRESSED_RGBA_BPTC_UNORM
#define GL_COMPRESSED_RGBA_BPTC_UNORM        0x8E8C
#endif
#ifndef GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM
#define GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM  0x8E8D
#endif

namespace {

// ---------------------------------------------------------------------------
// Health snapshot (mirrors the prop island's).
// ---------------------------------------------------------------------------
struct TerrainIslandHealth {
    int   vulkanAvailable = 0;
    int   buildEnabled    = 1;
    char  deviceName[256] = {0};
    unsigned long attempted   = 0;
    unsigned long usedVulkan  = 0;
    char  fallbackReason[256] = {0};
    unsigned long vkDraws     = 0;
    int   interopActive  = 0;
    int   vk2glSemActive = 0;
    unsigned long patchRecords    = 0;   // LOD0 patches co-written this frame
    unsigned long heightSnapshots = 0;   // height-SSBO snapshot copies (bumps on change)
    double stageUs = 0.0, drawUs = 0.0, totalUs = 0.0;
};
TerrainIslandHealth g_health;

struct InteropForbidden {
    unsigned long glGetTexImage = 0;   // color CPU pull (must be 0)
    unsigned long glReadPixels  = 0;   // color CPU pull (must be 0)
};
InteropForbidden g_forbidden;

void setFallback(const char* reason) {
    std::strncpy(g_health.fallbackReason, reason ? reason : "",
                 sizeof(g_health.fallbackReason) - 1);
    g_health.fallbackReason[sizeof(g_health.fallbackReason) - 1] = '\0';
}

void vlog(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    std::fprintf(stderr, "[VK_TERRAIN_ISLAND] ");
    std::vfprintf(stderr, fmt, ap);
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
    va_end(ap);
}

// Gate: MC2_VK_TERRAIN_TIER1==1 AND MC2_VK_SHARED_SCENE_DEPTH==1 (the shared depth
// the Tier-1 pass depth-tests against). Mirrors propColorTier1Requested().
bool terrainTier1Requested() {
    static int state = -1;
    if (state == -1) {
        // Umbrella members: MC2_VK_SHARED_GBUFFER=1 arms both; explicit
        // MC2_VK_TERRAIN_TIER1 / MC2_VK_SHARED_SCENE_DEPTH =0/1 override.
        const bool t1    = mc2vkgate::subGate("MC2_VK_TERRAIN_TIER1");
        const bool depth = mc2vkgate::subGate("MC2_VK_SHARED_SCENE_DEPTH");
        state = (t1 && depth) ? 1 : 0;
    }
    return state == 1;
}

bool terrainNativePrefixClearRequested() {
    static int state = -1;
    if (state == -1) {
        state = (terrainTier1Requested() &&
                 mc2vkgate::envTriState("MC2_VK_TERRAIN_NATIVE_PREFIX_CLEAR") == 1) ? 1 : 0;
    }
    return state == 1;
}

float g_prefixClearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};

// VK-TERRAIN-ALL-LODS sub-slice (rung 3): extend VK terrain ownership beyond LOD0
// (lodStep==1) to all coarse bands (lodStep>1), non-displaced, non-stitched only.
// Requires TIER1 (no coarse bands without the LOD0 island already live). Skirts and
// edge-stitch remain GL-owned this rung (documented crack-check below).
//
// NOT a member of the MC2_VK_SHARED_GBUFFER umbrella family (unlike TIER1/LIGHTING/
// DETAIL etc.) -- this is a standalone, explicit-opt-in rung-3 gate that stays OFF
// even when the umbrella is armed, per the task's "new sub-gate, default OFF" ask.
// envTriState (not subGate) so an unset env is OFF regardless of umbrella state.
bool terrainAllLodsRequested() {
    static int state = -1;
    if (state == -1) {
        state = (terrainTier1Requested() && mc2vkgate::envTriState("MC2_VK_TERRAIN_ALL_LODS") == 1) ? 1 : 0;
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
// keep the rung-3 band win. Same envTriState pattern as ALL_LODS (unset -> OFF
// regardless of umbrella state).
bool terrainSkirtsRequested() {
    static int state = -1;
    if (state == -1) {
        state = (terrainAllLodsRequested() && mc2vkgate::envTriState("MC2_VK_TERRAIN_SKIRTS") == 1) ? 1 : 0;
    }
    return state == 1;
}

// VK-TERRAIN-VISUALDISPLACE-FAR-1 (rung 5): far-band visual-displace mode-2 Z-swap
// sub-gate. Requires ALL_LODS (mode-2 only exists on lodStep>1 bands). Same explicit
// envTriState opt-in pattern as MC2_VK_TERRAIN_SKIRTS (unset -> OFF regardless of
// umbrella) so the oracle can isolate "VK bands undisplaced (GL owns displaced)" from
// "VK bands displaced" -- if the Z-swap diverges, flip this OFF alone and keep rung 3/4.
bool terrainVisualDisplaceFarRequested() {
    static int state = -1;
    if (state == -1) {
        state = (terrainAllLodsRequested() && mc2vkgate::envTriState("MC2_VK_TERRAIN_VISUALDISPLACE_FAR") == 1) ? 1 : 0;
    }
    return state == 1;
}

std::string spv_dir() {
    const char* env = std::getenv("MC2_VULKAN_SPV_DIR");
    std::string d = (env && *env) ? env : "shaders/vulkan";
    if (!d.empty() && d.back() != '/' && d.back() != '\\') d += '/';
    return d;
}

VkShaderModule load_spv(VkDevice device, const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { vlog("cannot open spv '%s'.", path.c_str()); return VK_NULL_HANDLE; }
    std::fseek(f, 0, SEEK_END);
    long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (len <= 0 || (len % 4) != 0) { vlog("spv '%s' bad size %ld.", path.c_str(), len); std::fclose(f); return VK_NULL_HANDLE; }
    std::vector<uint32_t> code(static_cast<size_t>(len) / 4);
    size_t got = std::fread(code.data(), 1, static_cast<size_t>(len), f);
    std::fclose(f);
    if (got != static_cast<size_t>(len)) { vlog("spv short read '%s'.", path.c_str()); return VK_NULL_HANDLE; }
    VkShaderModuleCreateInfo smci{};
    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = static_cast<size_t>(len);
    smci.pCode = code.data();
    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &smci, nullptr, &mod) != VK_SUCCESS) {
        vlog("vkCreateShaderModule('%s') failed.", path.c_str()); return VK_NULL_HANDLE;
    }
    return mod;
}

// std140 per-patch UBO (LOCKSTEP with terrain_tier1.vert TerrainFrameUBO). One slot
// per LOD0 patch, addressed by dynamic offset. Padded to 96B so the UBO stride meets
// minUniformBufferOffsetAlignment (256 on all target GPUs after rounding).
struct TerrainFrameUBO {
    float   worldToClipGL[16]; // off 0
    int32_t mapSide;           // off 64
    float   halfMap;           // off 68
    int32_t blockOriginX;      // off 72
    int32_t blockOriginY;      // off 76
    // COLORMAP-BASE sub-slice: atlas-UV reconstruction params (mirror GL
    // u_atlasTopLeftX/Y + u_atlasOneOverWorldUnits) + colormap-ready flag.
    float   atlasTopLeftX;          // off 80
    float   atlasTopLeftY;          // off 84
    float   atlasOneOverWorldUnits; // off 88
    int32_t colormapReady;          // off 92
    // CEMENT/ROAD atlas sub-slice: cement uniforms (mirror the GL LOD-chunk frag's
    // u_useCement/u_cementGridSide/u_cementWUPT/u_useTransitionMask/u_cementDiagConnect).
    int32_t useCement;              // off 96
    int32_t cementGridSide;         // off 100
    float   cementWUPT;             // off 104
    int32_t useTransitionMask;      // off 108
    int32_t cementDiagConnect;      // off 112
    // VK-TERRAIN-LIGHTING sub-slice. std140: pad to off 128 (vec4 align), then the sun
    // dir vec4 + the static light-space mat4 + scalar toggles. LOCKSTEP with the frag.
    int32_t _lightPad0;             // off 116
    int32_t _lightPad1;             // off 120
    int32_t _lightPad2;             // off 124
    float   terrainLightDir[4];     // off 128 (vec4)
    float   staticLightSpaceMatrix[16]; // off 144 (mat4)
    int32_t lightingEnabled;        // off 208: MC2_VK_TERRAIN_LIGHTING gate
    int32_t enableShadows;          // off 212: static shadow map imported+valid this frame
    float   shadowSoftness;         // off 216: GL default 2.5
    int32_t _lightPad3;             // off 220
    // VK-TERRAIN-DETAIL sub-slice: terrain normal perturbation (macro slope + material
    // detail normal) so grass reads with NdotL relief like GL. LOCKSTEP with the frag.
    int32_t detailEnabled;          // off 224: MC2_VK_TERRAIN_DETAIL gate (requires lighting)
    int32_t detailReady;            // off 228: matNormalArray 2D_ARRAY imported+bound
    float   nfhStrength;            // off 232: terrainNormalsFromHeightStrength (GL default 1.0)
    float   detailTiling;           // off 236: detailNormalTiling.x (GL default 1.0)
    float   detailStrength;         // off 240: detailNormalStrength.x (GL default 1.0)
    // VK-TERRAIN-DYNSHADOW sub-slice: dynamic (building/mech) shadow. LOCKSTEP with frag.
    // std140: pad the dynamic light-space mat4 to a 16B boundary (off 256).
    int32_t enableDynShadows;       // off 244: dynamic shadow map imported+valid this frame
    int32_t _dynPad0;               // off 248
    int32_t _dynPad1;               // off 252
    float   dynamicLightSpaceMatrix[16]; // off 256..319 (mat4)
    // VK-TERRAIN-LODFADE sub-slice: camera world position (Stuff/MLR space) + the
    // gate toggle, packed into the existing pad tail (was _pad[48], off 320..511) so
    // the 512B dynamic-alignment slot does NOT grow. LOCKSTEP with terrain_tier1.frag.
    float   cameraPosMC2[4];        // off 320: gos_GetTerrainCameraPos(), .w pad
    int32_t lodFadeEnabled;         // off 336
    // VK-TERRAIN-SKIRT sub-slice (rung 4): skirt pull-down depth, mirrors GL
    // u_skirtDepth (gos_terrain_lod_chunk.cpp skirtDepths[i]). Read by
    // terrain_tier1_skirt.vert only (the main-patch vert never reads it — main
    // patches always draw with skirtDepth conceptually 0, same as GL's "draw main
    // patch (skirtDepth=0)" comment). Packed into the existing pad tail so the 512B
    // dynamic-alignment slot does NOT grow. LOCKSTEP with terrain_tier1_skirt.vert.
    float   skirtDepth;              // off 340
    // VK-TERRAIN-CSM-NEAR-1 sub-slice: near-cascade-array dynamic shadow (ports GL's
    // calcDynamicShadow cascade-select VERBATIM). LOCKSTEP with terrain_tier1.frag.
    // Slot grew 512->1024B to fit N cascade matrices; pad to a 16B boundary first.
    int32_t _csmPad0;                // off 344
    int32_t _csmPad1;                // off 348
    int32_t csmEnabled;              // off 352: MC2_VK_TERRAIN_CSM near-cascade path armed+valid this frame
    int32_t csmCount;                // off 356: 1..MC2_SHADOW_CSM_MAX (mirrors GL dynamicCsmCount)
    float   csmDepthSpan;            // off 360: (farP-nearP) of the shared CSM ortho z-row
    int32_t csmDebugMode;             // off 364: CSM-BLACKOUT-RECON-1 one-shot debug viz (0=off)
    float   csmCascadeMatrices[16 * 3]; // off 368: dynamicCascadeMatrices[MC2_SHADOW_CSM_MAX] (mat4 x3)
    float   csmCascadeTexelWorld[4];    // off 560: per-cascade world-texel footprint (vec3 used + pad)
    float   csmFullMapTexelWorld;    // off 576: dynamicFullMapTexelWorld (separate lower-res full-map tex)
    // VK-TERRAIN-VISUALDISPLACE-FAR-1 (rung 5): mode-2 far-band Z-swap params, packed
    // into the pad tail (slot does NOT grow). LOCKSTEP with terrain_tier1.vert /
    // terrain_tier1_skirt.vert / terrain_tier1.frag.
    int32_t visualSide;              // off 580: fine bake side V (0 = bake not snapshotted)
    float   visualDisplaceFar;       // off 584: MC2_TERRAIN_VISUAL_DISPLACE_FAR fade 0..1
    int32_t visualMode;              // off 588: 0 = no displace, 2 = far-band Z-swap (per patch)
    int32_t edgeStitch;              // off 592: VK-TERRAIN-EDGE-STITCH-1 packed N/S/W/E stride bytes
    int32_t quadCountX;              // off 596: max local offset X for edge tests
    int32_t quadCountY;              // off 600: max local offset Y for edge tests
    int32_t _pad[105];               // off 604..1023: remaining pad
};
static_assert(sizeof(TerrainFrameUBO) == 1024, "UBO slot must be 1024B (dynamic alignment, grew for CSM-NEAR-1)");

// int16 local-offset vertex (mirrors terrain_lod_chunk.cpp LocalVertex { int16 lx,ly }).
struct LocalVertex { int16_t lx, ly; };

const VkFormat kColorFmt = VK_FORMAT_R16G16B16A16_SFLOAT; // MainColor-compatible

// A cached LOD0 patch grid (VK-owned VBO/IBO), keyed by (quadCountX,quadCountY).
struct PatchMesh {
    VkBuffer      vbo = VK_NULL_HANDLE; VmaAllocation vboAlloc = VK_NULL_HANDLE;
    VkBuffer      ibo = VK_NULL_HANDLE; VmaAllocation iboAlloc = VK_NULL_HANDLE;
    uint32_t      indexCount = 0;
};

// VK-TERRAIN-SKIRT (rung 4): a cached skirt-strip mesh (VK-owned VBO/IBO), keyed by
// (quadCountX,quadCountY,lodStep) same as the main PatchMesh -- mirrors GL's
// per-patch skirtVbo/skirtIbo built alongside the main grid (getOrBuildPatch,
// gos_terrain_lod_chunk.cpp:459-518). Per-edge offset/count (N,S,W,E) let the draw
// loop honor the per-block skirt edge mask exactly like GL's mask-bit loop.
struct SkirtMesh {
    VkBuffer      vbo = VK_NULL_HANDLE; VmaAllocation vboAlloc = VK_NULL_HANDLE;
    VkBuffer      ibo = VK_NULL_HANDLE; VmaAllocation iboAlloc = VK_NULL_HANDLE;
    uint32_t      edgeOffset[4] = {0,0,0,0};   // 0=N,1=S,2=W,3=E (index offsets, in indices)
    uint32_t      edgeCount[4]  = {0,0,0,0};
};

// VK-TERRAIN-SKIRT vertex: mirrors GL's SkirtVertex { int16 lx, ly, isSkirt, _pad }.
struct SkirtVertex { int16_t lx, ly, isSkirt, _pad; };

struct TerrainIsland {
    bool disabled = false;
    bool inited   = false;

    int w = 0, h = 0;

    // Borrowed from the shared engine context (NOT owned).
    VkInstance       instance  = VK_NULL_HANDLE;
    VkPhysicalDevice phys      = VK_NULL_HANDLE;
    VkDevice         device    = VK_NULL_HANDLE;
    uint32_t         gfxFamily = UINT32_MAX;
    VkQueue          queue     = VK_NULL_HANDLE;
    VmaAllocator     allocator = VK_NULL_HANDLE;

    // Borrowed shared-GBuffer images (owned by the postprocess registries).
    VkImageResource* colorRes  = nullptr;
    VkImageResource* normalRes = nullptr;
    VkImageResource* depthRes  = nullptr;
    GlInteropView*   colorView  = nullptr;
    GlInteropView*   normalView = nullptr;
    GlInteropView*   depthView  = nullptr;

    // LOAD-preserve co-write renderpass + framebuffer (over the 3 shared views).
    VkRenderPass  rpass = VK_NULL_HANDLE;
    VkFramebuffer fb    = VK_NULL_HANDLE;

    // Pipeline (2 GBuffer outputs, reverse-Z GEQUAL depth-write, cull NONE, no blend).
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkDescriptorPool  pool   = VK_NULL_HANDLE;
    VkDescriptorSet   set    = VK_NULL_HANDLE;
    VkPipelineLayout  playout = VK_NULL_HANDLE;
    VkPipeline        pipe   = VK_NULL_HANDLE;
    VkShaderModule    vert   = VK_NULL_HANDLE;
    VkShaderModule    frag   = VK_NULL_HANDLE;

    // VK-TERRAIN-SKIRT (rung 4): second pipeline for skirt strips (2-attribute vertex
    // input: ivec2 localOffset + int isSkirtFlag, mirrors GL's skirt VAO). Shares the
    // SAME descriptor set / pipeline layout / renderpass as the main pipeline (only
    // the vertex-input state + shader modules differ) -- cull/depth/blend state is
    // IDENTICAL (double-sided, reverse-Z GEQUAL, opaque), matching GL's skirt draw
    // which reuses the same terrain program + GL state, just a different VAO/attribs.
    VkPipeline        skirtPipe = VK_NULL_HANDLE;
    VkShaderModule    skirtVert = VK_NULL_HANDLE;
    VkShaderModule    skirtFrag = VK_NULL_HANDLE;

    // VK-owned snapshot of the GL height SSBO (binding 23 -> here binding 1).
    VkBuffer      heightBuf = VK_NULL_HANDLE; VmaAllocation heightAlloc = VK_NULL_HANDLE;
    VkDeviceSize  heightCapBytes = 0;
    uint64_t      heightVersionCopied = UINT64_MAX; // last snapshotted feed version
    uint64_t      heightChecksum = 0;               // FNV-1a of the snapshot bytes (height-audit)
    bool          heightChecksumValid = false;      // a real snapshot has been checksummed

    // VK-TERRAIN-VISUALDISPLACE-FAR-1 (rung 5): VK-owned snapshot of the GL 4x visual
    // heightfield bake (GL binding 26 -> here binding 10). Version-gated mission-load
    // copy, mirrors the cement-word SSBO snapshot machinery.
    VkBuffer      visualHeightBuf = VK_NULL_HANDLE; VmaAllocation visualHeightAlloc = VK_NULL_HANDLE;
    VkDeviceSize  visualHeightCapBytes = 0;
    uint64_t      visualHeightVersionCopied = UINT64_MAX;
    bool          visualHeightReady = false;        // a real bake snapshot is live

    // COLORMAP-BASE sub-slice: VK-owned copy-once import of the GL colormap atlas
    // (g_atlasGLTex). Sampled image + view + sampler; binding 2. Keyed by the GL tex
    // name so a re-built atlas (per-mission) re-imports.
    VkImage       colormapImg   = VK_NULL_HANDLE; VmaAllocation colormapAlloc = VK_NULL_HANDLE;
    VkImageView   colormapView  = VK_NULL_HANDLE;
    VkSampler     colormapSamp  = VK_NULL_HANDLE;
    uint32_t      colormapTexImported = 0;   // GL tex name last imported (0 = none yet)
    bool          colormapReady = false;     // set once the sampled image is live

    // CEMENT/ROAD atlas sub-slice: VK-owned copy-once import of the GL cement atlas
    // (g_cementAtlasGLTex, runtime RGBA8). Sampled image + view + REPEAT sampler; binding
    // 3. Keyed by the GL tex name so a per-mission rebuild re-imports. PLUS a VK-owned
    // snapshot of the GL per-vertex cement-word SSBO (binding 25 -> here binding 4).
    VkImage       cementImg   = VK_NULL_HANDLE; VmaAllocation cementAlloc = VK_NULL_HANDLE;
    VkImageView   cementView  = VK_NULL_HANDLE;
    VkSampler     cementSamp  = VK_NULL_HANDLE;   // REPEAT/LINEAR (GL_REPEAT wrap)
    uint32_t      cementTexImported = 0;          // GL cement tex name last imported (0 = none)
    bool          cementReady = false;            // set once the cement image is live
    VkBuffer      cementBuf = VK_NULL_HANDLE; VmaAllocation cementBufAlloc = VK_NULL_HANDLE;
    VkDeviceSize  cementCapBytes = 0;
    uint64_t      cementVersionCopied = UINT64_MAX; // last snapshotted cement-word version
    bool          cementWordsReady = false;       // set once the cement SSBO has a real snapshot

    // VK-TERRAIN-LIGHTING sub-slice: the STATIC sun shadow map. A VK-owned D32_SFLOAT
    // depth image the island copies the GL static shadow depth texture INTO per frame
    // (glGetTexImage(GL_DEPTH_COMPONENT,GL_FLOAT) -> staging -> vkCmdCopyBufferToImage).
    // Sampled through a depth-COMPARE sampler (compareOp=LESS_OR_EQUAL, LINEAR HW PCF,
    // CLAMP_TO_BORDER white=1.0) at binding 5, exactly like the GL sampler2DShadow. The
    // GL map is DEPTH_COMPONENT24 but the readback is normalized float, so D32 is exact.
    VkImage       shadowImg   = VK_NULL_HANDLE; VmaAllocation shadowAlloc = VK_NULL_HANDLE;
    VkImageView   shadowView  = VK_NULL_HANDLE;
    VkSampler     shadowSamp  = VK_NULL_HANDLE;  // depth-compare, LESS_OR_EQUAL, LINEAR (shared)
    uint32_t      shadowDim   = 0;               // current VK shadow image edge (texels)
    bool          shadowReady = false;           // a real GL map was copied this frame
    VkBuffer      shadowStage = VK_NULL_HANDLE; VmaAllocation shadowStageAlloc = VK_NULL_HANDLE;
    VkDeviceSize  shadowStageBytes = 0;
    // VK-SHADOW-COPY-ONCE-1: last feed.staticShadowGeneration actually copied. The GL
    // static map only changes when this generation bumps (mission load / re-arm), so we
    // skip the glGetTexImage+VK-upload entirely on frames where it is unchanged.
    uint64_t      shadowGenCopied = UINT64_MAX;  // sentinel: never copied yet

    // VK-TERRAIN-DYNSHADOW: second D32 depth-COMPARE image for the DYNAMIC (building/mech)
    // shadow map at binding 7 (shares shadowSamp). Same copy-per-frame mechanism as static.
    VkImage       dynShadowImg  = VK_NULL_HANDLE; VmaAllocation dynShadowAlloc = VK_NULL_HANDLE;
    VkImageView   dynShadowView = VK_NULL_HANDLE;
    uint32_t      dynShadowDim  = 0;
    bool          dynShadowReady = false;
    VkBuffer      dynShadowStage = VK_NULL_HANDLE; VmaAllocation dynShadowStageAlloc = VK_NULL_HANDLE;
    VkDeviceSize  dynShadowStageBytes = 0;

    // VK-SHADOW-DIRECT-CONSUME-1: when the VK shadow island produced+fenced this
    // frame's dynamic shadow depth (mc2_vk_shadow_island_dyn_shadow_ready()), binding 7
    // is repointed at ITS VkImageView directly (no glGetTexImage copy at all) instead of
    // this island's own dynShadowView. Reset to null every frame before the decision;
    // write_descriptors() must be called whenever this changes (the view handle itself
    // varies frame to frame, unlike the stable owned dynShadowView the copy path reuses).
    VkImageView   dynShadowExternalView = VK_NULL_HANDLE;
    bool          dynShadowDirect = false;   // true this frame -> binding 7 used the external view
    VkImageView   dynShadowBoundView = VK_NULL_HANDLE;   // what binding 7 was last vkUpdateDescriptorSets'd with

    // VK-TERRAIN-CSM-NEAR-1: near-cascade D32 depth-COMPARE ARRAY (binding 8) + the
    // separate lower-res full-map D32 depth-COMPARE 2D image (binding 9). Copied per
    // frame via glGetTexImage (GL_TEXTURE_2D_ARRAY has no zero-copy producer yet — the
    // VK shadow island only produces the single legacy map — so this is the fallback
    // GL-copy path, same mechanism as static/dynamic single-map). Layers = csmCount-1
    // (near cascades only; the last cascade lives in the separate full-map image).
    VkImage       csmArrayImg  = VK_NULL_HANDLE; VmaAllocation csmArrayAlloc = VK_NULL_HANDLE;
    VkImageView   csmArrayView = VK_NULL_HANDLE;
    uint32_t      csmArrayDim  = 0;
    uint32_t      csmArrayLayers = 0;
    bool          csmArrayReady = false;
    VkImageView   csmArrayExternalView = VK_NULL_HANDLE;
    bool          csmArrayDirect = false;
    VkImageView   csmArrayBoundView = VK_NULL_HANDLE;
    VkBuffer      csmArrayStage = VK_NULL_HANDLE; VmaAllocation csmArrayStageAlloc = VK_NULL_HANDLE;
    VkDeviceSize  csmArrayStageBytes = 0;
    // CSM-BLACKOUT-RECON-1 FIX: GL scratch depth array. glGetTexImage on the LIVE
    // cascade array (an active FBO depth attachment re-rendered every frame) poisons
    // subsequent GL rendering on this AMD driver (whole-frame near-black; proven by
    // bisect: array-read-only reproduces, no-array-read is clean). We GPU-copy the live
    // array into this never-attached scratch via glCopyImageSubData and CPU-read the
    // scratch instead.
    unsigned int  csmScratchTex = 0;      // GLuint; 0 = not created
    uint32_t      csmScratchDim = 0;
    uint32_t      csmScratchLayers = 0;

    VkImage       csmFullMapImg  = VK_NULL_HANDLE; VmaAllocation csmFullMapAlloc = VK_NULL_HANDLE;
    VkImageView   csmFullMapView = VK_NULL_HANDLE;
    uint32_t      csmFullMapDim  = 0;
    bool          csmFullMapReady = false;
    VkImageView   csmFullMapExternalView = VK_NULL_HANDLE;
    bool          csmFullMapDirect = false;
    VkImageView   csmFullMapBoundView = VK_NULL_HANDLE;
    VkBuffer      csmFullMapStage = VK_NULL_HANDLE; VmaAllocation csmFullMapStageAlloc = VK_NULL_HANDLE;
    VkDeviceSize  csmFullMapStageBytes = 0;

    // VK-TERRAIN-DETAIL sub-slice: the material detail-normal ARRAY. A VK copy-once
    // import of the GL terrain_normal_array_tex_ (9-layer RGBA8 GL_TEXTURE_2D_ARRAY,
    // GL_REPEAT wrap). Sampled at binding 6 (sampler2DArray, REPEAT/LINEAR). Dummy 1x1
    // 1-layer until the real array imports (matNormalReady gates the FS sample).
    VkImage       matNormalImg   = VK_NULL_HANDLE; VmaAllocation matNormalAlloc = VK_NULL_HANDLE;
    VkImageView   matNormalView  = VK_NULL_HANDLE;
    VkSampler     matNormalSamp  = VK_NULL_HANDLE;   // REPEAT/LINEAR (GL_REPEAT wrap)
    uint32_t      matNormalTexImported = 0;          // GL array tex name last imported (0 = none)
    uint32_t      matNormalLayers = 0;               // layer count of the imported array
    bool          matNormalReady = false;            // set once the real array is live

    // Per-patch dynamic UBO (one 512B slot per LOD0 patch this frame; 512 is a multiple
    // of the 256B minUniformBufferOffsetAlignment — grew from 256 for the dynshadow mat4).
    VkBuffer      ubo = VK_NULL_HANDLE; VmaAllocation uboAlloc = VK_NULL_HANDLE;
    VkDeviceSize  uboCapBytes = 0;

    std::unordered_map<uint32_t, PatchMesh> patchCache;
    std::unordered_map<uint32_t, SkirtMesh> skirtCache;  // VK-TERRAIN-SKIRT (rung 4)

    VkCommandPool     cpool  = VK_NULL_HANDLE;
    VkCommandBuffer   cbuf[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
};

TerrainIsland* g_island = nullptr;

// Forward decls (COLORMAP-BASE helpers defined after snapshot_height, used earlier).
bool ensure_dummy_colormap(TerrainIsland* s);
void import_colormap(TerrainIsland* s, const Mc2TerrainTier1Feed& feed);
bool ensure_dummy_cement(TerrainIsland* s);
void import_cement(TerrainIsland* s, const Mc2TerrainTier1Feed& feed);
bool snapshot_cement(TerrainIsland* s, const Mc2TerrainTier1Feed& feed);
bool ensure_shadow_sampler(TerrainIsland* s);
bool ensure_dummy_shadow(TerrainIsland* s);
bool copy_static_shadow(TerrainIsland* s, const Mc2TerrainTier1Feed& feed);
bool copy_dynamic_shadow(TerrainIsland* s, const Mc2TerrainTier1Feed& feed);
bool ensure_dummy_csm(TerrainIsland* s);
bool copy_csm_shadows(TerrainIsland* s, const Mc2TerrainTier1Feed& feed);
bool copy_shadow_slot(TerrainIsland* s, uint32_t glTex, uint32_t dim,
                      VkImage img, VkBuffer* stage, VmaAllocation* stageAlloc, VkDeviceSize* stageBytes);
bool ensure_dummy_matnormal(TerrainIsland* s);
void import_matnormal(TerrainIsland* s, const Mc2TerrainTier1Feed& feed);

bool make_buffer(TerrainIsland* s, VkDeviceSize size, VkBufferUsageFlags usage,
                 VkBuffer* buf, VmaAllocation* alloc, bool hostVisible) {
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size ? size : 16; bci.usage = usage;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = hostVisible ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT : 0;
    if (vmaCreateBuffer(s->allocator, &bci, &aci, buf, alloc, nullptr) != VK_SUCCESS) {
        vlog("vmaCreateBuffer failed."); return false;
    }
    return true;
}

// Build (or fetch cached) a patch grid for (qcX,qcY,lodStep). qcX/qcY are the
// max-offset SPAN in map-cells (feed.quadCountX/Y, already rounded down to a multiple
// of lodStep by the GL capture site); lodStep strides the local-offset grid the same
// way GL's makeSamplePositions() does (`p += lodStep`, p in [0,qc]) so the vert
// shader's un-scaled `blockOriginX + localOffset.x` map-space math is IDENTICAL
// whether lodStep==1 (LOD0) or >1 (coarse band, VK-TERRAIN-ALL-LODS rung). The CCW
// two-triangles-per-quad split is the FIXED (TL-BR) diagonal (checkerDiag is a
// separate default-off feature not ported to the probe), matching getOrBuildPatch()'s
// bottomRight default.
const PatchMesh* getOrBuildPatch(TerrainIsland* s, int qcX, int qcY, int lodStep) {
    if (lodStep <= 0) lodStep = 1;
    const uint32_t key = ((uint32_t)(qcX & 0xFFFF)) | (((uint32_t)(qcY & 0xFFFF)) << 16)
                        ^ ((uint32_t)(lodStep & 0xFF) << 24);
    auto it = s->patchCache.find(key);
    if (it != s->patchCache.end()) return &it->second;
    if (qcX <= 0 || qcY <= 0) return nullptr;

    // Sample positions: 0, lodStep, 2*lodStep, ... up to qc (mirrors GL
    // makeSamplePositions(quadCount, lodStep)). nx/ny = number of grid POINTS.
    std::vector<int> xs, ys;
    for (int p = 0; p <= qcX; p += lodStep) xs.push_back(p);
    if (xs.empty() || xs.back() != qcX) xs.push_back(qcX);
    for (int p = 0; p <= qcY; p += lodStep) ys.push_back(p);
    if (ys.empty() || ys.back() != qcY) ys.push_back(qcY);
    const int nx = (int)xs.size(), ny = (int)ys.size();

    // Vertices: nx x ny grid of int16 local offsets (map-cell units, lodStep-strided).
    std::vector<LocalVertex> verts;
    verts.reserve((size_t)nx * ny);
    for (int yy = 0; yy < ny; ++yy)
        for (int xx = 0; xx < nx; ++xx)
            verts.push_back({(int16_t)xs[xx], (int16_t)ys[yy]});

    // Indices: two CCW triangles per quad, FIXED TL-BR diagonal (== getOrBuildPatch
    // bottomRight default): {TL,BL,BR} + {TL,BR,TR}.
    std::vector<uint16_t> indices;
    const int W = nx;
    for (int j = 0; j < ny - 1; ++j) {
        for (int i = 0; i < nx - 1; ++i) {
            uint16_t tl = (uint16_t)(j * W + i);
            uint16_t tr = (uint16_t)(j * W + i + 1);
            uint16_t bl = (uint16_t)((j + 1) * W + i);
            uint16_t br = (uint16_t)((j + 1) * W + i + 1);
            indices.insert(indices.end(), {tl, bl, br, tl, br, tr});
        }
    }

    PatchMesh pm;
    pm.indexCount = (uint32_t)indices.size();
    const VkDeviceSize vboBytes = verts.size() * sizeof(LocalVertex);
    const VkDeviceSize iboBytes = indices.size() * sizeof(uint16_t);
    if (!make_buffer(s, vboBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &pm.vbo, &pm.vboAlloc, true)) return nullptr;
    if (!make_buffer(s, iboBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,  &pm.ibo, &pm.iboAlloc, true)) return nullptr;
    { void* p = nullptr;
      if (vmaMapMemory(s->allocator, pm.vboAlloc, &p) == VK_SUCCESS) {
          std::memcpy(p, verts.data(), (size_t)vboBytes); vmaUnmapMemory(s->allocator, pm.vboAlloc); } }
    { void* p = nullptr;
      if (vmaMapMemory(s->allocator, pm.iboAlloc, &p) == VK_SUCCESS) {
          std::memcpy(p, indices.data(), (size_t)iboBytes); vmaUnmapMemory(s->allocator, pm.iboAlloc); } }

    auto res = s->patchCache.emplace(key, pm);
    return &res.first->second;
}

// VK-TERRAIN-SKIRT (rung 4): build (or fetch cached) a skirt-strip mesh for
// (qcX,qcY,lodStep), keyed the SAME way as getOrBuildPatch. Reproduces GL's skirt
// builder BIT-FOR-BIT (gos_terrain_lod_chunk.cpp:459-500 buildEdge lambda + the
// N,S,W,E per-edge offset/count bookkeeping) using the identical xs/ys sample-position
// construction getOrBuildPatch uses (mirrors GL's makeSamplePositions(quadCount,lodStep)
// call sharing the same coarse-line sample points as the main patch, so skirt verts
// land exactly on the main patch's edge verts -- no seam between skirt-top and
// main-patch-edge).
const SkirtMesh* getOrBuildSkirtMesh(TerrainIsland* s, int qcX, int qcY, int lodStep) {
    if (lodStep <= 0) lodStep = 1;
    const uint32_t key = ((uint32_t)(qcX & 0xFFFF)) | (((uint32_t)(qcY & 0xFFFF)) << 16)
                        ^ ((uint32_t)(lodStep & 0xFF) << 24);
    auto it = s->skirtCache.find(key);
    if (it != s->skirtCache.end()) return &it->second;
    if (qcX <= 0 || qcY <= 0) return nullptr;

    std::vector<int> xs, ys;
    for (int p = 0; p <= qcX; p += lodStep) xs.push_back(p);
    if (xs.empty() || xs.back() != qcX) xs.push_back(qcX);
    for (int p = 0; p <= qcY; p += lodStep) ys.push_back(p);
    if (ys.empty() || ys.back() != qcY) ys.push_back(qcY);

    std::vector<SkirtVertex> skirtVerts;
    std::vector<uint16_t>    skirtIdx;

    // Mirrors GL's buildEdge lambda exactly (gos_terrain_lod_chunk.cpp:477-494).
    auto buildEdge = [&](const std::vector<int>& uPos, int fixedCoord, bool fixedIsY) {
        int base = (int)skirtVerts.size();
        int n    = (int)uPos.size();
        for (int i = 0; i < n; ++i) {
            int16_t lx = fixedIsY ? (int16_t)uPos[i] : (int16_t)fixedCoord;
            int16_t ly = fixedIsY ? (int16_t)fixedCoord : (int16_t)uPos[i];
            skirtVerts.push_back({lx, ly, 0, 0});  // surface
            skirtVerts.push_back({lx, ly, 1, 0});  // below
        }
        for (int i = 0; i < n - 1; ++i) {
            uint16_t t0 = (uint16_t)(base + 2*i);
            uint16_t b0 = (uint16_t)(base + 2*i + 1);
            uint16_t t1 = (uint16_t)(base + 2*(i+1));
            uint16_t b1 = (uint16_t)(base + 2*(i+1) + 1);
            skirtIdx.insert(skirtIdx.end(), {t0, b0, b1, t0, b1, t1});
        }
    };

    SkirtMesh sm;
    sm.edgeOffset[0] = (uint32_t)skirtIdx.size(); buildEdge(xs, (int)ys.front(), false); sm.edgeCount[0] = (uint32_t)skirtIdx.size() - sm.edgeOffset[0]; // North
    sm.edgeOffset[1] = (uint32_t)skirtIdx.size(); buildEdge(xs, (int)ys.back(),  false); sm.edgeCount[1] = (uint32_t)skirtIdx.size() - sm.edgeOffset[1]; // South
    sm.edgeOffset[2] = (uint32_t)skirtIdx.size(); buildEdge(ys, (int)xs.front(), true ); sm.edgeCount[2] = (uint32_t)skirtIdx.size() - sm.edgeOffset[2]; // West
    sm.edgeOffset[3] = (uint32_t)skirtIdx.size(); buildEdge(ys, (int)xs.back(),  true ); sm.edgeCount[3] = (uint32_t)skirtIdx.size() - sm.edgeOffset[3]; // East

    if (skirtVerts.empty() || skirtIdx.empty()) return nullptr;

    const VkDeviceSize vboBytes = skirtVerts.size() * sizeof(SkirtVertex);
    const VkDeviceSize iboBytes = skirtIdx.size() * sizeof(uint16_t);
    if (!make_buffer(s, vboBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &sm.vbo, &sm.vboAlloc, true)) return nullptr;
    if (!make_buffer(s, iboBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,  &sm.ibo, &sm.iboAlloc, true)) return nullptr;
    { void* p = nullptr;
      if (vmaMapMemory(s->allocator, sm.vboAlloc, &p) == VK_SUCCESS) {
          std::memcpy(p, skirtVerts.data(), (size_t)vboBytes); vmaUnmapMemory(s->allocator, sm.vboAlloc); } }
    { void* p = nullptr;
      if (vmaMapMemory(s->allocator, sm.iboAlloc, &p) == VK_SUCCESS) {
          std::memcpy(p, skirtIdx.data(), (size_t)iboBytes); vmaUnmapMemory(s->allocator, sm.iboAlloc); } }

    auto res = s->skirtCache.emplace(key, sm);
    return &res.first->second;
}

void write_descriptors(TerrainIsland* s) {
    if (s->set == VK_NULL_HANDLE) return;
    // binding 0 = dynamic UBO (per-patch frame block); binding 1 = height SSBO;
    // binding 2 = colormap combined-image-sampler (COLORMAP-BASE, only when live).
    // binding 3 = cement combined-image-sampler (dummy/real); binding 4 = cement-word SSBO.
    VkDescriptorBufferInfo uboInfo{ s->ubo, 0, sizeof(TerrainFrameUBO) }; // range = one slot
    VkDescriptorBufferInfo hInfo{ s->heightBuf, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet w[11]{};
    uint32_t n = 0;
    w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n].dstSet = s->set;
    w[n].dstBinding = 0; w[n].descriptorCount = 1;
    w[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; w[n].pBufferInfo = &uboInfo; ++n;
    w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n].dstSet = s->set;
    w[n].dstBinding = 1; w[n].descriptorCount = 1;
    w[n].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[n].pBufferInfo = &hInfo; ++n;
    VkDescriptorImageInfo cmInfo{};
    if (s->colormapReady && s->colormapView != VK_NULL_HANDLE && s->colormapSamp != VK_NULL_HANDLE) {
        cmInfo.sampler = s->colormapSamp; cmInfo.imageView = s->colormapView;
        cmInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n].dstSet = s->set;
        w[n].dstBinding = 2; w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[n].pImageInfo = &cmInfo; ++n;
    }
    // CEMENT: binding 3 sampler — always written (dummy 1x1 until the real atlas imports).
    VkDescriptorImageInfo ceInfo{};
    if (s->cementView != VK_NULL_HANDLE && s->cementSamp != VK_NULL_HANDLE) {
        ceInfo.sampler = s->cementSamp; ceInfo.imageView = s->cementView;
        ceInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n].dstSet = s->set;
        w[n].dstBinding = 3; w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[n].pImageInfo = &ceInfo; ++n;
    }
    // CEMENT: binding 4 cement-word SSBO — always written (dummy 16B until first snapshot).
    VkDescriptorBufferInfo ceBuf{};
    if (s->cementBuf != VK_NULL_HANDLE) {
        ceBuf.buffer = s->cementBuf; ceBuf.offset = 0; ceBuf.range = VK_WHOLE_SIZE;
        w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n].dstSet = s->set;
        w[n].dstBinding = 4; w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[n].pBufferInfo = &ceBuf; ++n;
    }
    // VK-TERRAIN-LIGHTING: binding 5 static shadow sampler2DShadow — always written once
    // the (dummy or real) depth-compare image + sampler exist (uT.enableShadows gates use).
    VkDescriptorImageInfo shInfo{};
    if (s->shadowView != VK_NULL_HANDLE && s->shadowSamp != VK_NULL_HANDLE) {
        shInfo.sampler = s->shadowSamp; shInfo.imageView = s->shadowView;
        shInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n].dstSet = s->set;
        w[n].dstBinding = 5; w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[n].pImageInfo = &shInfo; ++n;
    }
    // VK-TERRAIN-DETAIL: binding 6 material detail-normal sampler2DArray — always written
    // once the (dummy 1x1x1 or real) array image + sampler exist (uT.detailReady gates use).
    VkDescriptorImageInfo mnInfo{};
    if (s->matNormalView != VK_NULL_HANDLE && s->matNormalSamp != VK_NULL_HANDLE) {
        mnInfo.sampler = s->matNormalSamp; mnInfo.imageView = s->matNormalView;
        mnInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n].dstSet = s->set;
        w[n].dstBinding = 6; w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[n].pImageInfo = &mnInfo; ++n;
    }
    // VK-TERRAIN-DYNSHADOW: binding 7 dynamic shadow sampler2DShadow — always written once
    // the (dummy or real) depth-compare image exists (uT.enableDynShadows gates use).
    // VK-SHADOW-DIRECT-CONSUME-1: prefer the shadow island's own view (zero-copy) when
    // it produced+fenced this frame; falls back to this island's own copy-updated image
    // (dynShadowView) otherwise. Layout is DEPTH_STENCIL_READ_ONLY_OPTIMAL on the direct
    // path (see vulkan_shadow_island_live.cpp render-pass finalLayout) which is a valid
    // sampled-read layout for a combined-image-sampler descriptor.
    VkDescriptorImageInfo dshInfo{};
    const VkImageView dshView = (s->dynShadowExternalView != VK_NULL_HANDLE)
        ? s->dynShadowExternalView : s->dynShadowView;
    if (dshView != VK_NULL_HANDLE && s->shadowSamp != VK_NULL_HANDLE) {
        dshInfo.sampler = s->shadowSamp; dshInfo.imageView = dshView;
        dshInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n].dstSet = s->set;
        w[n].dstBinding = 7; w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[n].pImageInfo = &dshInfo; ++n;
    }
    // VK-TERRAIN-CSM-NEAR-1: binding 8 near-cascade array, binding 9 full-map cascade —
    // always written once the (dummy or real) images + shared shadowSamp exist.
    VkDescriptorImageInfo csmArrInfo{};
    const VkImageView csmArrView = (s->csmArrayExternalView != VK_NULL_HANDLE)
        ? s->csmArrayExternalView : s->csmArrayView;
    if (csmArrView != VK_NULL_HANDLE && s->shadowSamp != VK_NULL_HANDLE) {
        csmArrInfo.sampler = s->shadowSamp; csmArrInfo.imageView = csmArrView;
        csmArrInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n].dstSet = s->set;
        w[n].dstBinding = 8; w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[n].pImageInfo = &csmArrInfo; ++n;
    }
    VkDescriptorImageInfo csmFmInfo{};
    const VkImageView csmFmView = (s->csmFullMapExternalView != VK_NULL_HANDLE)
        ? s->csmFullMapExternalView : s->csmFullMapView;
    if (csmFmView != VK_NULL_HANDLE && s->shadowSamp != VK_NULL_HANDLE) {
        csmFmInfo.sampler = s->shadowSamp; csmFmInfo.imageView = csmFmView;
        csmFmInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n].dstSet = s->set;
        w[n].dstBinding = 9; w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[n].pImageInfo = &csmFmInfo; ++n;
    }
    // VK-TERRAIN-VISUALDISPLACE-FAR-1: binding 10 visual-height (fine bake) SSBO —
    // always written (dummy 16B until the first bake snapshot; uT.visualMode==0 gates use).
    VkDescriptorBufferInfo vhBuf{};
    if (s->visualHeightBuf != VK_NULL_HANDLE) {
        vhBuf.buffer = s->visualHeightBuf; vhBuf.offset = 0; vhBuf.range = VK_WHOLE_SIZE;
        w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n].dstSet = s->set;
        w[n].dstBinding = 10; w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[n].pBufferInfo = &vhBuf; ++n;
    }
    vkUpdateDescriptorSets(s->device, n, w, 0, nullptr);
}

bool ensure_height_capacity(TerrainIsland* s, VkDeviceSize bytes) {
    if (s->heightBuf != VK_NULL_HANDLE && bytes <= s->heightCapBytes) return true;
    if (s->heightBuf != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(s->device);
        vmaDestroyBuffer(s->allocator, s->heightBuf, s->heightAlloc);
        s->heightBuf = VK_NULL_HANDLE;
    }
    VkDeviceSize cap = bytes + bytes / 4 + 4096;
    if (!make_buffer(s, cap, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &s->heightBuf, &s->heightAlloc, true))
        return false;
    s->heightCapBytes = cap;
    s->heightVersionCopied = UINT64_MAX; // force re-snapshot into the new buffer
    s->heightChecksumValid = false;      // stale checksum until the re-snapshot runs
    write_descriptors(s);
    return true;
}

bool ensure_ubo_capacity(TerrainIsland* s, VkDeviceSize bytes) {
    if (s->ubo != VK_NULL_HANDLE && bytes <= s->uboCapBytes) return true;
    if (s->ubo != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(s->device);
        vmaDestroyBuffer(s->allocator, s->ubo, s->uboAlloc);
        s->ubo = VK_NULL_HANDLE;
    }
    VkDeviceSize cap = bytes + bytes / 2 + sizeof(TerrainFrameUBO);
    if (!make_buffer(s, cap, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &s->ubo, &s->uboAlloc, true))
        return false;
    s->uboCapBytes = cap;
    write_descriptors(s);
    return true;
}

void destroy_island(TerrainIsland* s) {
    if (!s) return;
    VkDevice d = s->device;
    if (d != VK_NULL_HANDLE) vkDeviceWaitIdle(d);
    for (auto& kv : s->patchCache) {
        if (kv.second.vbo) vmaDestroyBuffer(s->allocator, kv.second.vbo, kv.second.vboAlloc);
        if (kv.second.ibo) vmaDestroyBuffer(s->allocator, kv.second.ibo, kv.second.iboAlloc);
    }
    s->patchCache.clear();
    // VK-TERRAIN-SKIRT (rung 4): mirror the main-patch teardown for skirt resources.
    for (auto& kv : s->skirtCache) {
        if (kv.second.vbo) vmaDestroyBuffer(s->allocator, kv.second.vbo, kv.second.vboAlloc);
        if (kv.second.ibo) vmaDestroyBuffer(s->allocator, kv.second.ibo, kv.second.iboAlloc);
    }
    s->skirtCache.clear();
    if (s->skirtPipe) vkDestroyPipeline(d, s->skirtPipe, nullptr);
    if (s->skirtVert) vkDestroyShaderModule(d, s->skirtVert, nullptr);
    if (s->skirtFrag) vkDestroyShaderModule(d, s->skirtFrag, nullptr);
    if (s->pipe)    vkDestroyPipeline(d, s->pipe, nullptr);
    if (s->playout) vkDestroyPipelineLayout(d, s->playout, nullptr);
    if (s->vert)    vkDestroyShaderModule(d, s->vert, nullptr);
    if (s->frag)    vkDestroyShaderModule(d, s->frag, nullptr);
    if (s->dsl)     vkDestroyDescriptorSetLayout(d, s->dsl, nullptr);
    if (s->pool)    vkDestroyDescriptorPool(d, s->pool, nullptr);
    if (s->fb)      vkDestroyFramebuffer(d, s->fb, nullptr);
    if (s->rpass)   vkDestroyRenderPass(d, s->rpass, nullptr);
    if (s->heightBuf) vmaDestroyBuffer(s->allocator, s->heightBuf, s->heightAlloc);
    if (s->visualHeightBuf) vmaDestroyBuffer(s->allocator, s->visualHeightBuf, s->visualHeightAlloc);
    if (s->ubo)     vmaDestroyBuffer(s->allocator, s->ubo, s->uboAlloc);
    if (s->colormapSamp) vkDestroySampler(d, s->colormapSamp, nullptr);
    if (s->colormapView) vkDestroyImageView(d, s->colormapView, nullptr);
    if (s->colormapImg)  vmaDestroyImage(s->allocator, s->colormapImg, s->colormapAlloc);
    if (s->cementSamp) vkDestroySampler(d, s->cementSamp, nullptr);
    if (s->cementView) vkDestroyImageView(d, s->cementView, nullptr);
    if (s->cementImg)  vmaDestroyImage(s->allocator, s->cementImg, s->cementAlloc);
    if (s->cementBuf)  vmaDestroyBuffer(s->allocator, s->cementBuf, s->cementBufAlloc);
    if (s->shadowSamp) vkDestroySampler(d, s->shadowSamp, nullptr);
    if (s->shadowView) vkDestroyImageView(d, s->shadowView, nullptr);
    if (s->shadowImg)  vmaDestroyImage(s->allocator, s->shadowImg, s->shadowAlloc);
    if (s->shadowStage) vmaDestroyBuffer(s->allocator, s->shadowStage, s->shadowStageAlloc);
    if (s->dynShadowView) vkDestroyImageView(d, s->dynShadowView, nullptr);
    if (s->dynShadowImg)  vmaDestroyImage(s->allocator, s->dynShadowImg, s->dynShadowAlloc);
    if (s->dynShadowStage) vmaDestroyBuffer(s->allocator, s->dynShadowStage, s->dynShadowStageAlloc);
    if (s->csmArrayView) vkDestroyImageView(d, s->csmArrayView, nullptr);
    if (s->csmArrayImg)  vmaDestroyImage(s->allocator, s->csmArrayImg, s->csmArrayAlloc);
    if (s->csmArrayStage) vmaDestroyBuffer(s->allocator, s->csmArrayStage, s->csmArrayStageAlloc);
    if (s->csmScratchTex) { glDeleteTextures(1, &s->csmScratchTex); s->csmScratchTex = 0; }
    if (s->csmFullMapView) vkDestroyImageView(d, s->csmFullMapView, nullptr);
    if (s->csmFullMapImg)  vmaDestroyImage(s->allocator, s->csmFullMapImg, s->csmFullMapAlloc);
    if (s->csmFullMapStage) vmaDestroyBuffer(s->allocator, s->csmFullMapStage, s->csmFullMapStageAlloc);
    if (s->matNormalSamp) vkDestroySampler(d, s->matNormalSamp, nullptr);
    if (s->matNormalView) vkDestroyImageView(d, s->matNormalView, nullptr);
    if (s->matNormalImg)  vmaDestroyImage(s->allocator, s->matNormalImg, s->matNormalAlloc);
    if (s->cpool)   vkDestroyCommandPool(d, s->cpool, nullptr);
    // instance/phys/device/queue/allocator + the shared GBuffer images are BORROWED.
}

// One-time init: fetch shared GBuffer, build LOAD renderpass/framebuffer/pipeline.
bool init_island(TerrainIsland* s, int w, int h) {
    s->w = w; s->h = h;
    ++g_health.attempted;

    if (!mc2vk::ensureEngineVulkanContextForInterop()) { setFallback("engine_vk_context_birth_failed"); return false; }
    mc2vk::VulkanContext& ctx = mc2vk::getOrCreateEngineVulkanContext();
    if (!ctx.valid()) { setFallback("engine_vk_context_not_valid"); return false; }
    s->instance  = ctx.instance();
    s->phys      = ctx.physicalDevice();
    s->device    = ctx.device();
    s->queue     = ctx.graphicsQueue();
    s->gfxFamily = ctx.graphicsFamily();
    s->allocator = ctx.allocator();
    if (s->device == VK_NULL_HANDLE || s->allocator == VK_NULL_HANDLE) { setFallback("engine_vk_device_or_allocator_null"); return false; }
    g_health.vulkanAvailable = 1;
    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(s->phys, &props);
        std::strncpy(g_health.deviceName, props.deviceName, sizeof(g_health.deviceName) - 1);
        g_health.deviceName[sizeof(g_health.deviceName) - 1] = '\0';
    }

    if (!terrainTier1Requested()) { setFallback("gate_off"); return false; }

    // ---- Fetch the SHARED scene color(MRT0)+normal(MRT1)+depth exportable images the
    // GL scene renders terrain into. Fail-soft: any missing => GL fallback. Same
    // accessors the prop Tier-1 island uses (built by the postprocess createFBOs seam,
    // now triggered for the terrain gate too). ----
    s->colorRes  = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_COLOR0);
    s->normalRes = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_NORMAL);
    s->depthRes  = mc2SharedSceneDepthGetResource();
    s->colorView  = mc2SharedSceneColorGetGlView(MC2_SHARED_SCENE_COLOR0);
    s->normalView = mc2SharedSceneColorGetGlView(MC2_SHARED_SCENE_NORMAL);
    s->depthView  = mc2SharedSceneDepthGetGlView();
    const bool nativePrefixClear = terrainNativePrefixClearRequested();
    if (!s->colorRes || !s->normalRes || !s->depthRes ||
        (!nativePrefixClear && (!s->colorView || !s->normalView || !s->depthView))) {
        setFallback("shared_gbuffer_not_ready");
        return false;
    }
    g_health.interopActive = nativePrefixClear ? 0 : 1;
    g_health.vk2glSemActive = (!nativePrefixClear && s->colorView && s->colorView->vkToGlImported) ? 1 : 0;

    // ---- Shared-GBuffer renderpass. Default path preserves the GL-cleared color
    // attachments. MC2_VK_TERRAIN_NATIVE_PREFIX_CLEAR makes terrain the native first
    // owner of color0/normal/depth for the frame so the strict no-interop canary can
    // advance past this island without a GL->VK semaphore handoff.
    {
        VkAttachmentDescription att[3]{};
        att[0].format = s->colorRes->format; att[0].samples = VK_SAMPLE_COUNT_1_BIT;
        att[0].loadOp = nativePrefixClear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att[0].finalLayout   = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att[1].format = s->normalRes->format; att[1].samples = VK_SAMPLE_COUNT_1_BIT;
        att[1].loadOp = nativePrefixClear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        att[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[1].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att[1].finalLayout   = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        // FAR-BAND-BLACK-1 FIX: depth uses CLEAR, not LOAD. The shared depth image is
        // exported as VK_FORMAT_D32_SFLOAT_S8_UINT (D24_UNORM_S8_UINT export is
        // unsupported on this device -- see vulkan_shared_scene_depth.cpp), while GL's
        // per-frame glClearDepth(0.0f)/glClear(GL_DEPTH_BUFFER_BIT) writes through a
        // GL_DEPTH24_STENCIL8 *reinterpretation* of the SAME bytes. 0.0f happens to be
        // bit-identical (0x00000000) in both encodings, so the GL clear alone would be
        // safe in isolation -- but terrain (this island) is the FIRST GL/VK writer each
        // frame and LOAD_OP_LOAD makes its GEQUAL depth test trust whatever is already
        // in the image at that instant, which depends on GL/VK submission-order
        // interleaving with the shared image's cross-API layout transitions; any
        // non-zero residue read through the wrong format's bit-layout is garbage, and
        // reverse-Z GEQUAL against garbage fails almost everywhere on a hard camera cut
        // (root cause of FAR-BAND-BLACK: overview_center/oblique_horizon divergence).
        // A native VK CLEAR here is unambiguous -- vkCmdClearDepthStencilImage/
        // VK_ATTACHMENT_LOAD_OP_CLEAR writes through the image's OWN declared format
        // (D32_SFLOAT_S8_UINT), so terrain always starts its depth test from a
        // guaranteed-correct reverse-Z-far value, independent of what GL's differently-
        // interpreted clear wrote to the same memory. Terrain runs first in the frame
        // (Terrain::flushDrawCommands() before mcTextureManager->renderLists() --
        // code/gamecam.cpp:530,535), so clearing here does not discard anything real:
        // nothing else has written scene depth yet this frame. STORE_OP_STORE (below)
        // still hands the correct depth downstream to props/mech Tier-1 (LOAD-preserve,
        // unaffected by this change) and to GL's own post-process reads.
        att[2].format = s->depthRes->format; att[2].samples = VK_SAMPLE_COUNT_1_BIT;
        att[2].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; att[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; att[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[2].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        att[2].finalLayout   = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentReference colorRefs[2] = {
            {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
            {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        };
        VkAttachmentReference depthRef{2, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 2; sub.pColorAttachments = colorRefs;
        sub.pDepthStencilAttachment = &depthRef;

        VkSubpassDependency deps[2]{};
        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL; deps[0].dstSubpass = 0;
        deps[0].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        deps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[1].srcSubpass = 0; deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[1].dstStageMask  = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        deps[1].dstAccessMask = 0;

        VkRenderPassCreateInfo rpci{};
        rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpci.attachmentCount = 3; rpci.pAttachments = att;
        rpci.subpassCount = 1; rpci.pSubpasses = &sub;
        rpci.dependencyCount = 2; rpci.pDependencies = deps;
        if (vkCreateRenderPass(s->device, &rpci, nullptr, &s->rpass) != VK_SUCCESS) {
            vlog("vkCreateRenderPass failed."); setFallback("renderpass_failed"); return false;
        }
        VkImageView fbViews[3] = { s->colorRes->view, s->normalRes->view, s->depthRes->view };
        VkFramebufferCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = s->rpass; fci.attachmentCount = 3; fci.pAttachments = fbViews;
        fci.width = (uint32_t)w; fci.height = (uint32_t)h; fci.layers = 1;
        if (vkCreateFramebuffer(s->device, &fci, nullptr, &s->fb) != VK_SUCCESS) {
            vlog("vkCreateFramebuffer failed."); setFallback("framebuffer_failed"); return false;
        }
    }

    // ---- Buffers (height snapshot SSBO + per-patch dynamic UBO). ----
    if (!ensure_height_capacity(s, 256 * 1024)) return false;
    if (!ensure_ubo_capacity(s, 64 * sizeof(TerrainFrameUBO))) return false;

    // ---- COLORMAP-BASE: command pool must exist before the copy-once import can
    // run its one-shot cmd buffer; create it here (also allocs the 2 draw cbufs). ----
    {
        VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = s->gfxFamily;
        if (vkCreateCommandPool(s->device, &pci, nullptr, &s->cpool) != VK_SUCCESS) {
            vlog("vkCreateCommandPool failed."); return false;
        }
        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = s->cpool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 2;
        if (vkAllocateCommandBuffers(s->device, &cai, s->cbuf) != VK_SUCCESS) {
            vlog("vkAllocateCommandBuffers failed."); return false;
        }
    }
    // Bind a 1x1 dummy colormap so binding 2 is a valid descriptor from frame 1
    // (colormapReady stays false until the real atlas imports -> FS uses olive).
    if (!ensure_dummy_colormap(s)) { setFallback("colormap_dummy_failed"); return false; }
    // CEMENT: dummy cement atlas (binding 3) + dummy cement-word SSBO (binding 4) so both
    // are valid descriptors from frame 1 (useCement stays 0 until the real atlas+words
    // are ready -> FS ignores the cement branch -> colormap base, no hole).
    if (!ensure_dummy_cement(s)) { setFallback("cement_dummy_failed"); return false; }
    if (s->cementBuf == VK_NULL_HANDLE) {
        if (!make_buffer(s, 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &s->cementBuf, &s->cementBufAlloc, true)) {
            setFallback("cement_dummy_ssbo_failed"); return false;
        }
        s->cementCapBytes = 16;
    }
    // VK-TERRAIN-VISUALDISPLACE-FAR-1: dummy 16B visual-height SSBO so binding 10 is a
    // valid descriptor from frame 1 (visualHeightReady stays false until the sub-gate is
    // on AND the fine bake snapshots -> UBO visualMode stays 0 -> VS never reads it).
    if (s->visualHeightBuf == VK_NULL_HANDLE) {
        if (!make_buffer(s, 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &s->visualHeightBuf, &s->visualHeightAlloc, true)) {
            setFallback("visual_height_dummy_ssbo_failed"); return false;
        }
        s->visualHeightCapBytes = 16;
    }
    // VK-TERRAIN-LIGHTING: dummy 1x1 depth-compare shadow (binding 5) so binding 5 is a
    // valid descriptor from frame 1 (shadowReady stays false until MC2_VK_TERRAIN_LIGHTING
    // is on AND the GL static shadow map is live -> FS enableShadows=0 -> lit-only, no hole).
    if (!ensure_dummy_shadow(s)) { setFallback("shadow_dummy_failed"); return false; }
    // VK-TERRAIN-DETAIL: 1x1x1 flat-normal matNormalArray dummy so binding 6 is a valid
    // descriptor from frame 1 (matNormalReady stays false until MC2_VK_TERRAIN_DETAIL is on
    // AND the GL array is live -> FS uses macro-slope-only normal, no hole).
    if (!ensure_dummy_matnormal(s)) { setFallback("matnormal_dummy_failed"); return false; }
    // VK-TERRAIN-CSM-NEAR-1: dummy 1x1(x1) near-cascade array + full-map depth-compare
    // (bindings 8/9) so both are valid descriptors from frame 1 (csmArrayReady/csmFullMapReady
    // stay false until MC2_VK_TERRAIN_CSM is armed AND GL CSM is actually built -> FS
    // csmEnabled=0 -> falls back to the legacy single-map path, no hole).
    if (!ensure_dummy_csm(s)) { setFallback("csm_dummy_failed"); return false; }

    // ---- Descriptor layout: binding0 dynamic UBO (VS+FS), binding1 height SSBO (VS). ----
    {
        VkDescriptorSetLayoutBinding b[11]{};
        b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        // VK-TERRAIN-DETAIL: binding 1 (height SSBO) is now ALSO read by the FS
        // (mv_smoothTerrainNormal reproduces GL's height-derived macro slope).
        b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        // COLORMAP-BASE: binding 2 = colormap sampler (FS). Always in the layout; the
        // descriptor is always written (dummy 1x1 until the real atlas is imported).
        b[2].binding = 2; b[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[2].descriptorCount = 1; b[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        // CEMENT: binding 3 = cement atlas sampler (FS); binding 4 = cement-word SSBO (FS).
        b[3].binding = 3; b[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[3].descriptorCount = 1; b[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        b[4].binding = 4; b[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[4].descriptorCount = 1; b[4].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        // VK-TERRAIN-LIGHTING: binding 5 = static shadow sampler2DShadow (FS).
        b[5].binding = 5; b[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[5].descriptorCount = 1; b[5].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        // VK-TERRAIN-DETAIL: binding 6 = material detail-normal sampler2DArray (FS).
        b[6].binding = 6; b[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[6].descriptorCount = 1; b[6].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        // VK-TERRAIN-DYNSHADOW: binding 7 = dynamic shadow sampler2DShadow (FS).
        b[7].binding = 7; b[7].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[7].descriptorCount = 1; b[7].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        // VK-TERRAIN-CSM-NEAR-1: binding 8 = near-cascade array sampler2DArrayShadow (FS);
        // binding 9 = full-map cascade sampler2DShadow (FS).
        b[8].binding = 8; b[8].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[8].descriptorCount = 1; b[8].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        b[9].binding = 9; b[9].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[9].descriptorCount = 1; b[9].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        // VK-TERRAIN-VISUALDISPLACE-FAR-1: binding 10 = visual-height fine-bake SSBO (VS,
        // both main + skirt verts read it for the mode-2 Z-swap).
        b[10].binding = 10; b[10].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[10].descriptorCount = 1; b[10].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.bindingCount = 11; lci.pBindings = b;
        if (vkCreateDescriptorSetLayout(s->device, &lci, nullptr, &s->dsl) != VK_SUCCESS) {
            vlog("vkCreateDescriptorSetLayout failed."); return false;
        }
        VkDescriptorPoolSize sizes[4]{};
        sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;  sizes[0].descriptorCount = 1;
        sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;          sizes[1].descriptorCount = 3; // height + cement + visual-height
        sizes[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;  sizes[2].descriptorCount = 7; // colormap + cement + static shadow + matNormalArray + dyn shadow + csm array + csm fullmap
        VkDescriptorPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.maxSets = 1; pci.poolSizeCount = 3; pci.pPoolSizes = sizes;
        if (vkCreateDescriptorPool(s->device, &pci, nullptr, &s->pool) != VK_SUCCESS) {
            vlog("vkCreateDescriptorPool failed."); return false;
        }
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = s->pool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &s->dsl;
        if (vkAllocateDescriptorSets(s->device, &dsai, &s->set) != VK_SUCCESS) {
            vlog("vkAllocateDescriptorSets failed."); return false;
        }
        write_descriptors(s);
    }

    // ---- Pipeline: int16x2 SINT vertex, reverse-Z GEQUAL depth-test+WRITE, cull
    // NONE, frontFace CCW, 2 GBuffer outputs, NO blend (opaque overwrite). State COPIED
    // from the GL terrain contract (== prop tier1 pipeline). ----
    {
        s->vert = load_spv(s->device, spv_dir() + "terrain_tier1.vert.spv");
        s->frag = load_spv(s->device, spv_dir() + "terrain_tier1.frag.spv");
        if (s->vert == VK_NULL_HANDLE || s->frag == VK_NULL_HANDLE) { setFallback("spv_load_failed"); return false; }

        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 1; plci.pSetLayouts = &s->dsl;
        if (vkCreatePipelineLayout(s->device, &plci, nullptr, &s->playout) != VK_SUCCESS) {
            vlog("vkCreatePipelineLayout failed."); return false;
        }

        // Mirror the GL terrain VAO: loc0 ivec2 (int16 lx,ly), stride 4, integer.
        VkVertexInputBindingDescription vib{};
        vib.binding = 0; vib.stride = sizeof(LocalVertex); vib.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        VkVertexInputAttributeDescription via{};
        via.location = 0; via.binding = 0; via.format = VK_FORMAT_R16G16_SINT; via.offset = 0;
        VkPipelineVertexInputStateCreateInfo vin{};
        vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vin.vertexBindingDescriptionCount = 1; vin.pVertexBindingDescriptions = &vib;
        vin.vertexAttributeDescriptionCount = 1; vin.pVertexAttributeDescriptions = &via;

        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo vps{};
        vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vps.viewportCount = 1; vps.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;                        // double-sided (terrainMVP X-flip)
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;         // authored CCW
        rs.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo ds{};
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        ds.depthTestEnable = VK_TRUE;
        ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;    // reverse-Z GEQUAL

        VkPipelineColorBlendAttachmentState cba[2]{};
        for (int k = 0; k < 2; ++k) {
            cba[k].blendEnable = VK_FALSE;                      // opaque in-place overwrite
            cba[k].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        }
        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 2; cb.pAttachments = cba;

        VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dsci{};
        dsci.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dsci.dynamicStateCount = 2; dsci.pDynamicStates = dyn;

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = s->vert; stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = s->frag; stages[1].pName = "main";
        VkGraphicsPipelineCreateInfo gp{};
        gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gp.stageCount = 2; gp.pStages = stages;
        gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vps; gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms; gp.pDepthStencilState = &ds; gp.pColorBlendState = &cb;
        gp.pDynamicState = &dsci;
        gp.layout = s->playout; gp.renderPass = s->rpass; gp.subpass = 0;
        if (vkCreateGraphicsPipelines(s->device, VK_NULL_HANDLE, 1, &gp, nullptr, &s->pipe) != VK_SUCCESS) {
            vlog("vkCreateGraphicsPipelines failed."); setFallback("pipeline_failed"); return false;
        }

        // VK-TERRAIN-SKIRT (rung 4): second pipeline, same layout/renderpass/raster/
        // depth/blend state as above -- only the shader modules + vertex-input state
        // differ (2 attributes: ivec2 localOffset + int isSkirtFlag, mirrors GL's
        // skirt VAO glVertexAttribIPointer calls, gos_terrain_lod_chunk.cpp:1744-1748).
        s->skirtVert = load_spv(s->device, spv_dir() + "terrain_tier1_skirt.vert.spv");
        s->skirtFrag = load_spv(s->device, spv_dir() + "terrain_tier1_skirt.frag.spv");
        if (s->skirtVert == VK_NULL_HANDLE || s->skirtFrag == VK_NULL_HANDLE) {
            setFallback("spv_load_failed"); return false;
        }

        VkVertexInputBindingDescription svib{};
        svib.binding = 0; svib.stride = sizeof(SkirtVertex); svib.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        VkVertexInputAttributeDescription svia[2]{};
        svia[0].location = 0; svia[0].binding = 0; svia[0].format = VK_FORMAT_R16G16_SINT;  svia[0].offset = 0;
        svia[1].location = 1; svia[1].binding = 0; svia[1].format = VK_FORMAT_R16_SINT;      svia[1].offset = 4;
        VkPipelineVertexInputStateCreateInfo svin{};
        svin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        svin.vertexBindingDescriptionCount = 1; svin.pVertexBindingDescriptions = &svib;
        svin.vertexAttributeDescriptionCount = 2; svin.pVertexAttributeDescriptions = svia;

        VkPipelineShaderStageCreateInfo sstages[2]{};
        sstages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        sstages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; sstages[0].module = s->skirtVert; sstages[0].pName = "main";
        sstages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        sstages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; sstages[1].module = s->skirtFrag; sstages[1].pName = "main";

        VkGraphicsPipelineCreateInfo sgp{};
        sgp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        sgp.stageCount = 2; sgp.pStages = sstages;
        sgp.pVertexInputState = &svin; sgp.pInputAssemblyState = &ia;
        sgp.pViewportState = &vps; sgp.pRasterizationState = &rs;
        sgp.pMultisampleState = &ms; sgp.pDepthStencilState = &ds; sgp.pColorBlendState = &cb;
        sgp.pDynamicState = &dsci;
        sgp.layout = s->playout; sgp.renderPass = s->rpass; sgp.subpass = 0;
        if (vkCreateGraphicsPipelines(s->device, VK_NULL_HANDLE, 1, &sgp, nullptr, &s->skirtPipe) != VK_SUCCESS) {
            vlog("vkCreateGraphicsPipelines (skirt) failed."); setFallback("pipeline_failed"); return false;
        }
    }

    s->inited = true;
    setFallback("");
    vlog("island initialized %dx%d (shared GBuffer co-write; LOD0 main-triangle terrain "
         "prefix; reverse-Z GEQUAL double-sided; gbuffer1.a=1.0).", w, h);
    return true;
}

// FNV-1a 64 over a byte range (height-audit checksum; cheap, order-sensitive).
static uint64_t fnv1a64(const void* data, size_t n) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(data);
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

// Snapshot the GL height SSBO into the VK-owned buffer when the feed version changes.
// Map-load copy (glGetBufferSubData -> vmaMap), NOT a per-frame handoff readback.
bool snapshot_height(TerrainIsland* s, const Mc2TerrainTier1Feed& feed) {
    if (feed.heightVersion == s->heightVersionCopied) return true; // unchanged
    if (feed.glHeightSsbo == 0 || feed.heightBytes == 0) return false;
    if (!ensure_height_capacity(s, (VkDeviceSize)feed.heightBytes)) return false;
    std::vector<uint8_t> scratch((size_t)feed.heightBytes);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, (GLuint)feed.glHeightSsbo);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)feed.heightBytes, scratch.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    void* p = nullptr;
    if (vmaMapMemory(s->allocator, s->heightAlloc, &p) != VK_SUCCESS) return false;
    std::memcpy(p, scratch.data(), (size_t)feed.heightBytes);
    vmaUnmapMemory(s->allocator, s->heightAlloc);
    s->heightVersionCopied = feed.heightVersion;
    // HEIGHT-AUDIT: store the checksum of exactly the bytes we copied into the VK
    // snapshot. The audit gate below re-reads GL live heights each frame and compares
    // this checksum — a mismatch means GL heights changed WITHOUT a heightVersion bump
    // (the g_terrainHeightVersion-only-in-UploadHeightFull staleness hazard) so the VK
    // snapshot silently diverged from the terrain GL draws against.
    s->heightChecksum = fnv1a64(scratch.data(), (size_t)feed.heightBytes);
    s->heightChecksumValid = true;
    ++g_health.heightSnapshots;
    return true;
}

// HEIGHT-AUDIT gate (MC2_VK_TERRAIN_HEIGHT_AUDIT, default-OFF presence gate). When on,
// EVERY frame re-reads the GL height SSBO and re-checksums it, comparing against the
// snapshot checksum stored at the last version-gated copy. Detects height-staleness:
// GL heights mutated (crater/deform) without a heightVersion bump -> VK snapshot stale
// -> macro-slope normal (mv_smoothTerrainNormal reads heightsFS) diverges from GL.
// This is a VERIFICATION gate (read-only): it never mutates render output, only logs.
static bool heightAuditRequested() {
    static int state = -1;
    if (state == -1) {
        const char* e = std::getenv("MC2_VK_TERRAIN_HEIGHT_AUDIT");
        state = (e && e[0] == '1' && e[1] == '\0') ? 1 : 0;
    }
    return state == 1;
}

// NEGATIVE-CONTROL hook: MC2_VK_TERRAIN_HEIGHT_AUDIT_CORRUPT=1 deliberately corrupts the
// stored snapshot checksum right after a snapshot so the audit MUST fire on the next
// frame — proves the audit has teeth (can actually detect divergence). Never on in prod.
static bool heightAuditCorruptRequested() {
    static int state = -1;
    if (state == -1) {
        const char* e = std::getenv("MC2_VK_TERRAIN_HEIGHT_AUDIT_CORRUPT");
        state = (e && e[0] == '1' && e[1] == '\0') ? 1 : 0;
    }
    return state == 1;
}

// Runs each frame when the audit gate is on. Returns true if heights are consistent
// (or audit inapplicable), false + LOUD log on a detected mismatch.
bool audit_height(TerrainIsland* s, const Mc2TerrainTier1Feed& feed) {
    if (!heightAuditRequested()) return true;
    if (!s->heightChecksumValid) return true;           // no snapshot yet -> nothing to audit
    if (feed.glHeightSsbo == 0 || feed.heightBytes == 0) return true;
    // NEGATIVE CONTROL: poison the stored checksum so the compare below is guaranteed
    // to mismatch on this frame's live GL heights. One-shot per process (latches).
    static bool s_corrupted = false;
    if (heightAuditCorruptRequested() && !s_corrupted) {
        s->heightChecksum ^= 0xA5A5A5A5A5A5A5A5ull;
        s_corrupted = true;
        vlog("[HEIGHT_AUDIT] NEGATIVE CONTROL armed: snapshot checksum deliberately corrupted "
             "-> next compare MUST report a mismatch.");
    }
    std::vector<uint8_t> live((size_t)feed.heightBytes);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, (GLuint)feed.glHeightSsbo);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)feed.heightBytes, live.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    const uint64_t liveSum = fnv1a64(live.data(), (size_t)feed.heightBytes);
    if (liveSum != s->heightChecksum) {
        static unsigned long s_reported = 0;
        if (s_reported < 8 || (s_reported % 300) == 0) {  // loud but rate-limited
            vlog("[HEIGHT_AUDIT] ★MISMATCH: VK height snapshot DIVERGED from GL live heights "
                 "(snapshot_fnv=0x%016llx live_fnv=0x%016llx bytes=%llu ver=%llu). GL terrain "
                 "heights mutated without a heightVersion bump -> VK macro-slope normal is stale.",
                 (unsigned long long)s->heightChecksum, (unsigned long long)liveSum,
                 (unsigned long long)feed.heightBytes, (unsigned long long)s->heightVersionCopied);
        }
        ++s_reported;
        return false;
    }
    // POSITIVE WITNESS (rate-limited): prove the audit actually EXECUTED and matched
    // against LIVE re-read GL heights this frame — not merely absent. Emits the live
    // checksum so a soak/deform run can show the audit is reading live data (the value
    // tracks g_terrainHeightVersion: it changes iff the GL heightfield is re-uploaded).
    {
        static unsigned long s_ok = 0;
        static uint64_t s_lastLive = 0;
        if (s_ok == 0 || liveSum != s_lastLive || (s_ok % 600) == 0) {
            vlog("[HEIGHT_AUDIT] OK: live GL heights match VK snapshot "
                 "(live_fnv=0x%016llx bytes=%llu ver=%llu).",
                 (unsigned long long)liveSum, (unsigned long long)feed.heightBytes,
                 (unsigned long long)s->heightVersionCopied);
            s_lastLive = liveSum;
        }
        ++s_ok;
    }
    return true;
}

// ---------------------------------------------------------------------------
// COLORMAP-BASE sub-slice: VK-owned copy-once import of the GL colormap atlas.
// PROVEN pattern (tools/vk_gl_interop_spike/main_sampletex.cpp): read the GL texel
// bytes back on the GL side (RGBA8 -> glGetTexImage(GL_RGBA); BC7 ->
// glGetCompressedTexImage), stage to a VK buffer, vkCmdCopyBufferToImage into a
// sampled VK image, LINEAR/CLAMP_TO_EDGE sampler (mip0). Match VK format to the GL
// upload's channel order / block encoding; do NOT double-encode sRGB.
// ---------------------------------------------------------------------------

// One-shot command buffer on the island's borrowed queue (mirror the spike helper).
VkCommandBuffer begin_oneshot(TerrainIsland* s) {
    VkCommandBufferAllocateInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool = s->cpool; cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbi.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(s->device, &cbi, &cb) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    return cb;
}
void end_oneshot(TerrainIsland* s, VkCommandBuffer cb) {
    vkEndCommandBuffer(cb);
    VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    vkQueueSubmit(s->queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(s->queue);
    vkFreeCommandBuffers(s->device, s->cpool, 1, &cb);
}

bool create_colormap_sampler(TerrainIsland* s) {
    if (s->colormapSamp != VK_NULL_HANDLE) return true;
    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;   // GL_LINEAR
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; // GL_CLAMP_TO_EDGE
    sci.maxLod = 0.25f;
    return vkCreateSampler(s->device, &sci, nullptr, &s->colormapSamp) == VK_SUCCESS;
}

// Generic: build a VK sampled image from raw bytes (staging -> copy -> SHADER_READ).
// Frees the previous image/view (passed by ref). Reused by BOTH the colormap and cement
// copy-once imports. Match `fmt` to the GL upload's channel order (RGBA8 storage read
// back as GL_RGBA -> R8G8B8A8_UNORM; do NOT double-encode sRGB).
bool upload_sampled_image(TerrainIsland* s, VkImage* outImg, VmaAllocation* outAlloc,
                          VkImageView* outView, VkFormat fmt, uint32_t width, uint32_t height,
                          const void* bytes, VkDeviceSize byteCount) {
    // Free any prior import (idempotent re-import on a per-mission atlas rebuild).
    if (*outView) { vkDestroyImageView(s->device, *outView, nullptr); *outView = VK_NULL_HANDLE; }
    if (*outImg)  { vmaDestroyImage(s->allocator, *outImg, *outAlloc); *outImg = VK_NULL_HANDLE; }

    // Staging buffer (host-visible).
    VkBuffer stg = VK_NULL_HANDLE; VmaAllocation stgAlloc = VK_NULL_HANDLE;
    if (!make_buffer(s, byteCount, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &stg, &stgAlloc, true)) return false;
    { void* p = nullptr;
      if (vmaMapMemory(s->allocator, stgAlloc, &p) != VK_SUCCESS) { vmaDestroyBuffer(s->allocator, stg, stgAlloc); return false; }
      std::memcpy(p, bytes, (size_t)byteCount); vmaUnmapMemory(s->allocator, stgAlloc); }

    // Device-local sampled image.
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D; ici.format = fmt;
    ici.extent = {width, height, 1}; ici.mipLevels = 1; ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo aci{}; aci.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateImage(s->allocator, &ici, &aci, outImg, outAlloc, nullptr) != VK_SUCCESS) {
        vmaDestroyBuffer(s->allocator, stg, stgAlloc); return false;
    }

    // Copy staging -> image, transition UNDEFINED -> TRANSFER_DST -> SHADER_READ.
    VkCommandBuffer cb = begin_oneshot(s);
    if (cb == VK_NULL_HANDLE) { vmaDestroyBuffer(s->allocator, stg, stgAlloc); return false; }
    VkImageMemoryBarrier bar{};
    bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; bar.image = *outImg;
    bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    bar.srcAccessMask = 0; bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &bar);
    VkBufferImageCopy rgn{};
    rgn.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rgn.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(cb, stg, *outImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rgn);
    bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &bar);
    end_oneshot(s, cb);
    vmaDestroyBuffer(s->allocator, stg, stgAlloc);

    // Image view.
    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = *outImg; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = fmt;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(s->device, &vci, nullptr, outView) != VK_SUCCESS) return false;
    return true;
}

// Colormap-atlas wrapper over the generic uploader.
bool upload_colormap_image(TerrainIsland* s, VkFormat fmt, uint32_t width, uint32_t height,
                           const void* bytes, VkDeviceSize byteCount) {
    return upload_sampled_image(s, &s->colormapImg, &s->colormapAlloc, &s->colormapView,
                                fmt, width, height, bytes, byteCount);
}

// Create a 1x1 opaque placeholder so binding 2 is ALWAYS a valid descriptor even
// before/without the real atlas (colormapReady stays false -> FS uses olive).
bool ensure_dummy_colormap(TerrainIsland* s) {
    if (!create_colormap_sampler(s)) return false;
    if (s->colormapView != VK_NULL_HANDLE) return true; // something already bound
    const uint8_t px[4] = {107, 102, 71, 255}; // ~olive, RGBA8; irrelevant (flag=0)
    if (!upload_colormap_image(s, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, px, sizeof(px))) return false;
    s->colormapReady = false;
    return true;
}

// Copy-once import of the GL colormap atlas. Re-imports only when the GL tex name
// changes (per-mission rebuild). Sets s->colormapReady on success. Fail-soft: leaves
// the dummy bound (colormapReady=false) so the FS falls back to olive.
void import_colormap(TerrainIsland* s, const Mc2TerrainTier1Feed& feed) {
    const uint32_t glTex = feed.glColormapTex;
    if (glTex == 0) return;                       // atlas not built yet -> stay olive
    if (glTex == s->colormapTexImported && s->colormapReady) return; // already imported

    // Query the REAL internalformat of the GL atlas (RGBA8 vs BC7 UNORM/SRGB).
    glBindTexture(GL_TEXTURE_2D, (GLuint)glTex);
    GLint w = 0, h = 0, internalFmt = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &internalFmt);
    if (w <= 0 || h <= 0) { glBindTexture(GL_TEXTURE_2D, 0); return; }

    bool ok = false;
    if (internalFmt == GL_COMPRESSED_RGBA_BPTC_UNORM ||
        internalFmt == GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM) {
        // BC7: copy the compressed blocks verbatim. VK format per UNORM/SRGB.
        GLint compBytes = 0;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_COMPRESSED_IMAGE_SIZE, &compBytes);
        if (compBytes > 0) {
            std::vector<uint8_t> blocks((size_t)compBytes);
            glGetCompressedTexImage(GL_TEXTURE_2D, 0, blocks.data());
            const VkFormat vkFmt = (internalFmt == GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM)
                                 ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK;
            ok = upload_colormap_image(s, vkFmt, (uint32_t)w, (uint32_t)h, blocks.data(), (VkDeviceSize)compBytes);
        }
    } else {
        // RGBA8 path: the GL atlas is RGBA8 storage (BGRA source swizzled to RGBA on
        // upload). Read it back as RGBA -> VK_FORMAT_R8G8B8A8_UNORM (channel order
        // preserved end-to-end; do NOT feed BGRA to an R8G8B8A8 image).
        std::vector<uint8_t> px((size_t)w * (size_t)h * 4u);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        ok = upload_colormap_image(s, VK_FORMAT_R8G8B8A8_UNORM, (uint32_t)w, (uint32_t)h,
                                   px.data(), (VkDeviceSize)px.size());
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    if (ok) {
        s->colormapTexImported = glTex;
        s->colormapReady = true;
        write_descriptors(s);
        static bool s_firstCm = false;
        if (!s_firstCm) {
            vlog("COLORMAP-BASE: imported GL colormap atlas gltex=%u %dx%d internalFmt=0x%x "
                 "(RGBA8->R8G8B8A8 or BC7 verbatim); VK terrain base color now real.",
                 glTex, w, h, (unsigned)internalFmt);
            s_firstCm = true;
        }
    }
    // On failure the dummy stays bound (colormapReady=false) -> FS uses olive.
}

// ---------------------------------------------------------------------------
// CEMENT/ROAD atlas sub-slice: copy-once import of the GL cement/road overlay atlas
// (g_cementAtlasGLTex, runtime-assembled RGBA8, GL_REPEAT wrap) + a REPEAT sampler
// (binding 3), plus a map-load snapshot of the per-vertex cement-word SSBO (GL binding
// 25 -> here binding 4). REUSES the colormap copy-once primitive verbatim; the ONLY
// deltas are (a) a REPEAT (not CLAMP) sampler — cement tiles wrap via fract() UV — and
// (b) the extra cement-word SSBO snapshot fed to the FS's cement branch.
// ---------------------------------------------------------------------------

bool create_cement_sampler(TerrainIsland* s) {
    if (s->cementSamp != VK_NULL_HANDLE) return true;
    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;   // GL_LINEAR
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    // ★CEMENT: GL_REPEAT wrap (gos_terrain_indirect.cpp:1291-1292) — cement tiles wrap.
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.maxLod = 0.25f;
    return vkCreateSampler(s->device, &sci, nullptr, &s->cementSamp) == VK_SUCCESS;
}

// 1x1 placeholder so binding 3 is ALWAYS a valid descriptor before the real atlas
// imports (cementReady stays false -> FS ignores the cement branch -> colormap base).
bool ensure_dummy_cement(TerrainIsland* s) {
    if (!create_cement_sampler(s)) return false;
    if (s->cementView != VK_NULL_HANDLE) return true; // already bound
    const uint8_t px[4] = {128, 128, 128, 255}; // neutral grey; irrelevant (useCement=0)
    if (!upload_sampled_image(s, &s->cementImg, &s->cementAlloc, &s->cementView,
                              VK_FORMAT_R8G8B8A8_UNORM, 1, 1, px, sizeof(px))) return false;
    s->cementReady = false;
    return true;
}

// Copy-once import of the GL cement atlas. Re-imports only when the GL tex name changes
// (per-mission rebuild). The cement atlas is ALWAYS RGBA8 storage (runtime-assembled;
// never BC7), uploaded from BGRA source swizzled to RGBA8 on upload — so reading it back
// as GL_RGBA -> VK_FORMAT_R8G8B8A8_UNORM preserves channel order end-to-end, IDENTICAL
// to the colormap RGBA8 path. Fail-soft: on failure the dummy stays bound.
void import_cement(TerrainIsland* s, const Mc2TerrainTier1Feed& feed) {
    const uint32_t glTex = feed.glCementAtlasTex;
    if (glTex == 0) return;                     // atlas not built yet -> cement stays off
    if (glTex == s->cementTexImported && s->cementReady) return; // already imported

    glBindTexture(GL_TEXTURE_2D, (GLuint)glTex);
    GLint w = 0, h = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
    if (w <= 0 || h <= 0) { glBindTexture(GL_TEXTURE_2D, 0); return; }
    std::vector<uint8_t> px((size_t)w * (size_t)h * 4u);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    if (upload_sampled_image(s, &s->cementImg, &s->cementAlloc, &s->cementView,
                             VK_FORMAT_R8G8B8A8_UNORM, (uint32_t)w, (uint32_t)h,
                             px.data(), (VkDeviceSize)px.size())) {
        s->cementTexImported = glTex;
        s->cementReady = true;
        write_descriptors(s);
        static bool s_firstCem = false;
        if (!s_firstCem) {
            vlog("CEMENT/ROAD: imported GL cement atlas gltex=%u %dx%d (RGBA8->R8G8B8A8, "
                 "REPEAT wrap); VK terrain concrete apron now matches GL.", glTex, w, h);
            s_firstCem = true;
        }
    }
    // On failure the dummy stays bound (cementReady=false) -> FS uses colormap base.
}

// ---------------------------------------------------------------------------
// VK-TERRAIN-DETAIL: 2D_ARRAY sampled-image uploader (RGBA8, all layers at level 0).
// Mirrors upload_sampled_image but with arrayLayers=layerCount and a 2D_ARRAY view.
// `bytes` is tightly packed layer-major (layer0 WxH RGBA8, then layer1, ...), exactly
// what glGetTexImage(GL_TEXTURE_2D_ARRAY, level0) returns.
bool upload_sampled_image_array(TerrainIsland* s, VkImage* outImg, VmaAllocation* outAlloc,
                                VkImageView* outView, VkFormat fmt, uint32_t width,
                                uint32_t height, uint32_t layers,
                                const void* bytes, VkDeviceSize byteCount) {
    if (*outView) { vkDestroyImageView(s->device, *outView, nullptr); *outView = VK_NULL_HANDLE; }
    if (*outImg)  { vmaDestroyImage(s->allocator, *outImg, *outAlloc); *outImg = VK_NULL_HANDLE; }

    VkBuffer stg = VK_NULL_HANDLE; VmaAllocation stgAlloc = VK_NULL_HANDLE;
    if (!make_buffer(s, byteCount, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &stg, &stgAlloc, true)) return false;
    { void* p = nullptr;
      if (vmaMapMemory(s->allocator, stgAlloc, &p) != VK_SUCCESS) { vmaDestroyBuffer(s->allocator, stg, stgAlloc); return false; }
      std::memcpy(p, bytes, (size_t)byteCount); vmaUnmapMemory(s->allocator, stgAlloc); }

    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D; ici.format = fmt;
    ici.extent = {width, height, 1}; ici.mipLevels = 1; ici.arrayLayers = layers;
    ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo aci{}; aci.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateImage(s->allocator, &ici, &aci, outImg, outAlloc, nullptr) != VK_SUCCESS) {
        vmaDestroyBuffer(s->allocator, stg, stgAlloc); return false;
    }

    VkCommandBuffer cb = begin_oneshot(s);
    if (cb == VK_NULL_HANDLE) { vmaDestroyBuffer(s->allocator, stg, stgAlloc); return false; }
    VkImageMemoryBarrier bar{};
    bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; bar.image = *outImg;
    bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    bar.srcAccessMask = 0; bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &bar);
    // One copy region covering all layers (tightly packed layer-major, level 0).
    VkBufferImageCopy rgn{};
    rgn.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers};
    rgn.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(cb, stg, *outImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rgn);
    bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &bar);
    end_oneshot(s, cb);
    vmaDestroyBuffer(s->allocator, stg, stgAlloc);

    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = *outImg; vci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY; vci.format = fmt;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    if (vkCreateImageView(s->device, &vci, nullptr, outView) != VK_SUCCESS) return false;
    return true;
}

bool create_matnormal_sampler(TerrainIsland* s) {
    if (s->matNormalSamp != VK_NULL_HANDLE) return true;
    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;   // GL_LINEAR (mag)
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    // GL_REPEAT wrap (gameos_graphics.cpp:2174-2175) — material detail normals tile.
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.maxLod = 0.25f;  // base level only (VK detail port samples level 0, no anti-tile mips)
    return vkCreateSampler(s->device, &sci, nullptr, &s->matNormalSamp) == VK_SUCCESS;
}

// 1x1x1 flat-normal placeholder so binding 6 is ALWAYS a valid descriptor before the
// real array imports (matNormalReady stays false -> FS uses macro-slope-only normal).
bool ensure_dummy_matnormal(TerrainIsland* s) {
    if (!create_matnormal_sampler(s)) return false;
    if (s->matNormalView != VK_NULL_HANDLE) return true;
    const uint8_t px[4] = {128, 128, 255, 255}; // (0,0,1) tangent-space flat normal
    if (!upload_sampled_image_array(s, &s->matNormalImg, &s->matNormalAlloc, &s->matNormalView,
                                    VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, px, sizeof(px))) return false;
    s->matNormalReady = false;
    s->matNormalLayers = 1;
    return true;
}

// Copy-once import of the GL terrain_normal_array_tex_ (9-layer RGBA8 GL_TEXTURE_2D_ARRAY,
// GL_REPEAT). Re-imports only when the GL tex name changes (per-mission rebuild). The GL
// array is RGBA8 storage (glTexImage3D GL_RGBA8); read back level 0 all layers as GL_RGBA
// -> VK_FORMAT_R8G8B8A8_UNORM (channel order preserved). Level-0 only (VK detail port does
// not use anti-tile mips). Fail-soft: on failure the dummy stays bound.
void import_matnormal(TerrainIsland* s, const Mc2TerrainTier1Feed& feed) {
    const uint32_t glTex = feed.glMatNormalArrayTex;
    if (glTex == 0) return;                        // array not built yet -> macro-slope only
    if (glTex == s->matNormalTexImported && s->matNormalReady) return; // already imported

    glBindTexture(GL_TEXTURE_2D_ARRAY, (GLuint)glTex);
    // Read the ARRAY's base level (GL_TEXTURE_BASE_LEVEL may be >0 after mip trimming);
    // query dimensions at level 0 (glTexImage3D allocates the full array at level 0).
    GLint w = 0, h = 0, layers = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_WIDTH,  &w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_HEIGHT, &h);
    glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_DEPTH,  &layers);
    if (w <= 0 || h <= 0 || layers <= 0) { glBindTexture(GL_TEXTURE_2D_ARRAY, 0); return; }
    std::vector<uint8_t> px((size_t)w * (size_t)h * (size_t)layers * 4u);
    glGetTexImage(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

    if (upload_sampled_image_array(s, &s->matNormalImg, &s->matNormalAlloc, &s->matNormalView,
                                   VK_FORMAT_R8G8B8A8_UNORM, (uint32_t)w, (uint32_t)h,
                                   (uint32_t)layers, px.data(), (VkDeviceSize)px.size())) {
        s->matNormalTexImported = glTex;
        s->matNormalLayers = (uint32_t)layers;
        s->matNormalReady = true;
        write_descriptors(s);
        static bool s_firstMn = false;
        if (!s_firstMn) {
            vlog("VK-TERRAIN-DETAIL: imported GL matNormalArray gltex=%u %dx%d x%d layers "
                 "(RGBA8->R8G8B8A8_2D_ARRAY, REPEAT); VK terrain grass now perturbed like GL.",
                 glTex, w, h, layers);
            s_firstMn = true;
        }
    }
    // On failure the dummy stays bound (matNormalReady=false) -> FS uses macro-slope only.
}

// Snapshot the GL cement-word SSBO into the VK-owned buffer when the feed version
// changes. Map-load copy (glGetBufferSubData -> vmaMap), mirroring snapshot_height.
bool snapshot_cement(TerrainIsland* s, const Mc2TerrainTier1Feed& feed) {
    if (feed.glCementSsbo == 0 || feed.cementBytes == 0) return false;  // not live yet
    if (feed.cementVersion == s->cementVersionCopied && s->cementWordsReady) return true;
    // Ensure capacity (grow-only, like the height buffer).
    if (s->cementBuf == VK_NULL_HANDLE || (VkDeviceSize)feed.cementBytes > s->cementCapBytes) {
        if (s->cementBuf != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(s->device);
            vmaDestroyBuffer(s->allocator, s->cementBuf, s->cementBufAlloc);
            s->cementBuf = VK_NULL_HANDLE;
        }
        VkDeviceSize cap = (VkDeviceSize)feed.cementBytes + feed.cementBytes / 4 + 4096;
        if (!make_buffer(s, cap, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &s->cementBuf, &s->cementBufAlloc, true))
            return false;
        s->cementCapBytes = cap;
        s->cementVersionCopied = UINT64_MAX; // force re-snapshot into the new buffer
        write_descriptors(s);
    }
    std::vector<uint8_t> scratch((size_t)feed.cementBytes);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, (GLuint)feed.glCementSsbo);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)feed.cementBytes, scratch.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    void* p = nullptr;
    if (vmaMapMemory(s->allocator, s->cementBufAlloc, &p) != VK_SUCCESS) return false;
    std::memcpy(p, scratch.data(), (size_t)feed.cementBytes);
    vmaUnmapMemory(s->allocator, s->cementBufAlloc);
    s->cementVersionCopied = feed.cementVersion;
    s->cementWordsReady = true;
    return true;
}

// VK-TERRAIN-VISUALDISPLACE-FAR-1: snapshot the GL 4x visual-heightfield bake SSBO
// (binding 26) into the VK-owned buffer when the feed version changes. Version-gated
// mission-load copy (glGetBufferSubData -> vmaMap), mirroring snapshot_cement — the
// bake is uploaded once per mission (gos_TerrainLodChunk_UploadVisualHeightFull), so
// this is a cheap no-op on every other frame. Fail-soft: on any failure
// visualHeightReady stays false -> UBO visualMode forced 0 -> VS draws undisplaced.
bool snapshot_visual_height(TerrainIsland* s, const Mc2TerrainTier1Feed& feed) {
    if (feed.glVisualHeightSsbo == 0 || feed.visualHeightBytes == 0) return false;  // bake not live
    if (feed.visualHeightVersion == s->visualHeightVersionCopied && s->visualHeightReady) return true;
    // Ensure capacity (grow-only, like the cement buffer).
    if (s->visualHeightBuf == VK_NULL_HANDLE || (VkDeviceSize)feed.visualHeightBytes > s->visualHeightCapBytes) {
        if (s->visualHeightBuf != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(s->device);
            vmaDestroyBuffer(s->allocator, s->visualHeightBuf, s->visualHeightAlloc);
            s->visualHeightBuf = VK_NULL_HANDLE;
        }
        VkDeviceSize cap = (VkDeviceSize)feed.visualHeightBytes + feed.visualHeightBytes / 4 + 4096;
        if (!make_buffer(s, cap, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &s->visualHeightBuf, &s->visualHeightAlloc, true))
            return false;
        s->visualHeightCapBytes = cap;
        s->visualHeightVersionCopied = UINT64_MAX; // force re-snapshot into the new buffer
        write_descriptors(s);
    }
    std::vector<uint8_t> scratch((size_t)feed.visualHeightBytes);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, (GLuint)feed.glVisualHeightSsbo);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)feed.visualHeightBytes, scratch.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    void* p = nullptr;
    if (vmaMapMemory(s->allocator, s->visualHeightAlloc, &p) != VK_SUCCESS) return false;
    std::memcpy(p, scratch.data(), (size_t)feed.visualHeightBytes);
    vmaUnmapMemory(s->allocator, s->visualHeightAlloc);
    s->visualHeightVersionCopied = feed.visualHeightVersion;
    s->visualHeightReady = true;
    static bool s_first = false;
    if (!s_first) {
        vlog("VK-TERRAIN-VISUALDISPLACE-FAR-1: snapshotted GL visual-height bake "
             "(V=%d bytes=%llu ver=%llu); VK far bands now Z-swap like GL mode 2.",
             feed.visualSide, (unsigned long long)feed.visualHeightBytes,
             (unsigned long long)feed.visualHeightVersion);
        s_first = true;
    }
    return true;
}

// ---------------------------------------------------------------------------
// VK-TERRAIN-LIGHTING sub-slice: STATIC sun shadow map. VK-SHADOW-COPY-ONCE-1 UPDATE:
// the GL static shadow map is built ONCE PER MISSION (gos_StaticLightMatrixBuilt()
// latch), not per frame -- the original "GL map updates every frame" premise below was
// wrong for the STATIC map (it is correct for the DYNAMIC map, which is genuinely
// re-rendered every frame from moving mechs/buildings and stays copy-per-frame). The
// static copy is now gated on feed.staticShadowGeneration so it only re-copies when the
// GL side actually rebuilt (mission load / re-arm). glGetTexImage on a DEPTH texture is
// NOT a color readback, so it does not trip the co-write path's forbidden-readback
// assert (that counts glGetTexImage/glReadPixels via g_forbidden, which this does not
// bump — it is a distinct depth pull on the shadow FBO's texture, endorsed for a probe).
// ---------------------------------------------------------------------------

// Depth-COMPARE sampler: compareOp=LESS_OR_EQUAL, LINEAR HW PCF, CLAMP_TO_BORDER
// white=1.0 (out-of-map -> lit). Mirrors GL sampler2DShadow + the compare_shadow spike.
bool ensure_shadow_sampler(TerrainIsland* s) {
    if (s->shadowSamp != VK_NULL_HANDLE) return true;
    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;   // HW LINEAR PCF (2x2)
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;   // out-of-map compare -> 1.0 (lit)
    sci.compareEnable = VK_TRUE;
    sci.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;            // ref <= stored ? 1 : 0 (proven by spike)
    sci.maxLod = 0.25f;
    return vkCreateSampler(s->device, &sci, nullptr, &s->shadowSamp) == VK_SUCCESS;
}

// (Re)create the VK D32 depth image at `dim` texels (idempotent when unchanged).
// Leaves it in DEPTH SHADER_READ layout so binding 5 is always valid; the per-frame
// copy re-transitions it. Frees the prior image on a size change.
// Slot-parameterized D32 depth-compare image ensure (static uses shadowImg/View/Dim,
// dynamic uses dynShadow*). Leaves DEPTH SHADER_READ so the binding is always valid.
bool ensure_shadow_image_slot(TerrainIsland* s, uint32_t dim,
                              VkImage* img, VmaAllocation* alloc, VkImageView* view, uint32_t* curDim) {
    if (dim == 0) dim = 1;
    if (*img != VK_NULL_HANDLE && *curDim == dim) return true;
    if (*view) { vkDestroyImageView(s->device, *view, nullptr); *view = VK_NULL_HANDLE; }
    if (*img)  { vmaDestroyImage(s->allocator, *img, *alloc); *img = VK_NULL_HANDLE; }

    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D; ici.format = VK_FORMAT_D32_SFLOAT;
    ici.extent = {dim, dim, 1}; ici.mipLevels = 1; ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo aci{}; aci.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateImage(s->allocator, &ici, &aci, img, alloc, nullptr) != VK_SUCCESS) {
        *img = VK_NULL_HANDLE; return false;
    }
    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = *img; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = VK_FORMAT_D32_SFLOAT;
    vci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(s->device, &vci, nullptr, view) != VK_SUCCESS) {
        vmaDestroyImage(s->allocator, *img, *alloc); *img = VK_NULL_HANDLE;
        *view = VK_NULL_HANDLE; return false;
    }
    *curDim = dim;
    VkCommandBuffer cb = begin_oneshot(s);
    if (cb != VK_NULL_HANDLE) {
        VkImageMemoryBarrier bar{};
        bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; bar.image = *img;
        bar.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        bar.srcAccessMask = 0; bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &bar);
        end_oneshot(s, cb);
    }
    write_descriptors(s);
    return true;
}

bool ensure_shadow_image(TerrainIsland* s, uint32_t dim) {
    return ensure_shadow_image_slot(s, dim, &s->shadowImg, &s->shadowAlloc, &s->shadowView, &s->shadowDim);
}

// VK-TERRAIN-CSM-NEAR-1: (Re)create a D32 depth-COMPARE ARRAY image at dim x layers
// (idempotent when unchanged). Mirrors ensure_shadow_image_slot but with arrayLayers>1
// and a 2D_ARRAY view (so the frag can bind it as sampler2DArrayShadow).
bool ensure_csm_array_image(TerrainIsland* s, uint32_t dim, uint32_t layers) {
    if (dim == 0) dim = 1;
    if (layers == 0) layers = 1;
    if (s->csmArrayImg != VK_NULL_HANDLE && s->csmArrayDim == dim && s->csmArrayLayers == layers)
        return true;
    if (s->csmArrayView) { vkDestroyImageView(s->device, s->csmArrayView, nullptr); s->csmArrayView = VK_NULL_HANDLE; }
    if (s->csmArrayImg)  { vmaDestroyImage(s->allocator, s->csmArrayImg, s->csmArrayAlloc); s->csmArrayImg = VK_NULL_HANDLE; }

    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D; ici.format = VK_FORMAT_D32_SFLOAT;
    ici.extent = {dim, dim, 1}; ici.mipLevels = 1; ici.arrayLayers = layers;
    ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo aci{}; aci.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateImage(s->allocator, &ici, &aci, &s->csmArrayImg, &s->csmArrayAlloc, nullptr) != VK_SUCCESS) {
        s->csmArrayImg = VK_NULL_HANDLE; return false;
    }
    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = s->csmArrayImg; vci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY; vci.format = VK_FORMAT_D32_SFLOAT;
    vci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, layers};
    if (vkCreateImageView(s->device, &vci, nullptr, &s->csmArrayView) != VK_SUCCESS) {
        vmaDestroyImage(s->allocator, s->csmArrayImg, s->csmArrayAlloc); s->csmArrayImg = VK_NULL_HANDLE;
        s->csmArrayView = VK_NULL_HANDLE; return false;
    }
    s->csmArrayDim = dim; s->csmArrayLayers = layers;
    VkCommandBuffer cb = begin_oneshot(s);
    if (cb != VK_NULL_HANDLE) {
        VkImageMemoryBarrier bar{};
        bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; bar.image = s->csmArrayImg;
        bar.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, layers};
        bar.srcAccessMask = 0; bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &bar);
        end_oneshot(s, cb);
    }
    write_descriptors(s);
    return true;
}

bool ensure_dummy_csm(TerrainIsland* s) {
    if (!ensure_shadow_sampler(s)) return false;
    if (s->csmArrayView == VK_NULL_HANDLE) {
        if (!ensure_csm_array_image(s, 1, 1)) return false;
        s->csmArrayReady = false;
    }
    if (s->csmFullMapView == VK_NULL_HANDLE) {
        if (!ensure_shadow_image_slot(s, 1, &s->csmFullMapImg, &s->csmFullMapAlloc,
                                      &s->csmFullMapView, &s->csmFullMapDim)) return false;
        s->csmFullMapReady = false;
    }
    return true;
}

// Per-frame copy of the GL near-cascade array (GL_TEXTURE_2D_ARRAY depth) into the VK
// D32 depth-COMPARE array, one glGetTexImage covering all layers (matches copy_shadow_slot's
// single-image approach but reads the whole array in one call — GL_TEXTURE_2D_ARRAY
// glGetTexImage returns all layers contiguously).
bool copy_csm_array(TerrainIsland* s, uint32_t glTex, uint32_t dim, uint32_t layers) {
    const VkDeviceSize bytes = (VkDeviceSize)dim * (VkDeviceSize)dim * (VkDeviceSize)layers * 4u;
    if (s->csmArrayStage == VK_NULL_HANDLE || bytes > s->csmArrayStageBytes) {
        if (s->csmArrayStage != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(s->device);
            vmaDestroyBuffer(s->allocator, s->csmArrayStage, s->csmArrayStageAlloc);
            s->csmArrayStage = VK_NULL_HANDLE;
        }
        if (!make_buffer(s, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &s->csmArrayStage, &s->csmArrayStageAlloc, true))
            return false;
        s->csmArrayStageBytes = bytes;
    }
    // CSM-BLACKOUT-RECON-1 FIX: NEVER glGetTexImage the live cascade array (an active
    // FBO depth attachment) — on this AMD driver that poisons subsequent GL rendering
    // (whole-frame near-black; bisect-proven: array-read-only reproduces, fullmap-only
    // and no-read are clean). GPU-copy the live array into a never-FBO-attached scratch
    // texture first (glCopyImageSubData, same DEPTH_COMPONENT24 internal format), then
    // CPU-read the scratch. Binding save/restore kept for hygiene.
    if (s->csmScratchTex == 0 || s->csmScratchDim != dim || s->csmScratchLayers != layers) {
        if (s->csmScratchTex) glDeleteTextures(1, &s->csmScratchTex);
        glGenTextures(1, &s->csmScratchTex);
        GLint prevA0 = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &prevA0);
        glBindTexture(GL_TEXTURE_2D_ARRAY, s->csmScratchTex);
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH_COMPONENT24,
                     (GLsizei)dim, (GLsizei)dim, (GLsizei)layers,
                     0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindTexture(GL_TEXTURE_2D_ARRAY, (GLuint)prevA0);
        s->csmScratchDim = dim; s->csmScratchLayers = layers;
    }
    glCopyImageSubData((GLuint)glTex, GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0,
                       s->csmScratchTex, GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0,
                       (GLsizei)dim, (GLsizei)dim, (GLsizei)layers);
    void* p = nullptr;
    if (vmaMapMemory(s->allocator, s->csmArrayStageAlloc, &p) != VK_SUCCESS) return false;
    // CSM-BLACKOUT-RECON-1 isolation: MC2_VK_CSM_COPYMODE=noread skips the CPU readback
    // (staging filled with 1.0f = fully lit) to discriminate glCopyImageSubData-from-live
    // vs glGetTexImage-on-scratch as the poison trigger.
    static const int s_noRead = [](){
        const char* v = getenv("MC2_VK_CSM_COPYMODE");
        return (v && strcmp(v, "noread") == 0) ? 1 : 0;
    }();
    if (s_noRead) {
        float* f = reinterpret_cast<float*>(p);
        const size_t n = (size_t)dim * dim * layers;
        for (size_t i = 0; i < n; ++i) f[i] = 1.0f;
    } else {
        GLint prevArrayBind = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &prevArrayBind);
        glBindTexture(GL_TEXTURE_2D_ARRAY, s->csmScratchTex);
        glGetTexImage(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH_COMPONENT, GL_FLOAT, p);
        glBindTexture(GL_TEXTURE_2D_ARRAY, (GLuint)prevArrayBind);
    }
    vmaUnmapMemory(s->allocator, s->csmArrayStageAlloc);

    VkCommandBuffer cb = begin_oneshot(s);
    if (cb == VK_NULL_HANDLE) return false;
    VkImageMemoryBarrier bar{};
    bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; bar.image = s->csmArrayImg;
    bar.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, layers};
    bar.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.srcAccessMask = VK_ACCESS_SHADER_READ_BIT; bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &bar);
    VkBufferImageCopy rgn{};
    rgn.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, layers};
    rgn.imageExtent = {dim, dim, 1};
    vkCmdCopyBufferToImage(cb, s->csmArrayStage, s->csmArrayImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rgn);
    bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &bar);
    end_oneshot(s, cb);
    return true;
}

// VK-TERRAIN-CSM-NEAR-1: import both the near-cascade array + the full-map cascade for
// this frame. Fail-soft: on any failure, csmArrayReady/csmFullMapReady stay false and the
// UBO's csmEnabled is forced 0 by the caller -> frag falls back to the legacy single-map
// path (or unshadowed), never a hole/crash.
// CSM-BLACKOUT-RECON-1 bisect: MC2_VK_CSM_BISECT=
//   1 = copies run normally, but the UBO forces csmEnabled=0 (shader on legacy path)
//   2 = glGetTexImage reads into a heap scratch only (no VMA map, no VK upload), shader off
//   3 = skip the near-array copy only    4 = skip the full-map copy only
static int csmBisectMode() {
    static const int m = [](){
        const char* v = getenv("MC2_VK_CSM_BISECT");
        return v ? atoi(v) : 0;
    }();
    return m;
}

bool copy_csm_shadows(TerrainIsland* s, const Mc2TerrainTier1Feed& feed) {
    s->csmArrayReady = false;
    s->csmFullMapReady = false;
    s->csmArrayExternalView = VK_NULL_HANDLE;
    s->csmArrayDirect = false;
    s->csmFullMapExternalView = VK_NULL_HANDLE;
    s->csmFullMapDirect = false;
    if (!feed.csmEnabled || feed.glCsmArrayTex == 0 || feed.csmArraySize <= 0
        || feed.glCsmFullMapTex == 0 || feed.csmFullMapSize <= 0 || feed.csmCount <= 1)
        return false;
    if (csmBisectMode() == 5) return false;  // null test: gates armed, NO reads, NO copies
    if (csmBisectMode() == 2) {
        // Scratch-read isolation: exercise ONLY the GL side (glGetTexImage), no VMA/VK.
        static std::vector<float> scratch;
        const size_t layers2 = (size_t)(feed.csmCount - 1);
        scratch.resize((size_t)feed.csmArraySize * feed.csmArraySize * layers2
                       + (size_t)feed.csmFullMapSize * feed.csmFullMapSize);
        GLint prevA = 0, prev2 = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &prevA);
        glBindTexture(GL_TEXTURE_2D_ARRAY, (GLuint)feed.glCsmArrayTex);
        glGetTexImage(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH_COMPONENT, GL_FLOAT, scratch.data());
        glBindTexture(GL_TEXTURE_2D_ARRAY, (GLuint)prevA);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev2);
        glBindTexture(GL_TEXTURE_2D, (GLuint)feed.glCsmFullMapTex);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, GL_FLOAT,
                      scratch.data() + (size_t)feed.csmArraySize * feed.csmArraySize * layers2);
        glBindTexture(GL_TEXTURE_2D, (GLuint)prev2);
        return false;  // ready flags stay false -> csmEnabled forced 0
    }
    const uint32_t layers = (uint32_t)(feed.csmCount - 1);  // near cascades only (last = full-map, separate tex)
    // CSM-BLACKOUT-RECON-1: save/restore FBO bindings around the copies — any touch of
    // the live cascade array (glGetTexImage OR glCopyImageSubData) blacked the frame,
    // consistent with a driver-side FBO/state disturbance during the depth-array access.
    GLint prevDrawFbo = 0, prevReadFbo = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDrawFbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFbo);
    struct FboRestore {
        GLint d, r;
        ~FboRestore() {
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)d);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)r);
        }
    } fboRestore{prevDrawFbo, prevReadFbo};
    const int bisect = csmBisectMode();
    if (bisect != 3) {
        if (mc2_vk_shadow_island_csm_array_ready()) {
            void* rawView = mc2_vk_shadow_island_get_csm_array_view();
            if (rawView != nullptr) {
                s->csmArrayExternalView = reinterpret_cast<VkImageView>(rawView);
                s->csmArrayDirect = true;
            }
        }
        if (!s->csmArrayDirect) {
            if (!ensure_csm_array_image(s, (uint32_t)feed.csmArraySize, layers)) return false;
            if (!copy_csm_array(s, feed.glCsmArrayTex, (uint32_t)feed.csmArraySize, layers)) return false;
        }
    }
    if (bisect != 4) {
        if (mc2_vk_shadow_island_csm_fullmap_ready()) {
            void* rawView = mc2_vk_shadow_island_get_csm_fullmap_view();
            if (rawView != nullptr) {
                s->csmFullMapExternalView = reinterpret_cast<VkImageView>(rawView);
                s->csmFullMapDirect = true;
            }
        }
        if (!s->csmFullMapDirect) {
            if (!ensure_shadow_image_slot(s, (uint32_t)feed.csmFullMapSize, &s->csmFullMapImg,
                                          &s->csmFullMapAlloc, &s->csmFullMapView, &s->csmFullMapDim)) return false;
            if (!copy_shadow_slot(s, feed.glCsmFullMapTex, (uint32_t)feed.csmFullMapSize, s->csmFullMapImg,
                                  &s->csmFullMapStage, &s->csmFullMapStageAlloc, &s->csmFullMapStageBytes)) return false;
        }
    }
    if (bisect != 0) return false;  // bisect 1/3/4: shader stays on the legacy path
    s->csmArrayReady = true;
    s->csmFullMapReady = true;
    return true;
}

// 1x1 dummies so bindings 5 (static) and 7 (dynamic) are valid from frame 1.
bool ensure_dummy_shadow(TerrainIsland* s) {
    if (!ensure_shadow_sampler(s)) return false;
    if (s->shadowView == VK_NULL_HANDLE) {
        if (!ensure_shadow_image(s, 1)) return false;
        s->shadowReady = false;
    }
    if (s->dynShadowView == VK_NULL_HANDLE) {
        if (!ensure_shadow_image_slot(s, 1, &s->dynShadowImg, &s->dynShadowAlloc,
                                      &s->dynShadowView, &s->dynShadowDim)) return false;
        s->dynShadowReady = false;
    }
    return true;
}

// Slot-parameterized per-frame depth copy from a GL DEPTH_COMPONENT texture into a VK
// D32 image. Shared by static (binding 5) and dynamic (binding 7) shadow maps. glGetTexImage
// depth readback -> forbidden color counters untouched.
bool copy_shadow_slot(TerrainIsland* s, uint32_t glTex, uint32_t dim,
                      VkImage img, VkBuffer* stage, VmaAllocation* stageAlloc, VkDeviceSize* stageBytes) {
    const VkDeviceSize bytes = (VkDeviceSize)dim * (VkDeviceSize)dim * 4u;  // float32 per texel
    if (*stage == VK_NULL_HANDLE || bytes > *stageBytes) {
        if (*stage != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(s->device);
            vmaDestroyBuffer(s->allocator, *stage, *stageAlloc);
            *stage = VK_NULL_HANDLE;
        }
        if (!make_buffer(s, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, stage, stageAlloc, true))
            return false;
        *stageBytes = bytes;
    }
    void* p = nullptr;
    if (vmaMapMemory(s->allocator, *stageAlloc, &p) != VK_SUCCESS) return false;
    // CSM-BLACKOUT-RECON-1 FIX (same hazard class as copy_csm_array): save/restore the
    // active unit's GL_TEXTURE_2D binding instead of stomping it to 0 mid-frame.
    GLint prev2DBind = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev2DBind);
    glBindTexture(GL_TEXTURE_2D, (GLuint)glTex);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, GL_FLOAT, p);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev2DBind);
    vmaUnmapMemory(s->allocator, *stageAlloc);

    VkCommandBuffer cb = begin_oneshot(s);
    if (cb == VK_NULL_HANDLE) return false;
    VkImageMemoryBarrier bar{};
    bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; bar.image = img;
    bar.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    bar.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.srcAccessMask = VK_ACCESS_SHADER_READ_BIT; bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &bar);
    VkBufferImageCopy rgn{};
    rgn.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    rgn.imageExtent = {dim, dim, 1};
    vkCmdCopyBufferToImage(cb, *stage, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rgn);
    bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &bar);
    end_oneshot(s, cb);
    return true;
}

// VK-SHADOW-COPY-ONCE-1: copy-once-per-generation of the GL STATIC shadow depth texture
// into the VK D32 image (binding 5). The GL static map is built ONCE per mission
// (gos_StaticLightMatrixBuilt() latch; see gos_postprocess.h/.cpp), not per frame, so
// feed.staticShadowGeneration only bumps on a real (re)build. Skipping the
// glGetTexImage(4096^2 float)->staging->VK-upload on unchanged frames removes a fixed
// per-frame tax with zero pixel change (same bytes end up in the VK image either way).
bool copy_static_shadow(TerrainIsland* s, const Mc2TerrainTier1Feed& feed) {
    if (!feed.lightingEnabled || !feed.shadowsEnabled) { s->shadowReady = false; return false; }
    if (feed.glStaticShadowTex == 0 || feed.staticShadowSize <= 0) { s->shadowReady = false; return false; }
    const uint32_t dim = (uint32_t)feed.staticShadowSize;
    if (!ensure_shadow_image(s, dim)) { s->shadowReady = false; return false; }

    // Already have this generation's content in the VK image (and dim unchanged, else
    // ensure_shadow_image would have recreated it and shadowReady/shadowGenCopied must be
    // re-synced below since a size change invalidates the copied image contents).
    if (s->shadowReady && s->shadowGenCopied == feed.staticShadowGeneration && s->shadowDim == dim)
        return true;

    if (!copy_shadow_slot(s, feed.glStaticShadowTex, dim, s->shadowImg,
                          &s->shadowStage, &s->shadowStageAlloc, &s->shadowStageBytes)) {
        s->shadowReady = false;
        return false;
    }
    s->shadowGenCopied = feed.staticShadowGeneration;
    s->shadowReady = true;
    return true;
}

// Per-frame copy of the GL DYNAMIC (building/mech) shadow depth texture into the 2nd VK
// D32 image (binding 7). Same mechanism as static; gated by MC2_VK_TERRAIN_DYNSHADOW feed.
//
// VK-SHADOW-DIRECT-CONSUME-1: when the VK shadow island (MC2_VK_SHADOW_INTEROP) produced
// this frame's dynamic shadow depth, skip the glGetTexImage+vkCmdCopyBufferToImage
// round-trip entirely and point binding 7 at the shadow island's own VkImageView
// (dynShadowExternalView; write_descriptors() picks it up). Falls back to the existing
// GL-copy mechanism whenever the shadow island declined this frame (fail-soft, never a
// hole — matches the umbrella's existing per-frame fail-soft convention). This is a
// dyn-shadow-CONSUME optimization only; the interop gate + producer are unchanged.
bool copy_dynamic_shadow(TerrainIsland* s, const Mc2TerrainTier1Feed& feed) {
    s->dynShadowReady = false;
    s->dynShadowExternalView = VK_NULL_HANDLE;
    s->dynShadowDirect = false;
    if (!feed.lightingEnabled || !feed.dynShadowsEnabled) return false;

    // Direct path: the shadow island already produced+fenced a VK-native depth image
    // this exact frame (same-queue, already retired -- see accessor doc comment).
    if (mc2_vk_shadow_island_dyn_shadow_ready()) {
        void* rawView = mc2_vk_shadow_island_get_dyn_shadow_view();
        if (rawView != nullptr) {
            s->dynShadowExternalView = reinterpret_cast<VkImageView>(rawView);
            s->dynShadowDirect = true;
            s->dynShadowReady = true;
            return true;
        }
    }

    // Fallback: existing GL-copy mechanism (unchanged).
    if (feed.glDynamicShadowTex == 0 || feed.dynamicShadowSize <= 0) return false;
    const uint32_t dim = (uint32_t)feed.dynamicShadowSize;
    if (!ensure_shadow_image_slot(s, dim, &s->dynShadowImg, &s->dynShadowAlloc,
                                  &s->dynShadowView, &s->dynShadowDim)) return false;
    if (!copy_shadow_slot(s, feed.glDynamicShadowTex, dim, s->dynShadowImg,
                          &s->dynShadowStage, &s->dynShadowStageAlloc, &s->dynShadowStageBytes)) return false;
    s->dynShadowReady = true;
    return true;
}

} // namespace

// ===========================================================================
// PUBLIC ENTRY POINTS.
// ===========================================================================

extern "C" int mc2_vk_terrain_island_enabled() {
    return terrainTier1Requested() ? 1 : 0;
}

extern "C" int mc2_vk_terrain_all_lods_enabled() {
    return terrainAllLodsRequested() ? 1 : 0;
}

extern "C" int mc2_vk_terrain_skirts_enabled() {
    return terrainSkirtsRequested() ? 1 : 0;
}

extern "C" int mc2_vk_terrain_visualdisplace_far_enabled() {
    return terrainVisualDisplaceFarRequested() ? 1 : 0;
}

// FAIL-SOFT ISLAND-READY predicate. True only when the gate is on AND the island is in a
// state where run() is expected to co-write this frame's LOD0 patches. The GL LOD-chunk
// loop gates its SPECULATIVE LOD0-skip on this: if the island can NOT render (SPV/pipeline
// load failed, init failed, device/allocator missing, or a prior transient error latched
// s->disabled), the GL path keeps drawing LOD0 so there is NEVER a terrain hole.
//
// This does NOT birth the island (that happens in run() on the render thread with GL
// context current); if the island is not yet inited it is NOT ready this frame (GL draws
// LOD0), and the first successful run() flips it ready for subsequent frames — a one-frame
// GL-draw-then-VK-takeover with no hole.
extern "C" int mc2_vk_terrain_island_ready() {
    if (!terrainTier1Requested()) return 0;
    if (!g_island) return 0;                 // not born yet -> GL owns LOD0 this frame
    if (g_island->disabled) return 0;        // latched off by a prior fatal (spv/pipeline/device)
    if (!g_island->inited) return 0;
    if (g_island->vert == VK_NULL_HANDLE || g_island->frag == VK_NULL_HANDLE) return 0; // spv missing
    if (g_island->pipe == VK_NULL_HANDLE) return 0;                                      // pipeline missing
    return 1;
}

// Birth/(re)init the island for the given scene target size, independent of any
// per-frame feed. Returns true when g_island is live (inited, not disabled). Must run
// on the GL/render thread (init touches the shared VK context / GL interop). This is
// FACTORED OUT of run() so the GL loop can PREPARE the island BEFORE deciding to skip
// LOD0 — without this the skip would gate on ready, ready would gate on a born island,
// and the island would only be born from a non-empty feed produced by the skip
// (chicken-egg deadlock -> island never births -> LOD0 always GL-drawn).
static bool ensure_island_ready(int w, int h) {
    if (!terrainTier1Requested()) return false;
    if (w <= 0 || h <= 0) return false;
    if (g_island && (g_island->w != w || g_island->h != h)) {
        destroy_island(g_island); delete g_island; g_island = nullptr;
    }
    if (!g_island) {
        g_island = new TerrainIsland();
        if (!init_island(g_island, w, h)) {
            vlog("init failed (%s) — GL fallback.", g_health.fallbackReason);
            destroy_island(g_island); delete g_island; g_island = nullptr;
            return false;
        }
    }
    return g_island && g_island->inited && !g_island->disabled;
}

// FAIL-SOFT prepare: the GL loop calls this at loop-top (GL context current) so the
// island is born BEFORE the skip decision. Returns 1 iff the island is ready to co-write
// this frame -> the GL loop then commits the LOD0 skip. 0 -> GL draws LOD0 (no hole).
extern "C" int mc2_vk_terrain_island_prepare(int w, int h) {
    return ensure_island_ready(w, h) ? 1 : 0;
}

extern "C" int mc2_vk_terrain_island_run(int w, int h) {
    ZoneScopedN("VK-terrain-island");   // VK-FLIP-FIELD-REPORT-1 coarse region zone
    if (!terrainTier1Requested()) return 0;
    if (w <= 0 || h <= 0) return 0;

    using clk = std::chrono::steady_clock;
    auto usSince = [](clk::time_point t0){ return std::chrono::duration<double, std::micro>(clk::now() - t0).count(); };
    const clk::time_point tTotal0 = clk::now();

    // Pull this frame's LOD0 patch list + shared params from the GL terrain path.
    Mc2TerrainTier1Feed feed{};
    if (!mc2_terrain_tier1_get_frame_feed(&feed)) { setFallback("no_terrain_feed"); return 0; }
    if (!feed.patches || feed.patchCount == 0) { setFallback("no_lod0_patches"); return 0; }
    if (feed.mapSide <= 0) { setFallback("bad_mapside"); return 0; }

    if (!ensure_island_ready(w, h)) return 0;
    TerrainIsland* s = g_island;
    if (s->disabled || !s->inited) return 0;

    const unsigned long f0b = g_forbidden.glGetTexImage;
    const unsigned long f0c = g_forbidden.glReadPixels;

    const clk::time_point tStage0 = clk::now();

    // Snapshot the height field (only re-copies when the feed version changes).
    { ZoneScopedN("VK-terrain-interop-height");   // VK-FLIP-FIELD-REPORT-1: GL->VK height copy
    if (!snapshot_height(s, feed)) { setFallback("height_snapshot_failed"); return 0; } }

    // HEIGHT-AUDIT (default-OFF gate): compare the VK snapshot checksum against GL live
    // heights each frame. Detected mismatch is logged LOUDLY but does NOT fail the frame
    // (verification-only) so the audit can run alongside a normal render for diagnosis.
    audit_height(s, feed);

    // VK-FLIP-FIELD-REPORT-1: atlas + per-frame shadow-map interop copies grouped in
    // one coarse zone (colormap/cement copy-once; static copy-once-per-generation since
    // VK-SHADOW-COPY-ONCE-1; dyn shadow copy-per-frame).
    const clk::time_point tCopies0 = clk::now();
    // VK-TERRAIN-STAGING-PERF-1: per-step sub-timers inside the copies zone, only
    // sampled when MC2_VK_TERRAIN_COPY_STATS is set (same gate as the summary line).
    static const bool s_copyStatsOn = (getenv("MC2_VK_TERRAIN_COPY_STATS") != nullptr);
    double subStaticUs = 0.0, subDynUs = 0.0, subCsmUs = 0.0;
    {
    ZoneScopedN("VK-terrain-interop-copies");
    // COLORMAP-BASE: copy-once import of the GL colormap atlas (no-op once imported;
    // no-op while the atlas isn't built -> FS falls back to olive). Must run while the
    // GL context is current (this entry point is called on the GL/render thread).
    import_colormap(s, feed);

    // CEMENT/ROAD: copy-once import of the GL cement atlas + map-load snapshot of the
    // per-vertex cement-word SSBO. Both no-op once imported / while not ready. Runs on
    // the GL/render thread (GL context current) — same as the colormap/height copies.
    import_cement(s, feed);
    snapshot_cement(s, feed);

    // VK-TERRAIN-VISUALDISPLACE-FAR-1: version-gated mission-load snapshot of the GL 4x
    // visual-height bake (no-op unless the sub-gate armed the feed fields AND the bake
    // version changed). Fail-soft: not-ready -> UBO visualMode forced 0 (undisplaced).
    if (feed.glVisualHeightSsbo != 0)
        snapshot_visual_height(s, feed);

    // VK-TERRAIN-LIGHTING: copy-once-per-generation of the GL static sun shadow map into
    // the VK D32 depth-compare image (VK-SHADOW-COPY-ONCE-1: the GL map is built once per
    // mission, so this call is a cheap no-op on unchanged frames -- see
    // feed.staticShadowGeneration / copy_static_shadow above). No-op (shadowReady=false)
    // when the lighting gate is off or the GL map isn't live -> VK terrain is lit but
    // unshadowed (fail-soft, never a hole). Runs on the GL/render thread.
    {
        const clk::time_point t0 = clk::now();
        copy_static_shadow(s, feed);
        if (s_copyStatsOn) subStaticUs = usSince(t0);
    }

    // VK-TERRAIN-DYNSHADOW: per-frame copy of the GL DYNAMIC (building/mech) shadow map
    // into the 2nd VK D32 depth-compare image. No-op (dynShadowReady=false) when the
    // sub-gate is off or the GL dynamic map isn't live -> VK terrain has no dynamic
    // shadow (fail-soft, never a hole). Same mechanism as the static copy above.
    {
        const clk::time_point t0 = clk::now();
        copy_dynamic_shadow(s, feed);
        if (s_copyStatsOn) subDynUs = usSince(t0);
    }

    // VK-TERRAIN-CSM-NEAR-1: per-frame copy of the GL near-cascade array + full-map
    // cascade. No-op (csmArrayReady/csmFullMapReady=false) when the gate is off or GL
    // CSM isn't actually built this frame -> UBO csmEnabled forced 0 below -> frag falls
    // back to the legacy single-map path (fail-soft, never a hole).
    {
        const clk::time_point t0 = clk::now();
        copy_csm_shadows(s, feed);
        if (s_copyStatsOn) subCsmUs = usSince(t0);
    }

    // VK-SHADOW-DIRECT-CONSUME-1: the direct view's underlying VkImageView handle is
    // stable across frames (owned by the shadow island's persistent ShadowIsland), but
    // binding 7 must still be refreshed via write_descriptors() the FIRST frame it
    // switches to/from the direct view (the descriptor set otherwise still points at
    // whatever was last written -- unlike the owned dynShadowView image, whose CONTENTS
    // change per-frame via copy_shadow_slot without needing a new descriptor write).
    {
        const VkImageView wantView = s->dynShadowDirect ? s->dynShadowExternalView : s->dynShadowView;
        const VkImageView wantCsmArrayView = s->csmArrayDirect ? s->csmArrayExternalView : s->csmArrayView;
        const VkImageView wantCsmFullMapView = s->csmFullMapDirect ? s->csmFullMapExternalView : s->csmFullMapView;
        if (wantView != s->dynShadowBoundView || wantCsmArrayView != s->csmArrayBoundView
            || wantCsmFullMapView != s->csmFullMapBoundView) {
            write_descriptors(s);
            s->dynShadowBoundView = wantView;
            s->csmArrayBoundView = wantCsmArrayView;
            s->csmFullMapBoundView = wantCsmFullMapView;
        }
    }

    // VK-TERRAIN-DETAIL: copy-once import of the GL material detail-normal 2D_ARRAY. No-op
    // once imported / while the gate is off (feed.glMatNormalArrayTex==0). Fail-soft: leaves
    // the flat-normal dummy bound (matNormalReady=false) -> FS uses macro-slope-only normal.
    import_matnormal(s, feed);
    }   // VK-FLIP-FIELD-REPORT-1: close VK-terrain-interop-copies zone

    // VK-SHADOW-COPY-ONCE-1 evidence: optional per-frame ms log for the interop-copies
    // zone, gated the same way as MC2_MECH_BATCHER_STATS (env-presence, off by default,
    // zero cost when unset). Lets a smoke run produce before/after timing without a
    // Tracy capture tool in the loop.
    {
        if (s_copyStatsOn) {
            const double ms = usSince(tCopies0) / 1000.0;
            fprintf(stderr,
                    "[VK_TERRAIN_COPY_STATS] copies_ms=%.4f static_us=%.1f dyn_us=%.1f csm_us=%.1f staticGen=%llu shadowReady=%d dynReady=%d dynDirect=%d csmArrayReady=%d csmArrayDirect=%d csmFullMapReady=%d csmFullMapDirect=%d\n",
                    ms, subStaticUs, subDynUs, subCsmUs,
                    (unsigned long long)feed.staticShadowGeneration, s->shadowReady ? 1 : 0,
                    s->dynShadowReady ? 1 : 0, s->dynShadowDirect ? 1 : 0,
                    s->csmArrayReady ? 1 : 0, s->csmArrayDirect ? 1 : 0,
                    s->csmFullMapReady ? 1 : 0,
                    s->csmFullMapDirect ? 1 : 0);
        }
    }

    // Fill one dynamic-UBO slot per LOD0 patch (shared MVP/mapSide/halfMap + per-patch
    // blockOrigin). quadCount comes from the patch record (drives grid rebuild).
    const uint32_t patchCount = feed.patchCount;
    const VkDeviceSize needUbo = (VkDeviceSize)patchCount * sizeof(TerrainFrameUBO);
    if (!ensure_ubo_capacity(s, needUbo)) { s->disabled = true; return 0; }
    {
        void* p = nullptr;
        if (vmaMapMemory(s->allocator, s->uboAlloc, &p) == VK_SUCCESS) {
            TerrainFrameUBO* slots = reinterpret_cast<TerrainFrameUBO*>(p);
            for (uint32_t i = 0; i < patchCount; ++i) {
                TerrainFrameUBO u{};
                std::memcpy(u.worldToClipGL, feed.worldToClipGL, sizeof(u.worldToClipGL));
                u.mapSide      = feed.mapSide;
                u.halfMap      = feed.halfMap;
                u.blockOriginX = feed.patches[i].blockOriginX;
                u.blockOriginY = feed.patches[i].blockOriginY;
                u.atlasTopLeftX          = feed.atlasTopLeftX;
                u.atlasTopLeftY          = feed.atlasTopLeftY;
                u.atlasOneOverWorldUnits = feed.atlasOneOverWorldUnits;
                u.colormapReady          = s->colormapReady ? 1 : 0;
                // CEMENT: only enable the FS cement branch when the GL gate is on AND both
                // the VK cement atlas image and the cement-word snapshot are live (fail-soft
                // if only one of them is ready -> colormap base, no hole).
                u.useCement         = (feed.useCement && s->cementReady && s->cementWordsReady) ? 1 : 0;
                u.cementGridSide    = feed.cementGridSide;
                u.cementWUPT        = feed.cementWUPT;
                u.useTransitionMask = feed.useTransitionMask;
                u.cementDiagConnect = feed.cementDiagConnect;
                // VK-TERRAIN-LIGHTING: sun dir + static light-space matrix + toggles. The
                // shadow is only sampled when the gate is on AND a real GL map was copied
                // this frame (s->shadowReady) — else enableShadows=0 -> lit-only, no hole.
                u.terrainLightDir[0] = feed.terrainLightDir[0];
                u.terrainLightDir[1] = feed.terrainLightDir[1];
                u.terrainLightDir[2] = feed.terrainLightDir[2];
                u.terrainLightDir[3] = 0.0f;
                std::memcpy(u.staticLightSpaceMatrix, feed.staticLightSpaceMatrix,
                            sizeof(u.staticLightSpaceMatrix));
                u.lightingEnabled = feed.lightingEnabled ? 1 : 0;
                u.enableShadows   = (feed.lightingEnabled && feed.shadowsEnabled && s->shadowReady) ? 1 : 0;
                u.shadowSoftness  = feed.shadowSoftness;
                // VK-TERRAIN-DETAIL: normal perturbation. detailReady is only 1 once the GL
                // matNormalArray actually imported (else the FS uses the macro-slope-only
                // normal — still a big win over flat-up, and never a hole).
                u.detailEnabled  = (feed.lightingEnabled && feed.detailEnabled) ? 1 : 0;
                u.detailReady    = s->matNormalReady ? 1 : 0;
                u.nfhStrength    = feed.nfhStrength;
                u.detailTiling   = feed.detailTiling;
                u.detailStrength = feed.detailStrength;
                // VK-TERRAIN-DYNSHADOW: enable only when the sub-gate is on AND a real GL
                // dynamic map was copied this frame (s->dynShadowReady). Else 0 -> no
                // dynamic shadow term (byte-identical to the pre-dynshadow frame).
                u.enableDynShadows = (feed.lightingEnabled && feed.dynShadowsEnabled && s->dynShadowReady) ? 1 : 0;
                std::memcpy(u.dynamicLightSpaceMatrix, feed.dynamicLightSpaceMatrix,
                            sizeof(u.dynamicLightSpaceMatrix));
                // VK-TERRAIN-LODFADE: camera pos + gate, precondition-chained on detail
                // being enabled (fading an already-off detail term is a no-op).
                u.cameraPosMC2[0] = feed.cameraPosMC2[0];
                u.cameraPosMC2[1] = feed.cameraPosMC2[1];
                u.cameraPosMC2[2] = feed.cameraPosMC2[2];
                u.cameraPosMC2[3] = 0.0f;
                u.lodFadeEnabled  = (u.detailEnabled && feed.lodFadeEnabled) ? 1 : 0;
                // VK-TERRAIN-SKIRT (rung 4): per-patch skirt pull-down depth (0 = this
                // patch's skirt sub-gate was off at capture, or GL reported no skirt for
                // this block -- island draws no skirt strips for it, see the draw loop).
                u.skirtDepth = feed.patches[i].skirtDepth;
                // VK-TERRAIN-CSM-NEAR-1: enable only when the gate armed AND both the near
                // array + full-map copies succeeded this frame (s->csmArrayReady/
                // csmFullMapReady). Else 0 -> frag falls back to the legacy single-map
                // dynamic-shadow path (u.enableDynShadows above), never a hole.
                u.csmEnabled = (feed.lightingEnabled && feed.csmEnabled
                                && s->csmArrayReady && s->csmFullMapReady) ? 1 : 0;
                u.csmCount     = feed.csmCount;
                u.csmDepthSpan = feed.csmDepthSpan;
                // CSM-BLACKOUT-RECON-1: one-shot debug viz, env-gated (no perf/behavior
                // impact when unset). See MC2_VK_TERRAIN_CSM_DEBUG in mc2vkgate.
                {
                    static const int dbgMode = [](){
                        const char* v = getenv("MC2_VK_TERRAIN_CSM_DEBUG");
                        return v ? atoi(v) : 0;
                    }();
                    u.csmDebugMode = dbgMode;
                }
                std::memcpy(u.csmCascadeMatrices, feed.csmCascadeMatrices, sizeof(u.csmCascadeMatrices));
                u.csmCascadeTexelWorld[0] = feed.csmCascadeTexelWorld[0];
                u.csmCascadeTexelWorld[1] = feed.csmCascadeTexelWorld[1];
                u.csmCascadeTexelWorld[2] = feed.csmCascadeTexelWorld[2];
                u.csmCascadeTexelWorld[3] = 0.0f;
                u.csmFullMapTexelWorld = feed.csmFullMapTexelWorld;
                // VK-TERRAIN-VISUALDISPLACE-FAR-1: mode-2 far-band Z-swap. Per-patch
                // visualMode (0 or 2, from the GL capture site) is only honored when the
                // bake snapshot is live AND the side is sane -- else forced 0 (VS draws
                // undisplaced; fail-soft, never a hole).
                u.visualSide        = feed.visualSide;
                u.visualDisplaceFar = feed.visualDisplaceFar;
                u.visualMode        = (feed.patches[i].visualMode == 2
                                       && s->visualHeightReady && feed.visualSide > 1) ? 2 : 0;
                u.edgeStitch        = (int32_t)feed.patches[i].edgeStitch;
                u.quadCountX        = feed.patches[i].quadCountX;
                u.quadCountY        = feed.patches[i].quadCountY;
                slots[i] = u;
            }
            vmaUnmapMemory(s->allocator, s->uboAlloc);
        }
    }
    g_health.stageUs = usSince(tStage0);
    g_health.patchRecords = patchCount;

    // ---- Begin a frame on each shared image; GL hands them to VK in attachment
    // layouts. COPIED from vulkan_color_prop_island.cpp tier1 path. ----
    VkFrameSync fsC = vkImageResourceBeginFrame(s->colorRes);
    const bool nativePrefixClear = terrainNativePrefixClearRequested();
    VkFrameSync fsN = nativePrefixClear ? VkFrameSync{} : vkImageResourceBeginFrame(s->normalRes);
    VkFrameSync fsD = nativePrefixClear ? VkFrameSync{} : vkImageResourceBeginFrame(s->depthRes);
    VkCommandBuffer cb = s->cbuf[fsC.parity & 1u];

    if (!nativePrefixClear) {
        glInteropSignalToVk(*s->colorView,  GL_LAYOUT_COLOR_ATTACHMENT_EXT,         fsC.parity);
        glInteropSignalToVk(*s->normalView, GL_LAYOUT_COLOR_ATTACHMENT_EXT,         fsN.parity);
        glInteropSignalToVk(*s->depthView,  GL_LAYOUT_DEPTH_STENCIL_ATTACHMENT_EXT, fsD.parity);
    }

    const clk::time_point tRender0 = clk::now();
    ZoneNamedN(zTerrRec, "VK-terrain-record-submit", true);   // VK-FLIP-FIELD-REPORT-1: CPU record+queue-submit (to fn return)
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) { setFallback("record_failed"); s->disabled = true; return 0; }

    // FAR-BAND-BLACK-1 FIX: attachment 2 clears to reverse-Z far. The native prefix
    // clear path also clears color0 and normal here, matching the GL frame-entry
    // clear plus clearGBuffer1 sentinel before terrain draws into the shared GBuffer.
    VkClearValue clears[3]{};
    clears[0].color.float32[0] = g_prefixClearColor[0];
    clears[0].color.float32[1] = g_prefixClearColor[1];
    clears[0].color.float32[2] = g_prefixClearColor[2];
    clears[0].color.float32[3] = g_prefixClearColor[3];
    clears[1].color.float32[0] = 0.5f;
    clears[1].color.float32[1] = 0.5f;
    clears[1].color.float32[2] = 1.0f;
    clears[1].color.float32[3] = 0.0f;
    clears[2].depthStencil = {0.0f, 0u};

    VkRenderPassBeginInfo rbi{};
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = s->rpass; rbi.framebuffer = s->fb;
    rbi.renderArea = {{0, 0}, {(uint32_t)w, (uint32_t)h}};
    rbi.clearValueCount = 3; rbi.pClearValues = clears;
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {(uint32_t)w, (uint32_t)h}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &scissor);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipe);

    unsigned long drawn = 0;
    unsigned long skirtDrawn = 0;  // VK-TERRAIN-SKIRT (rung 4): skirt edge-strip draw count
    for (uint32_t i = 0; i < patchCount; ++i) {
        const Mc2TerrainTier1Patch& pr = feed.patches[i];
        const PatchMesh* pm = getOrBuildPatch(s, pr.quadCountX, pr.quadCountY, pr.lodStep);
        if (!pm || pm->indexCount == 0) continue;
        const uint32_t dynOff = i * (uint32_t)sizeof(TerrainFrameUBO);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->playout, 0, 1, &s->set, 1, &dynOff);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipe);
        VkDeviceSize vboOff = 0;
        vkCmdBindVertexBuffers(cb, 0, 1, &pm->vbo, &vboOff);
        vkCmdBindIndexBuffer(cb, pm->ibo, 0, VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(cb, pm->indexCount, 1, 0, 0, 0);
        ++drawn;

        // VK-TERRAIN-SKIRT (rung 4): draw this patch's skirt strips, gated per-block
        // exactly like GL (skirtDepth>0 AND per-edge mask bit set,
        // gos_terrain_lod_chunk.cpp:1736,1753-1758). pr.skirtDepth/skirtEdgeMask are
        // 0 whenever the GL-side skirts sub-gate wasn't armed at capture -- no-op then.
        if (pr.skirtDepth > 0.0f && pr.skirtEdgeMask != 0u) {
            const SkirtMesh* sm = getOrBuildSkirtMesh(s, pr.quadCountX, pr.quadCountY, pr.lodStep);
            if (sm) {
                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->skirtPipe);
                VkDeviceSize svboOff = 0;
                vkCmdBindVertexBuffers(cb, 0, 1, &sm->vbo, &svboOff);
                vkCmdBindIndexBuffer(cb, sm->ibo, 0, VK_INDEX_TYPE_UINT16);
                for (int e = 0; e < 4; ++e) {
                    if (((pr.skirtEdgeMask >> e) & 1u) == 0u) continue;
                    if (sm->edgeCount[e] == 0) continue;
                    vkCmdDrawIndexed(cb, sm->edgeCount[e], 1, sm->edgeOffset[e], 0, 0);
                    ++skirtDrawn;
                }
            }
        }
    }

    vkCmdEndRenderPass(cb);
    if (vkEndCommandBuffer(cb) != VK_SUCCESS) { setFallback("record_failed"); s->disabled = true; return 0; }

    // Submit: WAIT all three GL->VK sems; SIGNAL all three VK->GL sems + color fence.
    VkSemaphore waitSems[3];   VkPipelineStageFlags waitStages[3];
    VkSemaphore signalSems[3]; uint32_t waitN = 0, sigN = 0;
    auto addWait = [&](VkSemaphore sem, VkPipelineStageFlags stage){
        if (sem != VK_NULL_HANDLE) { waitSems[waitN] = sem; waitStages[waitN] = stage; ++waitN; } };
    if (!nativePrefixClear) {
        addWait(fsC.signal, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        addWait(fsN.signal, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        addWait(fsD.signal, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);
    }
    const bool gpuC = !nativePrefixClear && s->colorView &&
                      s->colorView->vkToGlImported && fsC.vk2gl != VK_NULL_HANDLE;
    const bool gpuN = !nativePrefixClear && s->normalView &&
                      s->normalView->vkToGlImported && fsN.vk2gl != VK_NULL_HANDLE;
    const bool gpuD = !nativePrefixClear && s->depthView &&
                      s->depthView->vkToGlImported && fsD.vk2gl != VK_NULL_HANDLE;
    if (gpuC) signalSems[sigN++] = fsC.vk2gl;
    if (gpuN) signalSems[sigN++] = fsN.vk2gl;
    if (gpuD) signalSems[sigN++] = fsD.vk2gl;

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = waitN; si.pWaitSemaphores = waitSems; si.pWaitDstStageMask = waitStages;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    si.signalSemaphoreCount = sigN; si.pSignalSemaphores = signalSems;
    if (vkQueueSubmit(s->queue, 1, &si, fsC.fence) != VK_SUCCESS) { setFallback("submit_failed"); s->disabled = true; return 0; }

    if (!nativePrefixClear) {
        if (gpuC) glInteropWaitFromVk(*s->colorView,  GL_LAYOUT_COLOR_ATTACHMENT_EXT,         fsC.parity);
        if (gpuN) glInteropWaitFromVk(*s->normalView, GL_LAYOUT_COLOR_ATTACHMENT_EXT,         fsN.parity);
        if (gpuD) glInteropWaitFromVk(*s->depthView,  GL_LAYOUT_DEPTH_STENCIL_ATTACHMENT_EXT, fsD.parity);
    }

    if (vkWaitForFences(s->device, 1, &fsC.fence, VK_TRUE, 5ull*1000*1000*1000) != VK_SUCCESS) {
        setFallback("fence_timeout"); s->disabled = true; return 0;
    }
    g_health.drawUs = usSince(tRender0);

    // HARD assert: no COLOR-side readback fired on the co-write path (the height
    // snapshot uses glGetBufferSubData on an SSBO — that is NOT a color/tex readback
    // and does not touch these counters).
    const bool readbackFired =
        (g_forbidden.glGetTexImage != f0b) || (g_forbidden.glReadPixels != f0c);
    if (readbackFired) {
        vlog("FATAL: a color readback fired on the co-write path — failing frame + disabling.");
        setFallback("interop_did_readback"); s->disabled = true; return 0;
    }

    g_health.vkDraws = drawn + skirtDrawn;  // VK-TERRAIN-SKIRT: include skirt edge-strip draws
    ++g_health.usedVulkan;
    g_health.totalUs = usSince(tTotal0);
    static bool s_firstOk = false;
    if (!s_firstOk) {
        vlog("first TIER-1 in-place terrain GBuffer co-write: lod0Patches=%u drawn=%lu skirtDrawn=%lu "
             "(color(MRT0)+normal(MRT1 a=1.0)+depth co-written; no composite).",
             patchCount, drawn, skirtDrawn);
        s_firstOk = true;
    }
    return 1;
}

extern "C" void mc2_render_backend_terrain_region_health(
    int* vulkanAvailable, int* buildEnabled, const char** deviceName,
    unsigned long* attempted, unsigned long* usedVulkan, const char** fallbackReason,
    unsigned long* vkDraws, int* interopActive, int* vk2glSemActive,
    unsigned long* patchRecords, unsigned long* heightSnapshots,
    unsigned long* forbiddenGetTexImage, unsigned long* forbiddenReadPixels,
    double* stageUs, double* drawUs, double* totalUs) {
    if (vulkanAvailable)  *vulkanAvailable  = g_health.vulkanAvailable;
    if (buildEnabled)     *buildEnabled     = g_health.buildEnabled;
    if (deviceName)       *deviceName       = g_health.deviceName;
    if (attempted)        *attempted        = g_health.attempted;
    if (usedVulkan)       *usedVulkan       = g_health.usedVulkan;
    if (fallbackReason)   *fallbackReason   = g_health.fallbackReason;
    if (vkDraws)          *vkDraws          = g_health.vkDraws;
    if (interopActive)    *interopActive    = g_health.interopActive;
    if (vk2glSemActive)   *vk2glSemActive   = g_health.vk2glSemActive;
    if (patchRecords)     *patchRecords     = g_health.patchRecords;
    if (heightSnapshots)  *heightSnapshots  = g_health.heightSnapshots;
    if (forbiddenGetTexImage) *forbiddenGetTexImage = g_forbidden.glGetTexImage;
    if (forbiddenReadPixels)  *forbiddenReadPixels  = g_forbidden.glReadPixels;
    if (stageUs)          *stageUs          = g_health.stageUs;
    if (drawUs)           *drawUs           = g_health.drawUs;
    if (totalUs)          *totalUs          = g_health.totalUs;
}

extern "C" void mc2_vk_terrain_island_destroy() {
    vlog("[VK_TERRAIN_TIER1] vulkan_available=%d used_vulkan=%lu interop_active=%d vk2gl_sem=%d "
         "patch_records=%lu height_snapshots=%lu fallback_reason=\"%s\" vk_draws=%lu "
         "forbidden_getTexImage=%lu forbidden_readPixels=%lu stage_us=%.1f draw_us=%.1f total_us=%.1f",
         g_health.vulkanAvailable, g_health.usedVulkan, g_health.interopActive, g_health.vk2glSemActive,
         g_health.patchRecords, g_health.heightSnapshots, g_health.fallbackReason, g_health.vkDraws,
         g_forbidden.glGetTexImage, g_forbidden.glReadPixels,
         g_health.stageUs, g_health.drawUs, g_health.totalUs);
    if (!g_island) return;
    destroy_island(g_island);
    delete g_island; g_island = nullptr;
}

extern "C" void mc2_vk_terrain_set_native_prefix_clear_color(float r, float g, float b, float a)
{
    g_prefixClearColor[0] = r;
    g_prefixClearColor[1] = g;
    g_prefixClearColor[2] = b;
    g_prefixClearColor[3] = a;
}

#endif // MC2_VULKAN_ISLAND
