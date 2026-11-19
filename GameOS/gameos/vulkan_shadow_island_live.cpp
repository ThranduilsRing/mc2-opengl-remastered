// One D16 mover map and the readiness-gated static bake, on the shared Vulkan device.

#if defined(_WIN32) && !defined(VK_USE_PLATFORM_WIN32_KHR)
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <volk.h>
#include "load_scope.h"
#include "Gates/gate.h"
#include "hitch_stats.h"
#include "vk_render_pass_timer.h"
#include "vk_gpu_profile.h"   // VK-GPU-ZONES: MC2_VK_GPU_SCOPE
#include "Gates/gate_cold.h"   // str<>()

#include <vk_mem_alloc.h>

#include "../../RenderCore/gpu_abi/GpuStaticPropInstance.h"  // single-source 112B instance record
#include "vk_image_resource.h"
#include "vk_island_util.h"
#include "vk_owned.h"
#include "vk_one_shot.h"
#include "vulkan_context.h"
#include "vk_barrier_batch_vk.h"
#include "vulkan_shadow_island_live.h"
#include "vulkan_shadow_island_live_algo.h"
#include "vk_depth_caster_pipeline.h"
#include "vulkan_color_prop_island.h"
#include "vulkan_shadow_mech_casters.h"
#include "vulkan_shadow_bake.h"
#include "vulkan_mover_shadow.h"
#include "shadow_quality.h"
#include "gos_postprocess.h"
#include "gameos.hpp"
#include "static_caster_set.h"
#include "vk_shared_gbuffer_gate.h"
#include "local_light_shadow.h"
#include "prop_array_buckets.h"

#include <tracy/Tracy.hpp>   // VK-ISLAND-TRACY-1: per-frame CPU attribution zones

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cmath>
#include <cstdint>
#include <cstddef>   // offsetof (M2 layout asserts)
#include <vector>
#include <memory>
#include <string>
#include <chrono>
#include <algorithm>
#include "../../mclib/battle_bench_wait.h"

// ---------------------------------------------------------------------------
// Health snapshot (mirrors the water island's) — read by debug_state_dump.cpp.
// ---------------------------------------------------------------------------
float mc2ShadowMinSunAltDeg();

#include "vulkan_shadow_island_state.h"
#include "vk_island_fail_policy.h"
#include "vk_dynamic_rendering.h"

namespace {


void vlog(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vk_island_util::vlogv("[VK_SHADOW_ISLAND] ", fmt, ap);
    va_end(ap);
}

// The island is unconditionally direct-chain-native. Kept as a named constant
// documenting that the depth images are non-exportable and consumed via VkImageView only.
constexpr bool kShadowDirectChain = true;

std::string spv_dir() { return vk_island_util::spvDir(); }

VkShaderModule load_spv(VkDevice device, std::string_view path) {
    return vk_island_util::loadSpv(device, path, vlog);
}

// std140 per-frame UBO (LOCKSTEP with shadow_prop.vert ShadowFrameUBO).
struct ShadowFrameUBO {
    float lightSpaceMatrix[16]; // getDynamicLightSpaceMatrix() — GL_FALSE   (off 0)
    int32_t instBase;           //                                            (off 64)
    int32_t _pad0, _pad1, _pad2;
};
static_assert(sizeof(ShadowFrameUBO) == 80, "ShadowFrameUBO std140 size (shadow_prop.vert)");
static_assert(offsetof(ShadowFrameUBO, instBase) == 64, "ShadowFrameUBO instBase offset");

// Per-type packet (subset of GpuStaticPropPacket the draw loop needs). Copied once
// per map from the batcher's accessor.
struct TypeRun {
    uint32_t typeID;
    uint32_t firstPacket;
    uint32_t packetCount;
};

// A run of casters of one type in the sorted instance buffer: one draw per packet of the type.
struct InstRun {
    uint32_t typeID;
    uint32_t firstInstance;
    uint32_t count;
};

// The passes of one frame: the dynamic map, the near cascade layers and the full-map cascade. Each writes its light-space matrix to its own slice of the
// frame UBO (bound with a dynamic offset), so the passes can be recorded back to back without the GPU having read the matrix of the previous one.
constexpr uint32_t kMaxPassesPerFrame = 40;   // LOCAL-LIGHTS-2: + up to 2 slices per local shadow tile
// The largest minUniformBufferOffsetAlignment a Vulkan device may report.
constexpr VkDeviceSize kUboSliceBytes = 256;

// The persistent island state. The engine context owns it while it is parked (see IslandSlot::Shadow), so the objects below are released by the
// context ahead of the allocator and the device they were made on.
struct ShadowIsland : mc2vk::IslandState {
    bool disabled = false;
    bool inited   = false;
    bool useInterop = false;
    // The island renders VK-owned (non-exportable) products for direct VK consumers;
    // useInterop stays false (no GL view/semaphore machinery) but the producers MUST
    // still run. The run entry points gate on (useInterop || directChain). Without
    // this, the resource-strict stack silently fell back to the terrain island's
    // per-frame GL CSM depth readback (8192^2 x layers glGetTexImage ~= 125 ms/frame).
    bool directChain = false;

    int size = 0;   // square shadow map side (getDynamicShadowMapSize())
    VkFormat depthFormat = VK_FORMAT_D16_UNORM;

    // Borrowed from the shared engine context (NOT owned).
    VkInstance       instance  = VK_NULL_HANDLE;
    VkPhysicalDevice phys      = VK_NULL_HANDLE;
    VkDevice         device    = VK_NULL_HANDLE;
    uint32_t         gfxFamily = UINT32_MAX;
    VkQueue          queue     = VK_NULL_HANDLE;
    VmaAllocator     allocator = VK_NULL_HANDLE; // borrowed from ctx

    // VK-PRODUCED D32 depth (the shadow map). Non-exportable; consumed via VkImageView. The image resources release their image in the destructor below.
    VkImageResource depthResource{};   // D32, depth attachment + sampled

    uint32_t      vertexStride = 0;
    bool          geomUploaded = false;
    VkDeviceSize  instCapBytes[mc2vk::kMaxFramesInFlight] = {0, 0};   // per frame parity

    // Per-type packets + type runs (copied once per map).
    std::vector<Mc2ShadowPacket> packets;  // firstIndex/indexCount/baseVertex per packet
    std::vector<TypeRun>         typeRuns;  // firstPacket/packetCount per typeID (indexed by typeID)

    VkDescriptorSet   set[mc2vk::kMaxFramesInFlight] = {VK_NULL_HANDLE, VK_NULL_HANDLE};   // one per frame parity, allocated from `pool`, freed with it
    VkCommandBuffer   cbuf[mc2vk::kMaxFramesInFlight] = {VK_NULL_HANDLE, VK_NULL_HANDLE};   // the frame's command buffer, one per parity, allocated from `cpool`, freed with it

    // The frame's recording. The first pass of an epoch begins its `epoch % kMaxFramesInFlight` command buffer; every pass of the frame appends a
    // render pass to it and mc2_vk_shadow_island_submit_frame() submits it once. Nothing waits for the GPU after the submit: the parity's fence is waited when
    // that parity is recorded again, two frames later, and it is the retire of everything the parity's buffers held.
    uint64_t recordEpoch = ~0ull;
    uint32_t parity = 0;
    uint32_t passCount = 0;
    bool     recording = false;
    // The casters staged for this frame (the same set feeds every pass), sorted by type into runs of one draw each.
    const void* stagedInstances = nullptr;
    uint32_t    stagedInstanceCount = 0;
    std::vector<InstRun> instRuns;
    mc2shadow::InstanceOrder instanceOrder;
    // Foliage and fence casters. Per packet of the shared geometry: the layer of the colour island's alpha-ON array that the packet's colour-pass fragments sample,
    // and the UV sub-region scale (layer < 0: an opaque packet). Built once per map from the colour island's material feed; the array view is re-bound whenever the
    // colour island hands out a different one. Until both exist a frame draws those packets opaque (a solid shadow), as the first frames of a map always did.
    using PacketAlpha = mc2vk::CasterPacketAlpha;
    std::vector<PacketAlpha> packetAlpha;
    bool        alphaTableBuilt = false;
    std::array<VkImageView, mc2::prop_buckets::kMaxBuckets> alphaBoundViews{};
    uint32_t    alphaBoundGen = 0;   // the colour island's map generation the set was written for
    bool        alphaReady = false;   // this frame: the array is bound and the table is built
    // The single dynamic map is only the fallback of a receiver that has no cascades, so its pass is held until the frame's cascades are known: it is recorded
    // at the submit when the cascade array and the full-map cascade were not both produced, and never otherwise.

    // OWNED objects, declared in creation order: the destructor releases them in reverse, so a framebuffer goes before the view and render pass it uses.
    mc2vk::OwnedBuffer vbo;   // VK-owned copies of the shared static-prop VBO/IBO (P8a (B))
    mc2vk::OwnedBuffer ibo;
    mc2vk::OwnedBuffer instBuf[mc2vk::kMaxFramesInFlight];   // per-parity instance SSBO (memcpy of the GpuStaticPropInstance vector)
    mc2vk::OwnedBuffer ubo;          // kMaxPassesPerFrame slices per parity, one light-space matrix each
    mc2vk::OwnedImageView   depthAttView;   // depth-attachment view of the shared image
    mc2vk::GpuTicket       frameTicket[mc2vk::kMaxFramesInFlight];  // timeline ticket of the parity-p submit; default ticket is already complete
    mc2vk::OwnedDescriptorSetLayout dsl;
    mc2vk::OwnedDescriptorSetLayout alphaDsl;   // set 1 of the caster layout: the colour island's alpha-ON albedo array
    mc2vk::OwnedDescriptorPool      pool;
    mc2vk::OwnedDescriptorPool      alphaPool;
    VkDescriptorSet                 alphaSet = VK_NULL_HANDLE;   // allocated from alphaPool, freed with it
    mc2vk::OwnedSampler             alphaSampler;
    mc2vk::OwnedPipelineLayout      playout;
    mc2vk::ShadowDrawRing           drawRing;   // the static-prop caster draws of a frame's passes as indirect records (one slice per parity)
    mc2vk::OwnedShaderModule        vert;
    mc2vk::OwnedShaderModule        frag;
    mc2vk::OwnedShaderModule        alphaFrag;
    mc2vk::OwnedPipeline            pipe;
    mc2vk::OwnedPipeline            alphaPipe;   // foliage and fence casters: discards the texels the colour pass discards
    mc2vk::OwnedCommandPool         cpool[mc2vk::kMaxFramesInFlight];   // one pool per parity: vkResetCommandPool instead of per-cb reset
    // The mech casters share the depth format and the UBO above, the static map the depth format and the geometry; declared last so they are released first.
    mc2vk::ShadowMechCasters        mech;
    mc2vk::ShadowBake               bake;   // borrows the alpha layout and the caster geometry above, so it is released first
    mc2vk::MoverShadowState         mover;
    mc2vk::OwnedImageView           debugArrayView; // compatibility view of the same one-layer D16 map, never a second allocation

