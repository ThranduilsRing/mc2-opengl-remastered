// VK-POST-FOG-NATIVE-1 (rung 8.1, VK full-ownership ladder — .claude/VK-RUNG8-LADDER-1.md).
//
// The FIRST rung-8 postprocess pass rendered natively by VK on the SHARED ENGINE
// VulkanContext, reading the shared scene GBuffer and writing the shared scene color
// IN PLACE — ZERO copies, ZERO CPU readbacks, ZERO private devices:
//
//   * samples the SHARED scene depth (vulkan_shared_scene_depth.cpp — the SAME
//     VkImage GL's sceneDepthTex_ is a view of) directly — kills the fog subgraph's
//     per-frame glGetTexImage(GL_DEPTH_COMPONENT) CPU readback + staging upload;
//   * blends edge fog + oob fog straight into the SHARED scene color0
//     (vulkan_shared_scene_color.cpp — the SAME VkImage GL's sceneColorTex_ views) —
//     kills the subgraph's 2x per-frame glCopyImageSubData color round-trips;
//   * borrows the shared engine VulkanContext (water/terrain/prop/mech island
//     pattern) — the old subgraph's PRIVATE VkInstance/VkDevice is bypassed entirely.
//
// SHADERS REUSED VERBATIM: shaders/vulkan/edge_fog.{vert,frag} + fog_oob.{vert,frag}
// (the parity-proven subgraph/island SPIR-V; port markers already registered in
// tools/glsl_port_diff.py). Same descriptor interface: set0 binding0 = depth
// combined-sampler, binding1 = std140 params UBO. UBO PODs are byte-identical to
// the subgraph's (cross-checked offline by tools/check_ubo_layout.py labels
// postfog_edge / postfog_oob).
//
// GATE: umbrella-armed (mc2vkgate::umbrellaOn(), see the block above
// vkPostFogNativeEnabled): the shared GBuffer substrate must be live. Edge and
// out-of-bounds fog are independently selected; only edge fog needs a map extent.
//
// SYNC: per frame BeginFrame on BOTH shared resources. Guarded by the full upstream
// native ownership chain, color/depth are treated as already Vulkan-owned: there is no
// GL<->VK handoff, fog renders in place and leaves both resources Vulkan-owned for later
// post/composite work.
//
// The whole TU compiles to nothing unless MC2_VULKAN_ISLAND is defined.

#include "gos_postprocess.h"
#include "vk_render_pass_timer.h"
#include "vk_gpu_profile.h"   // VK-GPU-ZONES: MC2_VK_GPU_SCOPE
#include <tracy/Tracy.hpp>
#include "Gates/gate.h"
#include "Gates/render_options.h"
#include "Gates/gate_cold.h"   // str<>()


#if defined(_WIN32) && !defined(VK_USE_PLATFORM_WIN32_KHR)
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <volk.h>

#include <vk_mem_alloc.h>

#include "vk_image_resource.h"
#include "vk_island_util.h"
#include "vulkan_context.h"
#include "../../RenderCore/postprocess_subpass_order.h"
// Expose the VkImageResource getters (island-side halves).
#define MC2_VULKAN_SHARED_SCENE_COLOR_IMPL_ISLAND
#include "vulkan_shared_scene_color.h"
#define MC2_VULKAN_SHARED_SCENE_DEPTH_IMPL_ISLAND
#include "vulkan_shared_scene_depth.h"
#include "vk_shared_gbuffer_gate.h"
#include "vulkan_post_chain.h"

// Fixed-timestep clock so the animated OOB FBM time matches the GL path under smoke.
#include "gos_smoke.h"
#include <SDL2/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>
#include <vector>
#include "../../mclib/battle_bench_wait.h"
#include "vk_island_fail_policy.h"

extern void gos_GetTerrainLightDir(float* x, float* y, float* z);
float mc2NightLightFactor();   // mclib/camera.cpp: 0 day/evening .. 1 full night

