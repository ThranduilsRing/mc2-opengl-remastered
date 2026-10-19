// vulkan_legacy_shape_island.cpp -- VK-PARITY-F1: VK owner of the legacy MLR
// solid-object vertex loop (ground vehicles etc.). See the header for the why;
// structure = vulkan_mech_island.cpp's shared-GBuffer co-write path (LOAD-preserve
// MRT0 color + MRT1 normal + shared D32S8 depth, GEQUAL reverse-Z, no composite,
// parity-fenced submit) with a much simpler feed:
//   * geometry: the node's CPU gos_VERTEX array (pre-transformed screen-space,
//     CPU-lit) is memcpy'd each frame into a per-parity host-visible vertex buffer;
//   * textures: copy-once import of the handle's decoded texels, keyed on the mcTextureManager node;
//   * no lights / no instancing: vertex colour is already CPU-lit.
// The whole TU compiles to nothing unless MC2_VULKAN_ISLAND is defined.

#ifdef MC2_VULKAN_ISLAND

#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <volk.h>
#include "Gates/gate.h"
#include "Gates/gate_cold.h"   // str<>()
#include "texture_decode.h"   // gos_GetTextureTexels

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include <vk_mem_alloc.h>

#include "vk_image_resource.h"
#include "vk_island_util.h"
#include "vk_one_shot.h"
#include "vulkan_context.h"
#include "vk_texture_residency.h"
#include "vulkan_legacy_shape_island.h"
#include "../../RenderCore/gpu_abi/GpuLegacyFrameUBO.h"  // single-source per-frame UBO
#include "vk_shared_gbuffer_gate.h"
#define MC2_VULKAN_SHARED_SCENE_COLOR_IMPL_ISLAND
#include "vulkan_shared_scene_color.h"
#define MC2_VULKAN_SHARED_SCENE_DEPTH_IMPL_ISLAND
#include "vulkan_shared_scene_depth.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cstdint>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <tracy/Tracy.hpp>

const float* gos_GetProj2ScreenMat4();             // gosRenderer::projection_ (row-major, GL_TRUE)

namespace {

struct Health {
    unsigned long attempted = 0, usedVulkan = 0, nodesDrawn = 0, vertsDrawn = 0, texUploads = 0;
    char fallbackReason[128] = {0};
};
Health g_health;

void setFallback(const char* r) {
    std::strncpy(g_health.fallbackReason, r ? r : "", sizeof(g_health.fallbackReason) - 1);
    g_health.fallbackReason[sizeof(g_health.fallbackReason) - 1] = 0;
}

void vlog(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vk_island_util::vlogv("[VK_LEGACY_SHAPE] ", fmt, ap);
    va_end(ap);
}

std::string spv_dir() { return vk_island_util::spvDir(); }

VkShaderModule load_spv(VkDevice device, std::string_view path) {
    return vk_island_util::loadSpv(device, path, vlog);
}

// The per-frame std140 UBO (LegacyFrameUBO) is single-sourced in
// RenderCore/gpu_abi/GpuLegacyFrameUBO.h; its generated block feeds both legacy shaders.

const uint32_t kVertexStride = 32;       // gos_VERTEX: x,y,z,rhw, argb, frgb, u, v
const uint32_t kMaxTextures  = 192;      // descriptor-set budget (x2 parity)

// The view is declared after the image so it is released first.
struct TexEntry {
    mc2vk::ResidentTextureRef texture;
    VkDescriptorSet set[2] = {};   // allocated from the island's pool, freed with it
};

// Owned objects are declared in creation order and released in reverse by the destructor, while the device is alive (the engine context releases the
// parked island ahead of the device).
struct Island : mc2vk::IslandState {
    bool inited = false, disabled = false;
    int  w = 0, h = 0;
    VkInstance instance = VK_NULL_HANDLE; VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE; uint32_t gfxFamily = UINT32_MAX;
    VkQueue queue = VK_NULL_HANDLE; VmaAllocator allocator = VK_NULL_HANDLE;

    struct VkImageResource* colorRes = nullptr;
    struct VkImageResource* normalRes = nullptr;
    struct VkImageResource* depthRes = nullptr;

