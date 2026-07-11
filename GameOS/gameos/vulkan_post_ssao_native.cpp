// VK-POST-SSAO-NATIVE-1 (rung 8.2, VK full-ownership ladder — .claude/VK-RUNG8-LADDER-1.md).
//
// SSAO (gen + apply) rendered natively by VK on the SHARED ENGINE VulkanContext,
// reading the shared scene GBuffer and multiplying into the shared scene color
// IN PLACE — ZERO copies, ZERO CPU readbacks, ZERO private devices:
//
//   * pass 1 (gen): samples the SHARED scene depth (vulkan_shared_scene_depth.cpp)
//     AND the SHARED scene normal (vulkan_shared_scene_color.cpp slot
//     MC2_SHARED_SCENE_NORMAL — the SAME VkImage GL's sceneNormalTex_ views)
//     directly into a VK-OWNED half-res R16_SFLOAT occlusion image (device-local,
//     non-shared — no interop; the "one VK-owned intermediate" the ladder expects).
//     Kills the SSAO island's per-frame glGetTexImage(DEPTH) + glGetTexImage(NORMAL)
//     CPU readbacks + staging uploads;
//   * pass 2 (apply): 4-tap box upsample of the occlusion image, MULTIPLIED
//     (DST_COLOR/ZERO — GL's BLENDMODE-MULTIPLY-1 PostProcessSsaoApply row)
//     straight into the SHARED scene color0 — kills the island's scene-color
//     pull/preload + blended-color readback/writeback round-trips;
//   * borrows the shared engine VulkanContext (rung-8.1 fog pattern) — no private
//     VkInstance/VkDevice.
//
// SHADERS REUSED VERBATIM: shaders/vulkan/ssao.{vert,frag} + ssao_apply.{vert,frag}
// (the parity-proven island SPIR-V — R16_SFLOAT occlusion format parity N0 BUG-3,
// LINEAR apply-sampler parity N0 BUG-2, column_major matrix parity
// VK-SSAO-PARITY-FAIL-1 are all already encoded in those sources). Same descriptor
// interfaces; UBO PODs byte-identical to the island's (offline-checked by
// tools/check_ubo_layout.py labels ssaonat_occ / ssaonat_apply).
//
// GATE: MC2_VK_POST_SSAO — envTriState OPT-IN, default-OFF (new-rung pattern, the
// name the rung-8 ladder prescribes; unset -> OFF regardless of
// MC2_VK_SHARED_GBUFFER, explicit 0 forces OFF). Runs ONLY when the GL runSSAO()
// would run (ssaoEnabled_, sceneHasTerrain_, programs valid, half-res FBO present)
// AND ssaoDebug_ == 0 (the debug grayscale-overwrite mode is a diagnostic; it is
// NOT ported — debug ON falls through to GL so the diagnostic stays exact) AND the
// shared GBuffer substrate is live; ANY not-ready condition returns false and the
// caller falls through to the unchanged region-iface/GL chain (fail-soft, never
// crashes, gate-OFF byte-identical).
//
// SYNC (rung-8.1 fog / water-tier1 pattern): per frame BeginFrame on color0 +
// normal + depth shared resources, GL signals color0/normal in COLOR_ATTACHMENT
// layout + depth in DEPTH_STENCIL_ATTACHMENT layout, VK submit waits all GL->VK
// sems, in-cb barriers move depth DS_ATTACHMENT -> SHADER_READ_ONLY and normal
// COLOR_ATTACHMENT -> SHADER_READ_ONLY for sampling and BACK after the gen pass
// (layout-stable across the API boundary), apply render pass is LOAD/STORE
// color-attachment-stable on color0, submit signals the VK->GL sems, GL waits
// them; fence CPU-wait as the slot-recycle guard.
//
// The whole TU compiles to nothing unless MC2_VULKAN_ISLAND is defined.

#include "gos_postprocess.h"

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
#include <vector>

namespace {

void vlog(const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "[VK_POST_SSAO_NATIVE] %s\n", buf);
    std::fflush(stderr);
}

// Health snapshot — read by debug_state_dump.cpp via the extern "C" getter below
// (used_vulkan receipt for every VK capture, per the arc's evidence protocol).
struct PostSsaoNativeHealth {
    int  runtimeGate   = 0;   // MC2_VK_POST_SSAO resolved ON
    int  substrateLive = 0;   // shared color0 + normal + depth present at init
    int  directChain   = 0;   // MC2_VK_POST_SSAO_DIRECT_CHAIN resolved ON
    unsigned long attempted  = 0;  // frames the native path was entered
    unsigned long usedVulkan = 0;  // frames both passes actually rendered via VK
    unsigned long usedInterop = 0; // frames that used GL<->VK semaphores
    unsigned long glSkipped  = 0;  // GL SSAO sites skipped (1 per used frame)
    char fallbackReason[64]  = {0};
    double renderUs = 0.0;         // record+submit+sem/fence wait, last frame
};
PostSsaoNativeHealth g_ps;

