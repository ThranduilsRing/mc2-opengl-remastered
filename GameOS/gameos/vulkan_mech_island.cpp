#include "../../RenderCore/TextureService.h"
// vulkan_mech_island.cpp — VK-P14 MECH TIER-1: the FIRST VK-native SKINNED mech region.
// =============================================================================
// Co-writes the GL mech batcher's visible instances IN-PLACE into the SHARED scene
// GBuffer (MRT0 color + MRT1 normal + shared D32S8 depth), VK-skinned. Structure =
// vulkan_color_prop_island.cpp's TIER-1 co-write path (shared-GBuffer LOAD-preserve
// renderpass, GEQUAL reverse-Z, no composite), with the static-prop 40B vertex +
// modelMatrix instance swapped for:
//   - the mech 48B vertex stream (GpuMechVertex: pos/normal/uv/boneIdx u8[4]/
//     boneWeights u8[4]/tangentOct/aRGBLight), copied ONCE per map;
//   - a per-frame BONE PALETTE SSBO (GpuMechBone 64B) memcpy'd each frame;
//   - a per-frame INSTANCE SSBO (GpuMechInstance 64B);
//   - a VS that does LINEAR-BLEND SKINNING over 4 bone weights (mech.vert:156-180),
//     then the Stuff->MC2 axis swap worldMC2 = vec3(-x,z,y) (mech.vert:178).
//
// SUB-SLICE 1 (this file): skinning + FLAT color. This is the P-conjugation /
// axis-swap silhouette proof — verify the mech is not gliding/mirrored/in-sky
// BEFORE adding material + calc_light lighting. Those are gated follow-ons:
//   MC2_VK_MECH_MATERIAL  — copy-once single 2D albedo (slot-0 paint scheme) sampled
//                           per run (single-material probe; per-actor VK-descriptor
//                           cache DEFERRED — see the run() material note).
//   MC2_VK_MECH_LIGHTING  — port calc_light + feed the LightsData UBO (DEFERRED;
//                           needs the whole ObjectLights SSBO mirrored to VK).
//
// Gate MC2_VK_MECH_TIER1 (requires MC2_VK_SHARED_SCENE_DEPTH), default-OFF -> GL
// mech color flush byte-identical. The feed (instances + bone palette + shared
// VBO/IBO + per-run slot-0 GL texture) comes from gos_mech_batcher.cpp via the
// Mc2Mech*Feed accessors. NO fenced GL buffer crosses.
//
// The whole TU compiles to nothing unless MC2_VULKAN_ISLAND is defined.
// =============================================================================

#ifdef MC2_VULKAN_ISLAND

#if defined(_WIN32) && !defined(VK_USE_PLATFORM_WIN32_KHR)
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <volk.h>
#include "../../mclib/battle_bench_wait.h"
#include "Gates/gate.h"
#include "vk_render_pass_timer.h"
#include "vk_gpu_profile.h"   // VK-GPU-ZONES: MC2_VK_GPU_SCOPE
#include "Gates/gate_cold.h"   // str<>()
#include "Gates/render_options.h"   // options.cfg SunAxisFix

#include <vk_mem_alloc.h>

#include "vk_image_resource.h"
#include "vk_island_util.h"
#include "vk_one_shot.h"
#include "vulkan_context.h"
#include "vulkan_mech_island.h"
#include "vk_texture_residency.h"
#include "texture_decode.h"              // gos_GetTextureTexels: material texels without a GL readback
#include "../../RenderCore/gpu_abi/GpuMechFrameUBO.h"  // single-source per-frame UBO
#include "visual_tuning_profile.h"
#include "vk_shared_gbuffer_gate.h"      // VK-P14 GATE-CONSOLIDATION-1 umbrella resolver
#include "vk_shared_light_feed.h"        // VK-LIGHTFEED-DEDUP-1: shared per-frame LightsData SSBO
#include "gos_terrain_lighting.h"
#include "vulkan_shadow_island_live.h"   // LOCAL-LIGHTS-2: mc2_vk_shadow_island_get_local_atlas_view
#include "vk_vegetation_ground.h"
#include "vk_gpu_timestamps.h"           // VK-DRAW-PERF-1: MC2_VK_GPU_TIMESTAMPS GPU segment timers
#define MC2_VULKAN_SHARED_SCENE_COLOR_IMPL_ISLAND
#include "vulkan_shared_scene_color.h"   // shared scene color/normal images
#define MC2_VULKAN_SHARED_SCENE_DEPTH_IMPL_ISLAND
#include "vulkan_shared_scene_depth.h"   // shared scene depth image

#include <array>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cstdint>
#include <vector>
#include <string>
#include <string_view>
#include <chrono>
#include <map>
#include <tuple>
#include <limits>
#include <cmath>
#include "../../RenderCore/gpu_abi/GpuMechInstance.h"
#include "../../RenderCore/axis_convert.h"
#include <tracy/Tracy.hpp>   // VK-FLIP-FIELD-REPORT-1: coarse per-region frame zones

namespace {

struct MechIslandHealth {
    int   vulkanAvailable = 0;
    int   buildEnabled    = 1;
    char  deviceName[256] = {0};
    unsigned long attempted   = 0;
    unsigned long usedVulkan  = 0;
    char  fallbackReason[256] = {0};
    unsigned long vkDraws     = 0;
    int   interopActive  = 0;
    int   vk2glSemActive = 0;
    unsigned long instRecords = 0;
    unsigned long boneRecords = 0;
    unsigned long runs        = 0;
    unsigned long geomVerts   = 0;
    double stageUs = 0.0, drawUs = 0.0, totalUs = 0.0;
};
MechIslandHealth g_health;

struct InteropForbidden {
    unsigned long glGetTexImage = 0;   // color CPU pull (must be 0)
    unsigned long glReadPixels  = 0;   // color CPU pull (must be 0)
};
InteropForbidden g_forbidden;

void setFallback(const char* reason) {
    std::strncpy(g_health.fallbackReason, reason ? reason : "",
                 sizeof(g_health.fallbackReason) - 1);
    g_health.fallbackReason[sizeof(g_health.fallbackReason) - 1] = '\0';
}

void vlog(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vk_island_util::vlogv("[VK_MECH_ISLAND] ", fmt, ap);
    va_end(ap);
}

bool mechFenceDeferEnabled() {
    static int state = -1;
    if (state == -1)
        state = mc2::gate::get<"MC2_VK_MECH_FENCE_DEFER">() ? 1 : 0;
    return state == 1;
}

bool mechPerfGateRequested() {
    static int state = -1;
    if (state == -1) {
        state = false;
    }
    return state == 1;
}

// The mech island always renders direct-chain (terrain leaves the shared GBuffer
// Vulkan-owned); there is no GL<->VK semaphore-handoff co-write path.

// VK-RUNG8 item-9: the native
// CPU geometry + LightsData feed is unconditional under the VK mech island.
// The env name MC2_VK_MECH_FEED_NATIVE is still parsed + noted as deprecated in gos_mech_batcher.cpp; there is no per-frame gate to query here.


std::string spv_dir() { return vk_island_util::spvDir(); }

VkShaderModule load_spv(VkDevice device, std::string_view path) {
    return vk_island_util::loadSpv(device, path, vlog);
}

// The per-frame std140 UBO (MechFrameUBO) is single-sourced in
// RenderCore/gpu_abi/GpuMechFrameUBO.h; its generated block feeds both mech shaders.

// VK-PARITY-MECH-1: object light-pool SSBO cap (GpuTerrainLight 48B; GL caps 256).
const uint32_t kMaxPoolLights = 256;

const VkFormat kColorFmt = VK_FORMAT_R16G16B16A16_SFLOAT;

struct MechRunPC { uint32_t materialFlags, poolFirst, poolCount; };
static_assert(sizeof(MechRunPC) == 12);
struct MechIsland : mc2vk::IslandState {
    mc2veg::GroundLighting groundLighting;
    bool disabled = false;
    bool inited   = false;

    int w = 0, h = 0;

    // Borrowed from the shared engine context (NOT owned).
    VkInstance       instance  = VK_NULL_HANDLE;
    VkPhysicalDevice phys      = VK_NULL_HANDLE;
    VkDevice         device    = VK_NULL_HANDLE;
    uint32_t         gfxFamily = UINT32_MAX;
    VkQueue          queue     = VK_NULL_HANDLE;
    VmaAllocator     allocator = VK_NULL_HANDLE;

    // Shared-GBuffer co-write objects (over the SAME shared color/normal/depth
    // images the GL scene renders terrain into). Images are BORROWED.
    struct VkImageResource* colorRes  = nullptr;
    struct VkImageResource* normalRes = nullptr;
    struct VkImageResource* depthRes  = nullptr;
    // VK-RUNG8-ITEM5: shared scene is VK-only (no GL interop view). Mech is
    // direct-chain-native only; the former colorView/normalView/depthView are gone.

    // Copy-once mech geometry (VK-owned copies of the shared mech VBO/IBO).
    uint32_t vertexStride = 48;
    bool     geomUploaded = false;
    uint32_t geomGen = 0;   // generation of the feed the buffers were filled from

    // Per-frame data (UBO, instance + bone snapshots, LightsData, night pool) lives in the engine frame ring; every scheme set binds it with dynamic
    // offsets (bindings 0, 1, 2, 4, 5). ringBound[par][binding] = the ring buffer that binding of every scheme set of that parity points at; a frame
    // that allocated from another buffer (the ring grew) or a set created since rewrites it.
    VkBuffer ringBound[mc2vk::kMaxFramesInFlight][8] = {};
    // LOCAL-LIGHTS-2: the local-light shadow atlas binding 8 of every scheme set of a parity (null = sets hold the dummy).
    VkImageView localShadowBound[mc2vk::kMaxFramesInFlight] = {};

    // VK-MECH-MATERIAL-LIGHTING-1: per-frame LightsData span (binding 4) for calc_light.
    bool     wantLighting  = false;   // MC2_VK_MECH_LIGHTING gate
    bool     lightingReady = false;   // LightsData copied this frame (>=1 ObjectLights)

    // VK-PARITY-MECH-1: per-frame object light-pool SSBO (binding 5, FS) = CPU copy
    // of the GL binding-28 pool array (GetObjectLightPoolParams().pool).

    // Descriptors: 0=UBO(VS/FS), 1=instance SSBO(VS), 2=bone SSBO(VS), 3=albedo(FS), 4=LightsData(VS),
    // 5=pool SSBO(FS), 6=AO map(FS), 7=normal map(FS)  (5-7: VK-PARITY-MECH-1).