namespace {

void vlog(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vk_island_util::vlogv512("[VK_POST_FOG_NATIVE] ", fmt, ap);
    va_end(ap);
}

// Health snapshot — read by debug_state_dump.cpp via the extern "C" getter below
// (used_vulkan receipt for every VK capture, per the arc's evidence protocol).
struct PostFogNativeHealth {
    int  runtimeGate   = 1;   // the native fog pair is always on
    int  substrateLive = 0;   // shared color0 + depth resources present at init
    int  directChain   = 1;   // direct-chain engaged
    unsigned long attempted  = 0;  // frames the native path was entered
    unsigned long usedVulkan = 0;  // frames BOTH fogs actually rendered via VK
    unsigned long usedInterop = 0; // frames that used GL<->VK semaphores
    unsigned long glSkipped  = 0;  // GL fog sites skipped (2 per used frame)
    char fallbackReason[64]  = {0};
    double renderUs = 0.0;         // record+submit+sem/fence wait, last frame
    // VK-QWI-TO-FENCE-1: water-style us_fence_wait telemetry around the per-frame
    // CPU-blocking queue wait (QWI before the conversion, the per-submit fence after).
    double waitUsLast = 0.0;
    double waitUsAccum = 0.0;
    unsigned long waitSamples = 0;
};
PostFogNativeHealth g_pf;

void setFallback(const char* reason) {
    std::strncpy(g_pf.fallbackReason, reason ? reason : "",
                 sizeof(g_pf.fallbackReason) - 1);
    g_pf.fallbackReason[sizeof(g_pf.fallbackReason) - 1] = '\0';
}

std::string spv_dir() { return vk_island_util::spvDir(); }

VkShaderModule load_spv(VkDevice device, std::string_view path) {
    return vk_island_util::loadSpv(device, path, vlog);
}

// std140 PODs for the same edge/oob fog shaders as the retired subgraph path;`r`n// offsets are offline-checked by check_ubo_layout.py postfog_edge/postfog_oob.
struct EdgeFogParams {
    float invViewProj[16];     // off 0  (same 16 floats the GL path uploads, GL_FALSE)
    float u_fogColor[3];       // off 64
    float _pad0;               // off 76
    float u_halfExtent;        // off 80
    float u_fogStart;          // off 84
    float u_fogHeight;         // off 88
    float u_fogMax;            // off 92
    float u_waterElevation;    // off 96
    int   u_horizonClampEnabled;   // off 100 (VK-PARITY-F3: FOG-HORIZON-CLAMP-1)
    float u_horizonFadeStartSin;   // off 104
    float u_horizonFadeEndSin;     // off 108
    float u_hazeStart;             // off 112 (source wu; shader applies x1.5)
    float u_hazeFull;              // off 116
    int   u_fogProfile;            // off 120 (DrawDistance 0 = Stock)
    float _pad1;                   // off 124
    float u_atmoParams[4];         // off 128 quality, ground extinction/wu, height falloff/wu, HG g
    float u_atmoSun[4];            // off 144 unit toward sun (Z up), in-scatter strength
    float u_atmoColor[4];          // off 160 ambient scatter rgb, base elevation
};
static_assert(sizeof(EdgeFogParams) == 176, "EdgeFogParams std140 offsets drifted");

struct FogOobParams {
    float invViewProj[16];     // off 0
    float u_fogColor[3];       // off 64
    float u_fogOpacity;        // off 76
    float u_time;              // off 80
    int   u_horizonClampEnabled;   // off 84 (VK-PARITY-F3: FOG-HORIZON-CLAMP-1)
    float u_horizonFadeStartSin;   // off 88
    float u_horizonFadeEndSin;     // off 92
};
static_assert(sizeof(FogOobParams) == 96, "FogOobParams std140 offsets drifted");

} // namespace

// Persistent state. Lazy init at the fog site; torn down by destroyFBOs (resize)
// and destroy() via destroyVkPostFogNative().
struct gosPostProcess::VkPostFogNative {
    bool disabled = false;   // init failed (Degrade, vk_island_fail_policy.h) -> no native fog for this process
    bool inited   = false;
    int  width = 0, height = 0;

    // BORROWED engine context handles (never destroyed here).
    VkPhysicalDevice phys   = VK_NULL_HANDLE;
    VkDevice         device = VK_NULL_HANDLE;
    uint32_t         gfxFamily = UINT32_MAX;
    VkQueue          queue  = VK_NULL_HANDLE;
    VmaAllocator     allocator = VK_NULL_HANDLE;

    // BORROWED shared GBuffer resources/views (owned by vulkan_shared_scene_*.cpp).
    VkImageResource* sColorRes = nullptr;
    VkImageResource* sDepthRes = nullptr;
    // VK-RUNG8-ITEM5: shared scene is VK-only (no GL view); direct-chain only.