void setFallback(const char* reason) {
    std::strncpy(g_ps.fallbackReason, reason ? reason : "",
                 sizeof(g_ps.fallbackReason) - 1);
    g_ps.fallbackReason[sizeof(g_ps.fallbackReason) - 1] = '\0';
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
    std::vector<uint32_t> code((size_t)len / 4);
    size_t got = std::fread(code.data(), 1, (size_t)len, f);
    std::fclose(f);
    if (got != (size_t)len) { vlog("spv short read '%s'.", path.c_str()); return VK_NULL_HANDLE; }
    VkShaderModuleCreateInfo smci{};
    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = (size_t)len; smci.pCode = code.data();
    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &smci, nullptr, &mod) != VK_SUCCESS) {
        vlog("vkCreateShaderModule('%s') failed.", path.c_str()); return VK_NULL_HANDLE;
    }
    return mod;
}

// std140 PODs: byte-identical to the GL SSAO uploads; offline-checked by
// check_ubo_layout.py ssaonat_occ / ssaonat_apply.
// Matrices: COLUMN-major raw memcpy of inverseViewProj_/viewProj_ — the GL SSAO
// path uploads them with a single glUniformMatrix4fv(GL_FALSE) (column-major
// read); the ssao.frag UBO is column_major (default). See VK-SSAO-PARITY-FAIL-1.
struct SsaoNativeOcclusionParams {
    float invViewProj[16]; // off 0   (column-major, matches the GL GL_FALSE upload)
    float viewProj[16];    // off 64
    float screenSize[2];   // off 128 (FULL-res width_/height_, like GL)
    float aoRadius;        // off 136
    float aoBias;          // off 140
    float aoStrength;      // off 144
    float aoPower;         // off 148
};
static_assert(sizeof(SsaoNativeOcclusionParams) == 152, "SsaoNativeOcclusionParams std140 offsets drifted");

struct SsaoNativeApplyParams {
    float ssaoTexel[2];    // off 0   (1/ssaoW_, 1/ssaoH_)
    int   debugMode;       // off 8   (always 0 on the native path — debug falls through to GL)
    int   _pad0;           // off 12  (std140 rounds the block to 16B)
};
static_assert(sizeof(SsaoNativeApplyParams) == 16, "SsaoNativeApplyParams std140 offsets drifted");

// N0 BUG-3 format parity: GL's half-res AO target is GL_R16F -> R16_SFLOAT.
const VkFormat kOcclusionFmt = VK_FORMAT_R16_SFLOAT;

} // namespace

// Persistent state. Lazy init at the SSAO site; torn down by destroyFBOs (resize)
// and destroy() via destroyVkPostSsaoNative().
struct gosPostProcess::VkPostSsaoNative {
    bool disabled = false;   // init failed once -> never retry, GL path forever
    bool inited   = false;
    int  width = 0, height = 0;      // full-res
    int  halfW = 0, halfH = 0;       // GL's exact ssaoW_/ssaoH_

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
    GlInteropView*   sColorView  = nullptr;
    GlInteropView*   sNormalView = nullptr;
    GlInteropView*   sDepthView  = nullptr;

    // OWNED objects.
    VkImageView   depthSampleView = VK_NULL_HANDLE;  // depth-aspect view over sDepthRes->image
    VkSampler     gbufSampler = VK_NULL_HANDLE;      // NEAREST/CLAMP (GL sceneDepthTex_/sceneNormalTex_ parity)
    VkSampler     occSampler  = VK_NULL_HANDLE;      // LINEAR/CLAMP (GL ssaoColorTex_ GL_LINEAR — N0 BUG-2)
    VkImage       occImage = VK_NULL_HANDLE; VmaAllocation occAlloc = VK_NULL_HANDLE;
    VkImageView   occView  = VK_NULL_HANDLE;         // VK-owned half-res R16F intermediate
    VkBuffer      occUbo   = VK_NULL_HANDLE; VmaAllocation occUboAlloc   = VK_NULL_HANDLE;
    VkBuffer      applyUbo = VK_NULL_HANDLE; VmaAllocation applyUboAlloc = VK_NULL_HANDLE;
    VkRenderPass  occRpass   = VK_NULL_HANDLE;   // gen: occ image, CLEAR/STORE -> SHADER_READ_ONLY
    VkRenderPass  applyRpass = VK_NULL_HANDLE;   // apply: shared color0, LOAD/STORE layout-stable
    VkFramebuffer occFb   = VK_NULL_HANDLE;
    VkFramebuffer applyFb = VK_NULL_HANDLE;
    VkDescriptorSetLayout occDsl   = VK_NULL_HANDLE;
    VkDescriptorSetLayout applyDsl = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet  occSet   = VK_NULL_HANDLE;
    VkDescriptorSet  applySet = VK_NULL_HANDLE;
    VkPipelineLayout occPlayout   = VK_NULL_HANDLE;
    VkPipelineLayout applyPlayout = VK_NULL_HANDLE;
    VkPipeline       occPipe   = VK_NULL_HANDLE;
    VkPipeline       applyPipe = VK_NULL_HANDLE;
    VkShaderModule   occVert = VK_NULL_HANDLE,  occFrag = VK_NULL_HANDLE;
    VkShaderModule   applyVert = VK_NULL_HANDLE, applyFrag = VK_NULL_HANDLE;
    VkCommandPool    cpool = VK_NULL_HANDLE;
    VkCommandBuffer  cbuf[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE}; // per parity slot
};

