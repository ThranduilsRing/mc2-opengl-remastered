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
#include "mission_environment_gpu.h"
#include "visual_tuning_profile.h"
#include "../../mclib/camera.h"
#include "vk_render_pass_timer.h"
#include "vk_gpu_profile.h"   // VK-GPU-ZONES: MC2_VK_GPU_SCOPE
#include <thread>
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
#include "vk_vegetation_ground.h"
#include "terrain_field_store.h"
#include "vulkan_post_chain.h"
#include "vulkan_sky_island.h"
#include <limits>

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
#include <optional>
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


struct FogOobParams {
    float invViewProj[16];     // off 0
    float u_fogColor[3];       // off 64
    float u_fogOpacity;        // off 76
    float u_time;              // off 80
    int   u_horizonClampEnabled;   // off 84 (VK-PARITY-F3: FOG-HORIZON-CLAMP-1)
    float u_horizonFadeStartSin;   // off 88
    float u_horizonFadeEndSin;     // off 92
    float u_cloudSea[4];           // off 96 altitude, thickness, billow, quality
    float u_cloudSun[4];           // off 112 toward sun, sky radiance blue scale
    float u_cloudMotion[4];        // off 128 seconds, map half extent, sky radiance red/green scales
    float u_cloudLight[4];         // off 144 shared live sun colour
    float u_cloudAmbient[4];       // off 160 shared live ambient colour
};
static_assert(sizeof(FogOobParams) == 176, "FogOobParams std140 offsets drifted");

// std140 POD of shaders/vulkan/valley_fog.frag ValleyFogParams; offsets are offline-checked by check_ubo_layout.py postfog_valley.
struct ValleyFogParams {
    float invViewProj[16];    // off 0
    float eye[4];             // off 64  xyz camera, w highest fog floor on the map
    float layer[4];           // off 80  thickness wu, sigma per wu, coverage threshold, noise scale wu
    float shape[4];           // off 96  top relief wu, soft top wu, time s, quality
    float fogColor[4];        // off 112 rgb
    float sunTint[4];         // off 128 rgb sun multiplier, w static-shadow strength
};
static_assert(sizeof(ValleyFogParams) == 144, "ValleyFogParams std140 offsets drifted");
constexpr float kValleyNoiseScale = 900.0f;   // bank size (wu)
constexpr float kValleyRelief = 90.0f;        // rolling of the lit top (wu)
constexpr float kValleySoftTop = 60.0f;       // density ramp under the top (wu)

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
        VkDescriptorSet edgeSet = VK_NULL_HANDLE, oobSet = VK_NULL_HANDLE, valleySet = VK_NULL_HANDLE;
        VkCommandBuffer cbuf = VK_NULL_HANDLE;
        mc2vk::OwnedCommandPool cpool;   // per-frame-slot pool: vkResetCommandPool instead of per-cb reset
        mc2vk::OwnedBuffer edgeUbo, oobUbo, valleyUbo;
        uint64_t ticket = 0;
    } frames[mc2vk::kMaxFramesInFlight];

    // OWNED objects, declared in creation order: the destructor releases them in reverse (the owner idles the GPU first, see destroy_state).
    mc2vk::OwnedImageView  depthSampleView; // depth-aspect view over sDepthRes->image
    mc2vk::OwnedSampler    depthSampler;    // NEAREST/CLAMP (matches GL fog sampling)
    mc2vk::OwnedImage      cloudImg;        // cloud-sea crown heights (tiling R16, mipped), baked once
    mc2vk::OwnedImageView  cloudView;
    mc2vk::OwnedSampler    cloudSampler;    // LINEAR/REPEAT
    mc2vk::OwnedBuffer     cloudUpload;
    bool                   cloudUploaded = false;
    mc2vk::OwnedRenderPass rpass;
    mc2vk::OwnedFramebuffer fb;
    mc2vk::OwnedDescriptorSetLayout dsl;
    mc2vk::OwnedDescriptorPool pool;
    mc2vk::OwnedShaderModule edgeVert, edgeFrag;
    mc2vk::OwnedShaderModule oobVert, oobFrag;
    mc2vk::OwnedPipelineLayout playout;
    mc2vk::OwnedPipeline   edgePipe;
    mc2vk::OwnedPipeline   oobPipe;
    mc2vk::OwnedShaderModule valleyFrag;
    mc2vk::OwnedPipeline   valleyPipe;
    mc2veg::GroundLighting groundLighting;   // set 1 of the valley fog: the terrain's frame UBO and live environment
    // VALLEY-FOG (ground-fog-4): the loaded mission's fog floor (TerrainFieldStore::fogFloor), re-uploaded when the height
    // field's version changes.
    mc2vk::OwnedBuffer fogFloorBuf;
    size_t   fogFloorCapacity = 0;   // floats
    uint64_t fogFloorVersion = 0;
};