    // Match the engine frame ring so fog does not serialize next-frame setup.
    struct Frame {
        VkDescriptorSet edgeSet = VK_NULL_HANDLE, oobSet = VK_NULL_HANDLE;
        VkCommandBuffer cbuf = VK_NULL_HANDLE;
        mc2vk::OwnedCommandPool cpool;   // per-frame-slot pool: vkResetCommandPool instead of per-cb reset
        mc2vk::OwnedBuffer edgeUbo, oobUbo;
        uint64_t ticket = 0;
    } frames[mc2vk::kMaxFramesInFlight];

    // OWNED objects, declared in creation order: the destructor releases them in reverse (the owner idles the GPU first, see destroy_state).
    mc2vk::OwnedImageView  depthSampleView; // depth-aspect view over sDepthRes->image
    mc2vk::OwnedSampler    depthSampler;    // NEAREST/CLAMP (matches GL fog sampling)
    mc2vk::OwnedRenderPass rpass;
    mc2vk::OwnedFramebuffer fb;
    mc2vk::OwnedDescriptorSetLayout dsl;
    mc2vk::OwnedDescriptorPool pool;
    mc2vk::OwnedShaderModule edgeVert, edgeFrag;
    mc2vk::OwnedShaderModule oobVert, oobFrag;
    mc2vk::OwnedPipelineLayout playout;
    mc2vk::OwnedPipeline   edgePipe;
    mc2vk::OwnedPipeline   oobPipe;
};

namespace {

bool make_buffer(gosPostProcess::VkPostFogNative* s, VkDeviceSize size,
                 VkBufferUsageFlags usage, mc2vk::OwnedBuffer& out,
                 std::string_view name, int i0 = -1, int i1 = -1) {
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size; bci.usage = usage;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;   // per-frame GPU-read feed: ReBAR device-local where available
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    if (!out.create(s->allocator, s->device, bci, aci, name, i0, i1)) {
        vlog("vmaCreateBuffer failed."); return false;
    }
    return true;
}

// Releases the owned objects (the state's destructor does it, once the GPU is idle) and resets the state; the borrowed handles are not touched.
void destroy_state(gosPostProcess::VkPostFogNative* s) {
    if (!s) return;
    if (s->device != VK_NULL_HANDLE) mc2vk::getOrCreateEngineVulkanContext().waitIdle("fog_destroy");
    const bool disabled = s->disabled;
    *s = gosPostProcess::VkPostFogNative{};
    s->disabled = disabled;
}

// Set once build_state is past its readiness checks (device, shared GBuffer at this size): a failure before that waits, not degrades.
static bool s_fogReachedBuild = false;

bool build_state(gosPostProcess::VkPostFogNative* s, int w, int h) {
    s->width = w; s->height = h;
    s_fogReachedBuild = false;

    // Shared engine device, on-demand (SSAO/water model; fail-soft -> GL fog).
    if (!mc2vk::ensureEngineVulkanContext()) {
        setFallback("engine_vk_context_birth_failed"); return false;
    }
    mc2vk::VulkanContext& ctx = mc2vk::getOrCreateEngineVulkanContext();
    if (!ctx.valid()) { setFallback("engine_vk_context_not_valid"); return false; }
    s->phys = ctx.physicalDevice();
    s->device = ctx.device();
    s->queue = ctx.graphicsQueue();
    s->gfxFamily = ctx.graphicsFamily();
    s->allocator = ctx.allocator();
    if (s->device == VK_NULL_HANDLE || s->allocator == VK_NULL_HANDLE) {
        setFallback("engine_vk_device_or_allocator_null"); return false;
    }

    // Borrow the shared GBuffer (same accessors the tier1 islands use).
    s->sColorRes  = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_COLOR0);
    s->sDepthRes  = mc2SharedSceneDepthGetResource();
    // VK-RUNG8 item-4/item-5: direct-chain-native only. Only the VK-only shared
    // VkImageResources are required; there is no GL interop view.
    if (!s->sColorRes || !s->sDepthRes) {
        setFallback("shared_gbuffer_not_ready"); return false;
    }
    if (static_cast<int>(s->sColorRes->w) != w || static_cast<int>(s->sColorRes->h) != h ||
        static_cast<int>(s->sDepthRes->w) != w || static_cast<int>(s->sDepthRes->h) != h) {
        setFallback("shared_gbuffer_size_mismatch"); return false;
    }
    g_pf.substrateLive = 1;
    s_fogReachedBuild = true;

    VkResult r;