static bool vkPostSsaoDirectChainEnabled();

namespace {

bool make_buffer(gosPostProcess::VkPostSsaoNative* s, VkDeviceSize size,
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

void destroy_state(gosPostProcess::VkPostSsaoNative* s) {
    if (!s) return;
    VkDevice d = s->device;
    if (d != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(d);
        if (s->occPipe)   vkDestroyPipeline(d, s->occPipe, nullptr);
        if (s->applyPipe) vkDestroyPipeline(d, s->applyPipe, nullptr);
        if (s->occPlayout)   vkDestroyPipelineLayout(d, s->occPlayout, nullptr);
        if (s->applyPlayout) vkDestroyPipelineLayout(d, s->applyPlayout, nullptr);
        if (s->occVert)   vkDestroyShaderModule(d, s->occVert, nullptr);
        if (s->occFrag)   vkDestroyShaderModule(d, s->occFrag, nullptr);
        if (s->applyVert) vkDestroyShaderModule(d, s->applyVert, nullptr);
        if (s->applyFrag) vkDestroyShaderModule(d, s->applyFrag, nullptr);
        if (s->pool)      vkDestroyDescriptorPool(d, s->pool, nullptr);
        if (s->occDsl)    vkDestroyDescriptorSetLayout(d, s->occDsl, nullptr);
        if (s->applyDsl)  vkDestroyDescriptorSetLayout(d, s->applyDsl, nullptr);
        if (s->occFb)     vkDestroyFramebuffer(d, s->occFb, nullptr);
        if (s->applyFb)   vkDestroyFramebuffer(d, s->applyFb, nullptr);
        if (s->occRpass)   vkDestroyRenderPass(d, s->occRpass, nullptr);
        if (s->applyRpass) vkDestroyRenderPass(d, s->applyRpass, nullptr);
        if (s->gbufSampler) vkDestroySampler(d, s->gbufSampler, nullptr);
        if (s->occSampler)  vkDestroySampler(d, s->occSampler, nullptr);
        if (s->occView)          vkDestroyImageView(d, s->occView, nullptr);
        if (s->depthSampleView)  vkDestroyImageView(d, s->depthSampleView, nullptr);
        if (s->cpool)     vkDestroyCommandPool(d, s->cpool, nullptr);
        if (s->allocator != VK_NULL_HANDLE) {
            if (s->occImage) vmaDestroyImage(s->allocator, s->occImage, s->occAlloc);
            if (s->occUbo)   vmaDestroyBuffer(s->allocator, s->occUbo, s->occUboAlloc);
            if (s->applyUbo) vmaDestroyBuffer(s->allocator, s->applyUbo, s->applyUboAlloc);
        }
    }
    // Borrowed handles are NOT destroyed. Zero the owned ones.
    const bool disabled = s->disabled;
    *s = gosPostProcess::VkPostSsaoNative{};
    s->disabled = disabled;
}

bool build_state(gosPostProcess::VkPostSsaoNative* s, int w, int h, int halfW, int halfH) {
    s->width = w; s->height = h;
    s->halfW = halfW; s->halfH = halfH;

    // Shared engine device, on-demand (rung-8.1 model; fail-soft -> GL SSAO).
    if (!mc2vk::ensureEngineVulkanContextForInterop()) {
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
    s->sColorView  = mc2SharedSceneColorGetGlView(MC2_SHARED_SCENE_COLOR0);
    s->sNormalRes  = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_NORMAL);
    s->sNormalView = mc2SharedSceneColorGetGlView(MC2_SHARED_SCENE_NORMAL);
    s->sDepthRes   = mc2SharedSceneDepthGetResource();
    s->sDepthView  = mc2SharedSceneDepthGetGlView();
    const bool directChain = vkPostSsaoDirectChainEnabled();
    if (!s->sColorRes || !s->sNormalRes || !s->sDepthRes ||
        (!directChain && (!s->sColorView || !s->sNormalView || !s->sDepthView))) {
        setFallback("shared_gbuffer_not_ready"); return false;
    }
    if ((int)s->sColorRes->w  != w || (int)s->sColorRes->h  != h ||
        (int)s->sNormalRes->w != w || (int)s->sNormalRes->h != h ||
        (int)s->sDepthRes->w  != w || (int)s->sDepthRes->h  != h) {
        setFallback("shared_gbuffer_size_mismatch"); return false;
    }
    g_ps.substrateLive = 1;

    VkResult r;

    // Depth-aspect sampling view over the SHARED depth image (fog-native pattern:
    // sampling a depth aspect returns depth in .r — the same values GL samples).
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
    // Samplers: NEAREST for depth+normal (GL parity), LINEAR for the AO upsample
    // (N0 BUG-2 — GL's ssaoColorTex_ is GL_LINEAR; the 4-tap box relies on it).
    {
        auto make_sampler = [&](VkSampler* samp, VkFilter filter) -> bool {
            VkSamplerCreateInfo sci{};
            sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            sci.magFilter = filter; sci.minFilter = filter;
            sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            if (vkCreateSampler(s->device, &sci, nullptr, samp) != VK_SUCCESS) {
                vlog("vkCreateSampler failed."); return false;
            }
            return true;
        };
        if (!make_sampler(&s->gbufSampler, VK_FILTER_NEAREST)) return false;
        if (!make_sampler(&s->occSampler,  VK_FILTER_LINEAR))  return false;
    }

    // VK-OWNED half-res occlusion image (device-local, non-shared, no interop).
    {
        VkImageCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = kOcclusionFmt;
        ici.extent = {(uint32_t)halfW, (uint32_t)halfH, 1};
        ici.mipLevels = 1; ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        if (vmaCreateImage(s->allocator, &ici, &aci, &s->occImage, &s->occAlloc, nullptr) != VK_SUCCESS) {
            vlog("vmaCreateImage(occlusion) failed."); return false;
        }
        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = s->occImage;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = kOcclusionFmt;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        r = vkCreateImageView(s->device, &vci, nullptr, &s->occView);
        if (r != VK_SUCCESS) { vlog("occ view create failed (%d).", (int)r); return false; }
    }

    if (!make_buffer(s, sizeof(SsaoNativeOcclusionParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &s->occUbo, &s->occUboAlloc)) return false;
    if (!make_buffer(s, sizeof(SsaoNativeApplyParams),     VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &s->applyUbo, &s->applyUboAlloc)) return false;

    // Render pass 1 (gen): 1 attachment = the VK-owned occlusion image.
    // UNDEFINED -> CLEAR (GL glClear parity; fully overwritten anyway) -> STORE,
    // finalLayout SHADER_READ_ONLY so pass 2 samples it without an extra barrier.
    {
        VkAttachmentDescription att{};
        att.format = kOcclusionFmt;
        att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout   = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1; sub.pColorAttachments = &ref;
        VkSubpassDependency deps[2]{};
        // Incoming: previous frame's apply-pass READ of this image must finish
        // before this frame's layout transition + write.
        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL; deps[0].dstSubpass = 0;
        deps[0].srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        // Outgoing: this pass's write visible to the apply pass's fragment read.
        deps[1].srcSubpass = 0; deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        VkRenderPassCreateInfo rpci{};
        rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpci.attachmentCount = 1; rpci.pAttachments = &att;
        rpci.subpassCount = 1; rpci.pSubpasses = &sub;
        rpci.dependencyCount = 2; rpci.pDependencies = deps;
        r = vkCreateRenderPass(s->device, &rpci, nullptr, &s->occRpass);
        if (r != VK_SUCCESS) { vlog("vkCreateRenderPass(occ) failed (%d).", (int)r); return false; }

        VkFramebufferCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = s->occRpass;
        fci.attachmentCount = 1; fci.pAttachments = &s->occView;
        fci.width = (uint32_t)halfW; fci.height = (uint32_t)halfH; fci.layers = 1;
        r = vkCreateFramebuffer(s->device, &fci, nullptr, &s->occFb);
        if (r != VK_SUCCESS) { vlog("vkCreateFramebuffer(occ) failed (%d).", (int)r); return false; }
    }

    // Render pass 2 (apply): 1 attachment = SHARED scene color0. LOAD (multiply
    // blends against the scene), STORE; layout-STABLE COLOR_ATTACHMENT both ends
    // (fog-native pattern).
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
        r = vkCreateRenderPass(s->device, &rpci, nullptr, &s->applyRpass);
        if (r != VK_SUCCESS) { vlog("vkCreateRenderPass(apply) failed (%d).", (int)r); return false; }

        VkFramebufferCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = s->applyRpass;
        fci.attachmentCount = 1; fci.pAttachments = &s->sColorRes->view;
        fci.width = (uint32_t)w; fci.height = (uint32_t)h; fci.layers = 1;
        r = vkCreateFramebuffer(s->device, &fci, nullptr, &s->applyFb);
        if (r != VK_SUCCESS) { vlog("vkCreateFramebuffer(apply) failed (%d).", (int)r); return false; }
    }

    // Descriptor layouts / pool / sets.
    // Gen (ssao.frag): 0=depth sampler, 1=normal sampler, 2=UBO.
    // Apply (ssao_apply.frag): 0=occ sampler, 1=UBO.
    {
        VkDescriptorSetLayoutBinding occBinds[3]{};
        occBinds[0].binding = 0; occBinds[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        occBinds[0].descriptorCount = 1; occBinds[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        occBinds[1].binding = 1; occBinds[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        occBinds[1].descriptorCount = 1; occBinds[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        occBinds[2].binding = 2; occBinds[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        occBinds[2].descriptorCount = 1; occBinds[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.bindingCount = 3; lci.pBindings = occBinds;
        r = vkCreateDescriptorSetLayout(s->device, &lci, nullptr, &s->occDsl);
        if (r != VK_SUCCESS) { vlog("vkCreateDescriptorSetLayout(occ) failed (%d).", (int)r); return false; }

        VkDescriptorSetLayoutBinding applyBinds[2]{};
        applyBinds[0].binding = 0; applyBinds[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        applyBinds[0].descriptorCount = 1; applyBinds[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        applyBinds[1].binding = 1; applyBinds[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        applyBinds[1].descriptorCount = 1; applyBinds[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        lci.bindingCount = 2; lci.pBindings = applyBinds;
        r = vkCreateDescriptorSetLayout(s->device, &lci, nullptr, &s->applyDsl);
        if (r != VK_SUCCESS) { vlog("vkCreateDescriptorSetLayout(apply) failed (%d).", (int)r); return false; }

        VkDescriptorPoolSize sizes[2]{};
        sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; sizes[0].descriptorCount = 3;
        sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;         sizes[1].descriptorCount = 2;
        VkDescriptorPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.maxSets = 2; pci.poolSizeCount = 2; pci.pPoolSizes = sizes;
        r = vkCreateDescriptorPool(s->device, &pci, nullptr, &s->pool);
        if (r != VK_SUCCESS) { vlog("vkCreateDescriptorPool failed (%d).", (int)r); return false; }

        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = s->pool; dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &s->occDsl;
        r = vkAllocateDescriptorSets(s->device, &dsai, &s->occSet);
        if (r != VK_SUCCESS) { vlog("vkAllocateDescriptorSets(occ) failed (%d).", (int)r); return false; }
        dsai.pSetLayouts = &s->applyDsl;
        r = vkAllocateDescriptorSets(s->device, &dsai, &s->applySet);
        if (r != VK_SUCCESS) { vlog("vkAllocateDescriptorSets(apply) failed (%d).", (int)r); return false; }

        VkDescriptorImageInfo depthInfo{};
        depthInfo.sampler = s->gbufSampler;
        depthInfo.imageView = s->depthSampleView;
        depthInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo normalInfo{};
        normalInfo.sampler = s->gbufSampler;
        normalInfo.imageView = s->sNormalRes->view;
        normalInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo occInfo{};
        occInfo.sampler = s->occSampler;
        occInfo.imageView = s->occView;
        occInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorBufferInfo occUboInfo{ s->occUbo, 0, VK_WHOLE_SIZE };
        VkDescriptorBufferInfo applyUboInfo{ s->applyUbo, 0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet writes[5]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = s->occSet; writes[0].dstBinding = 0; writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &depthInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = s->occSet; writes[1].dstBinding = 1; writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo = &normalInfo;
        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = s->occSet; writes[2].dstBinding = 2; writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[2].pBufferInfo = &occUboInfo;
        writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[3].dstSet = s->applySet; writes[3].dstBinding = 0; writes[3].descriptorCount = 1;
        writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[3].pImageInfo = &occInfo;
        writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[4].dstSet = s->applySet; writes[4].dstBinding = 1; writes[4].descriptorCount = 1;
        writes[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[4].pBufferInfo = &applyUboInfo;
        vkUpdateDescriptorSets(s->device, 5, writes, 0, nullptr);
    }

    // Two pipelines — fullscreen tri, no vertex input, cull none.
    // Gen: NO blend (overwrite, GL parity). Apply: MULTIPLY (DST_COLOR/ZERO color,
    // DST_ALPHA/ZERO alpha — GL glBlendFunc(GL_DST_COLOR, GL_ZERO) semantics).
    {
        s->occVert   = load_spv(s->device, spv_dir() + "ssao.vert.spv");
        s->occFrag   = load_spv(s->device, spv_dir() + "ssao.frag.spv");
        s->applyVert = load_spv(s->device, spv_dir() + "ssao_apply.vert.spv");
        s->applyFrag = load_spv(s->device, spv_dir() + "ssao_apply.frag.spv");
        if (s->occVert == VK_NULL_HANDLE || s->occFrag == VK_NULL_HANDLE ||
            s->applyVert == VK_NULL_HANDLE || s->applyFrag == VK_NULL_HANDLE) {
            setFallback("spv_load_failed"); return false;
        }
        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 1; plci.pSetLayouts = &s->occDsl;
        r = vkCreatePipelineLayout(s->device, &plci, nullptr, &s->occPlayout);
        if (r != VK_SUCCESS) { vlog("vkCreatePipelineLayout(occ) failed (%d).", (int)r); return false; }
        plci.pSetLayouts = &s->applyDsl;
        r = vkCreatePipelineLayout(s->device, &plci, nullptr, &s->applyPlayout);
        if (r != VK_SUCCESS) { vlog("vkCreatePipelineLayout(apply) failed (%d).", (int)r); return false; }

        auto make_pipeline = [&](VkShaderModule vmod, VkShaderModule fmod,
                                 VkPipelineLayout layout, VkRenderPass rpass,
                                 int vw, int vh, bool multiplyBlend,
                                 VkPipeline* out) -> bool {
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
            VkViewport vp{0.0f, 0.0f, (float)vw, (float)vh, 0.0f, 1.0f};
            VkRect2D sc{{0, 0}, {(uint32_t)vw, (uint32_t)vh}};
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
            if (multiplyBlend) {
                cba.blendEnable = VK_TRUE;
                cba.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
                cba.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
                cba.colorBlendOp = VK_BLEND_OP_ADD;
                cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_DST_ALPHA;
                cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
                cba.alphaBlendOp = VK_BLEND_OP_ADD;
            } else {
                cba.blendEnable = VK_FALSE;
            }
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
            gp.renderPass = rpass;
            gp.subpass = 0;
            if (vkCreateGraphicsPipelines(s->device, VK_NULL_HANDLE, 1, &gp, nullptr, out) != VK_SUCCESS) {
                vlog("vkCreateGraphicsPipelines failed."); return false;
            }
            return true;
        };
        if (!make_pipeline(s->occVert, s->occFrag, s->occPlayout, s->occRpass,
                           halfW, halfH, /*multiply*/false, &s->occPipe)) return false;
        if (!make_pipeline(s->applyVert, s->applyFrag, s->applyPlayout, s->applyRpass,
                           w, h, /*multiply*/true, &s->applyPipe)) return false;
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
    vlog("initialized %dx%d (occlusion %dx%d R16F, VK-owned) — NATIVE SSAO on the "
         "shared engine context (samples shared depth+normal, multiplies into shared "
         "scene color0 in place; ZERO copies, ZERO readbacks, no private device).",
         w, h, halfW, halfH);
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// vkPostSsaoNativeEnabled(): envTriState OPT-IN, default-OFF (new-rung pattern —
// unset -> OFF regardless of MC2_VK_SHARED_GBUFFER, explicit 0 forces OFF).
// Gate name per the rung-8 ladder: MC2_VK_POST_SSAO.
// ---------------------------------------------------------------------------
bool gosPostProcess::vkPostSsaoNativeEnabled()
{
    static int state = -1;
    if (state == -1) {
        state = (mc2vkgate::envTriState("MC2_VK_POST_SSAO") == 1) ? 1 : 0;
        g_ps.runtimeGate = state;
        if (state == 1)
            vlog("MC2_VK_POST_SSAO=1 — rung-8.2 native SSAO gate ON "
                 "(lazy init at the SSAO site; fail-soft to region-iface/GL).");
    }
    return state == 1;
}

static bool vkPostSsaoDirectChainEnabled()
{
    static int state = -1;
    if (state == -1) {
        state = (mc2vkgate::directChainGate("MC2_VK_POST_SSAO_DIRECT_CHAIN") &&
                 mc2vkgate::postDirectChainUpstreamOn()) ? 1 : 0;
        g_ps.directChain = state;
        if (state == 1)
            vlog("MC2_VK_POST_SSAO_DIRECT_CHAIN=1 - skipping SSAO GL<->VK handoff "
                 "(full terrain->prop->mech->water chain armed).");
    }
    return state == 1;
}

// ---------------------------------------------------------------------------
// runVkPostSsaoNative(): render SSAO gen + apply natively in VK. Returns true only
// when both passes were submitted (caller skips the GL SSAO site); ANY not-ready
// condition returns false and the unchanged chain runs.
// ---------------------------------------------------------------------------
bool gosPostProcess::runVkPostSsaoNative()
{
    // Run ONLY when the GL runSSAO() would run (parity guard — the exact
    // ssaoGatesLetItRun condition set from the region backend). ssaoDebug_ != 0
    // additionally falls through to GL: the debug grayscale-overwrite is a
    // diagnostic mode, not ported (documented limitation, like the fog path's
    // MC2_SKYBOX_FOG_EXCLUDE guard).
    if (!ssaoEnabled_ || !sceneHasTerrain_) return false;
    if (!ssaoProg_ || !ssaoProg_->is_valid()) return false;
    if (!ssaoApplyProg_ || !ssaoApplyProg_->is_valid()) return false;
    if (!ssaoFBO_ || !ssaoColorTex_) return false;
    if (ssaoDebug_ != 0) return false;
    if (width_ <= 0 || height_ <= 0 || ssaoW_ <= 0 || ssaoH_ <= 0) return false;

    if (!vkPostSsaoNative_) vkPostSsaoNative_ = new VkPostSsaoNative();
    VkPostSsaoNative* s = vkPostSsaoNative_;
    if (s->disabled) return false;
    if (!s->inited || s->width != width_ || s->height != height_ ||
        s->halfW != ssaoW_ || s->halfH != ssaoH_) {
        if (s->inited) destroy_state(s);
        if (!build_state(s, width_, height_, ssaoW_, ssaoH_)) {
            vlog("init FAILED (reason=%s) — disabling for this process, GL SSAO runs.",
                 g_ps.fallbackReason[0] ? g_ps.fallbackReason : "unknown");
            destroy_state(s);
            s->disabled = true;
            return false;
        }
    }

    ++g_ps.attempted;
    using clk = std::chrono::steady_clock;
    const clk::time_point t0 = clk::now();

    // ---- UBOs (values IDENTICAL to the GL runSSAO() uploads). ----
    {
        SsaoNativeOcclusionParams p{};
        std::memcpy(p.invViewProj, inverseViewProj_, 16 * sizeof(float));
        std::memcpy(p.viewProj,    viewProj_,        16 * sizeof(float));
        p.screenSize[0] = (float)width_;
        p.screenSize[1] = (float)height_;
        p.aoRadius   = aoRadius_;
        p.aoBias     = aoBias_;
        p.aoStrength = aoStrength_;
        p.aoPower    = aoPower_;
        void* m = nullptr;
        if (vmaMapMemory(s->allocator, s->occUboAlloc, &m) == VK_SUCCESS) {
            std::memcpy(m, &p, sizeof(p));
            vmaUnmapMemory(s->allocator, s->occUboAlloc);
        }
    }
    {
        SsaoNativeApplyParams p{};
        p.ssaoTexel[0] = 1.0f / (float)ssaoW_;
        p.ssaoTexel[1] = 1.0f / (float)ssaoH_;
        p.debugMode = 0;
        void* m = nullptr;
        if (vmaMapMemory(s->allocator, s->applyUboAlloc, &m) == VK_SUCCESS) {
            std::memcpy(m, &p, sizeof(p));
            vmaUnmapMemory(s->allocator, s->applyUboAlloc);
        }
    }

    // ---- Shared resource ownership (handoff mode, or direct-chain skip). ----
    const bool directChain = vkPostSsaoDirectChainEnabled();
    VkFrameSync fsC = directChain ? VkFrameSync{} : vkImageResourceBeginFrame(s->sColorRes);
    VkFrameSync fsN = directChain ? VkFrameSync{} : vkImageResourceBeginFrame(s->sNormalRes);
    VkFrameSync fsD = directChain ? VkFrameSync{} : vkImageResourceBeginFrame(s->sDepthRes);
    VkCommandBuffer cb = s->cbuf[directChain ? 0u : (fsC.parity & 1u)];
    if (!directChain) {
        glInteropSignalToVk(*s->sColorView,  GL_LAYOUT_COLOR_ATTACHMENT_EXT,         fsC.parity);
        glInteropSignalToVk(*s->sNormalView, GL_LAYOUT_COLOR_ATTACHMENT_EXT,         fsN.parity);
        glInteropSignalToVk(*s->sDepthView,  GL_LAYOUT_DEPTH_STENCIL_ATTACHMENT_EXT, fsD.parity);
    }

    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) {
        setFallback("record_failed"); s->disabled = true; return false;
    }

    // Depth: DEPTH_STENCIL_ATTACHMENT (GL hand-off layout) -> SHADER_READ_ONLY.
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
    // Normal: COLOR_ATTACHMENT (GL hand-off layout) -> SHADER_READ_ONLY.
    auto normalBarrier = [&](VkImageLayout oldL, VkImageLayout newL,
                             VkAccessFlags srcA, VkAccessFlags dstA,
                             VkPipelineStageFlags srcS, VkPipelineStageFlags dstS) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = oldL; b.newLayout = newL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = s->sNormalRes->image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcAccessMask = srcA; b.dstAccessMask = dstA;
        vkCmdPipelineBarrier(cb, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    depthBarrier(VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                 VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    normalBarrier(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    // ---- Pass 1: gen (half-res occlusion; CLEAR parity with GL's glClear). ----
    {
        VkClearValue clear{};
        clear.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
        VkRenderPassBeginInfo rbi{};
        rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rbi.renderPass = s->occRpass; rbi.framebuffer = s->occFb;
        rbi.renderArea = {{0, 0}, {(uint32_t)s->halfW, (uint32_t)s->halfH}};
        rbi.clearValueCount = 1; rbi.pClearValues = &clear;
        vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->occPipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->occPlayout, 0, 1, &s->occSet, 0, nullptr);
        vkCmdDraw(cb, 3, 1, 0, 0);
        vkCmdEndRenderPass(cb);
    }

    // Depth + normal back to the GL hand-off layouts (layout-stable across the
    // boundary) — they are not touched by the apply pass.
    depthBarrier(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                 VK_ACCESS_SHADER_READ_BIT, 0,
                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    normalBarrier(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_ACCESS_SHADER_READ_BIT, 0,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

    // ---- Pass 2: apply (multiply into shared color0 in place). ----
    {
        VkRenderPassBeginInfo rbi{};
        rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rbi.renderPass = s->applyRpass; rbi.framebuffer = s->applyFb;
        rbi.renderArea = {{0, 0}, {(uint32_t)s->width, (uint32_t)s->height}};
        rbi.clearValueCount = 0; // LOAD, no clear
        vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->applyPipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->applyPlayout, 0, 1, &s->applySet, 0, nullptr);
        vkCmdDraw(cb, 3, 1, 0, 0);
        vkCmdEndRenderPass(cb);
    }

    if (vkEndCommandBuffer(cb) != VK_SUCCESS) {
        setFallback("record_failed"); s->disabled = true; return false;
    }

    // ---- Submit: handoff mode waits/signals GL sems; direct-chain mode does not. ----
    VkSemaphore waitSems[3];   VkPipelineStageFlags waitStages[3];
    VkSemaphore signalSems[3]; uint32_t waitN = 0, sigN = 0;
    if (!directChain && fsC.signal != VK_NULL_HANDLE) {
        waitSems[waitN] = fsC.signal;
        waitStages[waitN] = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; ++waitN;
    }
    if (!directChain && fsN.signal != VK_NULL_HANDLE) {
        // The normal barrier's srcStage is COLOR_ATTACHMENT_OUTPUT; block sampling too.
        waitSems[waitN] = fsN.signal;
        waitStages[waitN] = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT; ++waitN;
    }
    if (!directChain && fsD.signal != VK_NULL_HANDLE) {
        waitSems[waitN] = fsD.signal;
        waitStages[waitN] = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT; ++waitN;
    }
    const bool gpuC = !directChain && s->sColorView->vkToGlImported  && fsC.vk2gl != VK_NULL_HANDLE;
    const bool gpuN = !directChain && s->sNormalView->vkToGlImported && fsN.vk2gl != VK_NULL_HANDLE;
    const bool gpuD = !directChain && s->sDepthView->vkToGlImported  && fsD.vk2gl != VK_NULL_HANDLE;
    if (gpuC) signalSems[sigN++] = fsC.vk2gl;
    if (gpuN) signalSems[sigN++] = fsN.vk2gl;
    if (gpuD) signalSems[sigN++] = fsD.vk2gl;

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = waitN; si.pWaitSemaphores = waitSems; si.pWaitDstStageMask = waitStages;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    si.signalSemaphoreCount = sigN; si.pSignalSemaphores = signalSems;
    if (vkQueueSubmit(s->queue, 1, &si, directChain ? VK_NULL_HANDLE : fsC.fence) != VK_SUCCESS) {
        setFallback("submit_failed"); s->disabled = true; return false;
    }

    // GL re-acquires ALL images in the layouts VK left them (layout-stable).
    if (gpuC) glInteropWaitFromVk(*s->sColorView,  GL_LAYOUT_COLOR_ATTACHMENT_EXT,         fsC.parity);
    if (gpuN) glInteropWaitFromVk(*s->sNormalView, GL_LAYOUT_COLOR_ATTACHMENT_EXT,         fsN.parity);
    if (gpuD) glInteropWaitFromVk(*s->sDepthView,  GL_LAYOUT_DEPTH_STENCIL_ATTACHMENT_EXT, fsD.parity);

    // Slot-recycle guard (water pattern): CPU-wait the color fence so the parity
    // slot's binary semaphores are retired before reuse two frames later.
    if (directChain) {
        if (vkQueueWaitIdle(s->queue) != VK_SUCCESS) {
            setFallback("queue_wait_idle_failed"); s->disabled = true; return false;
        }
    } else if (vkWaitForFences(s->device, 1, &fsC.fence, VK_TRUE, 5ull * 1000 * 1000 * 1000) != VK_SUCCESS) {
        setFallback("fence_timeout"); s->disabled = true; return false;
    }

    g_ps.renderUs = std::chrono::duration<double, std::micro>(clk::now() - t0).count();
    ++g_ps.usedVulkan;
    if (!directChain) ++g_ps.usedInterop;
    g_ps.glSkipped += 1;
    return true;
}

void gosPostProcess::destroyVkPostSsaoNative()
{
    if (g_ps.runtimeGate) {
        vlog("[HEALTH] runtime_gate=%d substrate_live=%d attempted=%lu used_vulkan=%lu "
             "direct_chain=%d used_interop=%lu gl_skipped=%lu fallback_reason=\"%s\" "
             "render_us_last=%.1f",
             g_ps.runtimeGate, g_ps.substrateLive, g_ps.attempted, g_ps.usedVulkan,
             g_ps.directChain, g_ps.usedInterop, g_ps.glSkipped, g_ps.fallbackReason,
             g_ps.renderUs);
    }
    if (!vkPostSsaoNative_) return;
    destroy_state(vkPostSsaoNative_);
    delete vkPostSsaoNative_;
    vkPostSsaoNative_ = nullptr;
}

// Debug-state dump receipt (used_vulkan receipts for every VK capture).
extern "C" void mc2_vk_post_ssao_native_health(
    int* runtimeGate, int* substrateLive,
    unsigned long* attempted, unsigned long* usedVulkan, unsigned long* glSkipped,
    const char** fallbackReason, double* renderUs)
{
    if (runtimeGate)    *runtimeGate = g_ps.runtimeGate;
    if (substrateLive)  *substrateLive = g_ps.substrateLive;
    if (attempted)      *attempted = g_ps.attempted;
    if (usedVulkan)     *usedVulkan = g_ps.usedVulkan;
    if (glSkipped)      *glSkipped = g_ps.glSkipped;
    if (fallbackReason) *fallbackReason = g_ps.fallbackReason;
    if (renderUs)       *renderUs = g_ps.renderUs;
}

#endif // MC2_VULKAN_ISLAND
