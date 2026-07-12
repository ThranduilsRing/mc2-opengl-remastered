// vulkan_vfx_island.cpp — VK-FIELD-VFX-1.
//
// Renders the GPU-particle billboard stream into the SHARED scene GBuffer color0
// as translucent, depth-tested (reverse-Z GEQUAL, NO depth write) colored soft
// billboards, so weapon fire (tracers / muzzle flashes / impact glows) survives
// the MC2_VK_SHARED_GBUFFER umbrella. Structurally a thin water island: it borrows
// the shared engine VK device + the same shared color0/normal/depth VkImageResources
// the terrain/water islands co-write, LOADs them, blends the particle billboards on
// top honoring the shared depth, and STOREs — no offscreen target, no composite, no
// GL<->VK interop.
//
// SCOPE (honest): UNTEXTURED — per-particle color + a radial soft falloff (the FS
// approximates a generic soft sprite). Two blend pipelines (alpha + additive) mirror
// the GL particle bridge's per-group blend. DEFERRED remainder (documented): textured
// sprite atlases, the tube-ribbon beam path (gos_tube_ribbon_flush_deferred), and the
// gosFX MESH shapes (gos_vfx_mesh_flush). The whole TU compiles to nothing unless
// MC2_VULKAN_ISLAND is defined (same discipline as the other island TUs).

#ifdef MC2_VULKAN_ISLAND

#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <volk.h>

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include <vk_mem_alloc.h>

#include "vk_image_resource.h"
#include "vulkan_context.h"
#define MC2_VULKAN_SHARED_SCENE_COLOR_IMPL_ISLAND
#include "vulkan_shared_scene_color.h"
#define MC2_VULKAN_SHARED_SCENE_DEPTH_IMPL_ISLAND
#include "vulkan_shared_scene_depth.h"
#include "vk_shared_gbuffer_gate.h"
#include "vulkan_vfx_island.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cstdint>
#include <string>
#include <vector>

