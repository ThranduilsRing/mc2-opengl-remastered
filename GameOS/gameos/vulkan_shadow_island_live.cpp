// One D16 mover map and the readiness-gated static bake, on the shared Vulkan device.
#ifdef MC2_VULKAN_ISLAND

#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <volk.h>
#include "Gates/gate.h"
#include "vk_render_pass_timer.h"
#include "vk_gpu_profile.h"   // VK-GPU-ZONES: MC2_VK_GPU_SCOPE
#include "Gates/gate_cold.h"   // str<>()

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include <vk_mem_alloc.h>

#include "../../RenderCore/gpu_abi/GpuStaticPropInstance.h"  // single-source 112B instance record
#include "vk_image_resource.h"
#include "vk_island_util.h"
#include "vk_owned.h"
#include "vk_one_shot.h"
#include "vulkan_context.h"
#include "vulkan_shadow_island_live.h"
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

// ---------------------------------------------------------------------------
// Health snapshot (mirrors the water island's) — read by debug_state_dump.cpp.
// ---------------------------------------------------------------------------
float mc2ShadowMinSunAltDeg();

namespace {

struct ShadowIslandHealth {
    int   vulkanAvailable = 0;
    int   buildEnabled    = 1;   // this TU only compiles when the island is built
    char  deviceName[256] = {0};
    unsigned long validationErrors = 0;
    unsigned long attempted   = 0;
    unsigned long usedVulkan  = 0;
    char  fallbackReason[256] = {0};
    unsigned long vkDraws     = 0;
    int   interopActive  = 0;
    int   vk2glSemActive = 0;
    int   directChain    = 0;
    unsigned long usedInterop = 0;
    unsigned long instRecords = 0;   // caster instances fed this frame
    unsigned long typeRuns    = 0;   // per-type instanced draws this frame
    unsigned long geomVerts   = 0;   // shared VBO verts copied once
    double stageInstUs = 0.0, drawUs = 0.0, totalUs = 0.0;
};
ShadowIslandHealth g_health;
// VK-SHADOW-DIRECT-CONSUME-1: true only between a successful mc2_vk_shadow_island_run()
// (fence-waited, layout-transitioned) and the next call to mc2_vk_shadow_island_run()
// (which resets it before doing any work) -- so a same-frame-later VK consumer (the
// terrain island) can tell "is this frame's depth ready right now" without racing a
// stale prior-frame flag into a later frame where the shadow island declined.
bool g_dynShadowReadyThisFrame = false;
// One value per frame (bumped by the first pass, the dynamic map): the mech casters stage their feed once per epoch and every later pass reuses it.
uint64_t g_shadowFrameEpoch = 0;

void setFallback(const char* reason) {
    std::strncpy(g_health.fallbackReason, reason ? reason : "",
                 sizeof(g_health.fallbackReason) - 1);
    g_health.fallbackReason[sizeof(g_health.fallbackReason) - 1] = '\0';
}

// Forbidden DEPTH-readback counters (zero-copy invariant). VK PRODUCES the shadow
// map; GL SAMPLES it in place. Nothing should ever pull the shadow-map bytes back
// to the CPU or copy them across a staging buffer. Any of these firing on the
// interop path means the zero-copy invariant was violated -> fail the frame.
struct InteropForbidden {
    unsigned long vkCmdCopyImageToBuffer = 0;  // shadow-map readback   (must be 0)
    unsigned long glGetTexImage          = 0;  // shadow-map CPU pull   (must be 0)
    unsigned long glReadPixels           = 0;  // shadow-map CPU pull   (must be 0)
};
InteropForbidden g_forbidden;

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
    VkDeviceSize  instCapBytes[2] = {0, 0};   // per frame parity

    // Per-type packets + type runs (copied once per map).
    std::vector<Mc2ShadowPacket> packets;  // firstIndex/indexCount/baseVertex per packet
    std::vector<TypeRun>         typeRuns;  // firstPacket/packetCount per typeID (indexed by typeID)

    VkDescriptorSet   set[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};   // one per frame parity, allocated from `pool`, freed with it
    VkCommandBuffer   cbuf[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};   // the frame's command buffer, one per parity, allocated from `cpool`, freed with it

