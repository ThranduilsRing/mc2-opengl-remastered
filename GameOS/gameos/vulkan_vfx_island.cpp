#include "mission_environment_gpu.h"
#include "visual_tuning_profile.h"
#include "../../mclib/camera.h"
// vulkan_vfx_island.cpp â€” VK-FIELD-VFX-1.
//
// Renders the GPU-particle billboard stream into the SHARED scene GBuffer color0
// as translucent, depth-tested (reverse-Z GEQUAL, NO depth write) colored soft
// billboards, so weapon fire (tracers / muzzle flashes / impact glows) survives
// the MC2_VK_SHARED_GBUFFER umbrella. Structurally a thin water island: it borrows
// the shared engine VK device + the same shared color0/normal/depth VkImageResources
// the terrain/water islands co-write, LOADs them, blends the particle billboards on
// top honoring the shared depth, and STOREs â€” no offscreen target, no composite, no
// GL<->VK interop.
//
// SCOPE (honest): textured â€” each group samples its sprite page (imported once per
// gos texture handle from the texels the GL path uploads) times the per-particle
// tint. Two blend pipelines (alpha + additive) mirror the GL particle bridge's
// per-group blend. Native geometry also covers tube ribbons and effect meshes.
// Built with the Vulkan renderer.


#if defined(_WIN32) && !defined(VK_USE_PLATFORM_WIN32_KHR)
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <volk.h>
#include "Gates/gate.h"
#include "vk_render_pass_timer.h"
#include "vk_gpu_profile.h"   // VK-GPU-ZONES: MC2_VK_GPU_SCOPE
#include "Gates/gate_cold.h"   // str<>()

#include <vk_mem_alloc.h>

#include "vk_image_resource.h"
#include "vk_island_util.h"
#include "vk_owned.h"
#include "vulkan_context.h"
#include "vk_dynamic_rendering.h"
#define MC2_VULKAN_SHARED_SCENE_COLOR_IMPL_ISLAND
#include "vulkan_shared_scene_color.h"
#define MC2_VULKAN_SHARED_SCENE_DEPTH_IMPL_ISLAND
#include "vulkan_shared_scene_depth.h"
#include "vk_shared_gbuffer_gate.h"
#include "vulkan_vfx_island.h"
#include "gos_postprocess.h"
#include "vulkan_vfx_island_algo.h"
#include "vulkan_vfx_geometry.h"
#include "vfx_draw_plan.h"
#include "vfx_native_batch.h"
#include "../../mclib/fx/fx_render.h"
#include "vk_desc_write_cache.h"
#include "vulkan_mover_shadow.h"
#include "vulkan_shadow_island_live.h"
#include "texture_decode.h"
#include "../../RenderCore/KtxLoader.h"
#include "ui_texture.h"
#include "vk_texture_residency.h"
#include "vk_vegetation_state.h"
#include "gos_vegetation.h"
#include "vk_vegetation_ground.h"

#include <tracy/Tracy.hpp>   // VK-ISLAND-TRACY-1: per-frame CPU attribution zones

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cstdint>
#include <cstddef>   // offsetof (M2 layout asserts)
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "../../mclib/battle_bench_wait.h"

namespace {

// â”€â”€ Health snapshot (mirrors the water island's; stderr summary at destroy) â”€â”€
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
    unsigned long nativeMeshDraws = 0, nativeTubeDraws = 0, nativeVegetationDraws = 0;
};
VfxIslandHealth g_health;
std::shared_ptr<const mc2tex::TextureArrayCpu> vegetationAtlas;

void setFallback(const char* reason) {
    std::strncpy(g_health.fallbackReason, reason ? reason : "",
                 sizeof(g_health.fallbackReason) - 1);
    g_health.fallbackReason[sizeof(g_health.fallbackReason) - 1] = '\0';
}

void vlog(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vk_island_util::vlogv("[VK_VFX] ", fmt, ap);
    va_end(ap);
}

std::string spv_dir() { return vk_island_util::spvDir(); }

VkShaderModule load_spv(VkDevice device, std::string_view path) {
    return vk_island_util::loadSpv(device, path, vlog);
}

// std140 UBO mirror â€” LOCKSTEP with vfx_particle.vert (VfxVsParams).
struct VfxVsParams {
    float worldToClipGL[16];  // column_major; SAME raw 16 floats as GL GL_FALSE upload   (off 0)
    float camRight[4];        // xyz + pad   (off 64)
    float camUp[4];           // xyz + pad   (off 80)
};
static_assert(sizeof(VfxVsParams) == 96, "VfxVsParams std140 size (vfx_particle.vert)");
static_assert(offsetof(VfxVsParams, camRight) == 64 && offsetof(VfxVsParams, camUp) == 80, "VfxVsParams offsets");

// Push constants: the group's atlas sub-rect â€” LOCKSTEP with VfxGroupPush in vfx_particle.vert.
struct VfxGroupPush {
    float    uvOffset[2];
    float    uvSize[2];
    uint32_t atlasColumns;
    uint32_t blendMode;
};
static_assert(sizeof(VfxGroupPush) == 24, "VfxGroupPush push-constant size (vfx_particle.vert)");
static_assert(offsetof(VfxGroupPush, uvSize) == 8 && offsetof(VfxGroupPush, atlasColumns) == 16 && offsetof(VfxGroupPush, blendMode) == 20);
static_assert(offsetof(VfxGeometryVertex, pad0) == 12);

// std430 Group in vfx_particle.vert; the record's unused pad at byte 12 selects it.
struct VfxIndexedGroup {
    VfxGroupPush draw;
    uint32_t page;
    uint32_t blend = 0;   // island blend mode; the fragment shader applies the premultiplied fold weights
};
static_assert(sizeof(VfxIndexedGroup) == 32 && offsetof(VfxIndexedGroup, page) == 24 && offsetof(VfxIndexedGroup, blend) == 28);

constexpr uint32_t kSpritePagesPerPool = 64;   // sprite pages per pool of the descriptor chain; a mission that needs more grows it
// Each engine frame can submit vegetation, billboards, tube ribbons and mesh effects.
// Give each call its own command buffer and descriptors: sharing the last slot makes
// the fourth call wait for the third call's same-frame GPU submission to retire.
constexpr uint32_t kVfxCallsPerFrame = 4;
constexpr uint32_t kVfxSlots = mc2vk::kMaxFramesInFlight * kVfxCallsPerFrame;

// Sprite pages borrow generation identities from shared residency. Only descriptor pairs live here; they stay until mission unload
// (mc2_vk_vfx_island_reset), which resets the whole chain after the GPU is idle.
struct SpritePage {
    VkDescriptorSet set[kVfxSlots]{};
    VkImageView bound[kVfxSlots]{};
};

struct VfxIsland : mc2vk::IslandState {
    VkInstance       instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys     = VK_NULL_HANDLE;
    VkDevice         device   = VK_NULL_HANDLE;
    VkQueue          queue    = VK_NULL_HANDLE;
    uint32_t         gfxFamily = 0;
    VmaAllocator     allocator = VK_NULL_HANDLE;

    VkImageResource* sColorRes  = nullptr;
    VkImageResource* sNormalRes = nullptr;
    VkImageResource* sDepthRes  = nullptr;

