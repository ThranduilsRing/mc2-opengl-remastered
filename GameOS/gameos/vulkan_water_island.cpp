// vulkan_water_island.cpp — VK-P9a WATER-INTEROP: the FIRST Vulkan GEOMETRY region.
// =============================================================================
// Renders the MC2 armed water fast-path mesh (base layer) in Vulkan into an
// exportable RGBA16F VkImageResource that GL composites into MainColor. Gate
// MC2_VK_WATER_INTEROP (default-OFF -> GL water path unchanged, byte-identical).
//
// SHAPE (SSAO-island seam + a real geometry draw, per VK-P9a-STEP0 note):
//   - Device: BORROWED from the shared engine VulkanContext
//     (getOrCreateEngineVulkanContext, like engine_present_vk.cpp:330-337). NO
//     second device, NO volkLoadDevice. Requires the ctx was born wantInterop
//     (gameosmain.cpp: init(wantInterop=true)).
//   - Feed (STEP 0 decision — VK-side copy/re-pack, NOT external buffer share):
//       * recipe  = mission-static, copied ONCE at first-run into a VK SSBO from
//         WaterStream::GetRecipes()/GetRecipeCount().
//       * thin    = per-frame, CPU-staged from the caller's thin-record array
//         each frame into a VK SSBO. No fenced GL buffer crosses the seam -> the
//         GL ring-3 fence (id 43) is irrelevant to VK; R2 sync hazard dissolves.
//       * scene color IN + scene depth IN = CPU/GL bridged like SSAO's inputs
//         (glCopyImageSubData color; glGetTexImage(DEPTH) -> staging -> depth img).
//   - Pipeline (THIS tree's PipelineRegistry WaterArmed row, :227-249):
//       reverse-Z GEQUAL, depth-TEST on, depth-WRITE on, cull NONE, AlphaBlend
//       SRC_ALPHA/ONE_MINUS_SRC_ALPHA, MRT (color0 MainColor + color1 GBuffer1).
//       MVP fudge lives in the VS (clip.z += 0.00025*clip.w), byte-copied.
//   - Output: the VK pass LOADs scene color into the shared RGBA16F, alpha-blends
//     water OVER it (identical math to the GL over-composite), STOREs it; GL copies
//     the shared image back into MainColor. Zero-copy on the color surface; forbidden
//     COLOR-op counters MUST stay 0 (depth-IN is EXPECTED, counted separately).
//
// The whole TU compiles to nothing unless MC2_VULKAN_ISLAND is defined.
// =============================================================================

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
#include "gos_terrain_water_stream.h"

// RUNG 6 (VK-WATER-TIER1): in-place shared scene GBuffer co-write — the water
// draw renders directly into the SAME scene color0(MRT0)+normal(MRT1)+depth
// images GL authored (no offscreen water target, no GL composite pass, no
// glGetTexImage depth readback). Same substrate + accessors the terrain/prop/
// mech Tier-1 islands use.
#define MC2_VULKAN_SHARED_SCENE_COLOR_IMPL_ISLAND
#include "vulkan_shared_scene_color.h"
#define MC2_VULKAN_SHARED_SCENE_DEPTH_IMPL_ISLAND
#include "vulkan_shared_scene_depth.h"
#include "vk_shared_gbuffer_gate.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cstdint>
#include <vector>
#include <string>
#include <chrono>

