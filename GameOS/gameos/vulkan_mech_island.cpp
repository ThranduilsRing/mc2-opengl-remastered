// vulkan_mech_island.cpp — VK-P14 MECH TIER-1: the FIRST VK-native SKINNED mech region.
// =============================================================================
// Co-writes the GL mech batcher's visible instances IN-PLACE into the SHARED scene
// GBuffer (MRT0 color + MRT1 normal + shared D32S8 depth), VK-skinned. Structure =
// vulkan_color_prop_island.cpp's TIER-1 co-write path (shared-GBuffer LOAD-preserve
// renderpass, GEQUAL reverse-Z, no composite), with the static-prop 40B vertex +
// modelMatrix instance swapped for:
//   - the mech 48B vertex stream (GpuMechVertex: pos/normal/uv/boneIdx u8[4]/
//     boneWeights u8[4]/tangentOct/aRGBLight), copied ONCE per map;
//   - a per-frame BONE PALETTE SSBO (GpuMechBone 64B) memcpy'd each frame;
//   - a per-frame INSTANCE SSBO (GpuMechInstance 64B);
//   - a VS that does LINEAR-BLEND SKINNING over 4 bone weights (mech.vert:156-180),
//     then the Stuff->MC2 axis swap worldMC2 = vec3(-x,z,y) (mech.vert:178).
//
// SUB-SLICE 1 (this file): skinning + FLAT color. This is the P-conjugation /
// axis-swap silhouette proof — verify the mech is not gliding/mirrored/in-sky
// BEFORE adding material + calc_light lighting. Those are gated follow-ons:
//   MC2_VK_MECH_MATERIAL  — copy-once single 2D albedo (slot-0 paint scheme) sampled
//                           per run (single-material probe; per-actor VK-descriptor
//                           cache DEFERRED — see the run() material note).
//   MC2_VK_MECH_LIGHTING  — port calc_light + feed the LightsData UBO (DEFERRED;
//                           needs the whole ObjectLights SSBO mirrored to VK).
//
// Gate MC2_VK_MECH_TIER1 (requires MC2_VK_SHARED_SCENE_DEPTH), default-OFF -> GL
// mech color flush byte-identical. The feed (instances + bone palette + shared
// VBO/IBO + per-run slot-0 GL texture) comes from gos_mech_batcher.cpp via the
// Mc2Mech*Feed accessors. NO fenced GL buffer crosses.
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
#include "vulkan_mech_island.h"
#include "vk_shared_gbuffer_gate.h"      // VK-P14 GATE-CONSOLIDATION-1 umbrella resolver
#include "vk_gpu_timestamps.h"           // VK-DRAW-PERF-1: MC2_VK_GPU_TIMESTAMPS GPU segment timers
#define MC2_VULKAN_SHARED_SCENE_COLOR_IMPL_ISLAND
#include "vulkan_shared_scene_color.h"   // shared scene color/normal images
#define MC2_VULKAN_SHARED_SCENE_DEPTH_IMPL_ISLAND
#include "vulkan_shared_scene_depth.h"   // shared scene depth image

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cstdint>
#include <vector>
#include <string>
#include <chrono>
#include <tracy/Tracy.hpp>   // VK-FLIP-FIELD-REPORT-1: coarse per-region frame zones

namespace {

struct MechIslandHealth {
    int   vulkanAvailable = 0;
    int   buildEnabled    = 1;
    char  deviceName[256] = {0};
    unsigned long attempted   = 0;
    unsigned long usedVulkan  = 0;
    char  fallbackReason[256] = {0};
    unsigned long vkDraws     = 0;
    int   interopActive  = 0;
    int   vk2glSemActive = 0;
    unsigned long instRecords = 0;
    unsigned long boneRecords = 0;
    unsigned long runs        = 0;
    unsigned long geomVerts   = 0;
    double stageUs = 0.0, drawUs = 0.0, totalUs = 0.0;
};
MechIslandHealth g_health;

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
    std::fprintf(stderr, "[VK_MECH_ISLAND] ");
    std::vfprintf(stderr, fmt, ap);
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
    va_end(ap);
}

bool mechGateRequested() {
    static int state = -1;
    if (state == -1) {
        // Umbrella members: MC2_VK_SHARED_GBUFFER=1 arms both; explicit
        // MC2_VK_MECH_TIER1 / MC2_VK_SHARED_SCENE_DEPTH =0/1 override.
        const bool on  = mc2vkgate::subGate("MC2_VK_MECH_TIER1");
        const bool dep = mc2vkgate::subGate("MC2_VK_SHARED_SCENE_DEPTH");
        state = (on && dep) ? 1 : 0;   // require shared depth
    }
    return state == 1;
}

bool mechDirectChainRequested() {
    static int state = -1;
    if (state == -1) {
        const bool mechGate = mechGateRequested();
        const bool propDirect = mc2vkgate::directChainGate("MC2_VK_PROP_DIRECT_CHAIN");
        const bool mechDirect = mc2vkgate::directChainGate("MC2_VK_MECH_DIRECT_CHAIN");
        state = (mechGate && propDirect && mechDirect) ? 1 : 0;
    }
    return state == 1;
}

// MC2_VK_MECH_MATERIAL (follow-on): copy-once single 2D albedo for the covered
// stock mech type. DEFERRED general case = per-frame slot0 handle -> VK-descriptor
// cache. Default-OFF -> flat color.
bool mechMaterialRequested() {
    // Umbrella member; explicit MC2_VK_MECH_MATERIAL=0/1 overrides.
    static int state = -1;
    if (state == -1) state = mc2vkgate::subGate("MC2_VK_MECH_MATERIAL") ? 1 : 0;
    return state == 1;
}

// MC2_VK_MECH_LIGHTING (follow-on): per-vertex calc_light Lambert (matches the GL
// default mech path). Default-OFF -> flat-white lit (v_litColor = 1). Independent of
// MC2_VK_MECH_MATERIAL, but only visible when material is on (albedo * litColor).
bool mechLightingRequested() {
    // Umbrella member; explicit MC2_VK_MECH_LIGHTING=0/1 overrides.
    static int state = -1;
    if (state == -1) state = mc2vkgate::subGate("MC2_VK_MECH_LIGHTING") ? 1 : 0;
    return state == 1;
}

// VK-RUNG8 item-9: MC2_VK_MECH_FEED_NATIVE (rung-7 opt-in) is RETIRED. The native
// CPU geometry + LightsData feed is now unconditional under the VK mech island (the GL
// glGetBufferSubData readback fallback was deleted). The env name is still parsed +
// noted as deprecated in gos_mech_batcher.cpp; there is no per-frame gate to query here.

// VK-MECH-FEED-AUDIT-1: verification-only (default-OFF, safety-gate class like
// MC2_VK_TERRAIN_HEIGHT_AUDIT). With the native feed armed, ALSO read the GL
// buffers back and byte-compare against the CPU sources -- proves the native
// feed carries the exact bytes GL rendered with.
bool mechFeedAuditRequested() {
    static int state = -1;
    if (state == -1)
        state = (mc2vkgate::envTriState("MC2_VK_MECH_FEED_AUDIT") == 1) ? 1 : 0;
    return state == 1;
}