    // The frame's recording. The first pass of an epoch (one per frame) begins the command buffer of parity `epoch & 1`; every pass of the frame appends a
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
    // Foliage and fence casters. Per packet of the shared geometry: the layer of the colour island's alpha-ON array that the packet's colour-pass fragments sample,
    // and the UV sub-region scale (layer < 0: an opaque packet). Built once per map from the colour island's material feed; the array view is re-bound whenever the
    // colour island hands out a different one. Until both exist a frame draws those packets opaque (a solid shadow), as the first frames of a map always did.
    using PacketAlpha = mc2vk::CasterPacketAlpha;
    std::vector<PacketAlpha> packetAlpha;
    bool        alphaTableBuilt = false;
    VkImageView alphaBoundView = VK_NULL_HANDLE;
    uint32_t    alphaBoundGen = 0;   // the colour island's map generation the set was written for
    bool        alphaReady = false;   // this frame: the array is bound and the table is built
    // The single dynamic map is only the fallback of a receiver that has no cascades, so its pass is held until the frame's cascades are known: it is recorded
    // at the submit when the cascade array and the full-map cascade were not both produced, and never otherwise.

    // OWNED objects, declared in creation order: the destructor releases them in reverse, so a framebuffer goes before the view and render pass it uses.
    mc2vk::OwnedBuffer vbo;   // VK-owned copies of the shared static-prop VBO/IBO (P8a (B))
    mc2vk::OwnedBuffer ibo;
    mc2vk::OwnedBuffer instBuf[2];   // per-parity instance SSBO (memcpy of the GpuStaticPropInstance vector)
    mc2vk::OwnedBuffer ubo;          // kMaxPassesPerFrame slices per parity, one light-space matrix each
    mc2vk::OwnedImageView   depthAttView;   // depth-attachment view of the shared image
    mc2vk::OwnedRenderPass  rpass;
    mc2vk::OwnedFramebuffer fb;
    mc2vk::OwnedFence       frameFence[2];   // signalled when the submit of parity p has retired; created signalled
    mc2vk::OwnedDescriptorSetLayout dsl;
    mc2vk::OwnedDescriptorSetLayout alphaDsl;   // set 1 of the caster layout: the colour island's alpha-ON albedo array
    mc2vk::OwnedDescriptorPool      pool;
    mc2vk::OwnedDescriptorPool      alphaPool;
    VkDescriptorSet                 alphaSet = VK_NULL_HANDLE;   // allocated from alphaPool, freed with it
    mc2vk::OwnedSampler             alphaSampler;
    mc2vk::OwnedPipelineLayout      playout;
    mc2vk::OwnedShaderModule        vert;
    mc2vk::OwnedShaderModule        frag;
    mc2vk::OwnedShaderModule        alphaFrag;
    mc2vk::OwnedPipeline            pipe;
    mc2vk::OwnedPipeline            alphaPipe;   // foliage and fence casters: discards the texels the colour pass discards
    mc2vk::OwnedCommandPool         cpool;
    // The mech casters share the render pass and the UBO above, the static map the render pass and the geometry; declared last so they are released first.
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
        mc2vk::OwnedRenderPass bakePass, livePass;
        mc2vk::OwnedFramebuffer bakeFb, liveFb;
        mc2vk::OwnedBuffer inst[2];
        VkDeviceSize instCap[2] = {0, 0};
        mc2vk::OwnedDescriptorPool pool;
        VkDescriptorSet set[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
        uint64_t bakedKey[mc2localshadow::kTiles] = {};
        std::vector<GpuStaticPropInstance> leaves;   // the resolved static caster set
        uint64_t leavesVersion = ~0ull, leavesDelta = ~0ull;
        uint32_t tilesDrawn = 0;
        uint64_t bakesTotal = 0;
    } local;