    // LOCAL-LIGHTS-2: the local-light shadow atlas (local_light_shadow.h). `staticAtlas` keeps each tile's baked static casters
    // (TRANSFER_SRC between frames); `liveAtlas` = static tile copy + this frame's mechs (DEPTH_STENCIL_READ_ONLY between frames),
    // sampled by every pool receiver.
    struct LocalShadow {
        bool ok = false;           // images, passes and sets exist
        bool layoutsReady = false; // the first-use transitions were recorded
        bool ready = false;        // liveAtlas has been through a recorded frame
        VkImageResource staticAtlas{}, liveAtlas{};   // their depth-aspect views serve as attachment and sampled view
        mc2vk::OwnedBuffer inst[mc2vk::kMaxFramesInFlight];
        VkDeviceSize instCap[mc2vk::kMaxFramesInFlight] = {0, 0};
        mc2vk::OwnedDescriptorPool pool;
        VkDescriptorSet set[mc2vk::kMaxFramesInFlight] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
        uint64_t bakedKey[mc2localshadow::kTiles] = {};
        std::vector<GpuStaticPropInstance> leaves;   // the resolved static caster set
        uint64_t leavesVersion = ~0ull, leavesGeneration = ~0ull;
        uint32_t tilesDrawn = 0;
        uint64_t bakesTotal = 0;
    } local;

    ShadowIsland() = default;
    // The views and framebuffers over the two image resources go first; the resources hold their images with raw allocations and are released here.
    ~ShadowIsland() override {
        debugArrayView.reset();
        depthAttView.reset();
        if (device != VK_NULL_HANDLE) {
            vkImageResourceDestroy(&depthResource, device);
            vkImageResourceDestroy(&local.staticAtlas, device);
            vkImageResourceDestroy(&local.liveAtlas, device);
        }
    }
};

std::unique_ptr<mc2vk::IslandState>& islandSlot() {
    return mc2vk::getOrCreateEngineVulkanContext().islandSlot(mc2vk::IslandSlot::Shadow);
}
ShadowIsland* parkedIsland() { return static_cast<ShadowIsland*>(islandSlot().get()); }

// Init failure policy (vk_island_fail_policy.h), as the mech/terrain islands: before the readiness gates (VK context not born /
// geometry not finalized) init retries next frame; past them a failure is fatal (one transient-VkResult retry).
// g_shadowInitReachedBuild flips to 1 once init passes the context + gate readiness gates.
int  g_shadowInitReachedBuild = 0;

bool make_buffer(ShadowIsland* s, mc2vk::OwnedBuffer& out, VkDeviceSize size, VkBufferUsageFlags usage,
                 bool hostVisible, std::string_view name, int i0 = -1, int i1 = -1) {
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size ? size : 16; bci.usage = usage;
    VmaAllocationCreateInfo aci{};
    aci.usage = hostVisible ? VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE : VMA_MEMORY_USAGE_AUTO;   // host-visible here = CPU-written, GPU-read: ReBAR device-local where available
    aci.flags = hostVisible ? VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT : 0;
    if (!out.create(s->allocator, s->device, bci, aci, name, i0, i1)) {
        vlog("vmaCreateBuffer failed."); return false;
    }
    return true;
}

// Writes the instances into a mapped instance buffer in the sorted order. A mapped allocation is only aligned to its buffer's offset alignment, which can be
// less than the instance's alignas(16): a typed struct copy lets the compiler use aligned 16-byte stores and the process faults (access violation at
// 0xFFFF...) when the allocator hands out a 4- or 8-aligned offset, so the bytes go through memcpy. The caller names the buffer's capacity: a longer order is
// refused (and logged) instead of writing past the allocation.
void copy_sorted_instances(void* mapped, VkDeviceSize capBytes, const GpuStaticPropInstance* insts, const std::vector<uint32_t>& order) {
    if (static_cast<VkDeviceSize>(order.size()) * sizeof(GpuStaticPropInstance) > capBytes) {
        vlog("instance re-stage refused: %zu instances do not fit the %llu-byte buffer.", order.size(), static_cast<unsigned long long>(capBytes));
        return;
    }
    uint8_t* dst = static_cast<uint8_t*>(mapped);
    for (size_t k = 0; k < order.size(); ++k)
        std::memcpy(dst + k * sizeof(GpuStaticPropInstance), &insts[order[k]], sizeof(GpuStaticPropInstance));
}

void write_descriptors(ShadowIsland* s, uint32_t parity) {
    if (s->set[parity] == VK_NULL_HANDLE || !s->instBuf[parity]) return;
    VkDescriptorBufferInfo uboInfo{ s->ubo.get(), 0, sizeof(ShadowFrameUBO) };
    VkDescriptorBufferInfo instInfo{ s->instBuf[parity].get(), 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet w[2]{};
    w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstSet = s->set[parity];
    w[0].dstBinding = 0; w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; w[0].pBufferInfo = &uboInfo;
    w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[1].dstSet = s->set[parity];
    w[1].dstBinding = 1; w[1].descriptorCount = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[1].pBufferInfo = &instInfo;
    vkUpdateDescriptorSets(s->device, 2, w, 0, nullptr);
}

// Grows the instance buffer of one parity. The caller has waited the parity's frame fence, so no submit still reads the buffer being replaced.
bool ensure_inst_capacity(ShadowIsland* s, uint32_t parity, VkDeviceSize bytes) {
    if (s->instBuf[parity] && bytes <= s->instCapBytes[parity]) return true;
    s->instBuf[parity].reset();
    VkDeviceSize cap = bytes + bytes / 2 + 4096;
    if (!make_buffer(s, s->instBuf[parity], cap, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, "shadow.instBuf", static_cast<int>(parity)))
        return false;
    s->instCapBytes[parity] = cap;
    write_descriptors(s, parity);
    return true;
}

// The per-packet push constant of the alpha-tested caster (vec2 uvScale, int layer, pad); declared above the pipelines so the layout checker pairs it with them.
constexpr VkPushConstantRange kCasterPushRange{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};

// The static-prop caster shaders. Loaded above the pipeline builder so the offline layout checker, which pairs a vertex binding with the shader names named
// before it in the file, checks the prop caster stride and attributes against shadow_prop.vert.
static bool load_caster_modules(ShadowIsland* s) {
    s->vert = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir() + "shadow_prop.vert.spv"));
    s->frag = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir() + "shadow_prop.frag.spv"));
    s->alphaFrag = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir() + "shadow_prop_alpha.frag.spv"));
    return s->vert && s->frag && s->alphaFrag;
}

// A static-prop caster pipeline: the shared static-prop vertex (position at 0, UV at 24; the alpha-tested kind reads the UV, the opaque kind ignores it).
bool create_caster_pipeline(ShadowIsland* s, mc2vk::OwnedPipeline& out, VkShaderModule frag, std::string_view name) {
    VkVertexInputBindingDescription vib{};
    vib.binding = 0; vib.stride = mc2_static_prop_vertex_stride();   // baked before the geometry upload: s->vertexStride is still 0 here
    vib.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription via[2]{};
    via[0].location = 0; via[0].binding = 0; via[0].format = VK_FORMAT_R32G32B32_SFLOAT; via[0].offset = 0;
    via[1].location = 2; via[1].binding = 0; via[1].format = VK_FORMAT_R32G32_SFLOAT;    via[1].offset = 24;   // the UV of the shared static-prop vertex
    if (!mc2vk::createDepthCasterPipeline(out, s->device, s->depthFormat, s->playout.get(), s->vert.get(), frag, vib, via)) {
        vlog("vkCreateGraphicsPipelines failed.");
        return false;
    }
    mc2vk::nameObject(s->device, out.get(), name);
    return true;
}

// LOCAL-LIGHTS-2: the local-light shadow atlas pair, its two render passes (both compatible with the caster pipelines' pass: one D16
// attachment) and the descriptor sets over its own caster instance buffers. Failure leaves the island without local shadows (logged).
static bool init_local_shadow(ShadowIsland* s) {
    auto& L = s->local;
    using namespace mc2localshadow;
    if (!vkImageResourceCreate(&L.staticAtlas, s->device, s->phys, kAtlasW, kAtlasH, s->depthFormat,
                               VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, "shadow.localStatic") ||
        !vkImageResourceCreate(&L.liveAtlas, s->device, s->phys, kAtlasW, kAtlasH, s->depthFormat,
                               VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                               "shadow.localLive")) {
        vlog("local light shadow atlas unavailable."); return false;
    }
    // bake: static tiles stay TRANSFER_SRC between frames (the live atlas copies them every frame).
    // live: the copied tiles arrive TRANSFER_DST and leave DEPTH_STENCIL_READ_ONLY for the pool receivers.
    // Both passes are dynamic rendering passes: record_local_shadows records the layout moves (image barriers) around them.
    VkDescriptorPoolSize sizes[2]{{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, mc2vk::kMaxFramesInFlight}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, mc2vk::kMaxFramesInFlight}};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = mc2vk::kMaxFramesInFlight; pci.poolSizeCount = 2; pci.pPoolSizes = sizes;
    if (!mc2vk::create(L.pool, s->device, [&](auto* h) { return vkCreateDescriptorPool(s->device, &pci, nullptr, h); })) return false;
    VkDescriptorSetLayout layouts[mc2vk::kMaxFramesInFlight]; for (auto& layout : layouts) layout = s->dsl.get();
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = L.pool.get(); dsai.descriptorSetCount = mc2vk::kMaxFramesInFlight; dsai.pSetLayouts = layouts;
    if (MC2_VK_ALLOC_SETS(s->device, &dsai, L.set) != VK_SUCCESS) return false;
    L.ok = true;
    vlog("local light shadow atlas %ux%u (%u tiles of %u^2, D16).", kAtlasW, kAtlasH, kTiles, kTileSide);
    return true;
}