// VK-MECH-FEED-AUDIT-1 tallies (reported via vlog; nonzero mismatch = feed bug).
struct FeedNativeStats {
    unsigned long nativeGeomCopies  = 0;   // geometry copied from CPU source
    unsigned long nativeLightFrames = 0;   // LightsData frames copied from CPU source
    unsigned long auditRuns         = 0;
    unsigned long auditMismatches   = 0;
};
FeedNativeStats g_feedNative;

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

// std140 per-frame UBO (LOCKSTEP with mech_tier1.vert MechFrameUBO).
struct MechFrameUBO {
    float   worldToClipGL[16]; // gos_GetMechFixBMVPMat4() — GL_FALSE (off 0)
    float   objectDepthBias;   //                            (off 64)
    int32_t materialReady;     // 1 = sample the copy-once albedo (off 68)
    int32_t lightingReady;     // 1 = LightsData copied -> calc_light valid (off 72)
    int32_t _pad1;             //                            (off 76)
    float   fogColorRGB[4];    // g_scene.fogColor.rgb (.a unused) — vec4-aligned (off 80)
};

const VkFormat kColorFmt = VK_FORMAT_R16G16B16A16_SFLOAT;

struct MechIsland {
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

    // Shared-GBuffer co-write objects (over the SAME shared color/normal/depth
    // images the GL scene renders terrain into). Images are BORROWED.
    VkRenderPass  rpass = VK_NULL_HANDLE;
    VkFramebuffer fb    = VK_NULL_HANDLE;
    VkPipeline    pipe  = VK_NULL_HANDLE;
    VkShaderModule vert = VK_NULL_HANDLE;
    VkShaderModule frag = VK_NULL_HANDLE;
    struct VkImageResource* colorRes  = nullptr;
    struct VkImageResource* normalRes = nullptr;
    struct VkImageResource* depthRes  = nullptr;
    struct GlInteropView*   colorView  = nullptr;
    struct GlInteropView*   normalView = nullptr;
    struct GlInteropView*   depthView  = nullptr;

    // Copy-once mech geometry (VK-owned copies of the shared mech VBO/IBO).
    VkBuffer vbo = VK_NULL_HANDLE; VmaAllocation vboAlloc = VK_NULL_HANDLE;
    VkBuffer ibo = VK_NULL_HANDLE; VmaAllocation iboAlloc = VK_NULL_HANDLE;
    uint32_t vertexStride = 48;
    bool     geomUploaded = false;

    // Per-frame instance + bone SSBOs (host-visible; grown as needed).
    VkBuffer instBuf = VK_NULL_HANDLE; VmaAllocation instAlloc = VK_NULL_HANDLE; VkDeviceSize instCap = 0;
    VkBuffer boneBuf = VK_NULL_HANDLE; VmaAllocation boneAlloc = VK_NULL_HANDLE; VkDeviceSize boneCap = 0;
    VkBuffer ubo     = VK_NULL_HANDLE; VmaAllocation uboAlloc  = VK_NULL_HANDLE;

    // VK-MECH-MATERIAL-LIGHTING-1: per-frame LightsData SSBO copy (binding 4) for calc_light.
    VkBuffer lightBuf = VK_NULL_HANDLE; VmaAllocation lightAlloc = VK_NULL_HANDLE; VkDeviceSize lightCap = 0;
    bool     wantLighting  = false;   // MC2_VK_MECH_LIGHTING gate
    bool     lightingReady = false;   // LightsData copied this frame (>=1 ObjectLights)

    // Descriptors: 0=UBO(VS/FS), 1=instance SSBO(VS), 2=bone SSBO(VS), 3=albedo(FS), 4=LightsData(VS).
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkDescriptorPool  pool   = VK_NULL_HANDLE;
    VkDescriptorSet   set    = VK_NULL_HANDLE;
    VkPipelineLayout  playout = VK_NULL_HANDLE;

    // MC2_VK_MECH_MATERIAL: per-actor paint-scheme VK-descriptor cache. Each distinct
    // slot-0 GL paint texture gets its own copy-once VkImage/view + a descriptor set
    // (all 5 bindings; only binding-3 albedo differs). The draw loop binds the set that
    // matches run.slot0GlTex, so a mission with >1 distinct scheme shows the right
    // scheme on each mech (the single-slot-0 probe showed the FIRST scheme on all).
    // A dummy 1x1 white scheme (glTex sentinel 0) backs runs with no paint texture.
    bool        wantMaterial  = false;
    bool        materialReady = false;   // >=1 real scheme imported
    VkSampler   albedoSamp = VK_NULL_HANDLE;
    struct SchemeEntry {
        uint32_t      glTex = 0;                 // 0 = dummy white
        VkImage       img   = VK_NULL_HANDLE;
        VmaAllocation alloc = VK_NULL_HANDLE;
        VkImageView   view  = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
    };
    std::vector<SchemeEntry> schemes;    // [0] = dummy white, always present
    static const uint32_t    kMaxSchemes = 32;   // descriptor-pool budget

    VkCommandPool   cpool  = VK_NULL_HANDLE;
    VkCommandBuffer cbuf[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};

    // VK-DRAW-PERF-1: per-pass GPU timestamp timer (3 stamps: pass-start / draws-start /
    // pass-end). Gated + fail-soft inside init(); no-op unless MC2_VK_GPU_TIMESTAMPS set.
    mc2vkts::GpuTimer gpuTimer;
};

MechIsland* g_island = nullptr;

bool make_buffer(MechIsland* s, VkDeviceSize size, VkBufferUsageFlags usage,
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

// Write the four per-frame BUFFER bindings (0 UBO / 1 inst / 2 bone / 4 lights) into
// ONE descriptor set. binding-3 (albedo) is written separately per scheme by
// bind_scheme_albedo — this keeps buffer-grow and albedo-import independent.
void write_buffer_bindings(MechIsland* s, VkDescriptorSet set) {
    if (set == VK_NULL_HANDLE) return;
    VkDescriptorBufferInfo uboInfo { s->ubo,     0, VK_WHOLE_SIZE };
    VkDescriptorBufferInfo instInfo{ s->instBuf, 0, VK_WHOLE_SIZE };
    VkDescriptorBufferInfo boneInfo{ s->boneBuf, 0, VK_WHOLE_SIZE };
    VkDescriptorBufferInfo lightInfo{ s->lightBuf, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet w[4]{}; uint32_t n = 0;
    w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n].dstSet = set;
    w[n].dstBinding = 0; w[n].descriptorCount = 1;
    w[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[n].pBufferInfo = &uboInfo; ++n;
    w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n].dstSet = set;
    w[n].dstBinding = 1; w[n].descriptorCount = 1;
    w[n].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[n].pBufferInfo = &instInfo; ++n;
    w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n].dstSet = set;
    w[n].dstBinding = 2; w[n].descriptorCount = 1;
    w[n].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[n].pBufferInfo = &boneInfo; ++n;
    if (s->lightBuf != VK_NULL_HANDLE) {
        w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n].dstSet = set;
        w[n].dstBinding = 4; w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[n].pBufferInfo = &lightInfo; ++n;
    }
    vkUpdateDescriptorSets(s->device, n, w, 0, nullptr);
}

// Re-write buffer bindings into EVERY cached scheme set (after a buffer grow swaps a
// VkBuffer handle). Albedo binding-3 in each set is untouched (per-scheme, stable).
void write_buffer_bindings_all(MechIsland* s) {
    for (auto& e : s->schemes) write_buffer_bindings(s, e.set);
}

