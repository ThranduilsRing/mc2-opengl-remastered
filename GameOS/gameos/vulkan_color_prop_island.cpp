// vulkan_color_prop_island.cpp — VK-P11 STATIC-PROP COLOR: the FIRST VK-native COLOR
// GEOMETRY region rendering REAL static-prop triangle geometry.
// Renders static props in Vulkan directly into the shared scene GBuffer. The
// retired Tier-0 side-image/composite proof path has been mechanically removed;
// this island now owns only the Tier-1 in-place co-write path.
//
// Shape:
//   - Borrows the shared engine VulkanContext; no private device.
//   - Uses the shared scene color0/normal/depth exportable images.
//   - Copies static-prop geometry once per map and stages visible instances per frame.
//   - Optional material/light feeds extend parity without changing the public health ABI.
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
#include "vulkan_shadow_island_live.h"   // Mc2ShadowGeomFeed + Mc2ColorPropFeed contracts
#include "vulkan_color_prop_island.h"
#include "vk_shared_gbuffer_gate.h"      // VK-P14 GATE-CONSOLIDATION-1 umbrella resolver
#define MC2_VULKAN_SHARED_SCENE_COLOR_IMPL_ISLAND
#include "vulkan_shared_scene_color.h"   // TIER-1: fetch shared scene color/normal images
#define MC2_VULKAN_SHARED_SCENE_DEPTH_IMPL_ISLAND
#include "vulkan_shared_scene_depth.h"   // TIER-1: fetch shared scene depth (island accessors)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cstdint>
#include <vector>
#include <string>
#include <chrono>
#include <algorithm>
#include <tracy/Tracy.hpp>   // VK-FLIP-FIELD-REPORT-1: coarse per-region frame zones

// VK-P11 STATIC-PROP MATERIAL: BC7 internalformat enums (fallback if GLEW headers lack).
#ifndef GL_COMPRESSED_RGBA_BPTC_UNORM
#define GL_COMPRESSED_RGBA_BPTC_UNORM        0x8E8C
#endif
#ifndef GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM
#define GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM  0x8E8D
#endif