    // The objects the island owns, in creation order: the destructor releases them in reverse (fence first, buffers last), so the island is released
    // by deleting it, after the GPU is idle. The descriptor set and the command buffer belong to their pool.
    VkFormat sceneFmts[2]{VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED};   // colour0, normal: the scene pass is dynamic rendering, pipelines carry the formats
    VkFormat sceneDepthFmt = VK_FORMAT_UNDEFINED;
    mc2vk::OwnedDescriptorSetLayout dsl;
    mc2vk::OwnedDescriptorPool pool;
    // One set per parity slot; both bind the engine frame ring with dynamic offsets (0 = frame constants, 1 = particles). ringBound[par][binding] = the ring
    // buffer that binding of set[par] currently points at; a frame whose span came from another buffer (the ring grew) re-points it after the slot's last
    // submit completed.
    VkDescriptorSet  set[kVfxSlots] = {};
    VkBuffer         ringBound[kVfxSlots][2] = {};
    mc2vk::DescWriteStamp moverStamp[kVfxSlots]{};   // binding 24 of set[i]
    mc2vk::DescWriteStamp maskStamp[kVfxSlots]{};    // binding 17 of set[i] (bake mask or the white fallback)
    mc2vk::OwnedShaderModule vert, frag, vegFrag, vegShadeFrag;
    mc2vk::OwnedSampler samp;
    mc2vk::OwnedSampler vegetationSampler;
    float vegBias = 0.0f; uint32_t vegGen = 0, vegBoundGen[kVfxSlots] = {};   // bias the vegetation sampler was built with; per-slot rewrite generation
    mc2vk::ResidentTextureRef vegetationTexture;
    std::shared_ptr<const mc2tex::TextureArrayCpu> loadedVegetation;
    mc2vk::OwnedDescriptorSetLayout spriteDsl;
    mc2vk::DescPoolChain spritePool;
    std::unordered_map<uint64_t, SpritePage> pages; // keyed on full service identity, never a mutable gos name
    mc2veg::GroundLighting groundLighting;
    mc2vk::OwnedPipelineLayout playout;
    mc2vk::OwnedPipeline alphaPipe;
    mc2vk::OwnedPipeline addPipe;
    mc2vk::OwnedPipeline vegetationPipe;
    mc2vk::OwnedPipeline vegetationDepthPipe;   // depth pre-pass of the same draw (vfx_vegetation.frag kDepthOnly)
    mc2vk::OwnedPipeline vegetationFringePipe;
    mc2vk::OwnedShaderModule vegVert;        // vfx_vegetation.vert: reads the GPU cull's survivor list
    mc2vk::OwnedPipelineLayout vegLayout;    // set 0 ring, set 1 atlas page, set 2 the vegetation state's draw set
    mc2vk::OwnedPipeline oneOnePipe;  // GL OneOne: (ONE, ONE), alpha ignored
    mc2vk::OwnedPipeline oneAlphaPipe, oneInvAlphaPipe, opaquePipe;
    bool indexed = false;
    mc2vk::OwnedShaderModule indexedVert, indexedFrag;
    mc2vk::OwnedDescriptorSetLayout indexedDsl;
    mc2vk::OwnedDescriptorPool indexedPool;
    VkDescriptorSet indexedSet[kVfxSlots]{};
    mc2vk::OwnedPipelineLayout indexedLayout;
    mc2vk::OwnedPipeline indexedPipes[5];   // [0] premultiplied fold of modes 0/1/2/5, [3] opaque, [4] ONE/SRC_ALPHA
    mc2vk::OwnedCommandPool cpool[kVfxSlots];   // per-slot pools: vkResetCommandPool instead of per-cb reset
    VkCommandBuffer  cbuf[kVfxSlots] = {};   // one per slot
    uint64_t         ticket[kVfxSlots] = {};   // timeline value of the slot's last submit (0 = never submitted)
    uint64_t         vegTicket[mc2vk::kMaxFramesInFlight] = {};        // timeline value of the last vegetation submit per engine frame slot (its buffer set is per frame slot)
    uint64_t         slotFrame = ~static_cast<uint64_t>(0);   // engine frame of callInFrame
    uint32_t         callInFrame = 0;

    int  w = 0, h = 0;
    bool inited = false;
    bool disabled = false;
};

// The slot for this call: the engine frame slot picks a group, the call index within the frame picks the member (extra calls reuse the last member).
uint32_t take_slot(VfxIsland* s, mc2vk::VulkanContext& ctx) {
    const auto fc = ctx.frameContext();
    if (s->slotFrame != fc.frame) { s->slotFrame = fc.frame; s->callInFrame = 0; }
    const uint32_t call = std::min(s->callInFrame++, kVfxCallsPerFrame - 1);
    return fc.slot * kVfxCallsPerFrame + call;
}

// The ring bindings of one parity set: 0 = frame constants (uniform, explicit range), 1 = particles (storage, the ring's dynamic range).
void write_ring_bindings(VfxIsland* s, uint32_t par, VkBuffer buf, VkDeviceSize range) {
    if (buf == VK_NULL_HANDLE || s->set[par] == VK_NULL_HANDLE) return;
    if (s->ringBound[par][0] != buf) {
        mc2vk::writeDynamicBinding(s->device, s->set[par], 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, buf, sizeof(VfxVsParams));
        s->ringBound[par][0] = buf;
    }
    if (s->ringBound[par][1] != buf) {
        mc2vk::writeDynamicBinding(s->device, s->set[par], 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, buf, range);
        s->ringBound[par][1] = buf;
    }
}

uint64_t white_texture_id(mc2vk::VulkanContext& ctx) {
    static const auto white = std::make_shared<const std::vector<uint8_t>>(4, 255u);
    return ctx.textures().legacyUiId(0, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, white);
}