// One-time init: borrow the shared device, build the exportable D32 shadow image +
// its GL interop view (GL_DEPTH_COMPONENT32F, compare-mode), depth-attachment view,
// depth-only render pass + framebuffer, instance/UBO buffers, pipeline, cmd pool.
bool init_island(ShadowIsland* s, int size) {
    s->size = size;
    s->depthFormat = VK_FORMAT_D16_UNORM;
    ++g_health.attempted;
    g_shadowInitReachedBuild = 0;   // reset; flips to 1 after readiness gates pass

    if (!mc2vk::ensureEngineVulkanContext()) {
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

    // Readiness gates (context + gate) passed: any failure past this point is a PERMANENT
    // build failure -> the island declines for the session (item-2 boot-decline).
    g_shadowInitReachedBuild = 1;

    // ---- VK-native D32 depth image. SAMPLED (native screenfx samples the VkImageView)
    // + DEPTH_STENCIL_ATTACHMENT (VK renders into it). Non-exportable (VK-RUNG8-ITEM6). ----
    const bool directChain = kShadowDirectChain;
    bool ok = vkImageResourceCreate(&s->depthResource, s->device, s->phys,
                                    static_cast<uint32_t>(size), static_cast<uint32_t>(size), s->depthFormat,
                                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                    VK_IMAGE_USAGE_SAMPLED_BIT, "shadow.depthResource");
    if (ok) mc2vk::nameObject(s->device, s->depthResource.image.get(), "shadow.depthResource");
    if (!ok) {
        vkImageResourceDestroy(&s->depthResource, s->device);
        setFallback("depth_resource_setup_failed");
        return false; // P8a is native-only; no readback fallback path
    }

    s->useInterop = !directChain;      // false (item-4: no GL<->VK semaphore machinery)
    s->directChain = directChain;      // true
    g_health.directChain = 1;
    g_health.interopActive = 0;        // item-4: no interop handoff
    g_health.vk2glSemActive = 0;       // item-4: no vk2gl semaphore
    vlog("depth producer ACTIVE - VK-native %s shadow map (direct-chain).","D16 movers");

    // ---- Depth-attachment view of the shared image (the interop view's own
    // .view is the SAMPLED view; the render pass needs a DEPTH-aspect attachment
    // view — same image, depth aspect). ----
    {
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = s->depthResource.image.get();
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = s->depthFormat;
        vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        if (!mc2vk::create(s->depthAttView, s->device, [&](auto* h) { return vkCreateImageView(s->device, &vi, nullptr, h); })) {
            vlog("depth attachment view failed."); return false;
        }
        {
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
            if (!mc2vk::create(s->debugArrayView,s->device,[&](auto* h) { return vkCreateImageView(s->device,&vi,nullptr,h); })) return false;
        }
    }

    // ---- Instance SSBO + UBO. ----
    if (!make_buffer(s, s->ubo, mc2vk::kMaxFramesInFlight * kMaxPassesPerFrame * kUboSliceBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true, "shadow.ubo")) return false;

    // ---- Depth-only dynamic rendering pass (single depth attachment; CLEAR to 1.0 forward-Z, STORE). Interop layout: VK is the sole producer; the
    // pass acquires the image in UNDEFINED (contents don't survive: it CLEARs every frame) and leaves it in DEPTH_STENCIL_READ_ONLY for the samplers.
    // record_pass records both layout moves and the stage/access dependencies (the VkSubpassDependency pair this pass used to carry). ----

    // ---- Descriptor layout/pool/set (UBO binding0 VS, instance SSBO binding1 VS). ----
    {
        VkDescriptorSetLayoutBinding b[2]{};
        b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.bindingCount = 2; lci.pBindings = b;
        if (!mc2vk::create(s->dsl, s->device, [&](auto* h) { return vkCreateDescriptorSetLayout(s->device, &lci, nullptr, h); })) {
            vlog("vkCreateDescriptorSetLayout failed."); return false;
        }
        VkDescriptorPoolSize sizes[2]{};
        sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; sizes[0].descriptorCount = mc2vk::kMaxFramesInFlight;
        sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; sizes[1].descriptorCount = mc2vk::kMaxFramesInFlight;
        VkDescriptorPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.maxSets = mc2vk::kMaxFramesInFlight; pci.poolSizeCount = 2; pci.pPoolSizes = sizes;
        if (!mc2vk::create(s->pool, s->device, [&](auto* h) { return vkCreateDescriptorPool(s->device, &pci, nullptr, h); })) {
            vlog("vkCreateDescriptorPool failed."); return false;
        }
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        const VkDescriptorSetLayout dslHandle = s->dsl.get();
        VkDescriptorSetLayout setLayouts[mc2vk::kMaxFramesInFlight]; for (auto& layout : setLayouts) layout = dslHandle;
        dsai.descriptorPool = s->pool.get(); dsai.descriptorSetCount = mc2vk::kMaxFramesInFlight; dsai.pSetLayouts = setLayouts;
        if (MC2_VK_ALLOC_SETS(s->device, &dsai, s->set) != VK_SUCCESS) {
            vlog("vkAllocateDescriptorSets failed."); return false;
        }
        for (uint32_t par = 0; par < mc2vk::kMaxFramesInFlight; ++par) {
            if (!ensure_inst_capacity(s, par, 256 * 1024)) return false;   // ~2340 instances to start
        }
    }

    // ---- Pipelines: the opaque caster and the alpha-tested one share one layout (set 0 the frame UBO + instances, set 1 the alpha array, a push constant
    // of the per-packet layer and UV scale), so the set 0 binding of a pass stays valid across a switch of pipeline. ----
    {
        if (!load_caster_modules(s)) { setFallback("spv_load_failed"); return false; }
        if (!s->drawRing.init(s->allocator, s->device, 1u << 17)) { setFallback("draw_ring_failed"); return false; }

        // Set 1: the alpha-ON albedo array the colour island imported (bound later, when it exists).
        VkDescriptorSetLayoutBinding ab{};
        ab.binding = 0; ab.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; ab.descriptorCount = mc2::prop_buckets::kMaxBuckets; ab.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo alci{};
        alci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        alci.bindingCount = 1; alci.pBindings = &ab;
        if (!mc2vk::create(s->alphaDsl, s->device, [&](auto* h) { return vkCreateDescriptorSetLayout(s->device, &alci, nullptr, h); })) { vlog("alpha descriptor layout failed."); return false; }
        VkDescriptorPoolSize aps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, mc2::prop_buckets::kMaxBuckets};
        VkDescriptorPoolCreateInfo apci{};
        apci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        apci.maxSets = 1; apci.poolSizeCount = 1; apci.pPoolSizes = &aps;
        if (!mc2vk::create(s->alphaPool, s->device, [&](auto* h) { return vkCreateDescriptorPool(s->device, &apci, nullptr, h); })) { vlog("alpha descriptor pool failed."); return false; }
        const VkDescriptorSetLayout alphaLayout = s->alphaDsl.get();
        VkDescriptorSetAllocateInfo adsai{};
        adsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        adsai.descriptorPool = s->alphaPool.get(); adsai.descriptorSetCount = 1; adsai.pSetLayouts = &alphaLayout;
        if (MC2_VK_ALLOC_SETS(s->device, &adsai, &s->alphaSet) != VK_SUCCESS) { vlog("alpha descriptor set allocation failed."); return false; }
        VkSamplerCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR; sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.maxLod = VK_LOD_CLAMP_NONE;
        if (!mc2vk::create(s->alphaSampler, s->device, [&](auto* h) { return vkCreateSampler(s->device, &sci, nullptr, h); })) { vlog("alpha sampler failed."); return false; }

        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        const VkDescriptorSetLayout setLayouts[2] = {s->dsl.get(), s->alphaDsl.get()};
        plci.setLayoutCount = 2; plci.pSetLayouts = setLayouts;
        plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &kCasterPushRange;
        if (!mc2vk::create(s->playout, s->device, [&](auto* h) { return vkCreatePipelineLayout(s->device, &plci, nullptr, h); })) {
            vlog("vkCreatePipelineLayout failed."); return false;
        }
        if (!create_caster_pipeline(s, s->pipe, s->frag.get(), "shadow.pipe")) return false;
        if (!create_caster_pipeline(s, s->alphaPipe, s->alphaFrag.get(), "shadow.alphaPipe")) return false;
    }

    // ---- Mech casters: a failure leaves the island with static-prop casters only (logged by the casters). ----
    if (!s->mech.init(s->device, s->allocator, s->depthFormat, s->ubo.get(), sizeof(ShadowFrameUBO), spv_dir()))
        vlog("mech casters unavailable: the cascades carry static-prop casters only.");
    // ---- Static sun shadow map: a failure leaves terrain and objects without building and terrain shadows (logged by the map). ----
    // ---- Static shadow bake (mask + object map): a failure leaves the bake off (logged by the bake). ----
    if (!s->bake.init(s->device, s->allocator, s->phys, s->alphaDsl.get(), spv_dir()))
        vlog("static shadow bake unavailable on this device.");
    if (!s->mover.init(ctx,true)) return false;
    // ---- LOCAL-LIGHTS-2: Potato keeps unshadowed local lights. ----
    if (mc2shadow::shadowQuality().level > 0) init_local_shadow(s);

    // ---- Command pool + 2 command buffers (per parity). ----
    {
        VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.queueFamilyIndex = s->gfxFamily;
        for (uint32_t i = 0; i < mc2vk::kMaxFramesInFlight; ++i) {
            if (!mc2vk::create(s->cpool[i], s->device, [&](auto* h) { return vkCreateCommandPool(s->device, &pci, nullptr, h); })) { vlog("vkCreateCommandPool failed."); return false; }
            VkCommandBufferAllocateInfo cai{};
            cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            cai.commandPool = s->cpool[i].get(); cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
            if (vkAllocateCommandBuffers(s->device, &cai, &s->cbuf[i]) != VK_SUCCESS) {
                vlog("vkAllocateCommandBuffers failed."); return false;
            }
        }
    }

    s->inited = true;
    setFallback("");
    vlog("island initialized %dx%d (borrowed shared device; native %s depth map; "
         "LESS forward-Z, dynamic depth bias).",size,size,"D16 movers");
    return true;
}

void release_geometry(ShadowIsland* s) {
    // In-flight frames may still draw these: park them on the open-frame ticket instead of idling the device.
    if (s->vbo) mc2vk::getOrCreateEngineVulkanContext().retire(std::move(s->vbo));
    if (s->ibo) mc2vk::getOrCreateEngineVulkanContext().retire(std::move(s->ibo));
    s->geomUploaded = false;
    s->mover.clearGeometry();
    s->packets.clear();
    s->typeRuns.clear();
    s->packetAlpha.clear();
    s->alphaTableBuilt = false;
}