// ---------------------------------------------------------------------------
// Health snapshot (mirrors the SSAO island's) — read by debug_state_dump.cpp.
// ---------------------------------------------------------------------------
namespace {

struct WaterIslandHealth {
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
    unsigned long usedInterop = 0;
    unsigned long thinRecords = 0;   // records fed this frame
    unsigned long recipeCount = 0;   // recipe rows uploaded once
    double copyColorInUs = 0.0, copyDepthInUs = 0.0, drawUs = 0.0, colorOutUs = 0.0, totalUs = 0.0;
};
WaterIslandHealth g_health;

void setFallback(const char* reason) {
    std::strncpy(g_health.fallbackReason, reason ? reason : "",
                 sizeof(g_health.fallbackReason) - 1);
    g_health.fallbackReason[sizeof(g_health.fallbackReason) - 1] = '\0';
}

// Forbidden COLOR-op counters (blocker #5). Any of these firing on the interop
// path means a COLOR copy slipped through -> fail the frame. DEPTH-in copies are
// EXPECTED (GL scene depth import) and counted SEPARATELY so they never trip the
// color assert.
struct InteropForbidden {
    unsigned long vkCmdCopyBufferToImage = 0;  // COLOR staging upload (must be 0)
    unsigned long vkCmdCopyImageToBuffer = 0;  // COLOR readback       (must be 0)
    unsigned long glGetTexImage          = 0;  // COLOR pull           (must be 0)
    unsigned long glTexSubImage2D        = 0;  // COLOR writeback      (must be 0)
};
InteropForbidden g_forbidden;
unsigned long g_depthInGetTexImage       = 0;  // EXPECTED (scene depth import)
unsigned long g_depthInCopyBufferToImage = 0;  // EXPECTED (staging -> depth image)

void vlog(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    std::fprintf(stderr, "[VK_WATER_ISLAND] ");
    std::vfprintf(stderr, fmt, ap);
    std::fprintf(stderr, "\n");
    va_end(ap);
}

bool waterInteropGateRequested() {
    static int state = -1;
    if (state == -1) {
        const char* e = std::getenv("MC2_VK_WATER_INTEROP");
        state = (e && e[0] == '1') ? 1 : 0;
    }
    return state == 1;
}

// MC2_VK_WATER_TIER1 (rung 6, VK full-ownership ladder): render the armed water
// BASE layer IN-PLACE into the shared scene GBuffer (color0+normal+depth) like
// the terrain/prop/mech Tier-1 islands — no offscreen target, no GL composite,
// no depth readback. envTriState OPT-IN (unset -> OFF regardless of
// MC2_VK_SHARED_GBUFFER; explicit "0" also OFF) — the same conservative
// new-rung pattern as MC2_VK_TERRAIN_SKIRTS / MC2_VK_TERRAIN_VISUALDISPLACE_FAR
// (rungs 4/5). Takes precedence over the legacy MC2_VK_WATER_INTEROP (P9a
// offscreen+composite proof) when both are set.
bool waterTier1Requested() {
    static const bool s_on = (mc2vkgate::envTriState("MC2_VK_WATER_TIER1") == 1);
    return s_on;
}

// MC2_VK_WATER_PERF — fine-grained per-sub-step microsecond breakdown of the VK
// water interop path (additive diagnostic; zero cost when OFF). Emits a
// [VK_WATER_PERF] line per frame with the readback/stage/depth-in/sem/submit/
// fence-wait/composite split so the ~11ms Tracy zone can be attributed.
bool waterPerfGateRequested() {
    static int state = -1;
    if (state == -1) {
        const char* e = std::getenv("MC2_VK_WATER_PERF");
        state = (e && e[0] == '1') ? 1 : 0;
    }
    return state == 1;
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

// std140 UBO mirrors (LOCKSTEP with water_geom.vert/.frag). ------------------
struct WaterVsParams {
    float worldToClipGL[16]; // column_major; SAME raw 16 floats as GL GL_FALSE upload
    float p0[4];             // waterElevation, alphaDepth, mapTopLeftX, mapTopLeftY
    float p1[4];             // frameCos, maxMinUV, uvScale, uvOffsetX
    float p2[4];             // uvOffsetY, fudge, alphaEdgeByte, alphaMiddleByte
    float p3[4];             // alphaDeepByte, pad, pad, pad
};
struct WaterFsParams {
    float fog_color[4];
    float f0[4];             // time, camX, camY, camZ
    float f1[4];             // alphaDepth, ABSORPTION_DENSITY, WATER_MAX_ALPHA, RIPPLE_GAIN
    float f2[4];             // GLINT_GAIN, pad, pad, pad
    float DEEP_COLOR[4];
    float SHALLOW_COLOR[4];
};

const VkFormat kColorFmt = VK_FORMAT_R16G16B16A16_SFLOAT; // MainColor + GBuffer1
const VkFormat kDepthFmt = VK_FORMAT_D32_SFLOAT;          // imported GL scene depth (float)

// The persistent island state.
struct WaterIsland {
    bool disabled = false;
    bool inited   = false;
    bool useInterop = false;
    bool tier1    = false;   // rung 6: in-place shared scene GBuffer co-write

    // Borrowed shared scene GBuffer (tier1 only; NOT owned — postprocess createFBOs
    // owns lifetime; a resize rebuilds them AND resizes this island, which refetches).
    VkImageResource* sColorRes  = nullptr;
    VkImageResource* sNormalRes = nullptr;
    VkImageResource* sDepthRes  = nullptr;
    GlInteropView*   sColorView  = nullptr;
    GlInteropView*   sNormalView = nullptr;
    GlInteropView*   sDepthView  = nullptr;

    int w = 0, h = 0;

    // Borrowed from the shared engine context (NOT owned).
    VkInstance       instance  = VK_NULL_HANDLE;
    VkPhysicalDevice phys      = VK_NULL_HANDLE;
    VkDevice         device    = VK_NULL_HANDLE;
    uint32_t         gfxFamily = UINT32_MAX;
    VkQueue          queue     = VK_NULL_HANDLE;
    VmaAllocator     allocator = VK_NULL_HANDLE; // borrowed from ctx

    // Shared exportable color (color0 = MainColor). GBuffer1 is a VK-owned local
    // (its parity ride is optional for P9a; still produced for MRT completeness).
    VkImageResource colorResource{};     // exportable RGBA16F, color attachment 0
    GlInteropView   colorGlView{};
    GLuint          interopFbo = 0;      // GL FBO wrapping colorGlView.glTex

    VkImage       gbuf1Image = VK_NULL_HANDLE; VmaAllocation gbuf1Alloc = VK_NULL_HANDLE;
    VkImageView   gbuf1View  = VK_NULL_HANDLE;
    VkImage       depthImage = VK_NULL_HANDLE; VmaAllocation depthAlloc = VK_NULL_HANDLE;
    VkImageView   depthView  = VK_NULL_HANDLE;

    // Depth import staging (GL scene depth -> host buffer -> depth image).
    VkBuffer      depthStaging = VK_NULL_HANDLE; VmaAllocation depthStagingAlloc = VK_NULL_HANDLE;
    std::vector<float> depthScratch;

    // Feed buffers (VK-side copies of the GL water feed).
    VkBuffer      recipeBuf = VK_NULL_HANDLE; VmaAllocation recipeAlloc = VK_NULL_HANDLE;
    VkDeviceSize  recipeBytes = 0;
    bool          recipeUploaded = false;
    VkBuffer      thinBuf   = VK_NULL_HANDLE; VmaAllocation thinAlloc = VK_NULL_HANDLE;
    VkDeviceSize  thinCapBytes = 0;          // grows to high-water-mark
    VkBuffer      vsUbo     = VK_NULL_HANDLE; VmaAllocation vsUboAlloc = VK_NULL_HANDLE;
    VkBuffer      fsUbo     = VK_NULL_HANDLE; VmaAllocation fsUboAlloc = VK_NULL_HANDLE;

    VkRenderPass      rpass  = VK_NULL_HANDLE;
    VkFramebuffer     fb     = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkDescriptorPool  pool   = VK_NULL_HANDLE;
    VkDescriptorSet   set    = VK_NULL_HANDLE;
    VkPipelineLayout  playout = VK_NULL_HANDLE;
    VkPipeline        pipe   = VK_NULL_HANDLE;
    VkShaderModule    vert   = VK_NULL_HANDLE;
    VkShaderModule    frag   = VK_NULL_HANDLE;

    VkCommandPool     cpool  = VK_NULL_HANDLE;
    VkCommandBuffer   cbuf[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkFence           fence  = VK_NULL_HANDLE;

    // GL composite (option b): lazily-compiled fullscreen-triangle program +
    // empty VAO used to alpha-blend the water-only shared texture onto MainColor.
    GLuint glCompositeProg = 0;
    GLuint glCompositeVao  = 0;
    GLint  glCompositeTexLoc = -1;

    bool descNeedsThinRewrite = true; // thinBuf realloc -> descriptor rewrite
};

void destroy_island(WaterIsland* s) {
    if (!s) return;
    VkDevice d = s->device;
    if (d != VK_NULL_HANDLE) vkDeviceWaitIdle(d);
    if (s->interopFbo) { glDeleteFramebuffers(1, &s->interopFbo); s->interopFbo = 0; }
    glInteropViewDestroy(&s->colorGlView);
    if (d != VK_NULL_HANDLE) vkImageResourceDestroy(&s->colorResource, d);
    if (s->pipe)    vkDestroyPipeline(d, s->pipe, nullptr);
    if (s->playout) vkDestroyPipelineLayout(d, s->playout, nullptr);
    if (s->vert)    vkDestroyShaderModule(d, s->vert, nullptr);
    if (s->frag)    vkDestroyShaderModule(d, s->frag, nullptr);
    if (s->dsl)     vkDestroyDescriptorSetLayout(d, s->dsl, nullptr);
    if (s->pool)    vkDestroyDescriptorPool(d, s->pool, nullptr);
    if (s->fb)      vkDestroyFramebuffer(d, s->fb, nullptr);
    if (s->rpass)   vkDestroyRenderPass(d, s->rpass, nullptr);
    if (s->gbuf1View)  vkDestroyImageView(d, s->gbuf1View, nullptr);
    if (s->gbuf1Image) vmaDestroyImage(s->allocator, s->gbuf1Image, s->gbuf1Alloc);
    if (s->depthView)  vkDestroyImageView(d, s->depthView, nullptr);
    if (s->depthImage) vmaDestroyImage(s->allocator, s->depthImage, s->depthAlloc);
    if (s->depthStaging) vmaDestroyBuffer(s->allocator, s->depthStaging, s->depthStagingAlloc);
    if (s->recipeBuf) vmaDestroyBuffer(s->allocator, s->recipeBuf, s->recipeAlloc);
    if (s->thinBuf)   vmaDestroyBuffer(s->allocator, s->thinBuf, s->thinAlloc);
    if (s->vsUbo)     vmaDestroyBuffer(s->allocator, s->vsUbo, s->vsUboAlloc);
    if (s->fsUbo)     vmaDestroyBuffer(s->allocator, s->fsUbo, s->fsUboAlloc);
    if (s->cpool)     vkDestroyCommandPool(d, s->cpool, nullptr);
    if (s->fence)     vkDestroyFence(d, s->fence, nullptr);
    if (s->glCompositeProg) { glDeleteProgram(s->glCompositeProg); s->glCompositeProg = 0; }
    if (s->glCompositeVao)  { glDeleteVertexArrays(1, &s->glCompositeVao); s->glCompositeVao = 0; }
    // NOTE: instance/phys/device/queue/allocator are BORROWED — never destroyed here.
}

WaterIsland* g_island = nullptr;

bool make_image(WaterIsland* s, VkFormat fmt, VkImageUsageFlags usage,
                VkImage* img, VmaAllocation* alloc) {
    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    ii.extent = {(uint32_t)s->w, (uint32_t)s->h, 1};
    ii.mipLevels = 1; ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    if (vmaCreateImage(s->allocator, &ii, &aci, img, alloc, nullptr) != VK_SUCCESS) {
        vlog("vmaCreateImage failed."); return false;
    }
    return true;
}
bool make_view(WaterIsland* s, VkImage img, VkFormat fmt, VkImageAspectFlags aspect, VkImageView* view) {
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt;
    vi.subresourceRange = {aspect, 0, 1, 0, 1};
    if (vkCreateImageView(s->device, &vi, nullptr, view) != VK_SUCCESS) {
        vlog("vkCreateImageView failed."); return false;
    }
    return true;
}
bool make_buffer(WaterIsland* s, VkDeviceSize size, VkBufferUsageFlags usage,
                 VkBuffer* buf, VmaAllocation* alloc) {
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

// Write the 4-binding descriptor set (0=vsUbo, 1=fsUbo, 5=recipe, 6=thin).
// Called at init (after the set is allocated) and whenever a buffer is (re)allocated.
// No-op until the descriptor SET exists — ensure_thin_capacity / recipe realloc can
// run during init BEFORE vkAllocateDescriptorSets, and writing to a NULL set is a
// validation error + crash (dstSet == VK_NULL_HANDLE). The post-alloc write_descriptors
// in init_island covers the initial bind; later reallocs re-write with a valid set.
void write_descriptors(WaterIsland* s) {
    if (s->set == VK_NULL_HANDLE) return;   // set not allocated yet (early init path)
    VkDescriptorBufferInfo vsInfo{ s->vsUbo, 0, VK_WHOLE_SIZE };
    VkDescriptorBufferInfo fsInfo{ s->fsUbo, 0, VK_WHOLE_SIZE };
    VkDescriptorBufferInfo recInfo{ s->recipeBuf, 0, VK_WHOLE_SIZE };
    VkDescriptorBufferInfo thinInfo{ s->thinBuf, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet w[4]{};
    w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstSet = s->set;
    w[0].dstBinding = 0; w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[0].pBufferInfo = &vsInfo;
    w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[1].dstSet = s->set;
    w[1].dstBinding = 1; w[1].descriptorCount = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[1].pBufferInfo = &fsInfo;
    w[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[2].dstSet = s->set;
    w[2].dstBinding = 5; w[2].descriptorCount = 1;
    w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[2].pBufferInfo = &recInfo;
    w[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[3].dstSet = s->set;
    w[3].dstBinding = 6; w[3].descriptorCount = 1;
    w[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[3].pBufferInfo = &thinInfo;
    vkUpdateDescriptorSets(s->device, 4, w, 0, nullptr);
}

// Grow the thin SSBO to at least `bytes`; rewrite descriptor binding 6 on realloc.
bool ensure_thin_capacity(WaterIsland* s, VkDeviceSize bytes) {
    if (s->thinBuf != VK_NULL_HANDLE && bytes <= s->thinCapBytes) return true;
    if (s->thinBuf != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(s->device); // simple + safe for a realloc (rare, grows once)
        vmaDestroyBuffer(s->allocator, s->thinBuf, s->thinAlloc);
        s->thinBuf = VK_NULL_HANDLE;
    }
    VkDeviceSize cap = bytes + bytes / 2 + 4096; // headroom
    if (!make_buffer(s, cap, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &s->thinBuf, &s->thinAlloc))
        return false;
    s->thinCapBytes = cap;
    write_descriptors(s);
    return true;
}

// One-time init: borrow the shared device, build the exportable color seam,
// gbuffer1 + depth images, recipe/thin/UBO buffers, render pass, pipeline.
bool init_island(WaterIsland* s, int w, int h) {
    s->w = w; s->h = h;
    ++g_health.attempted;

    // VK-P9a: bring up the shared engine device on-demand, DECOUPLED from present.
    // If MC2_RENDER_BACKEND=vulkan already born it, this is a no-op; otherwise it
    // inits the SAME shared device (one volkLoadDevice, interop exts, NO swapchain)
    // so the water region runs on the GL-present path (SSAO model, shared device).
    // Fail-soft -> GL water draw.
    if (!mc2vk::ensureEngineVulkanContextForInterop()) {
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

    // ---- RUNG 6 (tier1): borrow the SHARED scene GBuffer (color0+normal+depth)
    // the GL scene renders into — same accessors the terrain island uses
    // (vulkan_terrain_island.cpp:879-888). Fail-soft: not ready => GL water draw. ----
    s->tier1 = waterTier1Requested();
    if (s->tier1) {
        s->sColorRes  = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_COLOR0);
        s->sNormalRes = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_NORMAL);
        s->sDepthRes  = mc2SharedSceneDepthGetResource();
        s->sColorView  = mc2SharedSceneColorGetGlView(MC2_SHARED_SCENE_COLOR0);
        s->sNormalView = mc2SharedSceneColorGetGlView(MC2_SHARED_SCENE_NORMAL);
        s->sDepthView  = mc2SharedSceneDepthGetGlView();
        if (!s->sColorRes || !s->sNormalRes || !s->sDepthRes ||
            !s->sColorView || !s->sNormalView || !s->sDepthView) {
            setFallback("shared_gbuffer_not_ready");
            return false;
        }
        s->useInterop = true;
        g_health.interopActive = 1;
        g_health.vk2glSemActive = s->sColorView->vkToGlImported ? 1 : 0;
        vlog("TIER-1 shared scene GBuffer seam ACTIVE — in-place water co-write "
             "(color0+normal+depth, no composite, no depth readback; vk2gl_sem=%d).",
             g_health.vk2glSemActive);
    }

    // ---- Exportable shared RGBA16F color (color attachment 0). ----
    const bool wantInterop = waterInteropGateRequested();
    if (s->tier1) {
        // tier1 owns no color surface; skip the P9a offscreen seam entirely.
    } else if (wantInterop) {
        bool ok = vkImageResourceCreate(&s->colorResource, s->device, s->phys,
                                        (uint32_t)w, (uint32_t)h, kColorFmt,
                                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                        /*exportable*/ true);
        if (ok) ok = glInteropViewCreate(&s->colorGlView, &s->colorResource, s->phys, GL_RGBA16F);
        if (ok) {
            glGenFramebuffers(1, &s->interopFbo);
            glBindFramebuffer(GL_FRAMEBUFFER, s->interopFbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s->colorGlView.glTex, 0);
            GLenum fbst = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            if (fbst != GL_FRAMEBUFFER_COMPLETE) { vlog("shared-tex FBO incomplete 0x%x.", fbst); ok = false; }
        }
        if (ok) {
            s->useInterop = true;
            g_health.interopActive = 1;
            g_health.vk2glSemActive = s->colorGlView.vkToGlImported ? 1 : 0;
            vlog("color interop seam ACTIVE — zero-copy exportable RGBA16F color0 (vk2gl_sem=%d).",
                 g_health.vk2glSemActive);
        } else {
            if (s->interopFbo) { glDeleteFramebuffers(1, &s->interopFbo); s->interopFbo = 0; }
            glInteropViewDestroy(&s->colorGlView);
            vkImageResourceDestroy(&s->colorResource, s->device);
            setFallback("interop_setup_failed");
            return false; // P9a is interop-only; no readback fallback path
        }
    } else {
        setFallback("gate_off"); return false;
    }

    // ---- GBuffer1 (color attachment 1, VK-owned local) + depth (imported). ----
    // tier1 needs NONE of these: it attaches the shared scene normal + shared scene
    // depth directly (LOAD-preserve) — no local gbuf1, no depth import staging.
    if (!s->tier1) {
        if (!make_image(s, kColorFmt, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, &s->gbuf1Image, &s->gbuf1Alloc)) return false;
        if (!make_view(s, s->gbuf1Image, kColorFmt, VK_IMAGE_ASPECT_COLOR_BIT, &s->gbuf1View)) return false;
        if (!make_image(s, kDepthFmt, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        &s->depthImage, &s->depthAlloc)) return false;
        if (!make_view(s, s->depthImage, kDepthFmt, VK_IMAGE_ASPECT_DEPTH_BIT, &s->depthView)) return false;
        const VkDeviceSize depthBytes = (VkDeviceSize)w * h * 4;
        if (!make_buffer(s, depthBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &s->depthStaging, &s->depthStagingAlloc)) return false;
        s->depthScratch.resize((size_t)w * h);
    }

    // ---- UBOs. ----
    if (!make_buffer(s, sizeof(WaterVsParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &s->vsUbo, &s->vsUboAlloc)) return false;
    if (!make_buffer(s, sizeof(WaterFsParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &s->fsUbo, &s->fsUboAlloc)) return false;

    // ---- Recipe SSBO (mission-static, copied once at first run). Sized to the
    // current recipe count; if empty now, deferred to run() first frame. ----
    {
        const uint32_t rc = WaterStream::GetRecipeCount();
        // 64B/recipe (WaterRecipe). Allocate at least 1 to keep the descriptor valid.
        s->recipeBytes = (VkDeviceSize)(rc ? rc : 1) * 64;
        if (!make_buffer(s, s->recipeBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &s->recipeBuf, &s->recipeAlloc)) return false;
    }
    // Thin SSBO — start with a small ring; grows in run().
    if (!ensure_thin_capacity(s, 64 * 1024)) return false; // ~1365 records to start

    // ---- Render pass: color0 (shared, LOAD scene color, alpha-blend, STORE) +
    // color1 (gbuffer1, CLEAR, STORE local) + depth (LOAD imported, GEQUAL, write). ----
    {
        VkAttachmentDescription att[3]{};
        if (s->tier1) {
        // RUNG 6 tier1: LOAD-preserve pass over the SHARED scene GBuffer (pattern:
        // vulkan_terrain_island.cpp:895-974, EXCEPT depth is LOAD not CLEAR — water
        // runs AFTER terrain/props/mechs wrote scene depth this frame and must
        // GEQUAL-test against it, exactly like the GL water draw does).
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
        // Shared depth (D32_SFLOAT_S8_UINT export): LOAD both aspects (preserve the
        // scene depth + whatever stencil GL holds), STORE both (water WRITES depth,
        // matching GL — OOB-FOG-WATER-DEPTH-1 requires water depth writes).
        att[2].format = s->sDepthRes->format; att[2].samples = VK_SAMPLE_COUNT_1_BIT;
        att[2].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[2].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        att[2].finalLayout   = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        } else {
        // 0: shared color = WATER-ONLY contribution (composite option b). CLEAR to
        // TRANSPARENT (0,0,0,0) each frame; the water FS writes water color+alpha for
        // water fragments and discards non-water -> non-water pixels stay transparent.
        // GL then alpha-composites JUST this water result onto MainColor, so non-water
        // pixels are byte-identical to the GL path BY CONSTRUCTION (no whole-frame LOAD/
        // STORE round-trip). Interop layout-stable: GL hands the image in
        // COLOR_ATTACHMENT_OPTIMAL; we CLEAR+draw+STORE, stay COLOR_ATTACHMENT_OPTIMAL
        // (no transition write across the GL semaphore, sync-val clean).
        att[0].format = kColorFmt; att[0].samples = VK_SAMPLE_COUNT_1_BIT;
        att[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att[0].finalLayout   = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        // 1: gbuffer1 local. CLEAR to the flat-up sentinel is not needed (frag writes
        // every covered pixel); DONT_CARE load, STORE (unused downstream in P9a).
        att[1].format = kColorFmt; att[1].samples = VK_SAMPLE_COUNT_1_BIT;
        att[1].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        att[1].finalLayout   = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        // 2: depth (imported GL scene depth). LOAD (test against it), STORE (water writes).
        att[2].format = kDepthFmt; att[2].samples = VK_SAMPLE_COUNT_1_BIT;
        att[2].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[2].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        att[2].finalLayout   = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        }

        VkAttachmentReference colorRefs[2]{
            {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
            {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
        VkAttachmentReference depthRef{2, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 2; sub.pColorAttachments = colorRefs;
        sub.pDepthStencilAttachment = &depthRef;

        // Incoming + outgoing external deps — interop cross-frame WAW serialize (SSAO
        // P1.5 pattern). WAW FIX: the shared color image is written every frame (CLEAR +
        // water draw + STORE) across two parity slots; without an INCOMING external dep
        // sync-val flags a WRITE_AFTER_WRITE between frame N's storeOp and frame N+1's
        // color write (the GL semaphore serializes it functionally, but sync-val can't
        // see across the GL boundary). deps[0]: order THIS frame's color+depth writes
        // AFTER any prior color/depth write on the shared/local images. srcAccessMask
        // includes COLOR_ATTACHMENT_WRITE so the prior store is a visible hazard source.
        VkSubpassDependency deps[2]{};
        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL; deps[0].dstSubpass = 0;
        deps[0].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        deps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[0].dependencyFlags = 0;
        // deps[1]: OUTGOING — the color store completes before the cross-API signal
        // hands the image back to GL (dst = BOTTOM_OF_PIPE, no dstAccess).
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
        if (vkCreateRenderPass(s->device, &rpci, nullptr, &s->rpass) != VK_SUCCESS) {
            vlog("vkCreateRenderPass failed."); return false;
        }
        VkImageView fbViews[3];
        if (s->tier1) {
            fbViews[0] = s->sColorRes->view;   // shared scene color0 (MRT0)
            fbViews[1] = s->sNormalRes->view;  // shared scene normal (MRT1)
            fbViews[2] = s->sDepthRes->view;   // shared scene depth
        } else {
            fbViews[0] = s->colorResource.view;
            fbViews[1] = s->gbuf1View;
            fbViews[2] = s->depthView;
        }
        VkFramebufferCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = s->rpass; fci.attachmentCount = 3; fci.pAttachments = fbViews;
        fci.width = (uint32_t)w; fci.height = (uint32_t)h; fci.layers = 1;
        if (vkCreateFramebuffer(s->device, &fci, nullptr, &s->fb) != VK_SUCCESS) {
            vlog("vkCreateFramebuffer failed."); return false;
        }
    }

    // ---- Descriptor layout/pool/set (2 UBO + 2 SSBO), both stages. ----
    {
        VkDescriptorSetLayoutBinding b[4]{};
        b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        b[2].binding = 5; b[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[2].descriptorCount = 1; b[2].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        b[3].binding = 6; b[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[3].descriptorCount = 1; b[3].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.bindingCount = 4; lci.pBindings = b;
        if (vkCreateDescriptorSetLayout(s->device, &lci, nullptr, &s->dsl) != VK_SUCCESS) {
            vlog("vkCreateDescriptorSetLayout failed."); return false;
        }
        VkDescriptorPoolSize sizes[2]{};
        sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; sizes[0].descriptorCount = 2;
        sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; sizes[1].descriptorCount = 2;
        VkDescriptorPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.maxSets = 1; pci.poolSizeCount = 2; pci.pPoolSizes = sizes;
        if (vkCreateDescriptorPool(s->device, &pci, nullptr, &s->pool) != VK_SUCCESS) {
            vlog("vkCreateDescriptorPool failed."); return false;
        }
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = s->pool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &s->dsl;
        if (vkAllocateDescriptorSets(s->device, &dsai, &s->set) != VK_SUCCESS) {
            vlog("vkAllocateDescriptorSets failed."); return false;
        }
        write_descriptors(s);
    }

    // ---- Pipeline (procedural VS, no vertex input; GEQUAL depth on+write on;
    // cull none; MRT alpha-blend SRC_ALPHA/ONE_MINUS_SRC_ALPHA on both color atts). ----
    {
        s->vert = load_spv(s->device, spv_dir() + "water_geom.vert.spv");
        s->frag = load_spv(s->device, spv_dir() + "water_geom.frag.spv");
        if (s->vert == VK_NULL_HANDLE || s->frag == VK_NULL_HANDLE) { setFallback("spv_load_failed"); return false; }

        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 1; plci.pSetLayouts = &s->dsl;
        if (vkCreatePipelineLayout(s->device, &plci, nullptr, &s->playout) != VK_SUCCESS) {
            vlog("vkCreatePipelineLayout failed."); return false;
        }

        VkPipelineVertexInputStateCreateInfo vin{};
        vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO; // count=0 (procedural)
        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        // Depth: reverse-Z GEQUAL, test on, WRITE ON (PipelineRegistry WaterArmed row).
        VkPipelineDepthStencilStateCreateInfo ds{};
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        ds.depthTestEnable = VK_TRUE;
        ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;

        // COMPOSITE-STAGE PARITY FIX: PREMULTIPLIED-alpha accumulation into the
        // transparent-cleared water-only target. The FS emits PREMULTIPLIED
        // vec4(col*a, a); the target blend is ONE / ONE_MINUS_SRC_ALPHA (premult-over).
        // A single water fragment over transparent stores (col*a, a). GL then composites
        // with premult-over (GL_ONE / GL_ONE_MINUS_SRC_ALPHA): MainColor = col*a +
        // MainColor*(1-a) — BIT-IDENTICAL to GL's native one-stage water blend
        // (col*a + terrain*(1-a) pre-tonemap). The prior SRC_ALPHA/ONE_MINUS_SRC_ALPHA
        // into transparent + SRC_ALPHA composite double-applied alpha (stored a², then
        // blended again) -> the warm R+10/B-5 tint. Premultiplied two-stage == one-stage.
        VkPipelineColorBlendAttachmentState cba[2];
        if (s->tier1) {
            // RUNG 6 tier1 blend — in-place over the LOADed scene color.
            // The FS emits PREMULTIPLIED (col*a, a) (P9a composite-parity contract):
            //   color = ONE / ONE_MINUS_SRC_ALPHA  -> dst.rgb = col*a + dst*(1-a),
            //     the EXACT result of GL's straight-alpha SRC_ALPHA/OMSA one-stage
            //     water blend (GL FS emits vec4(col, a)).
            //   alpha = SRC_ALPHA / ONE_MINUS_SRC_ALPHA -> dst.a = a*a + dst.a*(1-a),
            //     matching GL's alpha-channel arithmetic (src factor uses src.a=a).
            cba[0] = {};
            cba[0].blendEnable = VK_TRUE;
            cba[0].srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            cba[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            cba[0].colorBlendOp = VK_BLEND_OP_ADD;
            cba[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            cba[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            cba[0].alphaBlendOp = VK_BLEND_OP_ADD;
            cba[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            // Attachment 1 = the SHARED scene normal. GL's water GBuffer1 output has
            // src.a == 0.0, so under GL's SRC_ALPHA blend it contributes NOTHING
            // (dst unchanged). writeMask = 0 reproduces that exact no-op without
            // relying on the independentBlend device feature.
            cba[1] = {};
            cba[1].blendEnable = VK_FALSE;
            cba[1].colorWriteMask = 0;
        } else {
        for (int i = 0; i < 2; ++i) {
            cba[i] = {};
            cba[i].blendEnable = VK_TRUE;
            cba[i].srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            cba[i].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            cba[i].colorBlendOp = VK_BLEND_OP_ADD;
            cba[i].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            cba[i].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            cba[i].alphaBlendOp = VK_BLEND_OP_ADD;
            cba[i].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        }
        }
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
        if (vkCreateGraphicsPipelines(s->device, VK_NULL_HANDLE, 1, &gp, nullptr, &s->pipe) != VK_SUCCESS) {
            vlog("vkCreateGraphicsPipelines failed."); return false;
        }
    }

    // ---- Command pool + 2 command buffers (per parity) + fence. ----
    {
        VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = s->gfxFamily;
        if (vkCreateCommandPool(s->device, &pci, nullptr, &s->cpool) != VK_SUCCESS) {
            vlog("vkCreateCommandPool failed."); return false;
        }
        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = s->cpool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 2;
        if (vkAllocateCommandBuffers(s->device, &cai, s->cbuf) != VK_SUCCESS) {
            vlog("vkAllocateCommandBuffers failed."); return false;
        }
        VkFenceCreateInfo fnci{}; fnci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vkCreateFence(s->device, &fnci, nullptr, &s->fence) != VK_SUCCESS) {
            vlog("vkCreateFence failed."); return false;
        }
    }

    s->inited = true;
    setFallback("");
    vlog("island initialized %dx%d (borrowed shared device; color0 interop RGBA16F, "
         "gbuffer1 local, depth D32 imported; procedural water VS; GEQUAL reverse-Z depth-write-on; "
         "MRT alpha-blend).", w, h);
    return true;
}

// Composite option (b): alpha-blend the WATER-ONLY shared texture onto MainColor
// (the currently-bound GL draw FBO at water-render time). Lazily compiles a
// fullscreen-triangle passthrough program. Saves+restores all touched GL state so
// the surrounding renderWaterFastPath epilogue is undisturbed. Non-water pixels in
// the shared image are transparent (a=0) -> unchanged; water pixels blend
// dst = src.rgb*src.a + dst*(1-src.a) via SRC_ALPHA/ONE_MINUS_SRC_ALPHA.
void composite_water_onto_maincolor(WaterIsland* s, int w, int h) {
    if (s->colorGlView.glTex == 0) return;
    if (s->glCompositeProg == 0) {
        // Fullscreen triangle (no VBO); sample the water tex at the screen UV.
        static const char* kVs =
            "#version 430\n"
            "out vec2 vUv;\n"
            "void main(){\n"
            "  vec2 p = vec2((gl_VertexID<<1)&2, gl_VertexID&2);\n"
            "  vUv = p; gl_Position = vec4(p*2.0-1.0, 0.0, 1.0);\n"
            "}\n";
        static const char* kFs =
            "#version 430\n"
            "in vec2 vUv; layout(location=0) out vec4 o;\n"
            "uniform sampler2D uWater;\n"
            "void main(){ o = texture(uWater, vUv); }\n";
        auto compile = [](GLenum type, const char* src)->GLuint{
            GLuint sh = glCreateShader(type);
            glShaderSource(sh, 1, &src, nullptr); glCompileShader(sh);
            GLint ok=0; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
            if(!ok){ char log[512]; glGetShaderInfoLog(sh,512,nullptr,log);
                vlog("composite shader compile fail: %s", log); glDeleteShader(sh); return 0; }
            return sh;
        };
        GLuint vs = compile(GL_VERTEX_SHADER, kVs);
        GLuint fs = compile(GL_FRAGMENT_SHADER, kFs);
        if (vs && fs) {
            GLuint prog = glCreateProgram();
            glAttachShader(prog, vs); glAttachShader(prog, fs); glLinkProgram(prog);
            GLint ok=0; glGetProgramiv(prog, GL_LINK_STATUS, &ok);
            if(ok){ s->glCompositeProg = prog; s->glCompositeTexLoc = glGetUniformLocation(prog,"uWater"); }
            else { char log[512]; glGetProgramInfoLog(prog,512,nullptr,log);
                vlog("composite link fail: %s", log); glDeleteProgram(prog); }
        }
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        if (s->glCompositeVao == 0) glGenVertexArrays(1, &s->glCompositeVao);
    }
    if (s->glCompositeProg == 0 || s->glCompositeVao == 0) return;

    // Save GL state we touch (mirror the discipline of the surrounding water path).
    GLint  savedProg=0; glGetIntegerv(GL_CURRENT_PROGRAM, &savedProg);
    GLint  savedVao=0;  glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &savedVao);
    GLint  savedActive=0; glGetIntegerv(GL_ACTIVE_TEXTURE, &savedActive);
    GLboolean savedBlend = glIsEnabled(GL_BLEND);
    GLboolean savedDepthTest = glIsEnabled(GL_DEPTH_TEST);
    GLboolean savedCull = glIsEnabled(GL_CULL_FACE);
    GLint  savedSrcRGB=0, savedDstRGB=0, savedSrcA=0, savedDstA=0;
    glGetIntegerv(GL_BLEND_SRC_RGB,&savedSrcRGB); glGetIntegerv(GL_BLEND_DST_RGB,&savedDstRGB);
    glGetIntegerv(GL_BLEND_SRC_ALPHA,&savedSrcA); glGetIntegerv(GL_BLEND_DST_ALPHA,&savedDstA);
    GLint savedTex0=0; glActiveTexture(GL_TEXTURE0); glGetIntegerv(GL_TEXTURE_BINDING_2D,&savedTex0);
    GLint savedSampler0=0; glGetIntegeri_v(GL_SAMPLER_BINDING,0,&savedSampler0);
    GLint savedViewport[4]; glGetIntegerv(GL_VIEWPORT, savedViewport);

    glUseProgram(s->glCompositeProg);
    glBindVertexArray(s->glCompositeVao);
    glViewport(0, 0, w, h);
    glDisable(GL_DEPTH_TEST);        // composite is a 2D overlay; MainColor FBO stays bound
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    // PREMULTIPLIED-alpha over: the shared water texture holds premultiplied (col*a, a)
    // (VK FS + premult-over pipeline blend). Composite dst = src.rgb + dst*(1-src.a) =
    // col*a + MainColor*(1-a) — BIT-IDENTICAL to GL's native one-stage water blend
    // (col*a + terrain*(1-a)). This is the composite-stage parity fix: premultiplied
    // two-stage compositing equals one-stage (straight-alpha two-stage double-applied a
    // and gave the warm R+10/B-5 tint).
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s->colorGlView.glTex);
    glBindSampler(0, 0);             // use the shared tex's own LINEAR/CLAMP params
    if (s->glCompositeTexLoc >= 0) glUniform1i(s->glCompositeTexLoc, 0);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // Restore.
    glBindTexture(GL_TEXTURE_2D, (GLuint)savedTex0);
    glBindSampler(0, (GLuint)savedSampler0);
    glActiveTexture((GLenum)savedActive);
    glViewport(savedViewport[0],savedViewport[1],savedViewport[2],savedViewport[3]);
    if (!savedBlend) glDisable(GL_BLEND);
    if (savedDepthTest) glEnable(GL_DEPTH_TEST);
    if (savedCull) glEnable(GL_CULL_FACE);
    glBlendFuncSeparate((GLenum)savedSrcRGB,(GLenum)savedDstRGB,(GLenum)savedSrcA,(GLenum)savedDstA);
    glBindVertexArray((GLuint)savedVao);
    glUseProgram((GLuint)savedProg);
}

} // namespace

// ===========================================================================
// PUBLIC ENTRY POINTS (consumed under #ifdef MC2_VULKAN_ISLAND from
// gameos_graphics.cpp:renderWaterFastPath + debug_state_dump.cpp).
// ===========================================================================

// Health accessor (mirrors mc2_vulkan_ssao_island_health).
extern "C" void mc2_render_backend_water_region_health(
    int* vulkanAvailable, int* buildEnabled, const char** deviceName,
    unsigned long* validationErrors, unsigned long* attempted, unsigned long* usedVulkan,
    const char** fallbackReason, unsigned long* vkDraws,
    int* interopActive, int* vk2glSemActive, unsigned long* usedInterop,
    unsigned long* thinRecords, unsigned long* recipeCount,
    unsigned long* forbiddenCopyBufToImg, unsigned long* forbiddenCopyImgToBuf,
    unsigned long* forbiddenGetTexImage, unsigned long* forbiddenTexSubImage,
    unsigned long* depthInGetTexImage, unsigned long* depthInCopyBufToImg,
    double* copyColorInUs, double* copyDepthInUs, double* drawUs, double* colorOutUs, double* totalUs) {
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
    if (thinRecords)      *thinRecords      = g_health.thinRecords;
    if (recipeCount)      *recipeCount      = g_health.recipeCount;
    if (forbiddenCopyBufToImg) *forbiddenCopyBufToImg = g_forbidden.vkCmdCopyBufferToImage;
    if (forbiddenCopyImgToBuf) *forbiddenCopyImgToBuf = g_forbidden.vkCmdCopyImageToBuffer;
    if (forbiddenGetTexImage)  *forbiddenGetTexImage  = g_forbidden.glGetTexImage;
    if (forbiddenTexSubImage)  *forbiddenTexSubImage  = g_forbidden.glTexSubImage2D;
    if (depthInGetTexImage)    *depthInGetTexImage    = g_depthInGetTexImage;
    if (depthInCopyBufToImg)   *depthInCopyBufToImg   = g_depthInCopyBufferToImage;
    if (copyColorInUs)    *copyColorInUs    = g_health.copyColorInUs;
    if (copyDepthInUs)    *copyDepthInUs    = g_health.copyDepthInUs;
    if (drawUs)           *drawUs           = g_health.drawUs;
    if (colorOutUs)       *colorOutUs       = g_health.colorOutUs;
    if (totalUs)          *totalUs          = g_health.totalUs;
}

// True when the gate requests the VK water region (the caller then SKIPS the GL
// MDI draw and calls mc2_vk_water_island_run instead — mutually exclusive).
extern "C" int mc2_vk_water_island_enabled() {
    return (waterTier1Requested() || waterInteropGateRequested()) ? 1 : 0;
}

// Per-frame entry. Inputs (all from the GL side, at water-render time):
//   sceneColorTex/sceneDepthTex — GL scene MainColor + depth textures (w x h).
//   mvp16 — the 16 raw floats GL uploads (glUniformMatrix4fv GL_FALSE) for u_worldToClipGL.
//   thinRecords/thinCount — the CPU-staged per-frame thin-record array (48B each).
//   recipes/recipeCount — mission-static recipe array (64B each; uploaded once).
//   vsP/fsP — the packed uniform params (LOCKSTEP with water_geom.{vert,frag}).
// Returns 1 if the VK water pass rendered+composited this frame; 0 = fell back
// (caller must then run the GL water draw, i.e. treat 0 as "not handled").
extern "C" int mc2_vk_water_island_run(
    unsigned int sceneColorTex, unsigned int sceneDepthTex, int w, int h,
    const void* thinRecords, unsigned int thinCount,
    const void* recipes, unsigned int recipeCount,
    const void* vsParams, const void* fsParams) {
    if (!waterTier1Requested() && !waterInteropGateRequested()) return 0;
    if (thinCount == 0 || recipeCount == 0 || w <= 0 || h <= 0) return 0;
    // tier1 renders in-place into the shared GBuffer and never touches the GL
    // scene textures; only the P9a path needs them.
    if (!waterTier1Requested() && (sceneColorTex == 0 || sceneDepthTex == 0)) return 0;

    using clk = std::chrono::steady_clock;
    auto usSince = [](clk::time_point t0){ return std::chrono::duration<double, std::micro>(clk::now() - t0).count(); };
    const clk::time_point tTotal0 = clk::now();
    const bool perf = waterPerfGateRequested();
    // Fine-grained sub-step timers (only meaningful when `perf`; cheap to always fill).
    double us_stage = 0.0;      // recipe/thin/UBO memcpy staging (steps 1-3)
    double us_depth_readback=0.0; // glGetTexImage(DEPTH) GL->CPU stall (step 5)
    double us_depth_stage = 0.0;  // memcpy depth scratch -> VK staging (step 5)
    double us_record = 0.0;     // command-buffer record (step 7)
    double us_submit = 0.0;     // vkQueueSubmit (step 8)
    double us_sem_wait = 0.0;   // glInteropWaitFromVk (step 8)
    double us_fence_wait = 0.0; // vkWaitForFences inline stall (step 8)
    double us_composite = 0.0;  // GL composite draw (step 9)

    // Lazy init / rebuild on size change.
    if (g_island && (g_island->w != w || g_island->h != h)) {
        destroy_island(g_island); delete g_island; g_island = nullptr;
    }
    if (!g_island) {
        g_island = new WaterIsland();
        if (!init_island(g_island, w, h)) {
            vlog("init failed (%s) — GL fallback.", g_health.fallbackReason);
            destroy_island(g_island); delete g_island; g_island = nullptr;
            return 0;
        }
    }
    WaterIsland* s = g_island;
    if (s->disabled || !s->inited || !s->useInterop) return 0;

    // Snapshot forbidden COLOR counters (prove none fire on the interop path).
    const unsigned long f0a = g_forbidden.vkCmdCopyBufferToImage;
    const unsigned long f0b = g_forbidden.vkCmdCopyImageToBuffer;
    const unsigned long f0c = g_forbidden.glGetTexImage;
    const unsigned long f0d = g_forbidden.glTexSubImage2D;

    const clk::time_point tStage0 = clk::now();
    // ---- 1) Upload recipe SSBO once (mission-static). ----
    if (!s->recipeUploaded) {
        const VkDeviceSize need = (VkDeviceSize)recipeCount * 64;
        if (need > s->recipeBytes) {
            vkDeviceWaitIdle(s->device);
            vmaDestroyBuffer(s->allocator, s->recipeBuf, s->recipeAlloc);
            if (!make_buffer(s, need, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &s->recipeBuf, &s->recipeAlloc)) { s->disabled = true; return 0; }
            s->recipeBytes = need;
            write_descriptors(s);
        }
        void* p = nullptr;
        if (vmaMapMemory(s->allocator, s->recipeAlloc, &p) == VK_SUCCESS) {
            std::memcpy(p, recipes, (size_t)recipeCount * 64);
            vmaUnmapMemory(s->allocator, s->recipeAlloc);
        }
        s->recipeUploaded = true;
        g_health.recipeCount = recipeCount;
    }

    // ---- 2) Upload the per-frame thin records (CPU-staged; no GL buffer crosses). ----
    const VkDeviceSize thinBytes = (VkDeviceSize)thinCount * 48;
    if (!ensure_thin_capacity(s, thinBytes)) { s->disabled = true; return 0; }
    {
        void* p = nullptr;
        if (vmaMapMemory(s->allocator, s->thinAlloc, &p) == VK_SUCCESS) {
            std::memcpy(p, thinRecords, (size_t)thinBytes);
            vmaUnmapMemory(s->allocator, s->thinAlloc);
        }
    }
    g_health.thinRecords = thinCount;

    // ---- 3) Upload UBOs. ----
    {
        void* p = nullptr;
        if (vmaMapMemory(s->allocator, s->vsUboAlloc, &p) == VK_SUCCESS) {
            std::memcpy(p, vsParams, sizeof(WaterVsParams)); vmaUnmapMemory(s->allocator, s->vsUboAlloc);
        }
        if (vmaMapMemory(s->allocator, s->fsUboAlloc, &p) == VK_SUCCESS) {
            std::memcpy(p, fsParams, sizeof(WaterFsParams)); vmaUnmapMemory(s->allocator, s->fsUboAlloc);
        }
    }

    // ============================ RUNG 6 TIER-1 PATH ============================
    // In-place co-write into the SHARED scene GBuffer. No color/depth import, no
    // composite. Frame-sync protocol COPIED from vulkan_terrain_island.cpp:2495-2603.
    if (s->tier1) {
        us_stage = usSince(tStage0);
        g_health.copyColorInUs = 0.0;
        g_health.copyDepthInUs = 0.0;

        VkFrameSync fsC = vkImageResourceBeginFrame(s->sColorRes);
        VkFrameSync fsN = vkImageResourceBeginFrame(s->sNormalRes);
        VkFrameSync fsD = vkImageResourceBeginFrame(s->sDepthRes);
        VkCommandBuffer cb = s->cbuf[fsC.parity & 1u];

        glInteropSignalToVk(*s->sColorView,  GL_LAYOUT_COLOR_ATTACHMENT_EXT,         fsC.parity);
        glInteropSignalToVk(*s->sNormalView, GL_LAYOUT_COLOR_ATTACHMENT_EXT,         fsN.parity);
        glInteropSignalToVk(*s->sDepthView,  GL_LAYOUT_DEPTH_STENCIL_ATTACHMENT_EXT, fsD.parity);

        const clk::time_point tRender0 = clk::now();
        vkResetCommandBuffer(cb, 0);
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) { setFallback("record_failed"); s->disabled = true; return 0; }

        // All attachments LOAD -> clear values ignored (array must still cover them).
        VkClearValue clears[3]{};
        VkRenderPassBeginInfo rbi{};
        rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rbi.renderPass = s->rpass; rbi.framebuffer = s->fb;
        rbi.renderArea = {{0, 0}, {(uint32_t)w, (uint32_t)h}};
        rbi.clearValueCount = 3; rbi.pClearValues = clears;
        vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->playout, 0, 1, &s->set, 0, nullptr);
        vkCmdDraw(cb, thinCount * 6u, 1, 0, 0);
        vkCmdEndRenderPass(cb);
        if (vkEndCommandBuffer(cb) != VK_SUCCESS) { setFallback("record_failed"); s->disabled = true; return 0; }
        us_record = usSince(tRender0);

        // Submit: WAIT all three GL->VK sems; SIGNAL all three VK->GL sems + color fence.
        VkSemaphore waitSems[3];   VkPipelineStageFlags waitStages[3];
        VkSemaphore signalSems[3]; uint32_t waitN = 0, sigN = 0;
        auto addWait = [&](VkSemaphore sem, VkPipelineStageFlags stage){
            if (sem != VK_NULL_HANDLE) { waitSems[waitN] = sem; waitStages[waitN] = stage; ++waitN; } };
        addWait(fsC.signal, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        addWait(fsN.signal, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        addWait(fsD.signal, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);
        const bool gpuC = s->sColorView->vkToGlImported  && fsC.vk2gl != VK_NULL_HANDLE;
        const bool gpuN = s->sNormalView->vkToGlImported && fsN.vk2gl != VK_NULL_HANDLE;
        const bool gpuD = s->sDepthView->vkToGlImported  && fsD.vk2gl != VK_NULL_HANDLE;
        if (gpuC) signalSems[sigN++] = fsC.vk2gl;
        if (gpuN) signalSems[sigN++] = fsN.vk2gl;
        if (gpuD) signalSems[sigN++] = fsD.vk2gl;

        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.waitSemaphoreCount = waitN; si.pWaitSemaphores = waitSems; si.pWaitDstStageMask = waitStages;
        si.commandBufferCount = 1; si.pCommandBuffers = &cb;
        si.signalSemaphoreCount = sigN; si.pSignalSemaphores = signalSems;
        const clk::time_point tSubmit0 = clk::now();
        if (vkQueueSubmit(s->queue, 1, &si, fsC.fence) != VK_SUCCESS) { setFallback("submit_failed"); s->disabled = true; return 0; }
        us_submit = usSince(tSubmit0);

        const clk::time_point tSem0 = clk::now();
        if (gpuC) glInteropWaitFromVk(*s->sColorView,  GL_LAYOUT_COLOR_ATTACHMENT_EXT,         fsC.parity);
        if (gpuN) glInteropWaitFromVk(*s->sNormalView, GL_LAYOUT_COLOR_ATTACHMENT_EXT,         fsN.parity);
        if (gpuD) glInteropWaitFromVk(*s->sDepthView,  GL_LAYOUT_DEPTH_STENCIL_ATTACHMENT_EXT, fsD.parity);
        us_sem_wait = usSince(tSem0);

        const clk::time_point tFence0 = clk::now();
        if (vkWaitForFences(s->device, 1, &fsC.fence, VK_TRUE, 5ull*1000*1000*1000) != VK_SUCCESS) {
            setFallback("fence_timeout"); s->disabled = true; return 0;
        }
        us_fence_wait = usSince(tFence0);
        g_health.drawUs = usSince(tRender0);
        g_health.colorOutUs = 0.0;   // no composite in tier1

        // HARD assert: no COLOR-side readback/copy fired (tier1 must be zero-copy on
        // color AND depth — there is no expected depth-in either).
        const bool tier1ReadbackFired =
            (g_forbidden.vkCmdCopyBufferToImage != f0a) || (g_forbidden.vkCmdCopyImageToBuffer != f0b) ||
            (g_forbidden.glGetTexImage != f0c) || (g_forbidden.glTexSubImage2D != f0d);
        if (tier1ReadbackFired) {
            vlog("FATAL: a COLOR-side readback/copy fired on the tier1 co-write path — "
                 "zero-copy invariant violated; failing frame + disabling.");
            setFallback("interop_did_readback"); s->disabled = true; return 0;
        }

        g_health.vkDraws = 1;
        ++g_health.usedVulkan;
        ++g_health.usedInterop;
        g_health.totalUs = usSince(tTotal0);
        static bool s_tier1FirstOk = false;
        if (!s_tier1FirstOk) {
            vlog("first TIER-1 in-place water GBuffer co-write: thin=%u recipes=%u "
                 "(color0 premult-over blend, normal untouched (writemask 0, GL a=0 parity), "
                 "shared depth GEQUAL test+write; no composite, no depth readback).",
                 thinCount, recipeCount);
            s_tier1FirstOk = true;
        }
        if (perf) {
            std::fprintf(stderr,
                "[VK_WATER_PERF] tier1=1 thin=%u recipes=%u stage_us=%.1f record_us=%.1f "
                "submit_us=%.1f sem_wait_us=%.1f fence_wait_us=%.1f island_total_us=%.1f\n",
                thinCount, recipeCount, us_stage, us_record, us_submit, us_sem_wait,
                us_fence_wait, g_health.totalUs);
            std::fflush(stderr);
        }
        return 1;
    }
    // ========================== END RUNG 6 TIER-1 PATH ==========================

    // ---- 4) COMPOSITE OPTION (b): NO scene-color copy-in. The VK pass renders WATER
    // ONLY into the shared image (CLEAR transparent + water fragments), and GL
    // alpha-composites just the water result onto MainColor afterward. So non-water
    // pixels are never touched -> byte-identical to the GL path by construction. (The
    // sceneColorTex param is now unused for input; kept for signature stability.) ----
    (void)sceneColorTex;
    g_health.copyColorInUs = 0.0;
    us_stage = usSince(tStage0);

    // ---- 5) Import GL scene depth into the VK depth image (EXPECTED depth-in copy).
    // Water still depth-TESTS against the staged GL scene depth so shoreline occlusion
    // is correct; only water fragments that pass the test survive to the transparent
    // target. ----
    const clk::time_point tDepthIn0 = clk::now();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sceneDepthTex);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, GL_FLOAT, s->depthScratch.data());
    ++g_depthInGetTexImage; // EXPECTED (depth-side, not a color op)
    glBindTexture(GL_TEXTURE_2D, 0);
    us_depth_readback = usSince(tDepthIn0);
    const clk::time_point tDepthStage0 = clk::now();
    {
        void* p = nullptr;
        if (vmaMapMemory(s->allocator, s->depthStagingAlloc, &p) == VK_SUCCESS) {
            std::memcpy(p, s->depthScratch.data(), (size_t)w * h * 4);
            vmaUnmapMemory(s->allocator, s->depthStagingAlloc);
        }
    }
    us_depth_stage = usSince(tDepthStage0);
    g_health.copyDepthInUs = usSince(tDepthIn0);

    // ---- 6) Begin frame parity; GL hands the shared color to VK. ----
    VkFrameSync fs = vkImageResourceBeginFrame(&s->colorResource);
    VkCommandBuffer cb = s->cbuf[fs.parity & 1u];
    glInteropSignalToVk(s->colorGlView, GL_LAYOUT_COLOR_ATTACHMENT_EXT, fs.parity);

    // ---- 7) Record: depth upload (staging->image) + water render pass. ----
    const clk::time_point tRender0 = clk::now();
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) { setFallback("record_failed"); s->disabled = true; return 0; }

    auto barrier = [&](VkImage img, VkImageAspectFlags aspect, VkImageLayout oldL, VkImageLayout newL,
                       VkAccessFlags srcA, VkAccessFlags dstA, VkPipelineStageFlags srcS, VkPipelineStageFlags dstS) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = oldL; b.newLayout = newL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img; b.subresourceRange = {aspect, 0, 1, 0, 1};
        b.srcAccessMask = srcA; b.dstAccessMask = dstA;
        vkCmdPipelineBarrier(cb, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
    };

    // WAW FIX (sync-val): the shared color image is written by BOTH parity command
    // buffers (CLEAR + water draw + STORE). Across consecutive submits sync-val flags a
    // WRITE_AFTER_WRITE between frame N's color storeOp and frame N+1's first color write
    // — the GL semaphore round-trip serializes it functionally, but sync-val can't see
    // across the GL boundary, and the render-pass external deps only cover work INSIDE
    // the pass (not the cross-submit edge). A no-op layout self-barrier on the shared
    // color image (COLOR_ATTACHMENT -> COLOR_ATTACHMENT, WRITE->WRITE, COLOR_OUTPUT ->
    // COLOR_OUTPUT) at the top of the command buffer makes the ordering explicit inside
    // VK, clearing the hazard. Layout unchanged (no transition write across the GL sem).
    barrier(s->colorResource.image, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    // Depth image: UNDEFINED -> TRANSFER_DST, copy staging, -> DEPTH_ATTACHMENT (LOAD).
    barrier(s->depthImage, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy dcopy{};
    dcopy.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    dcopy.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
    vkCmdCopyBufferToImage(cb, s->depthStaging, s->depthImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &dcopy);
    ++g_depthInCopyBufferToImage; // EXPECTED (depth-side)
    barrier(s->depthImage, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);

    // CLEAR values: attachment 0 (water color) -> TRANSPARENT (0,0,0,0) so non-water
    // pixels stay transparent (composite b); 1 (gbuffer1, DONT_CARE) + 2 (depth, LOAD)
    // ignore their values but the array must be long enough to index the clear'd att.
    VkClearValue clears[3]{};
    clears[0].color = {{0.0f, 0.0f, 0.0f, 0.0f}};   // transparent water target
    clears[1].color = {{0.0f, 0.0f, 0.0f, 0.0f}};   // gbuffer1 (DONT_CARE loadOp; unused)
    clears[2].depthStencil = {0.0f, 0};             // depth (LOAD loadOp; ignored)
    VkRenderPassBeginInfo rbi{};
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = s->rpass; rbi.framebuffer = s->fb;
    rbi.renderArea = {{0, 0}, {(uint32_t)w, (uint32_t)h}};
    rbi.clearValueCount = 3; rbi.pClearValues = clears;
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->playout, 0, 1, &s->set, 0, nullptr);
    // Procedural draw: thinCount records * 6 verts/record, 1 instance.
    vkCmdDraw(cb, thinCount * 6u, 1, 0, 0);
    vkCmdEndRenderPass(cb);

    if (vkEndCommandBuffer(cb) != VK_SUCCESS) { setFallback("record_failed"); s->disabled = true; return 0; }
    us_record = usSince(tRender0);

    // ---- 8) Submit: WAIT GL->VK sem; SIGNAL VK->GL sem + fence. ----
    const bool useGpuVk2Gl = s->colorGlView.vkToGlImported && (fs.vk2gl != VK_NULL_HANDLE);
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = 1; si.pWaitSemaphores = &fs.signal; si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    if (useGpuVk2Gl) { si.signalSemaphoreCount = 1; si.pSignalSemaphores = &fs.vk2gl; }
    const clk::time_point tSubmit0 = clk::now();
    if (vkQueueSubmit(s->queue, 1, &si, fs.fence) != VK_SUCCESS) { setFallback("submit_failed"); s->disabled = true; return 0; }
    us_submit = usSince(tSubmit0);
    if (useGpuVk2Gl) {
        const clk::time_point tSem0 = clk::now();
        glInteropWaitFromVk(s->colorGlView, GL_LAYOUT_COLOR_ATTACHMENT_EXT, fs.parity);
        us_sem_wait = usSince(tSem0);
    }
    // WAW FIX (sync-val, P9a): fully retire THIS frame's VK submit before the next
    // frame records/submits, so the two parity command buffers never have overlapping
    // in-flight writes to the shared color image on the queue. This is the VK-visible
    // serialization the GL-semaphore hand-off provides functionally but sync-val cannot
    // see across the GL boundary — it closes the cross-submit WRITE_AFTER_WRITE the
    // render-pass external deps + the color self-barrier could not (those only order work
    // WITHIN a submit). Small per-frame CPU cost (one already-near-complete fence wait);
    // P9a prioritizes correctness + clean sync-val. (Future: a VK->VK timeline serialize
    // could drop this stall once the region chains natively.)
    const clk::time_point tFence0 = clk::now();
    if (vkWaitForFences(s->device, 1, &fs.fence, VK_TRUE, 5ull*1000*1000*1000) != VK_SUCCESS) {
        setFallback("fence_timeout"); s->disabled = true; return 0;
    }
    us_fence_wait = usSince(tFence0);
    g_health.drawUs = usSince(tRender0);

    // ---- 9) Composite (option b): GL alpha-blends the WATER-ONLY shared image onto
    // MainColor (the currently-bound draw FBO at water-render time). Non-water pixels
    // are transparent (a=0) -> unchanged; water pixels blend
    // MainColor = water.rgb*a + MainColor*(1-a) via SRC_ALPHA/ONE_MINUS_SRC_ALPHA,
    // reproducing the GL water over-composite. Draws a fullscreen triangle sampling the
    // shared water texture. NOT a copy-back / NOT a forbidden readback. ----
    const clk::time_point tOut0 = clk::now();
    composite_water_onto_maincolor(s, w, h);
    us_composite = usSince(tOut0);
    g_health.colorOutUs = us_composite;

    // ---- HARD assert: no COLOR-side readback/copy fired on the interop path. ----
    const bool colorReadbackFired =
        (g_forbidden.vkCmdCopyBufferToImage != f0a) || (g_forbidden.vkCmdCopyImageToBuffer != f0b) ||
        (g_forbidden.glGetTexImage != f0c) || (g_forbidden.glTexSubImage2D != f0d);
    if (colorReadbackFired) {
        vlog("FATAL: a COLOR-side readback/copy fired on the interop path — zero-copy invariant "
             "violated; failing frame + disabling.");
        setFallback("interop_did_readback"); s->disabled = true; return 0;
    }

    g_health.vkDraws = 1;
    ++g_health.usedVulkan;
    ++g_health.usedInterop;
    g_health.totalUs = usSince(tTotal0);

    // MC2_VK_WATER_PERF: per-frame sub-step breakdown. `total_us` here is the island
    // run() only; it does NOT include ReadbackGpuThinForVk (the GL thin-buffer
    // readback at the call site, reported separately as thin_readback_us).
    if (perf) {
        std::fprintf(stderr,
            "[VK_WATER_PERF] thin=%u recipes=%u stage_us=%.1f depth_readback_us=%.1f depth_stage_us=%.1f "
            "record_us=%.1f submit_us=%.1f sem_wait_us=%.1f fence_wait_us=%.1f composite_us=%.1f island_total_us=%.1f\n",
            thinCount, recipeCount, us_stage, us_depth_readback, us_depth_stage,
            us_record, us_submit, us_sem_wait, us_fence_wait, us_composite, g_health.totalUs);
        std::fflush(stderr);
    }
    return 1;
}

// Teardown (called from gosPostProcess::destroy or renderer shutdown).
extern "C" void mc2_vk_water_island_destroy() {
    vlog("[VK_WATER_INTEROP] vulkan_available=%d used_vulkan=%lu used_interop=%lu interop_active=%d "
         "vk2gl_sem=%d validation_errors=%lu thin_records=%lu recipe_count=%lu fallback_reason=\"%s\" "
         "vk_draws=%lu forbidden_copyBufToImg=%lu forbidden_copyImgToBuf=%lu forbidden_getTexImage=%lu "
         "forbidden_texSubImage=%lu depthin_getTexImage=%lu depthin_copyBufToImg=%lu "
         "copy_color_in_us=%.1f copy_depth_in_us=%.1f draw_us=%.1f color_out_us=%.1f total_us=%.1f",
         g_health.vulkanAvailable, g_health.usedVulkan, g_health.usedInterop, g_health.interopActive,
         g_health.vk2glSemActive, g_health.validationErrors, g_health.thinRecords, g_health.recipeCount,
         g_health.fallbackReason, g_health.vkDraws,
         g_forbidden.vkCmdCopyBufferToImage, g_forbidden.vkCmdCopyImageToBuffer,
         g_forbidden.glGetTexImage, g_forbidden.glTexSubImage2D,
         g_depthInGetTexImage, g_depthInCopyBufferToImage,
         g_health.copyColorInUs, g_health.copyDepthInUs, g_health.drawUs, g_health.colorOutUs, g_health.totalUs);
    if (!g_island) return;
    destroy_island(g_island);
    delete g_island; g_island = nullptr;
}

#endif // MC2_VULKAN_ISLAND