// Resolve after this parity's ticket retires. New/changed pages keep the other flight untouched.
std::optional<VkDescriptorSet> get_page(VfxIsland* s,uint32_t handle,uint32_t par) {
    if (handle==kVfxVegetationAtlas && s->vegetationSampler && mc2vk::infra::materialBiasStale(s->vegBias)) {   // FSR/AA resolve state changed
        VkSamplerCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.maxLod = VK_LOD_CLAMP_NONE;
        mc2vk::infra::materialSampler(sci);
        VkPhysicalDeviceProperties vegetationProps{};
        vkGetPhysicalDeviceProperties(s->phys, &vegetationProps);
        sci.anisotropyEnable = mc2vk::profile::featureEnabled(mc2vk::getOrCreateEngineVulkanContext().profile(), "samplerAnisotropy") ? VK_TRUE : VK_FALSE;
        sci.maxAnisotropy = std::min(8.0f, vegetationProps.limits.maxSamplerAnisotropy);
        mc2vk::OwnedSampler fresh;
        if (mc2vk::create(fresh, s->device, [&](VkSampler* o) { return vkCreateSampler(s->device, &sci, nullptr, o); })) {
            mc2vk::getOrCreateEngineVulkanContext().retire(std::move(s->vegetationSampler));
            s->vegetationSampler = std::move(fresh);
            s->vegBias = sci.mipLodBias;
            ++s->vegGen;
            mc2vk::bumpDescEpoch();
        }
    }
    if (handle==0xffffffffu) return std::nullopt;
    auto& ctx=mc2vk::getOrCreateEngineVulkanContext();
    uint64_t id=0;
    if (handle==kVfxVegetationAtlas) {
        if (!vegetationAtlas) return std::nullopt;
        if (s->loadedVegetation != vegetationAtlas) {
            RenderCore::KtxImage image;
            image.width = static_cast<int>(vegetationAtlas->width); image.height = static_cast<int>(vegetationAtlas->height);
            image.vkFormat = vegetationAtlas->vkFormat; image.mipCount = static_cast<int>(vegetationAtlas->levels.size());
            image.isCompressed = image.vkFormat == mc2tex::kVkFormatBc7Unorm;
            image.blockSizeBytes = image.isCompressed ? 16 : 0;
            for (const auto& level : vegetationAtlas->levels) {
                image.mipByteOffsets.push_back(image.pixels.size());
                image.pixels.insert(image.pixels.end(), level.begin(), level.end());
            }
            RenderCore::TextureKey key;
            key.source = RenderCore::makeTextureSourceId("vegetation:P2", "cards",
                RenderCore::textureContentHash(image.pixels.data(), image.pixels.size()));
            key.format = image.vkFormat; key.hints = gosHint_MipmapFilter0;
            const uint32_t width = vegetationAtlas->width, height = vegetationAtlas->height;
            const int vegMips = image.mipCount;
            s->vegetationTexture = ctx.textures().acquire(key, std::make_shared<const RenderCore::KtxImage>(std::move(image)), {width, height, width, height}, 8, true);
            if (!s->vegetationTexture) return std::nullopt;
            s->loadedVegetation = vegetationAtlas;
            vlog("vegetation_cards layers=8 mips=%d depth_write=1 cutoff=0.5", vegMips);
        }
        id = RenderCore::uiTextureId(s->vegetationTexture.get());
    } else if (handle==0) {
        id=white_texture_id(ctx);
    } else id=gos_GetUiTextureId(handle);
    if (!id) return std::nullopt;
    auto it=s->pages.find(id);
    if (it==s->pages.end()) {
        SpritePage page;
        VkDescriptorSetLayout layouts[kVfxSlots]; for (auto& l : layouts) l=s->spriteDsl.get();
        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorSetCount=kVfxSlots;alloc.pSetLayouts=layouts;
        if (MC2_VK_ALLOC_SETS_CHAIN(s->spritePool,alloc,page.set)!=VK_SUCCESS) return std::nullopt;
        const auto [cachedIt, inserted] = s->pages.emplace(id,page);
        it=cachedIt;
        vlog("sprite page identity: source=%u texture=%llu (%zu, pools=%zu).",handle,static_cast<unsigned long long>(id),s->pages.size(),s->spritePool.length());
    }
    auto& [pageId, page]=*it;
    // Invalid/missing pages get the visible placeholder; eventual uploads change this view.
    const VkImageView view=ctx.textures().view(RenderCore::uiTextureHandle(id));
    const bool vegStale = handle==kVfxVegetationAtlas && s->vegBoundGen[par]!=s->vegGen;
    if (page.bound[par]!=view || vegStale) {
        if (handle==kVfxVegetationAtlas) s->vegBoundGen[par]=s->vegGen;
        const VkDescriptorImageInfo image{handle==kVfxVegetationAtlas ? s->vegetationSampler.get() : s->samp.get(),view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet=page.set[par];write.dstBinding=0;write.descriptorCount=1;
        write.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;write.pImageInfo=&image;
        vkUpdateDescriptorSets(s->device,1,&write,0,nullptr);page.bound[par]=view;
    }
    return page.set[par];
}

// VfxDrawGroup::additive values (LOCKSTEP with ADDITIVE_* in shaders/vulkan/vfx_particle.vert).
inline constexpr uint32_t VFX_ADD_ALPHA = 0, VFX_ADD_SRC_ALPHA = 1, VFX_ADD_ONE_ONE = 2;

mc2vk::OwnedPipeline make_pipeline(VfxIsland* s, int w, int h, bool additive, bool oneOne = false, bool vegetation = false, unsigned tubeMode = 0, bool indexed = false, bool depthOnly = false, bool fringe = false) {
    VkPipelineVertexInputStateCreateInfo vin{};
    vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;  // procedural
    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = vegetation ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;   // double-sided
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    // reverse-Z GEQUAL depth TEST, NO write (translucent) â€” matches the GL particle bridge.
    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    // Vegetation cores establish depth before shading; fractional edges blend afterwards without depth writes.
    ds.depthWriteEnable = ((vegetation && depthOnly) || (!vegetation && tubeMode == 3)) ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = vegetation && !fringe ? (depthOnly ? VK_COMPARE_OP_GREATER : VK_COMPARE_OP_EQUAL) : VK_COMPARE_OP_GREATER_OR_EQUAL;
    // color0: straight-alpha (FS emits straight rgb,a; src-alpha factor premults in HW).
    //   alpha    = SRC_ALPHA / ONE_MINUS_SRC_ALPHA  (GL alpha groups)
    //   additive = SRC_ALPHA / ONE                  (GL additive groups)
    VkPipelineColorBlendAttachmentState cba[2]{};
    cba[0].blendEnable = ((vegetation && !fringe) || tubeMode == 3) ? VK_FALSE : VK_TRUE;
    cba[0].srcColorBlendFactor = oneOne ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_ALPHA;
    cba[0].dstColorBlendFactor = additive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba[0].colorBlendOp = VK_BLEND_OP_ADD;
    cba[0].srcAlphaBlendFactor = oneOne ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_ALPHA;
    cba[0].dstAlphaBlendFactor = additive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    if (tubeMode == 4 || tubeMode == 5) {
        cba[0].srcColorBlendFactor = cba[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba[0].dstColorBlendFactor = cba[0].dstAlphaBlendFactor =
            tubeMode == 4 ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    }
    if (indexed && tubeMode == 0) {   // premultiplied fold of modes 0/1/2/5: the fragment shader weights rgb and alpha per record
        cba[0].srcColorBlendFactor = cba[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba[0].dstColorBlendFactor = cba[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    }
    cba[0].alphaBlendOp = VK_BLEND_OP_ADD;
    cba[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    cba[1].blendEnable = VK_FALSE;   // shared normal target left untouched
    cba[1].colorWriteMask = vegetation && !fringe ? cba[0].colorWriteMask : 0;
    if (depthOnly) cba[0].colorWriteMask = cba[1].colorWriteMask = 0;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 2; cb.pAttachments = cba;
    VkViewport vp{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {static_cast<uint32_t>(w), static_cast<uint32_t>(h)}};
    VkPipelineViewportStateCreateInfo vps{};
    vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1; vps.pViewports = &vp; vps.scissorCount = 1; vps.pScissors = &scissor;
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vegetation ? s->vegVert.get() : s->vert.get(); stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = vegetation ? (depthOnly ? s->vegFrag.get() : s->vegShadeFrag.get()) : s->frag.get(); stages[1].pName = "main";
    if (indexed) { stages[0].module = s->indexedVert.get(); stages[1].module = s->indexedFrag.get(); }
    const VkBool32 vegetationModes[]{depthOnly ? VK_TRUE : VK_FALSE, fringe ? VK_TRUE : VK_FALSE};
    const VkSpecializationMapEntry vegetationEntries[]{{0, 0, sizeof(VkBool32)}, {1, sizeof(VkBool32), sizeof(VkBool32)}};
    const VkSpecializationInfo vegetationSpec{2, vegetationEntries, sizeof(vegetationModes), vegetationModes};
    if (vegetation) stages[1].pSpecializationInfo = &vegetationSpec;
    VkGraphicsPipelineCreateInfo gp{};
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.stageCount = 2; gp.pStages = stages;
    gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia;
    const VkDynamicState dynViewport = VK_DYNAMIC_STATE_VIEWPORT;   // temporal AA jitters the viewport per frame
    VkPipelineDynamicStateCreateInfo dyns{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyns.dynamicStateCount = 1; dyns.pDynamicStates = &dynViewport;
    gp.pDynamicState = &dyns;
    gp.pViewportState = &vps; gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms; gp.pDepthStencilState = &ds; gp.pColorBlendState = &cb;
    const VkPipelineRenderingCreateInfo sceneRendering = mc2vk::dynrender::pipelineInfo(2, s->sceneFmts, s->sceneDepthFmt);
    gp.pNext = &sceneRendering;
    gp.layout = vegetation ? s->vegLayout.get() : s->playout.get(); gp.renderPass = VK_NULL_HANDLE; gp.subpass = 0;
    if (indexed) gp.layout = s->indexedLayout.get();
    mc2vk::OwnedPipeline pipe;
    if (!mc2vk::create(pipe, s->device, [&](VkPipeline* out) { return mc2vk::infra::graphics(s->device, VK_NULL_HANDLE, 1, &gp, nullptr, out); })) {
        vlog("mc2vk::infra::graphics(%s) failed.", additive ? "additive" : "alpha");
        return {};
    }
    mc2vk::nameObject(s->device, pipe.get(), additive ? "vfx.pipe.additive" : "vfx.pipe.alpha");
    return pipe;
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
    if (s->sColorRes->view.get() == VK_NULL_HANDLE || s->sColorRes->format == VK_FORMAT_UNDEFINED ||
        s->sNormalRes->view.get() == VK_NULL_HANDLE || s->sDepthRes->view.get() == VK_NULL_HANDLE ||
        s->sDepthRes->format == VK_FORMAT_UNDEFINED) {
        setFallback("shared_gbuffer_views_not_ready"); return false;
    }

    // Scene pass (VK_KHR_dynamic_rendering, no VkRenderPass/VkFramebuffer): color0 (LOAD, blend, STORE) + normal (LOAD, STORE, writemask 0) + depth (LOAD,
    // GEQUAL test, NO write, STORE unchanged). The pipelines carry these formats; the passes record sceneLoadBarrier (the old external dependency).
    s->sceneFmts[0] = s->sColorRes->format; s->sceneFmts[1] = s->sNormalRes->format; s->sceneDepthFmt = s->sDepthRes->format;

    // Descriptor set: UBO(0, vert) + particle SSBO(1, vert).
    {
        VkDescriptorSetLayoutBinding b[7]{};
        b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
        b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        b[2].binding = 17; b[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[2].descriptorCount = 1; b[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        b[3].binding = 24; b[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[3].descriptorCount = 1; b[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        b[4].binding = 25; b[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        b[4].descriptorCount = 1; b[4].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        b[5] = {26, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        b[6].binding = 27; b[6].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        b[6].descriptorCount = 1; b[6].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        lci.bindingCount = 7; lci.pBindings = b;
        if (!mc2vk::create(s->dsl, s->device, [&](VkDescriptorSetLayout* o) { return vkCreateDescriptorSetLayout(s->device, &lci, nullptr, o); })) { vlog("vkCreateDescriptorSetLayout failed."); return false; }
        VkDescriptorPoolSize sizes[4]{};
        sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; sizes[0].descriptorCount = kVfxSlots;
        sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC; sizes[1].descriptorCount = kVfxSlots;
        sizes[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; sizes[2].descriptorCount = 2 * kVfxSlots;
        sizes[3].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; sizes[3].descriptorCount = 3 * kVfxSlots;
        VkDescriptorPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.maxSets = kVfxSlots; pci.poolSizeCount = 4; pci.pPoolSizes = sizes;
        if (!mc2vk::create(s->pool, s->device, [&](VkDescriptorPool* o) { return vkCreateDescriptorPool(s->device, &pci, nullptr, o); })) { vlog("vkCreateDescriptorPool failed."); return false; }
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        VkDescriptorSetLayout dslHandles2[kVfxSlots]; for (auto& l : dslHandles2) l = s->dsl.get();
        dsai.descriptorPool = s->pool.get(); dsai.descriptorSetCount = kVfxSlots; dsai.pSetLayouts = dslHandles2;
        if (MC2_VK_ALLOC_SETS(s->device, &dsai, s->set) != VK_SUCCESS) { vlog("vkAllocateDescriptorSets failed."); return false; }
    }

    // Pipelines (alpha + additive).
    {
        s->vert = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir() + "vfx_particle.vert.spv"));
        s->frag = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir() + "vfx_particle.frag.spv"));
        if (GosVegetation::isEnabled()) {
            s->vegFrag = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir() + "vfx_vegetation.frag.spv"));
            s->vegShadeFrag = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir() + "vfx_vegetation_shade.frag.spv"));
        }
        if (!s->vert || !s->frag || (GosVegetation::isEnabled() && (!s->vegFrag || !s->vegShadeFrag))) { setFallback("spv_load_failed"); return false; }
        // Sprite pages: set 1 = one combined sampler (LINEAR, no mips -- the FS samples level 0 like the GL path).
        VkSamplerCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        if (!mc2vk::create(s->samp, s->device, [&](VkSampler* o) { return vkCreateSampler(s->device, &sci, nullptr, o); })) { vlog("vkCreateSampler failed."); return false; }
        sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.maxLod = VK_LOD_CLAMP_NONE;
        mc2vk::infra::materialSampler(sci);
        s->vegBias = sci.mipLodBias;
        VkPhysicalDeviceProperties vegetationProps{};
        vkGetPhysicalDeviceProperties(s->phys, &vegetationProps);
        sci.anisotropyEnable = mc2vk::profile::featureEnabled(ctx.profile(), "samplerAnisotropy") ? VK_TRUE : VK_FALSE;
        sci.maxAnisotropy = std::min(8.0f, vegetationProps.limits.maxSamplerAnisotropy);
        if (GosVegetation::isEnabled() && !mc2vk::create(s->vegetationSampler, s->device, [&](VkSampler* o) { return vkCreateSampler(s->device, &sci, nullptr, o); })) return false;

        VkDescriptorSetLayoutBinding sb{};
        sb.binding = 0; sb.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sb.descriptorCount = 1; sb.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo slci{};
        slci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        slci.bindingCount = 1; slci.pBindings = &sb;
        if (!mc2vk::create(s->spriteDsl, s->device, [&](VkDescriptorSetLayout* o) { return vkCreateDescriptorSetLayout(s->device, &slci, nullptr, o); })) { vlog("vkCreateDescriptorSetLayout(sprite) failed."); return false; }
        // Sets are never freed one by one: the chain is reset as a whole at mission unload (mc2_vk_vfx_island_reset).
        VkDescriptorPoolSize spriteSize{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kVfxSlots * kSpritePagesPerPool };
        VkDescriptorPoolCreateInfo spci{};
        spci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        spci.maxSets = kVfxSlots * kSpritePagesPerPool; spci.poolSizeCount = 1; spci.pPoolSizes = &spriteSize;
        if (const VkResult r = s->spritePool.init(s->device, spci); r != VK_SUCCESS) { vlog("vkCreateDescriptorPool(sprite) failed result=%d.", static_cast<int>(r)); return false; }

        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        if (!s->groundLighting.init(s->device)) return false;
        const VkDescriptorSetLayout dslHandles[4] = { s->dsl.get(), s->spriteDsl.get(), s->spriteDsl.get(), s->groundLighting.layout() };
        plci.setLayoutCount = 4; plci.pSetLayouts = dslHandles;
        const VkPushConstantRange pcr{ VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(VfxGroupPush) };
        plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
        if (!mc2vk::create(s->playout, s->device, [&](VkPipelineLayout* o) { return vkCreatePipelineLayout(s->device, &plci, nullptr, o); })) { vlog("vkCreatePipelineLayout failed."); return false; }
        if (GosVegetation::isEnabled()) {
            // Vegetation: its own vertex shader and a third descriptor set (the GPU-resident population and survivor list).
            mc2veg::State& veg = mc2veg::State::instance();
            if (!veg.ensureDrawLayout(ctx)) { setFallback("vegetation_draw_layout_failed"); return false; }
            s->vegVert = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir() + "vfx_vegetation.vert.spv"));
            if (!s->vegVert) { setFallback("spv_load_failed"); return false; }
            const VkDescriptorSetLayout vegHandles[4] = { s->dsl.get(), s->spriteDsl.get(), veg.drawLayout(), s->groundLighting.layout() };
            VkPipelineLayoutCreateInfo vlci{};
            vlci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            vlci.setLayoutCount = 4; vlci.pSetLayouts = vegHandles;
            if (!mc2vk::create(s->vegLayout, s->device, [&](VkPipelineLayout* o) { return vkCreatePipelineLayout(s->device, &vlci, nullptr, o); })) { vlog("vkCreatePipelineLayout(vegetation) failed."); return false; }
        }
        if (GosVegetation::isEnabled()) {
            s->vegetationPipe = make_pipeline(s, w, h, false, false, true);
            s->vegetationDepthPipe = make_pipeline(s, w, h, false, false, true, 0, false, true);
            s->vegetationFringePipe = make_pipeline(s, w, h, false, false, true, 0, false, false, true);
        }
        s->alphaPipe = make_pipeline(s, w, h, /*additive=*/false);
        s->addPipe   = make_pipeline(s, w, h, /*additive=*/true);
        s->oneOnePipe = make_pipeline(s, w, h, /*additive=*/true, /*oneOne=*/true);
        s->oneAlphaPipe = make_pipeline(s, w, h, false, false, false, 4);
        s->oneInvAlphaPipe = make_pipeline(s, w, h, false, false, false, 5);
        s->opaquePipe = make_pipeline(s, w, h, false, false, false, 3);
        if (!s->alphaPipe || !s->addPipe || !s->oneOnePipe ||
            !s->oneAlphaPipe || !s->oneInvAlphaPipe || !s->opaquePipe || (GosVegetation::isEnabled() && (!s->vegetationPipe || !s->vegetationDepthPipe || !s->vegetationFringePipe))) { setFallback("pipeline_create_failed"); return false; }

        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(s->phys, &props);
        s->indexed = mc2vk::profile::featureEnabled(ctx.profile(), "shaderSampledImageArrayNonUniformIndexing") &&
            props.limits.maxPerStageDescriptorSamplers >= kSpritePagesPerPool + 7 &&
            props.limits.maxPerStageDescriptorSampledImages >= kSpritePagesPerPool + 7;
        if (s->indexed) {
            s->indexedVert = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir() + "vfx_indexed.vert.spv"));
            s->indexedFrag = mc2vk::OwnedShaderModule(s->device, load_spv(s->device, spv_dir() + "vfx_indexed.frag.spv"));
            if (!s->indexedVert || !s->indexedFrag) return false;
            const VkDescriptorSetLayoutBinding bindings[] = {
                {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kSpritePagesPerPool, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr}};
            VkDescriptorSetLayoutCreateInfo ilci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            ilci.bindingCount = 2; ilci.pBindings = bindings;
            if (!mc2vk::create(s->indexedDsl, s->device, [&](VkDescriptorSetLayout* o) { return vkCreateDescriptorSetLayout(s->device, &ilci, nullptr, o); })) return false;
            const VkDescriptorPoolSize sizes[] = {
                {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kVfxSlots * kSpritePagesPerPool},
                {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kVfxSlots}};
            VkDescriptorPoolCreateInfo ipci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            ipci.maxSets = kVfxSlots; ipci.poolSizeCount = 2; ipci.pPoolSizes = sizes;
            if (!mc2vk::create(s->indexedPool, s->device, [&](VkDescriptorPool* o) { return vkCreateDescriptorPool(s->device, &ipci, nullptr, o); })) return false;
            VkDescriptorSetLayout layouts[kVfxSlots];
            for (auto& layout : layouts) layout = s->indexedDsl.get();
            VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            alloc.descriptorPool = s->indexedPool.get(); alloc.descriptorSetCount = kVfxSlots; alloc.pSetLayouts = layouts;
            if (MC2_VK_ALLOC_SETS(s->device, &alloc, s->indexedSet) != VK_SUCCESS) return false;
            const VkDescriptorSetLayout indexedLayouts[] = {s->dsl.get(), s->indexedDsl.get(), s->spriteDsl.get(), s->groundLighting.layout()};
            plci.pSetLayouts = indexedLayouts;
            if (!mc2vk::create(s->indexedLayout, s->device, [&](VkPipelineLayout* o) { return vkCreatePipelineLayout(s->device, &plci, nullptr, o); })) return false;
            for (const unsigned mode : {0u, 3u, 4u}) {
                s->indexedPipes[mode] = make_pipeline(s, w, h, false, false, false, mode, true);
                if (!s->indexedPipes[mode]) return false;
            }
        }
    }

    // One command buffer per call and frame slot; reuse waits on its last timeline ticket.
    {
        VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = 0;
        pci.queueFamilyIndex = s->gfxFamily;
        for (uint32_t i = 0; i < kVfxSlots; ++i) {
            if (!mc2vk::create(s->cpool[i], s->device, [&](VkCommandPool* o) { return vkCreateCommandPool(s->device, &pci, nullptr, o); })) { vlog("vkCreateCommandPool failed."); return false; }
            VkCommandBufferAllocateInfo cai{};
            cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            cai.commandPool = s->cpool[i].get(); cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
            if (vkAllocateCommandBuffers(s->device, &cai, &s->cbuf[i]) != VK_SUCCESS) { vlog("vkAllocateCommandBuffers failed."); return false; }
        }
    }

    s->inited = true;
    setFallback("");
    vlog("island initialized %dx%d (shared scene GBuffer co-write; procedural billboard VS; "
         "GEQUAL reverse-Z depth-test no-write; alpha+additive blend; textured sprite pages).", w, h);
    return true;
}

// The island lives in the engine context's slot: the context releases it ahead of the device whatever path ends the process. Returns the ready island,
// or nullptr when it is not available this frame (init failed, or an earlier failure disabled it).
VfxIsland* acquire_island(int w, int h) {
    mc2vk::VulkanContext& slotCtx = mc2vk::getOrCreateEngineVulkanContext();
    std::unique_ptr<mc2vk::IslandState>& slot = slotCtx.islandSlot(mc2vk::IslandSlot::Vfx);
    if (slot) {
        const VfxIsland* parked = static_cast<const VfxIsland*>(slot.get());
        if (parked->w != w || parked->h != h) slotCtx.retireIsland(mc2vk::IslandSlot::Vfx);
    }
    if (!slot) {
        auto fresh = std::make_unique<VfxIsland>();
        if (!init_island(fresh.get(), w, h)) {
            vlog("init failed (%s) -- VFX declined this frame.", g_health.fallbackReason);
            slotCtx.waitIdle("vfx_init_failure");   // objects made before the failure may have been submitted
            return nullptr;
        }
        slot = std::move(fresh);
    }
    VfxIsland* s = static_cast<VfxIsland*>(slot.get());
    return (s->disabled || !s->inited) ? nullptr : s;
}

// The frame constants every VFX draw reads (set 0 binding 0). A missing camera basis defaults to the identity axes.
void fill_frame_params(void* dst, const float* worldToClipGL, const float* camRight, const float* camUp, int w, int h) {
    VfxVsParams up{};
    std::memcpy(up.worldToClipGL, worldToClipGL, sizeof(up.worldToClipGL));
    if (camRight) { up.camRight[0]=camRight[0]; up.camRight[1]=camRight[1]; up.camRight[2]=camRight[2]; }
    if (camUp)    { up.camUp[0]=camUp[0];       up.camUp[1]=camUp[1];       up.camUp[2]=camUp[2]; }
    else          { up.camUp[1]=1.0f; }
    if (!camRight) up.camRight[0]=1.0f;
    up.camRight[3] = 2.0f / static_cast<float>(w);
    up.camUp[3] = 2.0f / static_cast<float>(h);
    std::memcpy(dst, &up, sizeof(up));
}

// Both draw paths borrow current shadow resources only after this parity retires.
bool prepare_shadow_descriptors(VfxIsland* s, uint32_t par) {
    auto& rctx = mc2vk::getOrCreateEngineVulkanContext();
    const auto slice = rctx.frameAlloc(sizeof(EnvironmentUBO));
    if (!slice) return false;
    EnvironmentUBO environment;
    mc2::environment::fillGpu(visualTuning_environment(), mc2SceneLightScale(), environment,
        eye ? eye->currentLighting() : mc2::lighting::LightingState{},
        eye ? eye->bakeReferenceLighting() : mc2::lighting::LightingState{});
    std::memcpy(slice.ptr, &environment, sizeof(environment));
    const VkDescriptorBufferInfo info{slice.buffer, slice.offset, sizeof(environment)};
    VkWriteDescriptorSet envWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    envWrite.dstSet = s->set[par]; envWrite.dstBinding = 26; envWrite.descriptorCount = 1;
    envWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; envWrite.pBufferInfo = &info;
    vkUpdateDescriptorSets(s->device, 1, &envWrite, 0, nullptr);
    mc2vk::noteDescWrites(1, 0);

    const auto fogSlice = rctx.frameAlloc(sizeof(EdgeFogParams));
    if (!fogSlice) return false;
    EdgeFogParams fog{};
    if (const auto* pp = getGosPostProcess()) pp->fillWorldFogParams(fog);
    std::memcpy(fogSlice.ptr, &fog, sizeof(fog));
    const VkDescriptorBufferInfo fogInfo{fogSlice.buffer, fogSlice.offset, sizeof(fog)};
    envWrite.dstBinding = 27; envWrite.pBufferInfo = &fogInfo;
    vkUpdateDescriptorSets(s->device, 1, &envWrite, 0, nullptr);

    if (!mc2vk::bindMoverShadowDescriptors(s->device,s->set[par],&s->moverStamp[par])) return false;
    // The white page is a valid fallback while the bake is pending. This set
    // has retired its prior submission, so changing borrowed views is safe.
    auto& ctx = mc2vk::getOrCreateEngineVulkanContext();
    const uint64_t whiteId = white_texture_id(ctx);
    if (!whiteId) return false;
    const VkImageView white = ctx.textures().view(RenderCore::uiTextureHandle(whiteId));
    if (!white) return false;
    const VkImageView mask = mc2_vk_shadow_island_bake_ready()
        ? reinterpret_cast<VkImageView>(mc2_vk_shadow_island_get_bake_mask_view()) : white;
    const VkDescriptorImageInfo image{s->samp.get(),mask,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    // Mask vs white is the view; whiteId keys a recycled white page.
    if (!s->maskStamp[par].needsWrite({mc2vk::descKeyWord(s->set[par]),mc2vk::descKeyWord(mask),mc2vk::descKeyWord(image.sampler),whiteId})) {
        mc2vk::noteDescWrites(0,1);
        return true;
    }
    mc2vk::noteDescWrites(1,0);
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet=s->set[par]; write.dstBinding=17; write.descriptorCount=1;
    write.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; write.pImageInfo=&image;
    vkUpdateDescriptorSets(s->device,1,&write,0,nullptr);
    return true;
}

}  // namespace

// ===========================================================================
// PUBLIC ENTRY POINTS
// ===========================================================================
extern "C" int mc2_vk_vfx_island_enabled() { return 1; }

extern "C" int mc2_vk_vfx_island_prewarm(int w,int h) {
    auto& ctx=mc2vk::getOrCreateEngineVulkanContext();
    if(!ctx.valid() || w<=0 || h<=0) return 0;
    auto& slot=ctx.islandSlot(mc2vk::IslandSlot::Vfx);
    if(!slot) {
        auto fresh=std::make_unique<VfxIsland>();
        if(!init_island(fresh.get(),w,h)) { ctx.waitIdle("vfx_prewarm_failure"); return 0; }
        slot=std::move(fresh);
    }
    if (vegetationAtlas) get_page(static_cast<VfxIsland*>(slot.get()),kVfxVegetationAtlas,0);
    if (mc2veg::State::exists() && mc2veg::State::instance().ready() &&
        !mc2veg::State::instance().prewarm(ctx)) return 0;
    return 1;
}

static mc2vfx::NativeBatch g_nativeBatch;   // see vfx_native_batch.h

static int run_vfx(
    const void* particles, uint32_t particleCount,
    const VfxParticleDraw* draws, uint32_t drawCount,
    const float* worldToClipGL, const float* camRight, const float* camUp,
    int w, int h, const uint32_t* indices = nullptr, uint32_t indexCount = 0) {
    if (particleCount == 0 || drawCount == 0 || particles == nullptr || draws == nullptr) return 0;
    if (w <= 0 || h <= 0 || worldToClipGL == nullptr) return 0;
    ZoneScopedN("VKIsl.Vfx.Run");   // VK-ISLAND-TRACY-1 per-call total (CPU)

    mc2vk::VulkanContext& slotCtx = mc2vk::getOrCreateEngineVulkanContext();
    VfxIsland* s = acquire_island(w, h);
    if (!s) return 0;

    // Per-frame data goes into the engine frame ring: frame constants (UBO) then the whole particle array (64B/record), one span. The parity slot's last
    // submit must have completed before its command buffer and set are reused: a timeline ticket, not an island fence.
    const uint32_t par = take_slot(s, slotCtx);
    { ZoneScopedN("VKIsl.Vfx.SlotWait"); mc2::battlebench::WaitScope gpuWait1(mc2::battlebench::WaitGpuMainNs);
    if (!slotCtx.wait(mc2vk::GpuTicket{s->ticket[par], 0}, 5ull * 1000 * 1000 * 1000, "vfx_slot")) { setFallback("slot_timeout"); s->disabled = true; return 0; } }
    const bool nativeIdx = indices != nullptr;   // native gosFX packets: RenderVertex + index buffer (distinct from the descriptor-indexed page path below)
    const uint32_t stride = nativeIdx ? sizeof(mc2::fx::RenderVertex) : 64u;
    const VkDeviceSize needBytes = static_cast<VkDeviceSize>(particleCount) * stride;
    const VkDeviceSize partOff = mc2vk::alignUp(sizeof(VfxVsParams), slotCtx.frameAlignment());
    const VkDeviceSize groupOff = mc2vk::alignUp(partOff + needBytes, slotCtx.frameAlignment());
    const VkDeviceSize indexOff = mc2vk::alignUp(groupOff + drawCount * sizeof(VfxIndexedGroup), slotCtx.frameAlignment());
    const mc2vk::FrameAlloc span = slotCtx.frameAlloc(indexOff + static_cast<VkDeviceSize>(indexCount) * sizeof(uint32_t));
    if (!span) { setFallback("frame_ring_alloc_failed"); s->disabled = true; return 0; }
    fill_frame_params(span.ptr, worldToClipGL, camRight, camUp, w, h);
    { ZoneScopedN("VKIsl.Vfx.Descriptors");
    if (!prepare_shadow_descriptors(s, par)) return 0;
    write_ring_bindings(s, par, span.buffer, span.range); }
    const uint32_t dynOffsets[2] = {static_cast<uint32_t>(span.offset), static_cast<uint32_t>(span.offset + partOff)};

    // Resolve service pages after parity retirement; uploads stay asynchronous.
    std::vector<std::optional<VkDescriptorSet>> groupPage(drawCount);
    { ZoneScopedN("VKIsl.Vfx.Pages");
    for (uint32_t gi = 0; gi < drawCount; ++gi) {
        if (draws[gi].count != 0 && (draws[gi].texture != 0 || draws[gi].atlasColumns == kVfxGeometryAtlas || draws[gi].atlasColumns == kVfxCompactGeometryAtlas))
            groupPage[gi] = get_page(s, draws[gi].texture, par);
        // The mesh bridge drew unresolved textures as white; preserve that fallback.
        if (!groupPage[gi] && draws[gi].atlasColumns == kVfxGeometryAtlas && draws[gi].start < particleCount &&
            (static_cast<const VfxGeometryVertex*>(particles)[draws[gi].start].flags & VfxGeometryMesh) != 0u)
            groupPage[gi] = get_page(s, 0, par);
    } }
    if (s->disabled) return 0;

    // Pack drawable groups so equal-state neighbours are one draw (vfx_draw_plan.h); the ring holds the records in packed order.
    std::vector<mc2vfx::PlanItem> items;
    std::vector<mc2vfx::PlanDraw> plan;
    unsigned long meshDraws = 0, tubeDraws = 0, vegetationDraws = 0;
    items.reserve(drawCount);
    for (uint32_t gi = 0; gi < drawCount; ++gi) {
        const VfxParticleDraw& d = draws[gi];
        if (d.count == 0) continue;
        if (static_cast<uint64_t>(d.start) + d.count > (nativeIdx ? indexCount : particleCount)) continue;
        if (nativeIdx && (d.atlasColumns != kVfxCompactGeometryAtlas ||
            std::any_of(indices + d.start, indices + d.start + d.count, [=](auto i) { return i >= particleCount; }))) continue;
        if (!groupPage[gi]) continue;   // no resident texture: skipped, as the GL path does
        if (d.atlasColumns == kVfxVegetationAtlas) continue;   // vegetation draws through mc2_vk_vfx_vegetation_run
        items.push_back({d.start, d.count, d.additive, d.atlasColumns, mc2vk::descKeyWord(*groupPage[gi]), d.u0, d.v0, d.us, d.vs, gi});
        if (d.atlasColumns == kVfxGeometryAtlas) {
            const auto flags = static_cast<const VfxGeometryVertex*>(particles)[d.start].flags;
            if ((flags & VfxGeometryMesh) != 0u) ++meshDraws;
            if ((flags & VfxGeometryTube) != 0u) ++tubeDraws;
        }
    }
    // Table writes happen only after this slot's previous submission retired. Fill
    // every descriptor; devices need neither partially-bound nor update-after-bind.
    bool indexed = s->indexed && !items.empty() && !nativeIdx;
    std::vector<VkDescriptorSet> pageSets;
    std::vector<VkDescriptorImageInfo> pageImages;
    auto* groups = reinterpret_cast<VfxIndexedGroup*>(static_cast<uint8_t*>(span.ptr) + groupOff);
    if (indexed) {
        const mc2vfx::PageOwners<decltype(s->pages)> owners(s->pages, par);
        for (const auto& it : items) {
            const VkDescriptorSet set = *groupPage[it.tag];
            auto page = std::find(pageSets.begin(), pageSets.end(), set);
            if (page == pageSets.end()) {
                if (pageSets.size() == kSpritePagesPerPool) { indexed = false; break; }
                const auto* resident = owners.find(set);
                if (resident == nullptr) { indexed = false; break; }
                pageSets.push_back(set);
                pageImages.push_back({s->samp.get(), resident->bound[par], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
                page = pageSets.end() - 1;
            }
            groups[it.tag] = {{{it.u0, it.v0}, {it.us, it.vs}, it.atlas, it.blend},
                              static_cast<uint32_t>(page - pageSets.begin()), it.blend};
        }
    }
    if (indexed) {
        pageImages.resize(kSpritePagesPerPool, pageImages.front());
        const VkDescriptorBufferInfo buffer{span.buffer, span.offset + groupOff, drawCount * sizeof(VfxIndexedGroup)};
        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = writes[1].dstSet = s->indexedSet[par];
        writes[0].dstBinding = 0; writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].descriptorCount = kSpritePagesPerPool; writes[0].pImageInfo = pageImages.data();
        writes[1].dstBinding = 1; writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].descriptorCount = 1; writes[1].pBufferInfo = &buffer;
        vkUpdateDescriptorSets(s->device, 2, writes, 0, nullptr);
    }
    if (nativeIdx) {
        // Native packets include multipass surfaces. Preserve authored ordering
        // across all families and passes, including consecutive additive passes.
        uint32_t at = 0;
        for (uint32_t i = 0; i < items.size(); ++i) { plan.push_back({i, at, items[i].count}); at += items[i].count; }
    } else mc2vfx::plan_draws(items, plan, indexed);
    {
        ZoneScopedN("VKIsl.Vfx.Feed");   // VK-ISLAND-TRACY-1: particle staging
        if (nativeIdx) {
            std::memcpy(static_cast<uint8_t*>(span.ptr) + partOff, particles, static_cast<size_t>(needBytes));
            auto* dest = static_cast<uint8_t*>(span.ptr) + indexOff;
            for (const auto& item : items) {
                const auto bytes = static_cast<size_t>(item.count) * sizeof(uint32_t);
                std::memcpy(dest, indices + item.src, bytes); dest += bytes;
            }
        } else {
        uint8_t* dst = static_cast<uint8_t*>(span.ptr) + partOff;
        uint8_t* const dstEnd = dst + needBytes;
        for (const mc2vfx::PlanItem& it : items) {
            if (static_cast<size_t>(dstEnd - dst) < static_cast<size_t>(it.count) * 64u) break;   // never write past the particle staging region
            std::memcpy(dst, static_cast<const uint8_t*>(particles) + static_cast<size_t>(it.src) * 64u, static_cast<size_t>(it.count) * 64u);
            if (indexed) {
                for (uint32_t vertex = 0; vertex < it.count; ++vertex)
                    std::memcpy(dst + static_cast<size_t>(vertex) * 64u + 12u, &it.tag, sizeof(it.tag));
            }
            dst += static_cast<size_t>(it.count) * 64u;
        }
        }
    }

    // Record: one renderpass, per-group pipeline + sub-range draw. The draw list is copied out here (the caller's arrays do not outlive this call; the
    // pipelines and pages are resolved now, on the render thread) and the recording runs on a worker with the frame's other recorded islands
    // (VulkanContext::submitRecorded). It touches only the copies and handles it captured.
    struct RecDraw { VkPipeline pipe; VkDescriptorSet page; VfxGroupPush push; uint32_t count; uint32_t start; };   // nativeIdx: count/start are indices
    std::vector<RecDraw> recDraws;
    recDraws.reserve(plan.size());
    for (const mc2vfx::PlanDraw& pd : plan) {
        const uint32_t gi = items[pd.item].tag;
        const VfxParticleDraw& d = draws[gi];
        VkPipeline want = d.additive == VFX_ADD_ONE_ONE ? s->oneOnePipe.get() : d.additive == VFX_ADD_SRC_ALPHA ? s->addPipe.get() : s->alphaPipe.get();
        if (d.additive == 4) want = s->oneAlphaPipe.get();
        else if (d.additive == 5) want = s->oneInvAlphaPipe.get();
        else if (d.additive == 3) want = s->opaquePipe.get();
        if (indexed) want = s->indexedPipes[mc2vfx::plan_pipe(d.additive)].get();
        const VfxGroupPush push{ {d.u0, d.v0}, {d.us, d.vs}, d.atlasColumns, d.additive };
        const uint32_t expansion = d.atlasColumns == kVfxGeometryAtlas ? 1u : 6u;
        recDraws.push_back({want, indexed ? VK_NULL_HANDLE : *groupPage[gi], push, nativeIdx ? pd.count : pd.count * expansion, nativeIdx ? pd.start : pd.start * expansion});
    }
    if (recDraws.empty()) return 0;   // nothing drawable this frame: nothing is recorded or submitted
    const unsigned long groupsDrawn = static_cast<unsigned long>(recDraws.size());

    const VkImageView sceneViews[3] = {s->sColorRes->view.get(), s->sNormalRes->view.get(), s->sDepthRes->view.get()};
    const VkFormat sceneDepthFmt = s->sceneDepthFmt;
    mc2vk::VulkanContext* const dctx = &mc2vk::engineVulkanContextNoWait();   // the recording runs on a worker: no wait/owner assert
    const VkPipelineLayout playout = indexed ? s->indexedLayout.get() : s->playout.get();
    const auto ground = mc2_vk_ground_lighting_frame();
    if (!ground) return 0;
    const uint32_t groundPar = par / kVfxCallsPerFrame;
    s->groundLighting.copy(s->device, groundPar, *ground);
    const VkDescriptorSet groundSet = s->groundLighting.set(groundPar);
    const auto groundOffsets = ground->offsets;
    const VkDescriptorSet frameSet = s->set[par];
    const VkDescriptorSet indexedSet = indexed ? s->indexedSet[par] : VK_NULL_HANDLE;
    const uint32_t offs0 = dynOffsets[0], offs1 = dynOffsets[1];
    const VkBuffer idxBuf = nativeIdx ? span.buffer : VK_NULL_HANDLE;
    const VkDeviceSize idxOffset = span.offset + indexOff;
    auto record = [recDraws = std::move(recDraws), nativeIdx, idxBuf, idxOffset, sceneViews, sceneDepthFmt, dctx, playout, frameSet, groundSet, groundOffsets, indexedSet, offs0, offs1, w, h](VkCommandBuffer cb) -> bool {
        ZoneScopedN("VKIsl.Vfx.Record");
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) return false;
        vk_render_pass_timer::Begin(cb, gos_render_pass_timer::Pass_AlphaVfx);
        MC2_VK_GPU_SCOPE(gpuScope, cb, "VKIsl.Vfx");   // all attachments LOAD: no clear values
        mc2vk::dynrender::beginSceneLoad(*dctx, cb, static_cast<uint32_t>(w), static_cast<uint32_t>(h), sceneViews[0], sceneViews[1], sceneViews[2], sceneDepthFmt);
        mc2vk::infra::setSceneViewport(cb, static_cast<uint32_t>(w), static_cast<uint32_t>(h));
        const uint32_t dynOffsets[2] = {offs0, offs1};
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, playout, 0, 1, &frameSet, 2, dynOffsets);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, playout, 3, 1, &groundSet, 4, groundOffsets.data());
        if (indexedSet != VK_NULL_HANDLE) vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, playout, 1, 1, &indexedSet, 0, nullptr);
        if (nativeIdx) vkCmdBindIndexBuffer(cb, idxBuf, idxOffset, VK_INDEX_TYPE_UINT32);
        VkPipeline bound = VK_NULL_HANDLE;
        VkDescriptorSet boundPage = VK_NULL_HANDLE;
        for (const RecDraw& d : recDraws) {
            if (d.pipe != bound) { mc2vk::infra::bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d.pipe); bound = d.pipe; }
            if (d.page != VK_NULL_HANDLE && d.page != boundPage) {
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, playout, 1, 1, &d.page, 0, nullptr);
                boundPage = d.page;
            }
            vkCmdPushConstants(cb, playout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(d.push), &d.push);
            mc2::battlebench::add(mc2::battlebench::DrawVfx);
            if (nativeIdx) vkCmdDrawIndexed(cb, d.count, 1, d.start, 0, 0);
            else vkCmdDraw(cb, d.count, 1, d.start, 0);
        }
        mc2vk::dynrender::end(*dctx, cb);
        gpuScope.end();
        vk_render_pass_timer::End(cb, gos_render_pass_timer::Pass_AlphaVfx);
        return vkEndCommandBuffer(cb) == VK_SUCCESS;
    };
    // done() runs on the render thread once the recording ran (and, with no worker pool, was submitted): a failed recording disables the island and
    // cancels the pass bracket, as the inline path did.
    auto done = [s](bool ok) {
        if (ok) return;
        vk_render_pass_timer::Cancel(gos_render_pass_timer::Pass_AlphaVfx);
        setFallback("record_failed");
        s->disabled = true;
    };
    uint64_t value = 0;
    { ZoneScopedN("VKIsl.Vfx.Submit");   // VK-ISLAND-TRACY-1: the reservation in the deferred submit batch (the recording runs at the flush)
      if (slotCtx.submitRecorded(std::move(record), std::move(done), &value) != VK_SUCCESS) return 0;   // done(false) already disabled the island
      s->ticket[par] = value; }

    if (meshDraws && !g_health.nativeMeshDraws) vlog("native_geometry kind=mesh draws=%lu", meshDraws);
    if (tubeDraws && !g_health.nativeTubeDraws) vlog("native_geometry kind=tube draws=%lu", tubeDraws);
    if (vegetationDraws && !g_health.nativeVegetationDraws) vlog("native_geometry kind=vegetation draws=%lu", vegetationDraws);
    g_health.nativeMeshDraws += meshDraws;
    g_health.nativeTubeDraws += tubeDraws;
    g_health.nativeVegetationDraws += vegetationDraws;
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

