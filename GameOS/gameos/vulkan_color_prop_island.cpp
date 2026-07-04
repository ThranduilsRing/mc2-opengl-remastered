// vulkan_color_prop_island.cpp — VK-P11 STATIC-PROP COLOR: the FIRST VK-native COLOR
// GEOMETRY region rendering REAL static-prop triangle geometry.
// =============================================================================
// VK renders the visible static-prop color instances into an exportable RGBA16F
// VkImageResource (with a VK-OWNED, non-exported D32 depth for inter-prop occlusion);
// GL alpha-composites (premult-over) that image onto MainColor at the point where the
// GL GpuStaticPropBatcher::flush() would have drawn the props. Gate ON: skip the GL
// prop color draws, run this island, GL composites. Gate OFF: GL flush() unchanged
// (byte-identical).
//
// STRUCTURE = the SHADOW island (vulkan_shadow_island_live.cpp) — the proven geometry
// region skeleton — with the depth-only output swapped for the WATER island's color
// half (exportable RGBA16F + GlInteropView + premult-over composite):
//   - Device: BORROWED from the shared engine VulkanContext (no second device).
//   - Geometry: the SAME shared static-prop VBO/IBO the shadow island copies once per
//     map (mc2_shadow_island_get_geometry_feed / Mc2ShadowGeomFeed), full 40B vertex
//     stride (pos@0/normal@12/uv@24/id@32/aRGB@36) mirrored into the VK vertex input.
//   - Instances: the per-frame VISIBLE color instances (Mc2ColorPropFeed, grouped by
//     typeID) memcpy'd into a VK instance SSBO. No fenced GL buffer crosses.
//   - Output: exportable RGBA16F color (CLEAR transparent, STORE) + VK-owned D32 depth
//     (CLEAR to 0.0 reverse-Z far, GEQUAL, write-on — matches GL StaticPropOpaque).
//   - vk2gl: VK submit SIGNALS vk2gl, GL WAITS it, then composites. WAW fence-retire.
//   - Forbidden COLOR-readback counters MUST stay 0 (zero-copy on the color surface).
//
// TIER-0 (per .claude/VK-P11-PROP-COLOR-PROBE-PLAN.md): flat/debug FS color, VK-owned
// depth (props composite OVER the scene — prop-vs-terrain occlusion APPROXIMATE, not
// pixel parity). Tier-1 (shared scene depth) is a separate slice, NOT here.
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
#include "vulkan_shadow_island_live.h"   // Mc2ShadowGeomFeed + Mc2ColorPropFeed contracts
#include "vulkan_color_prop_island.h"
#define MC2_VULKAN_SHARED_SCENE_COLOR_IMPL_ISLAND
#include "vulkan_shared_scene_color.h"   // TIER-1: fetch shared scene color/normal images
#define MC2_VULKAN_SHARED_SCENE_DEPTH_IMPL_ISLAND
#include "vulkan_shared_scene_depth.h"   // TIER-1: fetch shared scene depth (island accessors)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cstdint>
#include <vector>
#include <string>
#include <chrono>
#include <algorithm>