namespace {

// ---------------------------------------------------------------------------
// Health snapshot (mirrors the shadow island's).
// ---------------------------------------------------------------------------
struct ColorPropIslandHealth {
    int   vulkanAvailable = 0;
    int   buildEnabled    = 1;
    char  deviceName[256] = {0};
    unsigned long validationErrors = 0;
    unsigned long attempted   = 0;
    unsigned long usedVulkan  = 0;
    char  fallbackReason[256] = {0};
    unsigned long vkDraws     = 0;
    int   interopActive  = 0;
    int   vk2glSemActive = 0;
    unsigned long usedInterop = 0;
    unsigned long instRecords = 0;   // color instances fed this frame
    unsigned long typeRuns    = 0;   // per-type instanced draws this frame
    unsigned long geomVerts   = 0;   // shared VBO verts copied once
    double stageInstUs = 0.0, drawUs = 0.0, totalUs = 0.0;
};
ColorPropIslandHealth g_health;

void setFallback(const char* reason) {
    std::strncpy(g_health.fallbackReason, reason ? reason : "",
                 sizeof(g_health.fallbackReason) - 1);
    g_health.fallbackReason[sizeof(g_health.fallbackReason) - 1] = '\0';
}

// Forbidden COLOR-readback counters (zero-copy invariant on the color surface).
struct InteropForbidden {
    unsigned long vkCmdCopyImageToBuffer = 0;  // color readback   (must be 0)
    unsigned long glGetTexImage          = 0;  // color CPU pull   (must be 0)
    unsigned long glReadPixels           = 0;  // color CPU pull   (must be 0)
};
InteropForbidden g_forbidden;

void vlog(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    std::fprintf(stderr, "[VK_COLOR_PROP_ISLAND] ");
    std::vfprintf(stderr, fmt, ap);
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
    va_end(ap);
}

// TIER-1 (parity): co-write props IN-PLACE into the SHARED scene color+normal+depth
// GBuffer. Default-OFF; requires MC2_VK_SHARED_SCENE_DEPTH
// (the shared depth Tier-1 depth-tests against). When Tier-1 is armed it REPLACES the
// Gate OFF -> GL flush, byte-identical.
bool propColorTier1Requested() {
    static int state = -1;
    if (state == -1) {
        // Umbrella members: MC2_VK_SHARED_GBUFFER=1 arms both; explicit
        // MC2_VK_PROP_COLOR_TIER1 / MC2_VK_SHARED_SCENE_DEPTH =0/1 override.
        const bool tier1 = mc2vkgate::subGate("MC2_VK_PROP_COLOR_TIER1");
        const bool depth = mc2vkgate::subGate("MC2_VK_SHARED_SCENE_DEPTH");
        state = (tier1 && depth) ? 1 : 0;  // require shared depth
    }
    return state == 1;
}

// VK-RUNG8-ITEM5: MC2_VK_PROP_DIRECT_CHAIN is RETIRED. Prop co-write is now
// unconditionally direct-chain-native (the GL<->VK semaphore-handoff arm + the
// shared-scene GL-import half are deleted). The gate is a parseable no-op: setting
// it explicitly prints one deprecation note and changes nothing.
bool propDirectChainRequested() {
    static bool s_noted = false;
    if (!s_noted) {
        s_noted = true;
        const char* v = std::getenv("MC2_VK_PROP_DIRECT_CHAIN");
        if (v && v[0])
            vlog("[VK_PROP_COLOR_TIER1] MC2_VK_PROP_DIRECT_CHAIN is deprecated "
                 "(prop is unconditionally direct-chain-native); ignoring explicit value.");
    }
    return true;
}

// VK-P11 STATIC-PROP MATERIAL: real lit textured props (albedo 2D_ARRAY sample +
// shared computePBR). Requires the Tier-1 gate (shared-GBuffer co-write). Default-OFF.
bool propMaterialRequested() {
    // Umbrella member: MC2_VK_SHARED_GBUFFER=1 arms it; explicit
    // MC2_VK_PROP_MATERIAL=0/1 overrides. (Precondition = propColorTier1Requested,
    // enforced at the call site.)
    static int state = -1;
    if (state == -1) state = mc2vkgate::subGate("MC2_VK_PROP_MATERIAL") ? 1 : 0;
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

// GpuStaticPropInstance mirror (112B, LOCKSTEP with gos_static_prop_batcher.h AND
// shaders/vulkan/color_prop_tier1.vert). We READ modelMatrix in the VS; the full stride
// must match so the memcpy'd POD indexes correctly.
struct GpuStaticPropInstanceMirror {
    float    modelMatrix[16];  // off 0
    uint32_t typeID;           // off 64
    uint32_t firstColorOffset; // off 68
    uint32_t flags;            // off 72
    uint32_t lightDataIndex;   // off 76
    float    aRGBHighlight[4]; // off 80
    float    fogRGB[4];        // off 96
};                             // 112 bytes
static_assert(sizeof(GpuStaticPropInstanceMirror) == 112, "instance mirror must be 112B");

// std140 per-frame UBO (LOCKSTEP with color_prop_tier1.vert / color_prop_tier1.frag
// ColorFrameUBO). VK-P11 STATIC-PROP MATERIAL extends the shared prop UBO with the
// terrain sun (surface->sun, MC2 world) + a materialReady flag so the Tier-1 FS
// samples the real albedo array + shared computePBR when armed, else keeps teal.
// The tail (sun/materialReady) is default-zero when material feeds are absent
// so disabled material paths remain byte-identical.
// VK-PROP-AMBIENT-IBL-1: extends the tail with the IBL-SH coefficient table (9 vec4,
// .w pad -- std140 array stride for vec3[] is 16B) + strength + a lightingReady flag
// (LightsData SSBO copied this frame -> ambientTerm valid). Material-disabled paths never read past
// its first 20 bytes so this extension is byte-identical for that path.
struct ColorFrameUBO {
    float   worldToClipGL[16]; // gos_GetObjectFixBMVPMat4() — GL_FALSE   (off 0)
    float   objectDepthBias;   //                                          (off 64)
    int32_t instBase;          //                                          (off 68)
    int32_t materialReady;     // 1 = sample alpha-OFF albedo array         (off 72)
    int32_t materialReadyOn;   // 1 = sample alpha-ON albedo array (a-test) (off 76)
    float   terrainSunMC2[4];  // surface->sun (MC2 world); .w pad          (off 80)
    // VK-PROP-AMBIENT-IBL-1 tail (off 96):
    float   iblSh[9][4];       // RenderCore SH-L2 coeffs, .w pad each      (off 96..236)
    float   iblShStrength;     // g_iblShStrength (0.0 = feature off)       (off 240)
    int32_t lightingReady;     // 1 = LightsData copied -> ambientTerm real (off 244)
    float   _pad[2];           // pad to 16B multiple                       (off 248..256)
};

const VkFormat kColorFmt = VK_FORMAT_R16G16B16A16_SFLOAT; // MainColor-compatible
const VkFormat kDepthFmt = VK_FORMAT_D32_SFLOAT;          // VK-owned (non-exported)

// Per-type packet (subset of GpuStaticPropPacket the draw loop needs).
struct TypeRun {
    uint32_t typeID;
    uint32_t firstPacket;
    uint32_t packetCount;
};

struct ColorPropIsland {
    bool disabled = false;
    bool inited   = false;
    bool useInterop = false;
    bool tier1    = false;   // TIER-1 in-place shared-GBuffer co-write

    // ---- TIER-1: shared-GBuffer co-write objects (built over the SAME shared
    // color/normal/depth images the GL scene renders terrain into). ----
    VkRenderPass  t1_rpass = VK_NULL_HANDLE;   // 2 color LOAD + depth LOAD
    VkFramebuffer t1_fb    = VK_NULL_HANDLE;   // over the 3 shared image views
    VkPipeline    t1_pipe  = VK_NULL_HANDLE;   // depthWrite ON GEQUAL, 2 outputs, no blend
    VkShaderModule t1_vert = VK_NULL_HANDLE;
    VkShaderModule t1_frag = VK_NULL_HANDLE;
    // Borrowed (NOT owned) shared-image resources/views, fetched per-frame from the
    // shared-scene-color/-depth registries. Cached at init for the framebuffer build.
    struct VkImageResource* t1_colorRes  = nullptr;
    struct VkImageResource* t1_normalRes = nullptr;
    struct VkImageResource* t1_depthRes  = nullptr;
    // VK-RUNG8-ITEM5: shared scene is VK-only (no GL interop view). Prop co-writes
    // direct-chain-native only; the former t1_*View handoff handles are deleted.

    int w = 0, h = 0;

    // Borrowed from the shared engine context (NOT owned).
    VkInstance       instance  = VK_NULL_HANDLE;
    VkPhysicalDevice phys      = VK_NULL_HANDLE;
    VkDevice         device    = VK_NULL_HANDLE;
    uint32_t         gfxFamily = UINT32_MAX;
    VkQueue          queue     = VK_NULL_HANDLE;
    VmaAllocator     allocator = VK_NULL_HANDLE;

    // Caster/color geometry (VK-owned copies of the shared static-prop VBO/IBO).
    VkBuffer      vbo = VK_NULL_HANDLE; VmaAllocation vboAlloc = VK_NULL_HANDLE;
    VkBuffer      ibo = VK_NULL_HANDLE; VmaAllocation iboAlloc = VK_NULL_HANDLE;
    uint32_t      vertexStride = 0;
    bool          geomUploaded = false;

    std::vector<Mc2ShadowPacket> packets;
    std::vector<TypeRun>         typeRuns;
    // VK-FLIP-FIELD-REPORT-2 BLACK-FENCE FIX: per-packet (not per-type) albedo layer,
    // group-relative (same array as typeAlbedoLayer/typeAlbedoLayerOn). Indexed by
    // global packet index (same indexing as `packets` above). -1 = no texture.
    std::vector<int32_t>        packetLayerOff;
    std::vector<int32_t>        packetLayerOn;
    // FENCE-UVSCALE-1: per-packet UV sub-region scale, same indexing as
    // packetLayerOff/On above. Default 1.0 (no crop) until the feed populates
    // it -- matches GL's PerDrawEntry.uvScaleX/Y default (gos_static_prop_batcher.cpp
    // uvScaleXByPacket/uvScaleYByPacket init).
    std::vector<float>          packetUvScaleX;
    std::vector<float>          packetUvScaleY;

    VkBuffer      instBuf = VK_NULL_HANDLE; VmaAllocation instAlloc = VK_NULL_HANDLE;
    VkDeviceSize  instCapBytes = 0;
    VkBuffer      ubo = VK_NULL_HANDLE; VmaAllocation uboAlloc = VK_NULL_HANDLE;

    // VK-PROP-AMBIENT-IBL-1: per-frame copy of the live GL LightsData SSBO (binding 4,
    // Tier-1 only) so the vert shader's calc_light-style ambient sum reads the same
    // ObjectLights[] the GL prop VS does. wantLighting = tier1 (no separate gate --
    // this is a parity FIX, not a new opt-in feature).
    bool          wantLighting  = false;
    bool          lightingReady = false;   // LightsData copied this frame
    VkBuffer      lightBuf = VK_NULL_HANDLE; VmaAllocation lightAlloc = VK_NULL_HANDLE;
    VkDeviceSize  lightCap = 0;

    // VK-P11 STATIC-PROP MATERIAL: copy-once albedo 2D_ARRAY import + per-type layer
    // table + terrain sun. All optional (Tier-1 + MC2_VK_PROP_MATERIAL); fail-soft to
    // teal when unavailable (materialReady stays false).
    bool          wantMaterial   = false;   // MC2_VK_PROP_MATERIAL armed
    bool          materialReady  = false;   // albedo array imported this map
    uint32_t      albedoGlTexImported = 0;  // GL name last imported (re-import guard)
    VkImage       albedoImg   = VK_NULL_HANDLE; VmaAllocation albedoAlloc = VK_NULL_HANDLE;
    VkImageView   albedoView  = VK_NULL_HANDLE;
    VkSampler     albedoSamp  = VK_NULL_HANDLE;
    uint32_t      albedoLayers = 0;
    VkBuffer      layerBuf = VK_NULL_HANDLE; VmaAllocation layerAlloc = VK_NULL_HANDLE; // per-type int layer SSBO
    VkDeviceSize  layerCapBytes = 0;
    float         terrainSunMC2[3] = {0.0f, 0.0f, 1.0f};

    // VK-P11 STATIC-PROP MATERIAL alpha-ON (class 1): second copy-once array import +
    // second per-type layer SSBO. Class-1 types (alpha-MIXED buildings) sample this
    // array with a 0.5 alpha-test discard (matches GL static_prop.frag). Fail-soft to
    // teal when unavailable (materialReadyOn stays false).
    bool          materialReadyOn = false;  // alpha-ON array imported this map
    uint32_t      albedoOnGlTexImported = 0;
    VkImage       albedoOnImg  = VK_NULL_HANDLE; VmaAllocation albedoOnAlloc = VK_NULL_HANDLE;
    VkImageView   albedoOnView = VK_NULL_HANDLE;
    VkSampler     albedoOnSamp = VK_NULL_HANDLE;
    uint32_t      albedoOnLayers = 0;
    VkBuffer      layerOnBuf = VK_NULL_HANDLE; VmaAllocation layerOnAlloc = VK_NULL_HANDLE; // per-type int layer SSBO (class 1)
    VkDeviceSize  layerOnCapBytes = 0;

    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkDescriptorPool  pool   = VK_NULL_HANDLE;
    VkDescriptorSet   set    = VK_NULL_HANDLE;
    VkPipelineLayout  playout = VK_NULL_HANDLE;

    VkCommandPool     cpool  = VK_NULL_HANDLE;
    VkCommandBuffer   cbuf[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};

};

ColorPropIsland* g_island = nullptr;

bool make_buffer(ColorPropIsland* s, VkDeviceSize size, VkBufferUsageFlags usage,
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

void write_descriptors(ColorPropIsland* s) {
    if (s->set == VK_NULL_HANDLE) return;
    VkDescriptorBufferInfo uboInfo{ s->ubo, 0, VK_WHOLE_SIZE };
    VkDescriptorBufferInfo instInfo{ s->instBuf, 0, VK_WHOLE_SIZE };
    VkDescriptorBufferInfo lightInfo{};
    VkWriteDescriptorSet w[5]{};
    uint32_t n = 0;
    w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n].dstSet = s->set;
    w[n].dstBinding = 0; w[n].descriptorCount = 1;
    w[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[n].pBufferInfo = &uboInfo; ++n;
    w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n].dstSet = s->set;
    w[n].dstBinding = 1; w[n].descriptorCount = 1;
    w[n].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[n].pBufferInfo = &instInfo; ++n;
    // VK-PROP-AMBIENT-IBL-1: binding4 LightsData SSBO (Tier-1 only, valid dummy-or-real
    // buffer required whenever the DSL declared it -- see wantLighting bindingCount).
    if (s->tier1 && s->wantLighting && s->lightBuf != VK_NULL_HANDLE) {
        lightInfo = VkDescriptorBufferInfo{ s->lightBuf, 0, VK_WHOLE_SIZE };
        w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n].dstSet = s->set;
        w[n].dstBinding = 4; w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[n].pBufferInfo = &lightInfo; ++n;
    }
    // VK-P11 STATIC-PROP MATERIAL: binding2 layer SSBO (VS) + binding3 albedo array (FS).
    // Only written when the layout declared them (wantMaterial) AND the objects exist
    // (dummy or real). A valid descriptor is required for binding3 even pre-import.
    VkDescriptorBufferInfo layerInfo{};
    VkDescriptorImageInfo  albInfo{};
    VkDescriptorBufferInfo layerOnInfo{};
    VkDescriptorImageInfo  albOnInfo{};
    VkWriteDescriptorSet   wm[4]{};   // alpha-OFF (2/3) + alpha-ON (5/6) material writes
    uint32_t m = 0;
    if (s->wantMaterial && s->tier1 && s->layerBuf != VK_NULL_HANDLE &&
        s->albedoView != VK_NULL_HANDLE && s->albedoSamp != VK_NULL_HANDLE &&
        s->layerOnBuf != VK_NULL_HANDLE &&
        s->albedoOnView != VK_NULL_HANDLE && s->albedoOnSamp != VK_NULL_HANDLE) {
        layerInfo = VkDescriptorBufferInfo{ s->layerBuf, 0, VK_WHOLE_SIZE };
        wm[m].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wm[m].dstSet = s->set;
        wm[m].dstBinding = 2; wm[m].descriptorCount = 1;
        wm[m].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wm[m].pBufferInfo = &layerInfo; ++m;
        albInfo.sampler = s->albedoSamp; albInfo.imageView = s->albedoView;
        albInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        wm[m].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wm[m].dstSet = s->set;
        wm[m].dstBinding = 3; wm[m].descriptorCount = 1;
        wm[m].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wm[m].pImageInfo = &albInfo; ++m;
        // alpha-ON group: binding5 layer SSBO (VS) + binding6 albedo array (FS).
        layerOnInfo = VkDescriptorBufferInfo{ s->layerOnBuf, 0, VK_WHOLE_SIZE };
        wm[m].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wm[m].dstSet = s->set;
        wm[m].dstBinding = 5; wm[m].descriptorCount = 1;
        wm[m].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wm[m].pBufferInfo = &layerOnInfo; ++m;
        albOnInfo.sampler = s->albedoOnSamp; albOnInfo.imageView = s->albedoOnView;
        albOnInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        wm[m].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wm[m].dstSet = s->set;
        wm[m].dstBinding = 6; wm[m].descriptorCount = 1;
        wm[m].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wm[m].pImageInfo = &albOnInfo; ++m;
    }
    if (n) vkUpdateDescriptorSets(s->device, n, w, 0, nullptr);
    if (m) vkUpdateDescriptorSets(s->device, m, wm, 0, nullptr);
}

// VK-PROP-AMBIENT-IBL-1: grow-once light-copy buffer (mirrors ensure_inst_capacity's
// shape; separate because it needs no write_descriptors rebind tie-in until the
// caller decides -- the binding4 descriptor is written once at init from the dummy
// size, same convention as the mech island's lightBuf).
bool ensure_light_capacity(ColorPropIsland* s, VkDeviceSize bytes) {
    if (s->lightBuf != VK_NULL_HANDLE && bytes <= s->lightCap) return true;
    if (s->lightBuf != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(s->device);
        vmaDestroyBuffer(s->allocator, s->lightBuf, s->lightAlloc);
        s->lightBuf = VK_NULL_HANDLE;
    }
    VkDeviceSize cap = bytes + bytes / 2 + 4096;
    if (!make_buffer(s, cap, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &s->lightBuf, &s->lightAlloc, true))
        return false;
    s->lightCap = cap;
    write_descriptors(s);
    return true;
}

bool ensure_inst_capacity(ColorPropIsland* s, VkDeviceSize bytes) {
    if (s->instBuf != VK_NULL_HANDLE && bytes <= s->instCapBytes) return true;
    if (s->instBuf != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(s->device);
        vmaDestroyBuffer(s->allocator, s->instBuf, s->instAlloc);
        s->instBuf = VK_NULL_HANDLE;
    }
    VkDeviceSize cap = bytes + bytes / 2 + 4096;
    if (!make_buffer(s, cap, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &s->instBuf, &s->instAlloc, true))
        return false;
    s->instCapBytes = cap;
    write_descriptors(s);
    return true;
}

void destroy_island(ColorPropIsland* s) {
    if (!s) return;
    VkDevice d = s->device;
    if (d != VK_NULL_HANDLE) vkDeviceWaitIdle(d);
    // TIER-1 objects (own the render pass/framebuffer/pipeline/shaders; the shared
    // color/normal/depth images are BORROWED — owned by the postprocess registries).
    if (s->t1_pipe)  vkDestroyPipeline(d, s->t1_pipe, nullptr);
    if (s->t1_vert)  vkDestroyShaderModule(d, s->t1_vert, nullptr);
    if (s->t1_frag)  vkDestroyShaderModule(d, s->t1_frag, nullptr);
    if (s->t1_fb)    vkDestroyFramebuffer(d, s->t1_fb, nullptr);
    if (s->t1_rpass) vkDestroyRenderPass(d, s->t1_rpass, nullptr);
    if (s->playout) vkDestroyPipelineLayout(d, s->playout, nullptr);
    if (s->dsl)     vkDestroyDescriptorSetLayout(d, s->dsl, nullptr);
    if (s->pool)    vkDestroyDescriptorPool(d, s->pool, nullptr);
    if (s->vbo)     vmaDestroyBuffer(s->allocator, s->vbo, s->vboAlloc);
    if (s->ibo)     vmaDestroyBuffer(s->allocator, s->ibo, s->iboAlloc);
    if (s->instBuf) vmaDestroyBuffer(s->allocator, s->instBuf, s->instAlloc);
    if (s->ubo)     vmaDestroyBuffer(s->allocator, s->ubo, s->uboAlloc);
    if (s->lightBuf) vmaDestroyBuffer(s->allocator, s->lightBuf, s->lightAlloc);  // VK-PROP-AMBIENT-IBL-1
    // VK-P11 STATIC-PROP MATERIAL: copy-once albedo array + per-type layer SSBO.
    if (s->albedoView)  vkDestroyImageView(d, s->albedoView, nullptr);
    if (s->albedoImg)   vmaDestroyImage(s->allocator, s->albedoImg, s->albedoAlloc);
    if (s->albedoSamp)  vkDestroySampler(d, s->albedoSamp, nullptr);
    if (s->layerBuf)    vmaDestroyBuffer(s->allocator, s->layerBuf, s->layerAlloc);
    // alpha-ON group.
    if (s->albedoOnView)  vkDestroyImageView(d, s->albedoOnView, nullptr);
    if (s->albedoOnImg)   vmaDestroyImage(s->allocator, s->albedoOnImg, s->albedoOnAlloc);
    if (s->albedoOnSamp)  vkDestroySampler(d, s->albedoOnSamp, nullptr);
    if (s->layerOnBuf)    vmaDestroyBuffer(s->allocator, s->layerOnBuf, s->layerOnAlloc);
    if (s->cpool)   vkDestroyCommandPool(d, s->cpool, nullptr);
    // instance/phys/device/queue/allocator are BORROWED — never destroyed here.
}

// VK-P11 STATIC-PROP MATERIAL ------------------------------------------------
// Copy-once import of the GL alpha-OFF albedo GL_TEXTURE_2D_ARRAY into a VK sampled
// array image (all layers, full mips), + a LINEAR_MIPMAP_LINEAR / CLAMP_TO_EDGE
// sampler matching the GL static-prop array. Generalizes the terrain island's
// single-layer/single-mip upload_sampled_image to arrayLayers=N + mipLevels=M:
// glGetTexImage(GL_TEXTURE_2D_ARRAY) / glGetCompressedTexImage return ALL layers of a
// level contiguously, so one vkCmdCopyBufferToImage per mip with layerCount=N copies
// the whole level. This is a ONCE-PER-MAP material import (like the terrain colormap),
// NOT the per-frame zero-copy color surface — so it does NOT touch g_forbidden.

VkCommandBuffer cp_begin_oneshot(ColorPropIsland* s) {
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
void cp_end_oneshot(ColorPropIsland* s, VkCommandBuffer cb) {
    vkEndCommandBuffer(cb);
    VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    vkQueueSubmit(s->queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(s->queue);
    vkFreeCommandBuffers(s->device, s->cpool, 1, &cb);
}

// VK-P11 STATIC-PROP MATERIAL: a single array-import target (alpha-OFF or alpha-ON).
// Both albedo groups share the same import code via this indirection.
struct AlbedoTarget {
    VkImage*       img;
    VmaAllocation* alloc;
    VkImageView*   view;
    VkSampler*     samp;
    uint32_t*      layers;
    uint32_t*      importedGlName;
    bool*          ready;
    const char*    tag;   // "alpha-OFF" / "alpha-ON" for logs
};
AlbedoTarget cp_target_off(ColorPropIsland* s) {
    return AlbedoTarget{ &s->albedoImg, &s->albedoAlloc, &s->albedoView, &s->albedoSamp,
                         &s->albedoLayers, &s->albedoGlTexImported, &s->materialReady, "alpha-OFF" };
}
AlbedoTarget cp_target_on(ColorPropIsland* s) {
    return AlbedoTarget{ &s->albedoOnImg, &s->albedoOnAlloc, &s->albedoOnView, &s->albedoOnSamp,
                         &s->albedoOnLayers, &s->albedoOnGlTexImported, &s->materialReadyOn, "alpha-ON" };
}

bool cp_make_albedo_sampler(ColorPropIsland* s, VkSampler* samp, uint32_t mipLevels) {
    if (*samp != VK_NULL_HANDLE) return true;
    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;           // LINEAR_MIPMAP_LINEAR
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = (mipLevels > 0) ? (float)(mipLevels - 1) : 0.0f;
    return vkCreateSampler(s->device, &sci, nullptr, samp) == VK_SUCCESS;
}

// Build a VK sampled 2D_ARRAY image from a contiguous byte blob laid out
// mip-major then layer-major-per-mip is NOT how GL returns it; GL returns, per mip
// level, all layers contiguously. `regions` carries one VkBufferImageCopy per mip
// (layerCount = layers, correct bufferOffset). Caller owns `bytes`.
bool cp_upload_albedo_array(ColorPropIsland* s, const AlbedoTarget& tgt,
                            VkFormat fmt, uint32_t w, uint32_t h,
                            uint32_t layers, uint32_t mips, bool compressed,
                            const void* bytes, VkDeviceSize byteCount,
                            const std::vector<VkBufferImageCopy>& regions) {
    if (*tgt.view) { vkDestroyImageView(s->device, *tgt.view, nullptr); *tgt.view = VK_NULL_HANDLE; }
    if (*tgt.img)  { vmaDestroyImage(s->allocator, *tgt.img, *tgt.alloc); *tgt.img = VK_NULL_HANDLE; }
    (void)compressed;

    VkBuffer stg = VK_NULL_HANDLE; VmaAllocation stgAlloc = VK_NULL_HANDLE;
    if (!make_buffer(s, byteCount, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &stg, &stgAlloc, true)) return false;
    { void* p = nullptr;
      if (vmaMapMemory(s->allocator, stgAlloc, &p) != VK_SUCCESS) { vmaDestroyBuffer(s->allocator, stg, stgAlloc); return false; }
      std::memcpy(p, bytes, (size_t)byteCount); vmaUnmapMemory(s->allocator, stgAlloc); }

    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D; ici.format = fmt;
    ici.extent = {w, h, 1}; ici.mipLevels = mips; ici.arrayLayers = layers;
    ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo aci{}; aci.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateImage(s->allocator, &ici, &aci, tgt.img, tgt.alloc, nullptr) != VK_SUCCESS) {
        vmaDestroyBuffer(s->allocator, stg, stgAlloc); return false;
    }

    VkCommandBuffer cb = cp_begin_oneshot(s);
    if (cb == VK_NULL_HANDLE) { vmaDestroyBuffer(s->allocator, stg, stgAlloc); return false; }
    VkImageMemoryBarrier bar{};
    bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; bar.image = *tgt.img;
    bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, layers};
    bar.srcAccessMask = 0; bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &bar);
    vkCmdCopyBufferToImage(cb, stg, *tgt.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           (uint32_t)regions.size(), regions.data());
    bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &bar);
    cp_end_oneshot(s, cb);
    vmaDestroyBuffer(s->allocator, stg, stgAlloc);

    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = *tgt.img; vci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY; vci.format = fmt;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, layers};
    if (vkCreateImageView(s->device, &vci, nullptr, tgt.view) != VK_SUCCESS) return false;
    *tgt.layers = layers;
    return true;
}