// Point a scheme set's binding-3 at its own albedo image + the shared sampler.
void bind_scheme_albedo(MechIsland* s, VkDescriptorSet set, VkImageView view) {
    if (set == VK_NULL_HANDLE || view == VK_NULL_HANDLE || s->albedoSamp == VK_NULL_HANDLE) return;
    VkDescriptorImageInfo albInfo{};
    albInfo.sampler = s->albedoSamp; albInfo.imageView = view;
    albInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet w{};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.dstSet = set;
    w.dstBinding = 3; w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &albInfo;
    vkUpdateDescriptorSets(s->device, 1, &w, 0, nullptr);
}

bool ensure_buf_capacity(MechIsland* s, VkBuffer* buf, VmaAllocation* alloc, VkDeviceSize* cap,
                         VkDeviceSize bytes) {
    if (*buf != VK_NULL_HANDLE && bytes <= *cap) return true;
    if (*buf != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(s->device);
        vmaDestroyBuffer(s->allocator, *buf, *alloc);
        *buf = VK_NULL_HANDLE;
    }
    VkDeviceSize c = bytes + bytes / 2 + 4096;
    if (!make_buffer(s, c, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, buf, alloc, true)) return false;
    *cap = c;
    write_buffer_bindings_all(s);
    return true;
}

// MC2_VK_MECH_MATERIAL: one-shot command helpers + copy-once single-2D albedo import.
VkCommandBuffer cp_begin(MechIsland* s) {
    VkCommandBufferAllocateInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool = s->cpool; cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbi.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(s->device, &cbi, &cb) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkCommandBufferBeginInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    return cb;
}
void cp_end(MechIsland* s, VkCommandBuffer cb) {
    vkEndCommandBuffer(cb);
    VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    vkQueueSubmit(s->queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(s->queue);
    vkFreeCommandBuffers(s->device, s->cpool, 1, &cb);
}

bool make_albedo_sampler(MechIsland* s) {
    if (s->albedoSamp != VK_NULL_HANDLE) return true;
    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    return vkCreateSampler(s->device, &sci, nullptr, &s->albedoSamp) == VK_SUCCESS;
}

// Upload a single 2D RGBA8 image (w*h*4 bytes) into a fresh VkImage+view stored in the
// given SchemeEntry (per-actor paint-scheme cache slot).
bool upload_scheme_image(MechIsland* s, MechIsland::SchemeEntry& e,
                         uint32_t w, uint32_t h, const void* bytes, VkDeviceSize byteCount) {
    VkBuffer stg = VK_NULL_HANDLE; VmaAllocation stgAlloc = VK_NULL_HANDLE;
    if (!make_buffer(s, byteCount, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &stg, &stgAlloc, true)) return false;
    { void* p = nullptr;
      if (vmaMapMemory(s->allocator, stgAlloc, &p) != VK_SUCCESS) { vmaDestroyBuffer(s->allocator, stg, stgAlloc); return false; }
      std::memcpy(p, bytes, (size_t)byteCount); vmaUnmapMemory(s->allocator, stgAlloc); }

    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D; ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {w, h, 1}; ici.mipLevels = 1; ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo aci{}; aci.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateImage(s->allocator, &ici, &aci, &e.img, &e.alloc, nullptr) != VK_SUCCESS) {
        vmaDestroyBuffer(s->allocator, stg, stgAlloc); return false;
    }
    VkCommandBuffer cb = cp_begin(s);
    if (cb == VK_NULL_HANDLE) { vmaDestroyBuffer(s->allocator, stg, stgAlloc); return false; }
    VkImageMemoryBarrier bar{};
    bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; bar.image = e.img;
    bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    bar.srcAccessMask = 0; bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &bar);
    VkBufferImageCopy r{};
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(cb, stg, e.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &bar);
    cp_end(s, cb);
    vmaDestroyBuffer(s->allocator, stg, stgAlloc);

    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = e.img; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = VK_FORMAT_R8G8B8A8_UNORM;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return vkCreateImageView(s->device, &vci, nullptr, &e.view) == VK_SUCCESS;
}

// Allocate a descriptor set for a scheme (all 5 bindings), write buffer bindings + its
// albedo. Returns the set or VK_NULL_HANDLE on failure.
VkDescriptorSet alloc_scheme_set(MechIsland* s, VkImageView view) {
    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = s->pool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &s->dsl;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(s->device, &dsai, &set) != VK_SUCCESS) return VK_NULL_HANDLE;
    write_buffer_bindings(s, set);
    bind_scheme_albedo(s, set, view);
    return set;
}

// Create the always-present dummy white scheme (glTex sentinel 0). schemes[0].
bool ensure_dummy_scheme(MechIsland* s) {
    if (!make_albedo_sampler(s)) return false;
    if (!s->schemes.empty()) return true;
    MechIsland::SchemeEntry e; e.glTex = 0;
    const uint8_t px[4] = {255, 255, 255, 255};   // white -> flat FS uses vArgb only
    if (!upload_scheme_image(s, e, 1, 1, px, sizeof(px))) return false;
    e.set = alloc_scheme_set(s, e.view);
    if (e.set == VK_NULL_HANDLE) { vkDestroyImageView(s->device, e.view, nullptr);
                                   vmaDestroyImage(s->allocator, e.img, e.alloc); return false; }
    s->schemes.push_back(e);
    return true;
}

// Per-actor paint-scheme cache lookup. Returns the descriptor set to bind for a run's
// slot-0 GL paint texture, importing it copy-once on first sight. Keyed on the GL handle
// so a mission with >1 distinct scheme gets one VK image + descriptor set per scheme.
// Copy-once GL->VK texture import (glGetTexImage on a MATERIAL texture, NOT a color
// surface) — does NOT touch g_forbidden, mirrors the terrain/prop material imports.
VkDescriptorSet get_scheme_set(MechIsland* s, uint32_t glTex) {
    // 0 or unresolved -> dummy white (schemes[0]).
    if (glTex == 0) return s->schemes.empty() ? VK_NULL_HANDLE : s->schemes[0].set;
    for (auto& e : s->schemes) if (e.glTex == glTex) return e.set;
    if (s->schemes.size() >= MechIsland::kMaxSchemes) {
        // Budget exhausted — fall back to dummy so the mech still draws (unlit-ish).
        return s->schemes.empty() ? VK_NULL_HANDLE : s->schemes[0].set;
    }
    glBindTexture(GL_TEXTURE_2D, (GLuint)glTex);
    GLint w = 0, h = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) { glBindTexture(GL_TEXTURE_2D, 0);
        return s->schemes.empty() ? VK_NULL_HANDLE : s->schemes[0].set; }
    std::vector<uint8_t> blob((size_t)w * (size_t)h * 4u);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, blob.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    MechIsland::SchemeEntry e; e.glTex = glTex;
    if (!upload_scheme_image(s, e, (uint32_t)w, (uint32_t)h, blob.data(), (VkDeviceSize)blob.size())) {
        return s->schemes.empty() ? VK_NULL_HANDLE : s->schemes[0].set;
    }
    e.set = alloc_scheme_set(s, e.view);
    if (e.set == VK_NULL_HANDLE) { vkDestroyImageView(s->device, e.view, nullptr);
        vmaDestroyImage(s->allocator, e.img, e.alloc);
        return s->schemes.empty() ? VK_NULL_HANDLE : s->schemes[0].set; }
    s->schemes.push_back(e);
    s->materialReady = true;
    vlog("paint-scheme cached: %dx%d glTex=%u (scheme %zu/%u).", w, h, glTex,
         s->schemes.size() - 1, MechIsland::kMaxSchemes);
    return e.set;
}

