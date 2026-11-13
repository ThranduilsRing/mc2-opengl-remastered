// vulkan_impact_decals.cpp -- weapon impact box decals on the shared scene GBuffer (docs/impact-damage-design.md).
//
// WHAT: one pass, two pipelines over the same instanced unit box (36 vertices, one instance per decal):
//   * multiply: every live decal; the fragment reconstructs the surface from the shared depth, rejects pixels outside the box
//     or of a surface class the decal does not affect, and multiplies the scene colour per channel by the decal's albedo
//     ratio, char tint and the relight ratio of its perturbed normal (the deformation);
//   * additive: only the hot prefix of the instance list; adds the cooling emissive glow.
// WHERE: right after the terrain overlay/decal batch (mc2_vk_overlay_run), so the opaque scene is complete and water,
// particles and post have not run yet. The screen shadow and fog later in the frame apply to the decal like to any surface.
// SYNC: the screen-shadow pattern: in-command-buffer barriers take depth and normal to SHADER_READ_ONLY and back, the render
// pass LOADs/STOREs colour0 in COLOR_ATTACHMENT layout, one submit per frame, a timeline ticket per frame slot guards the
// host-written instance buffer and the command buffer.
// COST: rasterization is bounded by the boxes' screen area (back faces only, so each covered pixel shades once); the
// instance count is capped by the pool (impact::kMaxDecals); an empty frame records nothing.
// Clean-room: written from the published screen-space/box decal technique (deferred decals, Persson 2011; "Screen Space
// Decals" in Wolfire's and Bungie's talks), on MC2's own depth reconstruction (the screen-shadow passes).

#include "vulkan_impact_decals.h"
#include "vk_render_infra.h"

#include "../../RenderCore/ImpactDecals.h"
#include "../../RenderCore/ImpactDecalAtlas.h"

#include <cstring>
#include <mutex>
#include <vector>

namespace {
// The latched frame (written by the presentation layer before renderLists, read by the pass in the same frame).
std::vector<impact::GpuDecal> g_frame;
uint32_t g_frameHot = 0;
Mc2ImpactDecalHealth g_health;
}

extern "C" void gos_SetImpactDecalFrame(const void* records, uint32_t count, uint32_t hotCount) {
    count = count > impact::kMaxDecals ? impact::kMaxDecals : count;
    g_frame.resize(count);
    if (count) std::memcpy(g_frame.data(), records, static_cast<size_t>(count) * sizeof(impact::GpuDecal));
    g_frameHot = hotCount > count ? count : hotCount;
}

extern "C" void mc2_vk_impact_decals_health(Mc2ImpactDecalHealth* out) {
    if (out) *out = g_health;
}


#if defined(_WIN32) && !defined(VK_USE_PLATFORM_WIN32_KHR)
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <volk.h>

#include "gos_postprocess.h"
#include "vk_dynamic_rendering.h"
#include "vk_gpu_profile.h"
#include "vk_image_resource.h"
#include "vk_island_util.h"
#include "vk_texture_residency.h"
#include "vulkan_context.h"
#define MC2_VULKAN_SHARED_SCENE_COLOR_IMPL_ISLAND
#include "vulkan_shared_scene_color.h"
#define MC2_VULKAN_SHARED_SCENE_DEPTH_IMPL_ISLAND
#include "vulkan_shared_scene_depth.h"

#include <glm/glm.hpp>
#include <tracy/Tracy.hpp>

#include <cmath>
#include <cstdio>
#include <algorithm>
#include <memory>
#include <string>
#include <string_view>

extern void gos_GetTerrainLightDir(float* x, float* y, float* z);

