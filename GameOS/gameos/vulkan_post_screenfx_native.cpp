// VK-POST-SCREENFX-NATIVE-1 (rung 8.3, VK full-ownership ladder — .claude/VK-RUNG8-LADDER-1.md).
//
// CLOUD-SHADOW + SHORELINE-FOAM rendered natively by VK on the SHARED ENGINE
// VulkanContext, reading the shared scene GBuffer and multiplying into the
// shared scene color0 IN PLACE — zero copies, zero CPU readbacks, no private
// device (rung-8.1 fog / 8.2 SSAO pattern; both passes in ONE render pass /
// ONE submit since they are adjacent in the GL endScene tail):
//
//   * cloud-shadow: samples the SHARED scene depth (depth-aspect view), fbm
//     cloud factor from the reconstructed world position, multiplies
//     (DST_COLOR/ZERO — GL's PostProcessCloudShadow Multiply row) into color0.
//     Shader = shaders/vulkan/cloud_shadow.frag, a VERBATIM port of
//     shaders/cloud.frag (glsl_port_diff pairs cloudShadowNoise/cloudShadowMain).
//   * shoreline-foam: samples the SHARED scene normal (GBuffer1 — the .a water
//     tag), brightens water pixels at the land boundary (multiply with >1
//     values — GL's PostProcessShoreline Multiply row). Shader =
//     shaders/vulkan/shoreline.frag, VERBATIM port of shaders/shoreline.frag
//     (+ the noise.hglsl fbm it #includes; pairs shorelineNoise/shorelineMain).
//
// SCREEN-SHADOW (the third pass the ladder row names) is NOT ported in this
// slice — honest skip, documented in the ladder: shadow_screen.frag's dominant
// inputs are the GL-produced CSM cascade 2D_ARRAY + full-map cascade, which
// have NO zero-copy VK producer today (the terrain island imports the array via
// per-frame glGetTexImage — a documented cost). A native screen-shadow port
// would ADD that per-frame CPU readback tax to the tail, inverting the rung's
// purpose. It stays GL until ladder rung 2 (VK shadow ownership) makes the
// cascades VK-produced.
//
// GATES: master MC2_VK_POST_SCREENFX (explicit OPT-IN: an Exact1 row, only "1" arms it,
// default-OFF; unset -> OFF regardless of MC2_VK_SHARED_GBUFFER). Per-pass bisect sub-gates MC2_VK_POST_SCREENFX_CLOUD /
// MC2_VK_POST_SCREENFX_SHORELINE: explicit 0 forces that pass back to GL,
// unset/1 follows the master. Each pass additionally runs ONLY when its GL
// body would run (cloud: enableCloudShadow_+sceneHasTerrain_+prog valid;
// shoreline: shorelineEnabled_+sceneHasTerrain_+prog valid+!terrain-mask
// suppression); ANY not-ready condition falls that pass through to the
// unchanged GL body (fail-soft, gate-OFF byte-identical).
//
// SYNC (8.2 SSAO pattern — three shared resources): per frame BeginFrame on
// color0 + normal + depth, GL signals color0/normal COLOR_ATTACHMENT + depth
// DEPTH_STENCIL_ATTACHMENT, VK waits all GL->VK sems, in-cb barriers move
// depth DS->SHADER_READ_ONLY and normal COLOR_ATTACHMENT->SHADER_READ_ONLY
// for sampling and BACK after the pass (layout-stable across the API
// boundary), the render pass is LOAD/STORE color-attachment-stable on color0,
// submit signals the VK->GL sems, GL waits them; fence CPU-wait as the
// slot-recycle guard.
//
// The whole TU compiles to nothing unless MC2_VULKAN_ISLAND is defined.

#include "gos_postprocess.h"
#include "Gates/gate.h"
#include "Gates/gate_cold.h"   // str<>()
#include "Gates/render_options.h"   // options.cfg PropShadowReceive

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
#include "vulkan_context.h"
#include "vk_island_util.h"
#define MC2_VULKAN_SHARED_SCENE_COLOR_IMPL_ISLAND
#include "vulkan_shared_scene_color.h"
#define MC2_VULKAN_SHARED_SCENE_DEPTH_IMPL_ISLAND
#include "vulkan_shared_scene_depth.h"
#include "vulkan_shadow_island_live.h"
#include "vk_shared_gbuffer_gate.h"

// Fixed-timestep clock so the animated cloud/foam time matches the GL path under smoke.
#include "gos_smoke.h"
#include <SDL2/SDL.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>
#include <vector>

extern void gos_GetTerrainLightDir(float* x, float* y, float* z);