void destroy_island(MechIsland* s) {
    if (!s) return;
    VkDevice d = s->device;
    if (d != VK_NULL_HANDLE) vkDeviceWaitIdle(d);
    if (s->pipe)  vkDestroyPipeline(d, s->pipe, nullptr);
    if (s->vert)  vkDestroyShaderModule(d, s->vert, nullptr);
    if (s->frag)  vkDestroyShaderModule(d, s->frag, nullptr);
    if (s->fb)    vkDestroyFramebuffer(d, s->fb, nullptr);
    if (s->rpass) vkDestroyRenderPass(d, s->rpass, nullptr);
    if (s->playout) vkDestroyPipelineLayout(d, s->playout, nullptr);
    if (s->dsl)   vkDestroyDescriptorSetLayout(d, s->dsl, nullptr);
    if (s->pool)  vkDestroyDescriptorPool(d, s->pool, nullptr);
    if (s->vbo)     vmaDestroyBuffer(s->allocator, s->vbo, s->vboAlloc);
    if (s->ibo)     vmaDestroyBuffer(s->allocator, s->ibo, s->iboAlloc);
    if (s->instBuf) vmaDestroyBuffer(s->allocator, s->instBuf, s->instAlloc);
    if (s->boneBuf) vmaDestroyBuffer(s->allocator, s->boneBuf, s->boneAlloc);
    if (s->lightBuf) vmaDestroyBuffer(s->allocator, s->lightBuf, s->lightAlloc);
    if (s->ubo)     vmaDestroyBuffer(s->allocator, s->ubo, s->uboAlloc);
    for (auto& e : s->schemes) {
        if (e.view) vkDestroyImageView(d, e.view, nullptr);
        if (e.img)  vmaDestroyImage(s->allocator, e.img, e.alloc);
    }
    s->schemes.clear();
    if (s->albedoSamp) vkDestroySampler(d, s->albedoSamp, nullptr);
    if (s->cpool) vkDestroyCommandPool(d, s->cpool, nullptr);
    // instance/phys/device/queue/allocator + shared images are BORROWED.
}