// Copy the shared static-prop VBO/IBO into VK-owned buffers ONCE per map (P8a
// feed (B): VK owns a copy — simplest working feed). Also caches the per-type
// packet ranges. Returns false if the geometry feed is unavailable this frame.
bool upload_geometry_once(ShadowIsland* s) {
    if (s->geomUploaded) return true;
    release_geometry(s);   // drop any half-built buffers from a failed earlier attempt
    Mc2ShadowGeomFeed feed{};
    if (!mc2_shadow_island_get_geometry_feed(&feed)) return false;
    // The vertex / index bytes come from the batcher's CPU copy (the bytes the GL buffers were created from).
    const void* vboCpu = nullptr; const void* iboCpu = nullptr;
    uint64_t vboBytes = 0, iboBytes = 0;
    if (!mc2_static_prop_geometry_cpu(&vboCpu, &vboBytes, &iboCpu, &iboBytes)) return false;
    if (vboBytes == 0 || iboBytes == 0 || vboBytes != feed.vboBytes || iboBytes != feed.iboBytes) {
        vlog("geometry declined: CPU copy %llu/%llu bytes != feed %llu/%llu.", static_cast<unsigned long long>(vboBytes),
             static_cast<unsigned long long>(iboBytes), static_cast<unsigned long long>(feed.vboBytes), static_cast<unsigned long long>(feed.iboBytes));
        return false;
    }

    s->vertexStride = feed.vertexStride;
    if (feed.vertexStride != mc2_static_prop_vertex_stride())
        vlog("ERROR: feed vertexStride %u != pipeline stride %u (shadow geometry will explode).", feed.vertexStride, mc2_static_prop_vertex_stride());

    if (!make_buffer(s, s->vbo, vboBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true, "shadow.vbo")) return false;
    if (!make_buffer(s, s->ibo, iboBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true, "shadow.ibo")) return false;
    { void* p = nullptr;
      if (vmaMapMemory(s->allocator, s->vbo.allocation(), &p) != VK_SUCCESS) return false;
      std::memcpy(p, vboCpu, static_cast<size_t>(vboBytes));
      vmaFlushAllocation(s->allocator, s->vbo.allocation(), 0, VK_WHOLE_SIZE); vmaUnmapMemory(s->allocator, s->vbo.allocation()); }
    { void* p = nullptr;
      if (vmaMapMemory(s->allocator, s->ibo.allocation(), &p) != VK_SUCCESS) return false;
      std::memcpy(p, iboCpu, static_cast<size_t>(iboBytes));
      vmaFlushAllocation(s->allocator, s->ibo.allocation(), 0, VK_WHOLE_SIZE); vmaUnmapMemory(s->allocator, s->ibo.allocation()); }

    s->packets.assign(feed.packets, feed.packets + feed.packetCount);
    s->typeRuns.clear();
    s->typeRuns.resize(feed.typeCount);
    for (uint32_t t = 0; t < feed.typeCount; ++t) {
        s->typeRuns[t].typeID      = t;
        s->typeRuns[t].firstPacket = feed.typeFirstPacket[t];
        s->typeRuns[t].packetCount = feed.typePacketCount[t];
    }
    g_health.geomVerts = static_cast<unsigned long>((feed.vboBytes / (feed.vertexStride ? feed.vertexStride : 1)));
    if (!s->mover.cacheGeometry(feed,vboCpu,iboCpu)) return false;
    s->geomUploaded = true;
    vlog("caster geometry uploaded once: vbo=%llu B ibo=%llu B stride=%u packets=%u types=%u.",
         static_cast<unsigned long long>(feed.vboBytes), static_cast<unsigned long long>(feed.iboBytes),
         feed.vertexStride, feed.packetCount, feed.typeCount);
    return true;
}

// ===========================================================================
// MC2_VK_SHADOW_MATRIX_DIAG (Trace gate, default OFF). Evidence-only: matrices as
// received by the island per map, GL/VK depth readbacks -> normalized gray PNG.
// ===========================================================================
} // namespace

// ===========================================================================
// PUBLIC ENTRY POINTS (consumed under #ifdef MC2_VULKAN_ISLAND from
// gos_postprocess.cpp / txmmgr.cpp / debug_state_dump.cpp).
// ===========================================================================

// True when the gate requests the VK shadow region (the caller then SKIPS the GL
// dynamic shadow WRITE draws and calls mc2_vk_shadow_island_run instead).
extern "C" int mc2_vk_shadow_island_enabled() {
    return 1;
}

// A product the frame has recorded is usable by a consumer only after the frame's submit: the consumers run later on the same queue, and the render pass
// dependencies order their reads after the writes.
static bool productReady(bool recorded) {
    const ShadowIsland* s = parkedIsland();
    return s && recorded && !s->recording;
}

// Starts the frame's recording when the first pass of an epoch asks. The slot's fence retires its previous submit: it is
// never a wait on this frame's work, and in steady state it has long been signalled.
static bool begin_frame(ShadowIsland* s, uint64_t epoch) {
    if (s->recording && s->recordEpoch == epoch) return true;
    const uint32_t par = static_cast<uint32_t>(epoch % mc2vk::kMaxFramesInFlight);
    { ZoneScopedN("VKIsl.Shadow.FenceWait"); mc2::battlebench::WaitScope gpuWait1(mc2::battlebench::WaitGpuMainNs); mc2_hitch2::FenceWaitScope slotWait("shadow_island_live", mc2_hitch2::kSlot);
      if (!mc2vk::getOrCreateEngineVulkanContext().wait(s->frameTicket[par], 5000000000ull, "shadow frame slot")) {
          setFallback("fence_timeout"); s->disabled = true; return false;
      } }
    VkCommandBuffer cb = s->cbuf[par];
    vkResetCommandPool(s->device, s->cpool[par].get(), 0);   // also drops a recording an earlier frame never submitted
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) { setFallback("record_failed"); s->disabled = true; return false; }
    vk_render_pass_timer::Begin(cb, gos_render_pass_timer::Pass_ShadowDyn);
    s->recordEpoch = epoch;
    s->parity = par;
    s->passCount = 0;
    s->drawRing.begin(par);
    s->recording = true;
    s->stagedInstances = nullptr;
    s->stagedInstanceCount = 0;
    s->instRuns.clear();
    return true;
}

// Copies the frame's static-prop casters into the parity's instance buffer, sorted by type so each type is one instanced draw per packet. Every pass of a
// frame passes the same set; a different set in the middle of a frame has no buffer of its own and is refused.
static bool stage_instances(ShadowIsland* s, const void* instances, uint32_t instanceCount) {
    // An empty set still opens the frame's passes (the mechs cast without any prop); a null data pointer then stands in as a marker.
    if (instances == nullptr) instances = &s->stagedInstanceCount;
    if (s->stagedInstances == instances && s->stagedInstanceCount == instanceCount) return true;
    if (s->stagedInstances != nullptr) {
        static bool logged = false;
        if (!logged) { logged = true; vlog("a second caster set in one frame is not staged (the passes of a frame share one)."); }
        return false;
    }
    if (instanceCount == 0) {
        s->instanceOrder.clear();
        s->instRuns.clear();
        s->stagedInstances = instances;
        s->stagedInstanceCount = 0;
        return true;
    }
    ZoneScopedN("VKIsl.Shadow.Feed");
    const VkDeviceSize instBytes = static_cast<VkDeviceSize>(instanceCount) * sizeof(GpuStaticPropInstance);
    if (!ensure_inst_capacity(s, s->parity, instBytes)) { s->disabled = true; return false; }
    const GpuStaticPropInstance* insts = static_cast<const GpuStaticPropInstance*>(instances);
    const auto& order = s->instanceOrder.update(std::span(insts, instanceCount));
    void* mapped = nullptr;
    if (vmaMapMemory(s->allocator, s->instBuf[s->parity].allocation(), &mapped) != VK_SUCCESS) { setFallback("inst_map_failed"); s->disabled = true; return false; }
    copy_sorted_instances(mapped, s->instCapBytes[s->parity], insts, order);
    vmaFlushAllocation(s->allocator, s->instBuf[s->parity].allocation(), 0, VK_WHOLE_SIZE);
    vmaUnmapMemory(s->allocator, s->instBuf[s->parity].allocation());
    s->instRuns.clear();
    for (uint32_t i = 0; i < instanceCount;) {
        const uint32_t tid = insts[order[i]].typeID;
        uint32_t j = i;
        while (j < instanceCount && insts[order[j]].typeID == tid) ++j;
        s->instRuns.push_back({tid, i, j - i});
        i = j;
    }
    s->stagedInstances = instances;
    s->stagedInstanceCount = instanceCount;
    g_health.instRecords = instanceCount;
    g_health.typeRuns = s->instRuns.size();
    return true;
}