    ShadowIsland() = default;
    // The views and framebuffers over the two image resources go first; the resources hold their images with raw allocations and are released here.
    ~ShadowIsland() override {
        debugArrayView.reset();
        fb.reset(); depthAttView.reset();
        local.bakeFb.reset(); local.liveFb.reset();
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

// VK-RUNG8 item-2 (boot-level native-or-decline), mirrors the mech/terrain islands: a
// PERMANENT init failure (spv/pipeline/renderpass/buffer creation past the context +
// gate readiness gates) declines the shadow island for the SESSION with ONE receipt line
// (GL owns the dynamic shadow pass) instead of re-newing + re-init'ing every frame. A
// TRANSIENT readiness gate (VK context not born / gate-off / geometry not finalized) keeps
// the per-frame fail-soft-retry. g_shadowInitReachedBuild flips to 1 once init passes the
// context + gate readiness gates (i.e. any later failure is a permanent build failure).
bool g_shadowSessionDeclined  = false;
int  g_shadowInitReachedBuild = 0;

bool make_buffer(ShadowIsland* s, mc2vk::OwnedBuffer& out, VkDeviceSize size, VkBufferUsageFlags usage,
                 bool hostVisible, std::string_view name, int i0 = -1, int i1 = -1) {
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size ? size : 16; bci.usage = usage;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = hostVisible ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT : 0;
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
constexpr VkPushConstantRange kCasterPushRange{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};

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
    if (!mc2vk::createDepthCasterPipeline(out, s->device, s->rpass.get(), s->playout.get(), s->vert.get(), frag, vib, via)) {
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
    // The subpass dependencies must equal the caster pipelines' pass (render-pass compatibility), so the transfer
    // hazards are ordered by explicit barriers in record_local_shadows that chain into these dependencies.
    const auto makePass = [&](mc2vk::OwnedRenderPass& out, VkImageLayout initial, VkImageLayout final) {
        const VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        const VkAccessFlags srcAccess = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        const VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        const VkAccessFlags dstAccess = VK_ACCESS_SHADER_READ_BIT;
        VkAttachmentDescription att{};
        att.format = s->depthFormat; att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = initial; att.finalLayout = final;
        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{}; sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sub.pDepthStencilAttachment = &ref;
        const VkPipelineStageFlags testStages = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        VkSubpassDependency deps[2]{
            {VK_SUBPASS_EXTERNAL, 0, srcStage, testStages, srcAccess,
             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, 0},
            {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, dstStage,
             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, dstAccess, 0}};
        VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rpci.attachmentCount = 1; rpci.pAttachments = &att; rpci.subpassCount = 1; rpci.pSubpasses = &sub;
        rpci.dependencyCount = 2; rpci.pDependencies = deps;
        return mc2vk::create(out, s->device, [&](auto* h) { return vkCreateRenderPass(s->device, &rpci, nullptr, h); });
    };
    const auto makeFb = [&](mc2vk::OwnedFramebuffer& out, VkRenderPass pass, VkImageView view) {
        VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fci.renderPass = pass; fci.attachmentCount = 1; fci.pAttachments = &view;
        fci.width = kAtlasW; fci.height = kAtlasH; fci.layers = 1;
        return mc2vk::create(out, s->device, [&](auto* h) { return vkCreateFramebuffer(s->device, &fci, nullptr, h); });
    };
    if (!makePass(L.bakePass, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) ||
        !makePass(L.livePass, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL) ||
        !makeFb(L.bakeFb, L.bakePass.get(), L.staticAtlas.view.get()) ||
        !makeFb(L.liveFb, L.livePass.get(), L.liveAtlas.view.get())) {
        vlog("local light shadow passes unavailable."); return false;
    }
    VkDescriptorPoolSize sizes[2]{{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 2; pci.poolSizeCount = 2; pci.pPoolSizes = sizes;
    if (!mc2vk::create(L.pool, s->device, [&](auto* h) { return vkCreateDescriptorPool(s->device, &pci, nullptr, h); })) return false;
    const VkDescriptorSetLayout layouts[2] = {s->dsl.get(), s->dsl.get()};
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = L.pool.get(); dsai.descriptorSetCount = 2; dsai.pSetLayouts = layouts;
    if (vkAllocateDescriptorSets(s->device, &dsai, L.set) != VK_SUCCESS) return false;
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
    if (!make_buffer(s, s->ubo, 2 * kMaxPassesPerFrame * kUboSliceBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true, "shadow.ubo")) return false;

    // ---- Depth-only render pass (single depth attachment; CLEAR to 1.0 forward-Z,
    // STORE). Interop layout: GL never writes this image, VK is sole producer; we
    // acquire in UNDEFINED (contents don't survive — we CLEAR every frame) and leave
    // in DEPTH_STENCIL_READ_ONLY so GL samples it. External deps serialize the
    // cross-frame WAW (the vk2gl/GL semaphore round-trip functionally serializes it,
    // but sync-val can't see across the GL boundary — the water WAW-fix pattern). ----
    {
        VkAttachmentDescription att{};
        att.format = s->depthFormat; att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        // Cleared every frame -> prior contents irrelevant -> UNDEFINED initial (also
        // avoids a layout-transition write across the GL sampling that just happened).
        att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout   = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL; // GL samples next
        VkAttachmentReference depthRef{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 0;
        sub.pDepthStencilAttachment = &depthRef;

        // No CPU wait follows a submit, so the render pass itself orders the depth image against its neighbours on the one queue. deps[0]: this pass's depth
        // writes come after the previous frame's depth writes AND after its fragment-shader reads (terrain, the screen shadow, the debug overlay). deps[1]: the
        // fragment shaders of those consumers see this pass's depth writes.
        VkSubpassDependency deps[2]{};
        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL; deps[0].dstSubpass = 0;
        deps[0].srcStageMask  = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[0].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        deps[0].dstStageMask  = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        deps[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[1].srcSubpass = 0; deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask  = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        deps[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        VkRenderPassCreateInfo rpci{};
        rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpci.attachmentCount = 1; rpci.pAttachments = &att;
        rpci.subpassCount = 1; rpci.pSubpasses = &sub;
        rpci.dependencyCount = 2; rpci.pDependencies = deps;
        if (!mc2vk::create(s->rpass, s->device, [&](auto* h) { return vkCreateRenderPass(s->device, &rpci, nullptr, h); })) {
            vlog("vkCreateRenderPass failed."); return false;
        }
        VkFramebufferCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        const VkImageView attView = s->depthAttView.get();
        fci.renderPass = s->rpass.get(); fci.attachmentCount = 1; fci.pAttachments = &attView;
        fci.width = static_cast<uint32_t>(size); fci.height = static_cast<uint32_t>(size); fci.layers = 1;
        if (!mc2vk::create(s->fb, s->device, [&](auto* h) { return vkCreateFramebuffer(s->device, &fci, nullptr, h); })) {
            vlog("vkCreateFramebuffer failed."); return false;
        }
    }

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
        sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; sizes[0].descriptorCount = 2;
        sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; sizes[1].descriptorCount = 2;
        VkDescriptorPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.maxSets = 2; pci.poolSizeCount = 2; pci.pPoolSizes = sizes;
        if (!mc2vk::create(s->pool, s->device, [&](auto* h) { return vkCreateDescriptorPool(s->device, &pci, nullptr, h); })) {
            vlog("vkCreateDescriptorPool failed."); return false;
        }
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        const VkDescriptorSetLayout dslHandle = s->dsl.get();
        const VkDescriptorSetLayout setLayouts[2] = {dslHandle, dslHandle};
        dsai.descriptorPool = s->pool.get(); dsai.descriptorSetCount = 2; dsai.pSetLayouts = setLayouts;
        if (vkAllocateDescriptorSets(s->device, &dsai, s->set) != VK_SUCCESS) {
            vlog("vkAllocateDescriptorSets failed."); return false;
        }
        for (uint32_t par = 0; par < 2; ++par) {
            if (!ensure_inst_capacity(s, par, 256 * 1024)) return false;   // ~2340 instances to start
        }
    }

    // ---- Pipelines: the opaque caster and the alpha-tested one share one layout (set 0 the frame UBO + instances, set 1 the alpha array, a push constant
    // of the per-packet layer and UV scale), so the set 0 binding of a pass stays valid across a switch of pipeline. ----
    {
        if (!load_caster_modules(s)) { setFallback("spv_load_failed"); return false; }

        // Set 1: the alpha-ON albedo array the colour island imported (bound later, when it exists).
        VkDescriptorSetLayoutBinding ab{};
        ab.binding = 0; ab.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; ab.descriptorCount = 1; ab.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo alci{};
        alci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        alci.bindingCount = 1; alci.pBindings = &ab;
        if (!mc2vk::create(s->alphaDsl, s->device, [&](auto* h) { return vkCreateDescriptorSetLayout(s->device, &alci, nullptr, h); })) { vlog("alpha descriptor layout failed."); return false; }
        VkDescriptorPoolSize aps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
        VkDescriptorPoolCreateInfo apci{};
        apci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        apci.maxSets = 1; apci.poolSizeCount = 1; apci.pPoolSizes = &aps;
        if (!mc2vk::create(s->alphaPool, s->device, [&](auto* h) { return vkCreateDescriptorPool(s->device, &apci, nullptr, h); })) { vlog("alpha descriptor pool failed."); return false; }
        const VkDescriptorSetLayout alphaLayout = s->alphaDsl.get();
        VkDescriptorSetAllocateInfo adsai{};
        adsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        adsai.descriptorPool = s->alphaPool.get(); adsai.descriptorSetCount = 1; adsai.pSetLayouts = &alphaLayout;
        if (vkAllocateDescriptorSets(s->device, &adsai, &s->alphaSet) != VK_SUCCESS) { vlog("alpha descriptor set allocation failed."); return false; }
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
    if (!s->mech.init(s->device, s->allocator, s->rpass.get(), s->ubo.get(), sizeof(ShadowFrameUBO), spv_dir()))
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
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = s->gfxFamily;
        if (!mc2vk::create(s->cpool, s->device, [&](auto* h) { return vkCreateCommandPool(s->device, &pci, nullptr, h); })) {
            vlog("vkCreateCommandPool failed."); return false;
        }
        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = s->cpool.get(); cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 2;
        if (vkAllocateCommandBuffers(s->device, &cai, s->cbuf) != VK_SUCCESS) {
            vlog("vkAllocateCommandBuffers failed."); return false;
        }
        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        for (uint32_t par = 0; par < 2; ++par) {
            if (!mc2vk::create(s->frameFence[par], s->device, [&](auto* h) { return vkCreateFence(s->device, &fci, nullptr, h); })) {
                vlog("frame fence failed."); return false;
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
    if (s->vbo || s->ibo) mc2vk::getOrCreateEngineVulkanContext().waitIdle("shadow_geometry_release");
    s->vbo.reset();
    s->ibo.reset();
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

// Starts the frame's recording when the first pass of an epoch asks. The wait on the parity's fence is the retire of the submit made two frames ago: it is
// never a wait on this frame's work, and in steady state it has long been signalled.
static bool begin_frame(ShadowIsland* s, uint64_t epoch) {
    if (s->recording && s->recordEpoch == epoch) return true;
    const uint32_t par = static_cast<uint32_t>(epoch & 1u);
    const VkFence fence = s->frameFence[par].get();
    { ZoneScopedN("VKIsl.Shadow.FenceWait");
      if (vkWaitForFences(s->device, 1, &fence, VK_TRUE, 5ull * 1000 * 1000 * 1000) != VK_SUCCESS) {
          setFallback("fence_timeout"); s->disabled = true; return false;
      } }
    VkCommandBuffer cb = s->cbuf[par];
    vkResetCommandBuffer(cb, 0);   // also drops a recording an earlier frame never submitted
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) { setFallback("record_failed"); s->disabled = true; return false; }
    vk_render_pass_timer::Begin(cb, gos_render_pass_timer::Pass_ShadowDyn);
    s->recordEpoch = epoch;
    s->parity = par;
    s->passCount = 0;
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
        s->instRuns.clear();
        s->stagedInstances = instances;
        s->stagedInstanceCount = 0;
        return true;
    }
    ZoneScopedN("VKIsl.Shadow.Feed");
    const VkDeviceSize instBytes = static_cast<VkDeviceSize>(instanceCount) * sizeof(GpuStaticPropInstance);
    if (!ensure_inst_capacity(s, s->parity, instBytes)) { s->disabled = true; return false; }
    const GpuStaticPropInstance* insts = static_cast<const GpuStaticPropInstance*>(instances);
    std::vector<uint32_t> order(instanceCount);
    for (uint32_t k = 0; k < instanceCount; ++k) order[k] = k;
    std::sort(order.begin(), order.end(), [insts](uint32_t a, uint32_t b) { return insts[a].typeID < insts[b].typeID; });
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
    void* view = nullptr;
    uint32_t generation = 0;
    s->alphaReady = false;
    if (!mc2_color_prop_island_get_alpha_array(&view, &generation) || !view) return;
    const VkImageView arrayView = reinterpret_cast<VkImageView>(view);
    if (arrayView != s->alphaBoundView || generation != s->alphaBoundGen) {
        mc2vk::getOrCreateEngineVulkanContext().waitIdle("shadow_alpha_array");
        VkDescriptorImageInfo info{s->alphaSampler.get(), arrayView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.dstSet = s->alphaSet; w.dstBinding = 0; w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &info;
        vkUpdateDescriptorSets(s->device, 1, &w, 0, nullptr);
        s->alphaBoundView = arrayView;
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
    return enabled && pp && pp->shadowsEnabled_;
}

// The static shadow bake: retires a finished bake and records the next one when its inputs are ready and differ from the last. Like the static map it
// counts as a pass, so its frame is submitted even when every other pass declines.
static void record_bake(ShadowIsland* s) {

    mc2vk::ShadowBakeInputs in;
    std::vector<uint32_t> firstPacket, packetCount;
    if (s->geomUploaded) {
        firstPacket.resize(s->typeRuns.size());
        packetCount.resize(s->typeRuns.size());
        for (size_t t = 0; t < s->typeRuns.size(); ++t) { firstPacket[t] = s->typeRuns[t].firstPacket; packetCount[t] = s->typeRuns[t].packetCount; }
        in.vbo = s->vbo.get();
        in.ibo = s->ibo.get();
        in.packets = s->packets;
        in.typeFirstPacket = firstPacket;
        in.typePacketCount = packetCount;
        in.packetAlpha = s->packetAlpha;
        in.alphaSet = s->alphaSet;
        in.alphaReady = s->alphaReady;
        in.alphaContentHash = mc2_color_prop_island_alpha_content_hash();
    }
    if (!in.alphaReady) {
        Mc2ColorPropMaterialFeed mf{};
        in.alphaAbsent = mc2_color_prop_island_get_material_feed(&mf) && mf.arrayKeyOn == 0;
    }
    if (s->bake.update(s->cbuf[s->parity], s->recordEpoch, in)) ++s->passCount;
}

// The start every pass shares: the island is up, the caster geometry is resident, the frame's recording is open and the casters are staged.

// The static-prop caster draws of a run list (alpha-tested packets switch to the alpha pipeline and set 1, and back).
static void draw_static_runs(ShadowIsland* s, VkCommandBuffer cb, const std::vector<InstRun>& runs, bool& alphaBound) {
    for (const InstRun& run : runs) {
        if (run.typeID >= s->typeRuns.size()) continue;
        const TypeRun& tr = s->typeRuns[run.typeID];
        for (uint32_t k = 0; k < tr.packetCount; ++k) {
            const uint32_t pk = tr.firstPacket + k;
            if (pk >= s->packets.size()) break;
            const Mc2ShadowPacket& pkt = s->packets[pk];
            const bool alphaPacket = s->alphaReady && pk < s->packetAlpha.size() && s->packetAlpha[pk].layer >= 0;
            if (alphaPacket) {
                if (!alphaBound) {
                    mc2vk::infra::bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->alphaPipe.get());
                    const VkDescriptorSet aset = s->alphaSet;
                    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->playout.get(), 1, 1, &aset, 0, nullptr);
                    alphaBound = true;
                }
                const ShadowIsland::PacketAlpha& pa = s->packetAlpha[pk];
                const struct { float scale[2]; int32_t layer; int32_t pad; } pc = {{pa.scaleX, pa.scaleY}, pa.layer, 0};
                vkCmdPushConstants(cb, s->playout.get(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
            } else if (alphaBound) {
                mc2vk::infra::bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipe.get());
                alphaBound = false;
            }
            vkCmdDrawIndexed(cb, pkt.indexCount, run.count, pkt.firstIndex, static_cast<int32_t>(pkt.baseVertex), run.firstInstance);
        }
    }
}

// Appends one depth-only render pass to the frame's command buffer: the static-prop casters, then the mech casters, into `fb`. The light-space matrix goes to
// this pass's own slice of the frame UBO, so no pass reads a matrix a later pass overwrote. Nothing here waits for the GPU.
static bool record_pass(ShadowIsland* s, VkFramebuffer fb, int size, const float* lightSpace16, float biasFactor, float biasUnits, bool drawCasters = true) {
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

    VkClearValue clear{};
    clear.depthStencil = {1.0f, 0};   // forward-Z clear to 1.0 (GL glClearDepth(1.0))
    VkRenderPassBeginInfo rbi{};
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = s->rpass.get();
    rbi.framebuffer = fb;
    rbi.renderArea = {{0, 0}, {static_cast<uint32_t>(size), static_cast<uint32_t>(size)}};
    rbi.clearValueCount = 1;
    rbi.pClearValues = &clear;
    MC2_VK_GPU_SCOPE(gpuScope, cb, "VKIsl.Shadow.Dyn");
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);

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
    bool alphaBound = false;   // the alpha-tested pipeline and set 1 are bound
    const VkDeviceSize vboOff = 0;
    const VkBuffer vertexBuf = s->vbo.get();
    vkCmdBindVertexBuffers(cb, 0, 1, &vertexBuf, &vboOff);
    vkCmdBindIndexBuffer(cb, s->ibo.get(), 0, VK_INDEX_TYPE_UINT32);
    if (drawCasters) {
    draw_static_runs(s, cb, s->instRuns, alphaBound);
    s->mech.record(cb, s->parity, dynamicOffset);
    }
    vkCmdEndRenderPass(cb);
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
    bool alphaBound = false;
    draw_static_runs(s, cb, runs, alphaBound);
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
        VkImageMemoryBarrier b[2]{};
        for (auto& x : b) { x.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER; x.srcQueueFamilyIndex = x.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; x.subresourceRange = depthRange; x.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; }
        b[0].image = L.staticAtlas.image.get(); b[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; b[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b[1].image = L.liveAtlas.image.get();   b[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, b);
        const VkClearDepthStencilValue one{1.0f, 0};
        vkCmdClearDepthStencilImage(cb, L.liveAtlas.image.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &one, 1, &depthRange);
        VkImageMemoryBarrier r = b[1];
        r.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; r.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        r.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; r.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &r);
        L.layoutsReady = true;
        ++s->passCount;   // the frame must be submitted for the transitions
    }
    L.ready = true;
    uint32_t tileCount = 0;
    const Tile* tiles = currentTiles(&tileCount);
    if (!tiles || tileCount == 0) return;
    ZoneScopedN("VKIsl.Shadow.LocalLights");

    const auto& casterSet = mc2shadow::staticCasterSet();
    if (casterSet.version() != L.leavesVersion || casterSet.deltaVersion() != L.leavesDelta) {
        casterSet.resolve(L.leaves);
        L.leavesVersion = casterSet.version();
        L.leavesDelta = casterSet.deltaVersion();
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
        VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rbi.renderPass = L.bakePass.get(); rbi.framebuffer = L.bakeFb.get();
        rbi.renderArea = {{0, 0}, {kAtlasW, kAtlasH}};
        MC2_VK_GPU_SCOPE(bakeScope, cb, "VKIsl.Shadow.LocalBake");
        vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
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
        }
        vkCmdEndRenderPass(cb);
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
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = L.livePass.get(); rbi.framebuffer = L.liveFb.get();
    rbi.renderArea = {{0, 0}, {kAtlasW, kAtlasH}};
    MC2_VK_GPU_SCOPE(liveScope, cb, "VKIsl.Shadow.LocalLive");
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    for (uint32_t i = 0; i < usedCount; ++i) {
        const uint32_t t = used[i];
        if (!tiles[t].mover) continue;
        VkClearAttachment ca{VK_IMAGE_ASPECT_DEPTH_BIT, 0, {}};
        ca.clearValue.depthStencil = {1.0f, 0};
        const VkClearRect cr{{{static_cast<int32_t>((t % kCols) * kTileSide), static_cast<int32_t>((t / kCols) * kTileSide)}, {kTileSide, kTileSide}}, 0, 1};
        vkCmdClearAttachments(cb, 1, &ca, 1, &cr);
    }
    if (s->mech.hasCasters()) {
        for (uint32_t i = 0; i < usedCount; ++i) {
            const uint32_t t = used[i];
            float m[16];
            lightMatrix(tiles[t], m);
            uint32_t dyn = 0;
            if (!local_ubo_slice(s, m, dyn)) break;
            local_tile_viewport(cb, t);
            s->mech.record(cb, s->parity, dyn);
        }
    }
    vkCmdEndRenderPass(cb);
    liveScope.end();
    L.tilesDrawn = usedCount;
    ++s->passCount;
    static uint64_t frames = 0;
    if (++frames % 600 == 1 && mc2::gate::get<"MC2_DIAG_VERBOSE">())
        vlog("[LOCAL_SHADOW v1] tiles=%u baked_now=%u bakes_total=%llu static_leaves=%zu mech_casters=%d",
             usedCount, bakeCount, static_cast<unsigned long long>(L.bakesTotal), L.leaves.size(), s->mech.hasCasters() ? 1 : 0);
    if (bakeCount > 0 && mc2::gate::get<"MC2_LIGHT_CONE_DIAG">())
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
    // Item-2 boot-level native-or-decline: a prior PERMANENT init failure declined this island for the whole session -> GL owns the dynamic shadow pass.
    if (g_shadowSessionDeclined) return false;
    // Lazy init / rebuild on size change.
    mc2vk::VulkanContext& slotCtx = mc2vk::getOrCreateEngineVulkanContext();
    if (const ShadowIsland* parked = parkedIsland(); parked && parked->size != size) slotCtx.retireIsland(mc2vk::IslandSlot::Shadow);
    if (!islandSlot()) {
        auto fresh = std::make_unique<ShadowIsland>();
        if (!init_island(fresh.get(), size)) {
            const bool permanent = (g_shadowInitReachedBuild != 0);
            slotCtx.waitIdle("shadow_init_failure");   // objects made before the failure may have been submitted
            fresh.reset();
            if (permanent) {
                // Build-phase failure won't recover this session: latch decline + ONE receipt line (vs. re-init'ing every frame). GL renders the shadow pass.
                g_shadowSessionDeclined = true;
                vlog("[VK_SHADOW_ISLAND] boot-level decline: permanent init failure (%s) -- "
                     "GL owns the dynamic shadow pass for this session (no per-frame retry).",
                     g_health.fallbackReason);
            } else {
                // Transient readiness gate (VK context / gate not ready): fail-soft-retry.
                static bool s_transientLogged = false;
                if (!s_transientLogged) {
                    vlog("init not ready (%s) -- GL fallback (will retry).", g_health.fallbackReason);
                    s_transientLogged = true;
                }
            }
            return false;
        }
        islandSlot() = std::move(fresh);
    }
    return true;
}

extern "C" int mc2_vk_shadow_island_prewarm(int,int) {
    if (!ensure_island(mc2shadow::shadowQuality().mapSide)) return 0;
    auto* s=parkedIsland();
    upload_geometry_once(s);
    refresh_alpha_casters(s);
    return s->bake.prewarmProducts() ? 1 : 0;
}

bool mc2vk::bindMoverShadowDescriptors(VkDevice device, VkDescriptorSet set) {
    ShadowIsland* s=parkedIsland();
    if (!s) {
        if (!ensure_island(1)) return false; // inactive descriptors need no full-size map
        s=parkedIsland();
    }
    const bool ready=mover_shadows_enabled() && productReady(g_dynShadowReadyThisFrame);
    return s->mover.bind(device,set,s->depthResource.view.get(),ready);
}

extern "C" void mc2_vk_shadow_island_run_movers(const float cameraRows[16], const float sun[3],
                                                const void* instances, unsigned int instanceCount) {
    g_dynShadowReadyThisFrame=false;
    ++g_shadowFrameEpoch;
    if (!cameraRows || !sun || !mover_shadows_enabled() || !ensure_island(mc2shadow::shadowQuality().mapSide)) return;
    ShadowIsland* s=parkedIsland();
    if (!s || s->disabled || !upload_geometry_once(s) || !begin_frame(s,g_shadowFrameEpoch)) return;
    refresh_alpha_casters(s);
    s->mech.stage(g_shadowFrameEpoch);
    record_bake(s);
    record_local_shadows(s);   // LOCAL-LIGHTS-2
    const auto start=std::chrono::steady_clock::now();
    const auto light=mc2shadow::clampSunAltitude({sun[0],sun[1],sun[2]},mc2ShadowMinSunAltDeg());
    const auto props=std::span(static_cast<const GpuStaticPropInstance*>(instances),instanceCount);
    if (!s->mover.prepare(s->size,cameraRows,light,props,s->bake.ready())) return;
    const auto& selected=s->mover.props();
    if (!stage_instances(s,selected.data(),static_cast<uint32_t>(selected.size()))) return;
    const auto& frame=s->mover.frame();
    if (!record_pass(s,s->fb.get(),s->size,frame.matrix,1.0f,2.0f,frame.flags[0]!=0)) return;
    g_dynShadowReadyThisFrame=true;
    static uint64_t frames=0;
    if (++frames%600==1) {
        const double us=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count();
        vlog("movers actors_feed=%u actors_visible=%u rigid_leaves=%zu map=%d format=D16 texel_wu=%.3f depth_span=%.1f cpu_fit_and_record_us=%.1f resident_mb=%.1f coverage_kb=8",
             s->mover.actorCount(),s->mover.visibleActorCount(),selected.size(),s->size,frame.params[0],frame.params[1]>0?1.0f/frame.params[1]:0.0f,us,double(s->size)*s->size*2/1048576.0);
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
    const VkFence fence = s->frameFence[s->parity].get();
    vkResetFences(s->device, 1, &fence);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    ZoneScopedN("VKIsl.Shadow.Submit");
    if (mc2vk::submitGraphics(si, fence) != VK_SUCCESS) { setFallback("submit_failed"); disableFrame(); return; }
    timerGuard.submitted();
    s->bake.markSubmitted(s->recordEpoch);
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

#endif // MC2_VULKAN_ISLAND