// Bind a 1x1 opaque-magenta placeholder array so binding3 is a VALID descriptor even
// when the real albedo import fails/isn't ready. materialReady stays false -> the FS
// keeps flat teal (never samples the dummy).
bool cp_ensure_dummy_albedo(ColorPropIsland* s, const AlbedoTarget& tgt) {
    if (!cp_make_albedo_sampler(s, tgt.samp, 1)) return false;
    if (*tgt.view != VK_NULL_HANDLE) return true;
    const uint8_t px[4] = {255, 0, 255, 255};
    std::vector<VkBufferImageCopy> rgn(1);
    rgn[0].bufferOffset = 0;
    rgn[0].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rgn[0].imageExtent = {1, 1, 1};
    return cp_upload_albedo_array(s, tgt, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 1, false,
                                  px, sizeof(px), rgn);
}

// Copy-once import the GL alpha-OFF albedo 2D_ARRAY. Re-imports only when the GL tex
// name changes. Sets s->materialReady on success. Fail-soft: leaves the dummy bound
// (materialReady=false -> FS keeps teal). Returns true if a NEW real import landed.
bool cp_import_albedo_array(ColorPropIsland* s, const AlbedoTarget& tgt, uint32_t glTexArray) {
    const uint32_t glTexArrayOff = glTexArray;
    if (glTexArrayOff == 0) return false;
    if (glTexArrayOff == *tgt.importedGlName && *tgt.ready) return false; // already

    glBindTexture(GL_TEXTURE_2D_ARRAY, (GLuint)glTexArrayOff);
    GLint w = 0, h = 0, layers = 0, internalFmt = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_WIDTH, &w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_HEIGHT, &h);
    glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_DEPTH, &layers);   // array layer count
    glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_INTERNAL_FORMAT, &internalFmt);
    if (w <= 0 || h <= 0 || layers <= 0) { glBindTexture(GL_TEXTURE_2D_ARRAY, 0); return false; }

    // Full mip chain (match GL: LINEAR_MIPMAP_LINEAR needs the levels present).
    uint32_t mips = 1;
    { uint32_t mw = (uint32_t)w, mh = (uint32_t)h;
      while (mw > 1 || mh > 1) { mw = mw > 1 ? mw >> 1 : 1; mh = mh > 1 ? mh >> 1 : 1; ++mips; } }
    // Only trust mips that actually exist on the GL texture (a non-mipped array
    // reports level-0 only). Probe level 1; if absent, single mip.
    { GLint w1 = 0; glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 1, GL_TEXTURE_WIDTH, &w1);
      if (w1 <= 0) mips = 1; }

    // ADJUDICATION-1 FENCE-RECON: one-shot mip-count-parity verdict. GL's own
    // GL_TEXTURE_MAX_LEVEL (set at gos_static_prop_batcher.cpp:3094 from the same
    // w/h halving loop) vs this import's computed `mips` vs the sampler's maxLod
    // that will be created below. Mismatch here would mean VK samples a mip level
    // GL's own state clamps away (or vice versa).
    if (std::getenv("MC2_VK_PROP_MIP_PROBE")) {
        GLint glMaxLevel = -1;
        glGetTexParameteriv(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, &glMaxLevel);
        std::fprintf(stderr,
            "[PROP_MIP_PROBE v1] tag=%s glTexArray=%u w=%d h=%d layers=%d "
            "glTexMaxLevel=%d vkComputedMips=%u vkSamplerMaxLod=%.1f\n",
            tgt.tag, glTexArrayOff, (int)w, (int)h, (int)layers,
            (int)glMaxLevel, mips, (mips > 0) ? (float)(mips - 1) : 0.0f);
        std::fflush(stderr);
    }

    const bool bc7 = (internalFmt == GL_COMPRESSED_RGBA_BPTC_UNORM ||
                      internalFmt == GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM);
    const VkFormat vkFmt = bc7
        ? ((internalFmt == GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM) ? VK_FORMAT_BC7_SRGB_BLOCK
                                                                : VK_FORMAT_BC7_UNORM_BLOCK)
        : VK_FORMAT_R8G8B8A8_UNORM;

    // Gather every mip level (all layers contiguous per level) into one blob + regions.
    std::vector<uint8_t> blob;
    std::vector<VkBufferImageCopy> regions;
    regions.reserve(mips);
    bool ok = true;
    for (uint32_t m = 0; m < mips && ok; ++m) {
        GLint lw = 0, lh = 0;
        glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, (GLint)m, GL_TEXTURE_WIDTH, &lw);
        glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, (GLint)m, GL_TEXTURE_HEIGHT, &lh);
        if (lw <= 0 || lh <= 0) { mips = m; break; }
        const VkDeviceSize off = blob.size();
        if (bc7) {
            GLint compBytes = 0;
            glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, (GLint)m, GL_TEXTURE_COMPRESSED_IMAGE_SIZE, &compBytes);
            if (compBytes <= 0) { ok = false; break; }  // all layers this level
            blob.resize((size_t)off + (size_t)compBytes);
            glGetCompressedTexImage(GL_TEXTURE_2D_ARRAY, (GLint)m, blob.data() + off);
        } else {
            const size_t bytes = (size_t)lw * (size_t)lh * (size_t)layers * 4u;
            blob.resize(off + bytes);
            glGetTexImage(GL_TEXTURE_2D_ARRAY, (GLint)m, GL_RGBA, GL_UNSIGNED_BYTE, blob.data() + off);
        }
        VkBufferImageCopy r{};
        r.bufferOffset = off;
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, (uint32_t)layers};
        r.imageExtent = {(uint32_t)lw, (uint32_t)lh, 1};
        regions.push_back(r);
    }
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    if (!ok || regions.empty()) return false;

    // (Re)create sampler with the real mip count.
    if (*tgt.samp) { vkDestroySampler(s->device, *tgt.samp, nullptr); *tgt.samp = VK_NULL_HANDLE; }
    if (!cp_make_albedo_sampler(s, tgt.samp, mips)) return false;

    if (!cp_upload_albedo_array(s, tgt, vkFmt, (uint32_t)w, (uint32_t)h, (uint32_t)layers,
                                mips, bc7, blob.data(), (VkDeviceSize)blob.size(), regions)) {
        return false;
    }
    *tgt.importedGlName = glTexArrayOff;
    *tgt.ready = true;
    vlog("%s albedo 2D_ARRAY imported: %dx%d layers=%d mips=%u fmt=%s (%llu B).",
         tgt.tag, w, h, layers, mips, bc7 ? "BC7" : "RGBA8", (unsigned long long)blob.size());
    return true;
}