    // Depth-aspect sampling view over the SHARED depth image (sampling a depth
    // aspect returns depth in .r — identical values to the GL glGetTexImage float
    // conversion the subgraph uploaded, so the ported shader math is unchanged).
    {
        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = s->sDepthRes->image.get();
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = s->sDepthRes->format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        if (!mc2vk::create(s->depthSampleView, s->device, [&](VkImageView* h) { r = vkCreateImageView(s->device, &vci, nullptr, h); return r; })) {
            vlog("depth sample view create failed (%d).", static_cast<int>(r)); return false;
        }
    }
    {
        VkSamplerCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter = VK_FILTER_NEAREST; sci.minFilter = VK_FILTER_NEAREST;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (!mc2vk::create(s->depthSampler, s->device, [&](VkSampler* h) { r = vkCreateSampler(s->device, &sci, nullptr, h); return r; })) {
            vlog("vkCreateSampler failed (%d).", static_cast<int>(r)); return false;
        }
    }

    for (auto& frame : s->frames) {
        if (!make_buffer(s, sizeof(EdgeFogParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, frame.edgeUbo, "postfog.edgeUbo")) return false;
        if (!make_buffer(s, sizeof(FogOobParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, frame.oobUbo, "postfog.oobUbo")) return false;
    }

    // Render pass: 1 attachment = SHARED scene color0. LOAD (fog blends OVER the
    // scene), STORE; layout-STABLE COLOR_ATTACHMENT both ends (GL hands it over /
    // re-acquires it in GL_LAYOUT_COLOR_ATTACHMENT_EXT — subgraph-interop pattern).
    {
        VkAttachmentDescription att{};
        att.format = s->sColorRes->format;
        att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.finalLayout   = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1; sub.pColorAttachments = &ref;
        // Incoming + outgoing external deps (water/subgraph-interop WAW/RAW pattern:
        // sync-val can't see the GL semaphore serialization across parity slots).
        VkSubpassDependency deps[2]{};
        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL; deps[0].dstSubpass = 0;
        deps[0].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].srcSubpass = 0; deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstStageMask  = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        deps[1].dstAccessMask = 0;
        VkRenderPassCreateInfo rpci{};
        rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpci.attachmentCount = 1; rpci.pAttachments = &att;
        rpci.subpassCount = 1; rpci.pSubpasses = &sub;
        rpci.dependencyCount = 2; rpci.pDependencies = deps;
        if (!mc2vk::create(s->rpass, s->device, [&](VkRenderPass* h) { r = vkCreateRenderPass(s->device, &rpci, nullptr, h); return r; })) {
            vlog("vkCreateRenderPass failed (%d).", static_cast<int>(r)); return false;
        }

        VkFramebufferCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = s->rpass.get();
        const VkImageView colorAttachment = s->sColorRes->view.get();
        fci.attachmentCount = 1; fci.pAttachments = &colorAttachment;
        fci.width = static_cast<uint32_t>(w); fci.height = static_cast<uint32_t>(h); fci.layers = 1;
        if (!mc2vk::create(s->fb, s->device, [&](VkFramebuffer* h) { r = vkCreateFramebuffer(s->device, &fci, nullptr, h); return r; })) {
            vlog("vkCreateFramebuffer failed (%d).", static_cast<int>(r)); return false;
        }
    }

    // Descriptor layout / pool / 2 sets (subgraph interface: 0=sampler, 1=UBO, frag).
    {
        VkDescriptorSetLayoutBinding binds[2]{};
        binds[0].binding = 0; binds[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binds[0].descriptorCount = 1; binds[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        binds[1].binding = 1; binds[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        binds[1].descriptorCount = 1; binds[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.bindingCount = 2; lci.pBindings = binds;
        if (!mc2vk::create(s->dsl, s->device, [&](VkDescriptorSetLayout* h) { r = vkCreateDescriptorSetLayout(s->device, &lci, nullptr, h); return r; })) {
            vlog("vkCreateDescriptorSetLayout failed (%d).", static_cast<int>(r)); return false;
        }
        VkDescriptorPoolSize sizes[2]{};
        sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; sizes[0].descriptorCount = 2 * mc2vk::kMaxFramesInFlight;
        sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;         sizes[1].descriptorCount = 2 * mc2vk::kMaxFramesInFlight;
        VkDescriptorPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.maxSets = 2 * mc2vk::kMaxFramesInFlight; pci.poolSizeCount = 2; pci.pPoolSizes = sizes;
        if (!mc2vk::create(s->pool, s->device, [&](VkDescriptorPool* h) { r = vkCreateDescriptorPool(s->device, &pci, nullptr, h); return r; })) {
            vlog("vkCreateDescriptorPool failed (%d).", static_cast<int>(r)); return false;
        }
    }
    for (auto& frame : s->frames) {
        VkDescriptorSetLayout layouts[2] = { s->dsl.get(), s->dsl.get() };
        VkDescriptorSet setsOut[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = s->pool.get(); dsai.descriptorSetCount = 2; dsai.pSetLayouts = layouts;
        r = MC2_VK_ALLOC_SETS(s->device, &dsai, setsOut);
        if (r != VK_SUCCESS) { vlog("vkAllocateDescriptorSets failed (%d).", static_cast<int>(r)); return false; }
        frame.edgeSet = setsOut[0]; frame.oobSet = setsOut[1];

        VkDescriptorImageInfo imgInfo{};
        imgInfo.sampler = s->depthSampler.get();
        imgInfo.imageView = s->depthSampleView.get();
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorBufferInfo edgeUboInfo{ frame.edgeUbo.get(), 0, VK_WHOLE_SIZE };
        VkDescriptorBufferInfo oobUboInfo { frame.oobUbo.get(), 0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet writes[4]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = frame.edgeSet; writes[0].dstBinding = 0; writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &imgInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = frame.edgeSet; writes[1].dstBinding = 1; writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[1].pBufferInfo = &edgeUboInfo;
        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = frame.oobSet; writes[2].dstBinding = 0; writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[2].pImageInfo = &imgInfo;
        writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[3].dstSet = frame.oobSet; writes[3].dstBinding = 1; writes[3].descriptorCount = 1;
        writes[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[3].pBufferInfo = &oobUboInfo;
        vkUpdateDescriptorSets(s->device, 4, writes, 0, nullptr);
    }

    // Two pipelines — fixed-function state IDENTICAL to the subgraph's (fullscreen
    // tri, no vertex input, cull none, AlphaBlend SRC_ALPHA/ONE_MINUS_SRC_ALPHA,
    // no depth attachment). Shaders reused verbatim.
    {
        s->edgeVert = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir() + "edge_fog.vert.spv"));
        s->edgeFrag = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir() + "edge_fog.frag.spv"));
        s->oobVert  = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir() + "fog_oob.vert.spv"));
        s->oobFrag  = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir() + "fog_oob.frag.spv"));
        if (!s->edgeVert || !s->edgeFrag || !s->oobVert || !s->oobFrag) {
            setFallback("spv_load_failed"); return false;
        }
        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        const VkDescriptorSetLayout dslHandle = s->dsl.get();
        plci.setLayoutCount = 1; plci.pSetLayouts = &dslHandle;
        if (!mc2vk::create(s->playout, s->device, [&](VkPipelineLayout* h) { r = vkCreatePipelineLayout(s->device, &plci, nullptr, h); return r; })) {
            vlog("vkCreatePipelineLayout failed (%d).", static_cast<int>(r)); return false;
        }

        VkPipelineVertexInputStateCreateInfo vin{};
        vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkViewport vp{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
        VkRect2D sc{{0, 0}, {static_cast<uint32_t>(w), static_cast<uint32_t>(h)}};
        VkPipelineViewportStateCreateInfo vps{};
        vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vps.viewportCount = 1; vps.pViewports = &vp; vps.scissorCount = 1; vps.pScissors = &sc;
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
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.alphaBlendOp = VK_BLEND_OP_ADD;
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 1; cb.pAttachments = &cba;

        auto make_pipeline = [&](VkShaderModule vmod, VkShaderModule fmod, mc2vk::OwnedPipeline& out, std::string_view name) -> bool {
            VkPipelineShaderStageCreateInfo stages[2]{};
            stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vmod; stages[0].pName = "main";
            stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fmod; stages[1].pName = "main";
            VkGraphicsPipelineCreateInfo gp{};
            gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            gp.stageCount = 2; gp.pStages = stages;
            gp.pVertexInputState = &vin;
            gp.pInputAssemblyState = &ia;
            gp.pViewportState = &vps;
            gp.pRasterizationState = &rs;
            gp.pMultisampleState = &ms;
            gp.pColorBlendState = &cb;
            gp.layout = s->playout.get();
            gp.renderPass = s->rpass.get();
            gp.subpass = 0;
            if (!mc2vk::create(out, s->device, [&](VkPipeline* h) { return mc2vk::infra::graphics(s->device, VK_NULL_HANDLE, 1, &gp, nullptr, h); })) {
                vlog("vkCreateGraphicsPipelines failed."); return false;
            }
            mc2vk::nameObject(s->device, out.get(), name);
            return true;
        };
        if (!make_pipeline(s->edgeVert.get(), s->edgeFrag.get(), s->edgePipe, "postfog.edgePipe")) return false;
        if (!make_pipeline(s->oobVert.get(), s->oobFrag.get(), s->oobPipe, "postfog.oobPipe")) return false;
    }

    {
        VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = 0;
        pci.queueFamilyIndex = s->gfxFamily;
        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        for (auto& frame : s->frames) {
            if (!mc2vk::create(frame.cpool, s->device, [&](VkCommandPool* h) { r = vkCreateCommandPool(s->device, &pci, nullptr, h); return r; })) {
                vlog("vkCreateCommandPool failed (%d).", static_cast<int>(r)); return false;
            }
            cai.commandPool = frame.cpool.get();
            r = vkAllocateCommandBuffers(s->device, &cai, &frame.cbuf);
            if (r != VK_SUCCESS) { vlog("vkAllocateCommandBuffers failed (%d).", static_cast<int>(r)); return false; }
        }
    }

    s->inited = true;
    setFallback("");
    vlog("initialized %dx%d — NATIVE fog pair on the shared engine context "
         "(samples shared scene depth, blends into shared scene color0 in place; "
         "ZERO copies, ZERO readbacks, no private device).", w, h);
    return true;
}

} // namespace