    // Per-frame data (the frame UBO, then the vertex stream) lives in the engine frame ring; every texture set binds the UBO with a dynamic offset.
    // ringBound[par] = the ring buffer binding 0 of every [par] set currently points at; a frame whose span came from another buffer (the ring grew), or a
    // set created since, re-points them after beginFrame retired the parity.
    VkBuffer ringBound[2] = {};
    VkCommandBuffer cbuf[2] = {};   // allocated from `cpool`, freed with it

    mc2vk::OwnedCommandPool cpool;
    mc2vk::OwnedSampler samp;
    mc2vk::OwnedRenderPass rpass;
    mc2vk::OwnedFramebuffer fb;
    mc2vk::OwnedDescriptorSetLayout dsl;
    mc2vk::OwnedDescriptorPool pool;
    TexEntry dummyTex;                                  // 1x1 white (untextured / failed import)
    std::unordered_map<uint32_t, TexEntry> texs;        // keyed on mcTextureManager node
    std::unordered_set<uint32_t> untexturedNodes;       // nodes whose texture has no CPU texels: drawn with the dummy
    mc2vk::OwnedShaderModule vert, frag;
    mc2vk::OwnedPipelineLayout playout;
    mc2vk::OwnedPipeline pipes[MC2_LEGACY_PASS_COUNT];
};

// The island lives in the engine context's slot: the context releases it ahead of the device whatever path ends the process.
std::unique_ptr<mc2vk::IslandState>& islandSlot() {
    return mc2vk::getOrCreateEngineVulkanContext().islandSlot(mc2vk::IslandSlot::LegacyShape);
}
Island* parkedIsland() { return static_cast<Island*>(islandSlot().get()); }
bool g_sessionDeclined = false;
int  g_initReachedBuild = 0;
std::vector<Mc2LegacyMlrDraw> g_pending;

// One level of RGBA8 or BC7 texels, copied as they are into a sampled image of `fmt`.
bool upload_image(Island* s, TexEntry& e, uint32_t w, uint32_t h, VkFormat fmt, const void* bytes, VkDeviceSize byteCount) {
    // Explicit legacy producer boundary: CPU/gos bytes stay legacy-owned; the service owns their Vulkan image.
    auto& textures=mc2vk::getOrCreateEngineVulkanContext().textures();
    e.texture=textures.acquireLegacy(w,h,fmt,bytes,size_t(byteCount));
    return bool(e.texture);
}

// Per-parity descriptor set for one texture: 0 = frame UBO, 1 = albedo.
bool alloc_tex_sets(Island* s, TexEntry& t) {
    VkDescriptorSetLayout layouts[2] = {s->dsl.get(), s->dsl.get()};
    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = s->pool.get(); dsai.descriptorSetCount = 2; dsai.pSetLayouts = layouts;
    if (vkAllocateDescriptorSets(s->device, &dsai, t.set) != VK_SUCCESS) return false;
    for (uint32_t par = 0; par < 2; ++par) {
        VkDescriptorImageInfo ii{};
        ii.sampler = s->samp.get(); ii.imageView = mc2vk::getOrCreateEngineVulkanContext().textures().view(t.texture.get()); ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet w[1]{};
        w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstSet = t.set[par]; w[0].dstBinding = 1;
        w[0].descriptorCount = 1; w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = &ii;
        vkUpdateDescriptorSets(s->device, 1, w, 0, nullptr);
        // Binding 0 (the frame UBO) at the ring's current buffer; the parity is re-pointed at its next frame.
        if (const VkBuffer ring = mc2vk::getOrCreateEngineVulkanContext().frameRingBuffer())
            mc2vk::writeDynamicBinding(s->device, t.set[par], 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, ring, sizeof(LegacyFrameUBO));
        s->ringBound[par] = VK_NULL_HANDLE;
    }
    return true;
}

// Resolve (importing copy-once on first sight) the texture entry for a node.
// Called BEFORE recording (the import records its own one-shot upload); never inside the VK command buffer.
TexEntry* get_tex(Island* s, uint32_t texNode, uint32_t gosHandle) {
    if (gosHandle == 0 || gosHandle == 0xffffffffu) return &s->dummyTex;
    auto it = s->texs.find(texNode);
    if (it != s->texs.end()) return &it->second;
    if (s->untexturedNodes.contains(texNode)) return &s->dummyTex;
    if (s->texs.size() >= kMaxTextures) return &s->dummyTex;
    const GosTexels texels = gos_GetTextureTexels(gosHandle);
    if (texels.status == GosTexels::Status::Unavailable) {   // will never have texels: stop asking
        s->untexturedNodes.insert(texNode);
        return &s->dummyTex;
    }
    if (texels.status != GosTexels::Status::Ready) return &s->dummyTex;    // not resident yet; retry next frame
    const uint32_t w = texels.width, h = texels.height;
    if (w == 0 || h == 0 || w > 8192 || h > 8192) return &s->dummyTex;

    TexEntry t;
    if (!upload_image(s, t, w, h, mc2vk::texelFormat(texels.vkFormat), texels.bytes->data(),
                      (VkDeviceSize)texels.bytes->size()) ||
        !alloc_tex_sets(s, t)) {
        return &s->dummyTex;   // `t` releases whatever it created
    }
    ++g_health.texUploads;
    auto ins = s->texs.emplace(texNode, std::move(t));
    vlog("texture cached: node=%u handle=%u %ux%u format=%u (%zu/%u).", texNode, gosHandle, w, h, texels.vkFormat,
         s->texs.size(), kMaxTextures);
    return &ins.first->second;
}

bool init_island(Island* s, int w, int h) {
    s->w = w; s->h = h;
    ++g_health.attempted;
    g_initReachedBuild = 0;

    if (!mc2vk::ensureEngineVulkanContext()) { setFallback("engine_vk_context_birth_failed"); return false; }
    mc2vk::VulkanContext& ctx = mc2vk::getOrCreateEngineVulkanContext();
    if (!ctx.valid()) { setFallback("engine_vk_context_not_valid"); return false; }
    s->instance  = ctx.instance();
    s->phys      = ctx.physicalDevice();
    s->device    = ctx.device();
    s->queue     = ctx.graphicsQueue();
    s->gfxFamily = ctx.graphicsFamily();
    s->allocator = ctx.allocator();
    if (s->device == VK_NULL_HANDLE || s->allocator == VK_NULL_HANDLE) { setFallback("engine_vk_device_or_allocator_null"); return false; }

    s->colorRes  = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_COLOR0);
    s->normalRes = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_NORMAL);
    s->depthRes  = mc2SharedSceneDepthGetResource();
    if (!s->colorRes || !s->normalRes || !s->depthRes) { setFallback("shared_gbuffer_not_ready"); return false; }
    g_initReachedBuild = 1;   // readiness gates passed: failures past here are PERMANENT

    {   // command pool + per-parity command buffers
        VkCommandPoolCreateInfo cpi{};
        cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        cpi.queueFamilyIndex = s->gfxFamily;
        if (!mc2vk::create(s->cpool, s->device, [&](VkCommandPool* h) { return vkCreateCommandPool(s->device, &cpi, nullptr, h); })) { setFallback("cmdpool_failed"); return false; }
        VkCommandBufferAllocateInfo cbi{};
        cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbi.commandPool = s->cpool.get(); cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbi.commandBufferCount = 2;
        if (vkAllocateCommandBuffers(s->device, &cbi, s->cbuf) != VK_SUCCESS) { setFallback("cmdbuf_failed"); return false; }
    }

    {   // sampler (REPEAT / LINEAR, no mips -- the FS uses textureLod(..., 0))
        VkSamplerCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        if (!mc2vk::create(s->samp, s->device, [&](VkSampler* h) { return vkCreateSampler(s->device, &sci, nullptr, h); })) { setFallback("sampler_failed"); return false; }
    }

    {   // render pass + framebuffer: LOAD color0/normal/depth (same as the mech island)
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
        if (!mc2vk::create(s->rpass, s->device, [&](VkRenderPass* h) { return vkCreateRenderPass(s->device, &rpci, nullptr, h); })) { setFallback("renderpass_failed"); return false; }
        VkImageView fbViews[3] = { colorRes->view.get(), normalRes->view.get(), depthRes->view.get() };
        VkFramebufferCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = s->rpass.get(); fci.attachmentCount = 3; fci.pAttachments = fbViews;
        fci.width = (uint32_t)w; fci.height = (uint32_t)h; fci.layers = 1;
        if (!mc2vk::create(s->fb, s->device, [&](VkFramebuffer* h) { return vkCreateFramebuffer(s->device, &fci, nullptr, h); })) { setFallback("framebuffer_failed"); return false; }
    }

    {   // descriptor layout + pool, dummy white texture
        VkDescriptorSetLayoutBinding b[2]{};
        b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.bindingCount = 2; lci.pBindings = b;
        if (!mc2vk::create(s->dsl, s->device, [&](VkDescriptorSetLayout* h) { return vkCreateDescriptorSetLayout(s->device, &lci, nullptr, h); })) { setFallback("dsl_failed"); return false; }
        const uint32_t N = kMaxTextures + 1;   // + dummy
        VkDescriptorPoolSize sizes[2]{};
        sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; sizes[0].descriptorCount = 2 * N;
        sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; sizes[1].descriptorCount = 2 * N;
        VkDescriptorPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.maxSets = 2 * N; pci.poolSizeCount = 2; pci.pPoolSizes = sizes;
        if (!mc2vk::create(s->pool, s->device, [&](VkDescriptorPool* h) { return vkCreateDescriptorPool(s->device, &pci, nullptr, h); })) { setFallback("pool_failed"); return false; }

        const uint8_t px[4] = {255, 255, 255, 255};
        if (!upload_image(s, s->dummyTex, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, px, sizeof(px)) || !alloc_tex_sets(s, s->dummyTex)) {
            setFallback("dummy_tex_failed"); return false;
        }
    }

    {   // pipeline
        s->vert = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir() + "legacy_shape_tier1.vert.spv"));
        s->frag = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir() + "legacy_shape_tier1.frag.spv"));
        if (!s->vert || !s->frag) { setFallback("spv_load_failed"); return false; }

        VkPushConstantRange pcr{};
        pcr.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; pcr.offset = 0; pcr.size = 4;
        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        const VkDescriptorSetLayout dslHandle = s->dsl.get();
        plci.setLayoutCount = 1; plci.pSetLayouts = &dslHandle;
        plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
        if (!mc2vk::create(s->playout, s->device, [&](VkPipelineLayout* h) { return vkCreatePipelineLayout(s->device, &plci, nullptr, h); })) { setFallback("playout_failed"); return false; }

        // gos_VERTEX (32B): pos vec4 (x,y,z,rhw)@0, argb u8x4 unorm@16, frgb u8x4 unorm@20, uv vec2@24.
        VkVertexInputBindingDescription vib{};
        vib.binding = 0; vib.stride = kVertexStride; vib.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        VkVertexInputAttributeDescription via[4]{};
        via[0].location = 0; via[0].binding = 0; via[0].format = VK_FORMAT_R32G32B32A32_SFLOAT; via[0].offset = 0;
        via[1].location = 1; via[1].binding = 0; via[1].format = VK_FORMAT_R8G8B8A8_UNORM;      via[1].offset = 16;
        via[2].location = 2; via[2].binding = 0; via[2].format = VK_FORMAT_R8G8B8A8_UNORM;      via[2].offset = 20;
        via[3].location = 3; via[3].binding = 0; via[3].format = VK_FORMAT_R32G32_SFLOAT;       via[3].offset = 24;
        VkPipelineVertexInputStateCreateInfo vin{};
        vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vin.vertexBindingDescriptionCount = 1; vin.pVertexBindingDescriptions = &vib;
        vin.vertexAttributeDescriptionCount = 4; vin.pVertexAttributeDescriptions = via;

        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vps{};
        vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vps.viewportCount = 1; vps.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dsci{};
        dsci.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dsci.dynamicStateCount = 2; dsci.pDynamicStates = dyn;
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = s->vert.get(); stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = s->frag.get(); stages[1].pName = "main";

        // One pipeline per legacy loop (gosRenderer state each loop sets in renderLists):
        //   SOLID : OneZero (no blend), depth write, ZCompare 1 = GEQUAL  (reverse-Z)
        //   ALPHA : AlphaInvAlpha,      no write,    GEQUAL
        //   SHADOW: AlphaInvAlpha,      no write,    ZCompare 2 = GREATER
        //   ADD   : OneOne,             no write,    GEQUAL
        // Blended passes mask MRT1 (GBuffer normal) off so they never smear the normal buffer.
        for (int k = 0; k < MC2_LEGACY_PASS_COUNT; ++k) {
            VkPipelineDepthStencilStateCreateInfo ds{};
            ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
            ds.depthTestEnable = VK_TRUE;
            ds.depthWriteEnable = (k == MC2_LEGACY_PASS_SOLID) ? VK_TRUE : VK_FALSE;
            ds.depthCompareOp = (k == MC2_LEGACY_PASS_SHADOW) ? VK_COMPARE_OP_GREATER
                                                              : VK_COMPARE_OP_GREATER_OR_EQUAL;
            VkPipelineColorBlendAttachmentState cba[2]{};
            const VkColorComponentFlags all = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            cba[0].colorWriteMask = all;
            cba[1].colorWriteMask = (k == MC2_LEGACY_PASS_SOLID) ? all : 0;
            if (k == MC2_LEGACY_PASS_ALPHA || k == MC2_LEGACY_PASS_SHADOW) {
                cba[0].blendEnable = VK_TRUE;
                cba[0].srcColorBlendFactor = cba[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
                cba[0].dstColorBlendFactor = cba[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                cba[0].colorBlendOp = cba[0].alphaBlendOp = VK_BLEND_OP_ADD;
            } else if (k == MC2_LEGACY_PASS_ADD) {
                cba[0].blendEnable = VK_TRUE;
                cba[0].srcColorBlendFactor = cba[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                cba[0].dstColorBlendFactor = cba[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                cba[0].colorBlendOp = cba[0].alphaBlendOp = VK_BLEND_OP_ADD;
            }
            VkPipelineColorBlendStateCreateInfo cb{};
            cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
            cb.attachmentCount = 2; cb.pAttachments = cba;
            VkGraphicsPipelineCreateInfo gp{};
            gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            gp.stageCount = 2; gp.pStages = stages;
            gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia;
            gp.pViewportState = &vps; gp.pRasterizationState = &rs;
            gp.pMultisampleState = &ms; gp.pDepthStencilState = &ds; gp.pColorBlendState = &cb;
            gp.pDynamicState = &dsci;
            gp.layout = s->playout.get(); gp.renderPass = s->rpass.get(); gp.subpass = 0;
            if (!mc2vk::create(s->pipes[k], s->device, [&](VkPipeline* h) { return vkCreateGraphicsPipelines(s->device, VK_NULL_HANDLE, 1, &gp, nullptr, h); })) {
                setFallback("pipeline_failed"); return false;
            }
            mc2vk::nameObjectAt(s->device, s->pipes[k].get(), "legacyshape.pipe", static_cast<int>(k));
        }
    }

    s->inited = true;
    setFallback("");
    vlog("island initialized %dx%d (legacy MLR solid loop -> shared color(MRT0)+normal(MRT1)+depth; GEQUAL reverse-Z).", w, h);
    return true;
}

} // namespace

// ===========================================================================
// PUBLIC ENTRY POINTS.
// ===========================================================================

extern "C" int mc2_vk_legacy_shape_island_enabled() {
    return g_sessionDeclined ? 0 : 1;
}

extern "C" void mc2_vk_legacy_shape_submit(const Mc2LegacyMlrDraw* d) {
    if (d && d->vertices && d->vertexCount >= 3) g_pending.push_back(*d);
}

extern "C" int mc2_vk_legacy_shape_island_run(int w, int h, const Mc2LegacyMlrFrame* frame) {
    ZoneScopedN("VK-legacy-shape-island");
    // Always consume the queue (even on failure) so it can never grow across frames.
    std::vector<Mc2LegacyMlrDraw> draws;
    draws.swap(g_pending);
    if (g_sessionDeclined || !frame) return 0;
    if (draws.empty() || w <= 0 || h <= 0) return 0;

    mc2vk::VulkanContext& slotCtx = mc2vk::getOrCreateEngineVulkanContext();
    if (const Island* parked = parkedIsland(); parked && (parked->w != w || parked->h != h)) slotCtx.retireIsland(mc2vk::IslandSlot::LegacyShape);
    if (!islandSlot()) {
        auto fresh = std::make_unique<Island>();
        if (!init_island(fresh.get(), w, h)) {
            const bool permanent = (g_initReachedBuild != 0);
            slotCtx.waitIdle("legacy_shape_init_failure");   // objects made before the failure may have been submitted
            if (permanent) {
                g_sessionDeclined = true;
                vlog("boot-level decline: permanent init failure (%s) -- legacy shapes will not draw under VK.",
                     g_health.fallbackReason);
            } else {
                static bool s_logged = false;
                if (!s_logged) { vlog("init not ready (%s) -- will retry.", g_health.fallbackReason); s_logged = true; }
            }
            return 0;
        }
        islandSlot() = std::move(fresh);
    }
    Island* s = parkedIsland();
    if (s->disabled || !s->inited) return 0;

    // Resolve textures BEFORE recording; total the vertex bytes.
    struct Resolved { TexEntry* t; const Mc2LegacyMlrDraw* d; VkDeviceSize byteOff; };
    std::vector<Resolved> list;
    list.reserve(draws.size());
    VkDeviceSize totalBytes = 0;
    for (const Mc2LegacyMlrDraw& d : draws) {
        TexEntry* t = get_tex(s, d.texNode, d.gosTexHandle);
        list.push_back({t ? t : &s->dummyTex, &d, totalBytes});
        totalBytes += (VkDeviceSize)d.vertexCount * kVertexStride;
    }

    // The frame UBO and the vertex stream go into the engine frame ring BEFORE beginFrame (a ring failure after it could strand the fence). The ring slot
    // belongs to the frame, not to the parity, so staging here races nothing.
    mc2vk::VulkanContext& rctx = mc2vk::getOrCreateEngineVulkanContext();
    const VkDeviceSize vtxOff = mc2vk::alignUp(sizeof(LegacyFrameUBO), rctx.frameAlignment());
    const mc2vk::FrameAlloc span = rctx.frameAlloc(vtxOff + (totalBytes ? totalBytes : kVertexStride));
    if (!span) { s->disabled = true; setFallback("frame_ring_alloc_failed"); return 0; }

    VkImageResource* colorRes = reinterpret_cast<VkImageResource*>(s->colorRes);
    VkFrameSync fsC{};
    { ZoneScopedN("VKIsl.Legacy.FenceWait"); fsC = vkImageResourceBeginFrame(colorRes); }
    const uint32_t par = fsC.parity & 1u;

    {   // per-frame vertex stream + UBO
        uint8_t* const base = static_cast<uint8_t*>(span.ptr);
        for (const Resolved& r : list)
            std::memcpy(base + vtxOff + r.byteOff, r.d->vertices, (size_t)r.d->vertexCount * kVertexStride);
        LegacyFrameUBO u{};
        const float* proj = gos_GetProj2ScreenMat4();
        if (proj) std::memcpy(u.proj, proj, sizeof(u.proj));
        for (int k = 0; k < 4; ++k) u.fogColor[k] = frame->fogColor[k];
        std::memcpy(base, &u, sizeof(u));
    }
    // Every texture set of this parity follows the ring buffer the span came from; the draw passes the UBO offset.
    if (s->ringBound[par] != span.buffer) {
        for (auto& kv : s->texs)
            if (kv.second.set[par] != VK_NULL_HANDLE) mc2vk::writeDynamicBinding(s->device, kv.second.set[par], 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, span.buffer, sizeof(LegacyFrameUBO));
        if (s->dummyTex.set[par] != VK_NULL_HANDLE) mc2vk::writeDynamicBinding(s->device, s->dummyTex.set[par], 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, span.buffer, sizeof(LegacyFrameUBO));
        s->ringBound[par] = span.buffer;
    }
    const uint32_t uboOffset = static_cast<uint32_t>(span.offset);

    VkCommandBuffer cb = s->cbuf[par];
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) { setFallback("record_failed"); s->disabled = true; return 0; }

    VkRenderPassBeginInfo rbi{};
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = s->rpass.get(); rbi.framebuffer = s->fb.get();
    rbi.renderArea = {{0, 0}, {(uint32_t)w, (uint32_t)h}};
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {(uint32_t)w, (uint32_t)h}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &scissor);
    VkDeviceSize vertexOffset = span.offset + vtxOff;
    uint32_t boundPass = 0xffffffffu;
    vkCmdBindVertexBuffers(cb, 0, 1, &span.buffer, &vertexOffset);

    VkDescriptorSet boundSet = VK_NULL_HANDLE;
    unsigned long drawn = 0, verts = 0;
    for (const Resolved& r : list) {
        VkDescriptorSet set = r.t->set[par];
        if (set != boundSet) {
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->playout.get(), 0, 1, &set, 1, &uboOffset);
            boundSet = set;
        }
        const uint32_t pass = (r.d->pass < MC2_LEGACY_PASS_COUNT) ? r.d->pass : (uint32_t)MC2_LEGACY_PASS_SOLID;
        if (pass != boundPass) {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipes[pass].get());
            boundPass = pass;
        }
        // FS flags: bit0 alpha test, bit1 fog off (additive loops clear gos_State_Fog), bit2 opaque alpha.
        const uint32_t pflags = (r.d->alphaTest ? 1u : 0u) |
                                (pass == MC2_LEGACY_PASS_ADD ? 2u : 0u) |
                                (pass == MC2_LEGACY_PASS_SOLID ? 4u : 0u);
        vkCmdPushConstants(cb, s->playout.get(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4, &pflags);
        vkCmdDraw(cb, r.d->vertexCount, 1, (uint32_t)(r.byteOff / kVertexStride), 0);
        ++drawn; verts += r.d->vertexCount;
    }
    vkCmdEndRenderPass(cb);
    if (vkEndCommandBuffer(cb) != VK_SUCCESS) { setFallback("record_failed"); s->disabled = true; return 0; }

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    { ZoneScopedN("VKIsl.Legacy.Submit");
      if (mc2vk::submitGraphics(si, fsC.fence) != VK_SUCCESS) { setFallback("submit_failed"); s->disabled = true; return 0; } }

    g_health.nodesDrawn += drawn;
    g_health.vertsDrawn += verts;
    ++g_health.usedVulkan;
    static bool s_firstOk = false;
    if (!s_firstOk) {
        vlog("first legacy-shape co-write: nodes=%lu verts=%lu textures=%zu.", drawn, verts, s->texs.size());
        s_firstOk = true;
    }
    return 1;
}