extern "C" int mc2_vk_vfx_island_run(const void* particles, uint32_t count,
    const VfxParticleDraw* draws, uint32_t drawCount, const float* mvp,
    const float* right, const float* up, int w, int h) {
    return run_vfx(particles, count, draws, drawCount, mvp, right, up, w, h);
}
int mc2_vk_vfx_indexed_run(const void* vertices, uint32_t vertexCount,
    const uint32_t* indices, uint32_t indexCount, const VfxParticleDraw* draws,
    uint32_t drawCount, const float* mvp, const float* right, const float* up, int w, int h) {
    if (!indices || !indexCount) return 0;
    if (g_nativeBatch.open) {   // gamecam's frame batch: append only; mc2_vk_vfx_native_batch_end records one pass for all of them
        if (!vertices || !vertexCount || !draws || !drawCount || !mvp || w <= 0 || h <= 0) return 0;
        ZoneScopedN("VKIsl.Vfx.Append");
        g_nativeBatch.append(static_cast<const mc2::fx::RenderVertex*>(vertices), vertexCount, indices, indexCount, draws, drawCount, mvp, right, up, w, h);
        return 1;
    }
    return run_vfx(vertices, vertexCount, draws, drawCount, mvp, right, up, w, h, indices, indexCount);
}
extern "C" void mc2_vk_vfx_native_batch_begin() { g_nativeBatch.begin(); }
extern "C" int mc2_vk_vfx_native_batch_end() {
    if (!g_nativeBatch.open) return 0;
    g_nativeBatch.open = false;
    if (g_nativeBatch.draws.empty() || !g_nativeBatch.hasMvp) return 0;
    return run_vfx(g_nativeBatch.vertices.data(), static_cast<uint32_t>(g_nativeBatch.vertices.size()), g_nativeBatch.draws.data(),
                   static_cast<uint32_t>(g_nativeBatch.draws.size()), g_nativeBatch.mvp, g_nativeBatch.hasCam ? g_nativeBatch.camRight : nullptr,
                   g_nativeBatch.hasCam ? g_nativeBatch.camUp : nullptr, g_nativeBatch.w, g_nativeBatch.h,
                   g_nativeBatch.indices.data(), static_cast<uint32_t>(g_nativeBatch.indices.size()));
}