namespace {

// ---- Cloud-sea crown heights: the deck's billow noise baked once into a tiling R16 texture (all mips), so the
// shader's ray march costs one fetch per step instead of a dozen hashes.
constexpr int kCloudTex = 1024;      // texels per side
constexpr int kCloudPeriod = 8;      // tile = 8 noise cells (x2600 world units)
constexpr int kCloudMips = 11;

float cloudHash(float x, float y) {
    auto fr = [](float v) { return v - std::floor(v); };
    float a = fr(x * 0.1031f), b = fr(y * 0.1031f), c = a;
    const float d = a * (b + 33.33f) + b * (c + 33.33f) + c * (a + 33.33f);
    a += d; b += d; c += d;
    return fr((a + b) * c);
}
// Value noise on a lattice that wraps every `per` cells.
float cloudNoise(float x, float y, int per) {
    const float fx0 = std::floor(x), fy0 = std::floor(y);
    float fx = x - fx0, fy = y - fy0;
    fx = fx * fx * (3.0f - 2.0f * fx); fy = fy * fy * (3.0f - 2.0f * fy);
    auto wrap = [per](float v) { int i = static_cast<int>(v) % per; return static_cast<float>(i < 0 ? i + per : i); };
    const float x0 = wrap(fx0), x1 = wrap(fx0 + 1.0f), y0 = wrap(fy0), y1 = wrap(fy0 + 1.0f);
    const float a = cloudHash(x0, y0) + (cloudHash(x1, y0) - cloudHash(x0, y0)) * fx;
    const float b = cloudHash(x0, y1) + (cloudHash(x1, y1) - cloudHash(x0, y1)) * fx;
    return a + (b - a) * fy;
}
float cloudBillow(float x, float y, int per) { return std::fabs(2.0f * cloudNoise(x, y, per) - 1.0f); }
float cloudSmooth(float e0, float e1, float v) { const float t = std::clamp((v - e0) / (e1 - e0), 0.0f, 1.0f); return t * t * (3.0f - 2.0f * t); }

// All mip levels, level 0 first (tightly packed), crown height 0..1.2 mapped to 16 bits.
std::vector<uint16_t> bakeCloudHeights() {
    const int P = kCloudPeriod;
    std::vector<uint16_t> all;
    std::vector<uint16_t> level(static_cast<size_t>(kCloudTex) * kCloudTex);
    auto bakeRows = [&](int rowBegin, int rowStep) {
    for (int y = rowBegin; y < kCloudTex; y += rowStep) {
        for (int x = 0; x < kCloudTex; ++x) {
            const float qx = (static_cast<float>(x) + 0.5f) * P / kCloudTex, qy = (static_cast<float>(y) + 0.5f) * P / kCloudTex;
            const float wx = 0.45f * cloudNoise(qx * 0.75f + 3.0f, qy * 0.75f + 3.0f, 6) - 0.22f;
            const float wy = 0.45f * cloudNoise(qx * 0.75f + 9.0f, qy * 0.75f + 9.0f, 6) - 0.22f;
            const float ux = qx + wx, uy = qy + wy;
            float h = 0.55f * cloudBillow(ux, uy, P);
            h += 0.30f * cloudBillow(ux * 2.0f + 7.0f, uy * 2.0f + 7.0f, P * 2);
            h += 0.17f * cloudBillow(ux * 5.0f + 13.0f, uy * 5.0f + 13.0f, P * 5);
            h += 0.07f * cloudBillow(ux * 11.0f + 29.0f, uy * 11.0f + 29.0f, P * 11);
            h += 0.030f * cloudBillow(ux * 23.0f + 5.0f, uy * 23.0f + 5.0f, P * 23);
            h += 0.014f * cloudBillow(ux * 47.0f + 17.0f, uy * 47.0f + 17.0f, P * 47);
            const float cover = cloudNoise(ux * 0.25f + 21.0f, uy * 0.25f + 21.0f, P / 4);
            const float gaps = 0.3f + 0.7f * cloudSmooth(0.12f, 0.38f, cover);
            const float v = std::clamp(h * (0.85f + 1.1f * cover) * gaps, 0.0f, 1.2f) / 1.2f;
            level[static_cast<size_t>(y) * kCloudTex + x] = static_cast<uint16_t>(v * 65535.0f + 0.5f);
        }
    }
    };
    {
        const int workers = static_cast<int>(std::clamp(std::thread::hardware_concurrency(), 1u, 16u));
        std::vector<std::thread> pool;
        for (int w = 1; w < workers; ++w) pool.emplace_back(bakeRows, w, workers);
        bakeRows(0, workers);
        for (auto& t : pool) t.join();
    }
    all.insert(all.end(), level.begin(), level.end());
    int n = kCloudTex;
    for (int m = 1; m < kCloudMips; ++m) {
        const int h = n / 2;
        std::vector<uint16_t> next(static_cast<size_t>(h) * h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < h; ++x) {
                const uint32_t sum = level[static_cast<size_t>(2 * y) * n + 2 * x] + level[static_cast<size_t>(2 * y) * n + 2 * x + 1] +
                                     level[static_cast<size_t>(2 * y + 1) * n + 2 * x] + level[static_cast<size_t>(2 * y + 1) * n + 2 * x + 1];
                next[static_cast<size_t>(y) * h + x] = static_cast<uint16_t>(sum / 4);
            }
        all.insert(all.end(), next.begin(), next.end());
        level.swap(next);
        n = h;
    }
    return all;
}

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