    // Per-actor generation identities borrow shared residency; only descriptor pairs live here.
    bool        wantMaterial  = false;
    bool        materialReady = false;   // >=1 real scheme imported
    mc2vk::ResidentTextureRef whiteTexture;
    struct SchemeEntry {
        uint64_t albedo=0,ao=0,nrm=0;
        VkDescriptorSet set[mc2vk::kMaxFramesInFlight]={};
        std::array<VkImageView,3> bound[mc2vk::kMaxFramesInFlight]{};
        uint64_t lastUsedFrame=~uint64_t(0);
    };
    std::vector<SchemeEntry> schemes;
    static constexpr uint32_t kMaxSchemes=4096; // descriptor cache only; image admission belongs to TextureResidency

    VkCommandBuffer cbuf[mc2vk::kMaxFramesInFlight] = {VK_NULL_HANDLE, VK_NULL_HANDLE};   // allocated from `cpool`, freed with it
    mc2vk::GpuTicket submitted[mc2vk::kMaxFramesInFlight];

    // VK-DRAW-PERF-1: per-pass GPU timestamp timer (3 stamps: pass-start / draws-start /
    // pass-end). Gated + fail-soft inside init(); no-op unless MC2_VK_GPU_TIMESTAMPS set.
    mc2vkts::GpuTimer gpuTimer;

    // Owned objects are declared in creation order and released in reverse by the destructor, while the device is alive (the engine context releases the
    // parked island ahead of the device). The timer owns its query pool and is torn down first.
    mc2vk::OwnedCommandPool cpool;
    mc2vk::OwnedRenderPass rpass;
    mc2vk::OwnedFramebuffer fb;
    mc2vk::OwnedDescriptorSetLayout dsl;
    mc2vk::OwnedDescriptorPool pool;
    mc2vk::OwnedSampler albedoSamp;
    mc2vk::OwnedShaderModule vert, frag, depthVert, depthFrag;
    mc2vk::OwnedPipelineLayout playout;
    mc2vk::OwnedPipeline pipe, depthPipe;
    mc2vk::OwnedBuffer vbo, ibo;
    // Mech-lab panel pass: the same skinned material path, drawn into the UI preview target (RGBA8 color LOAD over the
    // legacy preview pass, own cleared depth). Fail-soft: a missing variant leaves previewReady false and the world path intact.
    mc2vk::OwnedShaderModule previewFrag;
    mc2vk::OwnedRenderPass previewRpass;
    mc2vk::OwnedPipeline previewPipe, previewDepthPipe, backdropPipe;
    mc2vk::OwnedShaderModule backdropVert, backdropFrag;
    mc2vk::OwnedFramebuffer previewFb;
    VkImageView previewFbViews[3] = {};
    int previewFbW = 0, previewFbH = 0;
    bool previewReady = false;

    ~MechIsland() override { gpuTimer.destroy(); }
};

// The island lives in the engine context's slot: the context releases it ahead of the device whatever path ends the process.
std::unique_ptr<mc2vk::IslandState>& islandSlot() {
    return mc2vk::getOrCreateEngineVulkanContext().islandSlot(mc2vk::IslandSlot::Mech);
}
MechIsland* parkedIsland() { return static_cast<MechIsland*>(islandSlot().get()); }

// VK-RUNG8 item-2 (boot-level native-or-decline), mirrors the terrain island: a
// PERMANENT init failure (spv/pipeline/renderpass/buffer creation past the context +
// shared-GBuffer readiness gates) declines the mech island for the SESSION with ONE
// receipt line (GL owns mechs from frame 0) instead of re-newing + re-init'ing every
// frame. A TRANSIENT readiness gate (VK context / shared GBuffer not born yet at boot)
// keeps the item-9 fail-hard-with-retry (GL draws this frame, retry next). g_mech-
// InitReachedBuild flips to 1 once init passes the readiness gates.
static bool g_mechSessionDeclined  = false;
static int  g_mechInitReachedBuild = 0;

// gpuReadHeavy: a host-written buffer the GPU reads many times per frame (the night pool, walked by every mech fragment) is written
// sequentially so VMA can place it in device-local, host-visible memory instead of system memory read across the bus.
bool make_buffer(MechIsland* s, mc2vk::OwnedBuffer& out, VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible,
                 std::string_view name, int i0 = -1, int i1 = -1, bool gpuReadHeavy = false) {
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size ? size : 16; bci.usage = usage;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = !hostVisible ? 0
              : gpuReadHeavy ? VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                             : VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    if (!out.create(s->allocator, s->device, bci, aci, name, i0, i1)) {
        vlog("buffer creation failed (%.*s).", static_cast<int>(name.size()), name.data()); return false;
    }
    return true;
}

// Write the five per-frame BUFFER bindings (0 UBO / 1 inst / 2 bone / 4 lights / 5 pool) of ONE descriptor set at the frame ring's current buffer, and mark
// the set's parity as needing a rewrite at its next frame (the frame knows which buffer its span came from). binding-3 (albedo) is written separately
// per scheme by bind_scheme_images (3/6/7).
constexpr VkDeviceSize kUboRange = sizeof(MechFrameUBO);

void write_ring_bindings(MechIsland* s, VkDescriptorSet set) {
    if (set == VK_NULL_HANDLE) return;
    const VkBuffer ring = mc2vk::getOrCreateEngineVulkanContext().frameRingBuffer();
    if (ring == VK_NULL_HANDLE) return;
    const VkDeviceSize range = mc2vk::getOrCreateEngineVulkanContext().frameRingRange();
    mc2vk::writeDynamicBinding(s->device, set, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, ring, kUboRange);
    for (const uint32_t b : {1u, 2u, 4u, 5u})
        mc2vk::writeDynamicBinding(s->device, set, b, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, ring, range);
}

// Point the ring bindings of EVERY cached scheme set's [par] slot at the buffers this frame's spans live in. Safe after beginFrame (the parity fence
// retired every [par] consumer); the [1-par] sets, possibly in flight, are untouched and follow at their own frame. A binding already pointing at its
// buffer is skipped.
void sync_ring_bindings(MechIsland* s, uint32_t par, const mc2vk::FrameAlloc& mainSpan, const mc2vk::FrameAlloc& lightSpan) {
    struct Want { uint32_t binding; VkDescriptorType type; VkBuffer buf; VkDeviceSize range; };
    const VkBuffer mainBuf = mainSpan.buffer;
    const VkBuffer lightBuf = lightSpan ? lightSpan.buffer : mainSpan.buffer;
    const VkDeviceSize lightRange = lightSpan ? lightSpan.range : mainSpan.range;
    const Want want[5] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, mainBuf, kUboRange},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, mainBuf, mainSpan.range},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, mainBuf, mainSpan.range},
        {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, lightBuf, lightRange},
        {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, mainBuf, mainSpan.range},
    };
    for (const Want& w : want) {
        if (w.buf == VK_NULL_HANDLE || s->ringBound[par][w.binding] == w.buf) continue;
        for (const auto& e : s->schemes)
            if (e.set[par] != VK_NULL_HANDLE) mc2vk::writeDynamicBinding(s->device, e.set[par], w.binding, w.type, w.buf, w.range);
        s->ringBound[par][w.binding] = w.buf;
    }
    // LOCAL-LIGHTS-2: point binding 8 at the shadow island's live local-light atlas once it exists, and back at the
    // white dummy when it goes (shadow island rebuilt without one: Potato / no atlas), so no set keeps a destroyed view.
    const auto atlas = reinterpret_cast<VkImageView>(mc2_vk_shadow_island_get_local_atlas_view());
    const VkImageView dummy = s->whiteTexture ? s->whiteTexture.view() : VK_NULL_HANDLE;
    if (s->localShadowBound[par] != atlas && s->albedoSamp && (atlas != VK_NULL_HANDLE || dummy != VK_NULL_HANDLE)) {
        const VkDescriptorImageInfo info = atlas != VK_NULL_HANDLE
            ? VkDescriptorImageInfo{s->albedoSamp.get(), atlas, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL}
            : VkDescriptorImageInfo{s->albedoSamp.get(), dummy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        for (const auto& e : s->schemes) {
            if (e.set[par] == VK_NULL_HANDLE) continue;
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = e.set[par]; w.dstBinding = 8; w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &info;
            vkUpdateDescriptorSets(s->device, 1, &w, 0, nullptr);
        }
        s->localShadowBound[par] = atlas;
    }
}