namespace {

// ---------------------------------------------------------------------------
// Health snapshot (mirrors the shadow island's).
// ---------------------------------------------------------------------------
struct ColorPropIslandHealth {
    int   vulkanAvailable = 0;
    int   buildEnabled    = 1;
    char  deviceName[256] = {0};
    unsigned long validationErrors = 0;
    unsigned long attempted   = 0;
    unsigned long usedVulkan  = 0;
    char  fallbackReason[256] = {0};
    unsigned long vkDraws     = 0;
    int   interopActive  = 0;
    int   vk2glSemActive = 0;
    unsigned long usedInterop = 0;
    unsigned long instRecords = 0;   // color instances fed this frame
    unsigned long typeRuns    = 0;   // per-type instanced draws this frame
    unsigned long geomVerts   = 0;   // shared VBO verts copied once
    double stageInstUs = 0.0, drawUs = 0.0, totalUs = 0.0;
};
ColorPropIslandHealth g_health;

void setFallback(const char* reason) {
    std::strncpy(g_health.fallbackReason, reason ? reason : "",
                 sizeof(g_health.fallbackReason) - 1);
    g_health.fallbackReason[sizeof(g_health.fallbackReason) - 1] = '\0';
}

// Forbidden COLOR-readback counters (zero-copy invariant on the color surface).
struct InteropForbidden {
    unsigned long vkCmdCopyImageToBuffer = 0;  // color readback   (must be 0)
    unsigned long glGetTexImage          = 0;  // color CPU pull   (must be 0)
    unsigned long glReadPixels           = 0;  // color CPU pull   (must be 0)
};
InteropForbidden g_forbidden;

void vlog(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    std::fprintf(stderr, "[VK_COLOR_PROP_ISLAND] ");
    std::vfprintf(stderr, fmt, ap);
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
    va_end(ap);
}

bool colorPropGateRequested() {
    static int state = -1;
    if (state == -1) {
        const char* e = std::getenv("MC2_VK_PROP_COLOR_INTEROP");
        state = (e && e[0] == '1') ? 1 : 0;
    }
    return state == 1;
}

// TIER-1 (parity): co-write props IN-PLACE into the SHARED scene color+normal+depth
// GBuffer (no side-image/composite). Default-OFF; requires MC2_VK_SHARED_SCENE_DEPTH
// (the shared depth Tier-1 depth-tests against). When Tier-1 is armed it REPLACES the
// Tier-0 side-image+composite path (both gate-OFF -> GL flush, byte-identical).
bool propColorTier1Requested() {
    static int state = -1;
    if (state == -1) {
        const char* e = std::getenv("MC2_VK_PROP_COLOR_TIER1");
        const char* d = std::getenv("MC2_VK_SHARED_SCENE_DEPTH");
        const bool tier1 = e && e[0] == '1' && e[1] == '\0';
        const bool depth = d && d[0] == '1' && d[1] == '\0';
        state = (tier1 && depth) ? 1 : 0;  // require shared depth
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

// GpuStaticPropInstance mirror (112B, LOCKSTEP with gos_static_prop_batcher.h AND
// shaders/vulkan/color_prop.vert). We READ modelMatrix in the VS; the full stride
// must match so the memcpy'd POD indexes correctly.
struct GpuStaticPropInstanceMirror {
    float    modelMatrix[16];  // off 0
    uint32_t typeID;           // off 64
    uint32_t firstColorOffset; // off 68
    uint32_t flags;            // off 72
    uint32_t lightDataIndex;   // off 76
    float    aRGBHighlight[4]; // off 80
    float    fogRGB[4];        // off 96
};                             // 112 bytes
static_assert(sizeof(GpuStaticPropInstanceMirror) == 112, "instance mirror must be 112B");

// std140 per-frame UBO (LOCKSTEP with color_prop.vert ColorFrameUBO).
struct ColorFrameUBO {
    float worldToClipGL[16]; // gos_GetObjectFixBMVPMat4() — GL_FALSE
    float objectDepthBias;
    int32_t instBase;
    int32_t _pad0, _pad1;
};

const VkFormat kColorFmt = VK_FORMAT_R16G16B16A16_SFLOAT; // MainColor-compatible
const VkFormat kDepthFmt = VK_FORMAT_D32_SFLOAT;          // VK-owned (non-exported)

// Per-type packet (subset of GpuStaticPropPacket the draw loop needs).
struct TypeRun {
    uint32_t typeID;
    uint32_t firstPacket;
    uint32_t packetCount;
};

struct ColorPropIsland {
    bool disabled = false;
    bool inited   = false;
    bool useInterop = false;
    bool tier1    = false;   // TIER-1 in-place shared-GBuffer co-write (vs Tier-0 side image)

    // ---- TIER-1: shared-GBuffer co-write objects (built over the SAME shared
    // color/normal/depth images the GL scene renders terrain into). ----
    VkRenderPass  t1_rpass = VK_NULL_HANDLE;   // 2 color LOAD + depth LOAD
    VkFramebuffer t1_fb    = VK_NULL_HANDLE;   // over the 3 shared image views
    VkPipeline    t1_pipe  = VK_NULL_HANDLE;   // depthWrite ON GEQUAL, 2 outputs, no blend
    VkShaderModule t1_vert = VK_NULL_HANDLE;
    VkShaderModule t1_frag = VK_NULL_HANDLE;
    // Borrowed (NOT owned) shared-image resources/views, fetched per-frame from the
    // shared-scene-color/-depth registries. Cached at init for the framebuffer build.
    struct VkImageResource* t1_colorRes  = nullptr;
    struct VkImageResource* t1_normalRes = nullptr;
    struct VkImageResource* t1_depthRes  = nullptr;
    struct GlInteropView*   t1_colorView  = nullptr;
    struct GlInteropView*   t1_normalView = nullptr;
    struct GlInteropView*   t1_depthView  = nullptr;

    int w = 0, h = 0;

    // Borrowed from the shared engine context (NOT owned).
    VkInstance       instance  = VK_NULL_HANDLE;
    VkPhysicalDevice phys      = VK_NULL_HANDLE;
    VkDevice         device    = VK_NULL_HANDLE;
    uint32_t         gfxFamily = UINT32_MAX;
    VkQueue          queue     = VK_NULL_HANDLE;
    VmaAllocator     allocator = VK_NULL_HANDLE;

    // Shared exportable RGBA16F color (GL composites it). VK-PRODUCED.
    VkImageResource colorResource{};
    GlInteropView   colorGlView{};

    // VK-owned local D32 depth (inter-prop occlusion; NOT exported).
    VkImage       depthImage = VK_NULL_HANDLE; VmaAllocation depthAlloc = VK_NULL_HANDLE;
    VkImageView   depthView  = VK_NULL_HANDLE;

    // Caster/color geometry (VK-owned copies of the shared static-prop VBO/IBO).
    VkBuffer      vbo = VK_NULL_HANDLE; VmaAllocation vboAlloc = VK_NULL_HANDLE;
    VkBuffer      ibo = VK_NULL_HANDLE; VmaAllocation iboAlloc = VK_NULL_HANDLE;
    uint32_t      vertexStride = 0;
    bool          geomUploaded = false;

    std::vector<Mc2ShadowPacket> packets;
    std::vector<TypeRun>         typeRuns;

    VkBuffer      instBuf = VK_NULL_HANDLE; VmaAllocation instAlloc = VK_NULL_HANDLE;
    VkDeviceSize  instCapBytes = 0;
    VkBuffer      ubo = VK_NULL_HANDLE; VmaAllocation uboAlloc = VK_NULL_HANDLE;

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

    // GL composite (premult-over) — reused water-island shape.
    GLuint glCompositeProg = 0;
    GLuint glCompositeVao  = 0;
    GLint  glCompositeTexLoc = -1;
};

ColorPropIsland* g_island = nullptr;

bool make_buffer(ColorPropIsland* s, VkDeviceSize size, VkBufferUsageFlags usage,
                 VkBuffer* buf, VmaAllocation* alloc, bool hostVisible) {
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size ? size : 16; bci.usage = usage;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = hostVisible ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT : 0;
    if (vmaCreateBuffer(s->allocator, &bci, &aci, buf, alloc, nullptr) != VK_SUCCESS) {
        vlog("vmaCreateBuffer failed."); return false;
    }
    return true;
}

void write_descriptors(ColorPropIsland* s) {
    if (s->set == VK_NULL_HANDLE) return;
    VkDescriptorBufferInfo uboInfo{ s->ubo, 0, VK_WHOLE_SIZE };
    VkDescriptorBufferInfo instInfo{ s->instBuf, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet w[2]{};
    w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstSet = s->set;
    w[0].dstBinding = 0; w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[0].pBufferInfo = &uboInfo;
    w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[1].dstSet = s->set;
    w[1].dstBinding = 1; w[1].descriptorCount = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[1].pBufferInfo = &instInfo;
    vkUpdateDescriptorSets(s->device, 2, w, 0, nullptr);
}

bool ensure_inst_capacity(ColorPropIsland* s, VkDeviceSize bytes) {
    if (s->instBuf != VK_NULL_HANDLE && bytes <= s->instCapBytes) return true;
    if (s->instBuf != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(s->device);
        vmaDestroyBuffer(s->allocator, s->instBuf, s->instAlloc);
        s->instBuf = VK_NULL_HANDLE;
    }
    VkDeviceSize cap = bytes + bytes / 2 + 4096;
    if (!make_buffer(s, cap, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &s->instBuf, &s->instAlloc, true))
        return false;
    s->instCapBytes = cap;
    write_descriptors(s);
    return true;
}

void destroy_island(ColorPropIsland* s) {
    if (!s) return;
    VkDevice d = s->device;
    if (d != VK_NULL_HANDLE) vkDeviceWaitIdle(d);
    // TIER-1 objects (own the render pass/framebuffer/pipeline/shaders; the shared
    // color/normal/depth images are BORROWED — owned by the postprocess registries).
    if (s->t1_pipe)  vkDestroyPipeline(d, s->t1_pipe, nullptr);
    if (s->t1_vert)  vkDestroyShaderModule(d, s->t1_vert, nullptr);
    if (s->t1_frag)  vkDestroyShaderModule(d, s->t1_frag, nullptr);
    if (s->t1_fb)    vkDestroyFramebuffer(d, s->t1_fb, nullptr);
    if (s->t1_rpass) vkDestroyRenderPass(d, s->t1_rpass, nullptr);
    // Tier-0 side-image color + local depth (only built in Tier-0 mode).
    glInteropViewDestroy(&s->colorGlView);
    if (d != VK_NULL_HANDLE) vkImageResourceDestroy(&s->colorResource, d);
    if (s->depthView)  vkDestroyImageView(d, s->depthView, nullptr);
    if (s->depthImage) vmaDestroyImage(s->allocator, s->depthImage, s->depthAlloc);
    if (s->pipe)    vkDestroyPipeline(d, s->pipe, nullptr);
    if (s->playout) vkDestroyPipelineLayout(d, s->playout, nullptr);
    if (s->vert)    vkDestroyShaderModule(d, s->vert, nullptr);
    if (s->frag)    vkDestroyShaderModule(d, s->frag, nullptr);
    if (s->dsl)     vkDestroyDescriptorSetLayout(d, s->dsl, nullptr);
    if (s->pool)    vkDestroyDescriptorPool(d, s->pool, nullptr);
    if (s->fb)      vkDestroyFramebuffer(d, s->fb, nullptr);
    if (s->rpass)   vkDestroyRenderPass(d, s->rpass, nullptr);
    if (s->vbo)     vmaDestroyBuffer(s->allocator, s->vbo, s->vboAlloc);
    if (s->ibo)     vmaDestroyBuffer(s->allocator, s->ibo, s->iboAlloc);
    if (s->instBuf) vmaDestroyBuffer(s->allocator, s->instBuf, s->instAlloc);
    if (s->ubo)     vmaDestroyBuffer(s->allocator, s->ubo, s->uboAlloc);
    if (s->cpool)   vkDestroyCommandPool(d, s->cpool, nullptr);
    if (s->glCompositeProg) { glDeleteProgram(s->glCompositeProg); s->glCompositeProg = 0; }
    if (s->glCompositeVao)  { glDeleteVertexArrays(1, &s->glCompositeVao); s->glCompositeVao = 0; }
    // instance/phys/device/queue/allocator are BORROWED — never destroyed here.
}

bool make_depth(ColorPropIsland* s) {
    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D; ii.format = kDepthFmt;
    ii.extent = {(uint32_t)s->w, (uint32_t)s->h, 1};
    ii.mipLevels = 1; ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    if (vmaCreateImage(s->allocator, &ii, &aci, &s->depthImage, &s->depthAlloc, nullptr) != VK_SUCCESS) {
        vlog("vmaCreateImage(depth) failed."); return false;
    }
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = s->depthImage; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = kDepthFmt;
    vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(s->device, &vi, nullptr, &s->depthView) != VK_SUCCESS) {
        vlog("depth view failed."); return false;
    }
    return true;
}

// One-time init.
bool init_island(ColorPropIsland* s, int w, int h) {
    s->w = w; s->h = h;
    ++g_health.attempted;

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

    // Tier-1 requires EITHER the Tier-0 gate OR the Tier-1 gate to be armed. Tier-1
    // takes precedence (it REPLACES Tier-0's side-image+composite path).
    s->tier1 = propColorTier1Requested();
    if (!s->tier1 && !colorPropGateRequested()) { setFallback("gate_off"); return false; }

    if (s->tier1) {
        // ---- TIER-1: fetch the SHARED scene color/normal/depth exportable images the
        // GL scene renders terrain into (built by the postprocess seams; the island
        // co-writes props into these SAME images in-place). Fail-soft: any missing =>
        // GL fallback (the caller runs the GL prop flush). ----
        s->t1_colorRes  = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_COLOR0);
        s->t1_normalRes = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_NORMAL);
        s->t1_depthRes  = mc2SharedSceneDepthGetResource();
        s->t1_colorView  = mc2SharedSceneColorGetGlView(MC2_SHARED_SCENE_COLOR0);
        s->t1_normalView = mc2SharedSceneColorGetGlView(MC2_SHARED_SCENE_NORMAL);
        s->t1_depthView  = mc2SharedSceneDepthGetGlView();
        if (!s->t1_colorRes || !s->t1_normalRes || !s->t1_depthRes ||
            !s->t1_colorView || !s->t1_normalView || !s->t1_depthView) {
            setFallback("tier1_shared_gbuffer_not_ready");
            return false;
        }
        s->useInterop = true;
        g_health.interopActive = 1;
        // vk2gl active if the shared color view imported its VK->GL semaphore.
        g_health.vk2glSemActive =
            reinterpret_cast<GlInteropView*>(s->t1_colorView)->vkToGlImported ? 1 : 0;
        vlog("TIER-1 shared-GBuffer co-write ACTIVE — in-place RGBA16F color(MRT0)+normal(MRT1) "
             "+ shared D32S8 depth (vk2gl_sem=%d).", g_health.vk2glSemActive);
    } else {
        // ---- TIER-0: Exportable shared RGBA16F color (GL composites it). ----
        bool ok = vkImageResourceCreate(&s->colorResource, s->device, s->phys,
                                        (uint32_t)w, (uint32_t)h, kColorFmt,
                                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                        VK_IMAGE_USAGE_SAMPLED_BIT,
                                        /*exportable*/ true);
        if (ok) ok = glInteropViewCreate(&s->colorGlView, &s->colorResource, s->phys, GL_RGBA16F);
        if (!ok) {
            glInteropViewDestroy(&s->colorGlView);
            vkImageResourceDestroy(&s->colorResource, s->device);
            setFallback("interop_setup_failed");
            return false; // interop-only; no readback fallback path
        }
        s->useInterop = true;
        g_health.interopActive = 1;
        g_health.vk2glSemActive = s->colorGlView.vkToGlImported ? 1 : 0;
        vlog("color interop seam ACTIVE — zero-copy exportable RGBA16F prop color (vk2gl_sem=%d).",
             g_health.vk2glSemActive);

        // ---- VK-owned local D32 depth (inter-prop occlusion). ----
        if (!make_depth(s)) { setFallback("depth_setup_failed"); return false; }
    }

    // ---- Instance SSBO + UBO (common to both tiers). ----
    if (!ensure_inst_capacity(s, 256 * 1024)) return false;
    if (!make_buffer(s, sizeof(ColorFrameUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &s->ubo, &s->uboAlloc, true)) return false;

    // ---- TIER-0 Render pass: color0 (shared, CLEAR transparent, STORE, layout-stable
    // COLOR_ATTACHMENT so no transition write across the GL semaphore) + depth
    // (VK-owned, CLEAR to 0.0 reverse-Z far, DONT_CARE store). ----
    if (!s->tier1) {
        VkAttachmentDescription att[2]{};
        att[0].format = kColorFmt; att[0].samples = VK_SAMPLE_COUNT_1_BIT;
        att[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att[0].finalLayout   = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att[1].format = kDepthFmt; att[1].samples = VK_SAMPLE_COUNT_1_BIT;
        att[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; att[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        att[1].finalLayout   = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1; sub.pColorAttachments = &colorRef;
        sub.pDepthStencilAttachment = &depthRef;

        // Cross-frame WAW: the shared color image is written (CLEAR + prop draws +
        // STORE) every frame across two parity command buffers. deps[0] orders THIS
        // frame's writes AFTER any prior write; deps[1] retires the store before the
        // vk2gl signal hands the image to GL. (Same shape as the shadow island deps.)
        VkSubpassDependency deps[2]{};
        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL; deps[0].dstSubpass = 0;
        deps[0].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        deps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[1].srcSubpass = 0; deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstStageMask  = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        deps[1].dstAccessMask = 0;

        VkRenderPassCreateInfo rpci{};
        rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpci.attachmentCount = 2; rpci.pAttachments = att;
        rpci.subpassCount = 1; rpci.pSubpasses = &sub;
        rpci.dependencyCount = 2; rpci.pDependencies = deps;
        if (vkCreateRenderPass(s->device, &rpci, nullptr, &s->rpass) != VK_SUCCESS) {
            vlog("vkCreateRenderPass failed."); return false;
        }
        VkImageView fbViews[2] = { s->colorResource.view, s->depthView };
        VkFramebufferCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = s->rpass; fci.attachmentCount = 2; fci.pAttachments = fbViews;
        fci.width = (uint32_t)w; fci.height = (uint32_t)h; fci.layers = 1;
        if (vkCreateFramebuffer(s->device, &fci, nullptr, &s->fb) != VK_SUCCESS) {
            vlog("vkCreateFramebuffer failed."); return false;
        }
    }

    // ---- TIER-1 Render pass: 2 color LOAD (preserve GL terrain in the SHARED
    // color(MRT0)+normal(MRT1)) + shared depth LOAD (D32S8, depth-TEST GEQUAL,
    // write-on to match GL StaticPropOpaque). NO clear of any attachment (respects
    // the GL scene render + clearGBuffer1 that already ran). GL handed all three over
    // in their *_ATTACHMENT layouts; keep them there so GL resumes cleanly. ----
    if (s->tier1) {
        VkImageResource* colorRes  = reinterpret_cast<VkImageResource*>(s->t1_colorRes);
        VkImageResource* normalRes = reinterpret_cast<VkImageResource*>(s->t1_normalRes);
        VkImageResource* depthRes  = reinterpret_cast<VkImageResource*>(s->t1_depthRes);

        VkAttachmentDescription att[3]{};
        // color0 (MRT0) — LOAD GL terrain color, STORE result.
        att[0].format = colorRes->format; att[0].samples = VK_SAMPLE_COUNT_1_BIT;
        att[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att[0].finalLayout   = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        // color1 (MRT1 GBuffer1) — LOAD GL terrain normals (respect clearGBuffer1), STORE.
        att[1].format = normalRes->format; att[1].samples = VK_SAMPLE_COUNT_1_BIT;
        att[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[1].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att[1].finalLayout   = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        // depth (shared D32S8) — LOAD GL scene depth, TEST+WRITE (GEQUAL), STORE so GL
        // (and post passes) see the prop-updated depth. Stencil LOAD/STORE preserved.
        att[2].format = depthRes->format; att[2].samples = VK_SAMPLE_COUNT_1_BIT;
        att[2].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[2].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        att[2].finalLayout   = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentReference colorRefs[2] = {
            {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
            {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        };
        VkAttachmentReference depthRef{2, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 2; sub.pColorAttachments = colorRefs;
        sub.pDepthStencilAttachment = &depthRef;

        // Same cross-submit WAW/hand-off deps shape as Tier-0 (GL semaphore provides
        // the actual ordering; these let sync-val see the LOAD-preserve co-write).
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
        deps[1].dstStageMask  = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        deps[1].dstAccessMask = 0;

        VkRenderPassCreateInfo rpci{};
        rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpci.attachmentCount = 3; rpci.pAttachments = att;
        rpci.subpassCount = 1; rpci.pSubpasses = &sub;
        rpci.dependencyCount = 2; rpci.pDependencies = deps;
        if (vkCreateRenderPass(s->device, &rpci, nullptr, &s->t1_rpass) != VK_SUCCESS) {
            vlog("TIER-1 vkCreateRenderPass failed."); setFallback("tier1_renderpass_failed"); return false;
        }
        VkImageView fbViews[3] = { colorRes->view, normalRes->view, depthRes->view };
        VkFramebufferCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = s->t1_rpass; fci.attachmentCount = 3; fci.pAttachments = fbViews;
        fci.width = (uint32_t)w; fci.height = (uint32_t)h; fci.layers = 1;
        if (vkCreateFramebuffer(s->device, &fci, nullptr, &s->t1_fb) != VK_SUCCESS) {
            vlog("TIER-1 vkCreateFramebuffer failed."); setFallback("tier1_framebuffer_failed"); return false;
        }
    }

    // ---- Descriptor layout/pool/set (UBO binding0 VS, instance SSBO binding1 VS). ----
    {
        VkDescriptorSetLayoutBinding b[2]{};
        b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.bindingCount = 2; lci.pBindings = b;
        if (vkCreateDescriptorSetLayout(s->device, &lci, nullptr, &s->dsl) != VK_SUCCESS) {
            vlog("vkCreateDescriptorSetLayout failed."); return false;
        }
        VkDescriptorPoolSize sizes[2]{};
        sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; sizes[0].descriptorCount = 1;
        sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; sizes[1].descriptorCount = 1;
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

    // ---- TIER-0 Pipeline: full 40B vertex stream (pos/normal/uv/id/aRGB), instanced,
    // reverse-Z GEQUAL depth-test+write, cull NONE, premult-over color blend. ----
    if (!s->tier1) {
        s->vert = load_spv(s->device, spv_dir() + "color_prop.vert.spv");
        s->frag = load_spv(s->device, spv_dir() + "color_prop.frag.spv");
        if (s->vert == VK_NULL_HANDLE || s->frag == VK_NULL_HANDLE) { setFallback("spv_load_failed"); return false; }

        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 1; plci.pSetLayouts = &s->dsl;
        if (vkCreatePipelineLayout(s->device, &plci, nullptr, &s->playout) != VK_SUCCESS) {
            vlog("vkCreatePipelineLayout failed."); return false;
        }

        // Mirror the GL static-prop VAO layout (kVertexStride=40): loc0 pos vec3 @0,
        // loc1 normal vec3 @12, loc2 uv vec2 @24, loc3 uint @32, loc4 uint @36.
        VkVertexInputBindingDescription vib{};
        vib.binding = 0; vib.stride = s->vertexStride ? s->vertexStride : 40;
        vib.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        VkVertexInputAttributeDescription via[5]{};
        via[0].location = 0; via[0].binding = 0; via[0].format = VK_FORMAT_R32G32B32_SFLOAT; via[0].offset = 0;
        via[1].location = 1; via[1].binding = 0; via[1].format = VK_FORMAT_R32G32B32_SFLOAT; via[1].offset = 12;
        via[2].location = 2; via[2].binding = 0; via[2].format = VK_FORMAT_R32G32_SFLOAT;    via[2].offset = 24;
        via[3].location = 3; via[3].binding = 0; via[3].format = VK_FORMAT_R32_UINT;         via[3].offset = 32;
        via[4].location = 4; via[4].binding = 0; via[4].format = VK_FORMAT_R32_UINT;         via[4].offset = 36;
        VkPipelineVertexInputStateCreateInfo vin{};
        vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vin.vertexBindingDescriptionCount = 1; vin.pVertexBindingDescriptions = &vib;
        vin.vertexAttributeDescriptionCount = 5; vin.pVertexAttributeDescriptions = via;

        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo vps{};
        vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vps.viewportCount = 1; vps.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;   // GL prop color pass does not hard-cull
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        // Reverse-Z GEQUAL, test+write on (matches GL StaticPropOpaque; clear depth 0.0).
        VkPipelineDepthStencilStateCreateInfo ds{};
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        ds.depthTestEnable = VK_TRUE;
        ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;

        // Premult-over: FS emits (rgb*a, a); ONE / ONE_MINUS_SRC_ALPHA into the
        // transparent-cleared target -> GL composites premult-over identically.
        VkPipelineColorBlendAttachmentState cba{};
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
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

        VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dsci{};
        dsci.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dsci.dynamicStateCount = 2; dsci.pDynamicStates = dyn;

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
        gp.pDynamicState = &dsci;
        gp.layout = s->playout; gp.renderPass = s->rpass; gp.subpass = 0;
        if (vkCreateGraphicsPipelines(s->device, VK_NULL_HANDLE, 1, &gp, nullptr, &s->pipe) != VK_SUCCESS) {
            vlog("vkCreateGraphicsPipelines failed."); return false;
        }
    }

    // ---- TIER-1 Pipeline: same 40B vertex stream, but emits TWO GBuffer outputs
    // (loc0 lit color, loc1 GBuffer1 normal+0.25), reverse-Z GEQUAL depth-test+WRITE,
    // cull NONE, and NO blend on either attachment (opaque in-place overwrite — a
    // winning prop fragment fully replaces the terrain GBuffer pixel, matching GL). ----
    if (s->tier1) {
        s->t1_vert = load_spv(s->device, spv_dir() + "color_prop_tier1.vert.spv");
        s->t1_frag = load_spv(s->device, spv_dir() + "color_prop_tier1.frag.spv");
        if (s->t1_vert == VK_NULL_HANDLE || s->t1_frag == VK_NULL_HANDLE) { setFallback("tier1_spv_load_failed"); return false; }

        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 1; plci.pSetLayouts = &s->dsl;
        if (vkCreatePipelineLayout(s->device, &plci, nullptr, &s->playout) != VK_SUCCESS) {
            vlog("TIER-1 vkCreatePipelineLayout failed."); return false;
        }

        VkVertexInputBindingDescription vib{};
        vib.binding = 0; vib.stride = s->vertexStride ? s->vertexStride : 40;
        vib.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        VkVertexInputAttributeDescription via[5]{};
        via[0].location = 0; via[0].binding = 0; via[0].format = VK_FORMAT_R32G32B32_SFLOAT; via[0].offset = 0;
        via[1].location = 1; via[1].binding = 0; via[1].format = VK_FORMAT_R32G32B32_SFLOAT; via[1].offset = 12;
        via[2].location = 2; via[2].binding = 0; via[2].format = VK_FORMAT_R32G32_SFLOAT;    via[2].offset = 24;
        via[3].location = 3; via[3].binding = 0; via[3].format = VK_FORMAT_R32_UINT;         via[3].offset = 32;
        via[4].location = 4; via[4].binding = 0; via[4].format = VK_FORMAT_R32_UINT;         via[4].offset = 36;
        VkPipelineVertexInputStateCreateInfo vin{};
        vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vin.vertexBindingDescriptionCount = 1; vin.pVertexBindingDescriptions = &vib;
        vin.vertexAttributeDescriptionCount = 5; vin.pVertexAttributeDescriptions = via;

        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo vps{};
        vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vps.viewportCount = 1; vps.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        // Reverse-Z GEQUAL, test+WRITE on (matches GL StaticPropOpaque; props occlude
        // vs the SHARED scene depth GL authored — the point of Tier-1).
        VkPipelineDepthStencilStateCreateInfo ds{};
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        ds.depthTestEnable = VK_TRUE;
        ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;

        // NO blend on EITHER attachment: opaque in-place overwrite. GBuffer1 is NOT
        // alpha-blendable (premult-over would corrupt the normal+class encoding), and
        // the color pass is opaque, so a winning fragment writes rgba directly.
        VkPipelineColorBlendAttachmentState cba[2]{};
        for (int k = 0; k < 2; ++k) {
            cba[k].blendEnable = VK_FALSE;
            cba[k].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        }
        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 2; cb.pAttachments = cba;

        VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dsci{};
        dsci.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dsci.dynamicStateCount = 2; dsci.pDynamicStates = dyn;

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = s->t1_vert; stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = s->t1_frag; stages[1].pName = "main";
        VkGraphicsPipelineCreateInfo gp{};
        gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gp.stageCount = 2; gp.pStages = stages;
        gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vps; gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms; gp.pDepthStencilState = &ds; gp.pColorBlendState = &cb;
        gp.pDynamicState = &dsci;
        gp.layout = s->playout; gp.renderPass = s->t1_rpass; gp.subpass = 0;
        if (vkCreateGraphicsPipelines(s->device, VK_NULL_HANDLE, 1, &gp, nullptr, &s->t1_pipe) != VK_SUCCESS) {
            vlog("TIER-1 vkCreateGraphicsPipelines failed."); setFallback("tier1_pipeline_failed"); return false;
        }
    }

    // ---- Command pool + 2 command buffers (per parity). ----
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
    }

    s->inited = true;
    setFallback("");
    vlog("island initialized %dx%d (borrowed shared device; exportable RGBA16F prop color, "
         "VK-owned D32 depth GEQUAL reverse-Z; instanced static-prop color draws).", w, h);
    return true;
}

// Copy the shared static-prop VBO/IBO into VK-owned buffers ONCE per map, reusing the
// shadow island's geometry feed (SAME shared buffers). Returns false if unavailable.
bool upload_geometry_once(ColorPropIsland* s) {
    if (s->geomUploaded) return true;
    Mc2ShadowGeomFeed feed{};
    if (!mc2_shadow_island_get_geometry_feed(&feed)) return false;
    if (feed.vboBytes == 0 || feed.iboBytes == 0 || feed.glVbo == 0 || feed.glIbo == 0) return false;

    s->vertexStride = feed.vertexStride;

    if (!make_buffer(s, feed.vboBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &s->vbo, &s->vboAlloc, true)) return false;
    if (!make_buffer(s, feed.iboBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,  &s->ibo, &s->iboAlloc, true)) return false;
    std::vector<uint8_t> scratch;
    { void* p = nullptr;
      if (vmaMapMemory(s->allocator, s->vboAlloc, &p) == VK_SUCCESS) {
          scratch.resize((size_t)feed.vboBytes);
          glBindBuffer(GL_ARRAY_BUFFER, feed.glVbo);
          glGetBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)feed.vboBytes, scratch.data());
          glBindBuffer(GL_ARRAY_BUFFER, 0);
          std::memcpy(p, scratch.data(), (size_t)feed.vboBytes);
          vmaUnmapMemory(s->allocator, s->vboAlloc);
      } }
    { void* p = nullptr;
      if (vmaMapMemory(s->allocator, s->iboAlloc, &p) == VK_SUCCESS) {
          scratch.resize((size_t)feed.iboBytes);
          glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, feed.glIbo);
          glGetBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, (GLsizeiptr)feed.iboBytes, scratch.data());
          glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
          std::memcpy(p, scratch.data(), (size_t)feed.iboBytes);
          vmaUnmapMemory(s->allocator, s->iboAlloc);
      } }

    s->packets.assign(feed.packets, feed.packets + feed.packetCount);
    s->typeRuns.clear();
    s->typeRuns.resize(feed.typeCount);
    for (uint32_t t = 0; t < feed.typeCount; ++t) {
        s->typeRuns[t].typeID      = t;
        s->typeRuns[t].firstPacket = feed.typeFirstPacket[t];
        s->typeRuns[t].packetCount = feed.typePacketCount[t];
    }
    g_health.geomVerts = (unsigned long)(feed.vboBytes / (feed.vertexStride ? feed.vertexStride : 1));
    s->geomUploaded = true;
    vlog("prop geometry uploaded once: vbo=%llu B ibo=%llu B stride=%u packets=%u types=%u.",
         (unsigned long long)feed.vboBytes, (unsigned long long)feed.iboBytes,
         feed.vertexStride, feed.packetCount, feed.typeCount);
    return true;
}

// GL composite (premult-over): alpha-blend the shared prop-color texture onto the
// currently-bound draw FBO (MainColor at flush() time). Reused water-island shape.
void composite_props_onto_maincolor(ColorPropIsland* s, int w, int h) {
    if (s->colorGlView.glTex == 0) return;
    if (s->glCompositeProg == 0) {
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
            "uniform sampler2D uProp;\n"
            "void main(){ o = texture(uProp, vUv); }\n";
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
            if(ok){ s->glCompositeProg = prog; s->glCompositeTexLoc = glGetUniformLocation(prog,"uProp"); }
            else { char log[512]; glGetProgramInfoLog(prog,512,nullptr,log);
                vlog("composite link fail: %s", log); glDeleteProgram(prog); }
        }
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        if (s->glCompositeVao == 0) glGenVertexArrays(1, &s->glCompositeVao);
    }
    if (s->glCompositeProg == 0 || s->glCompositeVao == 0) return;

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
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    // Premult-over: shared prop texture holds (rgb*a, a); dst = rgb + dst*(1-a).
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s->colorGlView.glTex);
    glBindSampler(0, 0);
    if (s->glCompositeTexLoc >= 0) glUniform1i(s->glCompositeTexLoc, 0);
    glDrawArrays(GL_TRIANGLES, 0, 3);

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
// PUBLIC ENTRY POINTS.
// ===========================================================================

extern "C" int mc2_vk_color_prop_island_enabled() {
    // Either tier armed => the caller skips the GL prop color flush and calls run().
    return (colorPropGateRequested() || propColorTier1Requested()) ? 1 : 0;
}

extern "C" int mc2_vk_color_prop_island_run(int w, int h) {
    if (!colorPropGateRequested() && !propColorTier1Requested()) return 0;
    if (w <= 0 || h <= 0) return 0;

    using clk = std::chrono::steady_clock;
    auto usSince = [](clk::time_point t0){ return std::chrono::duration<double, std::micro>(clk::now() - t0).count(); };
    const clk::time_point tTotal0 = clk::now();

    // Pull the per-frame color instance feed (visible buckets + world->clip + bias).
    Mc2ColorPropFeed feed{};
    if (!mc2_color_prop_island_get_frame_feed(&feed)) { setFallback("no_color_feed"); return 0; }
    if (!feed.instances112 || feed.count == 0) { setFallback("no_color_instances"); return 0; }

    // Lazy init / rebuild on size change.
    if (g_island && (g_island->w != w || g_island->h != h)) {
        destroy_island(g_island); delete g_island; g_island = nullptr;
    }
    if (!g_island) {
        g_island = new ColorPropIsland();
        if (!init_island(g_island, w, h)) {
            vlog("init failed (%s) — GL fallback.", g_health.fallbackReason);
            destroy_island(g_island); delete g_island; g_island = nullptr;
            return 0;
        }
    }
    ColorPropIsland* s = g_island;
    if (s->disabled || !s->inited || !s->useInterop) return 0;

    if (!upload_geometry_once(s)) { setFallback("geometry_not_ready"); return 0; }

    const unsigned long f0a = g_forbidden.vkCmdCopyImageToBuffer;
    const unsigned long f0b = g_forbidden.glGetTexImage;
    const unsigned long f0c = g_forbidden.glReadPixels;

    const clk::time_point tStage0 = clk::now();
    // ---- 1) Upload the per-frame color instances (already grouped by typeID). ----
    const uint32_t instanceCount = feed.count;
    const VkDeviceSize instBytes = (VkDeviceSize)instanceCount * sizeof(GpuStaticPropInstanceMirror);
    if (!ensure_inst_capacity(s, instBytes)) { s->disabled = true; return 0; }
    {
        void* p = nullptr;
        if (vmaMapMemory(s->allocator, s->instAlloc, &p) == VK_SUCCESS) {
            std::memcpy(p, feed.instances112, (size_t)instBytes);
            vmaUnmapMemory(s->allocator, s->instAlloc);
        }
    }
    g_health.instRecords = instanceCount;

    // ---- 2) Upload the per-frame UBO (worldToClipGL + bias; instBase=0). ----
    {
        ColorFrameUBO u{};
        std::memcpy(u.worldToClipGL, feed.worldToClipGL, sizeof(u.worldToClipGL));
        u.objectDepthBias = feed.objectDepthBias;
        u.instBase = 0;
        void* p = nullptr;
        if (vmaMapMemory(s->allocator, s->uboAlloc, &p) == VK_SUCCESS) {
            std::memcpy(p, &u, sizeof(u)); vmaUnmapMemory(s->allocator, s->uboAlloc);
        }
    }
    g_health.stageInstUs = usSince(tStage0);

    // The feed is already grouped by ascending typeID; build contiguous type-runs.
    const GpuStaticPropInstanceMirror* insts =
        reinterpret_cast<const GpuStaticPropInstanceMirror*>(feed.instances112);

    // ===================================================================
    // TIER-1 co-write: props rendered IN-PLACE into the SHARED scene color(MRT0)+
    // normal(MRT1)+depth GBuffer. GL authored terrain into all three and handed them
    // to VK (each in its *_ATTACHMENT layout); VK LOADs (does NOT clear) all three,
    // depth-tests GEQUAL reverse-Z, and only overwrites where the prop wins. No
    // side-image, no composite, NO re-clear of GBuffer1 (respects clearGBuffer1()).
    // ===================================================================
    if (s->tier1) {
        VkImageResource* colorRes  = reinterpret_cast<VkImageResource*>(s->t1_colorRes);
        VkImageResource* normalRes = reinterpret_cast<VkImageResource*>(s->t1_normalRes);
        VkImageResource* depthRes  = reinterpret_cast<VkImageResource*>(s->t1_depthRes);
        GlInteropView*   colorView  = reinterpret_cast<GlInteropView*>(s->t1_colorView);
        GlInteropView*   normalView = reinterpret_cast<GlInteropView*>(s->t1_normalView);
        GlInteropView*   depthView  = reinterpret_cast<GlInteropView*>(s->t1_depthView);

        // Begin a frame on EACH shared image (waits+resets its parity fence so its
        // binary semaphores are safe to re-signal). Drive all three on the color's
        // parity for a single coherent hand-off.
        VkFrameSync fsC = vkImageResourceBeginFrame(colorRes);
        VkFrameSync fsN = vkImageResourceBeginFrame(normalRes);
        VkFrameSync fsD = vkImageResourceBeginFrame(depthRes);
        VkCommandBuffer cb = s->cbuf[fsC.parity & 1u];

        // GL hands each shared image to VK in its attachment layout.
        glInteropSignalToVk(*colorView,  GL_LAYOUT_COLOR_ATTACHMENT_EXT,          fsC.parity);
        glInteropSignalToVk(*normalView, GL_LAYOUT_COLOR_ATTACHMENT_EXT,          fsN.parity);
        glInteropSignalToVk(*depthView,  GL_LAYOUT_DEPTH_STENCIL_ATTACHMENT_EXT,  fsD.parity);

        const clk::time_point tRender0 = clk::now();
        vkResetCommandBuffer(cb, 0);
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) { setFallback("record_failed"); s->disabled = true; return 0; }

        // Begin the LOAD renderpass — NO clear values (all attachments loadOp=LOAD).
        VkRenderPassBeginInfo rbi{};
        rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rbi.renderPass = s->t1_rpass; rbi.framebuffer = s->t1_fb;
        rbi.renderArea = {{0, 0}, {(uint32_t)w, (uint32_t)h}};
        rbi.clearValueCount = 0; rbi.pClearValues = nullptr;
        vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport vp{0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f};
        VkRect2D scissor{{0, 0}, {(uint32_t)w, (uint32_t)h}};
        vkCmdSetViewport(cb, 0, 1, &vp);
        vkCmdSetScissor(cb, 0, 1, &scissor);

        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->t1_pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->playout, 0, 1, &s->set, 0, nullptr);
        VkDeviceSize vboOff = 0;
        vkCmdBindVertexBuffers(cb, 0, 1, &s->vbo, &vboOff);
        vkCmdBindIndexBuffer(cb, s->ibo, 0, VK_INDEX_TYPE_UINT32);

        unsigned long typeRuns = 0;
        uint32_t i = 0;
        while (i < instanceCount) {
            const uint32_t tid = insts[i].typeID;
            uint32_t j = i;
            while (j < instanceCount && insts[j].typeID == tid) ++j;
            const uint32_t instCount = j - i;
            if (tid < s->typeRuns.size()) {
                const TypeRun& tr = s->typeRuns[tid];
                for (uint32_t p = 0; p < tr.packetCount; ++p) {
                    const uint32_t pk = tr.firstPacket + p;
                    if (pk >= s->packets.size()) break;
                    const Mc2ShadowPacket& pkt = s->packets[pk];
                    vkCmdDrawIndexed(cb, pkt.indexCount, instCount,
                                     pkt.firstIndex, (int32_t)pkt.baseVertex, i);
                }
                ++typeRuns;
            }
            i = j;
        }
        g_health.typeRuns = typeRuns;

        vkCmdEndRenderPass(cb);
        if (vkEndCommandBuffer(cb) != VK_SUCCESS) { setFallback("record_failed"); s->disabled = true; return 0; }

        // Submit: WAIT all three GL->VK sems (GL owns all three targets); SIGNAL all
        // three VK->GL sems + the color fence. GL then waits all three back.
        VkSemaphore waitSems[3];   VkPipelineStageFlags waitStages[3];
        VkSemaphore signalSems[3]; uint32_t waitN = 0, sigN = 0;
        auto addWait = [&](VkSemaphore sem, VkPipelineStageFlags stage){
            if (sem != VK_NULL_HANDLE) { waitSems[waitN] = sem; waitStages[waitN] = stage; ++waitN; } };
        addWait(fsC.signal, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        addWait(fsN.signal, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        addWait(fsD.signal, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);
        const bool gpuC = colorView->vkToGlImported  && fsC.vk2gl != VK_NULL_HANDLE;
        const bool gpuN = normalView->vkToGlImported && fsN.vk2gl != VK_NULL_HANDLE;
        const bool gpuD = depthView->vkToGlImported  && fsD.vk2gl != VK_NULL_HANDLE;
        if (gpuC) signalSems[sigN++] = fsC.vk2gl;
        if (gpuN) signalSems[sigN++] = fsN.vk2gl;
        if (gpuD) signalSems[sigN++] = fsD.vk2gl;

        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.waitSemaphoreCount = waitN; si.pWaitSemaphores = waitSems; si.pWaitDstStageMask = waitStages;
        si.commandBufferCount = 1; si.pCommandBuffers = &cb;
        si.signalSemaphoreCount = sigN; si.pSignalSemaphores = signalSems;
        if (vkQueueSubmit(s->queue, 1, &si, fsC.fence) != VK_SUCCESS) { setFallback("submit_failed"); s->disabled = true; return 0; }

        // GL waits each shared image back in its attachment layout (GPU hand-off).
        if (gpuC) glInteropWaitFromVk(*colorView,  GL_LAYOUT_COLOR_ATTACHMENT_EXT,         fsC.parity);
        if (gpuN) glInteropWaitFromVk(*normalView, GL_LAYOUT_COLOR_ATTACHMENT_EXT,         fsN.parity);
        if (gpuD) glInteropWaitFromVk(*depthView,  GL_LAYOUT_DEPTH_STENCIL_ATTACHMENT_EXT, fsD.parity);

        // Retire this frame's submit before the parity slot is reused (WAW).
        if (vkWaitForFences(s->device, 1, &fsC.fence, VK_TRUE, 5ull*1000*1000*1000) != VK_SUCCESS) {
            setFallback("fence_timeout"); s->disabled = true; return 0;
        }
        g_health.drawUs = usSince(tRender0);

        // NO composite — props were co-written in-place into the real GBuffer.

        const bool readbackFired =
            (g_forbidden.vkCmdCopyImageToBuffer != f0a) ||
            (g_forbidden.glGetTexImage != f0b) || (g_forbidden.glReadPixels != f0c);
        if (readbackFired) {
            vlog("FATAL(tier1): a readback fired on the co-write path — failing frame + disabling.");
            setFallback("interop_did_readback"); s->disabled = true; return 0;
        }

        g_health.vkDraws = 1;
        ++g_health.usedVulkan;
        ++g_health.usedInterop;
        g_health.totalUs = usSince(tTotal0);
        static bool s_t1FirstOk = false;
        if (!s_t1FirstOk) {
            vlog("first TIER-1 in-place GBuffer co-write: instCount=%u typeRuns=%lu "
                 "(props co-written into shared color(MRT0)+normal(MRT1)+depth; no composite).",
                 instanceCount, g_health.typeRuns);
            s_t1FirstOk = true;
        }
        return 1;
    }

    // ---- 3) Begin frame parity; GL hands the shared color to VK (this is a
    // GL-owns-target -> VK-writes case like the water island, so signal GL->VK). ----
    VkFrameSync fs = vkImageResourceBeginFrame(&s->colorResource);
    VkCommandBuffer cb = s->cbuf[fs.parity & 1u];
    glInteropSignalToVk(s->colorGlView, GL_LAYOUT_COLOR_ATTACHMENT_EXT, fs.parity);

    // ---- 4) Record: color+depth render pass, per-type instanced prop draws. ----
    const clk::time_point tRender0 = clk::now();
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) { setFallback("record_failed"); s->disabled = true; return 0; }

    // WAW self-barrier on the shared color image (COLOR_ATTACHMENT -> COLOR_ATTACHMENT)
    // so sync-val can see the cross-submit ordering the GL semaphore provides.
    {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = s->colorResource.image; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    }

    VkClearValue clears[2]{};
    clears[0].color = {{0.0f, 0.0f, 0.0f, 0.0f}};   // transparent (composite over)
    clears[1].depthStencil = {0.0f, 0};             // reverse-Z far
    VkRenderPassBeginInfo rbi{};
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = s->rpass; rbi.framebuffer = s->fb;
    rbi.renderArea = {{0, 0}, {(uint32_t)w, (uint32_t)h}};
    rbi.clearValueCount = 2; rbi.pClearValues = clears;
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {(uint32_t)w, (uint32_t)h}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &scissor);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->playout, 0, 1, &s->set, 0, nullptr);
    VkDeviceSize vboOff = 0;
    vkCmdBindVertexBuffers(cb, 0, 1, &s->vbo, &vboOff);
    vkCmdBindIndexBuffer(cb, s->ibo, 0, VK_INDEX_TYPE_UINT32);

    unsigned long typeRuns = 0;
    uint32_t i = 0;
    while (i < instanceCount) {
        const uint32_t tid = insts[i].typeID;
        uint32_t j = i;
        while (j < instanceCount && insts[j].typeID == tid) ++j;
        const uint32_t instCount = j - i;
        if (tid < s->typeRuns.size()) {
            const TypeRun& tr = s->typeRuns[tid];
            for (uint32_t p = 0; p < tr.packetCount; ++p) {
                const uint32_t pk = tr.firstPacket + p;
                if (pk >= s->packets.size()) break;
                const Mc2ShadowPacket& pkt = s->packets[pk];
                vkCmdDrawIndexed(cb, pkt.indexCount, instCount,
                                 pkt.firstIndex, (int32_t)pkt.baseVertex,
                                 /*firstInstance*/ i);
            }
            ++typeRuns;
        }
        i = j;
    }
    g_health.typeRuns = typeRuns;

    vkCmdEndRenderPass(cb);
    if (vkEndCommandBuffer(cb) != VK_SUCCESS) { setFallback("record_failed"); s->disabled = true; return 0; }

    // ---- 5) Submit: WAIT GL->VK sem (GL owns the target), SIGNAL VK->GL sem + fence. ----
    const bool useGpuVk2Gl = s->colorGlView.vkToGlImported && (fs.vk2gl != VK_NULL_HANDLE);
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = 1; si.pWaitSemaphores = &fs.signal; si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    if (useGpuVk2Gl) { si.signalSemaphoreCount = 1; si.pSignalSemaphores = &fs.vk2gl; }
    if (vkQueueSubmit(s->queue, 1, &si, fs.fence) != VK_SUCCESS) { setFallback("submit_failed"); s->disabled = true; return 0; }
    if (useGpuVk2Gl) {
        glInteropWaitFromVk(s->colorGlView, GL_LAYOUT_COLOR_ATTACHMENT_EXT, fs.parity);
    }
    // WAW retire (water/shadow pattern): fully retire this frame's submit before the
    // next frame reuses the parity slot.
    if (vkWaitForFences(s->device, 1, &fs.fence, VK_TRUE, 5ull*1000*1000*1000) != VK_SUCCESS) {
        setFallback("fence_timeout"); s->disabled = true; return 0;
    }
    g_health.drawUs = usSince(tRender0);

    // ---- 6) Composite the VK prop color onto MainColor (premult-over). ----
    composite_props_onto_maincolor(s, w, h);

    // ---- HARD assert: no COLOR-side readback fired on the interop path. ----
    const bool readbackFired =
        (g_forbidden.vkCmdCopyImageToBuffer != f0a) ||
        (g_forbidden.glGetTexImage != f0b) || (g_forbidden.glReadPixels != f0c);
    if (readbackFired) {
        vlog("FATAL: a COLOR-side readback/copy fired on the interop path — zero-copy "
             "invariant violated; failing frame + disabling.");
        setFallback("interop_did_readback"); s->disabled = true; return 0;
    }

    g_health.vkDraws = 1;
    ++g_health.usedVulkan;
    ++g_health.usedInterop;
    g_health.totalUs = usSince(tTotal0);
    static bool s_firstOk = false;
    if (!s_firstOk) {
        vlog("first VK prop color produced: instCount=%u typeRuns=%lu tex=%u (GL now composites "
             "the VK-produced RGBA16F over MainColor).", instanceCount, g_health.typeRuns, s->colorGlView.glTex);
        s_firstOk = true;
    }
    return 1;
}

extern "C" void mc2_render_backend_color_prop_region_health(
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

extern "C" void mc2_vk_color_prop_island_destroy() {
    vlog("[VK_PROP_COLOR_INTEROP] vulkan_available=%d used_vulkan=%lu used_interop=%lu interop_active=%d "
         "vk2gl_sem=%d validation_errors=%lu inst_records=%lu type_runs=%lu geom_verts=%lu "
         "fallback_reason=\"%s\" vk_draws=%lu forbidden_copyImgToBuf=%lu forbidden_getTexImage=%lu "
         "forbidden_readPixels=%lu stage_inst_us=%.1f draw_us=%.1f total_us=%.1f",
         g_health.vulkanAvailable, g_health.usedVulkan, g_health.usedInterop, g_health.interopActive,
         g_health.vk2glSemActive, g_health.validationErrors, g_health.instRecords, g_health.typeRuns,
         g_health.geomVerts, g_health.fallbackReason, g_health.vkDraws,
         g_forbidden.vkCmdCopyImageToBuffer, g_forbidden.glGetTexImage, g_forbidden.glReadPixels,
         g_health.stageInstUs, g_health.drawUs, g_health.totalUs);
    if (!g_island) return;
    destroy_island(g_island);
    delete g_island; g_island = nullptr;
}

#endif // MC2_VULKAN_ISLAND