namespace {

void vlog(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vk_island_util::vlogv512("[VK_POST_SCREENFX_NATIVE] ", fmt, ap);
    va_end(ap);
}

// Health snapshot — read by debug_state_dump.cpp via the extern "C" getter below
// (used_vulkan receipt for every VK capture, per the arc's evidence protocol).
struct PostScreenFxHealth {
    int  runtimeGate   = 0;   // MC2_VK_POST_SCREENFX resolved ON
    int  cloudGate     = 0;   // sub-gate resolution (1 = pass may go native)
    int  shoreGate     = 0;
    int  shadowGate    = 0;
    int  substrateLive = 0;   // shared color0 + normal + depth present at init
    int  directChain   = 0;   // direct-chain engaged
    unsigned long attempted   = 0;  // frames the native path was entered
    unsigned long usedVulkan  = 0;  // frames at least one pass rendered via VK
    unsigned long usedInterop = 0;  // frames that used GL<->VK semaphores
    unsigned long cloudFrames = 0;  // frames the cloud pass rendered via VK
    unsigned long shoreFrames = 0;  // frames the shoreline pass rendered via VK
    unsigned long shadowFrames = 0; // frames the static-only screen-shadow pass rendered via VK
    unsigned long glSkipped   = 0;  // GL pass bodies skipped (1 per pass per frame)
    char fallbackReason[64]   = {0};
    double renderUs = 0.0;          // record+submit+sem/fence wait, last frame
    // VK-QWI-TO-FENCE-1: water-style us_fence_wait telemetry around the per-frame
    // CPU-blocking queue waits (QWI before the conversion, per-submit fence after).
    double waitUsLast = 0.0;
    double waitUsAccum = 0.0;
    unsigned long waitSamples = 0;
};
PostScreenFxHealth g_fx;

void setFallback(const char* reason) {
    std::strncpy(g_fx.fallbackReason, reason ? reason : "",
                 sizeof(g_fx.fallbackReason) - 1);
    g_fx.fallbackReason[sizeof(g_fx.fallbackReason) - 1] = '\0';
}

float shadow_softness_default() {
    static const float s_s = mc2::gate::get<"MC2_SHADOW_CSM_SOFTNESS">();   // row: default 0.9, clamps to [0.1, 6]
    return s_s;
}

std::string spv_dir() {
    std::string d = mc2::gate::str<"MC2_VULKAN_SPV_DIR">();   // row default shaders/vulkan, empty reads the default
    if (!d.empty() && d.back() != '/' && d.back() != '\\') d += '/';
    return d;
}

VkShaderModule load_spv(VkDevice device, const std::string& path) {
    return vk_island_util::loadSpv(device, path, vlog);
}

// std140 PODs — offsets cross-checked offline by tools/check_ubo_layout.py
// (labels screenfx_cloud / screenfx_shore). invViewProj: COLUMN-major raw
// memcpy of inverseViewProj_ — the GL cloud pass uploads it with a single
// glUniformMatrix4fv(GL_FALSE) (column-major read, same convention as the
// SSAO path; see ssao.frag N0 FIX note — do NOT transpose).
struct ScreenFxCloudParams {
    float invViewProj[16];   // off 0   (column-major, matches the GL GL_FALSE upload)
    float cloudScroll[2];    // off 64
    float cloudThreshold[2]; // off 72
    float timeSec;           // off 80
    float cloudScale;        // off 84
    float cloudStrength;     // off 88
    int   cloudOctaves;      // off 92
    int   cloudEnable;       // off 96
    int   _pad0;             // off 100 (std140 rounds the block to 112)
    int   _pad1;             // off 104
    int   _pad2;             // off 108
};
static_assert(sizeof(ScreenFxCloudParams) == 112, "ScreenFxCloudParams std140 offsets drifted");

struct ScreenFxShoreParams {
    float screenSize[2];     // off 0
    float timeSec;           // off 8
    float _pad0;             // off 12 (std140 rounds the block to 16)
};
static_assert(sizeof(ScreenFxShoreParams) == 16, "ScreenFxShoreParams std140 offsets drifted");

struct ScreenFxShadowStaticParams {
    float invViewProj[16];       // off 0
    float lightSpaceMatrix[16];  // off 64
    float dynamicCascadeMatrices[16 * 3]; // off 128
    float dynamicCascadeTexelWorld[4];    // off 320
    float screenSize[2];         // off 336
    int   enableShadows;         // off 344
    int   enableDynamicShadows;  // off 348
    int   dynamicCsmCount;       // off 352
    int   debugMode;             // off 356
    float shadowSoftness;        // off 360
    float csmDepthSpan;          // off 364
    float dynamicFullMapTexelWorld; // off 368
    float objNormalBiasScale;    // off 372
    float _padBeforeLight[2];    // off 376..380
    float lightDir[4];           // off 384, xyz used
    float mechSoft;              // off 400
    int   propShadowReceive;     // off 404 (options.cfg PropShadowReceive)
    float _pad1, _pad2;          // off 408..412
};
static_assert(sizeof(ScreenFxShadowStaticParams) == 416, "ScreenFxShadowStaticParams std140 offsets drifted");

} // namespace

// Persistent state. Lazy init at the screenfx pass sites; torn down by
// destroyFBOs (resize) and destroy() via destroyVkPostScreenFxNative().
struct gosPostProcess::VkPostScreenFx {
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
    VkImageResource* sColorRes  = nullptr;
    VkImageResource* sNormalRes = nullptr;
    VkImageResource* sDepthRes  = nullptr;
    VkImageView      csmArrayView = VK_NULL_HANDLE;    // borrowed; shadow island owns it
    VkImageView      csmFullMapView = VK_NULL_HANDLE;  // borrowed; shadow island owns it
    // VK-RUNG8-ITEM5: shared scene is VK-only (no GL view); direct-chain only.
    // VK-RUNG8-ITEM6: the static-shadow import substrate was RETIRED. A non-CSM
    // static-only screen-shadow config now DECLINES to the GL screen-shadow pass
    // (hasStaticShadow is always false below); CSM screen-shadow is unaffected
    // (it samples the direct VK CSM array/full-map views).

    VkDescriptorSet  cloudSet = VK_NULL_HANDLE;   // allocated from `pool`, freed with it
    VkDescriptorSet  shoreSet = VK_NULL_HANDLE;
    VkDescriptorSet  shadowSet = VK_NULL_HANDLE;
    VkCommandBuffer  cbuf[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE}; // per parity slot; allocated from `cpool`, freed with it

    // OWNED objects, declared in creation order: the state is moved into a scope-local in destroy_state so the destructor releases them in REVERSE (a member-wise move-assignment would release forward); the owner idles the GPU first.
    mc2vk::OwnedImageView   depthSampleView;       // depth-aspect view over sDepthRes->image
    mc2vk::OwnedSampler     gbufSampler;           // NEAREST/CLAMP (GL sceneDepthTex_/sceneNormalTex_ parity)
    mc2vk::OwnedSampler     shadowCompareSampler;
    mc2vk::OwnedBuffer      cloudUbo;
    mc2vk::OwnedBuffer      shoreUbo;
    mc2vk::OwnedBuffer      shadowUbo;
    mc2vk::OwnedRenderPass  rpass;                 // shared color0, LOAD/STORE layout-stable
    mc2vk::OwnedFramebuffer fb;
    mc2vk::OwnedDescriptorSetLayout cloudDsl;
    mc2vk::OwnedDescriptorSetLayout shoreDsl;
    mc2vk::OwnedDescriptorSetLayout shadowDsl;
    mc2vk::OwnedDescriptorPool pool;
    mc2vk::OwnedPipelineLayout cloudPlayout;
    mc2vk::OwnedPipelineLayout shorePlayout;
    mc2vk::OwnedPipelineLayout shadowPlayout;
    mc2vk::OwnedShaderModule cloudVert, cloudFrag;
    mc2vk::OwnedShaderModule shoreVert, shoreFrag;
    mc2vk::OwnedShaderModule shadowVert, shadowFrag, shadowCsmFrag;
    mc2vk::OwnedPipeline    cloudPipe;
    mc2vk::OwnedPipeline    shorePipe;
    mc2vk::OwnedPipeline    shadowPipe;
    mc2vk::OwnedPipeline    shadowCsmPipe;
    mc2vk::OwnedCommandPool cpool;
    mc2vk::OwnedFence       submitFence;           // VK-QWI-TO-FENCE-1: per-submit retire fence (shared by both submits; wait+reset after each)
};

static bool vkPostScreenFxDirectChainEnabled();

