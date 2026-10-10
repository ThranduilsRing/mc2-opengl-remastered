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
// GATE: MC2_VK_POST_FOG_NATIVE — an umbrella member (mc2vkgate::armed<>, see the block above
// vkPostFogNativeEnabled): MC2_VK_SHARED_GBUFFER=1 arms it, explicit "1"/"0" wins. Runs ONLY when BOTH GL fog passes
// would run (sceneHasTerrain_, edgeFogEnabled_, fogOobEnabled_, mapHalfExtent_>0)
// AND the shared GBuffer substrate is live; ANY not-ready condition returns false
// and the caller falls through to the unchanged subgraph/island/GL precedence chain
// (fail-soft, never crashes, gate-OFF byte-identical).
//
// SYNC: per frame BeginFrame on BOTH shared resources. Guarded by the full upstream
// native ownership chain, color/depth are treated as already Vulkan-owned: there is no
// GL<->VK handoff, fog renders in place and leaves both resources Vulkan-owned for later
// post/composite work.
//
// The whole TU compiles to nothing unless MC2_VULKAN_ISLAND is defined.

#include "gos_postprocess.h"
#include "Gates/gate.h"
#include "Gates/gate_cold.h"   // str<>()

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
#include "vk_island_util.h"
#include "vulkan_context.h"
// Expose the VkImageResource getters (island-side halves).
#define MC2_VULKAN_SHARED_SCENE_COLOR_IMPL_ISLAND
#include "vulkan_shared_scene_color.h"
#define MC2_VULKAN_SHARED_SCENE_DEPTH_IMPL_ISLAND
#include "vulkan_shared_scene_depth.h"
#include "vk_shared_gbuffer_gate.h"

// Fixed-timestep clock so the animated OOB FBM time matches the GL path under smoke.
#include "gos_smoke.h"
#include <SDL2/SDL.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>
#include <vector>

namespace {

void vlog(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vk_island_util::vlogv512("[VK_POST_FOG_NATIVE] ", fmt, ap);
    va_end(ap);
}

// Health snapshot — read by debug_state_dump.cpp via the extern "C" getter below
// (used_vulkan receipt for every VK capture, per the arc's evidence protocol).
struct PostFogNativeHealth {
    int  runtimeGate   = 0;   // MC2_VK_POST_FOG_NATIVE resolved ON
    int  substrateLive = 0;   // shared color0 + depth resources present at init
    int  directChain   = 0;   // direct-chain engaged
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

std::string spv_dir() {
    std::string d = mc2::gate::str<"MC2_VULKAN_SPV_DIR">();   // row default shaders/vulkan, empty reads the default
    if (!d.empty() && d.back() != '/' && d.back() != '\\') d += '/';
    return d;
}

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
};
static_assert(sizeof(EdgeFogParams) == 112, "EdgeFogParams std140 offsets drifted");

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
    bool disabled = false;   // init failed once -> never retry, GL path forever
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

    // OWNED objects.
    VkImageView   depthSampleView = VK_NULL_HANDLE; // depth-aspect view over sDepthRes->image
    VkSampler     depthSampler = VK_NULL_HANDLE;    // NEAREST/CLAMP (matches GL fog sampling)
    VkBuffer      edgeUbo = VK_NULL_HANDLE; VmaAllocation edgeUboAlloc = VK_NULL_HANDLE;
    VkBuffer      oobUbo  = VK_NULL_HANDLE; VmaAllocation oobUboAlloc  = VK_NULL_HANDLE;
    VkRenderPass  rpass = VK_NULL_HANDLE;
    VkFramebuffer fb    = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet  edgeSet = VK_NULL_HANDLE;
    VkDescriptorSet  oobSet  = VK_NULL_HANDLE;
    VkPipelineLayout playout = VK_NULL_HANDLE;
    VkPipeline       edgePipe = VK_NULL_HANDLE;
    VkPipeline       oobPipe  = VK_NULL_HANDLE;
    VkShaderModule   edgeVert = VK_NULL_HANDLE, edgeFrag = VK_NULL_HANDLE;
    VkShaderModule   oobVert  = VK_NULL_HANDLE, oobFrag  = VK_NULL_HANDLE;
    VkCommandPool    cpool = VK_NULL_HANDLE;
    VkCommandBuffer  cbuf[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE}; // per parity slot
    VkFence          submitFence = VK_NULL_HANDLE; // VK-QWI-TO-FENCE-1: per-submit retire fence
};