// Makes this frame's alpha-tested casters possible: binds the colour island's alpha-ON array when it has one (a new view idles the device first, as the set
// may still be read by a frame in flight; that is once per map) and builds the per-packet table from its material feed.
static void refresh_alpha_casters(ShadowIsland* s) {
    void* views[mc2::prop_buckets::kMaxBuckets]{};
    uint32_t generation = 0;
    s->alphaReady = false;
    if (!mc2_color_prop_island_get_alpha_arrays(views, &generation) || !views[0]) return;
    // PROP-ARRAY-BUCKETS: binding 0 is the array of the group's size buckets (packet layers carry the bucket).
    std::array<VkImageView, mc2::prop_buckets::kMaxBuckets> arrayViews{};
    for (uint32_t b = 0; b < mc2::prop_buckets::kMaxBuckets; ++b) arrayViews[b] = reinterpret_cast<VkImageView>(views[b]);
    if (arrayViews != s->alphaBoundViews || generation != s->alphaBoundGen) {
        mc2vk::getOrCreateEngineVulkanContext().waitIdle("shadow_alpha_array");
        VkDescriptorImageInfo info[mc2::prop_buckets::kMaxBuckets]{};
        for (uint32_t b = 0; b < mc2::prop_buckets::kMaxBuckets; ++b)
            info[b] = {s->alphaSampler.get(), arrayViews[b], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.dstSet = s->alphaSet; w.dstBinding = 0; w.descriptorCount = mc2::prop_buckets::kMaxBuckets;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = info;
        vkUpdateDescriptorSets(s->device, 1, &w, 0, nullptr);
        s->alphaBoundViews = arrayViews;
        s->alphaBoundGen = generation;
    }
    if (!s->alphaTableBuilt) {
        Mc2ColorPropMaterialFeed mf{};
        if (!mc2_color_prop_island_get_material_feed(&mf) || mf.bucketed || mf.arrayKeyOn == 0 || !mf.packetAlbedoLayer) return;
        s->packetAlpha.assign(s->packets.size(), ShadowIsland::PacketAlpha{});
        uint32_t alphaPackets = 0;
        for (uint32_t t = 0; t < mf.typeCount && t < s->typeRuns.size(); ++t) {
            if (mf.typeAlphaClass[t] != 1) continue;   // class 1 = the alpha-ON group
            const TypeRun& tr = s->typeRuns[t];
            for (uint32_t k = 0; k < tr.packetCount; ++k) {
                const uint32_t pk = tr.firstPacket + k;
                if (pk >= s->packets.size() || pk >= mf.packetCount || mf.packetAlbedoLayer[pk] < 0) continue;
                s->packetAlpha[pk].layer = mf.packetAlbedoLayer[pk];
                if (mf.packetUvScaleX && mf.packetUvScaleY) { s->packetAlpha[pk].scaleX = mf.packetUvScaleX[pk]; s->packetAlpha[pk].scaleY = mf.packetUvScaleY[pk]; }
                ++alphaPackets;
            }
        }
        s->alphaTableBuilt = true;
        vlog("alpha-tested casters: %u packets cut their silhouette from the alpha-ON array.", alphaPackets);
    }
    s->alphaReady = s->alphaTableBuilt;
}

// Appends the static map's build to the frame's recording when the engine has a generation the map has not built (a map load, a destroy rebake): once
// per generation, never per frame. It counts as a pass of the frame, so the frame is submitted even when every other pass declines.
// Static inputs and dirty versions schedule the bake independently of camera motion.

static bool mover_shadows_enabled() {
    static const bool enabled=mc2::gate::get<"MC2_SHADOW_ENABLE">();
    const auto* pp=getGosPostProcess();
    return !mc2::options::lowCost() && enabled && pp && pp->shadowsEnabled_;
}

// Geometry half of the bake inputs, shared by record_bake and the prewarm.
static void fill_bake_geometry(ShadowIsland* s, mc2vk::ShadowBakeInputs& in, std::vector<uint32_t>& fp, std::vector<uint32_t>& pc) {
    fp.resize(s->typeRuns.size()); pc.resize(s->typeRuns.size());
    for (size_t t = 0; t < fp.size(); ++t) { fp[t] = s->typeRuns[t].firstPacket; pc[t] = s->typeRuns[t].packetCount; }
    in.vbo = s->vbo.get(); in.ibo = s->ibo.get(); in.packets = s->packets; in.typeFirstPacket = fp; in.typePacketCount = pc;
}

// The static shadow bake: retires a finished bake and records the next one when its inputs are ready and differ from the last. Like the static map it
// counts as a pass, so its frame is submitted even when every other pass declines.
static void record_bake(ShadowIsland* s) {
    mc2vk::ShadowBakeInputs in;
    std::vector<uint32_t> firstPacket, packetCount;
    if (s->geomUploaded) {
        fill_bake_geometry(s, in, firstPacket, packetCount);
        in.packetAlpha = s->packetAlpha; in.alphaSet = s->alphaSet; in.alphaReady = s->alphaReady;
        in.alphaContentHash = mc2_color_prop_island_alpha_content_hash();
    }
    if (!in.alphaReady) {
        Mc2ColorPropMaterialFeed mf{};
        in.alphaAbsent = mc2_color_prop_island_get_material_feed(&mf) && mf.arrayKeyOn == 0;
    }
    if (s->bake.update(s->cbuf[s->parity], s->recordEpoch, in)) ++s->passCount;
}

// The start every pass shares: the island is up, the caster geometry is resident, the frame's recording is open and the casters are staged.

// The static-prop caster draws of a run list: one indirect multi-draw of the opaque packets and one of the alpha-tested ones (alpha pipeline and set 1).
static uint32_t draw_static_runs(ShadowIsland* s, VkCommandBuffer cb, const std::vector<InstRun>& runs) {
    static thread_local std::vector<mc2vk::ShadowDraw> draws;
    draws.clear();
    for (const InstRun& run : runs) {
        if (run.typeID >= s->typeRuns.size()) continue;
        const TypeRun& tr = s->typeRuns[run.typeID];
        for (uint32_t k = 0; k < tr.packetCount; ++k) {
            const uint32_t pk = tr.firstPacket + k;
            if (pk >= s->packets.size()) break;
            const Mc2ShadowPacket& pkt = s->packets[pk];
            const bool alphaPacket = s->alphaReady && pk < s->packetAlpha.size() && s->packetAlpha[pk].layer >= 0;
            mc2vk::ShadowDraw d{pkt.indexCount, pkt.firstIndex, pkt.baseVertex, run.firstInstance, run.count, -1, 0.0f, 0.0f};
            if (alphaPacket) { const ShadowIsland::PacketAlpha& pa = s->packetAlpha[pk]; d.layer = pa.layer; d.scaleX = pa.scaleX; d.scaleY = pa.scaleY; }
            draws.push_back(d);
        }
    }
    return s->drawRing.record(cb, s->playout.get(), s->pipe.get(), s->alphaPipe.get(), s->alphaSet, draws);
}

// Appends one depth-only render pass to the frame's command buffer: the static-prop casters, then the mech casters, into the shared depth map. The light-space matrix goes to
// this pass's own slice of the frame UBO, so no pass reads a matrix a later pass overwrote. Nothing here waits for the GPU.
static bool record_pass(ShadowIsland* s, int size, const float* lightSpace16, float biasFactor, float biasUnits, bool drawCasters = true) {
    if (s->passCount >= kMaxPassesPerFrame) { setFallback("too_many_passes"); return false; }
    const VkDeviceSize uboOffset = (static_cast<VkDeviceSize>(s->parity) * kMaxPassesPerFrame + s->passCount) * kUboSliceBytes;
    {
        ShadowFrameUBO u{};
        std::memcpy(u.lightSpaceMatrix, lightSpace16, sizeof(u.lightSpaceMatrix));
        u.instBase = 0;
        void* mapped = nullptr;
        if (vmaMapMemory(s->allocator, s->ubo.allocation(), &mapped) != VK_SUCCESS) { setFallback("ubo_map_failed"); s->disabled = true; return false; }
        std::memcpy(static_cast<uint8_t*>(mapped) + uboOffset, &u, sizeof(u));
        vmaFlushAllocation(s->allocator, s->ubo.allocation(), uboOffset, sizeof(u));
        vmaUnmapMemory(s->allocator, s->ubo.allocation());
    }
    const uint32_t dynamicOffset = static_cast<uint32_t>(uboOffset);
    VkCommandBuffer cb = s->cbuf[s->parity];

    // No CPU wait follows a submit, so the barriers order the depth image against its neighbours on the one queue. Before: this pass's depth writes come
    // after the previous frame's depth writes AND after its fragment-shader reads (terrain, the screen shadow, the debug overlay); the image arrives
    // UNDEFINED because the pass CLEARs it (forward-Z 1.0, GL glClearDepth(1.0)). After: those fragment shaders see this pass's depth writes.
    namespace dr = mc2vk::dynrender;
    mc2vk::VulkanContext& dctx = mc2vk::getOrCreateEngineVulkanContext();
    const VkImage depthImage = s->depthResource.image.get();
    dr::depthImageBarrier(dctx, cb, depthImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                          VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT,
                          VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                          VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    MC2_VK_GPU_SCOPE(gpuScope, cb, "VKIsl.Shadow.Dyn");
    dr::beginDepthOnly(dctx, cb, s->depthAttView.get(), s->depthFormat, static_cast<uint32_t>(size), static_cast<uint32_t>(size), VK_ATTACHMENT_LOAD_OP_CLEAR);

    // A NON-flipped viewport: a flipped one Y-mirrors the depth and streaks terrain (checked against the GL depth dump); the live consumer is
    // shaders/vulkan/screen_shadow_csm.frag.
    const VkViewport vp{0.0f, 0.0f, static_cast<float>(size), static_cast<float>(size), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, {static_cast<uint32_t>(size), static_cast<uint32_t>(size)}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &scissor);
    // GL's glPolygonOffset(factor, units) is depthBias(constant = units, slope = factor).
    vkCmdSetDepthBias(cb, biasUnits, 0.0f, biasFactor);

    mc2vk::infra::bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipe.get());
    const VkDescriptorSet set = s->set[s->parity];
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->playout.get(), 0, 1, &set, 1, &dynamicOffset);
    const VkDeviceSize vboOff = 0;
    const VkBuffer vertexBuf = s->vbo.get();
    vkCmdBindVertexBuffers(cb, 0, 1, &vertexBuf, &vboOff);
    vkCmdBindIndexBuffer(cb, s->ibo.get(), 0, VK_INDEX_TYPE_UINT32);
    if (drawCasters) {
    draw_static_runs(s, cb, s->instRuns);
    // Only the mechs inside the view-swept set can land in the fitted map.
    static std::vector<uint8_t> viewMask;
    s->mover.runMaskView(s->mech.runs(), viewMask);
    s->mech.record(cb, s->parity, dynamicOffset, viewMask.data());
    }
    dr::end(dctx, cb);
    dr::depthImageBarrier(dctx, cb, depthImage, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    gpuScope.end();
    ++s->passCount;
    ++g_health.usedVulkan;
    return true;
}

// ---- LOCAL-LIGHTS-2: local-light shadow tiles ----------------------------------------------------------------------------------------

// Writes one light-space matrix to the next free slice of the frame UBO; the slice counts as a pass of the frame.
static bool local_ubo_slice(ShadowIsland* s, const float* m16, uint32_t& dynOffset) {
    if (s->passCount >= kMaxPassesPerFrame) return false;
    const VkDeviceSize off = (static_cast<VkDeviceSize>(s->parity) * kMaxPassesPerFrame + s->passCount) * kUboSliceBytes;
    ShadowFrameUBO u{};
    std::memcpy(u.lightSpaceMatrix, m16, sizeof(u.lightSpaceMatrix));
    void* mapped = nullptr;
    if (vmaMapMemory(s->allocator, s->ubo.allocation(), &mapped) != VK_SUCCESS) return false;
    std::memcpy(static_cast<uint8_t*>(mapped) + off, &u, sizeof(u));
    vmaFlushAllocation(s->allocator, s->ubo.allocation(), off, sizeof(u));
    vmaUnmapMemory(s->allocator, s->ubo.allocation());
    dynOffset = static_cast<uint32_t>(off);
    ++s->passCount;
    return true;
}

static bool local_ensure_instances(ShadowIsland* s, uint32_t par, VkDeviceSize bytes) {
    auto& L = s->local;
    if (!L.inst[par] || bytes > L.instCap[par]) {
        L.inst[par].reset();
        const VkDeviceSize cap = bytes + bytes / 2 + 4096;
        if (!make_buffer(s, L.inst[par], cap, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, "shadow.localInst", static_cast<int>(par))) return false;
        L.instCap[par] = cap;
        VkDescriptorBufferInfo uboInfo{s->ubo.get(), 0, sizeof(ShadowFrameUBO)};
        VkDescriptorBufferInfo instInfo{L.inst[par].get(), 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w[2]{};
        w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstSet = L.set[par]; w[0].dstBinding = 0; w[0].descriptorCount = 1;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; w[0].pBufferInfo = &uboInfo;
        w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[1].dstSet = L.set[par]; w[1].dstBinding = 1; w[1].descriptorCount = 1;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[1].pBufferInfo = &instInfo;
        vkUpdateDescriptorSets(s->device, 2, w, 0, nullptr);
    }
    return true;
}

// The static-prop caster draws of one tile (the record_pass loop over a tile's own instance runs).
static void local_draw_static(ShadowIsland* s, VkCommandBuffer cb, const std::vector<InstRun>& runs, uint32_t dynOffset) {
    mc2vk::infra::bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipe.get());
    const VkDescriptorSet set = s->local.set[s->parity];
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->playout.get(), 0, 1, &set, 1, &dynOffset);
    const VkDeviceSize vboOff = 0;
    const VkBuffer vertexBuf = s->vbo.get();
    vkCmdBindVertexBuffers(cb, 0, 1, &vertexBuf, &vboOff);
    vkCmdBindIndexBuffer(cb, s->ibo.get(), 0, VK_INDEX_TYPE_UINT32);
    mc2::battlebench::add(mc2::battlebench::DrawShadowLocal, draw_static_runs(s, cb, runs));
}