extern "C" void mc2_vk_legacy_shape_island_reset() {
    g_pending.clear();
    Island* s = parkedIsland();
    if (!s || !s->inited) return;
    // Texture node indices are reused by the next mission: drop the copy-once cache.
    mc2vk::getOrCreateEngineVulkanContext().waitIdle("legacy_shape_reset");
    s->texs.clear();
    s->untexturedNodes.clear();
    // The pool has no FREE_DESCRIPTOR_SET bit: reset it and re-create the dummy sets.
    if (s->pool) vkResetDescriptorPool(s->device, s->pool.get(), 0);
    s->dummyTex.set[0] = s->dummyTex.set[1] = VK_NULL_HANDLE;
    s->ringBound[0] = s->ringBound[1] = VK_NULL_HANDLE;
    if (!alloc_tex_sets(s, s->dummyTex)) { s->disabled = true; vlog("reset: dummy sets failed -- island disabled."); }
    vlog("texture cache reset on mission unload.");
}

extern "C" void mc2_vk_legacy_shape_island_destroy() {
    vlog("used_vulkan=%lu nodes_drawn=%lu verts_drawn=%lu tex_uploads=%lu fallback_reason=\"%s\"",
         g_health.usedVulkan, g_health.nodesDrawn, g_health.vertsDrawn, g_health.texUploads,
         g_health.fallbackReason);
    mc2vk::getOrCreateEngineVulkanContext().retireIsland(mc2vk::IslandSlot::LegacyShape);
}

#endif // MC2_VULKAN_ISLAND