bool init_island(MechIsland* s, int w, int h) {
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
    }

    s->wantMaterial = mechMaterialRequested();
    s->wantLighting = mechLightingRequested();

    // Fetch the SHARED scene color/normal/depth exportable images the GL scene renders
    // terrain into. Fail-soft: any missing => GL fallback (the GL mech flush runs).
    s->colorRes  = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_COLOR0);
    s->normalRes = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_NORMAL);
    s->depthRes  = mc2SharedSceneDepthGetResource();
    s->colorView  = mc2SharedSceneColorGetGlView(MC2_SHARED_SCENE_COLOR0);
    s->normalView = mc2SharedSceneColorGetGlView(MC2_SHARED_SCENE_NORMAL);
    s->depthView  = mc2SharedSceneDepthGetGlView();
    const bool directChain = mechDirectChainRequested();
    if (!s->colorRes || !s->normalRes || !s->depthRes ||
        (!directChain && (!s->colorView || !s->normalView || !s->depthView))) {
        setFallback("shared_gbuffer_not_ready"); return false;
    }
    g_health.interopActive = directChain ? 0 : 1;
    g_health.vk2glSemActive =
        (!directChain && s->colorView &&
         reinterpret_cast<GlInteropView*>(s->colorView)->vkToGlImported) ? 1 : 0;

    // Instance + bone + UBO buffers.
    if (!ensure_buf_capacity(s, &s->instBuf, &s->instAlloc, &s->instCap, 256 * 1024)) return false;
    if (!ensure_buf_capacity(s, &s->boneBuf, &s->boneAlloc, &s->boneCap, 256 * 1024)) return false;
    // LightsData copy buffer — valid before the first frame so binding4 is always bound
    // (one ObjectLights record is 3600B; start with a few slots).
    if (!ensure_buf_capacity(s, &s->lightBuf, &s->lightAlloc, &s->lightCap, 32 * 1024)) return false;
    if (!make_buffer(s, sizeof(MechFrameUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &s->ubo, &s->uboAlloc, true)) return false;
    // Pre-create tiny inst/bone SSBOs so scheme descriptor sets never bind a null buffer
    // (they grow in run() via ensure_buf_capacity, which rewrites all cached sets).
    if (!ensure_buf_capacity(s, &s->instBuf, &s->instAlloc, &s->instCap, 64)) return false;
    if (!ensure_buf_capacity(s, &s->boneBuf, &s->boneAlloc, &s->boneCap, 64)) return false;

    // Command pool + 2 command buffers (parity).
    {
        VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = s->gfxFamily;
        if (vkCreateCommandPool(s->device, &pci, nullptr, &s->cpool) != VK_SUCCESS) { vlog("cmd pool failed."); return false; }
        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = s->cpool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 2;
        if (vkAllocateCommandBuffers(s->device, &cai, s->cbuf) != VK_SUCCESS) { vlog("cmd bufs failed."); return false; }
    }

    // VK-DRAW-PERF-1: 3-stamp GPU timer (pass-start / draws-start / pass-end).
    // No-op unless MC2_VK_GPU_TIMESTAMPS is set; fail-soft if timestamps unsupported.
    if (mc2vkts::timestampsEnabled()) {
        if (s->gpuTimer.init(s->device, s->phys, s->gfxFamily, 3))
            vlog("GPU timestamps ON (period=%.2f ns): pass segments = setup / mech draws.",
                 s->gpuTimer.periodNs);
        else
            vlog("GPU timestamps requested but unsupported on this queue (validBits=0 or period<=0).");
    }

    // The per-scheme descriptor sets (incl. the dummy white schemes[0]) are created in
    // the descriptor-pool block below (needs s->pool). binding3 is always valid there.

    // Render pass: 2 color LOAD (preserve GL terrain in shared color/normal) + shared
    // depth LOAD (D32S8, GEQUAL test+write). NO clear. Same shape as the prop island Tier-1.
    {
        VkImageResource* colorRes  = reinterpret_cast<VkImageResource*>(s->colorRes);
        VkImageResource* normalRes = reinterpret_cast<VkImageResource*>(s->normalRes);
        VkImageResource* depthRes  = reinterpret_cast<VkImageResource*>(s->depthRes);
        VkAttachmentDescription att[3]{};
        att[0].format = colorRes->format;  att[0].samples = VK_SAMPLE_COUNT_1_BIT;
        att[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; att[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att[1].format = normalRes->format; att[1].samples = VK_SAMPLE_COUNT_1_BIT;
        att[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[1].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; att[1].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att[2].format = depthRes->format;  att[2].samples = VK_SAMPLE_COUNT_1_BIT;
        att[2].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[2].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL; att[2].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentReference colorRefs[2] = {
            {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL} };
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
        deps[1].dstStageMask  = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT; deps[1].dstAccessMask = 0;

        VkRenderPassCreateInfo rpci{};
        rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpci.attachmentCount = 3; rpci.pAttachments = att;
        rpci.subpassCount = 1; rpci.pSubpasses = &sub;
        rpci.dependencyCount = 2; rpci.pDependencies = deps;
        if (vkCreateRenderPass(s->device, &rpci, nullptr, &s->rpass) != VK_SUCCESS) { setFallback("renderpass_failed"); return false; }
        VkImageView fbViews[3] = { colorRes->view, normalRes->view, depthRes->view };
        VkFramebufferCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = s->rpass; fci.attachmentCount = 3; fci.pAttachments = fbViews;
        fci.width = (uint32_t)w; fci.height = (uint32_t)h; fci.layers = 1;
        if (vkCreateFramebuffer(s->device, &fci, nullptr, &s->fb) != VK_SUCCESS) { setFallback("framebuffer_failed"); return false; }
    }

    // Descriptor layout: 0=UBO(VS/FS), 1=instance SSBO(VS), 2=bone SSBO(VS), 3=albedo(FS).
    {
        VkDescriptorSetLayoutBinding b[5]{};
        b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        b[2].binding = 2; b[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[2].descriptorCount = 1; b[2].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        b[3].binding = 3; b[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[3].descriptorCount = 1; b[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        b[4].binding = 4; b[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;   // LightsData
        b[4].descriptorCount = 1; b[4].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.bindingCount = 5; lci.pBindings = b;
        if (vkCreateDescriptorSetLayout(s->device, &lci, nullptr, &s->dsl) != VK_SUCCESS) { vlog("dsl failed."); return false; }
        // Pool sized for kMaxSchemes descriptor sets (one per distinct paint scheme):
        // each set = 1 UBO + 3 SSBO (inst/bone/lights) + 1 sampler.
        const uint32_t N = MechIsland::kMaxSchemes;
        VkDescriptorPoolSize sizes[3]{};
        sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; sizes[0].descriptorCount = 1 * N;
        sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; sizes[1].descriptorCount = 3 * N;  // inst+bone+lights
        sizes[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; sizes[2].descriptorCount = 1 * N;
        VkDescriptorPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.maxSets = N; pci.poolSizeCount = 3; pci.pPoolSizes = sizes;
        if (vkCreateDescriptorPool(s->device, &pci, nullptr, &s->pool) != VK_SUCCESS) { vlog("pool failed."); return false; }
        // Always-present dummy white scheme (schemes[0]) = binding-3 fallback for runs
        // with no slot-0 paint texture (and the material-OFF flat-fill path).
        if (!ensure_dummy_scheme(s)) { setFallback("dummy_scheme_failed"); return false; }
    }

    // Pipeline: mech 48B vertex stream, instanced, GEQUAL reverse-Z test+write, cull NONE,
    // 2 GBuffer outputs, no blend (opaque in-place overwrite).
    {
        s->vert = load_spv(s->device, spv_dir() + "mech_tier1.vert.spv");
        s->frag = load_spv(s->device, spv_dir() + "mech_tier1.frag.spv");
        if (s->vert == VK_NULL_HANDLE || s->frag == VK_NULL_HANDLE) { setFallback("spv_load_failed"); return false; }

        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 1; plci.pSetLayouts = &s->dsl;
        if (vkCreatePipelineLayout(s->device, &plci, nullptr, &s->playout) != VK_SUCCESS) { vlog("playout failed."); return false; }

        // Mirror GpuMechVertex (48B): loc0 pos vec3@0, loc1 normal vec3@12, loc2 uv vec2@24,
        // loc3 boneIndices uvec4 (u8x4)@32, loc4 boneWeights vec4 (u8x4 normalized)@36,
        // loc5 tangentOct (s16x2 norm)@40, loc6 aRGBLight uint@44.
        VkVertexInputBindingDescription vib{};
        vib.binding = 0; vib.stride = s->vertexStride ? s->vertexStride : 48;
        vib.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        VkVertexInputAttributeDescription via[7]{};
        via[0].location = 0; via[0].binding = 0; via[0].format = VK_FORMAT_R32G32B32_SFLOAT; via[0].offset = 0;
        via[1].location = 1; via[1].binding = 0; via[1].format = VK_FORMAT_R32G32B32_SFLOAT; via[1].offset = 12;
        via[2].location = 2; via[2].binding = 0; via[2].format = VK_FORMAT_R32G32_SFLOAT;    via[2].offset = 24;
        via[3].location = 3; via[3].binding = 0; via[3].format = VK_FORMAT_R8G8B8A8_UINT;    via[3].offset = 32; // uvec4
        via[4].location = 4; via[4].binding = 0; via[4].format = VK_FORMAT_R8G8B8A8_UNORM;   via[4].offset = 36; // vec4 [0,1]
        via[5].location = 5; via[5].binding = 0; via[5].format = VK_FORMAT_R16G16_SNORM;     via[5].offset = 40; // vec2 [-1,1]
        via[6].location = 6; via[6].binding = 0; via[6].format = VK_FORMAT_R32_UINT;         via[6].offset = 44;
        VkPipelineVertexInputStateCreateInfo vin{};
        vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vin.vertexBindingDescriptionCount = 1; vin.pVertexBindingDescriptions = &vib;
        vin.vertexAttributeDescriptionCount = 7; vin.pVertexAttributeDescriptions = via;

        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo vps{};
        vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vps.viewportCount = 1; vps.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;   // GL mech color pass does not hard-cull
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo ds{};
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;   // reverse-Z, matches GL mech opaque

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
            setFallback("pipeline_failed"); return false;
        }
    }

    s->inited = true;
    setFallback("");
    vlog("island initialized %dx%d (borrowed shared device; skinned mech co-write into "
         "shared color(MRT0)+normal(MRT1)+depth; GEQUAL reverse-Z; material=%d lighting=%d).",
         w, h, s->wantMaterial ? 1 : 0, s->wantLighting ? 1 : 0);
    return true;
}

// Copy the shared mech VBO/IBO into VK-owned buffers ONCE per map.
bool upload_geometry_once(MechIsland* s) {
    if (s->geomUploaded) return true;
    Mc2MechGeomFeed feed{};
    if (!mc2_mech_island_get_geometry_feed(&feed)) return false;
    if (feed.vboBytes == 0 || feed.iboBytes == 0 || feed.glVbo == 0 || feed.glIbo == 0) return false;
    // VK-RUNG8 item-9: the CPU byte sources are now the ONLY mech geometry feed; the
    // GL glGetBufferSubData readback fallback was deleted. If the CPU sources aren't
    // ready yet (a normals rebuild raced the batcher accessor -> vboCpu null), decline
    // this frame and retry next frame -- do NOT allocate the VK buffers first. This is
    // the explicit fail-hard-with-retry replacement for the old per-frame GL rescue.
    if (!feed.vboCpu || !feed.iboCpu) return false;
    s->vertexStride = feed.vertexStride;

    if (!make_buffer(s, feed.vboBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &s->vbo, &s->vboAlloc, true)) return false;
    if (!make_buffer(s, feed.iboBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,  &s->ibo, &s->iboAlloc, true)) return false;

    { void* p = nullptr;
      if (vmaMapMemory(s->allocator, s->vboAlloc, &p) == VK_SUCCESS) {
          std::memcpy(p, feed.vboCpu, (size_t)feed.vboBytes);
          vmaUnmapMemory(s->allocator, s->vboAlloc);
      } }
    { void* p = nullptr;
      if (vmaMapMemory(s->allocator, s->iboAlloc, &p) == VK_SUCCESS) {
          std::memcpy(p, feed.iboCpu, (size_t)feed.iboBytes);
          vmaUnmapMemory(s->allocator, s->iboAlloc);
      } }
    ++g_feedNative.nativeGeomCopies;
    // VK-MECH-FEED-AUDIT-1: verification-only byte-compare of the CPU sources vs the
    // GL buffers (proves the native feed carries the exact bytes GL rendered with).
    if (mechFeedAuditRequested()) {
        ++g_feedNative.auditRuns;
        unsigned long mism = 0;
        std::vector<uint8_t> scratch;
        scratch.resize((size_t)feed.vboBytes);
        glBindBuffer(GL_ARRAY_BUFFER, feed.glVbo);
        glGetBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)feed.vboBytes, scratch.data());
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        if (std::memcmp(scratch.data(), feed.vboCpu, (size_t)feed.vboBytes) != 0) ++mism;
        scratch.resize((size_t)feed.iboBytes);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, feed.glIbo);
        glGetBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, (GLsizeiptr)feed.iboBytes, scratch.data());
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        if (std::memcmp(scratch.data(), feed.iboCpu, (size_t)feed.iboBytes) != 0) ++mism;
        g_feedNative.auditMismatches += mism;
        vlog("FEED-AUDIT geometry: vbo=%llu B ibo=%llu B mismatch_buffers=%lu %s",
             (unsigned long long)feed.vboBytes, (unsigned long long)feed.iboBytes,
             mism, mism ? "*** NATIVE FEED DIVERGES FROM GL ***" : "(byte-identical)");
    }
    g_health.geomVerts = (unsigned long)(feed.vboBytes / (feed.vertexStride ? feed.vertexStride : 1));
    s->geomUploaded = true;
    vlog("mech geometry uploaded once: vbo=%llu B ibo=%llu B stride=%u source=cpu-native.",
         (unsigned long long)feed.vboBytes, (unsigned long long)feed.iboBytes, feed.vertexStride);
    return true;
}

} // namespace

// ===========================================================================
// PUBLIC ENTRY POINTS.
// ===========================================================================

extern "C" int mc2_vk_mech_island_enabled() {
    return mechGateRequested() ? 1 : 0;
}

extern "C" int mc2_vk_mech_island_run(int w, int h) {
    ZoneScopedN("VK-mech-island");   // VK-FLIP-FIELD-REPORT-1 coarse region zone
    if (!mechGateRequested()) return 0;
    if (w <= 0 || h <= 0) return 0;

    using clk = std::chrono::steady_clock;
    auto usSince = [](clk::time_point t0){ return std::chrono::duration<double, std::micro>(clk::now() - t0).count(); };
    const clk::time_point tTotal0 = clk::now();

    Mc2MechFrameFeed feed{};
    if (!mc2_mech_island_get_frame_feed(&feed)) { setFallback("no_frame_feed"); return 0; }
    if (!feed.instances64 || feed.instanceCount == 0 || !feed.runs || feed.runCount == 0) {
        setFallback("no_mech_instances"); return 0;
    }

    // VK-MECH-CAPTURE-DETERMINISM-1: frame-stamped cull-parity witness. feed.instanceCount
    // is s_vkMechInstSnapshot.size() == the GL batcher's totalInstances for THIS frame
    // (assigned in the same flush, gos_mech_batcher.cpp:2796) so it is directly comparable
    // to GL's [MECHBATCHER v1] event=summary instances=. Gated by the same stats env so
    // controls stay clean (no default-on spam). extern frame counter (tgl.cpp).
    {
        static const bool s_cullWitness = (getenv("MC2_MECH_BATCHER_STATS") != nullptr);
        if (s_cullWitness) {
            // Ordinal = per-drawn-frame (usedVulkan+1); the GL batcher builds one feed
            // per frame the island draws, so the ordered sequence of vk_instances must
            // equal the batcher's instances= sequence (equal every frame by construction).
            std::fprintf(stderr, "[VK_MECH_ISLAND] event=cull_witness seq=%lu vk_instances=%u runs=%u\n",
                         g_health.usedVulkan + 1, feed.instanceCount, feed.runCount);
        }
    }

    if (g_island && (g_island->w != w || g_island->h != h)) {
        destroy_island(g_island); delete g_island; g_island = nullptr;
    }
    if (!g_island) {
        g_island = new MechIsland();
        if (!init_island(g_island, w, h)) {
            vlog("init failed (%s) — GL fallback.", g_health.fallbackReason);
            destroy_island(g_island); delete g_island; g_island = nullptr;
            return 0;
        }
    }
    MechIsland* s = g_island;
    if (s->disabled || !s->inited) return 0;

    if (!upload_geometry_once(s)) { setFallback("geometry_not_ready"); return 0; }

    // MC2_VK_MECH_MATERIAL: per-actor paint-scheme cache. Import each distinct run's
    // slot-0 paint scheme copy-once here (before recording), so get_scheme_set in the
    // draw loop is a pure cache lookup (no GL calls inside the VK command buffer).
    if (s->wantMaterial) {
        for (uint32_t r = 0; r < feed.runCount; ++r) {
            if (feed.runs[r].slot0GlTex != 0) get_scheme_set(s, feed.runs[r].slot0GlTex);
        }
    }

    const unsigned long f0b = g_forbidden.glGetTexImage;
    const unsigned long f0c = g_forbidden.glReadPixels;

    const clk::time_point tStage0 = clk::now();
    // Upload instance + bone snapshots.
    const VkDeviceSize instBytes = (VkDeviceSize)feed.instanceCount * 64ull;   // GpuMechInstance
    const VkDeviceSize boneBytes = (VkDeviceSize)(feed.boneCount ? feed.boneCount : 1) * 64ull; // GpuMechBone
    if (!ensure_buf_capacity(s, &s->instBuf, &s->instAlloc, &s->instCap, instBytes)) { s->disabled = true; return 0; }
    if (!ensure_buf_capacity(s, &s->boneBuf, &s->boneAlloc, &s->boneCap, boneBytes)) { s->disabled = true; return 0; }
    { ZoneScopedN("VK-mech-instance-bone-upload");   // VK-FLIP-FIELD-REPORT-1: per-frame skinning feed staging
    { void* p = nullptr;
      if (vmaMapMemory(s->allocator, s->instAlloc, &p) == VK_SUCCESS) { std::memcpy(p, feed.instances64, (size_t)instBytes); vmaUnmapMemory(s->allocator, s->instAlloc); } }
    if (feed.boneCount) { void* p = nullptr;
      if (vmaMapMemory(s->allocator, s->boneAlloc, &p) == VK_SUCCESS) { std::memcpy(p, feed.bones64, (size_t)(feed.boneCount * 64ull)); vmaUnmapMemory(s->allocator, s->boneAlloc); } } }
    g_health.instRecords = feed.instanceCount;
    g_health.boneRecords = feed.boneCount;

    // VK-MECH-MATERIAL-LIGHTING-1: per-frame copy of the live GL LightsData SSBO into
    // the VK SSBO (binding 4) so calc_light reads the same ObjectLights[] the GL mech VS
    // does. Copy-per-frame (light cache is per-actor/per-frame). glGetBufferSubData is a
    // buffer readback, NOT a color-surface readback — does NOT touch g_forbidden (mirrors
    // the once/frame geometry+bone copies already on this path).
    s->lightingReady = false;
    // VK-RUNG8 item-9: the CPU LightsData array (the byte source of the GL SSBO upload)
    // is now the ONLY per-frame lighting feed; the per-frame glGetBufferSubData GL SSBO
    // readback fallback was deleted. If the CPU array is not ready this frame, lighting
    // is simply not marked ready (the shader falls back to flat-lit via u.lightingReady=0)
    // -- no GL readback rescue.
    const bool nativeLight = feed.lightDataCpu && feed.lightDataCpuBytes >= 3600ull;
    if (s->wantLighting && nativeLight) {
        const VkDeviceSize lbytes = (VkDeviceSize)feed.lightDataCpuBytes;
        if (ensure_buf_capacity(s, &s->lightBuf, &s->lightAlloc, &s->lightCap, lbytes)) {
            void* p = nullptr;
            if (vmaMapMemory(s->allocator, s->lightAlloc, &p) == VK_SUCCESS) {
                std::memcpy(p, feed.lightDataCpu, (size_t)lbytes);
                vmaUnmapMemory(s->allocator, s->lightAlloc);
                s->lightingReady = true;
                ++g_feedNative.nativeLightFrames;
            }
        }
        // VK-MECH-FEED-AUDIT-1: byte-compare the shared prefix of the CPU array vs
        // the GL SSBO (sizes may differ: the GL SSBO is capacity-sized / uploaded at
        // renderLists, the CPU count is live -- compare min()).
        if (s->lightingReady && mechFeedAuditRequested()
            && feed.lightDataGlSsbo != 0 && feed.lightDataBytes >= 3600ull) {
            ++g_feedNative.auditRuns;
            const unsigned long long cmp64 =
                feed.lightDataCpuBytes < feed.lightDataBytes ? feed.lightDataCpuBytes
                                                             : feed.lightDataBytes;
            const size_t cmp = (size_t)cmp64;
            std::vector<uint8_t> glCopy(cmp);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, (GLuint)feed.lightDataGlSsbo);
            glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)cmp, glCopy.data());
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
            const bool diff = std::memcmp(glCopy.data(), feed.lightDataCpu, cmp) != 0;
            if (diff) ++g_feedNative.auditMismatches;
            static unsigned long s_auditLogged = 0;
            if (diff || s_auditLogged < 3) {
                ++s_auditLogged;
                vlog("FEED-AUDIT lights: cpu=%llu B gl=%llu B cmp=%zu B %s "
                     "(runs=%lu mismatches=%lu)",
                     (unsigned long long)feed.lightDataCpuBytes,
                     (unsigned long long)feed.lightDataBytes, cmp,
                     diff ? "*** NATIVE FEED DIVERGES FROM GL ***" : "byte-identical",
                     g_feedNative.auditRuns, g_feedNative.auditMismatches);
            }
        }
    }

    // Upload UBO.
    { MechFrameUBO u{};
      std::memcpy(u.worldToClipGL, feed.worldToClipGL, sizeof(u.worldToClipGL));
      u.objectDepthBias = feed.objectDepthBias;
      u.materialReady = (s->wantMaterial && s->materialReady) ? 1 : 0;
      u.lightingReady = s->lightingReady ? 1 : 0;
      u.fogColorRGB[0] = feed.fogColorRGB[0];
      u.fogColorRGB[1] = feed.fogColorRGB[1];
      u.fogColorRGB[2] = feed.fogColorRGB[2];
      u.fogColorRGB[3] = 0.0f;
      void* p = nullptr;
      if (vmaMapMemory(s->allocator, s->uboAlloc, &p) == VK_SUCCESS) { std::memcpy(p, &u, sizeof(u)); vmaUnmapMemory(s->allocator, s->uboAlloc); } }
    g_health.stageUs = usSince(tStage0);

    // ---- Shared-GBuffer co-write. ----
    VkImageResource* colorRes  = reinterpret_cast<VkImageResource*>(s->colorRes);
    VkImageResource* normalRes = reinterpret_cast<VkImageResource*>(s->normalRes);
    VkImageResource* depthRes  = reinterpret_cast<VkImageResource*>(s->depthRes);
    GlInteropView*   colorView  = reinterpret_cast<GlInteropView*>(s->colorView);
    GlInteropView*   normalView = reinterpret_cast<GlInteropView*>(s->normalView);
    GlInteropView*   depthView  = reinterpret_cast<GlInteropView*>(s->depthView);

    const bool directChain = mechDirectChainRequested();
    VkFrameSync fsC = vkImageResourceBeginFrame(colorRes);
    VkFrameSync fsN = directChain ? VkFrameSync{} : vkImageResourceBeginFrame(normalRes);
    VkFrameSync fsD = directChain ? VkFrameSync{} : vkImageResourceBeginFrame(depthRes);
    VkCommandBuffer cb = s->cbuf[fsC.parity & 1u];

    if (!directChain) {
        glInteropSignalToVk(*colorView,  GL_LAYOUT_COLOR_ATTACHMENT_EXT,         fsC.parity);
        glInteropSignalToVk(*normalView, GL_LAYOUT_COLOR_ATTACHMENT_EXT,         fsN.parity);
        glInteropSignalToVk(*depthView,  GL_LAYOUT_DEPTH_STENCIL_ATTACHMENT_EXT, fsD.parity);
    }

    const clk::time_point tRender0 = clk::now();
    ZoneNamedN(zMechRec, "VK-mech-record-submit", true);   // VK-FLIP-FIELD-REPORT-1: CPU record+queue-submit (to fn return)
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) { setFallback("record_failed"); s->disabled = true; return 0; }

    // VK-DRAW-PERF-1: reset the query pool (outside the render pass) + stamp pass-start.
    s->gpuTimer.reset(cb);
    s->gpuTimer.write(cb, 0);

    VkRenderPassBeginInfo rbi{};
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = s->rpass; rbi.framebuffer = s->fb;
    rbi.renderArea = {{0, 0}, {(uint32_t)w, (uint32_t)h}};
    rbi.clearValueCount = 0; rbi.pClearValues = nullptr;   // all LOAD
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {(uint32_t)w, (uint32_t)h}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &scissor);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipe);
    VkDeviceSize vboOff = 0;
    vkCmdBindVertexBuffers(cb, 0, 1, &s->vbo, &vboOff);
    vkCmdBindIndexBuffer(cb, s->ibo, 0, VK_INDEX_TYPE_UINT32);

    // VK-DRAW-PERF-1: stamp draws-start (gap 0->1 = setup + binds).
    s->gpuTimer.write(cb, 1);

    // One indexed instanced draw per run. firstInstance = run.instanceBase so the VS's
    // gl_InstanceIndex indexes the instance SSBO snapshot exactly as the GL VS's
    // u_instanceBase + gl_InstanceID did. Per run: bind the descriptor set for this run's
    // paint scheme (per-actor cache) so the right albedo (binding 3) is sampled.
    VkDescriptorSet dummySet = s->schemes.empty() ? VK_NULL_HANDLE : s->schemes[0].set;
    VkDescriptorSet boundSet = VK_NULL_HANDLE;
    unsigned long runs = 0;
    for (uint32_t r = 0; r < feed.runCount; ++r) {
        const Mc2MechDrawRun& run = feed.runs[r];
        if (run.indexCount == 0 || run.instanceCount == 0) continue;
        VkDescriptorSet set = s->wantMaterial ? get_scheme_set(s, run.slot0GlTex) : dummySet;
        if (set == VK_NULL_HANDLE) set = dummySet;
        if (set != boundSet) {
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->playout, 0, 1, &set, 0, nullptr);
            boundSet = set;
        }
        vkCmdDrawIndexed(cb, run.indexCount, run.instanceCount, run.firstIndex,
                         run.baseVertex, run.instanceBase);
        ++runs;
    }
    g_health.runs = runs;

    vkCmdEndRenderPass(cb);
    // VK-DRAW-PERF-1: stamp pass-end (gap 1->2 = mech instanced draws).
    s->gpuTimer.write(cb, 2);
    if (vkEndCommandBuffer(cb) != VK_SUCCESS) { setFallback("record_failed"); s->disabled = true; return 0; }

    VkSemaphore waitSems[3]; VkPipelineStageFlags waitStages[3];
    VkSemaphore signalSems[3]; uint32_t waitN = 0, sigN = 0;
    auto addWait = [&](VkSemaphore sem, VkPipelineStageFlags stage){
        if (sem != VK_NULL_HANDLE) { waitSems[waitN] = sem; waitStages[waitN] = stage; ++waitN; } };
    if (!directChain) {
        addWait(fsC.signal, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        addWait(fsN.signal, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        addWait(fsD.signal, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);
    }
    const bool gpuC = !directChain && colorView &&
                      colorView->vkToGlImported && fsC.vk2gl != VK_NULL_HANDLE;
    const bool gpuN = !directChain && normalView &&
                      normalView->vkToGlImported && fsN.vk2gl != VK_NULL_HANDLE;
    const bool gpuD = !directChain && depthView &&
                      depthView->vkToGlImported && fsD.vk2gl != VK_NULL_HANDLE;
    if (!directChain) {
        if (gpuC) signalSems[sigN++] = fsC.vk2gl;
        if (gpuN) signalSems[sigN++] = fsN.vk2gl;
        if (gpuD) signalSems[sigN++] = fsD.vk2gl;
    }

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = waitN; si.pWaitSemaphores = waitSems; si.pWaitDstStageMask = waitStages;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    si.signalSemaphoreCount = sigN; si.pSignalSemaphores = signalSems;
    if (vkQueueSubmit(s->queue, 1, &si, fsC.fence) != VK_SUCCESS) { setFallback("submit_failed"); s->disabled = true; return 0; }

    if (!directChain) {
        if (gpuC) glInteropWaitFromVk(*colorView,  GL_LAYOUT_COLOR_ATTACHMENT_EXT,         fsC.parity);
        if (gpuN) glInteropWaitFromVk(*normalView, GL_LAYOUT_COLOR_ATTACHMENT_EXT,         fsN.parity);
        if (gpuD) glInteropWaitFromVk(*depthView,  GL_LAYOUT_DEPTH_STENCIL_ATTACHMENT_EXT, fsD.parity);
    }

    if (vkWaitForFences(s->device, 1, &fsC.fence, VK_TRUE, 5ull*1000*1000*1000) != VK_SUCCESS) {
        setFallback("fence_timeout"); s->disabled = true; return 0;
    }
    g_health.drawUs = usSince(tRender0);
    s->gpuTimer.resolve();   // VK-DRAW-PERF-1: fence signalled -> fold GPU segment times

    const bool readbackFired = (g_forbidden.glGetTexImage != f0b) || (g_forbidden.glReadPixels != f0c);
    if (readbackFired) {
        vlog("FATAL: a color readback fired on the co-write path — failing frame + disabling.");
        setFallback("interop_did_readback"); s->disabled = true; return 0;
    }

    g_health.vkDraws = 1;
    ++g_health.usedVulkan;
    g_health.totalUs = usSince(tTotal0);
    static bool s_firstOk = false;
    if (!s_firstOk) {
        vlog("first skinned mech co-write: instCount=%u boneCount=%u runs=%lu "
             "(co-written into shared color(MRT0)+normal(MRT1)+depth; no composite).",
             feed.instanceCount, feed.boneCount, g_health.runs);
        s_firstOk = true;
    }
    return 1;
}

extern "C" void mc2_render_backend_mech_region_health(
    int* vulkanAvailable, int* buildEnabled, const char** deviceName,
    unsigned long* attempted, unsigned long* usedVulkan, const char** fallbackReason,
    unsigned long* vkDraws, int* interopActive, int* vk2glSemActive,
    unsigned long* instRecords, unsigned long* boneRecords, unsigned long* runs,
    unsigned long* geomVerts,
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
    if (instRecords)      *instRecords      = g_health.instRecords;
    if (boneRecords)      *boneRecords      = g_health.boneRecords;
    if (runs)             *runs             = g_health.runs;
    if (geomVerts)        *geomVerts        = g_health.geomVerts;
    if (forbiddenGetTexImage) *forbiddenGetTexImage = g_forbidden.glGetTexImage;
    if (forbiddenReadPixels)  *forbiddenReadPixels  = g_forbidden.glReadPixels;
    if (stageUs)          *stageUs          = g_health.stageUs;
    if (drawUs)           *drawUs           = g_health.drawUs;
    if (totalUs)          *totalUs          = g_health.totalUs;
}

extern "C" void mc2_vk_mech_island_destroy() {
    vlog("[VK_MECH_TIER1] vulkan_available=%d used_vulkan=%lu interop_active=%d vk2gl_sem=%d "
         "inst_records=%lu bone_records=%lu runs=%lu geom_verts=%lu fallback_reason=\"%s\" "
         "vk_draws=%lu forbidden_getTexImage=%lu forbidden_readPixels=%lu "
         "stage_us=%.1f draw_us=%.1f total_us=%.1f",
         g_health.vulkanAvailable, g_health.usedVulkan, g_health.interopActive, g_health.vk2glSemActive,
         g_health.instRecords, g_health.boneRecords, g_health.runs, g_health.geomVerts,
         g_health.fallbackReason, g_health.vkDraws, g_forbidden.glGetTexImage, g_forbidden.glReadPixels,
         g_health.stageUs, g_health.drawUs, g_health.totalUs);
    // VK-DRAW-PERF-1: GPU-timestamp segment breakdown (only when MC2_VK_GPU_TIMESTAMPS
    // armed and supported; gap1 = setup+binds, gap2 = mech instanced draws).
    if (g_island && g_island->gpuTimer.supported && g_island->gpuTimer.samples > 0) {
        vlog("GPU_TIMESTAMPS samples=%llu gpu_pass_us=%.1f gpu_setup_us=%.1f gpu_draw_us=%.1f "
             "(cpu_draw_us=%.1f incl. record+submit+fence-wait)",
             (unsigned long long)g_island->gpuTimer.samples,
             g_island->gpuTimer.avgTotalUs(),
             g_island->gpuTimer.avgGapUs(1),
             g_island->gpuTimer.avgGapUs(2),
             g_health.drawUs);
    }
    if (!g_island) return;
    g_island->gpuTimer.destroy();
    destroy_island(g_island);
    delete g_island; g_island = nullptr;
}

#endif // MC2_VULKAN_ISLAND