namespace {

// ── Health snapshot (mirrors the water island's; stderr summary at destroy) ──
struct VfxIslandHealth {
    int   vulkanAvailable = 0;
    int   buildEnabled    = 1;
    char  deviceName[256] = {0};
    unsigned long attempted   = 0;
    unsigned long usedVulkan  = 0;
    char  fallbackReason[256] = {0};
    unsigned long vkDraws     = 0;   // per-group draws issued (last frame)
    unsigned long particles   = 0;   // particles uploaded (last frame)
    unsigned long groups      = 0;
};
VfxIslandHealth g_health;

void setFallback(const char* reason) {
    std::strncpy(g_health.fallbackReason, reason ? reason : "",
                 sizeof(g_health.fallbackReason) - 1);
    g_health.fallbackReason[sizeof(g_health.fallbackReason) - 1] = '\0';
}

void vlog(const char* fmt, ...) {
    std::fprintf(stderr, "[VK_VFX] ");
    va_list ap; va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
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

// std140 UBO mirror — LOCKSTEP with vfx_particle.vert (VfxVsParams).
struct VfxVsParams {
    float worldToClipGL[16];  // column_major; SAME raw 16 floats as GL GL_FALSE upload
    float camRight[4];        // xyz + pad
    float camUp[4];           // xyz + pad
};

struct VfxIsland {
    VkInstance       instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys     = VK_NULL_HANDLE;
    VkDevice         device   = VK_NULL_HANDLE;
    VkQueue          queue    = VK_NULL_HANDLE;
    uint32_t         gfxFamily = 0;
    VmaAllocator     allocator = VK_NULL_HANDLE;

    VkImageResource* sColorRes  = nullptr;
    VkImageResource* sNormalRes = nullptr;
    VkImageResource* sDepthRes  = nullptr;

    VkRenderPass     rpass  = VK_NULL_HANDLE;
    VkFramebuffer    fb     = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkDescriptorPool pool  = VK_NULL_HANDLE;
    VkDescriptorSet  set   = VK_NULL_HANDLE;
    VkPipelineLayout playout = VK_NULL_HANDLE;
    VkPipeline       alphaPipe = VK_NULL_HANDLE;
    VkPipeline       addPipe   = VK_NULL_HANDLE;
    VkShaderModule   vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;

    VkBuffer         ubo = VK_NULL_HANDLE;      VmaAllocation uboAlloc = VK_NULL_HANDLE;
    VkBuffer         ssbo = VK_NULL_HANDLE;     VmaAllocation ssboAlloc = VK_NULL_HANDLE;
    VkDeviceSize     ssboBytes = 0;

    VkCommandPool    cpool = VK_NULL_HANDLE;
    VkCommandBuffer  cbuf  = VK_NULL_HANDLE;
    VkFence          fence = VK_NULL_HANDLE;

    int  w = 0, h = 0;
    bool inited = false;
    bool disabled = false;
};
VfxIsland* g_island = nullptr;

bool make_buffer(VfxIsland* s, VkDeviceSize size, VkBufferUsageFlags usage,
                 VkBuffer* buf, VmaAllocation* alloc) {
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size; bci.usage = usage;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    if (vmaCreateBuffer(s->allocator, &bci, &aci, buf, alloc, nullptr) != VK_SUCCESS) {
        vlog("vmaCreateBuffer failed (size=%llu).", (unsigned long long)size); return false;
    }
    return true;
}

void write_descriptors(VfxIsland* s) {
    if (s->set == VK_NULL_HANDLE) return;
    VkDescriptorBufferInfo uboInfo{ s->ubo,  0, VK_WHOLE_SIZE };
    VkDescriptorBufferInfo ssbInfo{ s->ssbo, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet w[2]{};
    w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstSet = s->set;
    w[0].dstBinding = 0; w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[0].pBufferInfo = &uboInfo;
    w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[1].dstSet = s->set;
    w[1].dstBinding = 1; w[1].descriptorCount = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[1].pBufferInfo = &ssbInfo;
    vkUpdateDescriptorSets(s->device, 2, w, 0, nullptr);
}

// Grow the particle SSBO to at least `bytes`; re-point the descriptor. Never shrinks.
bool ensure_ssbo(VfxIsland* s, VkDeviceSize bytes) {
    if (bytes == 0) bytes = 64;
    if (bytes <= s->ssboBytes && s->ssbo != VK_NULL_HANDLE) return true;
    vkDeviceWaitIdle(s->device);
    if (s->ssbo != VK_NULL_HANDLE) vmaDestroyBuffer(s->allocator, s->ssbo, s->ssboAlloc);
    s->ssbo = VK_NULL_HANDLE; s->ssboAlloc = VK_NULL_HANDLE; s->ssboBytes = 0;
    VkDeviceSize cap = bytes + bytes / 2;   // 1.5x headroom
    if (!make_buffer(s, cap, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &s->ssbo, &s->ssboAlloc)) return false;
    s->ssboBytes = cap;
    write_descriptors(s);
    return true;
}

VkPipeline make_pipeline(VfxIsland* s, int w, int h, bool additive) {
    VkPipelineVertexInputStateCreateInfo vin{};
    vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;  // procedural
    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;   // double-sided
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    // reverse-Z GEQUAL depth TEST, NO write (translucent) — matches the GL particle bridge.
    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
    // color0: straight-alpha (FS emits straight rgb,a; src-alpha factor premults in HW).
    //   alpha    = SRC_ALPHA / ONE_MINUS_SRC_ALPHA  (GL alpha groups)
    //   additive = SRC_ALPHA / ONE                  (GL additive groups)
    VkPipelineColorBlendAttachmentState cba[2]{};
    cba[0].blendEnable = VK_TRUE;
    cba[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cba[0].dstColorBlendFactor = additive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba[0].colorBlendOp = VK_BLEND_OP_ADD;
    cba[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cba[0].dstAlphaBlendFactor = additive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba[0].alphaBlendOp = VK_BLEND_OP_ADD;
    cba[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    cba[1].blendEnable = VK_FALSE;   // shared normal target left untouched
    cba[1].colorWriteMask = 0;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 2; cb.pAttachments = cba;
    VkViewport vp{0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {(uint32_t)w, (uint32_t)h}};
    VkPipelineViewportStateCreateInfo vps{};
    vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1; vps.pViewports = &vp; vps.scissorCount = 1; vps.pScissors = &scissor;
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
    gp.layout = s->playout; gp.renderPass = s->rpass; gp.subpass = 0;
    VkPipeline pipe = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(s->device, VK_NULL_HANDLE, 1, &gp, nullptr, &pipe) != VK_SUCCESS) {
        vlog("vkCreateGraphicsPipelines(%s) failed.", additive ? "additive" : "alpha");
        return VK_NULL_HANDLE;
    }
    return pipe;
}

void destroy_island(VfxIsland* s) {
    if (!s) return;
    if (s->device) vkDeviceWaitIdle(s->device);
    if (s->fence)     vkDestroyFence(s->device, s->fence, nullptr);
    if (s->cpool)     vkDestroyCommandPool(s->device, s->cpool, nullptr);
    if (s->alphaPipe) vkDestroyPipeline(s->device, s->alphaPipe, nullptr);
    if (s->addPipe)   vkDestroyPipeline(s->device, s->addPipe, nullptr);
    if (s->playout)   vkDestroyPipelineLayout(s->device, s->playout, nullptr);
    if (s->vert)      vkDestroyShaderModule(s->device, s->vert, nullptr);
    if (s->frag)      vkDestroyShaderModule(s->device, s->frag, nullptr);
    if (s->pool)      vkDestroyDescriptorPool(s->device, s->pool, nullptr);
    if (s->dsl)       vkDestroyDescriptorSetLayout(s->device, s->dsl, nullptr);
    if (s->fb)        vkDestroyFramebuffer(s->device, s->fb, nullptr);
    if (s->rpass)     vkDestroyRenderPass(s->device, s->rpass, nullptr);
    if (s->ubo)       vmaDestroyBuffer(s->allocator, s->ubo, s->uboAlloc);
    if (s->ssbo)      vmaDestroyBuffer(s->allocator, s->ssbo, s->ssboAlloc);
    *s = VfxIsland{};
}

bool init_island(VfxIsland* s, int w, int h) {
    s->w = w; s->h = h;
    ++g_health.attempted;

    if (!mc2vk::ensureEngineVulkanContext()) { setFallback("engine_vk_context_birth_failed"); return false; }
    mc2vk::VulkanContext& ctx = mc2vk::getOrCreateEngineVulkanContext();
    if (!ctx.valid()) { setFallback("engine_vk_context_not_valid"); return false; }
    s->instance = ctx.instance(); s->phys = ctx.physicalDevice(); s->device = ctx.device();
    s->queue = ctx.graphicsQueue(); s->gfxFamily = ctx.graphicsFamily(); s->allocator = ctx.allocator();
    if (s->device == VK_NULL_HANDLE || s->allocator == VK_NULL_HANDLE) {
        setFallback("engine_vk_device_or_allocator_null"); return false;
    }
    g_health.vulkanAvailable = 1;
    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(s->phys, &props);
        std::strncpy(g_health.deviceName, props.deviceName, sizeof(g_health.deviceName) - 1);
    }

    // Borrow the shared scene GBuffer (color0 + normal + depth). Not ready => decline.
    s->sColorRes  = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_COLOR0);
    s->sNormalRes = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_NORMAL);
    s->sDepthRes  = mc2SharedSceneDepthGetResource();
    if (!s->sColorRes || !s->sNormalRes || !s->sDepthRes) { setFallback("shared_gbuffer_not_ready"); return false; }
    // Not-ready guard: struct present but view/format not yet valid (boot warmup race).
    if (s->sColorRes->view == VK_NULL_HANDLE || s->sColorRes->format == VK_FORMAT_UNDEFINED ||
        s->sNormalRes->view == VK_NULL_HANDLE || s->sDepthRes->view == VK_NULL_HANDLE ||
        s->sDepthRes->format == VK_FORMAT_UNDEFINED) {
        setFallback("shared_gbuffer_views_not_ready"); return false;
    }

    // UBO + initial particle SSBO.
    if (!make_buffer(s, sizeof(VfxVsParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &s->ubo, &s->uboAlloc)) return false;
    if (!ensure_ssbo(s, 64 * 1024)) return false;   // ~1024 particles to start

    // Render pass: color0 (LOAD, blend, STORE) + normal (LOAD, STORE, writemask 0) +
    // depth (LOAD, GEQUAL test, NO write, STORE unchanged).
    {
        VkAttachmentDescription att[3]{};
        att[0].format = s->sColorRes->format; att[0].samples = VK_SAMPLE_COUNT_1_BIT;
        att[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att[0].finalLayout   = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att[1].format = s->sNormalRes->format; att[1].samples = VK_SAMPLE_COUNT_1_BIT;
        att[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[1].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att[1].finalLayout   = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att[2].format = s->sDepthRes->format; att[2].samples = VK_SAMPLE_COUNT_1_BIT;
        att[2].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[2].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        att[2].finalLayout   = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference colorRefs[2]{
            {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
            {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
        VkAttachmentReference depthRef{2, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 2; sub.pColorAttachments = colorRefs;
        sub.pDepthStencilAttachment = &depthRef;
        // Incoming external dep — order this frame's blend AFTER the upstream opaque/water
        // writes to the shared color+depth (same WAW pattern as the water island). Depth is
        // READ only here (no write), so dst depth access is READ.
        VkSubpassDependency deps[2]{};
        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL; deps[0].dstSubpass = 0;
        deps[0].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        deps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        deps[1].srcSubpass = 0; deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstStageMask  = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        deps[1].dstAccessMask = 0;
        VkRenderPassCreateInfo rpci{};
        rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpci.attachmentCount = 3; rpci.pAttachments = att;
        rpci.subpassCount = 1; rpci.pSubpasses = &sub;
        rpci.dependencyCount = 2; rpci.pDependencies = deps;
        if (vkCreateRenderPass(s->device, &rpci, nullptr, &s->rpass) != VK_SUCCESS) { vlog("vkCreateRenderPass failed."); return false; }
        VkImageView fbViews[3] = { s->sColorRes->view, s->sNormalRes->view, s->sDepthRes->view };
        VkFramebufferCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = s->rpass; fci.attachmentCount = 3; fci.pAttachments = fbViews;
        fci.width = (uint32_t)w; fci.height = (uint32_t)h; fci.layers = 1;
        if (vkCreateFramebuffer(s->device, &fci, nullptr, &s->fb) != VK_SUCCESS) { vlog("vkCreateFramebuffer failed."); return false; }
    }

    // Descriptor set: UBO(0, vert) + particle SSBO(1, vert).
    {
        VkDescriptorSetLayoutBinding b[2]{};
        b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.bindingCount = 2; lci.pBindings = b;
        if (vkCreateDescriptorSetLayout(s->device, &lci, nullptr, &s->dsl) != VK_SUCCESS) { vlog("vkCreateDescriptorSetLayout failed."); return false; }
        VkDescriptorPoolSize sizes[2]{};
        sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; sizes[0].descriptorCount = 1;
        sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; sizes[1].descriptorCount = 1;
        VkDescriptorPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.maxSets = 1; pci.poolSizeCount = 2; pci.pPoolSizes = sizes;
        if (vkCreateDescriptorPool(s->device, &pci, nullptr, &s->pool) != VK_SUCCESS) { vlog("vkCreateDescriptorPool failed."); return false; }
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = s->pool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &s->dsl;
        if (vkAllocateDescriptorSets(s->device, &dsai, &s->set) != VK_SUCCESS) { vlog("vkAllocateDescriptorSets failed."); return false; }
        write_descriptors(s);
    }

    // Pipelines (alpha + additive).
    {
        s->vert = load_spv(s->device, spv_dir() + "vfx_particle.vert.spv");
        s->frag = load_spv(s->device, spv_dir() + "vfx_particle.frag.spv");
        if (s->vert == VK_NULL_HANDLE || s->frag == VK_NULL_HANDLE) { setFallback("spv_load_failed"); return false; }
        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 1; plci.pSetLayouts = &s->dsl;
        if (vkCreatePipelineLayout(s->device, &plci, nullptr, &s->playout) != VK_SUCCESS) { vlog("vkCreatePipelineLayout failed."); return false; }
        s->alphaPipe = make_pipeline(s, w, h, /*additive=*/false);
        s->addPipe   = make_pipeline(s, w, h, /*additive=*/true);
        if (s->alphaPipe == VK_NULL_HANDLE || s->addPipe == VK_NULL_HANDLE) { setFallback("pipeline_create_failed"); return false; }
    }

    // Command pool + one command buffer + fence (inline-wait each frame).
    {
        VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = s->gfxFamily;
        if (vkCreateCommandPool(s->device, &pci, nullptr, &s->cpool) != VK_SUCCESS) { vlog("vkCreateCommandPool failed."); return false; }
        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = s->cpool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(s->device, &cai, &s->cbuf) != VK_SUCCESS) { vlog("vkAllocateCommandBuffers failed."); return false; }
        VkFenceCreateInfo fnci{}; fnci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vkCreateFence(s->device, &fnci, nullptr, &s->fence) != VK_SUCCESS) { vlog("vkCreateFence failed."); return false; }
    }

    s->inited = true;
    setFallback("");
    vlog("island initialized %dx%d (shared scene GBuffer co-write; procedural billboard VS; "
         "GEQUAL reverse-Z depth-test no-write; alpha+additive blend; untextured).", w, h);
    return true;
}

// Gate: default-ON under the umbrella (content that would VANISH otherwise), explicit
// MC2_VK_VFX=0 forces off, MC2_VK_VFX=1 forces on.
bool vfxGateOn() {
    static const bool s_on = mc2vkgate::subGate("MC2_VK_VFX");
    return s_on;
}

}  // namespace

// ===========================================================================
// PUBLIC ENTRY POINTS
// ===========================================================================
extern "C" int mc2_vk_vfx_island_enabled() { return vfxGateOn() ? 1 : 0; }

extern "C" int mc2_vk_vfx_island_run(
    const void* particles, uint32_t particleCount,
    const VfxParticleDraw* draws, uint32_t drawCount,
    const float* worldToClipGL, const float* camRight, const float* camUp,
    int w, int h) {
    if (!vfxGateOn()) return 0;
    if (particleCount == 0 || drawCount == 0 || particles == nullptr || draws == nullptr) return 0;
    if (w <= 0 || h <= 0 || worldToClipGL == nullptr) return 0;

    if (g_island && (g_island->w != w || g_island->h != h)) {
        destroy_island(g_island); delete g_island; g_island = nullptr;
    }
    if (!g_island) {
        g_island = new VfxIsland();
        if (!init_island(g_island, w, h)) {
            vlog("init failed (%s) — VFX declined this frame.", g_health.fallbackReason);
            destroy_island(g_island); delete g_island; g_island = nullptr;
            return 0;
        }
    }
    VfxIsland* s = g_island;
    if (s->disabled || !s->inited) return 0;

    // Upload the whole particle array (64B/record) + frame constants.
    const VkDeviceSize needBytes = (VkDeviceSize)particleCount * 64u;
    if (!ensure_ssbo(s, needBytes)) { s->disabled = true; return 0; }
    {
        void* p = nullptr;
        if (vmaMapMemory(s->allocator, s->ssboAlloc, &p) == VK_SUCCESS) {
            std::memcpy(p, particles, (size_t)needBytes);
            vmaUnmapMemory(s->allocator, s->ssboAlloc);
        }
    }
    {
        void* p = nullptr;
        if (vmaMapMemory(s->allocator, s->uboAlloc, &p) == VK_SUCCESS) {
            VfxVsParams up{};
            std::memcpy(up.worldToClipGL, worldToClipGL, sizeof(up.worldToClipGL));
            if (camRight) { up.camRight[0]=camRight[0]; up.camRight[1]=camRight[1]; up.camRight[2]=camRight[2]; }
            if (camUp)    { up.camUp[0]=camUp[0];       up.camUp[1]=camUp[1];       up.camUp[2]=camUp[2]; }
            else          { up.camUp[1]=1.0f; }
            if (!camRight) up.camRight[0]=1.0f;
            std::memcpy(p, &up, sizeof(up));
            vmaUnmapMemory(s->allocator, s->uboAlloc);
        }
    }

    // Record: one renderpass, per-group pipeline + sub-range draw.
    vkResetCommandBuffer(s->cbuf, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(s->cbuf, &bi) != VK_SUCCESS) { setFallback("record_failed"); s->disabled = true; return 0; }
    VkClearValue clears[3]{};   // all LOAD; ignored
    VkRenderPassBeginInfo rbi{};
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = s->rpass; rbi.framebuffer = s->fb;
    rbi.renderArea = {{0, 0}, {(uint32_t)w, (uint32_t)h}};
    rbi.clearValueCount = 3; rbi.pClearValues = clears;
    vkCmdBeginRenderPass(s->cbuf, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindDescriptorSets(s->cbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, s->playout, 0, 1, &s->set, 0, nullptr);
    unsigned long groupsDrawn = 0;
    VkPipeline bound = VK_NULL_HANDLE;
    for (uint32_t gi = 0; gi < drawCount; ++gi) {
        const VfxParticleDraw& d = draws[gi];
        if (d.count == 0) continue;
        if ((uint64_t)d.start + d.count > particleCount) continue;   // clamp bad ranges
        VkPipeline want = d.additive ? s->addPipe : s->alphaPipe;
        if (want != bound) { vkCmdBindPipeline(s->cbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, want); bound = want; }
        vkCmdDraw(s->cbuf, d.count * 6u, 1, d.start * 6u, 0);
        ++groupsDrawn;
    }
    vkCmdEndRenderPass(s->cbuf);
    if (vkEndCommandBuffer(s->cbuf) != VK_SUCCESS) { setFallback("record_failed"); s->disabled = true; return 0; }

    if (groupsDrawn == 0) return 0;   // nothing drawable this frame

    vkResetFences(s->device, 1, &s->fence);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &s->cbuf;
    if (vkQueueSubmit(s->queue, 1, &si, s->fence) != VK_SUCCESS) { setFallback("submit_failed"); s->disabled = true; return 0; }
    if (vkWaitForFences(s->device, 1, &s->fence, VK_TRUE, 5ull*1000*1000*1000) != VK_SUCCESS) {
        setFallback("fence_timeout"); s->disabled = true; return 0;
    }

    g_health.vkDraws = groupsDrawn;
    g_health.particles = particleCount;
    g_health.groups = drawCount;
    ++g_health.usedVulkan;
    static bool s_firstOk = false;
    if (!s_firstOk) {
        s_firstOk = true;
        vlog("first frame OK: particles=%u groups=%u draws=%lu (in-place color0 co-write, "
             "shared depth GEQUAL test, no depth write, no composite).",
             particleCount, drawCount, groupsDrawn);
    }
    return 1;
}

extern "C" void mc2_vk_vfx_island_destroy() {
    vlog("[VK_VFX] vulkan_available=%d used_vulkan=%lu vk_draws=%lu particles=%lu groups=%lu "
         "fallback_reason=\"%s\"",
         g_health.vulkanAvailable, g_health.usedVulkan, g_health.vkDraws,
         g_health.particles, g_health.groups, g_health.fallbackReason);
    if (!g_island) return;
    destroy_island(g_island);
    delete g_island; g_island = nullptr;
}

#endif  // MC2_VULKAN_ISLAND