static void local_barrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkAccessFlags srcAccess,
                          VkPipelineStageFlags dst, VkAccessFlags dstAccess) {
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = srcAccess; mb.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(cb, src, dst, 0, 1, &mb, 0, nullptr, 0, nullptr);
}

static void local_tile_viewport(VkCommandBuffer cb, uint32_t t) {
    using namespace mc2localshadow;
    const float x = static_cast<float>((t % kCols) * kTileSide), y = static_cast<float>((t / kCols) * kTileSide);
    const VkViewport vp{x, y, static_cast<float>(kTileSide), static_cast<float>(kTileSide), 0.0f, 1.0f};
    const VkRect2D sc{{static_cast<int32_t>(x), static_cast<int32_t>(y)}, {kTileSide, kTileSide}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdSetDepthBias(cb, 2.0f, 0.0f, 2.5f);
}

// Records this frame's local shadow tiles into the open frame: re-bakes the static casters of the tiles whose light changed, copies every
// used tile to the live atlas and draws the mechs on top. Static casters are culled to the light's range on the CPU, once per bake.
static void record_local_shadows(ShadowIsland* s) {
    using namespace mc2localshadow;
    auto& L = s->local;
    L.tilesDrawn = 0;
    if (!L.ok || !s->geomUploaded) return;
    VkCommandBuffer cb = s->cbuf[s->parity];
    const VkImageSubresourceRange depthRange{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    if (!L.layoutsReady) {
        // Both atlases come out of UNDEFINED: the trackers derive the source state of every transition from the previous use.
        mc2vk::VulkanContext& bctx = mc2vk::getOrCreateEngineVulkanContext();
        mc2vk::sync::ImageTracker staticTrk(reinterpret_cast<uint64_t>(L.staticAtlas.image.get()), VK_IMAGE_ASPECT_DEPTH_BIT);
        mc2vk::sync::ImageTracker liveTrk(reinterpret_cast<uint64_t>(L.liveAtlas.image.get()), VK_IMAGE_ASPECT_DEPTH_BIT);
        {
            mc2vk::sync::BarrierBatch batch;
            staticTrk.use(batch, {static_cast<uint32_t>(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL), VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, false});
            liveTrk.use(batch, {static_cast<uint32_t>(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL), VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, true});
            mc2vk::sync::flush(bctx, cb, batch);
        }
        const VkClearDepthStencilValue one{1.0f, 0};
        vkCmdClearDepthStencilImage(cb, L.liveAtlas.image.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &one, 1, &depthRange);
        {
            mc2vk::sync::BarrierBatch batch;
            liveTrk.use(batch, {static_cast<uint32_t>(VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL), VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT, false});
            mc2vk::sync::flush(bctx, cb, batch);
        }
        L.layoutsReady = true;
        ++s->passCount;   // the frame must be submitted for the transitions
    }
    L.ready = true;
    uint32_t tileCount = 0;
    const Tile* tiles = currentTiles(&tileCount);
    if (!tiles || tileCount == 0) return;
    ZoneScopedN("VKIsl.Shadow.LocalLights");

    const auto& casterSet = mc2shadow::staticCasterSet();
    // Static lamp tiles refresh once per submitted, drained caster/height burst.
    if (casterSet.version() != L.leavesVersion || s->bake.objectGeneration() != L.leavesGeneration) {
        casterSet.resolve(L.leaves, false);
        L.leavesVersion = casterSet.version();
        L.leavesGeneration = s->bake.objectGeneration();
        for (auto& k : L.bakedKey) k = 0;
    }
    uint32_t bake[kTiles], bakeCount = 0, used[kTiles], usedCount = 0;
    for (uint32_t t = 0; t < kTiles && t < tileCount; ++t) {
        if (!tiles[t].used) continue;
        used[usedCount++] = t;
        // LIGHTS-R4: a mover lamp's tile is never baked (its projection changes every frame).
        if (!tiles[t].mover && tiles[t].key != L.bakedKey[t]) bake[bakeCount++] = t;
    }
    if (usedCount == 0) return;
    if (bakeCount > 0) {
        std::vector<GpuStaticPropInstance> staged;
        std::vector<InstRun> runs[kTiles];
        std::vector<uint32_t> pick;
        for (uint32_t bi = 0; bi < bakeCount; ++bi) {
            const Tile& T = tiles[bake[bi]];
            pickTileCasters(L.leaves, T, pick);
            for (size_t i = 0; i < pick.size();) {
                const uint32_t tid = L.leaves[pick[i]].typeID;
                size_t j = i;
                while (j < pick.size() && L.leaves[pick[j]].typeID == tid) ++j;
                runs[bi].push_back({tid, static_cast<uint32_t>(staged.size()), static_cast<uint32_t>(j - i)});
                for (size_t k = i; k < j; ++k) staged.push_back(L.leaves[pick[k]]);
                i = j;
            }
        }
        const VkDeviceSize bytes = std::max<VkDeviceSize>(staged.size(), 1) * sizeof(GpuStaticPropInstance);
        if (!local_ensure_instances(s, s->parity, bytes)) { L.ok = false; return; }
        if (!staged.empty()) {
            void* mapped = nullptr;
            if (vmaMapMemory(s->allocator, s->local.inst[s->parity].allocation(), &mapped) != VK_SUCCESS) return;
            std::memcpy(mapped, staged.data(), staged.size() * sizeof(GpuStaticPropInstance));
            vmaFlushAllocation(s->allocator, s->local.inst[s->parity].allocation(), 0, VK_WHOLE_SIZE);
            vmaUnmapMemory(s->allocator, s->local.inst[s->parity].allocation());
        }
        // The previous frame's tile copies read the static atlas (TRANSFER): the bake's layout transition and writes wait for
        // them; the dst stages chain into the pass's external dependency.
        local_barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                      VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0);
        // The static atlas is TRANSFER_SRC between frames: the pass moves it to the attachment layout (the render pass's initial layout did that) and back.
        namespace dr = mc2vk::dynrender;
        mc2vk::VulkanContext& dctx = mc2vk::getOrCreateEngineVulkanContext();
        dr::depthImageBarrier(dctx, cb, L.staticAtlas.image.get(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                              VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                              VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT,
                              VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                              VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        MC2_VK_GPU_SCOPE(bakeScope, cb, "VKIsl.Shadow.LocalBake");
        dr::beginDepthOnly(dctx, cb, L.staticAtlas.view.get(), s->depthFormat, kAtlasW, kAtlasH, VK_ATTACHMENT_LOAD_OP_LOAD);
        for (uint32_t bi = 0; bi < bakeCount; ++bi) {
            const uint32_t t = bake[bi];
            float m[16];
            lightMatrix(tiles[t], m);
            uint32_t dyn = 0;
            if (!local_ubo_slice(s, m, dyn)) break;
            local_tile_viewport(cb, t);
            VkClearAttachment ca{VK_IMAGE_ASPECT_DEPTH_BIT, 0, {}};
            ca.clearValue.depthStencil = {1.0f, 0};
            const VkClearRect cr{{{static_cast<int32_t>((t % kCols) * kTileSide), static_cast<int32_t>((t / kCols) * kTileSide)}, {kTileSide, kTileSide}}, 0, 1};
            vkCmdClearAttachments(cb, 1, &ca, 1, &cr);
            local_draw_static(s, cb, runs[bi], dyn);
            L.bakedKey[t] = tiles[t].key;
            ++L.bakesTotal;
            s->bake.countLocalTileRebakes(1);
        }
        dr::end(dctx, cb);
        dr::depthImageBarrier(dctx, cb, L.staticAtlas.image.get(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                              VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        bakeScope.end();
        // The copy below reads what the bake wrote (the pass's exit dependency ends at FRAGMENT_SHADER).
        local_barrier(cb, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    }

    // Live atlas: the baked static tiles, then this frame's mechs.
    VkImageMemoryBarrier toDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toDst.srcQueueFamilyIndex = toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.image = L.liveAtlas.image.get(); toDst.subresourceRange = depthRange;
    toDst.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL; toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.srcAccessMask = 0; toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toDst);
    VkImageCopy regions[kTiles]{};
    uint32_t copyCount = 0;
    for (uint32_t i = 0; i < usedCount; ++i) {
        const uint32_t t = used[i];
        if (tiles[t].mover) continue;   // cleared in the live pass instead
        VkImageCopy& r = regions[copyCount++];
        r.srcSubresource = r.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
        r.srcOffset = r.dstOffset = {static_cast<int32_t>((t % kCols) * kTileSide), static_cast<int32_t>((t / kCols) * kTileSide), 0};
        r.extent = {kTileSide, kTileSide, 1};
    }
    if (copyCount > 0)
        vkCmdCopyImage(cb, L.staticAtlas.image.get(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       L.liveAtlas.image.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, copyCount, regions);
    // The mech pass loads the copied tiles.
    local_barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    // The live atlas arrives TRANSFER_DST (the tile copies) and leaves DEPTH_STENCIL_READ_ONLY for the pool receivers: the pass's layout moves.
    namespace dr = mc2vk::dynrender;
    mc2vk::VulkanContext& dctx = mc2vk::getOrCreateEngineVulkanContext();
    dr::depthImageBarrier(dctx, cb, L.liveAtlas.image.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                          VK_PIPELINE_STAGE_2_TRANSFER_BIT,   // the earlier fragment-shader reads are ordered by the toDst barrier above (src FRAGMENT_SHADER -> TRANSFER); only the copy writes remain
                          VK_ACCESS_2_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                          VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    MC2_VK_GPU_SCOPE(liveScope, cb, "VKIsl.Shadow.LocalLive");
    dr::beginDepthOnly(dctx, cb, L.liveAtlas.view.get(), s->depthFormat, kAtlasW, kAtlasH, VK_ATTACHMENT_LOAD_OP_LOAD);
    for (uint32_t i = 0; i < usedCount; ++i) {
        const uint32_t t = used[i];
        if (!tiles[t].mover) continue;
        VkClearAttachment ca{VK_IMAGE_ASPECT_DEPTH_BIT, 0, {}};
        ca.clearValue.depthStencil = {1.0f, 0};
        const VkClearRect cr{{{static_cast<int32_t>((t % kCols) * kTileSide), static_cast<int32_t>((t / kCols) * kTileSide)}, {kTileSide, kTileSide}}, 0, 1};
        vkCmdClearAttachments(cb, 1, &ca, 1, &cr);
    }
    if (s->mech.hasCasters()) {
        bool mechPipeBound = false;   // one pass, one pipeline: the tiles below share the bind
        for (uint32_t i = 0; i < usedCount; ++i) {
            const uint32_t t = used[i];
            float m[16];
            lightMatrix(tiles[t], m);
            uint32_t dyn = 0;
            if (!local_ubo_slice(s, m, dyn)) break;
            // Only the mechs inside this light's reach can cast into its tile.
            static std::vector<uint8_t> tileMask;
            s->mover.runMaskLight(s->mech.runs(), tiles[t].pos, tiles[t].farDistance, tileMask);
            local_tile_viewport(cb, t);
            s->mech.record(cb, s->parity, dyn, tileMask.data(), true, &mechPipeBound);
        }
    }
    dr::end(dctx, cb);
    dr::depthImageBarrier(dctx, cb, L.liveAtlas.image.get(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    liveScope.end();
    L.tilesDrawn = usedCount;
    ++s->passCount;
    static uint64_t frames = 0;
    if (++frames % 600 == 1 && mc2::gate::get<"MC2_DIAG_VERBOSE">())
        vlog("[LOCAL_SHADOW v1] tiles=%u baked_now=%u bakes_total=%llu static_leaves=%zu mech_casters=%d",
             usedCount, bakeCount, static_cast<unsigned long long>(L.bakesTotal), L.leaves.size(), s->mech.hasCasters() ? 1 : 0);
    if (bakeCount > 0 && false)
        for (uint32_t i = 0; i < usedCount; ++i) {
            const Tile& T = tiles[used[i]];
            float aim[3];
            decodeAim(T.packedType, aim);
            vlog("[LOCAL_SHADOW v1] tile=%u pos=(%.1f,%.1f,%.1f) aim=(%.2f,%.2f,%.2f) far=%.0f tanHalf=%.2f",
                 used[i], T.pos[0], T.pos[1], T.pos[2], aim[0], aim[1], aim[2], T.farDistance, tanHalf(T.packedType));
        }
}

static bool ensure_island(int size) {
    // A reduced map remains valid for the requested tier; pressure must not cause per-frame rebuilds.
    if (const ShadowIsland* parked=parkedIsland(); parked && parked->size<=size) return true;
    size=static_cast<int>(mc2vk::infra::shadowTier(static_cast<uint32_t>(size),2));
    if(!size) { setFallback("shadow_residency_budget"); return false; }
    // Lazy init / rebuild on size change.
    mc2vk::VulkanContext& slotCtx = mc2vk::getOrCreateEngineVulkanContext();
    if (const ShadowIsland* parked = parkedIsland(); parked && parked->size != size) slotCtx.retireIsland(mc2vk::IslandSlot::Shadow);
    if (!islandSlot()) {
        auto fresh = std::make_unique<ShadowIsland>();
        (void)mc2vk::takeLastCreateFailure();
        if (!init_island(fresh.get(), size)) {
            slotCtx.waitIdle("shadow_init_failure");   // objects made before the failure may have been submitted
            fresh.reset();
            mc2vk::onIslandInitFailure(mc2vk::IslandKind::Shadow, g_shadowInitReachedBuild != 0,
                                       mc2vk::takeLastCreateFailure(), g_health.fallbackReason);
            return false;
        }
        mc2vk::islandInitFailures(mc2vk::IslandKind::Shadow).reset();
        islandSlot() = std::move(fresh);
        mc2vk::bumpDescEpoch();   // new depth/dummy views and sampler may reuse retired handle values
    }
    return true;
}

extern "C" int mc2_vk_shadow_island_prewarm(int,int) {
    // No object shadows on Low/Potato: do not duplicate the entire prop VBO/IBO
    // (214 MiB on mc2_10) or prewarm unused bake products in the 512 MiB budget.
    if (mc2::options::lowCost()) return 1;
    if (![&] { MC2_LOAD_SCOPE("shadows.init"); return ensure_island(mc2shadow::shadowQuality().mapSide); }()) return 0;
    auto* s=parkedIsland();
    { MC2_LOAD_SCOPE("shadows.geometry"); upload_geometry_once(s); }
    { MC2_LOAD_SCOPE("shadows.alpha"); refresh_alpha_casters(s); }
    if (![&] { MC2_LOAD_SCOPE("shadows.products"); return s->bake.prewarmProducts(); }()) return 0;
    if (!s->geomUploaded) return 1;
    std::vector<uint32_t> fp, pc; mc2vk::ShadowBakeInputs in;
    { MC2_LOAD_SCOPE("shadows.inputs"); fill_bake_geometry(s, in, fp, pc); }
    MC2_LOAD_SCOPE("shadows.bounds");
    return s->bake.prewarmTypeBounds(in) ? 1 : 0;
}

bool mc2vk::bindMoverShadowDescriptors(VkDevice device, VkDescriptorSet set, DescWriteStamp* stamp24) {
    ShadowIsland* s=parkedIsland();
    if (!s) {
        if (!ensure_island(1)) return false; // inactive descriptors need no full-size map
        s=parkedIsland();
    }
    const bool ready=mover_shadows_enabled() && productReady(g_dynShadowReadyThisFrame);
    return s->mover.bind(device,set,s->depthResource.view.get(),ready,stamp24);
}

extern "C" void mc2_vk_shadow_island_run_movers(const float cameraRows[16], const float sun[3],
                                                const void* instances, unsigned int instanceCount) {
    g_dynShadowReadyThisFrame=false;
    ++g_shadowFrameEpoch;
    if (!cameraRows || !sun || !mover_shadows_enabled() || !ensure_island(mc2shadow::shadowQuality().mapSide)) return;
    ShadowIsland* s=parkedIsland();
    if (!s || s->disabled) return;
    if (!upload_geometry_once(s)) return;
    { ZoneScopedN("DSP.BeginFrame"); if (!begin_frame(s,g_shadowFrameEpoch)) return; }
    { ZoneScopedN("DSP.AlphaCasters"); refresh_alpha_casters(s); }
    { ZoneScopedN("DSP.MechStage"); s->mech.stage(g_shadowFrameEpoch); s->mech.recordSkin(s->cbuf[s->parity]); }   // compute pre-skin ahead of every caster pass (no render pass open)
    const auto light=mc2shadow::clampSunAltitude({sun[0],sun[1],sun[2]},mc2ShadowMinSunAltDeg());
    { ZoneScopedN("DSP.MechActors"); s->mover.refreshMechActors(cameraRows,light); }   // mech spheres for the per-map run masks below and for prepare()
    { ZoneScopedN("DSP.RecordBake"); record_bake(s); }
    { ZoneScopedN("DSP.LocalShadows"); record_local_shadows(s); }   // LOCAL-LIGHTS-2
    const auto start=std::chrono::steady_clock::now();
    const auto props=std::span(static_cast<const GpuStaticPropInstance*>(instances),instanceCount);
    { ZoneScopedN("DSP.MoverPrepare");
    if (!s->mover.prepare(s->size,cameraRows,light,props,s->bake.ready())) return;
    }
    const auto& selected=s->mover.props();
    { ZoneScopedN("DSP.StageInstances"); if (!stage_instances(s,selected.data(),static_cast<uint32_t>(selected.size()))) return; }
    const auto& frame=s->mover.frame();
    { ZoneScopedN("DSP.RecordPass"); if (!record_pass(s,s->size,frame.matrix,1.0f,2.0f,frame.flags[0]!=0)) return; }
    g_dynShadowReadyThisFrame=true;
    static uint64_t frames=0;
    if (++frames%600==1) {
        const double us=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count();
        vlog("movers actors_feed=%u actors_visible=%u rigid_leaves=%zu map=%d format=D16 texel_wu=%.3f depth_span=%.1f cpu_fit_and_record_us=%.1f resident_mb=%.1f coverage_kb=8",
             s->mover.actorCount(),s->mover.visibleActorCount(),selected.size(),s->size,frame.params[0],frame.params[1]>0?1.0f/frame.params[1]:0.0f,us,static_cast<double>(s->size)*s->size*2/1048576.0);
    }
}

// The dynamic (single) shadow map, and the first pass of every frame: it counts the frame's epoch even when it declines. Inputs, all from the GL side at
// dynamic-shadow-pass time:
//   size          -- square shadow map side (getDynamicShadowMapSize()).
//   lightSpace16  -- the 16 raw floats GL uploads (GL_FALSE) for lightSpaceMatrix.
//   instances     -- the CPU GpuStaticPropInstance vector (112B each).
//   instanceCount -- caster instance count.
//   biasFactor/biasUnits -- glPolygonOffset(shadowBiasFactor_, shadowBiasUnits_).
// The pass is only RECORDED here; the frame is submitted by mc2_vk_shadow_island_submit_frame(). Always returns 0: the consumers read the product through the
// view accessors.

// The catch-all (full-map) cascade: the last cascade matrix into a separate D32 image the terrain island binds directly.

// One near cascade layer of the VK D32 array the terrain island binds at binding 8. The array is ready once every layer of the frame has been recorded.


// Ends the frame's recording and submits it, once. Called after the last shadow pass of the frame and before any consumer reads a product. No fence wait
// follows: the render pass dependencies order the consumers behind the writes, and the parity's fence retires the buffers two frames later.
extern "C" void mc2_vk_shadow_island_submit_frame() {
    ShadowIsland* s = parkedIsland();
    if (!s) return;
    if (!s->recording) {
        ++g_shadowFrameEpoch;
        if (s->disabled || !s->inited || !s->bake.wantsFrame()) return;
        if (!begin_frame(s,g_shadowFrameEpoch)) return;
        record_bake(s);
    }
    VkCommandBuffer cb=s->cbuf[s->parity];
    s->recording=false;
    vk_render_pass_timer::CancelUnlessSubmitted timerGuard(gos_render_pass_timer::Pass_ShadowDyn);
    if (s->passCount==0) return;
    auto disableFrame=[&]() { s->disabled=true; g_dynShadowReadyThisFrame=false; };
    vk_render_pass_timer::End(cb,gos_render_pass_timer::Pass_ShadowDyn);
    if (vkEndCommandBuffer(cb) != VK_SUCCESS) { setFallback("record_failed"); disableFrame(); return; }
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    ZoneScopedN("VKIsl.Shadow.Submit");
    mc2vk::VulkanContext& sctx = mc2vk::getOrCreateEngineVulkanContext();
    if (sctx.submit(si, VK_NULL_HANDLE) != VK_SUCCESS) { setFallback("submit_failed"); disableFrame(); return; }
    s->frameTicket[s->parity] = sctx.tracker().now();
    timerGuard.submitted();
    s->bake.markSubmitted(s->recordEpoch);
    sctx.kick();   // the CSM pass starts on the GPU now, while the CPU records the scene
}

// F1 (hitch): run the static bake (cold tiles, or the warm cache upload plus the object map) behind the load screen instead of in the first mission frames,
// where its ~48 ms of GPU work became a present slot wait. Steps the same per-frame path the frames use (each step waits its parity fence) until the bake
// is retired, or nothing is due for a few steps (inputs not ready: the frames bake as before), or 4 s pass.
extern "C" int mc2_vk_shadow_island_load_bake(int, int) {
    ShadowIsland* s = parkedIsland();
    if (!s || s->disabled || !s->inited || mc2::options::lowCost()) return 1;
    ZoneScopedN("VKIsl.Shadow.LoadBake");
    const auto t0 = std::chrono::steady_clock::now();
    const auto deadline = t0 + std::chrono::seconds(4);
    // The family prewarm bound the alpha casters before textures().finishLoading(), when the alpha-ON array was not resident yet; bind it now,
    // or the bake waits for alpha (kMaxAlphaWaitFrames) until the first in-mission frame refreshes it.
    refresh_alpha_casters(s);
    int steps = 0, idle = 0, submits = 0;
    while (s->bake.wantsFrame() && idle < 2 * static_cast<int>(mc2vk::kMaxFramesInFlight) + 2 && steps < 1024
           && std::chrono::steady_clock::now() < deadline) {
        const bool before = s->bake.inFlight();
        mc2_vk_shadow_island_submit_frame();
        s->bake.waitForCacheLoad(deadline);
        ++steps;
        if (s->disabled) break;
        const bool busy = s->bake.inFlight();
        submits += (busy && !before) ? 1 : 0;
        idle = busy ? 0 : idle + 1;
    }
    // Both parity fences signalled: the last submit retired and its products are final.
    for (auto& t : s->frameTicket) mc2vk::getOrCreateEngineVulkanContext().wait(t, 2000000000ull, "shadow load bake retire");
    void* av[mc2::prop_buckets::kMaxBuckets]{}; uint32_t ag = 0; Mc2ColorPropMaterialFeed mf{};
    const int arr = mc2_color_prop_island_get_alpha_arrays(av, &ag);
    const int feed = mc2_color_prop_island_get_material_feed(&mf);
    if (mc2_diag::tagRequested("HITCH"))   // opt-in with the other hitch diagnostics (MC2_DIAG_TAGS=HITCH)
    std::fprintf(stderr, "[SHADOW_BAKE] load_bake steps=%d submits=%d ready=%d ms=%.1f alpha_array=%d feed=%d bucketed=%d key_on=%d layers=%d alpha_ready=%d\n",
                 steps, submits, s->bake.ready() ? 1 : 0,
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), arr, feed, mf.bucketed ? 1 : 0,
                 mf.arrayKeyOn != 0 ? 1 : 0, mf.packetAlbedoLayer ? 1 : 0, s->alphaReady ? 1 : 0);
    return 1;
}

// VK-SHADOW-DIRECT-CONSUME-1: raw VK-side view/ready accessors for the terrain
// island. g_dynShadowReadyThisFrame is set true at the tail of a successful
// mc2_vk_shadow_island_run() (recorded, and usable once the frame is submitted: the
// depth image content + layout transition to DEPTH_STENCIL_READ_ONLY_OPTIMAL are
// already complete) and reset false at the top of every call (including a failed/
// declined one), so a stale prior-frame view is never reported ready this frame.


extern "C" int mc2_vk_shadow_island_movers_ready() {
    return productReady(g_dynShadowReadyThisFrame) ? 1 : 0;
}

// LOCAL-LIGHTS-2: the producer may assign shadow tiles only while the island renders them.
extern "C" int mc2_vk_local_shadows_capable() {
    const ShadowIsland* s = parkedIsland();
    return (s && !s->disabled && s->local.ok && mover_shadows_enabled()) ? 1 : 0;
}

// The live local-light atlas (DEPTH_STENCIL_READ_ONLY), or null before its first-use transitions were recorded.
extern "C" void* mc2_vk_shadow_island_get_local_atlas_view() {
    const ShadowIsland* s = parkedIsland();
    return (s && s->local.ok && s->local.ready) ? reinterpret_cast<void*>(s->local.liveAtlas.view.get()) : nullptr;
}





// The static sun shadow map of the last submitted build, in DEPTH_STENCIL_READ_ONLY, and the engine generation it was built from; null / 0 before the
// first build of the map. A consumer pairs the view with the light matrix of the same generation.
// With the bake on, the static map is the bake's D16 object map (terrain, buildings, trees and props with the same static light matrix).
extern "C" void* mc2_vk_shadow_island_get_object_view() {
    const auto* s=parkedIsland(); return s?reinterpret_cast<void*>(s->bake.objectView()):nullptr;
}

extern "C" int mc2_vk_shadow_island_get_object_matrix(float* matrix16) {
    const auto* s=parkedIsland();
    if (!s || !matrix16 || !s->bake.objectView()) return 0;
    std::memcpy(matrix16,s->bake.objectMatrix(),16*sizeof(float));
    return 1;
}
extern "C" int mc2_vk_shadow_island_bake_ready() {
    const ShadowIsland* s = parkedIsland();
    return s && s->bake.ready() ? 1 : 0;
}
extern "C" void* mc2_vk_shadow_island_get_bake_mask_view() {
    const ShadowIsland* s = parkedIsland();
    return s ? reinterpret_cast<void*>(s->bake.maskView()) : nullptr;
}


extern "C" void* mc2_vk_shadow_island_get_mover_debug_view() {
    return productReady(g_dynShadowReadyThisFrame)?reinterpret_cast<void*>(parkedIsland()->debugArrayView.get()):nullptr;
}



// MC2_VK_SHADOW_MATRIX_DIAG public entries.
// Matrix qualifiers (all verified by grep: NO row_major/column_major anywhere in the shadow-related VK
// shaders): shadow_prop.vert ShadowFrameUBO std140 default (column-major) mat4 lightSpaceMatrix fed RAW
// (same as GL's glUniformMatrix4fv GL_FALSE); terrain_tier1.frag UBO dynamicLightSpaceMatrix /
// csmCascadeMatrices[3] std140 default column-major, fed RAW memcpy of the same GL arrays.




// Health accessor (mirrors mc2_render_backend_water_region_health).
extern "C" void mc2_render_backend_shadow_region_health(
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

// Static-prop feed invalidated (map unload/load): drop VK-owned caster geometry so the
// next map re-uploads instead of drawing stale/freed vertex data.
extern "C" void mc2_vk_shadow_island_invalidate_geometry() {
    ShadowIsland* parked = parkedIsland();
    if (!parked) return;
    release_geometry(parked);
    // The bake's products may still be read by a frame in flight; a map boundary idles once rather than ever on a rebake.
    if (parked->bake.hasProducts()) mc2vk::getOrCreateEngineVulkanContext().waitIdle("shadow_bake_release");
    parked->bake.invalidate();
    vlog("caster geometry invalidated (map boundary).");
}

// Teardown (called from gosPostProcess::destroy or renderer shutdown).
extern "C" void mc2_vk_shadow_island_destroy() {
    vlog("[VK_SHADOW_INTEROP] vulkan_available=%d used_vulkan=%lu used_interop=%lu interop_active=%d "
         "vk2gl_sem=%d direct_chain=%d validation_errors=%lu inst_records=%lu type_runs=%lu geom_verts=%lu "
         "fallback_reason=\"%s\" vk_draws=%lu forbidden_copyImgToBuf=%lu forbidden_getTexImage=%lu "
         "forbidden_readPixels=%lu stage_inst_us=%.1f draw_us=%.1f total_us=%.1f",
         g_health.vulkanAvailable, g_health.usedVulkan, g_health.usedInterop, g_health.interopActive,
         g_health.vk2glSemActive, g_health.directChain, g_health.validationErrors, g_health.instRecords, g_health.typeRuns,
         g_health.geomVerts, g_health.fallbackReason, g_health.vkDraws,
         g_forbidden.vkCmdCopyImageToBuffer, g_forbidden.glGetTexImage, g_forbidden.glReadPixels,
         g_health.stageInstUs, g_health.drawUs, g_health.totalUs);
    mc2vk::getOrCreateEngineVulkanContext().retireIsland(mc2vk::IslandSlot::Shadow);
}