// The owner's deleter: idles the GPU and releases the owned objects before the state itself goes.
void gosPostProcess::VkPostFogNativeDeleter::operator()(VkPostFogNative* s) const {
    destroy_state(s);
    delete s;
}

// ---------------------------------------------------------------------------
// Render the requested fog passes in canonical order, in one native render pass.
// ---------------------------------------------------------------------------
void gosPostProcess::prewarmVkFog() {
    if (mc2::options::lowCost()) return; // No two full-screen depth/noise walks on shared DDR4.
    if(width_<=0 || height_<=0 || (!edgeFogEnabled_ && !fogOobEnabled_)) return;
    if(!vkPostFogNative_) vkPostFogNative_.reset(std::make_unique<VkPostFogNative>().release());
    auto* s=vkPostFogNative_.get();
    if(!s->inited && !build_state(s,width_,height_)) destroy_state(s);
}

bool gosPostProcess::runVkPostFogNative()
{
    ZoneScopedN("VKIsl.Post.Fog");
    if (mc2::options::lowCost()) return true;
    const auto fog=RenderCore::vulkan::selectFogPasses(sceneHasTerrain_,edgeFogEnabled_,fogOobEnabled_,mapHalfExtent_);
    if (!fog.edge && !fog.oob) return false;
    // The GL fog passes' stencil sky-exclusion (SKYBOX-FOG-EXCLUDE-1) has no VK port, but
    // with the horizon clamp both fog passes are exactly zero above fogHorizonFadeEnd
    // (5 deg), the only elevations the exclusion would touch, so native fog runs without
    // it (VK-PARITY-F3).

    if (width_ <= 0 || height_ <= 0) return false;

    if (!vkPostFogNative_) vkPostFogNative_.reset(new VkPostFogNative());
    VkPostFogNative* s = vkPostFogNative_.get();
    if (s->disabled) return false;
    auto& frame = s->frames[mc2vk::getOrCreateEngineVulkanContext().frameContext().slot];
    // Wait only when reusing this slot, before host writes and command-buffer reset.
    {
        ZoneScopedN("VKIsl.Post.FogSlotWait"); mc2::battlebench::WaitScope gpuWait1(mc2::battlebench::WaitGpuMainNs);
        const std::chrono::steady_clock::time_point tWait0 = std::chrono::steady_clock::now();
        if (!mc2vk::getOrCreateEngineVulkanContext().wait(mc2vk::GpuTicket{frame.ticket, 0}, 5ull * 1000 * 1000 * 1000, "post_prev_submit")) {
            setFallback("fence_timeout"); s->disabled = true; return false;
        }
        g_pf.waitUsLast = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - tWait0).count();
        g_pf.waitUsAccum += g_pf.waitUsLast;
        ++g_pf.waitSamples;
        if ((g_pf.waitSamples % 600) == 1)
            vlog("[QWI-WAIT] wait_us_last=%.1f wait_us_avg=%.1f samples=%lu",
                 g_pf.waitUsLast,
                 g_pf.waitUsAccum / static_cast<double>(g_pf.waitSamples), g_pf.waitSamples);
    }
    if (!s->inited || s->width != width_ || s->height != height_) {
        if (s->inited) destroy_state(s);
        (void)mc2vk::takeLastCreateFailure();
        if (!build_state(s, width_, height_)) {
            // Fog is optional: a build failure degrades to no fog for this process (one transient-VkResult retry).
            const mc2vk::IslandInitAction action = mc2vk::onIslandInitFailure(mc2vk::IslandKind::Fog, s_fogReachedBuild,
                mc2vk::takeLastCreateFailure(), g_pf.fallbackReason[0] ? g_pf.fallbackReason : "unknown");
            destroy_state(s);
            s->disabled = action == mc2vk::IslandInitAction::Degrade;   // Wait / Retry: try again next frame
            return false;
        }
        mc2vk::islandInitFailures(mc2vk::IslandKind::Fog).reset();
    }

    ++g_pf.attempted;
    using clk = std::chrono::steady_clock;
    const clk::time_point t0 = clk::now();

    // ---- UBOs (values IDENTICAL to the GL runEdgeFog()/runFogOob() uploads). ----
    if (fog.edge) {
        EdgeFogParams p{};
        std::memcpy(p.invViewProj, inverseViewProj_, 16 * sizeof(float));
        p.u_fogColor[0] = edgeFogColor_[0];
        p.u_fogColor[1] = edgeFogColor_[1];
        p.u_fogColor[2] = edgeFogColor_[2];
        p._pad0 = 0.0f;
        p.u_halfExtent = mapHalfExtent_;
        p.u_fogStart = edgeFogStart_;
        p.u_fogHeight = edgeFogHeight_;
        // Fog profile (DrawDistance): source wu = effective / 1.5. Stock (0) keeps edgeFogMax_ and the shader's own haze range.
        static constexpr float kHaze[4][2] = {{6000.0f, 12000.0f}, {4667.0f, 9333.0f}, {3333.0f, 6667.0f}, {2333.0f, 4667.0f}};
        const int profile = std::clamp(mc2::options::current().drawDistance, 0, 3);
        p.u_fogMax = profile ? 1.0f : edgeFogMax_;
        p.u_hazeStart = kHaze[profile][0];
        p.u_hazeFull = kHaze[profile][1];
        p.u_fogProfile = profile;
        p._pad1 = 0.0f;
        p.u_waterElevation = waterElevation_;
        p.u_horizonClampEnabled = fogHorizonClampEnabled_ ? 1 : 0;
        p.u_horizonFadeStartSin = fogHorizonFadeStartSin_;
        p.u_horizonFadeEndSin   = fogHorizonFadeEndSin_;
        {
            const int quality = std::clamp(mc2::options::current().atmosphereQuality, 0, 2);
            const float night = mc2NightLightFactor();
            float lx = 0.f, ly = 0.f, lz = 1.f;
            gos_GetTerrainLightDir(&lx, &ly, &lz);
            const float len = std::sqrt(lx * lx + ly * ly + lz * lz);
            if (len > 1e-6f) { lx /= len; ly /= len; lz /= len; } else { lx = ly = 0.f; lz = 1.f; }
            const float nightDim = 1.0f - 0.85f * night;   // sun scatter and the sky colour fade with the sun
            p.u_atmoParams[0] = static_cast<float>(quality);
            p.u_atmoParams[1] = 6e-5f * atmoDensity_ * (quality == 1 ? 0.7f : 1.0f);
            p.u_atmoParams[2] = atmoFalloff_;
            p.u_atmoParams[3] = atmoAnisotropy_;
            p.u_atmoSun[0] = lx; p.u_atmoSun[1] = ly; p.u_atmoSun[2] = lz;
            p.u_atmoSun[3] = atmoSunScatter_ * nightDim;
            for (int c = 0; c < 3; ++c) p.u_atmoColor[c] = edgeFogColor_[c] * 0.85f * nightDim;
            p.u_atmoColor[3] = waterElevation_;
        }
        if (void* m = frame.edgeUbo.map()) {
            std::memcpy(m, &p, sizeof(p));
            frame.edgeUbo.unmap();
        }
    }
    if (fog.oob) {
        FogOobParams p{};
        std::memcpy(p.invViewProj, inverseViewProj_, 16 * sizeof(float));
        p.u_fogColor[0] = oobFogColor_[0];
        p.u_fogColor[1] = oobFogColor_[1];
        p.u_fogColor[2] = oobFogColor_[2];
        p.u_fogOpacity  = oobFogOpacity_;
        p.u_horizonClampEnabled = fogHorizonClampEnabled_ ? 1 : 0;
        p.u_horizonFadeStartSin = fogHorizonFadeStartSin_;
        p.u_horizonFadeEndSin   = fogHorizonFadeEndSin_;
        p.u_time = SmokeMode::fixedTimestepEnabled()
                     ? static_cast<float>(SmokeMode::fixedClockSeconds())
                     : static_cast<float>(SDL_GetTicks()) / 1000.0f;
        if (void* m = frame.oobUbo.map()) {
            std::memcpy(m, &p, sizeof(p));
            frame.oobUbo.unmap();
        }
    }

    // ---- Shared resource ownership (VK-RUNG8 item-4: direct-chain only). Native fog
    // engages only when upstream left the shared GBuffer Vulkan-owned; otherwise decline
    // to the GL fog pass (native-or-decline). No GL<->VK semaphore handoff. ----
    if (!mc2vk::postchain::chain().open) vkResetCommandPool(s->device, frame.cpool.get(), 0);
    VkCommandBuffer cb = mc2vk::postchain::acquire(frame.cbuf, s->sDepthRes->image.get(), nullptr);
    if (!cb) { setFallback("record_failed"); s->disabled = true; return false; }
    mc2vk::postchain::addTicket(&frame.ticket);

    // Fog blends over the shadowed scene inside the chain's render pass; depth is already in the sampleable layout (read once for the chain).
    mc2vk::postchain::beginPass(s->rpass.get(), s->fb.get(), static_cast<uint32_t>(s->width), static_cast<uint32_t>(s->height));
    MC2_VK_GPU_SCOPE(gpuScope, cb, "VKIsl.Post.Fog");
    const auto fogDraws = fog.record([&](RenderCore::vulkan::PostprocessSubPassId pass) {
        const bool edge=pass==RenderCore::vulkan::PostprocessSubPassId::EdgeFog;
        const auto pipeline=edge ? s->edgePipe.get() : s->oobPipe.get();
        const auto descriptor=edge ? frame.edgeSet : frame.oobSet;
        mc2vk::infra::bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->playout.get(), 0, 1, &descriptor, 0, nullptr);
        vkCmdDraw(cb, 3, 1, 0, 0);
    });
    gpuScope.end();

    g_pf.renderUs = std::chrono::duration<double, std::micro>(clk::now() - t0).count();
    ++g_pf.usedVulkan;
    g_pf.glSkipped += fogDraws;
    return true;
}

