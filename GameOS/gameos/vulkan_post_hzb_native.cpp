// Same-frame Vulkan depth pyramid and render-only static-prop occlusion.
#include "gos_postprocess.h"
#include "vulkan_shadow_island_live.h"
#include "vulkan_color_prop_island.h"
#include "../../RenderCore/hzb_prop_bounds.h"
#include <bit>
#include <array>
#include <algorithm>
#include <cmath>
#include <cfloat>

using mc2hzb::PropBounds;
using mc2hzb::ProjectedProp;
#include "Gates/gate.h"
#include "Gates/gate_cold.h"   // str<>()


#if defined(_WIN32) && !defined(VK_USE_PLATFORM_WIN32_KHR)
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <volk.h>

#include <vk_mem_alloc.h>

#include "vk_image_resource.h"
#include "vk_island_util.h"
#include "vulkan_context.h"
#include "vk_gpu_profile.h"   // VK-GPU-ZONES: MC2_VK_GPU_SCOPE
#define MC2_VULKAN_SHARED_SCENE_COLOR_IMPL_ISLAND
#include "vulkan_shared_scene_color.h"
#define MC2_VULKAN_SHARED_SCENE_DEPTH_IMPL_ISLAND
#include "vulkan_shared_scene_depth.h"
#include "vk_shared_gbuffer_gate.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>
#include <string_view>
#include <vector>
#include <new>
#include "../../mclib/battle_bench_wait.h"
#include "diagnostic_trace.h"
#include <tracy/Tracy.hpp>

namespace {

constexpr uint32_t kCullMaxLevels = 16;   // hzb_cull.comp levelDepth[] size

void vlog(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vk_island_util::vlogv512("[VK_POST_HZB_NATIVE] ", fmt, ap);
    va_end(ap);
}

struct HzbNativeHealth {
    int runtimeGate = 0;
    int substrateLive = 0;
    int directChain = 1;
    unsigned long attempted = 0;
    unsigned long usedVulkan = 0;
    unsigned long usedInterop = 0;
    unsigned long levels = 0;
    unsigned long vkDraws = 0;
    unsigned long copyDepthOps = 0;
    unsigned long readbackOps = 0;
    char fallbackReason[64] = {0};
    double renderUs = 0.0;
    // VK-QWI-TO-FENCE-1: water-style us_fence_wait telemetry around the per-frame
    // CPU-blocking queue wait (QWI before the conversion, per-submit fence after).
    double waitUsLast = 0.0;
    double waitUsAccum = 0.0;
    unsigned long waitSamples = 0;
};
HzbNativeHealth g_health;

void setFallback(const char* reason) {
    std::strncpy(g_health.fallbackReason, reason ? reason : "",
                 sizeof(g_health.fallbackReason) - 1);
    g_health.fallbackReason[sizeof(g_health.fallbackReason) - 1] = '\0';
}

std::string spv_dir() { return vk_island_util::spvDir(); }

VkShaderModule load_spv(VkDevice device, std::string_view path) {
    return vk_island_util::loadSpv(device, path, vlog);
}

// std140 POD, matching shaders/vulkan/hzb_reduce.frag.
struct HzbLevelParams {
    float srcTexel[2];
    int   reduce;
    int   _pad0;
};
static_assert(sizeof(HzbLevelParams) == 16, "HzbLevelParams std140 offsets drifted");

const VkFormat kLevelFmt = VK_FORMAT_R32_SFLOAT;

} // namespace

struct gosPostProcess::VkPostHzbNative {
    bool disabled = false;
    bool inited = false;
    int  width = 0, height = 0, mipCount = 0;

    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice         device = VK_NULL_HANDLE;
    uint32_t         gfxFamily = UINT32_MAX;
    VkQueue          queue = VK_NULL_HANDLE;
    VmaAllocator     allocator = VK_NULL_HANDLE;

    VkImageResource* colorRes = nullptr;
    VkImageResource* normalRes = nullptr;
    VkImageResource* depthRes = nullptr;
    // VK-RUNG8-ITEM5: shared scene is VK-only (no GL view); direct-chain only.

    VkCommandBuffer cbuf[mc2vk::kMaxFramesInFlight] = {VK_NULL_HANDLE, VK_NULL_HANDLE};   // allocated from `cpool`, freed with it

    // One reduction level: its image, view, framebuffer and UBO are owned (declared so the UBO goes first, the image last).
    struct Level {
        int w = 0, h = 0;
        VkDescriptorSet set = VK_NULL_HANDLE;   // allocated from `pool`, freed with it
        mc2vk::OwnedImage image;
        mc2vk::OwnedImageView view;
        mc2vk::OwnedFramebuffer fb;
        mc2vk::OwnedBuffer ubo;
    };

    // OWNED objects, declared in creation order: the destructor releases them in reverse (destroy_state idles the GPU first).
    mc2vk::OwnedImageView depthSampleView;
    mc2vk::OwnedSampler   depthSampler;
    mc2vk::OwnedRenderPass rpass;
    std::vector<Level> levels;   // framebuffers made against `rpass`, so released before it
    mc2vk::OwnedDescriptorSetLayout dsl;
    mc2vk::OwnedDescriptorPool pool;
    mc2vk::OwnedShaderModule vert;
    mc2vk::OwnedShaderModule frag;
    mc2vk::OwnedPipelineLayout playout;
    mc2vk::OwnedPipeline pipe;
    mc2vk::OwnedCommandPool cpool;
    std::vector<PropBounds> propBounds;
    uint32_t boundsGeneration = UINT32_MAX;
    uint32_t cullCapacity = 0, previousCount = 0;
    uint64_t visibilityBytes = 0;   // size `visibility` was made with (cullCapacity is bumped, or zeroed by a bounds rebuild, before the old buffer retires)
    uint64_t visibilityGeneration = 0;
    uint64_t measuredFrames = 0, totalCulled = 0, totalTested = 0;
    // `instances` is written by the host every run, so it is per frame slot (with the set that binds it): the slot's own ticket covers it.
    mc2vk::OwnedBuffer visibility, localBounds;
    mc2vk::OwnedBuffer instances[mc2vk::kMaxFramesInFlight];
    mc2vk::OwnedDescriptorSetLayout cullDsl;
    mc2vk::OwnedDescriptorPool cullPool;
    mc2vk::OwnedShaderModule cullShader;
    mc2vk::OwnedPipelineLayout cullLayout;
    mc2vk::OwnedPipeline cullPipe;
    VkDescriptorSet cullSet[mc2vk::kMaxFramesInFlight] = {};   // per slot: sampler[16] + visibility + the slot's instances + type bounds
    // [CULL_PARITY v1] counter-only check: a sampled subset of one frame's instances, with the matrix/bias/bounds that produced the GPU mask, compared
    // with the CPU reference once that frame's submit has completed (polled, never waited). Check frames copy the mips and the mask into `parityMips`
    // (the mask at `visOffset`), so later frames may rewrite the live mask before the check runs.
    struct ParityPending {
        bool valid = false;
        uint64_t generation = 0;
        int w = 0, h = 0, mips = 0;
        uint64_t ticket = 0;            // the check frame's submit
        VkDeviceSize visOffset = 0;     // byte offset of the mask copy in parityMips
        float worldToClip[16] = {};
        float bias = 0;
        std::vector<uint32_t> ids;
        std::vector<GpuStaticPropInstance> inst;
        std::vector<PropBounds> bounds;
    } parity;
    mc2vk::OwnedBuffer parityMips;
    VkDeviceSize parityMipBytes = 0;
    // Timeline value of each frame slot's last submit (0 = never): a run waits only for its own slot's previous use (frame N-3), not frame N-1.
    uint64_t ticket[mc2vk::kMaxFramesInFlight] = {};
    uint64_t lastTicket = 0;   // the newest submit: the [CULL] counter is read only once it has completed (polled)
};