static bool vkPostFogDirectChainEnabled();

namespace {

bool make_buffer(gosPostProcess::VkPostFogNative* s, VkDeviceSize size,
                 VkBufferUsageFlags usage, VkBuffer* buf, VmaAllocation* alloc) {
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size; bci.usage = usage;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    if (vmaCreateBuffer(s->allocator, &bci, &aci, buf, alloc, nullptr) != VK_SUCCESS) {
        vlog("vmaCreateBuffer failed."); return false;
    }
    return true;
}

void destroy_state(gosPostProcess::VkPostFogNative* s) {
    if (!s) return;
    VkDevice d = s->device;
    if (d != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(d);
        if (s->edgePipe) vkDestroyPipeline(d, s->edgePipe, nullptr);
        if (s->oobPipe)  vkDestroyPipeline(d, s->oobPipe, nullptr);
        if (s->playout)  vkDestroyPipelineLayout(d, s->playout, nullptr);
        if (s->edgeVert) vkDestroyShaderModule(d, s->edgeVert, nullptr);
        if (s->edgeFrag) vkDestroyShaderModule(d, s->edgeFrag, nullptr);
        if (s->oobVert)  vkDestroyShaderModule(d, s->oobVert, nullptr);
        if (s->oobFrag)  vkDestroyShaderModule(d, s->oobFrag, nullptr);
        if (s->pool)     vkDestroyDescriptorPool(d, s->pool, nullptr);
        if (s->dsl)      vkDestroyDescriptorSetLayout(d, s->dsl, nullptr);
        if (s->fb)       vkDestroyFramebuffer(d, s->fb, nullptr);
        if (s->rpass)    vkDestroyRenderPass(d, s->rpass, nullptr);
        if (s->depthSampler)    vkDestroySampler(d, s->depthSampler, nullptr);
        if (s->depthSampleView) vkDestroyImageView(d, s->depthSampleView, nullptr);
        if (s->submitFence) vkDestroyFence(d, s->submitFence, nullptr);   // VK-QWI-TO-FENCE-1
        if (s->cpool)    vkDestroyCommandPool(d, s->cpool, nullptr);
        if (s->allocator != VK_NULL_HANDLE) {
            if (s->edgeUbo) vmaDestroyBuffer(s->allocator, s->edgeUbo, s->edgeUboAlloc);
            if (s->oobUbo)  vmaDestroyBuffer(s->allocator, s->oobUbo,  s->oobUboAlloc);
        }
    }
    // Borrowed handles are NOT destroyed. Zero the owned ones.
    const bool disabled = s->disabled;
    *s = gosPostProcess::VkPostFogNative{};
    s->disabled = disabled;
}

bool build_state(gosPostProcess::VkPostFogNative* s, int w, int h) {
    s->width = w; s->height = h;

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
    if ((int)s->sColorRes->w != w || (int)s->sColorRes->h != h ||
        (int)s->sDepthRes->w != w || (int)s->sDepthRes->h != h) {
        setFallback("shared_gbuffer_size_mismatch"); return false;
    }
    g_pf.substrateLive = 1;

    VkResult r;

    // Depth-aspect sampling view over the SHARED depth image (sampling a depth
    // aspect returns depth in .r — identical values to the GL glGetTexImage float
    // conversion the subgraph uploaded, so the ported shader math is unchanged).
    {
        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = s->sDepthRes->image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = s->sDepthRes->format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        r = vkCreateImageView(s->device, &vci, nullptr, &s->depthSampleView);
        if (r != VK_SUCCESS) { vlog("depth sample view create failed (%d).", (int)r); return false; }
    }
    {
        VkSamplerCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter = VK_FILTER_NEAREST; sci.minFilter = VK_FILTER_NEAREST;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        r = vkCreateSampler(s->device, &sci, nullptr, &s->depthSampler);
        if (r != VK_SUCCESS) { vlog("vkCreateSampler failed (%d).", (int)r); return false; }
    }

    if (!make_buffer(s, sizeof(EdgeFogParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &s->edgeUbo, &s->edgeUboAlloc)) return false;
    if (!make_buffer(s, sizeof(FogOobParams),  VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &s->oobUbo,  &s->oobUboAlloc))  return false;

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
        r = vkCreateRenderPass(s->device, &rpci, nullptr, &s->rpass);
        if (r != VK_SUCCESS) { vlog("vkCreateRenderPass failed (%d).", (int)r); return false; }

        VkFramebufferCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = s->rpass;
        fci.attachmentCount = 1; fci.pAttachments = &s->sColorRes->view;
        fci.width = (uint32_t)w; fci.height = (uint32_t)h; fci.layers = 1;
        r = vkCreateFramebuffer(s->device, &fci, nullptr, &s->fb);
        if (r != VK_SUCCESS) { vlog("vkCreateFramebuffer failed (%d).", (int)r); return false; }
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
        r = vkCreateDescriptorSetLayout(s->device, &lci, nullptr, &s->dsl);
        if (r != VK_SUCCESS) { vlog("vkCreateDescriptorSetLayout failed (%d).", (int)r); return false; }
        VkDescriptorPoolSize sizes[2]{};
        sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; sizes[0].descriptorCount = 2;
        sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;         sizes[1].descriptorCount = 2;
        VkDescriptorPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.maxSets = 2; pci.poolSizeCount = 2; pci.pPoolSizes = sizes;
        r = vkCreateDescriptorPool(s->device, &pci, nullptr, &s->pool);
        if (r != VK_SUCCESS) { vlog("vkCreateDescriptorPool failed (%d).", (int)r); return false; }
        VkDescriptorSetLayout layouts[2] = { s->dsl, s->dsl };
        VkDescriptorSet setsOut[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = s->pool; dsai.descriptorSetCount = 2; dsai.pSetLayouts = layouts;
        r = vkAllocateDescriptorSets(s->device, &dsai, setsOut);
        if (r != VK_SUCCESS) { vlog("vkAllocateDescriptorSets failed (%d).", (int)r); return false; }
        s->edgeSet = setsOut[0]; s->oobSet = setsOut[1];

        VkDescriptorImageInfo imgInfo{};
        imgInfo.sampler = s->depthSampler;
        imgInfo.imageView = s->depthSampleView;
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorBufferInfo edgeUboInfo{ s->edgeUbo, 0, VK_WHOLE_SIZE };
        VkDescriptorBufferInfo oobUboInfo { s->oobUbo,  0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet writes[4]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = s->edgeSet; writes[0].dstBinding = 0; writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &imgInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = s->edgeSet; writes[1].dstBinding = 1; writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[1].pBufferInfo = &edgeUboInfo;
        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = s->oobSet; writes[2].dstBinding = 0; writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[2].pImageInfo = &imgInfo;
        writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[3].dstSet = s->oobSet; writes[3].dstBinding = 1; writes[3].descriptorCount = 1;
        writes[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[3].pBufferInfo = &oobUboInfo;
        vkUpdateDescriptorSets(s->device, 4, writes, 0, nullptr);
    }

    // Two pipelines — fixed-function state IDENTICAL to the subgraph's (fullscreen
    // tri, no vertex input, cull none, AlphaBlend SRC_ALPHA/ONE_MINUS_SRC_ALPHA,
    // no depth attachment). Shaders reused verbatim.
    {
        s->edgeVert = load_spv(s->device, spv_dir() + "edge_fog.vert.spv");
        s->edgeFrag = load_spv(s->device, spv_dir() + "edge_fog.frag.spv");
        s->oobVert  = load_spv(s->device, spv_dir() + "fog_oob.vert.spv");
        s->oobFrag  = load_spv(s->device, spv_dir() + "fog_oob.frag.spv");
        if (s->edgeVert == VK_NULL_HANDLE || s->edgeFrag == VK_NULL_HANDLE ||
            s->oobVert  == VK_NULL_HANDLE || s->oobFrag  == VK_NULL_HANDLE) {
            setFallback("spv_load_failed"); return false;
        }
        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 1; plci.pSetLayouts = &s->dsl;
        r = vkCreatePipelineLayout(s->device, &plci, nullptr, &s->playout);
        if (r != VK_SUCCESS) { vlog("vkCreatePipelineLayout failed (%d).", (int)r); return false; }

        VkPipelineVertexInputStateCreateInfo vin{};
        vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkViewport vp{0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f};
        VkRect2D sc{{0, 0}, {(uint32_t)w, (uint32_t)h}};
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

        auto make_pipeline = [&](VkShaderModule vmod, VkShaderModule fmod, VkPipeline* out) -> bool {
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
            gp.layout = s->playout;
            gp.renderPass = s->rpass;
            gp.subpass = 0;
            if (vkCreateGraphicsPipelines(s->device, VK_NULL_HANDLE, 1, &gp, nullptr, out) != VK_SUCCESS) {
                vlog("vkCreateGraphicsPipelines failed."); return false;
            }
            return true;
        };
        if (!make_pipeline(s->edgeVert, s->edgeFrag, &s->edgePipe)) return false;
        if (!make_pipeline(s->oobVert,  s->oobFrag,  &s->oobPipe))  return false;
    }

    {
        VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = s->gfxFamily;
        r = vkCreateCommandPool(s->device, &pci, nullptr, &s->cpool);
        if (r != VK_SUCCESS) { vlog("vkCreateCommandPool failed (%d).", (int)r); return false; }
        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = s->cpool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 2;
        r = vkAllocateCommandBuffers(s->device, &cai, s->cbuf);
        if (r != VK_SUCCESS) { vlog("vkAllocateCommandBuffers failed (%d).", (int)r); return false; }
    }

    s->inited = true;
    setFallback("");
    vlog("initialized %dx%d — NATIVE fog pair on the shared engine context "
         "(samples shared scene depth, blends into shared scene color0 in place; "
         "ZERO copies, ZERO readbacks, no private device).", w, h);
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// vkPostFogNativeEnabled(): UMBRELLA-ARMED (mc2vkgate::armed<>), NOT an opt-in.
//
// GATE-PARITY-CLOSE-1: this block used to read "OPT-IN, default-OFF (new-rung
// pattern — unset -> OFF regardless of MC2_VK_SHARED_GBUFFER, explicit 0 forces
// OFF)". That is the opposite of what the line below does, and had been since
// VK-FIELD-ENV-CONTENT-STACK-ARM-1 made it an umbrella member.
// mc2vkgate::armed<>() (vk_shared_gbuffer_gate.h) resolves explicit "1"/"0"
// first and otherwise returns umbrellaOn() — so unset does NOT mean OFF, it
// means "whatever MC2_VK_SHARED_GBUFFER says", which is the precise clause the
// old comment denied. Actual resolution:
//   MC2_VK_POST_FOG_NATIVE=1        -> ON   (explicit wins)
//   MC2_VK_POST_FOG_NATIVE=0        -> OFF  (explicit wins)
//   unset + MC2_VK_SHARED_GBUFFER=1 -> ON   <-- the arm the old comment denied
//   unset + umbrella unset/!=1      -> OFF  (default launch; also OFF once
//                                            g_mc2VkSessionForcedGL is set)
// Consequence for anyone reading this to size a change: the SKYBOX-FOG-EXCLUDE
// parity guard below is reachable on an unset MC2_VK_POST_FOG_NATIVE, so it is
// load-bearing under the umbrella and not just under an explicit debug flag.
// ---------------------------------------------------------------------------
bool gosPostProcess::vkPostFogNativeEnabled()
{
    static int state = -1;
    if (state == -1) {
        // VK-FIELD-ENV-CONTENT-STACK-ARM-1 (2026-07-11): now an umbrella family member
        // (armed<>). Post-VK-RUNG8-ITEM8 the GL fog pass this used to fail-soft to draws
        // into the DISCARDED GL backbuffer under VK-only present, so an unarmed native fog
        // means the scene atmospheric fog VANISHES (the user's "OOB fog missing"). Native
        // fog still engages only when upstream is direct-chain (postDirectChainUpstreamOn),
        // so umbrella-arming it is safe: a degenerate forced-hybrid config still declines.
        // The strict proof armed MC2_VK_POST_FOG_NATIVE=1 and passed. Explicit "1"/"0" wins.
        state = mc2vkgate::armed<"MC2_VK_POST_FOG_NATIVE">() ? 1 : 0;
        g_pf.runtimeGate = state;
        if (state == 1)
            vlog("MC2_VK_POST_FOG_NATIVE — rung-8.1 native fog pair gate ON "
                 "(umbrella-armed; lazy init at the fog site; fail-soft to subgraph/island/GL).");
    }
    return state == 1;
}

// Native fog runs only in direct-chain, engaged when the upstream scene producers left the
// shared GBuffer Vulkan-owned (postDirectChainUpstreamOn); otherwise it declines and the
// GL fog pass runs.
static bool vkPostFogDirectChainEnabled()
{
    static int state = -1;
    if (state == -1) {
        state = mc2vkgate::postDirectChainUpstreamOn() ? 1 : 0;
        g_pf.directChain = state;
        if (state == 1)
            vlog("native fog direct-chain: upstream left the shared GBuffer VK-owned; "
                 "no fog GL<->VK handoff.");
    }
    return state == 1;
}

// ---------------------------------------------------------------------------
// runVkPostFogNative(): render BOTH fog effects natively in VK. Returns true only
// when both draws were submitted (caller skips BOTH GL fog sites); ANY not-ready
// condition returns false and the unchanged precedence chain runs.
// ---------------------------------------------------------------------------
bool gosPostProcess::runVkPostFogNative()
{
    // Run ONLY when BOTH GL fog passes would run (parity guard — if either GL pass
    // would self-skip, fall through so per-pass behavior is preserved exactly).
    if (!sceneHasTerrain_ || !edgeFogEnabled_ || !fogOobEnabled_ ||
        mapHalfExtent_ <= 0.0f)
        return false;
    // SKYBOX-FOG-EXCLUDE-1 parity guard: the VK fog shaders have NO stencil
    // sky-exclusion path (usampler2D stencilTex + u_skyExcludeEnabled exist only
    // in the GL frags). When that gate is ON the GL passes diverge from the VK
    // port on sky pixels -> fall through to GL (same limitation as the subgraph,
    // now made explicit).
    //
    // SKYBOX-FOG-EXCLUDE-SINGLE-READER-1: this used to be a PRIVATE static reading
    // `v && v[0] == '1'` (default OFF) while gos_postprocess.cpp's
    // skyboxFogExcludeEnabled() reads `!(v && v[0] == '0')` (default ON, flipped by
    // SKYBOX-FOG-EXCLUDE-DEFAULT-ON-1). Same variable, two readers, opposite unset
    // defaults -- so on an unset environment this guard silently did NOT fire and
    // the native VK fog painted over the exact sky pixels the GL passes exclude:
    // the divergence the guard exists to prevent. GL is the authority; there is now
    // one reader for this gate and both the region-iface guard
    // (gos_postprocess.cpp runFogRegionVulkanBackend) and this one call it.
    //
    // VK-PARITY-F3: this decline made native fog NEVER run under the default gates, and
    // the GL fallback pass writes the discarded GL scene buffer under the VK umbrella, so
    // VK had NO distance/horizon/OOB fog at all (VK-PARITY-GAPS #8). The stencil exclusion
    // only ever tags sky with elevation > 0.22 (hdri_skybox_stencil_tag.frag), and with
    // FOG-HORIZON-CLAMP-1 (default ON, ported into the VK fog shaders) both fog passes are
    // already exactly zero above fogHorizonFadeEnd (5 deg), so the exclusion is a no-op and
    // the stencil path is not needed. Only the clamp-OFF legacy band (fog survives to ~12.7
    // deg, where stencil exclusion would matter) still declines.
    if (gos_SkyboxFogExcludeEnabled() && !fogHorizonClampEnabled_) return false;

    if (width_ <= 0 || height_ <= 0) return false;

    if (!vkPostFogNative_) vkPostFogNative_ = new VkPostFogNative();
    VkPostFogNative* s = vkPostFogNative_;
    if (s->disabled) return false;
    if (!s->inited || s->width != width_ || s->height != height_) {
        if (s->inited) destroy_state(s);
        if (!build_state(s, width_, height_)) {
            // Substrate not ready (e.g. shared GBuffer gates off) is NOT a permanent
            // failure UNLESS a real device/object create failed. Keep it simple and
            // conservative: disable for the process on any init failure (matches the
            // subgraph convention); the GL chain runs for the rest of the run.
            vlog("init FAILED (reason=%s) — disabling for this process, GL fog chain runs.",
                 g_pf.fallbackReason[0] ? g_pf.fallbackReason : "unknown");
            destroy_state(s);
            s->disabled = true;
            return false;
        }
    }

    ++g_pf.attempted;
    using clk = std::chrono::steady_clock;
    const clk::time_point t0 = clk::now();

    // ---- UBOs (values IDENTICAL to the GL runEdgeFog()/runFogOob() uploads). ----
    {
        EdgeFogParams p{};
        std::memcpy(p.invViewProj, inverseViewProj_, 16 * sizeof(float));
        p.u_fogColor[0] = edgeFogColor_[0];
        p.u_fogColor[1] = edgeFogColor_[1];
        p.u_fogColor[2] = edgeFogColor_[2];
        p._pad0 = 0.0f;
        p.u_halfExtent = mapHalfExtent_;
        p.u_fogStart = edgeFogStart_;
        p.u_fogHeight = edgeFogHeight_;
        p.u_fogMax = edgeFogMax_;
        p.u_waterElevation = waterElevation_;
        p.u_horizonClampEnabled = fogHorizonClampEnabled_ ? 1 : 0;
        p.u_horizonFadeStartSin = fogHorizonFadeStartSin_;
        p.u_horizonFadeEndSin   = fogHorizonFadeEndSin_;
        void* m = nullptr;
        if (vmaMapMemory(s->allocator, s->edgeUboAlloc, &m) == VK_SUCCESS) {
            std::memcpy(m, &p, sizeof(p));
            vmaUnmapMemory(s->allocator, s->edgeUboAlloc);
        }
    }
    {
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
                     ? (float)SmokeMode::fixedClockSeconds()
                     : (float)SDL_GetTicks() / 1000.0f;
        void* m = nullptr;
        if (vmaMapMemory(s->allocator, s->oobUboAlloc, &m) == VK_SUCCESS) {
            std::memcpy(m, &p, sizeof(p));
            vmaUnmapMemory(s->allocator, s->oobUboAlloc);
        }
    }

    // ---- Shared resource ownership (VK-RUNG8 item-4: direct-chain only). Native fog
    // engages only when upstream left the shared GBuffer Vulkan-owned; otherwise decline
    // to the GL fog pass (native-or-decline). No GL<->VK semaphore handoff. ----
    if (!vkPostFogDirectChainEnabled()) { setFallback("upstream_not_direct_chain"); return false; }
    VkCommandBuffer cb = s->cbuf[0u];

    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) {
        setFallback("record_failed"); s->disabled = true; return false;
    }

    // Depth: DEPTH_STENCIL_ATTACHMENT (GL hand-off layout) -> SHADER_READ_ONLY for
    // sampling; BOTH aspects in the range (combined depth/stencil layout rules).
    // Memory visibility of GL's depth writes rides the GL->VK semaphore in
    // handoff mode; direct-chain mode inherits Vulkan ownership from upstream.
    auto depthBarrier = [&](VkImageLayout oldL, VkImageLayout newL,
                            VkAccessFlags srcA, VkAccessFlags dstA,
                            VkPipelineStageFlags srcS, VkPipelineStageFlags dstS) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = oldL; b.newLayout = newL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = s->sDepthRes->image;
        b.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
        b.srcAccessMask = srcA; b.dstAccessMask = dstA;
        vkCmdPipelineBarrier(cb, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    depthBarrier(VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                 VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    VkRenderPassBeginInfo rbi{};
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = s->rpass; rbi.framebuffer = s->fb;
    rbi.renderArea = {{0, 0}, {(uint32_t)s->width, (uint32_t)s->height}};
    rbi.clearValueCount = 0; // LOAD, no clear
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->edgePipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->playout, 0, 1, &s->edgeSet, 0, nullptr);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->oobPipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->playout, 0, 1, &s->oobSet, 0, nullptr);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRenderPass(cb);

    // Depth back to the GL hand-off layout (layout-stable across the boundary).
    depthBarrier(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                 VK_ACCESS_SHADER_READ_BIT, 0,
                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

    if (vkEndCommandBuffer(cb) != VK_SUCCESS) {
        setFallback("record_failed"); s->disabled = true; return false;
    }

    // ---- Submit (VK-RUNG8 item-4: direct-chain only — no GL<->VK semaphores). The
    // images stay Vulkan-owned for the following native post/composite pass. ----
    // VK-QWI-TO-FENCE-1: retire THIS submit via a dedicated per-submit fence instead
    // of a queue-wide vkQueueWaitIdle drain. The slot-recycle guarantee only needs
    // this pass's own cbuf retired before reuse next frame; the fence preserves that
    // exactly (a vkQueueSubmit fence signal is also ordered after all earlier
    // submission-order work on the queue, so the wait is never WEAKER than the drain
    // it replaces). Fail-soft: if fence creation ever fails, keep the queue drain.
    if (s->submitFence == VK_NULL_HANDLE) {
        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vkCreateFence(s->device, &fci, nullptr, &s->submitFence) != VK_SUCCESS)
            s->submitFence = VK_NULL_HANDLE;
    }
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    if (vkQueueSubmit(s->queue, 1, &si, s->submitFence) != VK_SUCCESS) {
        setFallback("submit_failed"); s->disabled = true; return false;
    }

    // Slot-recycle guard: this frame's command buffer must be retired before reuse
    // next frame (per-submit fence; QWI only as the fence-creation fail-soft).
    {
        const clk::time_point tWait0 = clk::now();
        if (s->submitFence != VK_NULL_HANDLE) {
            if (vkWaitForFences(s->device, 1, &s->submitFence, VK_TRUE,
                                5ull * 1000 * 1000 * 1000) != VK_SUCCESS) {
                setFallback("fence_timeout"); s->disabled = true; return false;
            }
            vkResetFences(s->device, 1, &s->submitFence);
        } else if (vkQueueWaitIdle(s->queue) != VK_SUCCESS) {
            setFallback("queue_wait_idle_failed"); s->disabled = true; return false;
        }
        g_pf.waitUsLast = std::chrono::duration<double, std::micro>(clk::now() - tWait0).count();
        g_pf.waitUsAccum += g_pf.waitUsLast;
        ++g_pf.waitSamples;
        if ((g_pf.waitSamples % 600) == 1)
            vlog("[QWI-WAIT] wait_us_last=%.1f wait_us_avg=%.1f samples=%lu",
                 g_pf.waitUsLast,
                 g_pf.waitUsAccum / (double)g_pf.waitSamples, g_pf.waitSamples);
    }

    g_pf.renderUs = std::chrono::duration<double, std::micro>(clk::now() - t0).count();
    ++g_pf.usedVulkan;
    g_pf.glSkipped += 2;
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
             g_pf.waitSamples ? g_pf.waitUsAccum / (double)g_pf.waitSamples : 0.0,
             g_pf.waitSamples);
    }
    if (!vkPostFogNative_) return;
    destroy_state(vkPostFogNative_);
    delete vkPostFogNative_;
    vkPostFogNative_ = nullptr;
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

#endif // MC2_VULKAN_ISLAND