// Vegetation: GPU cull (cull -> scan -> compact) and the two indirect strips, in this island's own command buffer and parity slot. Nothing per instance
// crosses the CPU; the frame ring carries only the cull parameters and the block-visibility field.
bool mc2_vk_vfx_vegetation_run(const mc2veg::CullParams& params, const float* worldToClipGL, std::span<const uint32_t> visibility, int w, int h) {
    if (w <= 0 || h <= 0 || worldToClipGL == nullptr) return false;
    if (!mc2veg::State::exists()) return false;
    mc2veg::State& vegState = mc2veg::State::instance();
    mc2veg::State* veg = &vegState;
    if (!veg->ready()) return false;
    const auto ground = mc2_vk_ground_lighting_frame();
    if (!ground) return false;
    ZoneScopedN("VKIsl.Vfx.Vegetation");
    mc2vk::VulkanContext& slotCtx = mc2vk::getOrCreateEngineVulkanContext();
    VfxIsland* s = acquire_island(w, h);
    // The island skips the vegetation pipeline when vegetation was off at its creation; a later quality change does not re-create it.
    if (!s || !s->vegetationPipe) return false;
    const uint32_t par = take_slot(s, slotCtx);
    if (!slotCtx.wait(mc2vk::GpuTicket{s->ticket[par], 0}, 5ull * 1000 * 1000 * 1000, "vfx_slot")) { setFallback("slot_timeout"); s->disabled = true; return false; }
    // The vegetation state keeps one buffer set per engine frame slot (it is not per island slot): the set is free once the last
    // vegetation submit that used it has retired, whichever call index the vegetation call got this frame. Only that submit
    // touches the set, so wait on its ticket, not on the other island slots of the frame slot.
    const uint32_t vegPar = par / kVfxCallsPerFrame;
    if (!slotCtx.wait(mc2vk::GpuTicket{s->vegTicket[vegPar], 0}, 5ull * 1000 * 1000 * 1000, "vfx_veg_slot")) { setFallback("slot_timeout"); s->disabled = true; return false; }
    const VkDeviceSize align = slotCtx.frameAlignment();
    if (!prepare_shadow_descriptors(s, par)) return false;
    s->groundLighting.copy(s->device, vegPar, *ground);
    const VkDeviceSize cullOff = mc2vk::alignUp(sizeof(VfxVsParams), align);
    const VkDeviceSize visOff = mc2vk::alignUp(cullOff + sizeof(mc2veg::CullParams), align);
    const mc2vk::FrameAlloc span = slotCtx.frameAlloc(visOff + std::max<VkDeviceSize>(visibility.size_bytes(), 16));
    if (!span) { setFallback("frame_ring_alloc_failed"); s->disabled = true; return false; }
    fill_frame_params(span.ptr, worldToClipGL, nullptr, nullptr, w, h);
    write_ring_bindings(s, par, span.buffer, span.range);
    const uint32_t dynOffsets[2] = {static_cast<uint32_t>(span.offset), static_cast<uint32_t>(span.offset + cullOff)};
    const auto page = get_page(s, kVfxVegetationAtlas, par);
    if (!page || s->disabled) return false;

    VkCommandBuffer cb = s->cbuf[par];
    vkResetCommandPool(s->device, s->cpool[par].get(), 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) { setFallback("record_failed"); s->disabled = true; return false; }
    vk_render_pass_timer::Begin(cb, gos_render_pass_timer::Pass_AlphaVfx);
    vk_render_pass_timer::CancelUnlessSubmitted timerGuard(gos_render_pass_timer::Pass_AlphaVfx);
    const bool culled = veg->record(slotCtx, cb, vegPar, span, cullOff, visOff, params, visibility);
    if (culled) {
        MC2_VK_GPU_SCOPE(gpuScope, cb, "VKIsl.Vegetation");   // all attachments LOAD: no clear values
        mc2vk::dynrender::beginSceneLoad(slotCtx, cb, static_cast<uint32_t>(w), static_cast<uint32_t>(h), s->sColorRes->view.get(), s->sNormalRes->view.get(),
                                         s->sDepthRes->view.get(), s->sceneDepthFmt);
        mc2vk::infra::setSceneViewport(cb, static_cast<uint32_t>(w), static_cast<uint32_t>(h));
        mc2vk::infra::bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->vegetationDepthPipe.get());
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->vegLayout.get(), 0, 1, &s->set[par], 2, dynOffsets);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->vegLayout.get(), 1, 1, &*page, 0, nullptr);
        const uint32_t drawDyn[2] = {veg->paramsDynamicOffset(vegPar), static_cast<uint32_t>(span.offset + visOff)};
        const VkDescriptorSet drawSet = veg->drawSet(vegPar);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->vegLayout.get(), 2, 1, &drawSet, 2, drawDyn);
        const VkDescriptorSet groundSet = s->groundLighting.set(vegPar);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->vegLayout.get(), 3, 1, &groundSet, 4, ground->offsets.data());
        {
            MC2_VK_GPU_SCOPE(depthScope, cb, "VKIsl.Vegetation.Depth");
            veg->drawIndirect(cb, vegPar);
        }
        mc2vk::infra::bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->vegetationPipe.get());   // shading pass; the bound sets stay valid (same layout)
        {
            MC2_VK_GPU_SCOPE(shadeScope, cb, "VKIsl.Vegetation.Shade");
            veg->drawIndirect(cb, vegPar);
        }
        mc2vk::infra::bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->vegetationFringePipe.get());
        veg->drawIndirect(cb, vegPar);
        mc2vk::dynrender::end(slotCtx, cb);
        gpuScope.end();
    }
    vk_render_pass_timer::End(cb, gos_render_pass_timer::Pass_AlphaVfx);
    if (vkEndCommandBuffer(cb) != VK_SUCCESS) { setFallback("record_failed"); s->disabled = true; return false; }
    if (!culled) return false;

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    { ZoneScopedN("VKIsl.Vfx.Submit");
      uint64_t value = 0;
      if (slotCtx.submit(si, VK_NULL_HANDLE, &value) != VK_SUCCESS) { setFallback("submit_failed"); s->disabled = true; return false; }
      s->ticket[par] = value; s->vegTicket[vegPar] = value; }
    timerGuard.submitted();
    if (!g_health.nativeVegetationDraws) vlog("native_geometry kind=vegetation gpu_cull=1 regions=%u instances=%u", veg->regionCount(), veg->instanceCount());
    g_health.nativeVegetationDraws += 3;
    ++g_health.usedVulkan;
    return true;
}