// Upload the per-type albedo-layer table (int32 per type) into a layer SSBO
// (alpha-OFF binding2, or alpha-ON binding5).
bool cp_upload_layer_table_into(ColorPropIsland* s, VkBuffer* buf, VmaAllocation* alloc,
                                VkDeviceSize* capBytes, const int32_t* layers, uint32_t count) {
    const VkDeviceSize bytes = (VkDeviceSize)(count ? count : 1) * sizeof(int32_t);
    if (*buf != VK_NULL_HANDLE && bytes > *capBytes) {
        vkDeviceWaitIdle(s->device);
        vmaDestroyBuffer(s->allocator, *buf, *alloc);
        *buf = VK_NULL_HANDLE;
    }
    if (*buf == VK_NULL_HANDLE) {
        if (!make_buffer(s, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, buf, alloc, true))
            return false;
        *capBytes = bytes;
    }
    void* p = nullptr;
    if (vmaMapMemory(s->allocator, *alloc, &p) != VK_SUCCESS) return false;
    if (count && layers) std::memcpy(p, layers, (size_t)count * sizeof(int32_t));
    else { int32_t neg = -1; std::memcpy(p, &neg, sizeof(neg)); }
    vmaUnmapMemory(s->allocator, *alloc);
    return true;
}
bool cp_upload_layer_table(ColorPropIsland* s, const int32_t* layers, uint32_t count) {
    return cp_upload_layer_table_into(s, &s->layerBuf, &s->layerAlloc, &s->layerCapBytes, layers, count);
}
bool cp_upload_layer_table_on(ColorPropIsland* s, const int32_t* layers, uint32_t count) {
    return cp_upload_layer_table_into(s, &s->layerOnBuf, &s->layerOnAlloc, &s->layerOnCapBytes, layers, count);
}