    {
        if (!s->cloudImg.createSampled2D(s->allocator, s->device, VK_FORMAT_R16_UNORM, kCloudTex, kCloudTex, kCloudMips, 0, "postfog.cloudHeights")) {
            vlog("cloud height image create failed."); return false;
        }
        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = s->cloudImg.get(); vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = VK_FORMAT_R16_UNORM;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, kCloudMips, 0, 1};
        if (!mc2vk::create(s->cloudView, s->device, [&](VkImageView* h) { r = vkCreateImageView(s->device, &vci, nullptr, h); return r; })) {
            vlog("cloud height view create failed (%d).", static_cast<int>(r)); return false;
        }
        VkSamplerCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.maxLod = static_cast<float>(kCloudMips);
        if (!mc2vk::create(s->cloudSampler, s->device, [&](VkSampler* h) { r = vkCreateSampler(s->device, &sci, nullptr, h); return r; })) {
            vlog("cloud sampler create failed (%d).", static_cast<int>(r)); return false;
        }
        const std::vector<uint16_t> heights = bakeCloudHeights();
        if (!s->cloudUpload.createFilled(s->allocator, s->device, heights.data(), heights.size() * sizeof(uint16_t), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, "postfog.cloudUpload")) {
            vlog("cloud height upload buffer failed."); return false;
        }
    }

    for (auto& frame : s->frames) {
        if (!make_buffer(s, sizeof(EdgeFogParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, frame.edgeUbo, "postfog.edgeUbo")) return false;
        if (!make_buffer(s, sizeof(FogOobParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, frame.oobUbo, "postfog.oobUbo")) return false;
        if (!make_buffer(s, sizeof(ValleyFogParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, frame.valleyUbo, "postfog.valleyUbo")) return false;
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

    // Descriptor layout / pool / 3 sets (subgraph interface: 0=sampler, 1=UBO, frag). VALLEY-FOG (ground-fog-4) adds
    // 2 = the fog-floor SSBO and 3 = the terrain static-shadow mask copy; the edge/oob shaders read neither.
    {
        VkDescriptorSetLayoutBinding binds[5]{};
        binds[0].binding = 0; binds[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binds[0].descriptorCount = 1; binds[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        binds[1].binding = 1; binds[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        binds[1].descriptorCount = 1; binds[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        binds[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        binds[3] = {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        binds[4] = binds[0]; binds[4].binding = 4;
        lci.bindingCount = 5; lci.pBindings = binds;
        if (!mc2vk::create(s->dsl, s->device, [&](VkDescriptorSetLayout* h) { r = vkCreateDescriptorSetLayout(s->device, &lci, nullptr, h); return r; })) {
            vlog("vkCreateDescriptorSetLayout failed (%d).", static_cast<int>(r)); return false;
        }
        VkDescriptorPoolSize sizes[3]{};
        sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; sizes[0].descriptorCount = 9 * mc2vk::kMaxFramesInFlight;
        sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;         sizes[1].descriptorCount = 3 * mc2vk::kMaxFramesInFlight;
        sizes[2].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;         sizes[2].descriptorCount = 3 * mc2vk::kMaxFramesInFlight;
        VkDescriptorPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.maxSets = 3 * mc2vk::kMaxFramesInFlight; pci.poolSizeCount = 3; pci.pPoolSizes = sizes;
        if (!mc2vk::create(s->pool, s->device, [&](VkDescriptorPool* h) { r = vkCreateDescriptorPool(s->device, &pci, nullptr, h); return r; })) {
            vlog("vkCreateDescriptorPool failed (%d).", static_cast<int>(r)); return false;
        }
    }
    for (auto& frame : s->frames) {
        VkDescriptorSetLayout layouts[3] = { s->dsl.get(), s->dsl.get(), s->dsl.get() };
        VkDescriptorSet setsOut[3] = { VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE };
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = s->pool.get(); dsai.descriptorSetCount = 3; dsai.pSetLayouts = layouts;
        r = MC2_VK_ALLOC_SETS(s->device, &dsai, setsOut);
        if (r != VK_SUCCESS) { vlog("vkAllocateDescriptorSets failed (%d).", static_cast<int>(r)); return false; }
        frame.edgeSet = setsOut[0]; frame.oobSet = setsOut[1]; frame.valleySet = setsOut[2];

        VkDescriptorImageInfo imgInfo{};
        imgInfo.sampler = s->depthSampler.get();
        imgInfo.imageView = s->depthSampleView.get();
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorBufferInfo edgeUboInfo{ frame.edgeUbo.get(), 0, VK_WHOLE_SIZE };
        VkDescriptorBufferInfo oobUboInfo { frame.oobUbo.get(), 0, VK_WHOLE_SIZE };
        VkDescriptorBufferInfo valleyUboInfo { frame.valleyUbo.get(), 0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet writes[6]{};
        VkDescriptorImageInfo cloudInfo{ s->cloudSampler.get(), s->cloudView.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet cloudWrites[2]{};
        for (int i = 4; i < 6; ++i) {
            cloudWrites[i - 4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            cloudWrites[i - 4].dstSet = i == 4 ? frame.edgeSet : frame.oobSet; cloudWrites[i - 4].dstBinding = 3; cloudWrites[i - 4].descriptorCount = 1;
            cloudWrites[i - 4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            cloudWrites[i - 4].pImageInfo = &cloudInfo;
        }
        vkUpdateDescriptorSets(s->device, 2, cloudWrites, 0, nullptr);
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
        writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[4].dstSet = frame.valleySet; writes[4].dstBinding = 1; writes[4].descriptorCount = 1;
        writes[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[4].pBufferInfo = &valleyUboInfo;
        writes[5] = writes[0];
        writes[5].dstSet = frame.valleySet;
        vkUpdateDescriptorSets(s->device, 6, writes, 0, nullptr);
    }

    // Pipelines over one fullscreen-triangle vertex shader (edge_fog.vert) — fixed-function state IDENTICAL to the subgraph's
    // (no vertex input, cull none, AlphaBlend SRC_ALPHA/ONE_MINUS_SRC_ALPHA, no depth attachment). Fog shaders reused verbatim.
    {
        s->edgeVert = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir() + "edge_fog.vert.spv"));
        s->edgeFrag = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir() + "edge_fog.frag.spv"));
        s->oobVert  = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir() + "fog_oob.vert.spv"));
        s->oobFrag  = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir() + "fog_oob.frag.spv"));
        s->valleyFrag = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir() + "valley_fog.frag.spv"));
        if (!s->edgeVert || !s->edgeFrag || !s->oobVert || !s->oobFrag || !s->valleyFrag) {
            setFallback("spv_load_failed"); return false;
        }
        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        // Set 1 (the terrain's lighting subset) is bound by the valley fog draw only.
        if (!s->groundLighting.init(s->device, true)) { setFallback("ground_lighting_init_failed"); return false; }
        const VkDescriptorSetLayout setLayouts[2] = { s->dsl.get(), s->groundLighting.layout() };
        plci.setLayoutCount = 2; plci.pSetLayouts = setLayouts;
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
        if (!make_pipeline(s->edgeVert.get(), s->valleyFrag.get(), s->valleyPipe, "postfog.valleyPipe")) return false;
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

// VALLEY-FOG: the baked fog floor of the loaded mission in the SSBO (binding 2 of the valley sets), re-uploaded when the
// height field's version changes (mission load, craters). Null while no terrain is loaded.
const mc2terrain::TerrainFieldStore* sync_fog_floor(gosPostProcess::VkPostFogNative* s) {
    const auto* store = gos_TerrainFieldStore();
    if (!store || store->mapSide() <= 0) return nullptr;
    const auto& floor = store->fogFloor();
    if (floor.size() > s->fogFloorCapacity) {
        // A bigger map than this buffer ever held: another frame slot may still read the old floor, so retire it first.
        mc2vk::getOrCreateEngineVulkanContext().waitIdle("fog_floor_grow");
        s->fogFloorBuf.reset();
        if (!make_buffer(s, static_cast<VkDeviceSize>(floor.size()) * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                         s->fogFloorBuf, "postfog.fogFloor")) return nullptr;
        s->fogFloorCapacity = floor.size();
        s->fogFloorVersion = 0;
        for (auto& frame : s->frames) {
            VkDescriptorBufferInfo info{ s->fogFloorBuf.get(), 0, VK_WHOLE_SIZE };
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = frame.valleySet; write.dstBinding = 2; write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; write.pBufferInfo = &info;
            vkUpdateDescriptorSets(s->device, 1, &write, 0, nullptr);
        }
    }
    if (s->fogFloorVersion != store->heightVersion()) {
        if (!s->fogFloorBuf.upload(floor.data(), floor.size() * sizeof(float))) return nullptr;
        s->fogFloorVersion = store->heightVersion();
    }
    return store;
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

void gosPostProcess::fillWorldFogParams(EdgeFogParams& p) const {
    p = {};
    std::memcpy(p.invViewProj, inverseViewProj_, 16 * sizeof(float));
    const auto& env = visualTuning_environment();
    const auto radiance = mc2::environment::skyRadianceScale(env);
    for (int c = 0; c < 3; ++c)
        p.u_fogColor[c] = edgeFogColor_[c] * std::pow(radiance[c], 1.0f / 2.2f);
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
        float lx = env.lighting.effectiveDirection[0];
        float ly = env.lighting.effectiveDirection[1];
        float lz = env.lighting.effectiveDirection[2];
        const float len = std::sqrt(lx * lx + ly * ly + lz * lz);
        if (len > 1e-6f) { lx /= len; ly /= len; lz /= len; } else { lx = ly = 0.f; lz = 1.f; }
        const float sunScale = (std::max)({env.lighting.directionalColor[0], env.lighting.directionalColor[1], env.lighting.directionalColor[2]});
        p.u_atmoParams[0] = static_cast<float>(quality);
        p.u_atmoParams[1] = 6e-5f * atmoDensity_ * (quality == 1 ? 0.7f : 1.0f);
        p.u_atmoParams[2] = atmoFalloff_;
        p.u_atmoParams[3] = atmoAnisotropy_;
        p.u_atmoSun[0] = lx; p.u_atmoSun[1] = ly; p.u_atmoSun[2] = lz;
        p.u_atmoSun[3] = atmoSunScatter_ * sunScale;
        for (int c = 0; c < 3; ++c) p.u_atmoColor[c] = p.u_fogColor[c] * 0.85f;
        p.u_atmoColor[3] = waterElevation_;
    }
    if (!sceneHasTerrain_ || !edgeFogEnabled_) { p.u_fogMax = 0.0f; p.u_atmoParams[0] = 0.0f; p.u_halfExtent = 0.0f; }
}

bool gosPostProcess::runVkPostFogNative()
{
    ZoneScopedN("VKIsl.Post.Fog");
    if (mc2::options::lowCost()) return true;
    // VALLEY-FOG (ground-fog-4): banks on missions keyed with a thickness (visual_tuning valleyFogThickness), scaled by the
    // player's ValleyFog option. The pass reads the terrain's lighting set, published once the terrain drew this frame.
    const int valleyOption = std::clamp(mc2::options::current().valleyFog, 0, 2);
    const bool valleyWanted = sceneHasTerrain_ && valleyFogThickness_ > 0.0f && valleyOption > 0;
    auto fog=RenderCore::vulkan::selectFogPasses(sceneHasTerrain_,edgeFogEnabled_,fogOobEnabled_,mapHalfExtent_,valleyWanted);
    const auto ground = valleyWanted ? mc2_vk_ground_lighting_frame() : std::nullopt;
    fog.valley = fog.valley && ground.has_value();
    if (!fog.edge && !fog.oob && !fog.valley) return false;
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

    // These descriptor sets belong to the retired frame slot. Borrow the sky
    // image rather than allocating another copy for horizon colour.
    VkDescriptorImageInfo horizon{};
    const bool haveHorizon = mc2_vk_sky_sample_info(&horizon) != 0;
    if (!horizon.imageView) return false;
    const float horizonYaw = haveHorizon ? skyYaw_ : std::numeric_limits<float>::quiet_NaN();
    VkWriteDescriptorSet horizonWrites[2]{};
    for (int i = 0; i < 2; ++i) {
        auto& write = horizonWrites[i];
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = i == 0 ? frame.edgeSet : frame.oobSet;
        write.dstBinding = 4; write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &horizon;
    }
    vkUpdateDescriptorSets(s->device, 2, horizonWrites, 0, nullptr);

    // ---- UBOs ----
    EnvironmentUBO environment;
    mc2::environment::fillGpu(visualTuning_environment(), mc2SceneLightScale(), environment,
        eye ? eye->currentLighting() : mc2::lighting::LightingState{},
        eye ? eye->bakeReferenceLighting() : mc2::lighting::LightingState{});
    const float animTime = SmokeMode::fixedTimestepEnabled()
                               ? static_cast<float>(SmokeMode::fixedClockSeconds())
                               : static_cast<float>(SDL_GetTicks()) / 1000.0f;
    if (fog.edge) {
        EdgeFogParams p{};
        fillWorldFogParams(p);
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
        p.u_time = horizonYaw;
        p.u_cloudSea[0] = waterElevation_ + cloudSeaAltitude_;
        p.u_cloudSea[1] = cloudSeaThickness_;
        p.u_cloudSea[2] = cloudSeaBillow_;
        p.u_cloudSea[3] = static_cast<float>(mc2::options::current().cloudSeaQuality);
        std::memcpy(p.u_cloudSun, environment.sunDirLive, 3 * sizeof(float));
        const auto skyRadiance = mc2::environment::skyRadianceScale(visualTuning_environment());
        p.u_cloudSun[3] = skyRadiance[2];
        std::memcpy(p.u_cloudLight, environment.sunLive, sizeof(p.u_cloudLight));
        std::memcpy(p.u_cloudAmbient, environment.ambLive, sizeof(p.u_cloudAmbient));
        p.u_cloudMotion[0] = static_cast<float>(SmokeMode::fixedTimestepEnabled()
            ? SmokeMode::fixedClockSeconds() : SDL_GetTicks64() * 0.001);
        p.u_cloudMotion[1] = mapHalfExtent_;
        p.u_cloudMotion[2] = skyRadiance[0];
        p.u_cloudMotion[3] = skyRadiance[1];
        if (void* m = frame.oobUbo.map()) {
            std::memcpy(m, &p, sizeof(p));
            frame.oobUbo.unmap();
        }
    }
    if (fog.valley) {
        ValleyFogParams p{};
        std::memcpy(p.invViewProj, inverseViewProj_, sizeof(p.invViewProj));
        // Each pixel reconstructs its own near-plane ray origin in the shader.
        const bool low = valleyOption < 2;
        p.layer[0] = valleyFogThickness_;
        p.layer[1] = valleyFogSigma_ * (low ? 0.7f : 1.0f);
        p.layer[2] = valleyFogCoverage_;
        p.layer[3] = kValleyNoiseScale;
        p.shape[0] = kValleyRelief;
        p.shape[1] = kValleySoftTop;
        p.shape[2] = animTime;
        p.shape[3] = low ? 1.0f : 2.0f;
        for (int c = 0; c < 3; ++c) p.fogColor[c] = valleyFogColor_[c];
        p.sunTint[0] = p.sunTint[1] = p.sunTint[2] = 1.0f;
        p.sunTint[3] = 0.5f;   // static terrain shadow: faint
        const auto* store = sync_fog_floor(s);
        if (!store) fog.valley = false;
        else {
            p.eye[3] = store->fogFloorMax();
            if (void* m = frame.valleyUbo.map()) {
                std::memcpy(m, &p, sizeof(p));
                frame.valleyUbo.unmap();
            }
            s->groundLighting.copy(s->device, mc2vk::getOrCreateEngineVulkanContext().frameContext().slot, *ground);
            // The terrain's static-shadow mask into this slot's set (the slot's previous use has completed above).
            VkCopyDescriptorSet maskCopy{VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET};
            maskCopy.srcSet = ground->set; maskCopy.srcBinding = 17;
            maskCopy.dstSet = frame.valleySet; maskCopy.dstBinding = 3; maskCopy.descriptorCount = 1;
            vkUpdateDescriptorSets(s->device, 0, nullptr, 1, &maskCopy);
        }
    }

    // ---- Shared resource ownership (VK-RUNG8 item-4: direct-chain only). Native fog
    // engages only when upstream left the shared GBuffer Vulkan-owned; otherwise decline
    // to the GL fog pass (native-or-decline). No GL<->VK semaphore handoff. ----
    if (!mc2vk::postchain::chain().open) vkResetCommandPool(s->device, frame.cpool.get(), 0);
    VkCommandBuffer cb = mc2vk::postchain::acquire(frame.cbuf, s->sDepthRes->image.get(), nullptr);
    if (!cb) { setFallback("record_failed"); s->disabled = true; return false; }
    mc2vk::postchain::addTicket(&frame.ticket);

    if (!s->cloudUploaded) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.image = s->cloudImg.get(); b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, kCloudMips, 0, 1};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        VkBufferImageCopy copies[kCloudMips]{};
        VkDeviceSize offset = 0; uint32_t n = kCloudTex;
        for (int m = 0; m < kCloudMips; ++m, n /= 2) {
            copies[m].bufferOffset = offset; copies[m].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, static_cast<uint32_t>(m), 0, 1};
            copies[m].imageExtent = {n, n, 1};
            offset += static_cast<VkDeviceSize>(n) * n * sizeof(uint16_t);
        }
        vkCmdCopyBufferToImage(cb, s->cloudUpload.get(), s->cloudImg.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kCloudMips, copies);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        s->cloudUploaded = true;
    }

    // Fog blends over the shadowed scene inside the chain's render pass; depth is already in the sampleable layout (read once for the chain).
    mc2vk::postchain::beginPass(s->rpass.get(), s->fb.get(), static_cast<uint32_t>(s->width), static_cast<uint32_t>(s->height));
    MC2_VK_GPU_SCOPE(gpuScope, cb, "VKIsl.Post.Fog");
    const auto fogDraws = fog.record([&](RenderCore::vulkan::PostprocessSubPassId pass) {
        if (pass==RenderCore::vulkan::PostprocessSubPassId::ValleyFog) {
            MC2_VK_GPU_SCOPE(valleyScope, cb, "VKIsl.Post.ValleyFog");
            mc2vk::infra::bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->valleyPipe.get());
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->playout.get(), 0, 1, &frame.valleySet, 0, nullptr);
            const auto groundSet = s->groundLighting.set(mc2vk::getOrCreateEngineVulkanContext().frameContext().slot);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->playout.get(), 1, 1, &groundSet, 4, ground->offsets.data());
            vkCmdDraw(cb, 3, 1, 0, 0);
            return;
        }
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