namespace {

bool make_buffer(gosPostProcess::VkPostScreenFx* s, mc2vk::OwnedBuffer& out, VkDeviceSize size,
                 VkBufferUsageFlags usage, std::string_view name, int i0 = -1, int i1 = -1) {
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size; bci.usage = usage;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    if (!out.create(s->allocator, s->device, bci, aci, name, i0, i1)) {
        vlog("buffer creation failed (%.*s).", static_cast<int>(name.size()), name.data()); return false;
    }
    return true;
}

// Releases the owned objects in reverse declaration order (the moved-to local's destructor does it, once the GPU is idle) and resets the state; the borrowed handles are not touched.
void destroy_state(gosPostProcess::VkPostScreenFx* s) {
    if (!s) return;
    if (s->device != VK_NULL_HANDLE) mc2vk::getOrCreateEngineVulkanContext().waitIdle("post_screenfx_destroy");
    { auto dying = std::move(*s); (void)dying; }
    const bool disabled = s->disabled;
    *s = gosPostProcess::VkPostScreenFx{};
    s->disabled = disabled;
}

bool build_state(gosPostProcess::VkPostScreenFx* s, int w, int h) {
    s->width = w; s->height = h;

    // Shared engine device, on-demand (rung-8.1 model; fail-soft -> GL passes).
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

    // Borrow the shared GBuffer (color0 write target, normal + depth sample sources).
    s->sColorRes   = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_COLOR0);
    s->sNormalRes  = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_NORMAL);
    s->sDepthRes   = mc2SharedSceneDepthGetResource();
    // VK-RUNG8 item-4/6: direct-chain only. Only the shared VkImageResources are required.
    // Static-only screen-shadow declines to GL below.
    if (!s->sColorRes || !s->sNormalRes || !s->sDepthRes) {
        setFallback("shared_gbuffer_not_ready"); return false;
    }
    if ((int)s->sColorRes->w  != w || (int)s->sColorRes->h  != h ||
        (int)s->sNormalRes->w != w || (int)s->sNormalRes->h != h ||
        (int)s->sDepthRes->w  != w || (int)s->sDepthRes->h  != h) {
        setFallback("shared_gbuffer_size_mismatch"); return false;
    }
    g_fx.substrateLive = 1;

    VkResult r;

    // Depth-aspect sampling view over the SHARED depth image (fog/ssao pattern).
    {
        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = s->sDepthRes->image.get();
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = s->sDepthRes->format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        r = mc2vk::createChecked(s->depthSampleView, s->device, [&](auto* h) { return vkCreateImageView(s->device, &vci, nullptr, h); });
        if (r != VK_SUCCESS) { vlog("depth sample view create failed (%d).", (int)r); return false; }
    }
    // Sampler: NEAREST/CLAMP for depth + normal (GL parity — both GL textures
    // are sampled unfiltered at texel centers by these fullscreen passes).
    {
        VkSamplerCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter = VK_FILTER_NEAREST; sci.minFilter = VK_FILTER_NEAREST;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (!mc2vk::create(s->gbufSampler, s->device, [&](VkSampler* h) { return vkCreateSampler(s->device, &sci, nullptr, h); })) {
            vlog("vkCreateSampler failed."); return false;
        }
    }
    {
        VkSamplerCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        sci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        sci.compareEnable = VK_TRUE;
        sci.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        if (!mc2vk::create(s->shadowCompareSampler, s->device, [&](VkSampler* h) { return vkCreateSampler(s->device, &sci, nullptr, h); })) {
            vlog("vkCreateSampler(static shadow compare) failed."); return false;
        }
    }

    if (!make_buffer(s, s->cloudUbo, sizeof(ScreenFxCloudParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, "screenfx.cloudUbo")) return false;
    if (!make_buffer(s, s->shoreUbo, sizeof(ScreenFxShoreParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, "screenfx.shoreUbo")) return false;
    if (!make_buffer(s, s->shadowUbo, sizeof(ScreenFxShadowStaticParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, "screenfx.shadowUbo")) return false;

    // Render pass: 1 attachment = SHARED scene color0. LOAD (multiply blends
    // against the scene), STORE; layout-STABLE COLOR_ATTACHMENT both ends
    // (8.1/8.2 pattern). Both draws live in this one pass.
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
        r = mc2vk::createChecked(s->rpass, s->device, [&](auto* h) { return vkCreateRenderPass(s->device, &rpci, nullptr, h); });
        if (r != VK_SUCCESS) { vlog("vkCreateRenderPass failed (%d).", (int)r); return false; }

        VkFramebufferCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = s->rpass.get();
        const VkImageView colorAttachment = s->sColorRes->view.get();
        fci.attachmentCount = 1; fci.pAttachments = &colorAttachment;
        fci.width = (uint32_t)w; fci.height = (uint32_t)h; fci.layers = 1;
        r = mc2vk::createChecked(s->fb, s->device, [&](auto* h) { return vkCreateFramebuffer(s->device, &fci, nullptr, h); });
        if (r != VK_SUCCESS) { vlog("vkCreateFramebuffer failed (%d).", (int)r); return false; }
    }

    // Descriptor layouts / pool / sets.
    // Cloud (cloud_shadow.frag): 0=depth sampler, 1=UBO.
    // Shore (shoreline.frag):    0=depth sampler (declared-unused, interface
    //                            parity), 1=normal sampler, 2=UBO.
    {
        VkDescriptorSetLayoutBinding cloudBinds[2]{};
        cloudBinds[0].binding = 0; cloudBinds[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        cloudBinds[0].descriptorCount = 1; cloudBinds[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        cloudBinds[1].binding = 1; cloudBinds[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        cloudBinds[1].descriptorCount = 1; cloudBinds[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.bindingCount = 2; lci.pBindings = cloudBinds;
        r = mc2vk::createChecked(s->cloudDsl, s->device, [&](auto* h) { return vkCreateDescriptorSetLayout(s->device, &lci, nullptr, h); });
        if (r != VK_SUCCESS) { vlog("vkCreateDescriptorSetLayout(cloud) failed (%d).", (int)r); return false; }

        VkDescriptorSetLayoutBinding shoreBinds[3]{};
        shoreBinds[0].binding = 0; shoreBinds[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        shoreBinds[0].descriptorCount = 1; shoreBinds[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        shoreBinds[1].binding = 1; shoreBinds[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        shoreBinds[1].descriptorCount = 1; shoreBinds[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        shoreBinds[2].binding = 2; shoreBinds[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        shoreBinds[2].descriptorCount = 1; shoreBinds[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        lci.bindingCount = 3; lci.pBindings = shoreBinds;
        r = mc2vk::createChecked(s->shoreDsl, s->device, [&](auto* h) { return vkCreateDescriptorSetLayout(s->device, &lci, nullptr, h); });
        if (r != VK_SUCCESS) { vlog("vkCreateDescriptorSetLayout(shore) failed (%d).", (int)r); return false; }

        VkDescriptorSetLayoutBinding shadowBinds[6]{};
        shadowBinds[0].binding = 0; shadowBinds[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        shadowBinds[0].descriptorCount = 1; shadowBinds[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        shadowBinds[1].binding = 1; shadowBinds[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        shadowBinds[1].descriptorCount = 1; shadowBinds[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        shadowBinds[2].binding = 2; shadowBinds[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        shadowBinds[2].descriptorCount = 1; shadowBinds[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        shadowBinds[3].binding = 3; shadowBinds[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        shadowBinds[3].descriptorCount = 1; shadowBinds[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        shadowBinds[4].binding = 4; shadowBinds[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        shadowBinds[4].descriptorCount = 1; shadowBinds[4].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        shadowBinds[5].binding = 5; shadowBinds[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        shadowBinds[5].descriptorCount = 1; shadowBinds[5].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        lci.bindingCount = 6; lci.pBindings = shadowBinds;
        r = mc2vk::createChecked(s->shadowDsl, s->device, [&](auto* h) { return vkCreateDescriptorSetLayout(s->device, &lci, nullptr, h); });
        if (r != VK_SUCCESS) { vlog("vkCreateDescriptorSetLayout(shadow) failed (%d).", (int)r); return false; }

        VkDescriptorPoolSize sizes[2]{};
        sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; sizes[0].descriptorCount = 8;
        sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;         sizes[1].descriptorCount = 3;
        VkDescriptorPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.maxSets = 3; pci.poolSizeCount = 2; pci.pPoolSizes = sizes;
        r = mc2vk::createChecked(s->pool, s->device, [&](auto* h) { return vkCreateDescriptorPool(s->device, &pci, nullptr, h); });
        if (r != VK_SUCCESS) { vlog("vkCreateDescriptorPool failed (%d).", (int)r); return false; }

        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = s->pool.get(); dsai.descriptorSetCount = 1;
        const VkDescriptorSetLayout cloudDslHandle = s->cloudDsl.get();
        const VkDescriptorSetLayout shoreDslHandle = s->shoreDsl.get();
        const VkDescriptorSetLayout shadowDslHandle = s->shadowDsl.get();
        dsai.pSetLayouts = &cloudDslHandle;
        r = vkAllocateDescriptorSets(s->device, &dsai, &s->cloudSet);
        if (r != VK_SUCCESS) { vlog("vkAllocateDescriptorSets(cloud) failed (%d).", (int)r); return false; }
        dsai.pSetLayouts = &shoreDslHandle;
        r = vkAllocateDescriptorSets(s->device, &dsai, &s->shoreSet);
        if (r != VK_SUCCESS) { vlog("vkAllocateDescriptorSets(shore) failed (%d).", (int)r); return false; }
        dsai.pSetLayouts = &shadowDslHandle;
        r = vkAllocateDescriptorSets(s->device, &dsai, &s->shadowSet);
        if (r != VK_SUCCESS) { vlog("vkAllocateDescriptorSets(shadow) failed (%d).", (int)r); return false; }

        VkDescriptorImageInfo depthInfo{};
        depthInfo.sampler = s->gbufSampler.get();
        depthInfo.imageView = s->depthSampleView.get();
        depthInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo normalInfo{};
        normalInfo.sampler = s->gbufSampler.get();
        normalInfo.imageView = s->sNormalRes->view.get();
        normalInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo staticShadowInfo{};
        staticShadowInfo.sampler = s->shadowCompareSampler.get();
        // Binding 7 (static-shadow slot) is statically present in both shadow shaders.
        // VK-RUNG8-ITEM6: the static-shadow import substrate is retired, so this slot
        // always binds a valid depth view and the static-shadow branch is driven OFF
        // via the UBO (enableShadows=0). This descriptor set is only built/used when
        // CSM direct-chain is available; static-only configs decline to GL earlier.
        staticShadowInfo.imageView = s->depthSampleView.get();
        staticShadowInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorBufferInfo cloudUboInfo{ s->cloudUbo.get(), 0, VK_WHOLE_SIZE };
        VkDescriptorBufferInfo shoreUboInfo{ s->shoreUbo.get(), 0, VK_WHOLE_SIZE };
        VkDescriptorBufferInfo shadowUboInfo{ s->shadowUbo.get(), 0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet writes[9]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = s->cloudSet; writes[0].dstBinding = 0; writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &depthInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = s->cloudSet; writes[1].dstBinding = 1; writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[1].pBufferInfo = &cloudUboInfo;
        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = s->shoreSet; writes[2].dstBinding = 0; writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[2].pImageInfo = &depthInfo;
        writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[3].dstSet = s->shoreSet; writes[3].dstBinding = 1; writes[3].descriptorCount = 1;
        writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[3].pImageInfo = &normalInfo;
        writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[4].dstSet = s->shoreSet; writes[4].dstBinding = 2; writes[4].descriptorCount = 1;
        writes[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[4].pBufferInfo = &shoreUboInfo;
        writes[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[5].dstSet = s->shadowSet; writes[5].dstBinding = 0; writes[5].descriptorCount = 1;
        writes[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[5].pImageInfo = &depthInfo;
        writes[6].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[6].dstSet = s->shadowSet; writes[6].dstBinding = 1; writes[6].descriptorCount = 1;
        writes[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[6].pImageInfo = &normalInfo;
        writes[7].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[7].dstSet = s->shadowSet; writes[7].dstBinding = 2; writes[7].descriptorCount = 1;
        writes[7].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[7].pImageInfo = &staticShadowInfo;
        writes[8].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[8].dstSet = s->shadowSet; writes[8].dstBinding = 3; writes[8].descriptorCount = 1;
        writes[8].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[8].pBufferInfo = &shadowUboInfo;
        vkUpdateDescriptorSets(s->device, 9, writes, 0, nullptr);
    }

    // Two pipelines — fullscreen tri, no vertex input, cull none, MULTIPLY
    // blend (DST_COLOR/ZERO color, DST_ALPHA/ZERO alpha — GL's
    // glBlendFunc(GL_DST_COLOR, GL_ZERO) BLENDMODE-MULTIPLY-1 rows for BOTH
    // the cloud and shoreline passes).
    {
        s->cloudVert = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir() + "cloud_shadow.vert.spv"));
        s->cloudFrag = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir() + "cloud_shadow.frag.spv"));
        s->shoreVert = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir() + "shoreline.vert.spv"));
        s->shoreFrag = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir() + "shoreline.frag.spv"));
        s->shadowVert = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir() + "screen_shadow_static.vert.spv"));
        s->shadowFrag = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir() + "screen_shadow_static.frag.spv"));
        s->shadowCsmFrag = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir() + "screen_shadow_csm.frag.spv"));
        if (!s->cloudVert || !s->cloudFrag || !s->shoreVert || !s->shoreFrag ||
            !s->shadowVert || !s->shadowFrag || !s->shadowCsmFrag) {
            setFallback("spv_load_failed"); return false;
        }
        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        const VkDescriptorSetLayout cloudDslForLayout = s->cloudDsl.get();
        const VkDescriptorSetLayout shoreDslForLayout = s->shoreDsl.get();
        const VkDescriptorSetLayout shadowDslForLayout = s->shadowDsl.get();
        plci.setLayoutCount = 1; plci.pSetLayouts = &cloudDslForLayout;
        r = mc2vk::createChecked(s->cloudPlayout, s->device, [&](auto* h) { return vkCreatePipelineLayout(s->device, &plci, nullptr, h); });
        if (r != VK_SUCCESS) { vlog("vkCreatePipelineLayout(cloud) failed (%d).", (int)r); return false; }
        plci.pSetLayouts = &shoreDslForLayout;
        r = mc2vk::createChecked(s->shorePlayout, s->device, [&](auto* h) { return vkCreatePipelineLayout(s->device, &plci, nullptr, h); });
        if (r != VK_SUCCESS) { vlog("vkCreatePipelineLayout(shore) failed (%d).", (int)r); return false; }
        plci.pSetLayouts = &shadowDslForLayout;
        r = mc2vk::createChecked(s->shadowPlayout, s->device, [&](auto* h) { return vkCreatePipelineLayout(s->device, &plci, nullptr, h); });
        if (r != VK_SUCCESS) { vlog("vkCreatePipelineLayout(shadow) failed (%d).", (int)r); return false; }

        auto make_pipeline = [&](VkShaderModule vmod, VkShaderModule fmod,
                                 VkPipelineLayout layout, mc2vk::OwnedPipeline& out, std::string_view name) -> bool {
            VkPipelineShaderStageCreateInfo stages[2]{};
            stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vmod; stages[0].pName = "main";
            stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fmod; stages[1].pName = "main";
            VkPipelineVertexInputStateCreateInfo vin{};
            vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
            VkPipelineInputAssemblyStateCreateInfo ia{};
            ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
            ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkViewport vp{0.0f, 0.0f, (float)s->width, (float)s->height, 0.0f, 1.0f};
            VkRect2D sc{{0, 0}, {(uint32_t)s->width, (uint32_t)s->height}};
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
            cba.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
            cba.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
            cba.colorBlendOp = VK_BLEND_OP_ADD;
            cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_DST_ALPHA;
            cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            cba.alphaBlendOp = VK_BLEND_OP_ADD;
            cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                 VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            VkPipelineColorBlendStateCreateInfo cb{};
            cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
            cb.attachmentCount = 1; cb.pAttachments = &cba;
            VkGraphicsPipelineCreateInfo gp{};
            gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            gp.stageCount = 2; gp.pStages = stages;
            gp.pVertexInputState = &vin;
            gp.pInputAssemblyState = &ia;
            gp.pViewportState = &vps;
            gp.pRasterizationState = &rs;
            gp.pMultisampleState = &ms;
            gp.pColorBlendState = &cb;
            gp.layout = layout;
            gp.renderPass = s->rpass.get();
            gp.subpass = 0;
            if (!mc2vk::create(out, s->device, [&](VkPipeline* h) { return vkCreateGraphicsPipelines(s->device, VK_NULL_HANDLE, 1, &gp, nullptr, h); })) {
                vlog("vkCreateGraphicsPipelines failed."); return false;
            }
            mc2vk::nameObject(s->device, out.get(), name);
            return true;
        };
        if (!make_pipeline(s->cloudVert.get(), s->cloudFrag.get(), s->cloudPlayout.get(), s->cloudPipe, "screenfx.cloudPipe")) return false;
        if (!make_pipeline(s->shoreVert.get(), s->shoreFrag.get(), s->shorePlayout.get(), s->shorePipe, "screenfx.shorePipe")) return false;
        if (!make_pipeline(s->shadowVert.get(), s->shadowFrag.get(), s->shadowPlayout.get(), s->shadowPipe, "screenfx.shadowPipe")) return false;
        if (!make_pipeline(s->shadowVert.get(), s->shadowCsmFrag.get(), s->shadowPlayout.get(), s->shadowCsmPipe, "screenfx.shadowCsmPipe")) return false;
    }

    {
        VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = s->gfxFamily;
        VkResult rr = mc2vk::createChecked(s->cpool, s->device, [&](auto* h) { return vkCreateCommandPool(s->device, &pci, nullptr, h); });
        if (rr != VK_SUCCESS) { vlog("vkCreateCommandPool failed (%d).", (int)rr); return false; }
        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = s->cpool.get();
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 2;
        rr = vkAllocateCommandBuffers(s->device, &cai, s->cbuf);
        if (rr != VK_SUCCESS) { vlog("vkAllocateCommandBuffers failed (%d).", (int)rr); return false; }
    }

    s->inited = true;
    setFallback("");
    vlog("initialized %dx%d — NATIVE cloud-shadow + shoreline on the shared engine "
         "context (samples shared depth+normal, multiplies into shared scene color0 "
         "in place; ZERO copies, ZERO readbacks, no private device).", w, h);
    return true;
}

} // namespace

// The owner's deleter: idles the GPU and releases the owned objects before the state itself goes.
void gosPostProcess::VkPostScreenFxDeleter::operator()(VkPostScreenFx* s) const {
    destroy_state(s);
    delete s;
}

// ---------------------------------------------------------------------------
// vkPostScreenFxNativeEnabled(): explicit OPT-IN, default-OFF (an Exact1 row: only "1"
// arms it; unset -> OFF regardless of MC2_VK_SHARED_GBUFFER). Gate name per the rung-8 ladder: MC2_VK_POST_SCREENFX. Per-pass bisect
// sub-gates resolved here too (explicit 0 forces that pass to GL).
// ---------------------------------------------------------------------------
bool gosPostProcess::vkPostScreenFxNativeEnabled()
{
    static int state = -1;
    if (state == -1) {
        state = mc2::gate::get<"MC2_VK_POST_SCREENFX">() ? 1 : 0;
        g_fx.runtimeGate = state;
        if (state == 1) {
            g_fx.cloudGate = mc2::gate::get<"MC2_VK_POST_SCREENFX_CLOUD">() ? 1 : 0;
            g_fx.shoreGate = mc2::gate::get<"MC2_VK_POST_SCREENFX_SHORELINE">() ? 1 : 0;
            g_fx.shadowGate = mc2::gate::get<"MC2_VK_POST_SCREENFX_SHADOW">() ? 1 : 0;
            vlog("MC2_VK_POST_SCREENFX=1 — rung-8.3 native screenfx gate ON "
                 "(shadow=%d cloud=%d shoreline=%d; lazy init at the pass site; fail-soft to GL).",
                 g_fx.shadowGate, g_fx.cloudGate, g_fx.shoreGate);
        }
    }
    return state == 1;
}

// Native screenfx runs only in direct-chain, engaged when upstream left the shared GBuffer
// Vulkan-owned (umbrellaOn); otherwise it declines and the GL screenfx
// passes run.
static bool vkPostScreenFxDirectChainEnabled()
{
    static int state = -1;
    if (state == -1) {
        state = mc2vkgate::umbrellaOn() ? 1 : 0;
        g_fx.directChain = state;
        if (state == 1)
            vlog("native screenfx direct-chain: upstream left the shared GBuffer VK-owned; "
                 "no screenfx GL<->VK handoff.");
    }
    return state == 1;
}

bool gosPostProcess::runVkPostScreenShadowStaticNative()
{
    const bool dynamicCasterActive = mc2::gate::get<"MC2_SHADOW_ENABLE">();   // DEFAULT ON; =0 kill-switch
    const bool csmActive = dynamicCasterActive && (dynShadowArrayTex_ != 0);
    const bool csmDirectReady = csmActive
        && mc2_vk_shadow_island_csm_array_ready()
        && mc2_vk_shadow_island_csm_fullmap_ready()
        && mc2_vk_shadow_island_get_csm_array_view() != nullptr
        && mc2_vk_shadow_island_get_csm_fullmap_view() != nullptr;
    const bool dynamicActive = dynamicCasterActive && (csmActive || dynShadowDepthTex_ != 0);
    const bool wantShadow = g_fx.shadowGate != 0
        && screenShadowEnabled_ && sceneHasTerrain_
        && screenShadowProg_ && screenShadowProg_->is_valid()
        && shadowsEnabled_ && (!dynamicActive || csmDirectReady) && screenShadowDebug_ == 0;
    if (!wantShadow || width_ <= 0 || height_ <= 0) return false;

    if (!vkPostScreenFx_) vkPostScreenFx_.reset(new VkPostScreenFx());
    VkPostScreenFx* s = vkPostScreenFx_.get();
    if (s->disabled) return false;
    if (!s->inited || s->width != width_ || s->height != height_) {
        if (s->inited) destroy_state(s);
        if (!build_state(s, width_, height_)) {
            vlog("static screen-shadow init FAILED (reason=%s) -- GL pass runs.",
                 g_fx.fallbackReason[0] ? g_fx.fallbackReason : "unknown");
            destroy_state(s);
            s->disabled = true;
            return false;
        }
    }
    // VK-RUNG8-ITEM6: static-shadow import substrate retired -> never available.
    // A static-only (non-CSM) config therefore declines here (returns false) and the
    // GL screen-shadow pass runs; CSM configs proceed with dynamic shadows only.
    const bool hasStaticShadow = false;
    if ((!hasStaticShadow && !csmDirectReady) || !s->shadowSet ||
        !s->shadowPipe || (csmDirectReady && !s->shadowCsmPipe)) {
        setFallback("static_shadow_substrate_not_ready");
        return false;
    }
    s->csmArrayView = csmDirectReady
        ? reinterpret_cast<VkImageView>(mc2_vk_shadow_island_get_csm_array_view()) : VK_NULL_HANDLE;
    s->csmFullMapView = csmDirectReady
        ? reinterpret_cast<VkImageView>(mc2_vk_shadow_island_get_csm_fullmap_view()) : VK_NULL_HANDLE;

    ScreenFxShadowStaticParams p{};
    std::memcpy(p.invViewProj, inverseViewProj_, 16 * sizeof(float));
    std::memcpy(p.lightSpaceMatrix, staticLightSpaceMatrix_, 16 * sizeof(float));
    if (csmDirectReady) {
        std::memcpy(p.dynamicCascadeMatrices, dynamicCascadeMatrices_, 16 * 3 * sizeof(float));
        std::memcpy(p.dynamicCascadeTexelWorld, dynamicCascadeTexelWorld_, 3 * sizeof(float));
        p.dynamicCsmCount = csmCount_;
        p.csmDepthSpan = csmDepthSpan_;
        p.dynamicFullMapTexelWorld = dynamicFullMapTexelWorld_;
        p.enableDynamicShadows = 1;
        // Same rows as the GL path (gos_postprocess_gates.cpp): default 2, clamps to [0, 16] and default 1, [0, 4].
        p.objNormalBiasScale = mc2::gate::get<"MC2_SHADOW_OBJ_NORMAL_BIAS">();
        p.mechSoft = mc2::gate::get<"MC2_SHADOW_MECH_SOFT">();
        p.propShadowReceive = mc2::options::current().propShadowReceive;   // the option the GL screen shadow reads
        float lx = 0.0f, ly = 0.0f, lz = 1.0f;
        gos_GetTerrainLightDir(&lx, &ly, &lz);
        float len = sqrtf(lx*lx + ly*ly + lz*lz);
        if (len > 1e-6f) { lx /= len; ly /= len; lz /= len; }
        p.lightDir[0] = -lx; p.lightDir[1] = -ly; p.lightDir[2] = -lz; p.lightDir[3] = 0.0f;
    }
    p.screenSize[0] = (float)width_;
    p.screenSize[1] = (float)height_;
    p.enableShadows = hasStaticShadow ? 1 : 0;
    p.debugMode = 0;
    p.shadowSoftness = shadow_softness_default();
    void* m = nullptr;
    if ((m = s->shadowUbo.map()) != nullptr) {
        std::memcpy(m, &p, sizeof(p));
        s->shadowUbo.unmap();
    }
    if (csmDirectReady) {
        VkDescriptorImageInfo csmArrayInfo{};
        csmArrayInfo.sampler = s->shadowCompareSampler.get();
        csmArrayInfo.imageView = s->csmArrayView;
        csmArrayInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo csmFullMapInfo{};
        csmFullMapInfo.sampler = s->shadowCompareSampler.get();
        csmFullMapInfo.imageView = s->csmFullMapView;
        csmFullMapInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = s->shadowSet; writes[0].dstBinding = 4; writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &csmArrayInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = s->shadowSet; writes[1].dstBinding = 5; writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo = &csmFullMapInfo;
        vkUpdateDescriptorSets(s->device, 2, writes, 0, nullptr);
    }

    ++g_fx.attempted;
    using clk = std::chrono::steady_clock;
    const clk::time_point t0 = clk::now();

    // VK-RUNG8 item-4: direct-chain only — engage only when upstream is direct-chain,
    // else decline to the GL screenfx passes. No GL<->VK semaphore handoff.
    if (!vkPostScreenFxDirectChainEnabled()) { setFallback("upstream_not_direct_chain"); return false; }
    VkCommandBuffer cb = s->cbuf[0u];

    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) {
        setFallback("record_failed"); return false;
    }

    auto barrier = [&](VkImage image, VkImageAspectFlags aspect,
                       VkImageLayout oldL, VkImageLayout newL,
                       VkAccessFlags srcA, VkAccessFlags dstA,
                       VkPipelineStageFlags srcS, VkPipelineStageFlags dstS) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = oldL; b.newLayout = newL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {aspect, 0, 1, 0, 1};
        b.srcAccessMask = srcA; b.dstAccessMask = dstA;
        vkCmdPipelineBarrier(cb, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    barrier(s->sDepthRes->image.get(), VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    barrier(s->sNormalRes->image.get(), VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    if (csmDirectReady) {
        if (void* csmArray = mc2_vk_shadow_island_get_csm_array_image()) {
            barrier(reinterpret_cast<VkImage>(csmArray), VK_IMAGE_ASPECT_DEPTH_BIT,
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        }
        if (void* csmFullMap = mc2_vk_shadow_island_get_csm_fullmap_image()) {
            barrier(reinterpret_cast<VkImage>(csmFullMap), VK_IMAGE_ASPECT_DEPTH_BIT,
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        }
    }

    VkRenderPassBeginInfo rbi{};
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = s->rpass.get(); rbi.framebuffer = s->fb.get();
    rbi.renderArea = {{0, 0}, {(uint32_t)s->width, (uint32_t)s->height}};
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, csmDirectReady ? s->shadowCsmPipe.get() : s->shadowPipe.get());
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->shadowPlayout.get(), 0, 1, &s->shadowSet, 0, nullptr);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRenderPass(cb);

    if (csmDirectReady) {
        if (void* csmArray = mc2_vk_shadow_island_get_csm_array_image()) {
            barrier(reinterpret_cast<VkImage>(csmArray), VK_IMAGE_ASPECT_DEPTH_BIT,
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                    VK_ACCESS_SHADER_READ_BIT, 0,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        }
        if (void* csmFullMap = mc2_vk_shadow_island_get_csm_fullmap_image()) {
            barrier(reinterpret_cast<VkImage>(csmFullMap), VK_IMAGE_ASPECT_DEPTH_BIT,
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                    VK_ACCESS_SHADER_READ_BIT, 0,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        }
    }
    barrier(s->sDepthRes->image.get(), VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            VK_ACCESS_SHADER_READ_BIT, 0,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    barrier(s->sNormalRes->image.get(), VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_SHADER_READ_BIT, 0,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

    if (vkEndCommandBuffer(cb) != VK_SUCCESS) {
        setFallback("record_failed"); return false;
    }

    // VK-RUNG8 item-4: direct-chain only — the submit waits/signals no GL<->VK
    // semaphores; the images stay Vulkan-owned for the following native pass.
    // VK-QWI-TO-FENCE-1: dedicated per-submit fence instead of the queue-wide drain
    // (see vulkan_post_fog_native.cpp for the full rationale). Fail-soft to QWI if
    // fence creation ever fails.
    if (!s->submitFence) {
        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        mc2vk::create(s->submitFence, s->device, [&](VkFence* h) { return vkCreateFence(s->device, &fci, nullptr, h); });   // empty on failure: the queue idle stands in
    }
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    if (vkQueueSubmit(s->queue, 1, &si, s->submitFence.get()) != VK_SUCCESS) {
        setFallback("submit_failed"); return false;
    }

    // Slot-recycle guard: this frame's command buffer must be retired before reuse
    // next frame (per-submit fence; QWI only as the fence-creation fail-soft).
    {
        const clk::time_point tWait0 = clk::now();
        if (s->submitFence) {
            const VkFence submitFence = s->submitFence.get();
            if (vkWaitForFences(s->device, 1, &submitFence, VK_TRUE,
                                5ull * 1000 * 1000 * 1000) != VK_SUCCESS) {
                setFallback("fence_timeout"); return false;
            }
            vkResetFences(s->device, 1, &submitFence);
        } else if (vkQueueWaitIdle(s->queue) != VK_SUCCESS) {
            setFallback("queue_wait_idle_failed"); return false;
        }
        g_fx.waitUsLast = std::chrono::duration<double, std::micro>(clk::now() - tWait0).count();
        g_fx.waitUsAccum += g_fx.waitUsLast;
        ++g_fx.waitSamples;
        if ((g_fx.waitSamples % 600) == 1)
            vlog("[QWI-WAIT] wait_us_last=%.1f wait_us_avg=%.1f samples=%lu",
                 g_fx.waitUsLast,
                 g_fx.waitUsAccum / (double)g_fx.waitSamples, g_fx.waitSamples);
    }

    g_fx.renderUs = std::chrono::duration<double, std::micro>(clk::now() - t0).count();
    ++g_fx.usedVulkan;
    ++g_fx.shadowFrames;
    ++g_fx.glSkipped;
    setFallback("");
    return true;
}

// ---------------------------------------------------------------------------
// runVkPostScreenFxNative(): render the armed subset of {cloud-shadow,
// shoreline} natively in VK. Returns a bitmask (bit0 = cloud rendered, bit1 =
// shoreline rendered); the caller skips exactly those GL pass bodies. ANY
// not-ready condition returns 0 and the unchanged GL chain runs (fail-soft).
// ---------------------------------------------------------------------------
unsigned gosPostProcess::runVkPostScreenFxNative()
{
    // Per-pass parity guards — EXACT mirrors of the GL bodies' early-outs.
    const bool wantCloud = g_fx.cloudGate != 0
        && enableCloudShadow_ && sceneHasTerrain_
        && cloudProg_ && cloudProg_->is_valid();
    const bool wantShore = g_fx.shoreGate != 0
        && shorelineEnabled_ && sceneHasTerrain_
        && shorelineProg_ && shorelineProg_->is_valid()
        && !shorelineSuppressedByTerrainMask_;
    if (!wantCloud && !wantShore) return 0;
    if (width_ <= 0 || height_ <= 0) return 0;

    if (!vkPostScreenFx_) vkPostScreenFx_.reset(new VkPostScreenFx());
    VkPostScreenFx* s = vkPostScreenFx_.get();
    if (s->disabled) return 0;
    if (!s->inited || s->width != width_ || s->height != height_) {
        if (s->inited) destroy_state(s);
        if (!build_state(s, width_, height_)) {
            vlog("init FAILED (reason=%s) — disabling for this process, GL passes run.",
                 g_fx.fallbackReason[0] ? g_fx.fallbackReason : "unknown");
            destroy_state(s);
            s->disabled = true;
            return 0;
        }
    }

    ++g_fx.attempted;
    using clk = std::chrono::steady_clock;
    const clk::time_point t0 = clk::now();

    // ---- UBOs (values IDENTICAL to the GL pass uploads). ----
    if (wantCloud) {
        ScreenFxCloudParams p{};
        std::memcpy(p.invViewProj, inverseViewProj_, 16 * sizeof(float));
        p.cloudScroll[0]    = cloudScrollX_;
        p.cloudScroll[1]    = cloudScrollY_;
        p.cloudThreshold[0] = cloudThreshLo_;
        p.cloudThreshold[1] = cloudThreshHi_;
        p.timeSec = SmokeMode::fixedTimestepEnabled()
                        ? (float)SmokeMode::fixedClockSeconds()
                        : (float)SDL_GetTicks() * 0.001f;
        p.cloudScale    = cloudScale_;
        p.cloudStrength = cloudStrength_;
        p.cloudOctaves  = cloudOctaves_;
        p.cloudEnable   = 1;   // gated in C++ above; 1 inside the pass (GL parity)
        void* m = nullptr;
        if ((m = s->cloudUbo.map()) != nullptr) {
            std::memcpy(m, &p, sizeof(p));
            s->cloudUbo.unmap();
        }
    }
    if (wantShore) {
        ScreenFxShoreParams p{};
        p.screenSize[0] = (float)width_;
        p.screenSize[1] = (float)height_;
        p.timeSec = SmokeMode::fixedTimestepEnabled()
                        ? (float)SmokeMode::fixedClockSeconds()
                        : (float)SDL_GetTicks() / 1000.0f;
        void* m = nullptr;
        if ((m = s->shoreUbo.map()) != nullptr) {
            std::memcpy(m, &p, sizeof(p));
            s->shoreUbo.unmap();
        }
    }

    // ---- Shared resource ownership (handoff mode, or direct-chain skip). ----
    // VK-RUNG8 item-4: direct-chain only — engage only when upstream is direct-chain,
    // else decline to the GL screenfx passes. No GL<->VK semaphore handoff.
    if (!vkPostScreenFxDirectChainEnabled()) { setFallback("upstream_not_direct_chain"); return 0; }
    VkCommandBuffer cb = s->cbuf[0u];

    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) {
        setFallback("record_failed"); s->disabled = true; return 0;
    }

    auto depthBarrier = [&](VkImageLayout oldL, VkImageLayout newL,
                            VkAccessFlags srcA, VkAccessFlags dstA,
                            VkPipelineStageFlags srcS, VkPipelineStageFlags dstS) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = oldL; b.newLayout = newL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = s->sDepthRes->image.get();
        b.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
        b.srcAccessMask = srcA; b.dstAccessMask = dstA;
        vkCmdPipelineBarrier(cb, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    auto normalBarrier = [&](VkImageLayout oldL, VkImageLayout newL,
                             VkAccessFlags srcA, VkAccessFlags dstA,
                             VkPipelineStageFlags srcS, VkPipelineStageFlags dstS) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = oldL; b.newLayout = newL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = s->sNormalRes->image.get();
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcAccessMask = srcA; b.dstAccessMask = dstA;
        vkCmdPipelineBarrier(cb, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    // Depth + normal to sampleable layouts for the pass's fragment reads.
    depthBarrier(VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                 VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    normalBarrier(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    // ---- One render pass, up to two multiply draws (GL order: cloud, shore). ----
    {
        VkRenderPassBeginInfo rbi{};
        rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rbi.renderPass = s->rpass.get(); rbi.framebuffer = s->fb.get();
        rbi.renderArea = {{0, 0}, {(uint32_t)s->width, (uint32_t)s->height}};
        rbi.clearValueCount = 0; // LOAD, no clear
        vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        if (wantCloud) {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->cloudPipe.get());
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->cloudPlayout.get(), 0, 1, &s->cloudSet, 0, nullptr);
            vkCmdDraw(cb, 3, 1, 0, 0);
        }
        if (wantShore) {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->shorePipe.get());
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->shorePlayout.get(), 0, 1, &s->shoreSet, 0, nullptr);
            vkCmdDraw(cb, 3, 1, 0, 0);
        }
        vkCmdEndRenderPass(cb);
    }

    // Depth + normal back to the GL hand-off layouts (layout-stable).
    depthBarrier(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                 VK_ACCESS_SHADER_READ_BIT, 0,
                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    normalBarrier(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_ACCESS_SHADER_READ_BIT, 0,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

    if (vkEndCommandBuffer(cb) != VK_SUCCESS) {
        setFallback("record_failed"); s->disabled = true; return 0;
    }

    // ---- Submit (VK-RUNG8 item-4: direct-chain only — no GL<->VK semaphores). The
    // images stay Vulkan-owned for the following native pass. ----
    // VK-QWI-TO-FENCE-1: dedicated per-submit fence instead of the queue-wide drain
    // (see vulkan_post_fog_native.cpp for the full rationale). Fail-soft to QWI if
    // fence creation ever fails.
    if (!s->submitFence) {
        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        mc2vk::create(s->submitFence, s->device, [&](VkFence* h) { return vkCreateFence(s->device, &fci, nullptr, h); });   // empty on failure: the queue idle stands in
    }
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    if (vkQueueSubmit(s->queue, 1, &si, s->submitFence.get()) != VK_SUCCESS) {
        setFallback("submit_failed"); s->disabled = true; return 0;
    }

    // Slot-recycle guard: this frame's command buffer must be retired before reuse
    // next frame (per-submit fence; QWI only as the fence-creation fail-soft).
    {
        const clk::time_point tWait0 = clk::now();
        if (s->submitFence) {
            const VkFence submitFence = s->submitFence.get();
            if (vkWaitForFences(s->device, 1, &submitFence, VK_TRUE,
                                5ull * 1000 * 1000 * 1000) != VK_SUCCESS) {
                setFallback("fence_timeout"); s->disabled = true; return 0;
            }
            vkResetFences(s->device, 1, &submitFence);
        } else if (vkQueueWaitIdle(s->queue) != VK_SUCCESS) {
            setFallback("queue_wait_idle_failed"); s->disabled = true; return 0;
        }
        g_fx.waitUsLast = std::chrono::duration<double, std::micro>(clk::now() - tWait0).count();
        g_fx.waitUsAccum += g_fx.waitUsLast;
        ++g_fx.waitSamples;
        if ((g_fx.waitSamples % 600) == 1)
            vlog("[QWI-WAIT] wait_us_last=%.1f wait_us_avg=%.1f samples=%lu",
                 g_fx.waitUsLast,
                 g_fx.waitUsAccum / (double)g_fx.waitSamples, g_fx.waitSamples);
    }

    g_fx.renderUs = std::chrono::duration<double, std::micro>(clk::now() - t0).count();
    ++g_fx.usedVulkan;
    unsigned mask = 0;
    if (wantCloud) { mask |= 1u; ++g_fx.cloudFrames; ++g_fx.glSkipped; }
    if (wantShore) { mask |= 2u; ++g_fx.shoreFrames; ++g_fx.glSkipped; }
    return mask;
}

void gosPostProcess::destroyVkPostScreenFxNative()
{
    if (g_fx.runtimeGate) {
        vlog("[HEALTH] runtime_gate=%d shadow_gate=%d cloud_gate=%d shore_gate=%d substrate_live=%d "
             "attempted=%lu used_vulkan=%lu shadow_frames=%lu cloud_frames=%lu shore_frames=%lu "
             "direct_chain=%d used_interop=%lu gl_skipped=%lu fallback_reason=\"%s\" render_us_last=%.1f "
             "wait_us_last=%.1f wait_us_avg=%.1f wait_samples=%lu",
             g_fx.runtimeGate, g_fx.shadowGate, g_fx.cloudGate, g_fx.shoreGate, g_fx.substrateLive,
             g_fx.attempted, g_fx.usedVulkan, g_fx.shadowFrames, g_fx.cloudFrames, g_fx.shoreFrames,
             g_fx.directChain, g_fx.usedInterop, g_fx.glSkipped, g_fx.fallbackReason, g_fx.renderUs,
             g_fx.waitUsLast,
             g_fx.waitSamples ? g_fx.waitUsAccum / (double)g_fx.waitSamples : 0.0,
             g_fx.waitSamples);
    }
    if (!vkPostScreenFx_) return;
    vkPostScreenFx_.reset();
}

// Debug-state dump receipt (used_vulkan receipts for every VK capture).
extern "C" void mc2_vk_post_screenfx_native_health(
    int* runtimeGate, int* cloudGate, int* shoreGate, int* substrateLive,
    unsigned long* attempted, unsigned long* usedVulkan,
    unsigned long* cloudFrames, unsigned long* shoreFrames, unsigned long* glSkipped,
    const char** fallbackReason, double* renderUs)
{
    if (runtimeGate)    *runtimeGate = g_fx.runtimeGate;
    if (cloudGate)      *cloudGate = g_fx.cloudGate;
    if (shoreGate)      *shoreGate = g_fx.shoreGate;
    if (substrateLive)  *substrateLive = g_fx.substrateLive;
    if (attempted)      *attempted = g_fx.attempted;
    if (usedVulkan)     *usedVulkan = g_fx.usedVulkan;
    if (cloudFrames)    *cloudFrames = g_fx.cloudFrames;
    if (shoreFrames)    *shoreFrames = g_fx.shoreFrames;
    if (glSkipped)      *glSkipped = g_fx.glSkipped;
    if (fallbackReason) *fallbackReason = g_fx.fallbackReason;
    if (renderUs)       *renderUs = g_fx.renderUs;
}

#endif // MC2_VULKAN_ISLAND