namespace {

bool make_buffer(gosPostProcess::VkPostHzbNative* s, VkDeviceSize size,
                 VkBufferUsageFlags usage, mc2vk::OwnedBuffer& out,
                 std::string_view name, int i0 = -1, int i1 = -1) {
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size;
    bci.usage = usage;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    if (!out.create(s->allocator, s->device, bci, aci, name, i0, i1)) {
        vlog("vmaCreateBuffer failed.");
        return false;
    }
    return true;
}

// Releases the owned objects (the state's destructor does it, once the GPU is idle) and resets the state; the borrowed handles are not touched.
void destroy_state(gosPostProcess::VkPostHzbNative* s) {
    if (!s) return;
    if (s->device != VK_NULL_HANDLE) mc2vk::getOrCreateEngineVulkanContext().waitIdle("hzb_destroy");
    const bool disabled = s->disabled;
    *s = gosPostProcess::VkPostHzbNative{};
    s->disabled = disabled;
}

// [CULL_PARITY v1] process-wide totals (survive a resize reset of the state).
struct ParityStats { uint64_t runs = 0, checks = 0, tested = 0, falseCulls = 0, falseKeeps = 0, loggedFalseCulls = 0; };
ParityStats g_parity;
constexpr uint64_t kParityEvery = 64;       // check one frame in 64
constexpr uint32_t kParitySamples = 4096;   // instances compared per check (strided); keeps the CPU cost far below 0.02 ms/frame

void parity_log(std::string_view tag) {
    if (!mc2_diag::tagRequested("CULL_PARITY")) return;   // quiet by default (MC2_DIAG_TAGS=CULL_PARITY prints)
    vlog("[CULL_PARITY v1] engaged=%d checks=%llu tested=%llu false_culls=%llu false_keeps=%llu runs=%llu%s",
         g_parity.checks ? 1 : 0, static_cast<unsigned long long>(g_parity.checks), static_cast<unsigned long long>(g_parity.tested),
         static_cast<unsigned long long>(g_parity.falseCulls), static_cast<unsigned long long>(g_parity.falseKeeps),
         static_cast<unsigned long long>(g_parity.runs), tag.data());
}

// Compares the GPU keep mask of the pending check frame with the CPU reference on the same bounds and the same pyramid copy.
void parity_check(gosPostProcess::VkPostHzbNative* s) {
    auto& pc = s->parity;
    if (!pc.valid || !mc2vk::getOrCreateEngineVulkanContext().isComplete(mc2vk::GpuTicket{pc.ticket, 0})) return;   // not done yet: a later run checks
    pc.valid = false;
    if (pc.generation != s->visibilityGeneration || pc.w != s->width || pc.h != s->height || pc.mips != s->mipCount || !s->parityMips) return;
    ZoneScopedN("HZB.ParityCheck");
    vmaInvalidateAllocation(s->allocator, s->parityMips.allocation(), 0, VK_WHOLE_SIZE);
    void* const mapped = s->parityMips.map();
    const float* mips = static_cast<const float*>(mapped);
    const uint32_t* vis = mapped ? static_cast<const uint32_t*>(static_cast<const void*>(static_cast<const uint8_t*>(mapped) + pc.visOffset)) : nullptr;
    if (vis && mips) {
        std::vector<size_t> offset(static_cast<size_t>(s->mipCount));
        size_t at = 0;
        for (int l = 0; l < s->mipCount; ++l) { offset[static_cast<size_t>(l)] = at; at += static_cast<size_t>(s->levels[static_cast<size_t>(l)].w) * static_cast<size_t>(s->levels[static_cast<size_t>(l)].h); }
        const auto fetch = [&](int l, int x, int y) { return mips[offset[static_cast<size_t>(l)] + static_cast<size_t>(y) * static_cast<size_t>(s->levels[static_cast<size_t>(l)].w) + static_cast<size_t>(x)]; };
        ++g_parity.checks;
        for (size_t i = 0; i < pc.ids.size(); ++i) {
            const auto p = mc2hzb::project_prop(pc.inst[i], pc.worldToClip, pc.bias, pc.bounds, pc.w, pc.h, pc.mips);
            const bool cpuHidden = mc2hzb::hzb_hidden(p, pc.w, pc.h, fetch);
            const bool gpuHidden = vis[1u + pc.ids[i]] == 0u;
            ++g_parity.tested;
            if (gpuHidden && !cpuHidden) {
                ++g_parity.falseCulls;
                if (g_parity.loggedFalseCulls++ < 8)
                    vlog("[CULL_PARITY v1] FALSE_CULL runs=%llu id=%u type=%u keep=%u lod=%u nearest=%.6f pixels=%.1f,%.1f,%.1f,%.1f",
                         static_cast<unsigned long long>(g_parity.runs), pc.ids[i], pc.inst[i].typeID, p.keep, p.lod, p.nearest,
                         p.pixels[0], p.pixels[1], p.pixels[2], p.pixels[3]);
            } else if (!gpuHidden && cpuHidden) {
                ++g_parity.falseKeeps;
            }
        }
    }
    if (mips) s->parityMips.unmap();
}

// Geometry is cached per map; all eight corners of the actual local mesh AABB
// enclose every packet, including off-centre building meshes and rotated props.
bool cache_prop_bounds(gosPostProcess::VkPostHzbNative* s, uint32_t generation) {
    if (s->boundsGeneration == generation) return true;
    Mc2ShadowGeomFeed geom{};
    const void *vertices = nullptr, *indices = nullptr;
    uint64_t vb = 0, ib = 0;
    if (!mc2_shadow_island_get_geometry_feed(&geom) ||
        !mc2_static_prop_geometry_cpu(&vertices, &vb, &indices, &ib) ||
        !vertices || !indices || geom.vertexStride < 12 || vb != geom.vboBytes || ib != geom.iboBytes) return false;
    s->propBounds.assign(geom.typeCount, {});
    const auto* ix = static_cast<const uint32_t*>(indices);
    const auto* vx = static_cast<const uint8_t*>(vertices);
    for (uint32_t t = 0; t < geom.typeCount; ++t) {
        PropBounds b{{FLT_MAX, FLT_MAX, FLT_MAX}, {-FLT_MAX, -FLT_MAX, -FLT_MAX}, false};
        bool invalid = false;
        for (uint32_t p = 0; p < geom.typePacketCount[t]; ++p) {
            const uint64_t pk = static_cast<uint64_t>(geom.typeFirstPacket[t]) + p;
            if (pk >= geom.packetCount) { invalid = true; break; }
            const auto& packet = geom.packets[pk];
            if (static_cast<uint64_t>(packet.firstIndex) + packet.indexCount > ib / sizeof(uint32_t)) { invalid = true; break; }
            for (uint32_t k = 0; k < packet.indexCount; ++k) {
                const uint64_t vi = static_cast<uint64_t>(packet.baseVertex) + ix[packet.firstIndex + k];
                if (vi >= vb / geom.vertexStride) { invalid = true; break; }
                float pos[3];
                std::memcpy(pos, vx + vi * geom.vertexStride, sizeof(pos));
                for (int a = 0; a < 3; ++a) {
                    if (!std::isfinite(pos[a])) invalid = true;
                    b.lo[a] = (std::min)(b.lo[a], pos[a]); b.hi[a] = (std::max)(b.hi[a], pos[a]);
                }
                b.valid = true;
            }
        }
        b.valid = b.valid && !invalid;
        s->propBounds[t] = b;
    }
    std::vector<std::array<float, 8>> gpuBounds;
    gpuBounds.reserve(s->propBounds.size());
    for (const auto& b : s->propBounds)
        gpuBounds.push_back({b.lo[0], b.lo[1], b.lo[2], b.valid ? 1.0f : 0.0f,
                             b.hi[0], b.hi[1], b.hi[2], 0.0f});
    if (s->localBounds) mc2vk::getOrCreateEngineVulkanContext().retire(std::move(s->localBounds));   // older slots' culls may still read it
    const size_t bytes = gpuBounds.size() * sizeof(gpuBounds[0]);
    if (!make_buffer(s, (std::max)(bytes, static_cast<size_t>(32)), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     s->localBounds, "hzb.local_bounds") ||
        (bytes && !s->localBounds.upload(gpuBounds.data(), bytes))) return false;
    s->cullCapacity = 0; // descriptors must follow the new map's bounds buffer
    s->boundsGeneration = generation;
    return true;
}


bool build_prop_cull(gosPostProcess::VkPostHzbNative& state, uint32_t count) {
    auto* s = &state;
    if (s->cullCapacity >= count) return true;
    // The color pass can still be reading the old mask after the HZB ticket: park the old storage and pool until the frame that may use them has
    // completed (deferred destroy) instead of idling the device.
    auto& ectx = mc2vk::getOrCreateEngineVulkanContext();
    s->previousCount = 0; // the retired mask no longer supplies telemetry
    s->cullCapacity = (std::max)(1024u, std::bit_ceil(count));
    if (s->cullPool) ectx.retire(std::move(s->cullPool));
    if (s->visibility) ectx.retire(std::move(s->visibility), s->visibilityBytes);
    for (auto& b : s->instances) if (b) ectx.retire(std::move(b));
    for (auto& set : s->cullSet) set = VK_NULL_HANDLE;
    // The shader samples a fixed 16-slot level array; a deeper pyramid cannot be addressed.
    if (s->mipCount < 1 || s->mipCount > static_cast<int>(kCullMaxLevels)) return false;
    for (int i = 0; i < static_cast<int>(mc2vk::kMaxFramesInFlight); ++i)
        if (!make_buffer(s, static_cast<uint64_t>(s->cullCapacity)*sizeof(GpuStaticPropInstance), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, s->instances[i], "hzb.instances", i)) return false;
    if (!make_buffer(s, static_cast<uint64_t>(s->cullCapacity+1)*sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, s->visibility, "hzb.visibility")) return false;
    s->visibilityBytes = static_cast<uint64_t>(s->cullCapacity+1)*sizeof(uint32_t);
    // A recycled Vulkan handle must not leave consumers bound to the old mask.
    static uint64_t nextVisibilityGeneration = 0;
    s->visibilityGeneration = ++nextVisibilityGeneration;
    if (!s->cullDsl) {
        VkDescriptorSetLayoutBinding bindings[4] = {};
        for (uint32_t i = 0; i < 4; ++i) {
            bindings[i].binding = i; bindings[i].descriptorCount = i == 0 ? kCullMaxLevels : 1; bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            bindings[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        }
        VkDescriptorSetLayoutCreateInfo di{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        di.bindingCount = 4; di.pBindings = bindings;
        if (!mc2vk::create(s->cullDsl, s->device, [&](VkDescriptorSetLayout* p){return vkCreateDescriptorSetLayout(s->device,&di,nullptr,p);})) return false;
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 96};
        const VkDescriptorSetLayout layout = s->cullDsl.get();
        VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        li.setLayoutCount = 1; li.pSetLayouts = &layout; li.pushConstantRangeCount = 1; li.pPushConstantRanges = &push;
        if (!mc2vk::create(s->cullLayout,s->device,[&](VkPipelineLayout* p){return vkCreatePipelineLayout(s->device,&li,nullptr,p);})) return false;
        s->cullShader = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir()+"hzb_cull.comp.spv"));
        if (!s->cullShader) return false;
        VkComputePipelineCreateInfo pi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; pi.stage.module = s->cullShader.get(); pi.stage.pName = "main"; pi.layout = s->cullLayout.get();
        if (!mc2vk::create(s->cullPipe,s->device,[&](VkPipeline* p){return mc2vk::infra::compute(s->device,VK_NULL_HANDLE,1,&pi,nullptr,p);})) return false;
    }
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kCullMaxLevels * mc2vk::kMaxFramesInFlight}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * mc2vk::kMaxFramesInFlight}};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = mc2vk::kMaxFramesInFlight; pci.poolSizeCount = 2; pci.pPoolSizes = sizes;
    if (!mc2vk::create(s->cullPool,s->device,[&](VkDescriptorPool* p){return vkCreateDescriptorPool(s->device,&pci,nullptr,p);})) return false;
    VkDescriptorSetLayout layouts[mc2vk::kMaxFramesInFlight];
    for (auto& l : layouts) l = s->cullDsl.get();
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = s->cullPool.get(); ai.descriptorSetCount = mc2vk::kMaxFramesInFlight; ai.pSetLayouts = layouts;
    if (MC2_VK_ALLOC_SETS(s->device,&ai,s->cullSet) != VK_SUCCESS) return false;
    // Slots past the real mip count repeat the last level (the shader never selects them).
    VkDescriptorImageInfo images[kCullMaxLevels];
    for (uint32_t i = 0; i < kCullMaxLevels; ++i)
        images[i] = {s->depthSampler.get(), s->levels[(std::min)(i, static_cast<uint32_t>(s->mipCount) - 1)].view.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    for (uint32_t slot = 0; slot < mc2vk::kMaxFramesInFlight; ++slot) {
        VkDescriptorBufferInfo buffers[] = {{s->visibility.get(),0,VK_WHOLE_SIZE},{s->instances[slot].get(),0,VK_WHOLE_SIZE},{s->localBounds.get(),0,VK_WHOLE_SIZE}};
        VkWriteDescriptorSet writes[4] = {};
        for (uint32_t j = 0; j < 4; ++j) {
            writes[j].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[j].dstSet = s->cullSet[slot]; writes[j].dstBinding = j;
            writes[j].descriptorCount = j == 0 ? kCullMaxLevels : 1;
            writes[j].descriptorType = j==0 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            if (j==0) writes[j].pImageInfo = images; else writes[j].pBufferInfo = &buffers[j-1];
        }
        vkUpdateDescriptorSets(s->device,4,writes,0,nullptr);
    }
    return true;
}