void mc2_vk_vfx_vegetation_atlas(const mc2tex::TextureArrayCpu& atlas) {
    vegetationAtlas = atlas.levels.empty() ? nullptr : std::make_shared<mc2tex::TextureArrayCpu>(atlas);
}

// Mission unload: the sprite pages name texture identities of the mission that ended. Idle the GPU (every submitted set retired), drop the pages and
// reset the whole sprite chain; its capacity is kept for the next mission, so the chain length is the largest mission's need, not a sum.
extern "C" void mc2_vk_vfx_island_reset() {
    auto& ctx=mc2vk::getOrCreateEngineVulkanContext();
    auto* s=static_cast<VfxIsland*>(ctx.islandSlot(mc2vk::IslandSlot::Vfx).get());
    if (!s || !s->inited) return;
    ctx.waitIdle("vfx_reset");
    s->spritePool.logChain("vfx_sprite");
    const size_t before=s->pages.size();
    s->pages.clear();
    if (s->spritePool) s->spritePool.reset();
    vlog("sprite page cache reset on mission unload: pages_before=%zu pools=%zu",before,s->spritePool.length());
}

extern "C" void mc2_vk_vfx_island_destroy() {
    vlog("native_mesh_draws=%lu native_tube_draws=%lu native_vegetation_draws=%lu",
         g_health.nativeMeshDraws, g_health.nativeTubeDraws, g_health.nativeVegetationDraws);
    vlog("[VK_VFX] vulkan_available=%d used_vulkan=%lu vk_draws=%lu particles=%lu groups=%lu "
         "fallback_reason=\"%s\"",
         g_health.vulkanAvailable, g_health.usedVulkan, g_health.vkDraws,
         g_health.particles, g_health.groups, g_health.fallbackReason);
    mc2vk::getOrCreateEngineVulkanContext().retireIsland(mc2vk::IslandSlot::Vfx);
}