namespace {

void decline(std::string_view reason) {
    ++g_health.declined;
    if (reason != g_health.reason) {
        static int s_logs = 0;   // each new reason is logged, capped because a pose can alternate between two
        if (s_logs++ < 8) std::fprintf(stderr, "[VK_IMPACT_DECALS] declined: reason=%.*s sample=%d/8\n", static_cast<int>(reason.size()), reason.data(), s_logs);
        const size_t n = std::min(reason.size(), sizeof(g_health.reason) - 1);
        std::memcpy(g_health.reason, reason.data(), n);
        g_health.reason[n] = '\0';
    }
}

// std140 frame block (binding 4). Matrices are the raw terrain world-to-clip arrays (MC2 world space in), as the overlay
// and screen-shadow shaders take them.
struct FrameUbo {
    float viewProj[16];
    float invViewProj[16];
    float eye[4];          // homogeneous camera point (w = 1 for perspective)
    float lightDir[4];     // toward the sun, MC2 space; w = ambient share of the relight
    float screen[4];       // w, h, 1/w, 1/h
};
static_assert(sizeof(FrameUbo) == 176, "FrameUbo std140 size");

struct PushMode { uint32_t mode; uint32_t pad[3]; };   // 0 multiply, 1 additive

constexpr uint32_t kSlots = mc2vk::kMaxFramesInFlight;

struct State {
    bool disabled = false, inited = false;
    int w = 0, h = 0;
    VkDevice device = VK_NULL_HANDLE;
    VmaAllocator allocator = VK_NULL_HANDLE;
    VkImageResource* colorRes = nullptr;
    VkImageResource* normalRes = nullptr;
    VkImageResource* depthRes = nullptr;
    VkDescriptorSet set[kSlots] = {};
    VkCommandBuffer cbuf[kSlots] = {};
    uint64_t ticket[kSlots] = {};
    VkImageView boundAtlas[kSlots][2] = {};   // the residency serves a placeholder until the upload lands
    // Owned, in creation order (released in reverse by the moved-to local in destroy()).
    mc2vk::OwnedImageView depthView;
    mc2vk::OwnedSampler pointSampler;
    mc2vk::OwnedSampler atlasSampler;
    mc2vk::ResidentTextureRef surfaceAtlas;
    mc2vk::ResidentTextureRef normalAtlas;
    mc2vk::OwnedBuffer ubo[kSlots];
    mc2vk::OwnedBuffer ssbo[kSlots];
    VkFormat colorFmt = VK_FORMAT_UNDEFINED;
    mc2vk::OwnedDescriptorSetLayout dsl;
    mc2vk::OwnedDescriptorPool pool;
    mc2vk::OwnedPipelineLayout playout;
    mc2vk::OwnedShaderModule vert, frag;
    mc2vk::OwnedPipeline mulPipe, addPipe;
    mc2vk::OwnedCommandPool cpool[kSlots];   // one pool per slot: vkResetCommandPool instead of per-cb reset
};
std::unique_ptr<State> g_state;

void destroy(State* s) {
    if (!s) return;
    if (s->device != VK_NULL_HANDLE && mc2vk::engineVulkanContextValid())
        mc2vk::getOrCreateEngineVulkanContext().waitIdle("impact_decals_destroy");
    const bool disabled = s->disabled;
    { State dying = std::move(*s); (void)dying; }
    *s = State{};
    s->disabled = disabled;
}


bool build(State* s, int w, int h) {
    s->w = w; s->h = h;
    if (!mc2vk::ensureEngineVulkanContext()) { decline("no_engine_context"); return false; }
    mc2vk::VulkanContext& ctx = mc2vk::getOrCreateEngineVulkanContext();
    if (!ctx.valid()) { decline("engine_context_invalid"); return false; }
    s->device = ctx.device();
    s->allocator = ctx.allocator();
    s->colorRes = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_COLOR0);
    s->normalRes = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_NORMAL);
    s->depthRes = mc2SharedSceneDepthGetResource();
    if (!s->colorRes || !s->normalRes || !s->depthRes) { decline("shared_gbuffer_not_ready"); return false; }
    if (static_cast<int>(s->colorRes->w) != w || static_cast<int>(s->colorRes->h) != h || static_cast<int>(s->depthRes->w) != w || static_cast<int>(s->depthRes->h) != h) {
        decline("shared_gbuffer_size_mismatch"); return false;
    }
    VkDevice dev = s->device;

    {   // depth-aspect view for sampling
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = s->depthRes->image.get(); vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = s->depthRes->format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        if (!mc2vk::create(s->depthView, dev, [&](auto* o) { return vkCreateImageView(dev, &vci, nullptr, o); })) return false;
    }
    {
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = sci.minFilter = VK_FILTER_NEAREST; sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (!mc2vk::create(s->pointSampler, dev, [&](auto* o) { return vkCreateSampler(dev, &sci, nullptr, o); })) return false;
        sci.magFilter = sci.minFilter = VK_FILTER_LINEAR; sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.maxLod = 16.0f;
        if (!mc2vk::create(s->atlasSampler, dev, [&](auto* o) { return vkCreateSampler(dev, &sci, nullptr, o); })) return false;
    }
    {   // the procedural atlas: two RGBA8 2D-array stacks with mips (counted by the texture residency budget)
        const impact::atlas::Layers layers = impact::atlas::generate();
        const uint32_t n = impact::kTileCount, e = impact::atlas::kSize;
        s->surfaceAtlas = ctx.textures().acquirePixels(e, e, VK_FORMAT_R8G8B8A8_UNORM, layers.surface.data(), layers.surface.size(), true, n, true);
        s->normalAtlas = ctx.textures().acquirePixels(e, e, VK_FORMAT_R8G8B8A8_UNORM, layers.normal.data(), layers.normal.size(), true, n, true);
        if (!s->surfaceAtlas || !s->normalAtlas) { decline("atlas_upload_failed"); return false; }
    }
    for (uint32_t i = 0; i < kSlots; ++i) {
        if (!s->ubo[i].createGpuRead(s->allocator, s->device, sizeof(FrameUbo), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, "impact.ubo", static_cast<int>(i)) ||
            !s->ssbo[i].createGpuRead(s->allocator, s->device, sizeof(impact::GpuDecal) * impact::kMaxDecals,
                                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "impact.ssbo", static_cast<int>(i))) {
            decline("buffer_create_failed"); return false;
        }
    }
    s->colorFmt = s->colorRes->format;   // pass: shared colour0 only, LOAD/STORE, layout-stable; dynamic rendering (pipelines carry the format)
    {   // set: 0 depth, 1 scene normal (class alpha), 2 surface atlas, 3 normal atlas, 4 frame UBO, 5 decal SSBO
        VkDescriptorSetLayoutBinding b[6]{};
        for (uint32_t i = 0; i < 6; ++i) {
            b[i].binding = i; b[i].descriptorCount = 1;
            b[i].descriptorType = i < 4 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                        : (i == 4 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
            b[i].stageFlags = (i >= 4) ? (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT) : VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        lci.bindingCount = 6; lci.pBindings = b;
        if (!mc2vk::create(s->dsl, dev, [&](auto* o) { return vkCreateDescriptorSetLayout(dev, &lci, nullptr, o); })) return false;
        VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 * kSlots},
                                         {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kSlots},
                                         {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kSlots}};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pci.maxSets = kSlots; pci.poolSizeCount = 3; pci.pPoolSizes = sizes;
        if (!mc2vk::create(s->pool, dev, [&](auto* o) { return vkCreateDescriptorPool(dev, &pci, nullptr, o); })) return false;
        for (uint32_t i = 0; i < kSlots; ++i) {
            const VkDescriptorSetLayout l = s->dsl.get();
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = s->pool.get(); ai.descriptorSetCount = 1; ai.pSetLayouts = &l;
            if (MC2_VK_ALLOC_SETS(dev, &ai, &s->set[i]) != VK_SUCCESS) return false;
            VkDescriptorImageInfo img[4] = {
                {s->pointSampler.get(), s->depthView.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                {s->pointSampler.get(), s->normalRes->view.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                {s->atlasSampler.get(), s->surfaceAtlas.view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                {s->atlasSampler.get(), s->normalAtlas.view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
            VkDescriptorBufferInfo ub{s->ubo[i].get(), 0, VK_WHOLE_SIZE};
            VkDescriptorBufferInfo sb{s->ssbo[i].get(), 0, VK_WHOLE_SIZE};
            VkWriteDescriptorSet wr[6]{};
            for (uint32_t k = 0; k < 6; ++k) {
                wr[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[k].dstSet = s->set[i]; wr[k].dstBinding = k;
                wr[k].descriptorCount = 1; wr[k].descriptorType = b[k].descriptorType;
                if (k < 4) wr[k].pImageInfo = &img[k]; else wr[k].pBufferInfo = (k == 4) ? &ub : &sb;
            }
            vkUpdateDescriptorSets(dev, 6, wr, 0, nullptr);
        }
    }
    {   // pipelines: same shaders, two blend states
        s->vert = mc2vk::OwnedShaderModule(dev, vk_island_util::loadSpv(dev, vk_island_util::spvDir() + "impact_decal.vert.spv", nullptr));
        s->frag = mc2vk::OwnedShaderModule(dev, vk_island_util::loadSpv(dev, vk_island_util::spvDir() + "impact_decal.frag.spv", nullptr));
        if (!s->vert || !s->frag) { decline("spv_load_failed"); return false; }
        const VkDescriptorSetLayout l = s->dsl.get();
        VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushMode)};
        VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        plci.setLayoutCount = 1; plci.pSetLayouts = &l; plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
        if (!mc2vk::create(s->playout, dev, [&](auto* o) { return vkCreatePipelineLayout(dev, &plci, nullptr, o); })) return false;
        auto make = [&](bool additive, mc2vk::OwnedPipeline& out, std::string_view name) {
            VkPipelineShaderStageCreateInfo st[2]{};
            st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = s->vert.get(); st[0].pName = "main";
            st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = s->frag.get(); st[1].pName = "main";
            VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
            ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkViewport vp{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
            VkRect2D sc{{0, 0}, {static_cast<uint32_t>(w), static_cast<uint32_t>(h)}};
            VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
            vps.viewportCount = 1; vps.pViewports = &vp; vps.scissorCount = 1; vps.pScissors = &sc;
            VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
            rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;   // the VS drops camera-facing faces
            rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
            VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
            ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            VkPipelineColorBlendAttachmentState cba{};
            cba.blendEnable = VK_TRUE;
            cba.srcColorBlendFactor = additive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_DST_COLOR;
            cba.dstColorBlendFactor = additive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ZERO;
            cba.colorBlendOp = VK_BLEND_OP_ADD;
            cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.alphaBlendOp = VK_BLEND_OP_ADD;
            cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
            VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
            cb.attachmentCount = 1; cb.pAttachments = &cba;
            VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
            gp.stageCount = 2; gp.pStages = st; gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia;
            const VkDynamicState dynViewport = VK_DYNAMIC_STATE_VIEWPORT;   // temporal AA jitters the viewport per frame
            VkPipelineDynamicStateCreateInfo dyns{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
            dyns.dynamicStateCount = 1; dyns.pDynamicStates = &dynViewport;
            gp.pDynamicState = &dyns;
            gp.pViewportState = &vps; gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pColorBlendState = &cb;
            const VkPipelineRenderingCreateInfo prci = mc2vk::dynrender::pipelineInfo(1, &s->colorFmt, VK_FORMAT_UNDEFINED);
            gp.pNext = &prci;
            gp.layout = s->playout.get(); gp.renderPass = VK_NULL_HANDLE; gp.subpass = 0;
            if (!mc2vk::create(out, dev, [&](VkPipeline* o) { return mc2vk::infra::graphics(dev, VK_NULL_HANDLE, 1, &gp, nullptr, o); })) return false;
            mc2vk::nameObject(dev, out.get(), name);
            return true;
        };
        if (!make(false, s->mulPipe, "impact.mulPipe") || !make(true, s->addPipe, "impact.addPipe")) return false;
    }
    {
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = 0; pci.queueFamilyIndex = ctx.graphicsFamily();
        for (uint32_t i = 0; i < kSlots; ++i) {
            if (!mc2vk::create(s->cpool[i], dev, [&](auto* o) { return vkCreateCommandPool(dev, &pci, nullptr, o); })) return false;
            VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            cai.commandPool = s->cpool[i].get(); cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
            if (vkAllocateCommandBuffers(dev, &cai, &s->cbuf[i]) != VK_SUCCESS) return false;
        }
    }
    s->inited = true;
    std::fprintf(stderr, "[VK_IMPACT_DECALS] initialized %dx%d: box decals over shared depth/normal, %u-layer procedural atlas, cap %u.\n", w, h,
         impact::kTileCount, impact::kMaxDecals);
    return true;
}

void fillFrame(FrameUbo& u, const gosPostProcess& pp, int w, int h) {
    std::memcpy(u.viewProj, pp.getViewProj(), sizeof(u.viewProj));
    glm::mat4 vp;
    std::memcpy(&vp, pp.getViewProj(), sizeof(vp));
    const glm::mat4 inv = glm::inverse(vp);
    std::memcpy(u.invViewProj, &inv, sizeof(u.invViewProj));
    // The camera is the homogeneous point the projection sends to w = 0 at the screen centre: inv * (0,0,1,0).
    glm::vec4 eye = inv * glm::vec4(0.0f, 0.0f, 1.0f, 0.0f);
    if (std::fabs(eye.w) > 1e-8f) eye /= eye.w;
    else if (eye.z < 0.0f) eye = -eye;   // orthographic: a direction toward the viewer (the camera looks down)
    u.eye[0] = eye.x; u.eye[1] = eye.y; u.eye[2] = eye.z; u.eye[3] = eye.w;
    float lx = 0.0f, ly = 0.0f, lz = -1.0f;
    gos_GetTerrainLightDir(&lx, &ly, &lz);
    const float l = std::sqrt(lx * lx + ly * ly + lz * lz);
    if (l > 1e-6f) { lx /= l; ly /= l; lz /= l; }
    u.lightDir[0] = -lx; u.lightDir[1] = -ly; u.lightDir[2] = -lz; u.lightDir[3] = 0.55f;
    u.screen[0] = static_cast<float>(w); u.screen[1] = static_cast<float>(h); u.screen[2] = 1.0f / static_cast<float>(w); u.screen[3] = 1.0f / static_cast<float>(h);
}

}  // namespace

extern "C" void mc2_vk_impact_decals_destroy() {
    if (g_state) { destroy(g_state.get()); g_state.reset(); }
}

// Compiles both impact pipelines (multiply, additive) and uploads the procedural atlas at load, so the first weapon hit
// records a draw instead of building them. A shared GBuffer that is not ready yet leaves the state to the first real frame.
extern "C" int mc2_vk_impact_decals_prewarm(int w, int h) {
    if (w <= 0 || h <= 0 || !mc2vk::engineVulkanContextValid()) return 0;
    if (!g_state) g_state = std::make_unique<State>();
    State* s = g_state.get();
    if (s->disabled) return 0;
    if (s->inited && s->w == w && s->h == h) return 1;
    if (s->inited) destroy(s);
    const Mc2ImpactDecalHealth before = g_health;
    if (build(s, w, h)) return 1;
    destroy(s);   // not ready (or failed): the first hit retries and owns the decline accounting
    g_health = before;
    return 0;
}

extern "C" int mc2_vk_impact_decals_run(int w, int h) {
    const uint32_t count = static_cast<uint32_t>(g_frame.size());
    g_health.lastCount = count; g_health.lastHot = g_frameHot;
    if (count == 0) { ++g_health.framesEmpty; return 0; }
    if (w <= 0 || h <= 0) { decline("no_viewport"); return 0; }
    gosPostProcess* pp = getGosPostProcess();
    if (!pp) { decline("no_postprocess"); return 0; }
    if (!g_state) g_state = std::make_unique<State>();
    State* s = g_state.get();
    if (s->disabled) return 0;
    if (s->inited && (s->w != w || s->h != h)) destroy(s);
    if (!s->inited) {
    ZoneScopedN("VK-impact-decals.LazyBuild");
    if (!build(s, w, h)) {
        std::fprintf(stderr, "[VK_IMPACT_DECALS] init failed (%s): impact decals stay off this session.\n", g_health.reason);
        destroy(s); s->disabled = true; return 0;
    }
    }
    ZoneScopedN("VK-impact-decals");
    mc2vk::VulkanContext& ctx = mc2vk::getOrCreateEngineVulkanContext();
    // The context retired this frame slot; retain the ticket wait for every resource reuse.
    const uint32_t slot = ctx.frameContext().slot;
    { ZoneScopedN("VK-impact-decals.SlotWait");
    if (!ctx.wait(mc2vk::GpuTicket{s->ticket[slot], 0}, 5ull * 1000 * 1000 * 1000, "impact_decals_slot")) {
        decline("slot_wait_timeout"); s->disabled = true; return 0;
    }
    }
    {
        ZoneScopedN("VK-impact-decals.PrepareAndUpload");
        FrameUbo u{};
        fillFrame(u, *pp, w, h);
        // Rebind the atlas views when residency swaps the placeholder for the uploaded images.
        const VkImageView views[2] = {s->surfaceAtlas.view(), s->normalAtlas.view()};
        if (views[0] != s->boundAtlas[slot][0] || views[1] != s->boundAtlas[slot][1]) {
            VkDescriptorImageInfo img[2] = {{s->atlasSampler.get(), views[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                            {s->atlasSampler.get(), views[1], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
            VkWriteDescriptorSet wr[2]{};
            for (uint32_t k = 0; k < 2; ++k) {
                wr[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[k].dstSet = s->set[slot]; wr[k].dstBinding = 2 + k;
                wr[k].descriptorCount = 1; wr[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr[k].pImageInfo = &img[k];
            }
            vkUpdateDescriptorSets(s->device, 2, wr, 0, nullptr);
            s->boundAtlas[slot][0] = views[0]; s->boundAtlas[slot][1] = views[1];
        }
        if (void* m = s->ubo[slot].map()) { std::memcpy(m, &u, sizeof(u)); s->ubo[slot].unmap(); } else { decline("ubo_map"); return 0; }
        if (void* m = s->ssbo[slot].map()) { std::memcpy(m, g_frame.data(), static_cast<size_t>(count) * sizeof(impact::GpuDecal)); s->ssbo[slot].unmap(); }
        else { decline("ssbo_map"); return 0; }
    }

    VkCommandBuffer cb = s->cbuf[slot];
    { ZoneScopedN("VK-impact-decals.Record");
    vkResetCommandPool(s->device, s->cpool[slot].get(), 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) { decline("record_failed"); return 0; }
    auto barrier = [&](VkImage image, VkImageAspectFlags aspect, VkImageLayout oldL, VkImageLayout newL,
                       VkAccessFlags srcA, VkAccessFlags dstA, VkPipelineStageFlags srcS, VkPipelineStageFlags dstS) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = oldL; b.newLayout = newL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image; b.subresourceRange = {aspect, 0, 1, 0, 1};
        b.srcAccessMask = srcA; b.dstAccessMask = dstA;
        vkCmdPipelineBarrier(cb, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    const VkImageAspectFlags depthAspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    barrier(s->depthRes->image.get(), depthAspect, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    barrier(s->normalRes->image.get(), VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    {
        MC2_VK_GPU_SCOPE(gpuScope, cb, "VKIsl.ImpactDecals");
        {   // the external dependency the render pass carried: colour writes ordered against this pass's blend reads/writes
            constexpr VkPipelineStageFlags2 kStage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            constexpr VkAccessFlags2 kRW = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            mc2vk::dynrender::memoryBarrier(ctx, cb, kStage, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, kStage, kRW);
        }
        const VkImageView colorView = s->colorRes->view.get();
        mc2vk::dynrender::beginLoad(ctx, cb, static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1, &colorView, VK_NULL_HANDLE, VK_FORMAT_UNDEFINED);
        mc2vk::infra::setSceneViewport(cb, static_cast<uint32_t>(w), static_cast<uint32_t>(h));
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->playout.get(), 0, 1, &s->set[slot], 0, nullptr);
        PushMode pm{};
        mc2vk::infra::bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->mulPipe.get());
        vkCmdPushConstants(cb, s->playout.get(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pm), &pm);
        vkCmdDraw(cb, 36, count, 0, 0);
        if (g_frameHot) {
            pm.mode = 1;
            mc2vk::infra::bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->addPipe.get());
            vkCmdPushConstants(cb, s->playout.get(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pm), &pm);
            vkCmdDraw(cb, 36, g_frameHot, 0, 0);
        }
        mc2vk::dynrender::end(ctx, cb);
        {   // the pass's closing dependency (colour writes visible to later colour reads/writes)
            constexpr VkPipelineStageFlags2 kStage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            mc2vk::dynrender::memoryBarrier(ctx, cb, kStage, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, kStage,
                                            VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        }
        gpuScope.end();
    }
    barrier(s->depthRes->image.get(), depthAspect, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
    barrier(s->normalRes->image.get(), VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    if (vkEndCommandBuffer(cb) != VK_SUCCESS) { decline("record_failed"); return 0; }
    }
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    { ZoneScopedN("VK-impact-decals.Submit");
    if (ctx.submit(si, VK_NULL_HANDLE, &s->ticket[slot]) != VK_SUCCESS) { decline("submit_failed"); s->disabled = true; return 0; }
    }
    ++g_health.framesDrawn;
    if ((g_health.framesDrawn % 600) == 1)
        std::fprintf(stderr, "[VK_IMPACT_DECALS] drawn: frames=%lu decals=%u hot=%u declined=%lu\n", g_health.framesDrawn, count, g_frameHot, g_health.declined);
    return 1;
}