bool build_state(gosPostProcess::VkPostHzbNative* s, int w, int h, int mipCount) {
    s->width = w;
    s->height = h;
    s->mipCount = mipCount;

    if (!mc2vk::ensureEngineVulkanContext()) {
        setFallback("engine_vk_context_birth_failed");
        return false;
    }
    mc2vk::VulkanContext& ctx = mc2vk::getOrCreateEngineVulkanContext();
    if (!ctx.valid()) {
        setFallback("engine_vk_context_not_valid");
        return false;
    }
    s->phys = ctx.physicalDevice();
    {   // hzb_cull.comp uses subgroupElect + subgroupAdd in the compute stage
        VkPhysicalDeviceSubgroupProperties sg{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &sg};
        vkGetPhysicalDeviceProperties2(s->phys, &p2);
        const VkSubgroupFeatureFlags need = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_ARITHMETIC_BIT;
        if (!(sg.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) || (sg.supportedOperations & need) != need) {
            setFallback("subgroup_ops_unsupported");
            return false;
        }
    }
    s->device = ctx.device();
    s->queue = ctx.graphicsQueue();
    s->gfxFamily = ctx.graphicsFamily();
    s->allocator = ctx.allocator();
    if (s->device == VK_NULL_HANDLE || s->allocator == VK_NULL_HANDLE) {
        setFallback("engine_vk_device_or_allocator_null");
        return false;
    }

    s->colorRes = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_COLOR0);
    s->normalRes = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_NORMAL);
    s->depthRes = mc2SharedSceneDepthGetResource();
    // VK-RUNG8 item-4/item-5: direct-chain-native only. Only the VK-only shared
    // VkImageResources are required; there is no GL interop view.
    if (!s->colorRes || !s->normalRes || !s->depthRes) {
        setFallback("shared_scene_depth_not_ready");
        return false;
    }
    if (static_cast<int>(s->colorRes->w) != w || static_cast<int>(s->colorRes->h) != h ||
        static_cast<int>(s->normalRes->w) != w || static_cast<int>(s->normalRes->h) != h ||
        static_cast<int>(s->depthRes->w) != w || static_cast<int>(s->depthRes->h) != h) {
        setFallback("shared_scene_depth_size_mismatch");
        return false;
    }
    g_health.substrateLive = 1;

    VkResult r;

    {
        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = s->depthRes->image.get();
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = s->depthRes->format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        if (!mc2vk::create(s->depthSampleView, s->device, [&](VkImageView* h) { r = vkCreateImageView(s->device, &vci, nullptr, h); return r; })) {
            vlog("depth sample view create failed (%d).", static_cast<int>(r));
            return false;
        }
    }
    {
        VkSamplerCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter = VK_FILTER_NEAREST;
        sci.minFilter = VK_FILTER_NEAREST;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (!mc2vk::create(s->depthSampler, s->device, [&](VkSampler* h) { r = vkCreateSampler(s->device, &sci, nullptr, h); return r; })) {
            vlog("vkCreateSampler failed (%d).", static_cast<int>(r));
            return false;
        }
    }

    // Render pass: one R32F color attachment, STORE, finalLayout SHADER_READ_ONLY.
    {
        VkAttachmentDescription att{};
        att.format = kLevelFmt;
        att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &ref;
        VkSubpassDependency dep[2]{};
        // Incoming: the previous frame's reads of these levels (cull compute, the next level's fragment read, the parity copy)
        // must finish before this frame rewrites them; no other pass is allowed to fence this by accident.
        dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        dep[0].dstSubpass = 0;
        dep[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
        dep[0].srcAccessMask = 0;   // read-after-write is already made visible; this is a write-after-read execution dependency
        dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dep[1].srcSubpass = 0;
        dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dep[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        dep[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        VkRenderPassCreateInfo rpci{};
        rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpci.attachmentCount = 1;
        rpci.pAttachments = &att;
        rpci.subpassCount = 1;
        rpci.pSubpasses = &sub;
        rpci.dependencyCount = 2;
        rpci.pDependencies = dep;
        if (!mc2vk::create(s->rpass, s->device, [&](VkRenderPass* h) { r = vkCreateRenderPass(s->device, &rpci, nullptr, h); return r; })) {
            vlog("vkCreateRenderPass failed (%d).", static_cast<int>(r));
            return false;
        }
    }

    s->levels.resize(static_cast<size_t>(mipCount));
    {
        int lw = w;
        int lh = h;
        for (int level = 0; level < mipCount; ++level) {
            auto& lv = s->levels[static_cast<size_t>(level)];
            lv.w = lw;
            lv.h = lh;

            VkImageCreateInfo ici{};
            ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            ici.imageType = VK_IMAGE_TYPE_2D;
            ici.format = kLevelFmt;
            ici.extent = {static_cast<uint32_t>(lw), static_cast<uint32_t>(lh), 1};
            ici.mipLevels = 1;
            ici.arrayLayers = 1;
            ici.samples = VK_SAMPLE_COUNT_1_BIT;
            ici.tiling = VK_IMAGE_TILING_OPTIMAL;
            ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
            if (!lv.image.create(s->allocator, s->device, ici, aci, "posthzb.level", level)) {
                vlog("vmaCreateImage(level %d) failed.", level);
                return false;
            }

            VkImageViewCreateInfo vci{};
            vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            vci.image = lv.image.get();
            vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vci.format = kLevelFmt;
            vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            if (!mc2vk::create(lv.view, s->device, [&](VkImageView* h) { r = vkCreateImageView(s->device, &vci, nullptr, h); return r; })) {
                vlog("vkCreateImageView(level %d) failed (%d).", level, static_cast<int>(r));
                return false;
            }

            VkFramebufferCreateInfo fci{};
            fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fci.renderPass = s->rpass.get();
            fci.attachmentCount = 1;
            const VkImageView levelView = lv.view.get();
            fci.pAttachments = &levelView;
            fci.width = static_cast<uint32_t>(lw);
            fci.height = static_cast<uint32_t>(lh);
            fci.layers = 1;
            if (!mc2vk::create(lv.fb, s->device, [&](VkFramebuffer* h) { r = vkCreateFramebuffer(s->device, &fci, nullptr, h); return r; })) {
                vlog("vkCreateFramebuffer(level %d) failed (%d).", level, static_cast<int>(r));
                return false;
            }

            if (!make_buffer(s, sizeof(HzbLevelParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                             lv.ubo, "posthzb.levelUbo", level)) {
                return false;
            }

            HzbLevelParams params{};
            if (level == 0) {
                params.srcTexel[0] = 1.0f / static_cast<float>(w);
                params.srcTexel[1] = 1.0f / static_cast<float>(h);
                params.reduce = 0;
            } else {
                const auto& prev = s->levels[static_cast<size_t>(level) - 1];
                params.srcTexel[0] = 1.0f / static_cast<float>(prev.w);
                params.srcTexel[1] = 1.0f / static_cast<float>(prev.h);
                params.reduce = 1;
            }
            if (!lv.ubo.upload(&params, sizeof(params))) {
                vlog("vmaMapMemory(ubo level %d) failed.", level);
                return false;
            }

            lw = (lw + 1) / 2;
            lh = (lh + 1) / 2;
            lw = (std::max)(lw, 1);
            lh = (std::max)(lh, 1);
        }
    }

    {
        VkDescriptorSetLayoutBinding binds[2]{};
        binds[0].binding = 0;
        binds[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binds[0].descriptorCount = 1;
        binds[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        binds[1].binding = 1;
        binds[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        binds[1].descriptorCount = 1;
        binds[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.bindingCount = 2;
        lci.pBindings = binds;
        if (!mc2vk::create(s->dsl, s->device, [&](VkDescriptorSetLayout* h) { r = vkCreateDescriptorSetLayout(s->device, &lci, nullptr, h); return r; })) {
            vlog("vkCreateDescriptorSetLayout failed (%d).", static_cast<int>(r));
            return false;
        }
        VkDescriptorPoolSize sizes[2]{};
        sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sizes[0].descriptorCount = static_cast<uint32_t>(mipCount);
        sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        sizes[1].descriptorCount = static_cast<uint32_t>(mipCount);
        VkDescriptorPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.maxSets = static_cast<uint32_t>(mipCount);
        pci.poolSizeCount = 2;
        pci.pPoolSizes = sizes;
        if (!mc2vk::create(s->pool, s->device, [&](VkDescriptorPool* h) { r = vkCreateDescriptorPool(s->device, &pci, nullptr, h); return r; })) {
            vlog("vkCreateDescriptorPool failed (%d).", static_cast<int>(r));
            return false;
        }

        const VkDescriptorSetLayout dslHandle = s->dsl.get();
        for (int level = 0; level < mipCount; ++level) {
            auto& lv = s->levels[static_cast<size_t>(level)];
            VkDescriptorSetAllocateInfo dsai{};
            dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            dsai.descriptorPool = s->pool.get();
            dsai.descriptorSetCount = 1;
            dsai.pSetLayouts = &dslHandle;
            r = MC2_VK_ALLOC_SETS(s->device, &dsai, &lv.set);
            if (r != VK_SUCCESS) {
                vlog("MC2_VK_ALLOC_SETS(level %d) failed (%d).", level, static_cast<int>(r));
                return false;
            }

            VkDescriptorImageInfo srcInfo{};
            srcInfo.sampler = s->depthSampler.get();
            srcInfo.imageView = (level == 0) ? s->depthSampleView.get()
                                             : s->levels[static_cast<size_t>(level) - 1].view.get();
            srcInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkDescriptorBufferInfo uboInfo{lv.ubo.get(), 0, VK_WHOLE_SIZE};
            VkWriteDescriptorSet writes[2]{};
            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = lv.set;
            writes[0].dstBinding = 0;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[0].pImageInfo = &srcInfo;
            writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1].dstSet = lv.set;
            writes[1].dstBinding = 1;
            writes[1].descriptorCount = 1;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[1].pBufferInfo = &uboInfo;
            vkUpdateDescriptorSets(s->device, 2, writes, 0, nullptr);
        }
    }

    s->vert = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir() + "hzb_reduce.vert.spv"));
    s->frag = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir() + "hzb_reduce.frag.spv"));
    if (!s->vert || !s->frag) {
        setFallback("spv_load_failed");
        return false;
    }

    {
        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        const VkDescriptorSetLayout dslHandle = s->dsl.get();
        plci.setLayoutCount = 1;
        plci.pSetLayouts = &dslHandle;
        if (!mc2vk::create(s->playout, s->device, [&](VkPipelineLayout* h) { r = vkCreatePipelineLayout(s->device, &plci, nullptr, h); return r; })) {
            vlog("vkCreatePipelineLayout failed (%d).", static_cast<int>(r));
            return false;
        }

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = s->vert.get();
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = s->frag.get();
        stages[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo vin{};
        vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vps{};
        vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vps.viewportCount = 1;
        vps.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState cba{};
        cba.blendEnable = VK_FALSE;
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 1;
        cb.pAttachments = &cba;
        VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dsci{};
        dsci.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dsci.dynamicStateCount = 2;
        dsci.pDynamicStates = dyn;
        VkGraphicsPipelineCreateInfo gp{};
        gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gp.stageCount = 2;
        gp.pStages = stages;
        gp.pVertexInputState = &vin;
        gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vps;
        gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms;
        gp.pColorBlendState = &cb;
        gp.pDynamicState = &dsci;
        gp.layout = s->playout.get();
        gp.renderPass = s->rpass.get();
        gp.subpass = 0;
        const bool pipeOk = mc2vk::create(s->pipe, s->device, [&](VkPipeline* h) { r = mc2vk::infra::graphics(s->device, VK_NULL_HANDLE, 1, &gp, nullptr, h); return r; });
        mc2vk::nameObject(s->device, s->pipe.get(), "posthzb.pipe");
        if (!pipeOk) {
            vlog("vkCreateGraphicsPipelines failed (%d).", static_cast<int>(r));
            return false;
        }
    }

    {
        VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = s->gfxFamily;
        if (!mc2vk::create(s->cpool, s->device, [&](VkCommandPool* h) { r = vkCreateCommandPool(s->device, &pci, nullptr, h); return r; })) {
            vlog("vkCreateCommandPool failed (%d).", static_cast<int>(r));
            return false;
        }
        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = s->cpool.get();
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = mc2vk::kMaxFramesInFlight;
        r = vkAllocateCommandBuffers(s->device, &cai, s->cbuf);
        if (r != VK_SUCCESS) {
            vlog("vkAllocateCommandBuffers failed (%d).", static_cast<int>(r));
            return false;
        }
    }

    s->inited = true;
    setFallback("");
    g_health.levels = static_cast<unsigned long>(mipCount);
    vlog("initialized %dx%d with %d ceil-ladder levels on the shared engine context "
         "(zero depth ingress/readback egress on the normal path).", w, h, mipCount);
    return true;
}

} // namespace

// The owner's deleter: idles the GPU and releases the owned objects before the state itself goes.
void gosPostProcess::VkPostHzbNativeDeleter::operator()(VkPostHzbNative* s) const {
    destroy_state(s);
    delete s;
}

void gosPostProcess::prewarmVkHzb() {
    if(!hzbEnabled_ || width_<=0 || height_<=0) return;
    if(!vkPostHzbNative_) vkPostHzbNative_.reset(std::make_unique<VkPostHzbNative>().release());
    auto* s=vkPostHzbNative_.get();
    int levels=1; for(int edge=(std::max)(width_,height_);edge>1;edge=(edge+1)/2) ++levels;
    if(!s->inited && !build_state(s,width_,height_,levels)) { destroy_state(s); return; }
    // Prewarm precedes the map bounds upload; descriptors still need a valid buffer.
    if (!s->localBounds && !make_buffer(s, 32, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                      s->localBounds, "hzb.local_bounds")) { destroy_state(s); return; }
    if(!build_prop_cull(*s,1024)) destroy_state(s);
}

bool gosPostProcess::runVkPostHzbNative(const Mc2ColorPropFeed& feed, uint32_t generation, void** maskBuffer, uint64_t* maskGeneration)
{
    g_health.runtimeGate = hzbEnabled_ ? 1 : 0;
    if (maskBuffer) *maskBuffer = nullptr;
    if (maskGeneration) *maskGeneration = 0;
    if (!hzbEnabled_ || !maskBuffer || !maskGeneration || !feed.instances112 || !feed.count || feed.count > 131072) return false;
    // Small feeds cannot amortize the pyramid and dispatches. A null mask means
    // the color pass keeps every instance, including after a larger prior frame.
    if (feed.count < 1024) {
        setFallback("low_candidate_count");
        return false;
    }
    int levels = 1;
    for (int size = (std::max)(width_, height_); size > 1; size = (size+1)/2) ++levels;
    hzbW_ = width_; hzbH_ = height_; hzbMipCount_ = levels;
    if (!vkPostHzbNative_) vkPostHzbNative_.reset(new (std::nothrow) VkPostHzbNative());
    VkPostHzbNative* s = vkPostHzbNative_.get();
    if (!s) {
        setFallback("alloc_failed");
        return false;
    }
    if (s->disabled) return false;

    // This frame slot's previous submit (frame N-3) must have completed before its command buffer, instance buffer and set are reused. Waiting for the
    // slot's own ticket, not the newest one, keeps this mid-frame point from capping the CPU lead at one frame.
    const uint32_t slot = mc2vk::getOrCreateEngineVulkanContext().frameContext().slot % mc2vk::kMaxFramesInFlight;
    {
        const std::chrono::steady_clock::time_point tWait0 = std::chrono::steady_clock::now();
        mc2::battlebench::WaitScope gpuWait(mc2::battlebench::WaitGpuMainNs);
        ZoneScopedN("HZB.WaitPreviousSubmit");
        if (!mc2vk::getOrCreateEngineVulkanContext().wait(mc2vk::GpuTicket{s->ticket[slot], 0}, 5ull * 1000 * 1000 * 1000, "post_prev_submit")) {
            setFallback("fence_timeout"); s->disabled = true; return false;
        }
        g_health.waitUsLast = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - tWait0).count();
        g_health.waitUsAccum += g_health.waitUsLast;
        ++g_health.waitSamples;
        if ((g_health.waitSamples % 600) == 1)
            vlog("[QWI-WAIT] wait_us_last=%.1f wait_us_avg=%.1f samples=%lu",
                 g_health.waitUsLast,
                 g_health.waitUsAccum / static_cast<double>(g_health.waitSamples), g_health.waitSamples);
    }
    if (!s->inited || s->width != width_ || s->height != height_ || s->mipCount != hzbMipCount_) {
        if (s->inited) destroy_state(s);
        if (!build_state(s, width_, height_, hzbMipCount_)) {
            vlog("init FAILED (reason=%s) -- disabling for this process, culling keeps every prop.",
                 g_health.fallbackReason[0] ? g_health.fallbackReason : "unknown");
            destroy_state(s);
            s->disabled = true;
            return false;
        }
    }

    if (!cache_prop_bounds(s, generation) || !build_prop_cull(*s, feed.count)) {
        setFallback("cull_resources_failed"); s->disabled = true; return false;
    }
    // The [CULL] counter is the newest run's: read it only once that submit has completed (polled), else skip this sample.
    if (s->previousCount && mc2vk::getOrCreateEngineVulkanContext().isComplete(mc2vk::GpuTicket{s->lastTicket, 0})) {
        vmaInvalidateAllocation(s->allocator, s->visibility.allocation(), 0, sizeof(uint32_t));
        if (const void* p = s->visibility.map()) {
            const uint32_t culled = *static_cast<const uint32_t*>(p);
            s->visibility.unmap();
            ++s->measuredFrames; s->totalCulled += culled; s->totalTested += s->previousCount;
            if (s->measuredFrames <= 3 || s->measuredFrames % 120 == 0)
                vlog("[CULL] tested=%u culled=%u kept=%u total_tested=%llu total_culled=%llu frames=%llu same_frame=1 render_only=1",
                    s->previousCount, culled, s->previousCount-culled,
                    static_cast<unsigned long long>(s->totalTested), static_cast<unsigned long long>(s->totalCulled), static_cast<unsigned long long>(s->measuredFrames));
        }
    }
    parity_check(s);   // runs once the pending check frame has completed
    if (!s->instances[slot].upload(feed.instances112, static_cast<uint64_t>(feed.count)*sizeof(GpuStaticPropInstance))) {
        setFallback("instances_upload_failed"); s->disabled = true; return false;
    }

    // Check frame: one in kParityEvery. Needs a host-readable copy of every pyramid level (recorded below, after the cull).
    // A pending check still owns `parityMips`: no new check frame until it has run.
    const bool parityFrame = (g_parity.runs++ % kParityEvery) == 0 && !s->parity.valid;
    bool parityRecord = false;
    VkDeviceSize parityOffset[64] = {};
    VkDeviceSize visOffset = 0;
    if (parityFrame && s->mipCount <= 64) {
        VkDeviceSize bytes = 0;
        for (int l = 0; l < s->mipCount; ++l) {
            parityOffset[l] = bytes;
            bytes += static_cast<VkDeviceSize>(s->levels[static_cast<size_t>(l)].w) * static_cast<VkDeviceSize>(s->levels[static_cast<size_t>(l)].h) * sizeof(float);
        }
        visOffset = bytes;   // the mask copy follows the mips (4-byte aligned: float levels)
        bytes += static_cast<VkDeviceSize>(feed.count + 1) * sizeof(uint32_t);
        if (!s->parityMips || s->parityMipBytes < bytes) {
            s->parityMips.reset();
            s->parityMipBytes = make_buffer(s, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, s->parityMips, "hzb.parity_mips") ? bytes : 0;
        }
        parityRecord = s->parityMipBytes >= bytes && s->parityMipBytes != 0;
    }
    if ((g_parity.runs % 600) == 1) parity_log("");

    ++g_health.attempted;
    using clk = std::chrono::steady_clock;
    const clk::time_point t0 = clk::now();

    // VK-RUNG8 item-4: direct-chain only — engage only when upstream is direct-chain,
    // else decline to the GL HZB pass. No GL<->VK semaphore handoff.
    VkCommandBuffer cb = s->cbuf[slot];   // one command buffer per frame slot

    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) {
        setFallback("record_failed");
        s->disabled = true;
        return false;
    }

    auto barrier = [&](VkImage img, VkImageAspectFlags aspect, VkImageLayout oldL, VkImageLayout newL,
                       VkAccessFlags srcA, VkAccessFlags dstA,
                       VkPipelineStageFlags srcS, VkPipelineStageFlags dstS) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = oldL;
        b.newLayout = newL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img;
        b.subresourceRange = {aspect, 0, 1, 0, 1};
        b.srcAccessMask = srcA;
        b.dstAccessMask = dstA;
        vkCmdPipelineBarrier(cb, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
    };

    barrier(s->depthRes->image.get(), VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    for (int level = 0; level < s->mipCount; ++level) {
        auto& lv = s->levels[static_cast<size_t>(level)];
        VkRenderPassBeginInfo rbi{};
        rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rbi.renderPass = s->rpass.get();
        rbi.framebuffer = lv.fb.get();
        rbi.renderArea = {{0, 0}, {static_cast<uint32_t>(lv.w), static_cast<uint32_t>(lv.h)}};
        rbi.clearValueCount = 0;
        MC2_VK_GPU_SCOPE(gpuScope, cb, "VKIsl.Post.HzbBuild");
        vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        mc2vk::infra::bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipe.get());
        VkViewport vp{0.0f, 0.0f, static_cast<float>(lv.w), static_cast<float>(lv.h), 0.0f, 1.0f};
        VkRect2D sc{{0, 0}, {static_cast<uint32_t>(lv.w), static_cast<uint32_t>(lv.h)}};
        vkCmdSetViewport(cb, 0, 1, &vp);
        vkCmdSetScissor(cb, 0, 1, &sc);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->playout.get(), 0, 1, &lv.set, 0, nullptr);
        vkCmdDraw(cb, 3, 1, 0, 0);
        vkCmdEndRenderPass(cb);
        gpuScope.end();
    }

    // Order the prior color pass's mask reads before this frame's reset.
    auto memoryBarrier = [&](VkPipelineStageFlags src, VkPipelineStageFlags dst, VkAccessFlags from, VkAccessFlags to) {
        VkMemoryBarrier m{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; m.srcAccessMask = from; m.dstAccessMask = to;
        vkCmdPipelineBarrier(cb,src,dst,0,1,&m,0,nullptr,0,nullptr);
    };
    memoryBarrier(VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdFillBuffer(cb,s->visibility.get(),0,sizeof(uint32_t),0);
    memoryBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                  VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    MC2_VK_GPU_SCOPE(gpuCull, cb, "VKIsl.Post.HzbCull");
    mc2vk::infra::bind(cb,VK_PIPELINE_BIND_POINT_COMPUTE,s->cullPipe.get());
    {
        struct CullParams {
            uint32_t count, reserved, typeCount, levels;
            float worldToClip[16];
            float viewportDepth[4];
        } push{feed.count, 0u, static_cast<uint32_t>(s->propBounds.size()), static_cast<uint32_t>(s->mipCount)};
        static_assert(sizeof(CullParams) == 96);
        std::memcpy(push.worldToClip, feed.worldToClipGL, sizeof(push.worldToClip));
        push.viewportDepth[0] = static_cast<float>(width_);
        push.viewportDepth[1] = static_cast<float>(height_);
        push.viewportDepth[2] = feed.objectDepthBias;
        vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,s->cullLayout.get(),0,1,&s->cullSet[slot],0,nullptr);
        vkCmdPushConstants(cb,s->cullLayout.get(),VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),&push);
        vkCmdDispatch(cb,(feed.count+63)/64,1,1);   // the barrier below orders the mask for the color pass and the host
    }
    gpuCull.end();
    memoryBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_HOST_BIT,
                  VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_HOST_READ_BIT);

    if (parityRecord) {
        // Pyramid levels to a host-readable buffer (check frames only), then back to the sampled layout.
        for (int l = 0; l < s->mipCount; ++l)
            barrier(s->levels[static_cast<size_t>(l)].image.get(), VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        for (int l = 0; l < s->mipCount; ++l) {
            const auto& lv = s->levels[static_cast<size_t>(l)];
            VkBufferImageCopy region{};
            region.bufferOffset = parityOffset[l];
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {static_cast<uint32_t>(lv.w), static_cast<uint32_t>(lv.h), 1};
            vkCmdCopyImageToBuffer(cb, lv.image.get(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s->parityMips.get(), 1, &region);
        }
        for (int l = 0; l < s->mipCount; ++l)
            barrier(s->levels[static_cast<size_t>(l)].image.get(), VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        memoryBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        const VkBufferCopy maskCopy{0, visOffset, static_cast<VkDeviceSize>(feed.count + 1) * sizeof(uint32_t)};
        vkCmdCopyBuffer(cb, s->visibility.get(), s->parityMips.get(), 1, &maskCopy);
        memoryBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
    }

    barrier(s->depthRes->image.get(), VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);

    if (vkEndCommandBuffer(cb) != VK_SUCCESS) {
        setFallback("record_failed");
        s->disabled = true;
        return false;
    }

    // VK-RUNG8 item-4: direct-chain only — the submit waits/signals no GL<->VK
    // semaphores; the images stay Vulkan-owned for the following native pass.
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    if (mc2vk::getOrCreateEngineVulkanContext().submit(si, VK_NULL_HANDLE, &s->ticket[slot]) != VK_SUCCESS) {
        setFallback("submit_failed");
        s->disabled = true;
        return false;
    }

    if (parityRecord) {
        ZoneScopedN("HZB.ParityCheck");
        auto& pc = s->parity;
        const uint32_t stride = (std::max)(1u, feed.count / kParitySamples);
        pc.ids.clear(); pc.inst.clear();
        for (uint32_t i = static_cast<uint32_t>(g_parity.checks % stride); i < feed.count; i += stride) {
            pc.ids.push_back(i);
            pc.inst.push_back(static_cast<const GpuStaticPropInstance*>(feed.instances112)[i]);
        }
        std::memcpy(pc.worldToClip, feed.worldToClipGL, sizeof(pc.worldToClip));
        pc.bias = feed.objectDepthBias;
        pc.bounds = s->propBounds;
        pc.generation = s->visibilityGeneration;
        pc.w = width_; pc.h = height_; pc.mips = s->mipCount;
        pc.ticket = s->ticket[slot];
        pc.visOffset = visOffset;
        pc.valid = true;
    }
    s->lastTicket = s->ticket[slot];
    s->previousCount = feed.count;
    *maskBuffer = reinterpret_cast<void*>(s->visibility.get());
    *maskGeneration = s->visibilityGeneration;
    ++hzbBuildCount_;
    g_health.copyDepthOps = 0;
    g_health.readbackOps = 0;
    g_health.vkDraws = static_cast<unsigned long>(s->mipCount);
    ++g_health.usedVulkan;
    g_health.renderUs = std::chrono::duration<double, std::micro>(clk::now() - t0).count();
    setFallback("");
    return true;
}

void gosPostProcess::destroyVkPostHzbNative()
{
    if (g_health.runtimeGate) {
        vlog("[HEALTH] runtime_gate=%d substrate_live=%d attempted=%lu used_vulkan=%lu "
             "direct_chain=%d used_interop=%lu levels=%lu vk_draws=%lu copy_depth_ops=%lu readback_ops=%lu "
             "fallback_reason=\"%s\" render_us_last=%.1f wait_us_last=%.1f wait_us_avg=%.1f wait_samples=%lu",
             g_health.runtimeGate, g_health.substrateLive, g_health.attempted,
             g_health.usedVulkan, g_health.directChain, g_health.usedInterop,
             g_health.levels, g_health.vkDraws,
             g_health.copyDepthOps, g_health.readbackOps, g_health.fallbackReason,
             g_health.renderUs, g_health.waitUsLast,
             g_health.waitSamples ? g_health.waitUsAccum / static_cast<double>(g_health.waitSamples) : 0.0,
             g_health.waitSamples);
    }
    if (g_health.runtimeGate) parity_log(" final=1");
    if (!vkPostHzbNative_) return;
    vkPostHzbNative_.reset();
}

extern "C" void mc2_vk_post_hzb_native_health(
    int* runtimeGate, int* substrateLive,
    unsigned long* attempted, unsigned long* usedVulkan,
    unsigned long* levels, unsigned long* vkDraws,
    unsigned long* copyDepthOps, unsigned long* readbackOps,
    const char** fallbackReason, double* renderUs)
{
    if (runtimeGate)    *runtimeGate = g_health.runtimeGate;
    if (substrateLive)  *substrateLive = g_health.substrateLive;
    if (attempted)      *attempted = g_health.attempted;
    if (usedVulkan)     *usedVulkan = g_health.usedVulkan;
    if (levels)         *levels = g_health.levels;
    if (vkDraws)        *vkDraws = g_health.vkDraws;
    if (copyDepthOps)   *copyDepthOps = g_health.copyDepthOps;
    if (readbackOps)    *readbackOps = g_health.readbackOps;
    if (fallbackReason) *fallbackReason = g_health.fallbackReason;
    if (renderUs)       *renderUs = g_health.renderUs;
}