// One-time init.
bool init_island(ColorPropIsland* s, int w, int h) {
    s->w = w; s->h = h;
    ++g_health.attempted;

    if (!mc2vk::ensureEngineVulkanContextForInterop()) {
        setFallback("engine_vk_context_birth_failed"); return false;
    }
    mc2vk::VulkanContext& ctx = mc2vk::getOrCreateEngineVulkanContext();
    if (!ctx.valid()) { setFallback("engine_vk_context_not_valid"); return false; }
    s->instance  = ctx.instance();
    s->phys      = ctx.physicalDevice();
    s->device    = ctx.device();
    s->queue     = ctx.graphicsQueue();
    s->gfxFamily = ctx.graphicsFamily();
    s->allocator = ctx.allocator();
    if (s->device == VK_NULL_HANDLE || s->allocator == VK_NULL_HANDLE) {
        setFallback("engine_vk_device_or_allocator_null"); return false;
    }
    g_health.vulkanAvailable = 1;
    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(s->phys, &props);
        std::strncpy(g_health.deviceName, props.deviceName, sizeof(g_health.deviceName) - 1);
        g_health.deviceName[sizeof(g_health.deviceName) - 1] = '\0';
    }

    // Tier-1 is the only live Vulkan prop color path.

    // VK-RUNG8-ITEM5: fire the one-shot MC2_VK_PROP_DIRECT_CHAIN deprecation note if
    // the retired gate was explicitly set (result ignored — always direct-chain-native).
    (void)propDirectChainRequested();

    s->tier1 = propColorTier1Requested();
    // VK-P11 STATIC-PROP MATERIAL requires the Tier-1 shared-GBuffer co-write path.
    s->wantMaterial = s->tier1 && propMaterialRequested();
    // VK-PROP-AMBIENT-IBL-1: real ambient/IBL feed also requires Tier-1.
    // Not a separate opt-in gate --
    // this closes a documented parity gap in the same tier the washout fix landed in.
    s->wantLighting = s->tier1;
    if (!s->tier1) { setFallback("gate_off"); return false; }

    if (s->tier1) {
        // ---- TIER-1: fetch the SHARED scene color/normal/depth exportable images the
        // GL scene renders terrain into (built by the postprocess seams; the island
        // co-writes props into these SAME images in-place). Fail-soft: any missing =>
        // GL fallback (the caller runs the GL prop flush). ----
        s->t1_colorRes  = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_COLOR0);
        s->t1_normalRes = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_NORMAL);
        s->t1_depthRes  = mc2SharedSceneDepthGetResource();
        // VK-RUNG8-ITEM5: direct-chain-native only. The shared GBuffer is VK-only and
        // left Vulkan-owned by the terrain first-owner; only the VkImageResources are
        // required (no GL interop view, no GL<->VK semaphore handoff).
        if (!s->t1_colorRes || !s->t1_normalRes || !s->t1_depthRes) {
            setFallback("tier1_shared_gbuffer_not_ready");
            return false;
        }
        s->useInterop = false;
        g_health.interopActive = 0;
        g_health.vk2glSemActive = 0;
        vlog("TIER-1 shared-GBuffer co-write ACTIVE — in-place RGBA16F color(MRT0)+normal(MRT1) "
             "+ shared D32S8 depth (direct-chain native, no interop).");
    }
    // ---- Instance SSBO + UBO (common to both tiers). ----
    if (!ensure_inst_capacity(s, 256 * 1024)) return false;
    if (!make_buffer(s, sizeof(ColorFrameUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &s->ubo, &s->uboAlloc, true)) return false;

    if (s->tier1) {
        VkImageResource* colorRes  = reinterpret_cast<VkImageResource*>(s->t1_colorRes);
        VkImageResource* normalRes = reinterpret_cast<VkImageResource*>(s->t1_normalRes);
        VkImageResource* depthRes  = reinterpret_cast<VkImageResource*>(s->t1_depthRes);

        VkAttachmentDescription att[3]{};
        // color0 (MRT0) — LOAD GL terrain color, STORE result.
        att[0].format = colorRes->format; att[0].samples = VK_SAMPLE_COUNT_1_BIT;
        att[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att[0].finalLayout   = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        // color1 (MRT1 GBuffer1) — LOAD GL terrain normals (respect clearGBuffer1), STORE.
        att[1].format = normalRes->format; att[1].samples = VK_SAMPLE_COUNT_1_BIT;
        att[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[1].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att[1].finalLayout   = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        // depth (shared D32S8) — LOAD GL scene depth, TEST+WRITE (GEQUAL), STORE so GL
        // (and post passes) see the prop-updated depth. Stencil LOAD/STORE preserved.
        att[2].format = depthRes->format; att[2].samples = VK_SAMPLE_COUNT_1_BIT;
        att[2].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
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

        // Cross-submit WAW/hand-off deps shape (GL semaphore provides
        // the actual ordering; these let sync-val see the LOAD-preserve co-write).
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
        if (vkCreateRenderPass(s->device, &rpci, nullptr, &s->t1_rpass) != VK_SUCCESS) {
            vlog("TIER-1 vkCreateRenderPass failed."); setFallback("tier1_renderpass_failed"); return false;
        }
        VkImageView fbViews[3] = { colorRes->view, normalRes->view, depthRes->view };
        VkFramebufferCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = s->t1_rpass; fci.attachmentCount = 3; fci.pAttachments = fbViews;
        fci.width = (uint32_t)w; fci.height = (uint32_t)h; fci.layers = 1;
        if (vkCreateFramebuffer(s->device, &fci, nullptr, &s->t1_fb) != VK_SUCCESS) {
            vlog("TIER-1 vkCreateFramebuffer failed."); setFallback("tier1_framebuffer_failed"); return false;
        }
    }

    // ---- Descriptor layout/pool/set (UBO binding0 VS, instance SSBO binding1 VS).
    // VK-P11 STATIC-PROP MATERIAL (tier1 only): binding2 = per-type albedo-layer SSBO
    // (VS), binding3 = albedo sampler2DArray (FS). Declared on the tier1 layout even
    // when material isn't ready (dummy layer SSBO + dummy 1x1 array bound) so the
    // Tier-1 shaders can always reference them; the shader gates real sampling on
    // uFrame.materialReady. base descriptor set keeps exactly 2 bindings when optional feeds are off. ----
    const bool materialBindings = s->tier1 && s->wantMaterial;
    // VK-PROP-AMBIENT-IBL-1: binding4 LightsData SSBO, declared whenever tier1 (not
    // gated on wantMaterial -- ambient/IBL parity is independent of the albedo import).
    const bool lightBinding = s->tier1 && s->wantLighting;
    {
        // Base bindings 0(UBO),1(inst SSBO); optional +4(LightsData SSBO, VS). Material
        // (tier1 + MC2_VK_PROP_MATERIAL): +2(alpha-OFF layer SSBO VS), 3(alpha-OFF albedo
        // FS), 5(alpha-ON layer SSBO VS), 6(alpha-ON albedo FS).
        VkDescriptorSetLayoutBinding b[7]{};
        uint32_t nb = 0;
        b[nb].binding = 0; b[nb].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        b[nb].descriptorCount = 1; b[nb].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT; ++nb;
        b[nb].binding = 1; b[nb].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[nb].descriptorCount = 1; b[nb].stageFlags = VK_SHADER_STAGE_VERTEX_BIT; ++nb;
        if (materialBindings) {
            b[nb].binding = 2; b[nb].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            b[nb].descriptorCount = 1; b[nb].stageFlags = VK_SHADER_STAGE_VERTEX_BIT; ++nb;
            b[nb].binding = 3; b[nb].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[nb].descriptorCount = 1; b[nb].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; ++nb;
        }
        if (lightBinding) {
            b[nb].binding = 4; b[nb].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            b[nb].descriptorCount = 1; b[nb].stageFlags = VK_SHADER_STAGE_VERTEX_BIT; ++nb;
        }
        if (materialBindings) {
            b[nb].binding = 5; b[nb].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            b[nb].descriptorCount = 1; b[nb].stageFlags = VK_SHADER_STAGE_VERTEX_BIT; ++nb;
            b[nb].binding = 6; b[nb].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[nb].descriptorCount = 1; b[nb].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; ++nb;
        }
        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.bindingCount = nb; lci.pBindings = b;
        if (vkCreateDescriptorSetLayout(s->device, &lci, nullptr, &s->dsl) != VK_SUCCESS) {
            vlog("vkCreateDescriptorSetLayout failed."); return false;
        }
        VkDescriptorPoolSize sizes[3]{};
        sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; sizes[0].descriptorCount = 1;
        sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        sizes[1].descriptorCount = 1u + (materialBindings ? 2u : 0u) + (lightBinding ? 1u : 0u);
        sizes[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; sizes[2].descriptorCount = materialBindings ? 2u : 1u;
        VkDescriptorPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.maxSets = 1; pci.poolSizeCount = materialBindings ? 3u : 2u; pci.pPoolSizes = sizes;
        if (vkCreateDescriptorPool(s->device, &pci, nullptr, &s->pool) != VK_SUCCESS) {
            vlog("vkCreateDescriptorPool failed."); return false;
        }
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = s->pool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &s->dsl;
        if (vkAllocateDescriptorSets(s->device, &dsai, &s->set) != VK_SUCCESS) {
            vlog("vkAllocateDescriptorSets failed."); return false;
        }
        // VK-PROP-AMBIENT-IBL-1: light buffer must exist BEFORE write_descriptors below
        // (binding4 needs a valid buffer object). Dummy-sized (grows on first real copy).
        if (lightBinding) {
            if (!ensure_light_capacity(s, 3600)) { setFallback("light_buf_failed"); return false; }
        }
        write_descriptors(s);
    }

    // ---- TIER-1 Pipeline: same 40B vertex stream, but emits TWO GBuffer outputs
    // (loc0 lit color, loc1 GBuffer1 normal+0.25), reverse-Z GEQUAL depth-test+WRITE,
    // cull NONE, and NO blend on either attachment (opaque in-place overwrite — a
    // winning prop fragment fully replaces the terrain GBuffer pixel, matching GL). ----
    if (s->tier1) {
        s->t1_vert = load_spv(s->device, spv_dir() + "color_prop_tier1.vert.spv");
        s->t1_frag = load_spv(s->device, spv_dir() + "color_prop_tier1.frag.spv");
        if (s->t1_vert == VK_NULL_HANDLE || s->t1_frag == VK_NULL_HANDLE) { setFallback("tier1_spv_load_failed"); return false; }

        // VK-FLIP-FIELD-REPORT-2 BLACK-FENCE FIX: per-PACKET albedo layer push constant
        // (2x int32 = layerOff, layerOn). GL's coalesce path samples texArrayLayer per
        // DRAW (PerDrawEntry), not per typeID — a multi-packet type (e.g. a fence type
        // with separate "post"/"arch" sub-mesh packets) can have DIFFERENT textures per
        // packet. The prior per-type SSBO lookup (by inst.typeID) forced every packet
        // of a type to reuse one ("first-valid-wins") layer, so non-first packets
        // sampled the WRONG BC7 layer/UV combination — this is what produced solid-black
        // fragments (wrong layer decodes to a black margin block, alpha>=0.5 so no
        // alpha-test discard masked it). Fixed here: each vkCmdDrawIndexed for a packet
        // now pushes THAT packet's own layer, consumed directly by vert/frag instead of
        // the SSBO-by-typeID path.
        // FENCE-UVSCALE-1: widened 8 -> 16 bytes (2x int32 layer + 2x float
        // uvScale). Well inside the Vulkan-guaranteed minimum
        // maxPushConstantsSize of 128 bytes (spec table 32.1) -- no need to
        // pack into half floats or move to the per-instance SSBO path.
        VkPushConstantRange pcr{};
        pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pcr.offset = 0; pcr.size = 16; // int32 packetLayerOff, int32 packetLayerOn, float uvScaleX, float uvScaleY

        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 1; plci.pSetLayouts = &s->dsl;
        plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
        if (vkCreatePipelineLayout(s->device, &plci, nullptr, &s->playout) != VK_SUCCESS) {
            vlog("TIER-1 vkCreatePipelineLayout failed."); return false;
        }

        VkVertexInputBindingDescription vib{};
        vib.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        VkVertexInputAttributeDescription via[5]{};
        via[0].location = 0; via[0].binding = 0; via[0].format = VK_FORMAT_R32G32B32_SFLOAT; via[0].offset = 0;
        via[1].location = 1; via[1].binding = 0; via[1].format = VK_FORMAT_R32G32B32_SFLOAT; via[1].offset = 12;
        via[2].location = 2; via[2].binding = 0; via[2].format = VK_FORMAT_R32G32_SFLOAT;    via[2].offset = 24;
        via[3].location = 3; via[3].binding = 0; via[3].format = VK_FORMAT_R32_UINT;         via[3].offset = 32;
        via[4].location = 4; via[4].binding = 0; via[4].format = VK_FORMAT_R32_UINT;         via[4].offset = 36;
        VkPipelineVertexInputStateCreateInfo vin{};
        vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vin.vertexBindingDescriptionCount = 1; vin.pVertexBindingDescriptions = &vib;
        vin.vertexAttributeDescriptionCount = 5; vin.pVertexAttributeDescriptions = via;

        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo vps{};
        vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vps.viewportCount = 1; vps.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        // Reverse-Z GEQUAL, test+WRITE on (matches GL StaticPropOpaque; props occlude
        // vs the SHARED scene depth GL authored — the point of Tier-1).
        VkPipelineDepthStencilStateCreateInfo ds{};
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        ds.depthTestEnable = VK_TRUE;
        ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;

        // NO blend on EITHER attachment: opaque in-place overwrite. GBuffer1 is NOT
        // alpha-blendable (premult-over would corrupt the normal+class encoding), and
        // the color pass is opaque, so a winning fragment writes rgba directly.
        VkPipelineColorBlendAttachmentState cba[2]{};
        for (int k = 0; k < 2; ++k) {
            cba[k].blendEnable = VK_FALSE;
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
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = s->t1_vert; stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = s->t1_frag; stages[1].pName = "main";
        VkGraphicsPipelineCreateInfo gp{};
        gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gp.stageCount = 2; gp.pStages = stages;
        gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vps; gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms; gp.pDepthStencilState = &ds; gp.pColorBlendState = &cb;
        gp.pDynamicState = &dsci;
        gp.layout = s->playout; gp.renderPass = s->t1_rpass; gp.subpass = 0;
        if (vkCreateGraphicsPipelines(s->device, VK_NULL_HANDLE, 1, &gp, nullptr, &s->t1_pipe) != VK_SUCCESS) {
            vlog("TIER-1 vkCreateGraphicsPipelines failed."); setFallback("tier1_pipeline_failed"); return false;
        }
    }

    // ---- Command pool + 2 command buffers (per parity). ----
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

    // VK-P11 STATIC-PROP MATERIAL: bind a valid dummy albedo array + layer SSBO now
    // (needs the command pool above for the one-shot upload) so binding3 is a valid
    // descriptor before the first frame. The REAL array is imported once geometry is
    // finalized (cp_import_albedo_array in run()); materialReady gates FS sampling.
    if (s->wantMaterial && s->tier1) {
        if (!cp_ensure_dummy_albedo(s, cp_target_off(s))) { setFallback("material_dummy_failed"); return false; }
        // VK-P11 STATIC-PROP MATERIAL alpha-ON: dummy alpha-ON array + layer SSBO so
        // bindings 5+6 are valid before the first frame (materialReadyOn gates sampling).
        if (!cp_ensure_dummy_albedo(s, cp_target_on(s))) { setFallback("material_on_dummy_failed"); return false; }
        int32_t neg = -1;
        if (!cp_upload_layer_table(s, &neg, 1))    { setFallback("material_layer_buf_failed"); return false; }
        if (!cp_upload_layer_table_on(s, &neg, 1)) { setFallback("material_on_layer_buf_failed"); return false; }
        write_descriptors(s);  // now writes bindings 2/3 (alpha-OFF) + 5/6 (alpha-ON) dummies too
    }

    s->inited = true;
    setFallback("");
    vlog("island initialized %dx%d (borrowed shared device; Tier-1 shared-GBuffer prop co-write).", w, h);
    return true;
}

// Copy the shared static-prop VBO/IBO into VK-owned buffers ONCE per map, reusing the
// shadow island's geometry feed (SAME shared buffers). Returns false if unavailable.
bool upload_geometry_once(ColorPropIsland* s) {
    if (s->geomUploaded) return true;
    Mc2ShadowGeomFeed feed{};
    if (!mc2_shadow_island_get_geometry_feed(&feed)) return false;
    if (feed.vboBytes == 0 || feed.iboBytes == 0 || feed.glVbo == 0 || feed.glIbo == 0) return false;

    if (!make_buffer(s, feed.vboBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &s->vbo, &s->vboAlloc, true)) return false;
    if (!make_buffer(s, feed.iboBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,  &s->ibo, &s->iboAlloc, true)) return false;
    std::vector<uint8_t> scratch;
    { void* p = nullptr;
      if (vmaMapMemory(s->allocator, s->vboAlloc, &p) == VK_SUCCESS) {
          scratch.resize((size_t)feed.vboBytes);
          glBindBuffer(GL_ARRAY_BUFFER, feed.glVbo);
          glGetBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)feed.vboBytes, scratch.data());
          glBindBuffer(GL_ARRAY_BUFFER, 0);
          std::memcpy(p, scratch.data(), (size_t)feed.vboBytes);
          vmaUnmapMemory(s->allocator, s->vboAlloc);
      } }
    { void* p = nullptr;
      if (vmaMapMemory(s->allocator, s->iboAlloc, &p) == VK_SUCCESS) {
          scratch.resize((size_t)feed.iboBytes);
          glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, feed.glIbo);
          glGetBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, (GLsizeiptr)feed.iboBytes, scratch.data());
          glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
          std::memcpy(p, scratch.data(), (size_t)feed.iboBytes);
          vmaUnmapMemory(s->allocator, s->iboAlloc);
      } }

    s->packets.assign(feed.packets, feed.packets + feed.packetCount);
    s->typeRuns.clear();
    s->typeRuns.resize(feed.typeCount);
    for (uint32_t t = 0; t < feed.typeCount; ++t) {
        s->typeRuns[t].typeID      = t;
        s->typeRuns[t].firstPacket = feed.typeFirstPacket[t];
        s->typeRuns[t].packetCount = feed.typePacketCount[t];
    }
    g_health.geomVerts = (unsigned long)(feed.vboBytes / (feed.vertexStride ? feed.vertexStride : 1));
    s->geomUploaded = true;
    vlog("prop geometry uploaded once: vbo=%llu B ibo=%llu B stride=%u packets=%u types=%u.",
         (unsigned long long)feed.vboBytes, (unsigned long long)feed.iboBytes,
         feed.vertexStride, feed.packetCount, feed.typeCount);
    return true;
}