// Point a scheme set's binding-3 (albedo) / 6 (AO) / 7 (normal) at its images + the
// shared sampler (REPEAT/LINEAR; the FS samples all three with textureLod(...,0)).
void bind_scheme_images(MechIsland* s, VkDescriptorSet set, VkImageView albedo,
                        VkImageView ao, VkImageView nrm) {
    if (set == VK_NULL_HANDLE || albedo == VK_NULL_HANDLE || ao == VK_NULL_HANDLE ||
        nrm == VK_NULL_HANDLE || !s->albedoSamp) return;
    VkDescriptorImageInfo info[3]{};
    const VkImageView views[3] = { albedo, ao, nrm };
    const uint32_t    binds[3] = { 3u, 6u, 7u };
    VkWriteDescriptorSet w[3]{};
    for (int k = 0; k < 3; ++k) {
        info[k].sampler = s->albedoSamp.get(); info[k].imageView = views[k];
        info[k].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[k].dstSet = set;
        w[k].dstBinding = binds[k]; w[k].descriptorCount = 1;
        w[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[k].pImageInfo = &info[k];
    }
    vkUpdateDescriptorSets(s->device, 3, w, 0, nullptr);
}

bool make_albedo_sampler(MechIsland* s) {
    if (s->albedoSamp) return true;
    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR; sci.maxLod=VK_LOD_CLAMP_NONE;
    mc2vk::infra::materialSampler(sci);
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    return mc2vk::create(s->albedoSamp, s->device, [&](VkSampler* h) { return vkCreateSampler(s->device, &sci, nullptr, h); });
}

// Allocate a descriptor set pair for a scheme (all 8 bindings), write buffer bindings
// + its three images. Returns false on failure.
bool alloc_scheme_sets(MechIsland* s, VkImageView albedo, VkImageView ao, VkImageView nrm,
                       VkDescriptorSet out[mc2vk::kMaxFramesInFlight]) {
    VkDescriptorSetLayout layouts[mc2vk::kMaxFramesInFlight]; for (auto& layout : layouts) layout = s->dsl.get();
    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = s->pool.get(); dsai.descriptorSetCount = mc2vk::kMaxFramesInFlight; dsai.pSetLayouts = layouts;
    if (vkAllocateDescriptorSets(s->device, &dsai, out) != VK_SUCCESS) return false;
    for (uint32_t par = 0; par < mc2vk::kMaxFramesInFlight; ++par) {
        write_ring_bindings(s, out[par]);
        for (VkBuffer& b : s->ringBound[par]) b = VK_NULL_HANDLE;   // the next frame of this parity rewrites its sets at the buffers its spans live in
        bind_scheme_images(s, out[par], albedo, ao, nrm);
        // LOCAL-LIGHTS-2: binding 8 holds a dummy until the next sync points every set of the parity at the atlas.
        if (albedo != VK_NULL_HANDLE && s->albedoSamp) {
            const VkDescriptorImageInfo info{s->albedoSamp.get(), albedo, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = out[par]; w.dstBinding = 8; w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &info;
            vkUpdateDescriptorSets(s->device, 1, &w, 0, nullptr);
        }
        s->localShadowBound[par] = VK_NULL_HANDLE;
    }
    return true;
}

// Schemes borrow generation identities. The service owns images and retires replaced paint frames.
bool ensure_dummy_scheme(MechIsland* s) {
    if (!make_albedo_sampler(s)) return false;
    if (s->whiteTexture && !RenderCore::textureService().isLive(s->whiteTexture.get())) s->whiteTexture={};
    if (!s->whiteTexture) {
        const uint8_t white[4]={255,255,255,255};
        s->whiteTexture=mc2vk::getOrCreateEngineVulkanContext().textures().acquireLegacy(1,1,VK_FORMAT_R8G8B8A8_UNORM,white,4);
        if (!s->whiteTexture) return false;
    }
    if (!s->schemes.empty()) return true;
    MechIsland::SchemeEntry entry;
    const auto view=s->whiteTexture.view();
    if (!alloc_scheme_sets(s,view,view,view,entry.set)) return false;
    s->schemes.push_back(entry); return true;
}
VkImageView get_tex_view(MechIsland* s,uint64_t id) {
    if (!RenderCore::isUiTextureId(id)) return s->whiteTexture.view();
    return mc2vk::getOrCreateEngineVulkanContext().textures().view(RenderCore::uiTextureHandle(id));
}
VkDescriptorSet get_scheme_set(MechIsland* s,uint64_t albedo,uint64_t ao,uint64_t nrm,uint32_t par) {
    auto& ctx=mc2vk::getOrCreateEngineVulkanContext();
    const uint64_t frame=ctx.tracker().frameNo();
    MechIsland::SchemeEntry* entry=nullptr;
    for (auto& candidate:s->schemes)
        if (candidate.albedo==albedo && candidate.ao==ao && candidate.nrm==nrm) { entry=&candidate;break; }
    const std::array<VkImageView,3> views{get_tex_view(s,albedo),get_tex_view(s,ao),get_tex_view(s,nrm)};
    if (!entry) {
        if (s->schemes.size()<s->kMaxSchemes) {
            MechIsland::SchemeEntry fresh;
            if (!alloc_scheme_sets(s,views[0],views[1],views[2],fresh.set)) return s->schemes[0].set[par];
            s->schemes.push_back(fresh); entry=&s->schemes.back();
        } else {
            auto victim=std::min_element(s->schemes.begin()+1,s->schemes.end(),[](const auto& a,const auto& b){return a.lastUsedFrame<b.lastUsedFrame;});
            if (victim==s->schemes.end() || victim->lastUsedFrame==frame) return s->schemes[0].set[par];
            entry=&*victim; for (auto& bound : entry->bound) bound = {};
        }
        entry->albedo=albedo; entry->ao=ao; entry->nrm=nrm;
    }
    // Called after this parity's fence. The other parity retains its submitted descriptors.
    if (entry->bound[par]!=views) {
        bind_scheme_images(s,entry->set[par],views[0],views[1],views[2]); entry->bound[par]=views;
    }
    entry->lastUsedFrame=frame;
    if (albedo) s->materialReady=true;
    return entry->set[par];
}

bool init_island(MechIsland* s, int w, int h) {
    s->w = w; s->h = h;
    ++g_health.attempted;
    g_mechInitReachedBuild = 0;  // reset; flips to 1 after readiness gates pass

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
    g_health.vulkanAvailable = 1;
    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(s->phys, &props);
        std::strncpy(g_health.deviceName, props.deviceName, sizeof(g_health.deviceName) - 1);
    }

    s->wantMaterial = true;
    s->wantLighting = true;

    // Fetch the SHARED scene color/normal/depth exportable images the GL scene renders
    // terrain into. Fail-soft: any missing => GL fallback (the GL mech flush runs).
    s->colorRes  = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_COLOR0);
    s->normalRes = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_NORMAL);
    s->depthRes  = mc2SharedSceneDepthGetResource();
    if (!s->colorRes || !s->normalRes || !s->depthRes) {
        setFallback("shared_gbuffer_not_ready"); return false;
    }
    // VK-RUNG8 item-4/item-5: direct-chain-native only. The shared GBuffer is
    // VK-only and left Vulkan-owned by the terrain first-owner, so the mech island
    // issues no GL<->VK semaphore handoff and has no interop/vk2gl semaphore state.
    g_health.interopActive = 0;
    g_health.vk2glSemActive = 0;
    // Readiness gates (context + shared GBuffer) passed: any failure past this point is a
    // PERMANENT build failure -> the island declines for the session (item-2).
    g_mechInitReachedBuild = 1;

    // Command pool + 2 command buffers (parity).
    {
        VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = s->gfxFamily;
        if (!mc2vk::create(s->cpool, s->device, [&](VkCommandPool* h) { return vkCreateCommandPool(s->device, &pci, nullptr, h); })) { vlog("cmd pool failed."); return false; }
        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = s->cpool.get(); cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = mc2vk::kMaxFramesInFlight;
        if (vkAllocateCommandBuffers(s->device, &cai, s->cbuf) != VK_SUCCESS) { vlog("cmd bufs failed."); return false; }
    }

    // VK-DRAW-PERF-1: 3-stamp GPU timer (pass-start / draws-start / pass-end).
    // No-op unless MC2_VK_GPU_TIMESTAMPS is set; fail-soft if timestamps unsupported.
    if (mc2vkts::timestampsEnabled()) {
        if (s->gpuTimer.init(s->device, s->phys, s->gfxFamily, 3))
            vlog("GPU timestamps ON (period=%.2f ns): pass segments = setup / mech draws.",
                 s->gpuTimer.periodNs);
        else
            vlog("GPU timestamps requested but unsupported on this queue (validBits=0 or period<=0).");
    }

    // The per-scheme descriptor sets (incl. the dummy white schemes[0]) are created in
    // the descriptor-pool block below (needs s->pool). binding3 is always valid there.

    // Render pass: 2 color LOAD (preserve GL terrain in shared color/normal) + shared
    // depth LOAD (D32S8, GEQUAL test+write). NO clear. Same shape as the prop island Tier-1.
    {
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
        fci.width = static_cast<uint32_t>(w); fci.height = static_cast<uint32_t>(h); fci.layers = 1;
        if (!mc2vk::create(s->fb, s->device, [&](VkFramebuffer* h) { return vkCreateFramebuffer(s->device, &fci, nullptr, h); })) { setFallback("framebuffer_failed"); return false; }
    }

    // Descriptor layout: 0=UBO(VS/FS), 1=instance SSBO(VS), 2=bone SSBO(VS), 3=albedo(FS).
    {
        VkDescriptorSetLayoutBinding b[9]{};
        b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
        b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        b[2].binding = 2; b[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
        b[2].descriptorCount = 1; b[2].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        b[3].binding = 3; b[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[3].descriptorCount = 1; b[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        b[4].binding = 4; b[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;   // LightsData
        b[4].descriptorCount = 1; b[4].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        // VK-PARITY-MECH-1: 5 = object light-pool SSBO, 6 = AO map, 7 = normal map (FS).
        b[5].binding = 5; b[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
        b[5].descriptorCount = 1; b[5].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        b[6].binding = 6; b[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[6].descriptorCount = 1; b[6].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        b[7].binding = 7; b[7].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[7].descriptorCount = 1; b[7].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        // LOCAL-LIGHTS-2: 8 = local-light shadow atlas (FS, plain sampler2D + gather).
        b[8].binding = 8; b[8].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[8].descriptorCount = 1; b[8].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.bindingCount = 9; lci.pBindings = b;
        if (!mc2vk::create(s->dsl, s->device, [&](VkDescriptorSetLayout* h) { return vkCreateDescriptorSetLayout(s->device, &lci, nullptr, h); })) { vlog("dsl failed."); return false; }
        // Pool sized for kMaxSchemes descriptor sets (one per distinct paint scheme):
        // each set = 1 UBO + 4 SSBO (inst/bone/lights/pool) + 3 samplers (albedo/AO/normal).
        const uint32_t N = MechIsland::kMaxSchemes;
        VkDescriptorPoolSize sizes[3]{};
        sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; sizes[0].descriptorCount = 1 * mc2vk::kMaxFramesInFlight * N;
        sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC; sizes[1].descriptorCount = 4 * mc2vk::kMaxFramesInFlight * N;  // inst+bone+lights+pool, two parities
        sizes[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; sizes[2].descriptorCount = 4 * mc2vk::kMaxFramesInFlight * N;  // albedo+AO+normal+local shadow, two parities
        VkDescriptorPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.maxSets = mc2vk::kMaxFramesInFlight * N; pci.poolSizeCount = 3; pci.pPoolSizes = sizes;
        if (!mc2vk::create(s->pool, s->device, [&](VkDescriptorPool* h) { return vkCreateDescriptorPool(s->device, &pci, nullptr, h); })) { vlog("pool failed."); return false; }
        // Always-present dummy white scheme (schemes[0]) = binding-3 fallback for runs
        // with no slot-0 paint texture (and the material-OFF flat-fill path).
        if (!ensure_dummy_scheme(s)) { setFallback("dummy_scheme_failed"); return false; }
    }

    // Pipeline: mech 48B vertex stream, instanced, GEQUAL reverse-Z test+write, cull NONE,
    // 2 GBuffer outputs, no blend (opaque in-place overwrite).
    {
        s->vert = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir() + "mech_tier1.vert.spv"));
        s->frag = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir() + "mech_tier1.frag.spv"));
        s->depthVert = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir()+"mech_depth.vert.spv"));
        s->depthFrag = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir()+"mech_depth.frag.spv"));
        if (!s->vert || !s->frag || !s->depthVert || !s->depthFrag) { setFallback("spv_load_failed"); return false; }

        // VK-PARITY-MECH-1: per-run materialFlags (ALPHA_TEST discard, mech.frag:177).
        VkPushConstantRange pcr{};
        pcr.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; pcr.offset = 0; pcr.size = sizeof(MechRunPC);
        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        if (!s->groundLighting.init(s->device)) return false;
        const VkDescriptorSetLayout layouts[]{s->dsl.get(), s->groundLighting.layout()};
        plci.setLayoutCount = 2; plci.pSetLayouts = layouts;
        plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
        if (!mc2vk::create(s->playout, s->device, [&](VkPipelineLayout* h) { return vkCreatePipelineLayout(s->device, &plci, nullptr, h); })) { vlog("playout failed."); return false; }

        // Mirror GpuMechVertex (48B): loc0 pos vec3@0, loc1 normal vec3@12, loc2 uv vec2@24,
        // loc3 boneIndices uvec4 (u8x4)@32, loc4 boneWeights vec4 (u8x4 normalized)@36,
        // loc5 tangentOct (s16x2 norm)@40, loc6 aRGBLight uint@44.
        VkVertexInputBindingDescription vib{};
        vib.binding = 0; vib.stride = mc2_mech_vertex_stride();  // pipeline is baked before geometry upload (s->vertexStride may be 0)
        vib.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        VkVertexInputAttributeDescription via[7]{};
        via[0].location = 0; via[0].binding = 0; via[0].format = VK_FORMAT_R32G32B32_SFLOAT; via[0].offset = 0;
        via[1].location = 1; via[1].binding = 0; via[1].format = VK_FORMAT_R32G32B32_SFLOAT; via[1].offset = 12;
        via[2].location = 2; via[2].binding = 0; via[2].format = VK_FORMAT_R32G32_SFLOAT;    via[2].offset = 24;
        via[3].location = 3; via[3].binding = 0; via[3].format = VK_FORMAT_R8G8B8A8_UINT;    via[3].offset = 32; // uvec4
        via[4].location = 4; via[4].binding = 0; via[4].format = VK_FORMAT_R8G8B8A8_UNORM;   via[4].offset = 36; // vec4 [0,1]
        via[5].location = 5; via[5].binding = 0; via[5].format = VK_FORMAT_R16G16_SNORM;     via[5].offset = 40; // vec2 [-1,1]
        via[6].location = 6; via[6].binding = 0; via[6].format = VK_FORMAT_R32_UINT;         via[6].offset = 44;
        VkPipelineVertexInputStateCreateInfo vin{};
        vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vin.vertexBindingDescriptionCount = 1; vin.pVertexBindingDescriptions = &vib;
        vin.vertexAttributeDescriptionCount = 7; vin.pVertexAttributeDescriptions = via;

        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo vps{};
        vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vps.viewportCount = 1; vps.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;   // GL mech color pass does not hard-cull
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo ds{};
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;   // reverse-Z, matches GL mech opaque

        VkPipelineColorBlendAttachmentState cba[2]{};
        for (auto & cbaElem : cba) {
            cbaElem.blendEnable = VK_FALSE;
            cbaElem.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
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
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = s->vert.get(); stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = s->frag.get(); stages[1].pName = "main";
        VkGraphicsPipelineCreateInfo gp{};
        gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gp.stageCount = 2; gp.pStages = stages;
        gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vps; gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms; gp.pDepthStencilState = &ds; gp.pColorBlendState = &cb;
        gp.pDynamicState = &dsci;
        gp.layout = s->playout.get(); gp.renderPass = s->rpass.get(); gp.subpass = 0;
        // The depth shader consumes only position, UV and skinning attributes.
        const VkVertexInputAttributeDescription depthVia[]{via[0], via[2], via[3], via[4]};
        vin.vertexAttributeDescriptionCount = 4; vin.pVertexAttributeDescriptions = depthVia;
        stages[0].module=s->depthVert.get();stages[1].module=s->depthFrag.get();
        for(auto& attachment:cba)attachment.colorWriteMask=0;
        if (!mc2vk::create(s->depthPipe,s->device,[&](VkPipeline* h) { return mc2vk::infra::graphics(s->device,VK_NULL_HANDLE,1,&gp,nullptr,h); })) {
            setFallback("depth_pipeline_failed");return false;
        }
        mc2vk::nameObject(s->device,s->depthPipe.get(),"mech.depth");
        vin.vertexAttributeDescriptionCount = 7; vin.pVertexAttributeDescriptions = via;
        stages[0].module=s->vert.get();stages[1].module=s->frag.get();
        for(auto& attachment:cba)attachment.colorWriteMask=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
        ds.depthWriteEnable=VK_FALSE;
        ds.depthCompareOp=VK_COMPARE_OP_EQUAL;
        if (!mc2vk::create(s->pipe, s->device, [&](VkPipeline* h) { return mc2vk::infra::graphics(s->device, VK_NULL_HANDLE, 1, &gp, nullptr, h); })) {
            setFallback("pipeline_failed"); return false;
        }
        mc2vk::nameObject(s->device, s->pipe.get(), "mech.pipe");

        // Mech-lab panel variants: same layout/state against the preview render pass (RGBA8 color + normal, D32 depth).
        s->previewReady = false;
        s->previewFrag = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir() + "mech_preview.frag.spv"));
        if (s->previewFrag) {
            VkAttachmentDescription patt[3]{};
            for (auto& a : patt) {
                a.samples = VK_SAMPLE_COUNT_1_BIT;
                a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            }
            // Color: LOAD the legacy preview pass's output (it leaves the target SHADER_READ_ONLY for the UI).
            patt[0].format = VK_FORMAT_R8G8B8A8_UNORM;
            patt[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; patt[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            patt[0].initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; patt[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            // The normal output is unused by the panel; depth is private to this pass (reverse-Z clear 0).
            patt[1].format = VK_FORMAT_R8G8B8A8_UNORM;
            patt[1].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; patt[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            patt[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; patt[1].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            patt[2].format = VK_FORMAT_D32_SFLOAT;
            patt[2].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; patt[2].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            patt[2].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; patt[2].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            VkAttachmentReference pColor[2] = {{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
            VkAttachmentReference pDepth{2, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
            VkSubpassDescription psub{};
            psub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            psub.colorAttachmentCount = 2; psub.pColorAttachments = pColor; psub.pDepthStencilAttachment = &pDepth;
            VkSubpassDependency pdeps[2]{};
            pdeps[0].srcSubpass = VK_SUBPASS_EXTERNAL; pdeps[0].dstSubpass = 0;
            pdeps[0].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                     VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            pdeps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            pdeps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
            pdeps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                     VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            pdeps[1].srcSubpass = 0; pdeps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
            pdeps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            pdeps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            pdeps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT; pdeps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            VkRenderPassCreateInfo prci{};
            prci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
            prci.attachmentCount = 3; prci.pAttachments = patt;
            prci.subpassCount = 1; prci.pSubpasses = &psub;
            prci.dependencyCount = 2; prci.pDependencies = pdeps;
            bool ok = mc2vk::create(s->previewRpass, s->device, [&](VkRenderPass* h) { return vkCreateRenderPass(s->device, &prci, nullptr, h); });
            gp.renderPass = s->previewRpass.get();
            // Color variant first (current state = color/EQUAL), then the depth prepass variant.
            stages[1].module = s->previewFrag.get();
            ok = ok && mc2vk::create(s->previewPipe, s->device, [&](VkPipeline* h) { return mc2vk::infra::graphics(s->device, VK_NULL_HANDLE, 1, &gp, nullptr, h); });
            vin.vertexAttributeDescriptionCount = 4; vin.pVertexAttributeDescriptions = depthVia;
            stages[0].module = s->depthVert.get(); stages[1].module = s->depthFrag.get();
            for (auto& attachment : cba) attachment.colorWriteMask = 0;
            ds.depthWriteEnable = VK_TRUE; ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
            ok = ok && mc2vk::create(s->previewDepthPipe, s->device, [&](VkPipeline* h) { return mc2vk::infra::graphics(s->device, VK_NULL_HANDLE, 1, &gp, nullptr, h); });
            // Panel backdrop: fullscreen triangle over the panel viewport, alpha-blended into colour 0 only, no depth.
            s->backdropVert = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir() + "mech_backdrop.vert.spv"));
            s->backdropFrag = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir() + "mech_backdrop.frag.spv"));
            if (s->backdropVert && s->backdropFrag) {
                stages[0].module = s->backdropVert.get(); stages[1].module = s->backdropFrag.get();
                vin.vertexBindingDescriptionCount = 0; vin.vertexAttributeDescriptionCount = 0;
                ds.depthTestEnable = VK_FALSE; ds.depthWriteEnable = VK_FALSE;
                cba[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
                cba[0].blendEnable = VK_TRUE;
                cba[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA; cba[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                cba[0].colorBlendOp = VK_BLEND_OP_ADD;
                cba[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; cba[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                cba[0].alphaBlendOp = VK_BLEND_OP_ADD;
                cba[1].colorWriteMask = 0;
                mc2vk::create(s->backdropPipe, s->device, [&](VkPipeline* h) { return mc2vk::infra::graphics(s->device, VK_NULL_HANDLE, 1, &gp, nullptr, h); });
                if (s->backdropPipe) mc2vk::nameObject(s->device, s->backdropPipe.get(), "mech.preview.backdrop");
            }
            s->previewReady = ok;
            if (ok) { mc2vk::nameObject(s->device, s->previewPipe.get(), "mech.preview"); mc2vk::nameObject(s->device, s->previewDepthPipe.get(), "mech.preview.depth"); }
        }
        if (!s->previewReady) vlog("mech-lab preview variant unavailable (mech_preview.frag.spv / pipeline) -- lab keeps the CPU mech draw.");
    }

    s->inited = true;
    setFallback("");
    vlog("island initialized %dx%d (borrowed shared device; skinned mech co-write into "
         "shared color(MRT0)+normal(MRT1)+depth; GEQUAL reverse-Z; material=%d lighting=%d).",
         w, h, s->wantMaterial ? 1 : 0, s->wantLighting ? 1 : 0);
    return true;
}

// Copy the shared mech VBO/IBO into VK-owned buffers ONCE per map.
bool upload_geometry_once(MechIsland* s) {
    Mc2MechGeomFeed feed{};
    const bool haveFeed = mc2_mech_island_get_geometry_feed(&feed) != 0;
    if (s->geomUploaded && (!haveFeed || feed.geomGen == s->geomGen)) return true;
    if (!haveFeed) return false;
    // A null source means a normals rebuild raced the accessor: decline before the waitIdle and the buffer destroy, so the old
    // buffers stay and no device stall is taken for a retry.
    if (feed.vboBytes == 0 || feed.iboBytes == 0 || !feed.vboCpu || !feed.iboCpu) return false;
    if (s->geomUploaded) {
        // A new map (or a normals rebuild) replaced the batcher's vertex and index sets: the draw runs index into the NEW
        // set, so a buffer kept from the previous map is read past its end (VUID-vkCmdDrawIndexed-robustBufferAccess2-08798).
        // Frames in flight may still read the old buffers.
        mc2vk::getOrCreateEngineVulkanContext().waitIdle("mech_geometry_regen");
        s->vbo.reset();
        s->ibo.reset();
        s->geomUploaded = false;
    }
    // The bytes come from the batcher's CPU copy (the exact bytes the GL buffers were created from).
    if (feed.vertexStride != mc2_mech_vertex_stride())
        vlog("ERROR: mech feed vertexStride %u != pipeline stride %u (mech geometry will explode).", feed.vertexStride, mc2_mech_vertex_stride());
    s->vertexStride = feed.vertexStride;

    // A failed copy leaves both buffers empty, so the next frame's retry starts from nothing instead of drawing unfilled buffers.
    if (!s->vbo.createFilled(s->allocator, s->device, feed.vboCpu, feed.vboBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, "mech.vbo") ||
        !s->ibo.createFilled(s->allocator, s->device, feed.iboCpu, feed.iboBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, "mech.ibo")) {
        static bool s_logged = false;
        if (!s_logged) { s_logged = true; vlog("mech geometry upload failed (buffer creation or map); retrying each frame, logged once."); }
        s->vbo.reset();
        s->ibo.reset();
        return false;
    }
    s->geomGen = feed.geomGen;
    g_health.geomVerts = static_cast<unsigned long>((feed.vboBytes / (feed.vertexStride ? feed.vertexStride : 1)));
    s->geomUploaded = true;
    vlog("mech geometry uploaded once: vbo=%llu B ibo=%llu B stride=%u source=cpu-native.",
         static_cast<unsigned long long>(feed.vboBytes), static_cast<unsigned long long>(feed.iboBytes), feed.vertexStride);
    return true;
}

} // namespace

// ===========================================================================
// PUBLIC ENTRY POINTS.
// ===========================================================================

extern "C" int mc2_vk_mech_island_enabled() {
    return 1;
}

extern "C" int mc2_vk_mech_island_prewarm(int w,int h) {
    auto& ctx=mc2vk::getOrCreateEngineVulkanContext();
    if(!ctx.valid() || w<=0 || h<=0) return 0;
    auto& slot=ctx.islandSlot(mc2vk::IslandSlot::Mech);
    if(!slot) {
        auto fresh=std::make_unique<MechIsland>();
        if(!init_island(fresh.get(),w,h)) { ctx.waitIdle("mech_prewarm_failure"); return 0; }
        slot=std::move(fresh);
    }
    auto* s=static_cast<MechIsland*>(slot.get());
    // Empty mech geometry is valid on maps without mechs; the generation guard retries later.
    upload_geometry_once(s);
    return ensure_dummy_scheme(s) ? 1 : 0;
}

// GPU-FRONTIER-1: pure command recording for the mech pass (depth pre-pass + colour runs). Staging, descriptor-set resolution and the
// dynamic offsets are prepared by mc2_vk_mech_island_run on the main thread; this touches only the command buffer and the island timers.
static bool record_mech(MechIsland* s, VkCommandBuffer cb, uint32_t par, int w, int h, const Mc2MechFrameFeed& feed,
                        const std::vector<VkDescriptorSet>& runSets, const uint32_t (&dynOffsets)[5],
                        const Mc2GroundLightingFrame* ground, int poolN, unsigned long& runsOut) {
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) return false;
    vk_render_pass_timer::Begin(cb, gos_render_pass_timer::Pass_Mechs);

    // VK-DRAW-PERF-1: reset the query pool (outside the render pass) + stamp pass-start.
    s->gpuTimer.reset(cb);
    s->gpuTimer.write(cb, 0);

    VkRenderPassBeginInfo rbi{};
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = s->rpass.get(); rbi.framebuffer = s->fb.get();
    rbi.renderArea = {{0, 0}, {static_cast<uint32_t>(w), static_cast<uint32_t>(h)}};
    rbi.clearValueCount = 0; rbi.pClearValues = nullptr;   // all LOAD
    MC2_VK_GPU_SCOPE(gpuScope, cb, "VKIsl.Mech");
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {static_cast<uint32_t>(w), static_cast<uint32_t>(h)}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &scissor);

    VkDeviceSize vboOff = 0;
    const VkBuffer vertexBuffer = s->vbo.get();
    vkCmdBindVertexBuffers(cb, 0, 1, &vertexBuffer, &vboOff);
    vkCmdBindIndexBuffer(cb, s->ibo.get(), 0, VK_INDEX_TYPE_UINT32);

    // VK-DRAW-PERF-1: stamp draws-start (gap 0->1 = setup + binds).
    s->gpuTimer.write(cb, 1);

    // Establish final visible depth with cheap skinning + the identical cutout test.
    // The color pipeline is read-only EQUAL with early fragment tests: hidden
    // triangles never execute the local-light loop, and discard cannot write depth.
    mc2vk::infra::bind(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,s->depthPipe.get());
    VkDescriptorSet depthSet=VK_NULL_HANDLE;
    for(uint32_t r=0;r<feed.runCount;++r) {
        const auto& run=feed.runs[r];if(!run.indexCount || !run.instanceCount)continue;
        const auto set=runSets[r];
        if(set!=depthSet) {
            vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,s->playout.get(),0,1,&set,5,dynOffsets);
            depthSet=set;
        }
        const MechRunPC pc{run.materialFlags,0,0};
        vkCmdPushConstants(cb,s->playout.get(),VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(pc),&pc);
        mc2::battlebench::add(mc2::battlebench::DrawMech), vkCmdDrawIndexed(cb,run.indexCount,run.instanceCount,run.firstIndex,run.baseVertex,run.instanceBase);
    }
    mc2vk::infra::bind(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,s->pipe.get());
    const auto groundSet = s->groundLighting.set(par);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->playout.get(), 1, 1, &groundSet, 4, ground->offsets.data());

    // One indexed instanced draw per run. firstInstance = run.instanceBase so the VS's
    // gl_InstanceIndex indexes the instance SSBO snapshot exactly as the GL VS's
    // u_instanceBase + gl_InstanceID did. Per run: bind the descriptor set for this run's
    // paint scheme (per-actor cache) so the right albedo (binding 3) is sampled.
    VkDescriptorSet boundSet = VK_NULL_HANDLE;
    unsigned long runs = 0;
    for (uint32_t r = 0; r < feed.runCount; ++r) {
        const Mc2MechDrawRun& run = feed.runs[r];
        if (run.indexCount == 0 || run.instanceCount == 0) continue;
        const VkDescriptorSet set = runSets[r];
        if (set != boundSet) {
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->playout.get(), 0, 1, &set, 5, dynOffsets);
            boundSet = set;
        }
        // VK-PARITY-MECH-1: per-run ALPHA_TEST flag (mech.frag u_materialFlags).
        const MechRunPC pc{run.materialFlags, 0, uint32_t(poolN)};
        vkCmdPushConstants(cb, s->playout.get(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
        mc2::battlebench::add(mc2::battlebench::DrawMech), vkCmdDrawIndexed(cb, run.indexCount, run.instanceCount, run.firstIndex, run.baseVertex, run.instanceBase);
        ++runs;
    }
    // The retained feed counts actor/node pairs once, across material packets and idle-frame reuse.
    if (runs && mc2::battlebench::output) mc2::battlebench::nativeMechLeaves += feed.leafCount;

    vkCmdEndRenderPass(cb);
    gpuScope.end();
    // VK-DRAW-PERF-1: stamp pass-end (gap 1->2 = mech instanced draws).
    s->gpuTimer.write(cb, 2);
    vk_render_pass_timer::End(cb, gos_render_pass_timer::Pass_Mechs);
    if (vkEndCommandBuffer(cb) != VK_SUCCESS) return false;
    runsOut = runs;
    return true;
}

extern "C" int mc2_vk_mech_island_run(int w, int h) {
    ZoneScopedN("VK-mech-island");   // VK-FLIP-FIELD-REPORT-1 coarse region zone
    if (w <= 0 || h <= 0) return 0;

    using clk = std::chrono::steady_clock;
    auto usSince = [](clk::time_point t0){ return std::chrono::duration<double, std::micro>(clk::now() - t0).count(); };
    const clk::time_point tTotal0 = clk::now();
    const bool perf = mechPerfGateRequested();
    double us_fence_wait = 0.0;

    Mc2MechFrameFeed feed{};
    if (!mc2_mech_island_get_frame_feed(&feed)) { setFallback("no_frame_feed"); return 0; }
    if (!feed.instances64 || feed.instanceCount == 0 || !feed.runs || feed.runCount == 0) {
        setFallback("no_mech_instances"); return 0;
    }

    // VK-MECH-CAPTURE-DETERMINISM-1: frame-stamped cull-parity witness. feed.instanceCount
    // is s_vkMechInstSnapshot.size() == the GL batcher's totalInstances for THIS frame
    // (assigned in the same flush, gos_mech_batcher.cpp:2796) so it is directly comparable
    // to GL's [MECHBATCHER v1] event=summary instances=. Gated by the same stats env so
    // controls stay clean (no default-on spam). extern frame counter (tgl.cpp).
    {
        static const bool s_cullWitness = mc2::gate::get<"MC2_MECH_BATCHER_STATS">();
        if (s_cullWitness) {
            // Ordinal = per-drawn-frame (usedVulkan+1); the GL batcher builds one feed
            // per frame the island draws, so the ordered sequence of vk_instances must
            // equal the batcher's instances= sequence (equal every frame by construction).
            std::fprintf(stderr, "[VK_MECH_ISLAND] event=cull_witness seq=%lu vk_instances=%u runs=%u\n",
                         g_health.usedVulkan + 1, feed.instanceCount, feed.runCount);
        }
    }

    // Item-2 boot-level native-or-decline: a prior PERMANENT init failure declined this
    // island for the whole session -> GL owns mechs, no per-frame re-init churn.
    if (g_mechSessionDeclined) return 0;
    mc2vk::VulkanContext& slotCtx = mc2vk::getOrCreateEngineVulkanContext();
    if (const MechIsland* parked = parkedIsland(); parked && (parked->w != w || parked->h != h)) slotCtx.retireIsland(mc2vk::IslandSlot::Mech);
    if (!islandSlot()) {
        auto fresh = std::make_unique<MechIsland>();
        if (!init_island(fresh.get(), w, h)) {
            const bool permanent = (g_mechInitReachedBuild != 0);
            slotCtx.waitIdle("mech_init_failure");   // objects made before the failure may have been submitted
            if (permanent) {
                // Build-phase failure won't recover this session: latch decline + ONE
                // receipt line (vs. re-init'ing every frame). GL renders mechs from frame 0.
                g_mechSessionDeclined = true;
                vlog("[VK_MECH_ISLAND] boot-level decline: permanent init failure (%s) -- "
                     "GL owns mechs for this session (no per-frame retry).",
                     g_health.fallbackReason);
            } else {
                // Transient readiness gate (VK context / shared GBuffer not born yet):
                // keep item-9 fail-hard-with-retry -- GL draws this frame, retry next.
                static bool s_transientLogged = false;
                if (!s_transientLogged) {
                    vlog("init not ready (%s) — GL fallback (will retry).", g_health.fallbackReason);
                    s_transientLogged = true;
                }
            }
            return 0;
        }
        islandSlot() = std::move(fresh);
    }
    MechIsland* s = parkedIsland();
    if (s->disabled || !s->inited) return 0;

    if (!upload_geometry_once(s)) { setFallback("geometry_not_ready"); return 0; }
    if (!ensure_dummy_scheme(s)) return 0;

    const unsigned long f0b = g_forbidden.glGetTexImage;
    const unsigned long f0c = g_forbidden.glReadPixels;

    // Per-frame data goes into the engine frame ring BEFORE beginFrame: a ring failure after beginFrame would reset the acquired shared fence and could
    // strand it unsignalled. The ring slot belongs to the frame, not to the island's parity, so staging here races nothing.
    mc2vk::VulkanContext& rctx = mc2vk::getOrCreateEngineVulkanContext();
    const VkDeviceSize ringAlign = rctx.frameAlignment();
    const VkDeviceSize instBytes = static_cast<VkDeviceSize>(feed.instanceCount) * 64ull;
    const VkDeviceSize boneBytes = static_cast<VkDeviceSize>((feed.boneCount ? feed.boneCount : 1)) * 64ull;
    const bool nativeLight = feed.lights.data && feed.lights.bytes >= 3600ull;
    int poolN = (feed.poolLights48 && feed.numPoolLights > 0) ? feed.numPoolLights : 0;
    if (poolN > (int)kMaxPoolLights) poolN = static_cast<int>(kMaxPoolLights);
    const size_t poolBytes = std::max(poolN, 1) * sizeof(gos_terrain_lighting::GpuTerrainLight);
    const void* poolUpload = feed.poolLights48;
    const VkDeviceSize instOff = mc2vk::alignUp(sizeof(MechFrameUBO), ringAlign);
    const VkDeviceSize boneOff = mc2vk::alignUp(instOff + (instBytes ? instBytes : 64ull), ringAlign);
    const VkDeviceSize poolOff = mc2vk::alignUp(boneOff + boneBytes, ringAlign);
    const mc2vk::FrameAlloc mainSpan = rctx.frameAlloc(poolOff + poolBytes);
    if (!mainSpan) { setFallback("frame_ring_alloc_failed"); s->disabled = true; return 0; }
    uint8_t* const mainPtr = static_cast<uint8_t*>(mainSpan.ptr);

    // Private recording resources follow the engine frame slot. Shared image
    // fences may already name terrain from this frame; only this slot's prior
    // mech submission must retire before its commands/descriptors change.
    const clk::time_point tFence0 = clk::now();
    const uint32_t par = rctx.frameContext().slot;
    { ZoneScopedN("VKIsl.Mech.FenceWait"); mc2::battlebench::WaitScope gpuWait1(mc2::battlebench::WaitGpuMainNs);
      if (!rctx.wait(s->submitted[par], 5000000000ull, "mech submission slot")) {
          setFallback("submission_slot_timeout"); s->disabled = true; return 0;
      } }
    us_fence_wait = usSince(tFence0);
    const auto ground = mc2_vk_ground_lighting_frame();
    if (!ground) return 0;
    s->groundLighting.copy(s->device, par, *ground);

    const clk::time_point tStage0 = clk::now();
    // Upload instance + bone snapshots into the frame span.
    { ZoneScopedN("VK-mech-instance-bone-upload");   // VK-FLIP-FIELD-REPORT-1: per-frame skinning feed staging
      if (instBytes) std::memcpy(mainPtr + instOff, feed.instances64, static_cast<size_t>(instBytes));
      if (feed.boneCount) std::memcpy(mainPtr + boneOff, feed.bones64, static_cast<size_t>((feed.boneCount * 64ull))); }
    g_health.instRecords = feed.instanceCount;
    g_health.boneRecords = feed.boneCount;

    // VK-MECH-MATERIAL-LIGHTING-1: per-frame copy of the CPU LightsData array (the byte source of the GL SSBO upload) into a frame-ring span (binding 4)
    // so calc_light reads the same ObjectLights[] the GL mech VS does. If the CPU array is not ready this frame, lighting is simply not marked ready
    // (the shader falls back to flat-lit via u.lightingReady=0). Binding 4 still names a valid span (the main one) then.
    s->lightingReady = false;
    mc2vk::FrameAlloc lightSpan;
    if (s->wantLighting && nativeLight) {
        // VK-LIGHTFEED-DEDUP-1: the shared once-per-frame LightsData span first (ONE copy total across the mech + prop islands; the copy is zoned
        // VKIsl.Shared.LightFeed inside acquire). Killswitch MC2_VK_LIGHTFEED_DEDUP=0 or any acquire fail-soft (mid-frame light-count growth, ring
        // failure) copies into this island's own span below (zone VKIsl.Mech.LightFeed).
        lightSpan = mc2vkSharedLightFeedAcquire(feed.lights);
        if (!lightSpan) {
            ZoneScopedN("VKIsl.Mech.LightFeed");   // VK-ISLAND-TRACY-1: per-frame LightsData CPU->VK copy (own-span path)
            lightSpan = rctx.frameAlloc(static_cast<VkDeviceSize>(feed.lights.bytes));
            if (lightSpan) std::memcpy(lightSpan.ptr, feed.lights.data, static_cast<size_t>(feed.lights.bytes));
        }
        s->lightingReady = static_cast<bool>(lightSpan);
    }

    // Resolve every run before uploading materialReady or synchronizing dynamic buffers.
    // No descriptor allocation or cache eviction may happen inside the recorded pass.
    const VkDescriptorSet dummySet = get_scheme_set(s,0,0,0,par);
    std::vector<VkDescriptorSet> runSets(feed.runCount, dummySet);
    s->materialReady = false;
    for (uint32_t r=0; s->wantMaterial && r<feed.runCount; ++r) {
        const auto& run=feed.runs[r];
        if (!run.indexCount || !run.instanceCount) continue;
        const auto set=get_scheme_set(s,run.slot0Tex,run.aoTex,run.normalTex,par);
        if (set) runSets[r]=set;
    }

    // Upload UBO.
    { MechFrameUBO u{};
      std::memcpy(u.worldToClipGL, feed.worldToClipGL, sizeof(u.worldToClipGL));
      u.objectDepthBias = feed.objectDepthBias;
      u.materialReady = (s->wantMaterial && s->materialReady) ? 1 : 0;
      u.lightingReady = s->lightingReady ? 1 : 0;
      static const int mechFill = mc2::gate::get<"MC2_MECH_FILL">();   // ships ON at 2; 0 = original look for A/B
      // The shader applies the swapped sun axis to mech diffuse itself when the fill is on; ground vehicles keep the option's value.
      u.sunAxisMc2Fix = mc2::options::current().sunAxisFix ? 1 : 0;   // same option the GL mech flush reads
      u.fogColorRGB[0] = feed.fogColorRGB[0];
      u.fogColorRGB[1] = feed.fogColorRGB[1];
      u.fogColorRGB[2] = feed.fogColorRGB[2];
      u.fogColorRGB[3] = 0.0f;
      // VK-PARITY-MECH-1: same producers as the GL mech flush uniforms.
      for (int k = 0; k < 4; ++k) u.cameraWorldPos[k] = feed.cameraWorldPos[k];
      u.shadeA[0] = feed.ambientStrength;  u.shadeA[1] = feed.specularStrength;
      u.shadeA[2] = feed.metalRoughness;   u.shadeA[3] = feed.glassRoughness;
      u.shadeB[0] = feed.glassLumaThresh;  u.shadeB[1] = feed.glassMaxChanThresh;
      u.shadeB[2] = feed.importedGamma;    u.shadeB[3] = feed.importedAlbedoScale;
      u.shadeC[0] = feed.aoStrength;       u.shadeC[1] = feed.normalStrength;
      if (poolN) std::memcpy(mainPtr + poolOff, poolUpload, poolBytes);
      u.poolF[0] = feed.poolMoverDim; u.poolF[1] = feed.poolMoverScale; u.poolF[2] = feed.poolObjScale;
      u.poolI[0] = poolN;             u.poolI[1] = feed.poolSoft;
      u.poolI[2] = feed.poolMoverMax; u.poolI[3] = feed.poolCompress;
      u.miscI[0] = feed.specDebugMask;
      u.miscI[1] = mechFill;
      const auto& ambient = visualTuning_environment().sky.hemisphereSky;
      for (size_t c=0; c<3; ++c)
          u.environmentAmbient[c] = ambient[c] * visualTuning_environment().unitAmbientScale;
      std::memcpy(mainPtr, &u, sizeof(u)); }
    // Every scheme set of this parity follows the buffers the spans came from; the draw passes the offsets.
    sync_ring_bindings(s, par, mainSpan, lightSpan);
    const uint32_t dynOffsets[5] = {
        static_cast<uint32_t>(mainSpan.offset),
        static_cast<uint32_t>(mainSpan.offset + instOff),
        static_cast<uint32_t>(mainSpan.offset + boneOff),
        static_cast<uint32_t>(lightSpan ? lightSpan.offset : mainSpan.offset),
        static_cast<uint32_t>(mainSpan.offset + poolOff)};
    g_health.stageUs = usSince(tStage0);

    // ---- Shared-GBuffer co-write (VK-RUNG8 item-4: direct-chain only). Terrain left
    // the shared color/normal/depth Vulkan-owned in their attachment layouts, so there
    // is NO GL<->VK semaphore handoff — begin a frame only on the color parity and rely
    // on the submit fence below for CPU ordering before the next island. ----
    VkCommandBuffer cb = s->cbuf[par];

    const clk::time_point tRender0 = clk::now();
    ZoneNamedN(zMechRec, "VK-mech-record-submit", true);   // VK-FLIP-FIELD-REPORT-1: CPU record+queue-submit (to fn return)
    vk_render_pass_timer::CancelUnlessSubmitted timerGuard(gos_render_pass_timer::Pass_Mechs);
    unsigned long runsRec = 0;
    if (!record_mech(s, cb, par, w, h, feed, runSets, dynOffsets, &*ground, poolN, runsRec)) { setFallback("record_failed"); s->disabled = true; return 0; }
    g_health.runs = runsRec;

    // Render-pass dependencies order shared attachments; the ticket protects
    // only this island's command buffer and descriptors.
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    { ZoneScopedN("VKIsl.Mech.Submit");   // VK-ISLAND-TRACY-1: vkQueueSubmit
      if (mc2vk::submitGraphics(si, VK_NULL_HANDLE) != VK_SUCCESS) { setFallback("submit_failed"); s->disabled = true; return 0; } }
    s->submitted[par] = rctx.tracker().now();
    timerGuard.submitted();

    if (!mechFenceDeferEnabled()) {
        ZoneScopedN("VKIsl.Mech.InlineFenceWait"); mc2::battlebench::WaitScope gpuWait2(mc2::battlebench::WaitGpuMainNs);   // VK-ISLAND-TRACY-1: killswitch-only inline retire
        const clk::time_point tInline0 = clk::now();
        if (!rctx.wait(s->submitted[par], 5000000000ull, "mech inline retire")) {
            setFallback("fence_timeout"); s->disabled = true; return 0;
        }
        us_fence_wait = usSince(tInline0);
    }
    g_health.drawUs = usSince(tRender0);
    s->gpuTimer.resolve();   // VK-DRAW-PERF-1: fence signalled -> fold GPU segment times

    const bool readbackFired = (g_forbidden.glGetTexImage != f0b) || (g_forbidden.glReadPixels != f0c);
    if (readbackFired) {
        vlog("FATAL: a color readback fired on the co-write path — failing frame + disabling.");
        setFallback("interop_did_readback"); s->disabled = true; return 0;
    }

    g_health.vkDraws = 1;
    ++g_health.usedVulkan;
    g_health.totalUs = usSince(tTotal0);
    static bool s_firstOk = false;
    if (!s_firstOk) {
        vlog("first skinned mech co-write: instCount=%u boneCount=%u runs=%lu "
             "(co-written into shared color(MRT0)+normal(MRT1)+depth; no composite).",
             feed.instanceCount, feed.boneCount, g_health.runs);
        s_firstOk = true;
    }
    if (perf) {
        std::fprintf(stderr,
            "[VK_MECH_PERF] tier1=1 inst=%u bones=%u runs=%lu stage_us=%.1f "
            "record_submit_us=%.1f fence_wait_us=%.1f island_total_us=%.1f\n",
            feed.instanceCount, feed.boneCount, g_health.runs, g_health.stageUs,
            g_health.drawUs, us_fence_wait, g_health.totalUs);
        std::fflush(stderr);
    }
    return 1;
}

// ---- Mech-lab panel pass ------------------------------------------------------------------------------------------------
namespace {
bool g_previewPending = false;
bool g_previewUnusable = false;   // latched: a preview init/record failure keeps the lab on its CPU mech draw
Mc2MechPreviewRequest g_previewReq{};

// First occurrence of each transient skip reason (the lab then keeps its CPU draw that frame).
int previewSkip(std::string_view why) {   // callers pass string literals
    static const char* s_last = nullptr;
    if (why.data() != s_last) { s_last = why.data(); vlog("mech-lab preview skipped this frame (%.*s) -- CPU mech draw stays on.", int(why.size()), why.data()); }
    return 0;
}

int previewFail(std::string_view why) {
    g_previewUnusable = true;
    vlog("mech-lab preview disabled (%.*s) -- the lab keeps the CPU mech draw.", int(why.size()), why.data());
    return 0;
}

// Depth prepass + material runs into the preview framebuffer, viewport/scissor = the panel rect.
bool record_mech_preview(MechIsland* s, VkCommandBuffer cb, const Mc2MechFrameFeed& feed,
                         const std::vector<VkDescriptorSet>& runSets, const uint32_t (&dynOffsets)[5], int w, int h) {
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) return false;
    VkClearValue clears[3]{};
    clears[2].depthStencil = {0.0f, 0};   // reverse-Z far
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = s->previewRpass.get(); rbi.framebuffer = s->previewFb.get();
    rbi.renderArea = {{0, 0}, {uint32_t(w), uint32_t(h)}};
    rbi.clearValueCount = 3; rbi.pClearValues = clears;
    MC2_VK_GPU_SCOPE(gpuScope, cb, "VKIsl.MechPreview");
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    const auto& q = g_previewReq;
    // The request rect is in the panel's fixed 800x600 logical space (y down), the space the legacy preview pass
    // projects with (vulkan_legacy_shape_island.cpp, proj 2/800 and -2/600); the target is that space scaled to w x h
    // and stores rows bottom-up (drawPreviewToPanel flips v), so the rect's top edge sits at row h - vpY * sy.
    const float sx = float(w) / 800.0f, sy = float(h) / 600.0f;
    const float fx = float(q.vpX) * sx, fw = float(q.vpW) * sx, fh = float(q.vpH) * sy;
    const float fy = float(h) - float(q.vpY + q.vpH) * sy;
    const int x0 = std::clamp(int(fx), 0, w - 1), y0 = std::clamp(int(fy), 0, h - 1);
    const int x1 = std::clamp(int(fx + fw + 0.5f), x0 + 1, w), y1 = std::clamp(int(fy + fh + 0.5f), y0 + 1, h);
    // The viewport is the full panel rect (it may hang off the target); the scissor clips it to the target.
    VkViewport vp{fx, fy, fw, fh, 0.0f, 1.0f};
    VkRect2D scissor{{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &scissor);
    if (s->backdropPipe && !runSets.empty()) {
        mc2vk::infra::bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->backdropPipe.get());
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->playout.get(), 0, 1, &runSets[0], 5, dynOffsets);
        vkCmdDraw(cb, 3, 1, 0, 0);
    }
    VkDeviceSize vboOff = 0;
    const VkBuffer vertexBuffer = s->vbo.get();
    vkCmdBindVertexBuffers(cb, 0, 1, &vertexBuffer, &vboOff);
    vkCmdBindIndexBuffer(cb, s->ibo.get(), 0, VK_INDEX_TYPE_UINT32);
    for (const VkPipeline pipe : {s->previewDepthPipe.get(), s->previewPipe.get()}) {
        mc2vk::infra::bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        VkDescriptorSet bound = VK_NULL_HANDLE;
        for (uint32_t r = 0; r < feed.runCount; ++r) {
            const Mc2MechDrawRun& run = feed.runs[r];
            if (!run.indexCount || !run.instanceCount) continue;
            if (runSets[r] != bound) {
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->playout.get(), 0, 1, &runSets[r], 5, dynOffsets);
                bound = runSets[r];
            }
            const MechRunPC pc{run.materialFlags, 0, 0};
            vkCmdPushConstants(cb, s->playout.get(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
            vkCmdDrawIndexed(cb, run.indexCount, run.instanceCount, run.firstIndex, run.baseVertex, run.instanceBase);
        }
    }
    vkCmdEndRenderPass(cb);
    gpuScope.end();
    return vkEndCommandBuffer(cb) == VK_SUCCESS;
}
}  // namespace

extern "C" void mc2_vk_mech_preview_request(const Mc2MechPreviewRequest* r) {
    g_previewPending = r != nullptr;
    if (r) g_previewReq = *r;
}

extern "C" int mc2_vk_mech_preview_usable(void) { return (g_previewUnusable || g_mechSessionDeclined) ? 0 : 1; }

namespace { bool g_previewDrewLast = false; }
extern "C" int mc2_vk_mech_preview_drew_last(void) { return g_previewDrewLast ? 1 : 0; }

static int mech_preview_flush_impl(void* colorView, void* normalView, void* depthView, int w, int h);
extern "C" int mc2_vk_mech_preview_flush(void* colorView, void* normalView, void* depthView, int w, int h) {
    if (!g_previewPending) return 0;
    const int drew = mech_preview_flush_impl(colorView, normalView, depthView, w, h);
    g_previewDrewLast = drew != 0;
    return drew;
}

static int mech_preview_flush_impl(void* colorView, void* normalView, void* depthView, int w, int h) {
    if (!g_previewPending) return 0;
    g_previewPending = false;
    if (g_previewUnusable || g_mechSessionDeclined) return 0;
    if (!colorView || !normalView || !depthView || w <= 0 || h <= 0) return previewSkip("no_preview_target");
    ZoneScopedN("VK-mech-preview");
    const Mc2MechPreviewRequest& q = g_previewReq;
    Mc2MechFrameFeed feed{};
    if (!mc2_mech_island_get_frame_feed(&feed) || !feed.instances64 || !feed.instanceCount || !feed.runs || !feed.runCount) return previewSkip("no_frame_feed");

    mc2vk::VulkanContext& rctx = mc2vk::getOrCreateEngineVulkanContext();
    if (const MechIsland* parked = parkedIsland(); parked && (parked->w != q.sceneW || parked->h != q.sceneH))
        rctx.retireIsland(mc2vk::IslandSlot::Mech);
    if (!islandSlot()) {
        auto fresh = std::make_unique<MechIsland>();
        if (!init_island(fresh.get(), q.sceneW, q.sceneH)) {
            rctx.waitIdle("mech_preview_init_failure");
            // A build-phase failure is permanent; a readiness gate (context / shared GBuffer) retries next frame.
            return g_mechInitReachedBuild ? previewFail(g_health.fallbackReason) : previewSkip("island_not_ready");
        }
        islandSlot() = std::move(fresh);
    }
    MechIsland* s = parkedIsland();
    if (s->disabled || !s->inited) return previewSkip("island_disabled");
    if (!s->previewReady) return previewFail("preview_variant_missing");
    if (!upload_geometry_once(s) || !ensure_dummy_scheme(s)) return previewSkip("geometry_upload");

    const VkDeviceSize ringAlign = rctx.frameAlignment();
    const VkDeviceSize instBytes = VkDeviceSize(feed.instanceCount) * 64ull;
    const VkDeviceSize boneBytes = VkDeviceSize(feed.boneCount ? feed.boneCount : 1) * 64ull;
    const VkDeviceSize instOff = mc2vk::alignUp(sizeof(MechFrameUBO), ringAlign);
    const VkDeviceSize boneOff = mc2vk::alignUp(instOff + instBytes, ringAlign);
    const VkDeviceSize poolOff = mc2vk::alignUp(boneOff + boneBytes, ringAlign);
    const mc2vk::FrameAlloc mainSpan = rctx.frameAlloc(poolOff + sizeof(gos_terrain_lighting::GpuTerrainLight));
    if (!mainSpan) return previewSkip("frame_alloc");
    uint8_t* const mainPtr = static_cast<uint8_t*>(mainSpan.ptr);
    const uint32_t par = rctx.frameContext().slot;
    if (!rctx.wait(s->submitted[par], 5000000000ull, "mech preview slot")) return previewFail("slot_timeout");

    std::memcpy(mainPtr + instOff, feed.instances64, size_t(instBytes));
    if (feed.boneCount) std::memcpy(mainPtr + boneOff, feed.bones64, size_t(feed.boneCount * 64ull));
    std::memset(mainPtr + poolOff, 0, sizeof(gos_terrain_lighting::GpuTerrainLight));
    mc2vk::FrameAlloc lightSpan;
    if (s->wantLighting && q.lights && q.lightBytes) {
        lightSpan = rctx.frameAlloc(VkDeviceSize(q.lightBytes));
        if (lightSpan) std::memcpy(lightSpan.ptr, q.lights, size_t(q.lightBytes));
    }

    const VkDescriptorSet dummySet = get_scheme_set(s, 0, 0, 0, par);
    std::vector<VkDescriptorSet> runSets(feed.runCount, dummySet);
    s->materialReady = false;
    for (uint32_t r = 0; s->wantMaterial && r < feed.runCount; ++r) {
        const auto& run = feed.runs[r];
        if (!run.indexCount || !run.instanceCount) continue;
        if (const auto set = get_scheme_set(s, run.slot0Tex, run.aoTex, run.normalTex, par)) runSets[r] = set;
    }

    { MechFrameUBO u{};
      std::memcpy(u.worldToClipGL, q.worldToClipGL, sizeof(u.worldToClipGL));
      u.objectDepthBias = 0.0f;
      u.materialReady = (s->wantMaterial && s->materialReady) ? 1 : 0;
      u.lightingReady = lightSpan ? 1 : 0;
      u.sunAxisMc2Fix = mc2::options::current().sunAxisFix ? 1 : 0;
      u.fogColorRGB[3] = q.exposure;   // MECH_PREVIEW: panel exposure (the world pass leaves it 0)
      // Backdrop (mech_backdrop.frag): feet centre and contact-shadow ellipse radii in panel-rect uv, y down.
      if (q.groundH > 1.0f && q.vpW > 0 && q.vpH > 0) {
          u.poolF[0] = (q.groundX - float(q.vpX)) / float(q.vpW);
          u.poolF[1] = (q.groundY - float(q.vpY)) / float(q.vpH);
          u.poolF[2] = 0.30f * q.groundH / float(q.vpW);
          u.poolF[3] = 0.045f * q.groundH / float(q.vpH);
      }
      for (int k = 0; k < 4; ++k) u.cameraWorldPos[k] = q.cameraWorldPos[k];
      u.shadeA[0] = feed.ambientStrength;  u.shadeA[1] = feed.specularStrength;
      u.shadeA[2] = feed.metalRoughness;   u.shadeA[3] = feed.glassRoughness;
      u.shadeB[0] = feed.glassLumaThresh;  u.shadeB[1] = feed.glassMaxChanThresh;
      u.shadeB[2] = feed.importedGamma;    u.shadeB[3] = feed.importedAlbedoScale;
      u.shadeC[0] = feed.aoStrength;       u.shadeC[1] = feed.normalStrength;
      static const int mechFill = mc2::gate::get<"MC2_MECH_FILL">();
      u.miscI[1] = mechFill;
      const auto& ambient = visualTuning_environment().sky.hemisphereSky;
      for (size_t c = 0; c < 3; ++c) u.environmentAmbient[c] = ambient[c] * visualTuning_environment().unitAmbientScale;
      std::memcpy(mainPtr, &u, sizeof(u)); }
    sync_ring_bindings(s, par, mainSpan, lightSpan);
    const uint32_t dynOffsets[5] = {
        uint32_t(mainSpan.offset), uint32_t(mainSpan.offset + instOff), uint32_t(mainSpan.offset + boneOff),
        uint32_t(lightSpan ? lightSpan.offset : mainSpan.offset), uint32_t(mainSpan.offset + poolOff)};

    // The preview target can be rebuilt (panel resize / texture service reset): rebuild the framebuffer on any change.
    const VkImageView views[3] = {static_cast<VkImageView>(colorView), static_cast<VkImageView>(normalView), static_cast<VkImageView>(depthView)};
    if (!s->previewFb || s->previewFbW != w || s->previewFbH != h || !std::equal(views, views + 3, s->previewFbViews)) {
        for (const auto& t : s->submitted)
            if (s->previewFb && !rctx.wait(t, 5000000000ull, "mech preview fb rebuild")) return previewFail("fb_rebuild_timeout");
        s->previewFb.reset();
        VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fci.renderPass = s->previewRpass.get(); fci.attachmentCount = 3; fci.pAttachments = views;
        fci.width = uint32_t(w); fci.height = uint32_t(h); fci.layers = 1;
        if (!mc2vk::create(s->previewFb, s->device, [&](VkFramebuffer* f) { return vkCreateFramebuffer(s->device, &fci, nullptr, f); }))
            return previewFail("framebuffer_failed");
        std::copy(views, views + 3, s->previewFbViews);
        s->previewFbW = w; s->previewFbH = h;
    }

    VkCommandBuffer cb = s->cbuf[par];
    if (!record_mech_preview(s, cb, feed, runSets, dynOffsets, w, h)) return previewFail("record_failed");
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    if (mc2vk::submitGraphics(si, VK_NULL_HANDLE) != VK_SUCCESS) return previewFail("submit_failed");
    s->submitted[par] = rctx.tracker().now();
    static bool s_firstOk = false;
    if (!s_firstOk) {
        s_firstOk = true;
        vlog("first mech-lab panel draw: inst=%u bones=%u runs=%u target=%dx%d panel=%d,%d %dx%d material=%d lighting=%d",
             feed.instanceCount, feed.boneCount, feed.runCount, w, h, q.vpX, q.vpY, q.vpW, q.vpH,
             s->materialReady ? 1 : 0, lightSpan ? 1 : 0);
    }
    return 1;
}

extern "C" void mc2_render_backend_mech_region_health(
    int* vulkanAvailable, int* buildEnabled, const char** deviceName,
    unsigned long* attempted, unsigned long* usedVulkan, const char** fallbackReason,
    unsigned long* vkDraws, int* interopActive, int* vk2glSemActive,
    unsigned long* instRecords, unsigned long* boneRecords, unsigned long* runs,
    unsigned long* geomVerts,
    unsigned long* forbiddenGetTexImage, unsigned long* forbiddenReadPixels,
    double* stageUs, double* drawUs, double* totalUs) {
    if (vulkanAvailable)  *vulkanAvailable  = g_health.vulkanAvailable;
    if (buildEnabled)     *buildEnabled     = g_health.buildEnabled;
    if (deviceName)       *deviceName       = g_health.deviceName;
    if (attempted)        *attempted        = g_health.attempted;
    if (usedVulkan)       *usedVulkan       = g_health.usedVulkan;
    if (fallbackReason)   *fallbackReason   = g_health.fallbackReason;
    if (vkDraws)          *vkDraws          = g_health.vkDraws;
    if (interopActive)    *interopActive    = g_health.interopActive;
    if (vk2glSemActive)   *vk2glSemActive   = g_health.vk2glSemActive;
    if (instRecords)      *instRecords      = g_health.instRecords;
    if (boneRecords)      *boneRecords      = g_health.boneRecords;
    if (runs)             *runs             = g_health.runs;
    if (geomVerts)        *geomVerts        = g_health.geomVerts;
    if (forbiddenGetTexImage) *forbiddenGetTexImage = g_forbidden.glGetTexImage;
    if (forbiddenReadPixels)  *forbiddenReadPixels  = g_forbidden.glReadPixels;
    if (stageUs)          *stageUs          = g_health.stageUs;
    if (drawUs)           *drawUs           = g_health.drawUs;
    if (totalUs)          *totalUs          = g_health.totalUs;
}

extern "C" void mc2_vk_mech_island_destroy() {
    vlog("[VK_MECH_TIER1] vulkan_available=%d used_vulkan=%lu interop_active=%d vk2gl_sem=%d "
         "inst_records=%lu bone_records=%lu runs=%lu geom_verts=%lu fallback_reason=\"%s\" "
         "vk_draws=%lu forbidden_getTexImage=%lu forbidden_readPixels=%lu "
         "stage_us=%.1f draw_us=%.1f total_us=%.1f",
         g_health.vulkanAvailable, g_health.usedVulkan, g_health.interopActive, g_health.vk2glSemActive,
         g_health.instRecords, g_health.boneRecords, g_health.runs, g_health.geomVerts,
         g_health.fallbackReason, g_health.vkDraws, g_forbidden.glGetTexImage, g_forbidden.glReadPixels,
         g_health.stageUs, g_health.drawUs, g_health.totalUs);
    // VK-DRAW-PERF-1: GPU-timestamp segment breakdown (only when MC2_VK_GPU_TIMESTAMPS
    // armed and supported; gap1 = setup+binds, gap2 = mech instanced draws).
    const MechIsland* parked = parkedIsland();
    if (parked && parked->gpuTimer.supported && parked->gpuTimer.samples > 0) {
        vlog("GPU_TIMESTAMPS samples=%llu gpu_pass_us=%.1f gpu_setup_us=%.1f gpu_draw_us=%.1f "
             "(cpu_draw_us=%.1f incl. record+submit+fence-wait)",
             static_cast<unsigned long long>(parked->gpuTimer.samples),
             parked->gpuTimer.avgTotalUs(),
             parked->gpuTimer.avgGapUs(1),
             parked->gpuTimer.avgGapUs(2),
             g_health.drawUs);
    }
    mc2vk::getOrCreateEngineVulkanContext().retireIsland(mc2vk::IslandSlot::Mech);
}

#endif // MC2_VULKAN_ISLAND