void gosPostProcess::destroyVkPostFogNative()
{
    if (g_pf.runtimeGate) {
        vlog("[HEALTH] runtime_gate=%d substrate_live=%d attempted=%lu used_vulkan=%lu "
             "direct_chain=%d used_interop=%lu gl_skipped=%lu fallback_reason=\"%s\" "
             "render_us_last=%.1f wait_us_last=%.1f wait_us_avg=%.1f wait_samples=%lu",
             g_pf.runtimeGate, g_pf.substrateLive, g_pf.attempted, g_pf.usedVulkan,
             g_pf.directChain, g_pf.usedInterop, g_pf.glSkipped, g_pf.fallbackReason,
             g_pf.renderUs, g_pf.waitUsLast,
             g_pf.waitSamples ? g_pf.waitUsAccum / static_cast<double>(g_pf.waitSamples) : 0.0,
             g_pf.waitSamples);
    }
    if (!vkPostFogNative_) return;
    vkPostFogNative_.reset();
}

// Debug-state dump receipt (used_vulkan receipts for every VK capture).
extern "C" void mc2_vk_post_fog_native_health(
    int* runtimeGate, int* substrateLive,
    unsigned long* attempted, unsigned long* usedVulkan, unsigned long* glSkipped,
    const char** fallbackReason, double* renderUs)
{
    if (runtimeGate)    *runtimeGate = g_pf.runtimeGate;
    if (substrateLive)  *substrateLive = g_pf.substrateLive;
    if (attempted)      *attempted = g_pf.attempted;
    if (usedVulkan)     *usedVulkan = g_pf.usedVulkan;
    if (glSkipped)      *glSkipped = g_pf.glSkipped;
    if (fallbackReason) *fallbackReason = g_pf.fallbackReason;
    if (renderUs)       *renderUs = g_pf.renderUs;
}