// currently-bound draw FBO (MainColor at flush() time). Reused water-island shape.
extern "C" int mc2_vk_color_prop_island_enabled() {
    // Either tier armed => the caller skips the GL prop color flush and calls run().
    return propColorTier1Requested() ? 1 : 0;
}

extern "C" int mc2_vk_color_prop_island_run(int w, int h) {
    ZoneScopedN("VK-props-island");   // VK-FLIP-FIELD-REPORT-1 coarse region zone
    if (!propColorTier1Requested()) return 0;
    if (w <= 0 || h <= 0) return 0;

    using clk = std::chrono::steady_clock;
    auto usSince = [](clk::time_point t0){ return std::chrono::duration<double, std::micro>(clk::now() - t0).count(); };
    const clk::time_point tTotal0 = clk::now();

    // Pull the per-frame color instance feed (visible buckets + world->clip + bias).
    Mc2ColorPropFeed feed{};
    if (!mc2_color_prop_island_get_frame_feed(&feed)) { setFallback("no_color_feed"); return 0; }
    if (!feed.instances112 || feed.count == 0) { setFallback("no_color_instances"); return 0; }

    // Lazy init / rebuild on size change.
    if (g_island && (g_island->w != w || g_island->h != h)) {
        destroy_island(g_island); delete g_island; g_island = nullptr;
    }
    if (!g_island) {
        g_island = new ColorPropIsland();
        if (!init_island(g_island, w, h)) {
            vlog("init failed (%s) — GL fallback.", g_health.fallbackReason);
            destroy_island(g_island); delete g_island; g_island = nullptr;
            return 0;
        }
    }
    ColorPropIsland* s = g_island;
    // VK-OWNERSHIP-GAP-FIX (item-4 correctness): direct-chain sets useInterop=false
    // (init:781) but the old guard '|| !s->useInterop' then bailed here, so under the
    // strict umbrella the island returned 0 and GL silently drew props into the
    // VK-owned GBuffer (used_vulkan=0). Enter the native render path when direct-chain
    // OR the hybrid interop arm is armed -- mirrors mech/terrain (a0cba0c4/1f4a1a5a)
    // whose guard is 'disabled || !inited' with directChain unconditional-native.
    if (s->disabled || !s->inited) return 0;  // VK-RUNG8-ITEM5: direct-chain-native only

    if (!upload_geometry_once(s)) { setFallback("geometry_not_ready"); return 0; }

    // VK-P11 STATIC-PROP MATERIAL: copy-once import the alpha-OFF albedo 2D_ARRAY +
    // per-type layer table once geometry is finalized. Fail-soft: any miss (feed not
    // ready, BC7-bucketed, import failure) leaves materialReady=false -> FS keeps teal.
    if (s->wantMaterial && s->tier1 && (!s->materialReady || !s->materialReadyOn)) {
        Mc2ColorPropMaterialFeed mf{};
        if (mc2_color_prop_island_get_material_feed(&mf) && mf.glTexArrayOff != 0 && !mf.bucketed) {
            s->terrainSunMC2[0] = mf.terrainSunMC2[0];
            s->terrainSunMC2[1] = mf.terrainSunMC2[1];
            s->terrainSunMC2[2] = mf.terrainSunMC2[2];
            bool changed = false;
            // ---- alpha-OFF (class 0): textured layer per type -> s_texArrayOff. ----
            if (!s->materialReady) {
                std::vector<int32_t> layerTbl(mf.typeCount, -1);
                for (uint32_t t = 0; t < mf.typeCount; ++t) {
                    if (mf.typeAlphaClass[t] == 0 && mf.typeAlbedoLayer[t] >= 0)
                        layerTbl[t] = mf.typeAlbedoLayer[t];
                }
                if (cp_import_albedo_array(s, cp_target_off(s), mf.glTexArrayOff)) {
                    cp_upload_layer_table(s, layerTbl.data(), mf.typeCount);
                    changed = true;
                    vlog("material feed active: alpha-OFF %u types textured (sun=%.2f,%.2f,%.2f).",
                         mf.typeCount, s->terrainSunMC2[0], s->terrainSunMC2[1], s->terrainSunMC2[2]);
                }
            }
            // VK-FLIP-FIELD-REPORT-2 BLACK-FENCE FIX: snapshot the per-packet layer
            // tables (both groups) once the packet feed is available, regardless of
            // which group's SSBO import just ran — the push-constant draw path reads
            // these every frame, not gated by materialReady/materialReadyOn latches.
            if (mf.packetAlbedoLayer && mf.packetCount > 0) {
                s->packetLayerOff.assign(mf.packetCount, -1);
                s->packetLayerOn.assign(mf.packetCount, -1);
                // FENCE-UVSCALE-1: default 1.0 (no crop) so any packet the feed
                // doesn't cover (or a stale/mismatched feed) behaves exactly as
                // before this change -- fail-soft to the old full-layer sample.
                s->packetUvScaleX.assign(mf.packetCount, 1.0f);
                s->packetUvScaleY.assign(mf.packetCount, 1.0f);
                for (uint32_t t = 0; t < mf.typeCount && t < s->typeRuns.size(); ++t) {
                    const TypeRun& tr = s->typeRuns[t];
                    const bool isOn = (mf.typeAlphaClass[t] == 1);
                    for (uint32_t p = 0; p < tr.packetCount; ++p) {
                        const uint32_t pk = tr.firstPacket + p;
                        if (pk >= mf.packetCount) break;
                        const int32_t layer = mf.packetAlbedoLayer[pk];
                        if (isOn) s->packetLayerOn[pk]  = layer;
                        else      s->packetLayerOff[pk] = layer;
                        if (mf.packetUvScaleX && mf.packetUvScaleY) {
                            s->packetUvScaleX[pk] = mf.packetUvScaleX[pk];
                            s->packetUvScaleY[pk] = mf.packetUvScaleY[pk];
                        }
                    }
                }
            }
            // ---- alpha-ON (class 1): the alpha-MIXED buildings. typeAlbedoLayer[t] is
            // ALREADY group-relative into s_texArrayOn for class-1 types, so the same
            // table shape works with the alpha-ON array + a 0.5 alpha-test in the FS. ----
            if (!s->materialReadyOn && mf.glTexArrayOn != 0) {
                std::vector<int32_t> layerOnTbl(mf.typeCount, -1);
                for (uint32_t t = 0; t < mf.typeCount; ++t) {
                    if (mf.typeAlphaClass[t] == 1 && mf.typeAlbedoLayer[t] >= 0)
                        layerOnTbl[t] = mf.typeAlbedoLayer[t];
                }
                if (cp_import_albedo_array(s, cp_target_on(s), mf.glTexArrayOn)) {
                    cp_upload_layer_table_on(s, layerOnTbl.data(), mf.typeCount);
                    changed = true;
                    vlog("material feed active: alpha-ON %u types textured (alpha-test 0.5).",
                         mf.typeCount);
                }
            }
            if (changed) write_descriptors(s);  // rebind real arrays (3/6) + layer SSBOs (2/5)
        } else if (mf.bucketed) {
            static bool warned = false;
            if (!warned) { vlog("material feed is BC7-bucketed — DEFERRED; props keep flat teal."); warned = true; }
        }
    }

    // VK-PROP-AMBIENT-IBL-1: per-frame copy of the live GL LightsData SSBO into the
    // island's own SSBO (binding 4) so the vert shader's ambientTerm sum reads the same
    // ObjectLights[] the GL prop VS does. Mirrors VK-MECH-MATERIAL-LIGHTING-1's copy
    // exactly (glGetBufferSubData is a buffer readback, NOT a color-surface readback --
    // does not touch g_forbidden). IBL-SH coeffs are a static per-mission table (no GL
    // round-trip) fetched fresh each frame in case the mission's SH set changed.
    Mc2ColorPropLightFeed lightFeed{};
    const bool haveLightFeed = mc2_color_prop_island_get_light_feed(&lightFeed);
    s->lightingReady = false;
    if (s->tier1 && s->wantLighting && haveLightFeed &&
        lightFeed.lightDataGlSsbo != 0 && lightFeed.lightDataBytes >= 3600ull) {
        const VkDeviceSize lbytes = (VkDeviceSize)lightFeed.lightDataBytes;
        if (ensure_light_capacity(s, lbytes)) {
            void* p = nullptr;
            if (vmaMapMemory(s->allocator, s->lightAlloc, &p) == VK_SUCCESS) {
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, (GLuint)lightFeed.lightDataGlSsbo);
                glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)lbytes, p);
                // ADJUDICATION-1 WASHOUT-RECON: one-shot CPU-side replica of the vert
                // shader's ambientTerm/sunColor scan against the JUST-COPIED bytes (the
                // exact bytes the VK vert shader will read this frame). If this ever
                // disagrees with a live GL-side dump of the SAME ObjectLights[idx] taken
                // at GL's own draw time, the copy is stale/racing GL's write. Probes
                // entries 0..7 (first few ObjectLights rows) to catch whichever is
                // actually populated for on-screen buildings this frame.
                if (std::getenv("MC2_VK_PROP_LIGHT_VERTEX_PROBE")) {
                    static bool s_vtx_dumped = false;
                    if (!s_vtx_dumped) {
                        s_vtx_dumped = true;
                        struct ObjLightsMirror {
                            float light_to_world[32][16];
                            float light_dir[32][4];
                            float light_color[32][4];
                            float light_falloff[32][4];
                            int32_t numLights[4];
                        };
                        const size_t stride = sizeof(ObjLightsMirror);
                        const size_t maxEntries = (size_t)lbytes / stride;
                        const uint8_t* base = (const uint8_t*)p;
                        auto scanEntry = [&](size_t idx) {
                            if (idx >= maxEntries) return;
                            const ObjLightsMirror* ld = (const ObjLightsMirror*)(base + idx * stride);
                            float ambient[3] = {0, 0, 0};
                            float sunColor[3] = {0, 0, 0};
                            bool sunFound = false;
                            int n = ld->numLights[0];
                            if (n > 32) n = 32;
                            if (n < 0) n = 0;
                            for (int i = 0; i < n; ++i) {
                                int lt = (int)ld->light_dir[i][3];
                                if (lt == 0) { // TG_LIGHT_AMBIENT
                                    ambient[0] += ld->light_color[i][0];
                                    ambient[1] += ld->light_color[i][1];
                                    ambient[2] += ld->light_color[i][2];
                                } else if (!sunFound && (lt == 1 || lt == 2)) {
                                    sunColor[0] = ld->light_color[i][0];
                                    sunColor[1] = ld->light_color[i][1];
                                    sunColor[2] = ld->light_color[i][2];
                                    sunFound = true;
                                }
                            }
                            std::fprintf(stderr,
                                "[PROP_LIGHT_VERTEX_PROBE v1] idx=%zu numLights=%d "
                                "ambient=(%.4f,%.4f,%.4f) sunColor=(%.4f,%.4f,%.4f) sunFound=%d\n",
                                idx, n, ambient[0], ambient[1], ambient[2],
                                sunColor[0], sunColor[1], sunColor[2], sunFound ? 1 : 0);
                        };
                        // Real per-instance lightDataIndex values actually used this
                        // frame (first few real props), instead of a blind 0..7 scan.
                        {
                            const GpuStaticPropInstanceMirror* insts =
                                reinterpret_cast<const GpuStaticPropInstanceMirror*>(feed.instances112);
                            std::fprintf(stderr, "[PROP_LIGHT_VERTEX_PROBE v1] real inst.lightDataIndex sample: ");
                            for (uint32_t ii = 0; ii < feed.count && ii < 12; ++ii) {
                                std::fprintf(stderr, "[%u]=%u typeID=%u ", ii, insts[ii].lightDataIndex, insts[ii].typeID);
                            }
                            std::fprintf(stderr, "\n");
                            std::fflush(stderr);
                            // Scan the ACTUAL indices these instances use (dedup small set).
                            uint32_t seen[12]; int seenCount = 0;
                            for (uint32_t ii = 0; ii < feed.count && ii < 200; ++ii) {
                                uint32_t li = insts[ii].lightDataIndex;
                                bool dup = false;
                                for (int k = 0; k < seenCount; ++k) if (seen[k] == li) { dup = true; break; }
                                if (!dup && seenCount < 12) { seen[seenCount++] = li; scanEntry((size_t)li); }
                            }
                        }
                        for (size_t idx = 0; idx < maxEntries && idx < 8; ++idx) scanEntry(idx);
                        std::fflush(stderr);
                    }
                }
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
                vmaUnmapMemory(s->allocator, s->lightAlloc);
                s->lightingReady = true;
            }
        }
    }

    const unsigned long f0a = g_forbidden.vkCmdCopyImageToBuffer;
    const unsigned long f0b = g_forbidden.glGetTexImage;
    const unsigned long f0c = g_forbidden.glReadPixels;

    const clk::time_point tStage0 = clk::now();
    // ---- 1) Upload the per-frame color instances (already grouped by typeID). ----
    const uint32_t instanceCount = feed.count;
    const VkDeviceSize instBytes = (VkDeviceSize)instanceCount * sizeof(GpuStaticPropInstanceMirror);
    if (!ensure_inst_capacity(s, instBytes)) { s->disabled = true; return 0; }
    {
        ZoneScopedN("VK-props-instance-upload");   // VK-FLIP-FIELD-REPORT-1: per-frame instance SSBO staging
        void* p = nullptr;
        if (vmaMapMemory(s->allocator, s->instAlloc, &p) == VK_SUCCESS) {
            std::memcpy(p, feed.instances112, (size_t)instBytes);
            vmaUnmapMemory(s->allocator, s->instAlloc);
        }
    }
    g_health.instRecords = instanceCount;

    // ---- 2) Upload the per-frame UBO (worldToClipGL + bias; instBase=0). ----
    {
        ColorFrameUBO u{};
        std::memcpy(u.worldToClipGL, feed.worldToClipGL, sizeof(u.worldToClipGL));
        u.objectDepthBias = feed.objectDepthBias;
        u.instBase = 0;
        // VK-P11 STATIC-PROP MATERIAL: gate FS albedo sample + computePBR on the real
        // import having landed (Tier-1 only). material-disabled paths leave these zero (the shaders
        // never read them).
        u.materialReady = (s->wantMaterial && s->tier1 && s->materialReady) ? 1 : 0;
        u.materialReadyOn = (s->wantMaterial && s->tier1 && s->materialReadyOn) ? 1 : 0;
        u.terrainSunMC2[0] = s->terrainSunMC2[0];
        u.terrainSunMC2[1] = s->terrainSunMC2[1];
        u.terrainSunMC2[2] = s->terrainSunMC2[2];
        u.terrainSunMC2[3] = 0.0f;
        // VK-PROP-AMBIENT-IBL-1: IBL-SH coeffs (std140 vec3[9] tail, .w pad each) +
        // strength + lightingReady. lightingReady gates the vert shader's real
        // ObjectLights ambientTerm sum; iblShStrength==0.0 short-circuits evalShL2
        // exactly like the GL guard (byte-identical when the feature/env is off).
        if (haveLightFeed) {
            for (int i = 0; i < 9; ++i) {
                u.iblSh[i][0] = lightFeed.iblSh[i][0];
                u.iblSh[i][1] = lightFeed.iblSh[i][1];
                u.iblSh[i][2] = lightFeed.iblSh[i][2];
                u.iblSh[i][3] = 0.0f;
            }
            u.iblShStrength = lightFeed.iblShStrength;
        } else {
            std::memset(u.iblSh, 0, sizeof(u.iblSh));
            u.iblShStrength = 0.0f;
        }
        u.lightingReady = s->lightingReady ? 1 : 0;
        // ADJUDICATION-1 WASHOUT engagement probe: one-shot dump of the exact UBO
        // lighting terms so we can tell fed-real from fail-soft-constant. If
        // lightingReady==0, the VS ambientTerm falls back to a hemisphere constant
        // (= washout). Prints WHY the LightsData copy did/didn't land.
        {
            static bool s_lit_dumped = false;
            if (std::getenv("MC2_VK_PROP_LIGHT_PROBE") && !s_lit_dumped) {
                s_lit_dumped = true;
                std::fprintf(stderr,
                    "[PROP_LIGHT_PROBE v1] lightingReady=%d haveLightFeed=%d tier1=%d "
                    "wantLighting=%d wantMaterial=%d materialReady=%d "
                    "iblShStrength=%.4f iblSh0=(%.4f,%.4f,%.4f) "
                    "lightDataGlSsbo=%u lightDataBytes=%llu terrainSun=(%.3f,%.3f,%.3f)\n",
                    u.lightingReady, haveLightFeed ? 1 : 0, s->tier1 ? 1 : 0,
                    s->wantLighting ? 1 : 0, s->wantMaterial ? 1 : 0, u.materialReady,
                    u.iblShStrength, u.iblSh[0][0], u.iblSh[0][1], u.iblSh[0][2],
                    (unsigned)lightFeed.lightDataGlSsbo,
                    (unsigned long long)lightFeed.lightDataBytes,
                    u.terrainSunMC2[0], u.terrainSunMC2[1], u.terrainSunMC2[2]);
                std::fflush(stderr);
            }
        }
        void* p = nullptr;
        if (vmaMapMemory(s->allocator, s->uboAlloc, &p) == VK_SUCCESS) {
            std::memcpy(p, &u, sizeof(u)); vmaUnmapMemory(s->allocator, s->uboAlloc);
        }
    }
    g_health.stageInstUs = usSince(tStage0);
    // VK-FLIP-FIELD-REPORT-1: the instance+UBO staging above is the per-frame prop
    // upload cost (a coarse zone for it would nest cleanly; kept as the usSince metric
    // to avoid an extra scope around the two vmaMap blocks).

    // The feed is already grouped by ascending typeID; build contiguous type-runs.
    const GpuStaticPropInstanceMirror* insts =
        reinterpret_cast<const GpuStaticPropInstanceMirror*>(feed.instances112);

    // ===================================================================
    // TIER-1 co-write: props rendered IN-PLACE into the SHARED scene color(MRT0)+
    // normal(MRT1)+depth GBuffer. GL authored terrain into all three and handed them
    // to VK (each in its *_ATTACHMENT layout); VK LOADs (does NOT clear) all three,
    // depth-tests GEQUAL reverse-Z, and only overwrites where the prop wins. No
    // no composite, NO re-clear of GBuffer1 (respects clearGBuffer1()).
    // ===================================================================
    if (s->tier1) {
        VkImageResource* colorRes  = reinterpret_cast<VkImageResource*>(s->t1_colorRes);
        (void)colorRes;

        // VK-RUNG8-ITEM5: direct-chain-native only. The shared GBuffer was left
        // Vulkan-owned by the terrain first-owner (same attachment layouts), so no
        // GL<->VK semaphore handoff is needed. Begin a frame on the color image only
        // (parity fence for CPU ordering); the render pass covers all three.
        VkFrameSync fsC = vkImageResourceBeginFrame(colorRes);
        VkCommandBuffer cb = s->cbuf[fsC.parity & 1u];

        const clk::time_point tRender0 = clk::now();
        ZoneNamedN(zPropRec, "VK-props-record-submit", true);   // VK-FLIP-FIELD-REPORT-1: CPU record+queue-submit (to tier1-block end)
        vkResetCommandBuffer(cb, 0);
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) { setFallback("record_failed"); s->disabled = true; return 0; }

        // Begin the LOAD renderpass — NO clear values (all attachments loadOp=LOAD).
        VkRenderPassBeginInfo rbi{};
        rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rbi.renderPass = s->t1_rpass; rbi.framebuffer = s->t1_fb;
        rbi.renderArea = {{0, 0}, {(uint32_t)w, (uint32_t)h}};
        rbi.clearValueCount = 0; rbi.pClearValues = nullptr;
        vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport vp{0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f};
        VkRect2D scissor{{0, 0}, {(uint32_t)w, (uint32_t)h}};
        vkCmdSetViewport(cb, 0, 1, &vp);
        vkCmdSetScissor(cb, 0, 1, &scissor);

        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->t1_pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->playout, 0, 1, &s->set, 0, nullptr);
        VkDeviceSize vboOff = 0;
        vkCmdBindVertexBuffers(cb, 0, 1, &s->vbo, &vboOff);
        vkCmdBindIndexBuffer(cb, s->ibo, 0, VK_INDEX_TYPE_UINT32);

        // ADJUDICATION-1 draw-level dump: env-gated one-shot CSV of every packet's
        // draw parameters + push-constant layers. This is the fence-fix ground truth
        // (the exact per-packet data RenderDoc would show at a VK vkCmdDrawIndexed,
        // which a GL-only RenderDoc capture cannot see for this headless-VK island).
        // Gated on the same sim-freeze predicate the deterministic sweep uses.
        FILE* s_pktDump = nullptr;
        {
            static bool s_dumped = false;
            const char* dp = std::getenv("MC2_VK_PROP_PACKET_DUMP");
            extern int mc2CaptureGaitFreezeActive();
            const bool frozen = mc2CaptureGaitFreezeActive() != 0 ||
                                std::getenv("MC2_VISUAL_BOOKMARK_CAPTURE") == nullptr;
            if (dp && dp[0] && !s_dumped && frozen) {
                s_dumped = true;
                s_pktDump = std::fopen(dp, "w");
                if (s_pktDump)
                    std::fprintf(s_pktDump, "typeID,packet,layerOff,layerOn,uvScaleX,uvScaleY,indexCount,firstIndex,baseVertex,instCount,firstInstance\n");
            }
        }

        unsigned long typeRuns = 0;
        uint32_t i = 0;
        while (i < instanceCount) {
            const uint32_t tid = insts[i].typeID;
            uint32_t j = i;
            while (j < instanceCount && insts[j].typeID == tid) ++j;
            const uint32_t instCount = j - i;
            if (tid < s->typeRuns.size()) {
                const TypeRun& tr = s->typeRuns[tid];
                for (uint32_t p = 0; p < tr.packetCount; ++p) {
                    const uint32_t pk = tr.firstPacket + p;
                    if (pk >= s->packets.size()) break;
                    const Mc2ShadowPacket& pkt = s->packets[pk];
                    // VK-FLIP-FIELD-REPORT-2 BLACK-FENCE FIX: push THIS packet's own
                    // group-relative albedo layer (off/on), not the type's shared
                    // "first-valid-wins" layer — see packetLayerOff/On comment above.
                    // FENCE-UVSCALE-1: push this packet's own uvScaleX/Y alongside
                    // its layer -- default 1.0 (no crop) matches pre-fix behavior
                    // for any packet the feed didn't cover.
                    struct { int32_t layerOff, layerOn; float uvScaleX, uvScaleY; } pc = {
                        (pk < s->packetLayerOff.size())  ? s->packetLayerOff[pk]  : -1,
                        (pk < s->packetLayerOn.size())   ? s->packetLayerOn[pk]   : -1,
                        (pk < s->packetUvScaleX.size())  ? s->packetUvScaleX[pk]  : 1.0f,
                        (pk < s->packetUvScaleY.size())  ? s->packetUvScaleY[pk]  : 1.0f,
                    };
                    if (s_pktDump)
                        std::fprintf(s_pktDump, "%u,%u,%d,%d,%.4f,%.4f,%u,%u,%d,%u,%u\n",
                                     tid, pk, pc.layerOff, pc.layerOn, pc.uvScaleX, pc.uvScaleY,
                                     pkt.indexCount, pkt.firstIndex,
                                     (int32_t)pkt.baseVertex, instCount, i);
                    vkCmdPushConstants(cb, s->playout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                        0, sizeof(pc), &pc);
                    vkCmdDrawIndexed(cb, pkt.indexCount, instCount,
                                     pkt.firstIndex, (int32_t)pkt.baseVertex, i);
                }
                ++typeRuns;
            }
            i = j;
        }
        if (s_pktDump) { std::fclose(s_pktDump); s_pktDump = nullptr; }
        g_health.typeRuns = typeRuns;

        vkCmdEndRenderPass(cb);
        if (vkEndCommandBuffer(cb) != VK_SUCCESS) { setFallback("record_failed"); s->disabled = true; return 0; }

        // VK-RUNG8-ITEM5: direct-chain-native submit — waits/signals NO GL<->VK
        // semaphores (the shared GBuffer stays Vulkan-owned); the color fence below
        // orders CPU before the next Vulkan island records against these images.
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.waitSemaphoreCount = 0; si.pWaitSemaphores = nullptr; si.pWaitDstStageMask = nullptr;
        si.commandBufferCount = 1; si.pCommandBuffers = &cb;
        si.signalSemaphoreCount = 0; si.pSignalSemaphores = nullptr;
        if (vkQueueSubmit(s->queue, 1, &si, fsC.fence) != VK_SUCCESS) { setFallback("submit_failed"); s->disabled = true; return 0; }

        // Retire this frame's submit before the parity slot is reused (WAW).
        if (vkWaitForFences(s->device, 1, &fsC.fence, VK_TRUE, 5ull*1000*1000*1000) != VK_SUCCESS) {
            setFallback("fence_timeout"); s->disabled = true; return 0;
        }
        g_health.drawUs = usSince(tRender0);

        // NO composite — props were co-written in-place into the real GBuffer.

        const bool readbackFired =
            (g_forbidden.vkCmdCopyImageToBuffer != f0a) ||
            (g_forbidden.glGetTexImage != f0b) || (g_forbidden.glReadPixels != f0c);
        if (readbackFired) {
            vlog("FATAL(tier1): a readback fired on the co-write path — failing frame + disabling.");
            setFallback("interop_did_readback"); s->disabled = true; return 0;
        }

        g_health.vkDraws = 1;
        ++g_health.usedVulkan;
        // VK-RUNG8-ITEM5: never interop (direct-chain-native only).
        g_health.totalUs = usSince(tTotal0);
        static bool s_t1FirstOk = false;
        if (!s_t1FirstOk) {
            vlog("first TIER-1 in-place GBuffer co-write: instCount=%u typeRuns=%lu "
                 "(props co-written into shared color(MRT0)+normal(MRT1)+depth; no composite).",
                 instanceCount, g_health.typeRuns);
            s_t1FirstOk = true;
        }
        return 1;
    }

    return 0;
}
extern "C" void mc2_render_backend_color_prop_region_health(
    int* vulkanAvailable, int* buildEnabled, const char** deviceName,
    unsigned long* validationErrors, unsigned long* attempted, unsigned long* usedVulkan,
    const char** fallbackReason, unsigned long* vkDraws,
    int* interopActive, int* vk2glSemActive, unsigned long* usedInterop,
    unsigned long* instRecords, unsigned long* typeRuns, unsigned long* geomVerts,
    unsigned long* forbiddenCopyImgToBuf, unsigned long* forbiddenGetTexImage,
    unsigned long* forbiddenReadPixels,
    double* stageInstUs, double* drawUs, double* totalUs) {
    if (vulkanAvailable)  *vulkanAvailable  = g_health.vulkanAvailable;
    if (buildEnabled)     *buildEnabled     = g_health.buildEnabled;
    if (deviceName)       *deviceName       = g_health.deviceName;
    if (validationErrors) *validationErrors = g_health.validationErrors;
    if (attempted)        *attempted        = g_health.attempted;
    if (usedVulkan)       *usedVulkan       = g_health.usedVulkan;
    if (fallbackReason)   *fallbackReason   = g_health.fallbackReason;
    if (vkDraws)          *vkDraws          = g_health.vkDraws;
    if (interopActive)    *interopActive    = g_health.interopActive;
    if (vk2glSemActive)   *vk2glSemActive   = g_health.vk2glSemActive;
    if (usedInterop)      *usedInterop      = g_health.usedInterop;
    if (instRecords)      *instRecords      = g_health.instRecords;
    if (typeRuns)         *typeRuns         = g_health.typeRuns;
    if (geomVerts)        *geomVerts        = g_health.geomVerts;
    if (forbiddenCopyImgToBuf) *forbiddenCopyImgToBuf = g_forbidden.vkCmdCopyImageToBuffer;
    if (forbiddenGetTexImage)  *forbiddenGetTexImage  = g_forbidden.glGetTexImage;
    if (forbiddenReadPixels)   *forbiddenReadPixels   = g_forbidden.glReadPixels;
    if (stageInstUs)      *stageInstUs      = g_health.stageInstUs;
    if (drawUs)           *drawUs           = g_health.drawUs;
    if (totalUs)          *totalUs          = g_health.totalUs;
}

extern "C" void mc2_vk_color_prop_island_destroy() {
    vlog("[VK_PROP_COLOR_TIER1] vulkan_available=%d used_vulkan=%lu used_interop=%lu interop_active=%d "
         "vk2gl_sem=%d validation_errors=%lu inst_records=%lu type_runs=%lu geom_verts=%lu "
         "fallback_reason=\"%s\" vk_draws=%lu forbidden_copyImgToBuf=%lu forbidden_getTexImage=%lu "
         "forbidden_readPixels=%lu stage_inst_us=%.1f draw_us=%.1f total_us=%.1f",
         g_health.vulkanAvailable, g_health.usedVulkan, g_health.usedInterop, g_health.interopActive,
         g_health.vk2glSemActive, g_health.validationErrors, g_health.instRecords, g_health.typeRuns,
         g_health.geomVerts, g_health.fallbackReason, g_health.vkDraws,
         g_forbidden.vkCmdCopyImageToBuffer, g_forbidden.glGetTexImage, g_forbidden.glReadPixels,
         g_health.stageInstUs, g_health.drawUs, g_health.totalUs);
    if (!g_island) return;
    destroy_island(g_island);
    delete g_island; g_island = nullptr;
}

}

#endif // MC2_VULKAN_ISLAND
