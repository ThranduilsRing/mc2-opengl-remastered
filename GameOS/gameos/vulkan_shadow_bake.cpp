// vulkan_shadow_bake.cpp -- see the header. Compiles to nothing unless MC2_VULKAN_ISLAND is defined.
#ifdef MC2_VULKAN_ISLAND

#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <volk.h>

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include <vk_mem_alloc.h>

#include "vulkan_shadow_bake.h"

#include "../../RenderCore/gpu_abi/GpuStaticPropInstance.h"
#include "gos_postprocess.h"
#include "mc2fmt.h"
#include "static_caster_set.h"
#include "static_shadow_spec.h"
#include "terrain_field_store.h"
#include "vk_depth_caster_pipeline.h"
#include "vk_gpu_profile.h"
#include "vk_island_util.h"
#include "vulkan_color_prop_island.h"
#include "vulkan_context.h"

#include <tracy/Tracy.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>

extern void gos_GetTerrainLightDir(float* x, float* y, float* z);
extern void gos_GetTerrainChunkGridInfo(int* mapSide, float* halfMap);
float mc2ShadowMinSunAltDeg();

namespace mc2vk {

namespace {

constexpr VkFormat kWindowFmt = VK_FORMAT_D32_SFLOAT;
constexpr VkFormat kObjectFmt = VK_FORMAT_D16_UNORM;
constexpr VkFormat kMaskFmt = VK_FORMAT_R8_UNORM;
constexpr int kObjectMapSize = 4096;              // High preset (D4); presets arrive with P7
constexpr float kMaskTargetTexelWorld = 1.5f;     // High preset target (D1)
constexpr int kMaskCapTexels = 12288;
constexpr VkDeviceSize kSlice = 256;              // >= any device's minUniformBufferOffsetAlignment
constexpr uint32_t kQueriesPerTile = 5;
// Waiting this many frames for a late alpha-ON array, the bake goes ahead with those casters opaque (a solid canopy shadow) and says so.
constexpr uint32_t kMaxAlphaWaitFrames = 600;

// LOCKSTEP with shadow_prop.vert / shadow_terrain_static.vert ShadowFrameUBO (std140).
struct CasterUBO {
    float lightSpace[16];
    int32_t instBase;
    int32_t pad[3];
};
static_assert(sizeof(CasterUBO) == 80, "ShadowFrameUBO std140 size");

// LOCKSTEP with shaders/vulkan/include/shadow_bake_common.glsl BakeTile (std140).
struct BakeTileParams {
    float window[16];
    float region[4];
    int32_t regionInfo[4];
    float sun[4];
    float windowInfo[4];
    float heightInfo[4];
    float fade[4];
    float march[4];
};
static_assert(sizeof(BakeTileParams) == 176, "BakeTile std140 size (shadow_bake_common.glsl)");

void blog(const char* what) { mc2::fmt::print_err("[SHADOW_BAKE] {}\n", what); }

VkImageMemoryBarrier imageBarrier(VkImage image, VkImageAspectFlags aspect, uint32_t baseMip, uint32_t mips, VkImageLayout from, VkImageLayout to,
                                  VkAccessFlags src, VkAccessFlags dst) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from; b.newLayout = to;
    b.srcAccessMask = src; b.dstAccessMask = dst;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, baseMip, mips, 0, 1};
    return b;
}

// A depth-only pass: cleared, stored, left read-only for sampling. The dependencies order it after earlier reads of the image (compute or fragment) and
// its writes before later reads.
bool makeDepthPass(OwnedRenderPass& out, VkDevice device, VkFormat format) {
    VkAttachmentDescription a{};
    a.format = format;
    a.samples = VK_SAMPLE_COUNT_1_BIT;
    a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    a.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp{};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.pDepthStencilAttachment = &ref;
    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL; deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].srcAccessMask = 0;   // write-after-read: an execution dependency is enough
    deps[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0; deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo rci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rci.attachmentCount = 1; rci.pAttachments = &a;
    rci.subpassCount = 1; rci.pSubpasses = &sp;
    rci.dependencyCount = 2; rci.pDependencies = deps;
    return create(out, device, [&](auto* h) { return vkCreateRenderPass(device, &rci, nullptr, h); });
}

bool makeImage(OwnedImage& image, OwnedImageView& view, VmaAllocator allocator, VkDevice device, VkFormat format, uint32_t side, uint32_t mips,
               VkImageUsageFlags usage, VkImageAspectFlags aspect, std::string_view name) {
    view.reset();
    image.reset();
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {side, side, 1};
    ici.mipLevels = mips;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    if (!image.create(allocator, device, ici, aci, name)) return false;
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = image.get();
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = {aspect, 0, mips, 0, 1};
    return create(view, device, [&](auto* h) { return vkCreateImageView(device, &vci, nullptr, h); });
}

bool makeFramebuffer(OwnedFramebuffer& fb, VkDevice device, VkRenderPass pass, VkImageView view, uint32_t side) {
    VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fci.renderPass = pass; fci.attachmentCount = 1; fci.pAttachments = &view;
    fci.width = side; fci.height = side; fci.layers = 1;
    return create(fb, device, [&](auto* h) { return vkCreateFramebuffer(device, &fci, nullptr, h); });
}

bool makeHostBuffer(OwnedBuffer& buf, VmaAllocator allocator, VkDevice device, VkDeviceSize bytes, VkBufferUsageFlags usage, std::string_view name) {
    buf.reset();
    return buf.createHostWritable(allocator, device, (std::max<VkDeviceSize>)(bytes, 256), usage, name);
}

// World-space corners of a type's local box under an instance's transform: Stuff = (pos, 1) * M (row vector, M column-major), MC2 = (-x, z, y).
void instanceBox(const float m[16], const float mn[3], const float mx[3], mc2shadow::Vec3& lo, mc2shadow::Vec3& hi) {
    lo = {1e30f, 1e30f, 1e30f};
    hi = {-1e30f, -1e30f, -1e30f};
    for (int c = 0; c < 8; ++c) {
        const float v[3] = {(c & 1) ? mx[0] : mn[0], (c & 2) ? mx[1] : mn[1], (c & 4) ? mx[2] : mn[2]};
        float s[3];
        for (int j = 0; j < 3; ++j) s[j] = v[0] * m[4 * j + 0] + v[1] * m[4 * j + 1] + v[2] * m[4 * j + 2] + m[4 * j + 3];
        const mc2shadow::Vec3 w{-s[0], s[2], s[1]};
        lo = {(std::min)(lo.x, w.x), (std::min)(lo.y, w.y), (std::min)(lo.z, w.z)};
        hi = {(std::max)(hi.x, w.x), (std::max)(hi.y, w.y), (std::max)(hi.z, w.z)};
    }
}

// Instances sorted by type, appended to `out` as runs of one type each (firstInstance counts from `base`).
template <class Run>
void appendRuns(std::vector<Run>& out, const std::vector<uint32_t>& typeIds, uint32_t base) {
    for (uint32_t i = 0; i < typeIds.size();) {
        uint32_t j = i;
        while (j < typeIds.size() && typeIds[j] == typeIds[i]) ++j;
        out.push_back({typeIds[i], base + i, j - i});
        i = j;
    }
}

}  // namespace

bool ShadowBake::init(VkDevice device, VmaAllocator allocator, VkPhysicalDevice phys, VkDescriptorSetLayout alphaDsl, std::string_view spvDir) {
    device_ = device;
    allocator_ = allocator;
    phys_ = phys;
    alphaDsl_ = alphaDsl;
    const std::string dir(spvDir);
    if (!makeDepthPass(windowPass_, device_, kWindowFmt) || !makeDepthPass(objectPass_, device_, kObjectFmt)) { blog("render pass creation failed: no bake."); return false; }

    // The caster layout: set 0 = the per-pass matrix (dynamic UBO) + the instances, set 1 = the island's alpha-ON array, a per-packet push constant.
    VkDescriptorSetLayoutBinding b[2]{};
    b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = 2; lci.pBindings = b;
    if (!create(casterDsl_, device_, [&](auto* h) { return vkCreateDescriptorSetLayout(device_, &lci, nullptr, h); })) { blog("caster layout failed."); return false; }
    const VkDescriptorSetLayout setLayouts[2] = {casterDsl_.get(), alphaDsl_};
    VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 2; plci.pSetLayouts = setLayouts;
    plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
    if (!create(casterLayout_, device_, [&](auto* h) { return vkCreatePipelineLayout(device_, &plci, nullptr, h); })) { blog("caster pipeline layout failed."); return false; }
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 2; pci.poolSizeCount = 2; pci.pPoolSizes = sizes;
    if (!create(casterPool_, device_, [&](auto* h) { return vkCreateDescriptorPool(device_, &pci, nullptr, h); })) { blog("caster pool failed."); return false; }
    const VkDescriptorSetLayout two[2] = {casterDsl_.get(), casterDsl_.get()};
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = casterPool_.get(); dsai.descriptorSetCount = 2; dsai.pSetLayouts = two;
    VkDescriptorSet sets[2]{};
    if (vkAllocateDescriptorSets(device_, &dsai, sets) != VK_SUCCESS) { blog("caster sets failed."); return false; }
    windowCasterSet_ = sets[0];
    objectCasterSet_ = sets[1];

    opaqueFrag_ = OwnedShaderModule(device_, vk_island_util::loadSpv(device_, dir + "shadow_prop.frag.spv", nullptr));
    alphaFrag_ = OwnedShaderModule(device_, vk_island_util::loadSpv(device_, dir + "shadow_prop_alpha.frag.spv", nullptr));
    if (!opaqueFrag_ || !alphaFrag_) { blog("caster fragment SPIR-V missing: no bake."); return false; }

    // Terrain casters of the object map: a packed world-space position per vertex.
    terrainVert_ = OwnedShaderModule(device_, vk_island_util::loadSpv(device_, dir + "shadow_terrain_static.vert.spv", nullptr));
    if (!terrainVert_) { blog("shadow_terrain_static.vert SPIR-V missing: no bake."); return false; }
    VkVertexInputBindingDescription vib{};
    vib.binding = 0; vib.stride = 12; vib.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription tvia[1]{};
    tvia[0].location = 0; tvia[0].binding = 0; tvia[0].format = VK_FORMAT_R32G32B32_SFLOAT; tvia[0].offset = 0;
    if (!createDepthCasterPipeline(objectTerrain_, device_, objectPass_.get(), casterLayout_.get(), terrainVert_.get(), opaqueFrag_.get(), vib, tvia)) { blog("object terrain pipeline failed."); return false; }

    // Static-prop casters: the shared static-prop vertex (position at 0, UV at 24 for the alpha cut).
    propVert_ = OwnedShaderModule(device_, vk_island_util::loadSpv(device_, dir + "shadow_prop.vert.spv", nullptr));
    if (!propVert_) { blog("shadow_prop.vert SPIR-V missing: no bake."); return false; }
    VkVertexInputBindingDescription pvib{};
    pvib.binding = 0; pvib.stride = mc2_static_prop_vertex_stride(); pvib.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription pvia[2]{};
    pvia[0].location = 0; pvia[0].binding = 0; pvia[0].format = VK_FORMAT_R32G32B32_SFLOAT; pvia[0].offset = 0;
    pvia[1].location = 2; pvia[1].binding = 0; pvia[1].format = VK_FORMAT_R32G32_SFLOAT;    pvia[1].offset = 24;
    if (!createDepthCasterPipeline(windowOpaque_, device_, windowPass_.get(), casterLayout_.get(), propVert_.get(), opaqueFrag_.get(), pvib, pvia) ||
        !createDepthCasterPipeline(windowAlpha_, device_, windowPass_.get(), casterLayout_.get(), propVert_.get(), alphaFrag_.get(), pvib, pvia) ||
        !createDepthCasterPipeline(objectOpaque_, device_, objectPass_.get(), casterLayout_.get(), propVert_.get(), opaqueFrag_.get(), pvib, pvia) ||
        !createDepthCasterPipeline(objectAlpha_, device_, objectPass_.get(), casterLayout_.get(), propVert_.get(), alphaFrag_.get(), pvib, pvia)) {
        blog("caster pipelines failed: no bake.");
        return false;
    }
    nameObject(device_, windowOpaque_.get(), "shadow.bake.windowOpaque");
    nameObject(device_, objectOpaque_.get(), "shadow.bake.objectOpaque");

    // The three compute passes share one set: the same binding list gives identically defined layouts.
    const std::vector<ComputeBinding> cb = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC}, {9, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER}, {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
        {4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}, {5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}, {6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}, {7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
        {8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}};
    if (!pcss_.create(device_, dir + "shadow_bake_pcss.comp.spv", cb, "shadow.bake.pcss") ||
        !march_.create(device_, dir + "shadow_bake_terrain_march.comp.spv", cb, "shadow.bake.march") ||
        !resolve_.create(device_, dir + "shadow_bake_resolve.comp.spv", cb, "shadow.bake.resolve")) {
        blog("compute SPIR-V missing: no bake.");
        return false;
    }
    const VkDescriptorPoolSize csizes[4] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2},
                                            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4}};
    VkDescriptorPoolCreateInfo cpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    cpci.maxSets = 1; cpci.poolSizeCount = 4; cpci.pPoolSizes = csizes;
    if (!create(computePool_, device_, [&](auto* h) { return vkCreateDescriptorPool(device_, &cpci, nullptr, h); })) { blog("compute pool failed."); return false; }
    const VkDescriptorSetLayout cdsl = pcss_.setLayout();
    VkDescriptorSetAllocateInfo cai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    cai.descriptorPool = computePool_.get(); cai.descriptorSetCount = 1; cai.pSetLayouts = &cdsl;
    if (vkAllocateDescriptorSets(device_, &cai, &computeSet_) != VK_SUCCESS) { blog("compute set failed."); return false; }

    // Outside the window everything is lit: a white border is the far plane.
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    if (!create(depthRawSampler_, device_, [&](auto* h) { return vkCreateSampler(device_, &sci, nullptr, h); })) { blog("sampler failed."); return false; }
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.compareEnable = VK_TRUE;
    sci.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    if (!create(depthCompareSampler_, device_, [&](auto* h) { return vkCreateSampler(device_, &sci, nullptr, h); })) { blog("sampler failed."); return false; }

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(phys_, &props);
    timestampPeriodNs_ = props.limits.timestampPeriod;
    uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys_, &families, nullptr);
    std::vector<VkQueueFamilyProperties> fam(families);
    vkGetPhysicalDeviceQueueFamilyProperties(phys_, &families, fam.data());
    const uint32_t gfx = getOrCreateEngineVulkanContext().graphicsFamily();
    timed_ = gfx < families && fam[gfx].timestampValidBits != 0;
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(phys_, kMaskFmt, &fp);
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if ((fp.optimalTilingFeatures & need) != need) { blog("R8 cannot be blitted with a linear filter here: no bake."); return false; }
    inited_ = true;
    return true;
}

void ShadowBake::releaseTransients() {
    windowFb_.reset(); windowView_.reset(); windowImage_.reset();
    for (int i = 0; i < 4; ++i) { scratchView_[i].reset(); scratch_[i].reset(); }
    staging_.reset(); tileParams_.reset(); passParams_.reset(); casterUbo_.reset(); instances_.reset(); heightBuf_.reset();
    queries_.reset();
    queryCount_ = 0;
    leaves_.clear(); leaves_.shrink_to_fit();
    tiles_.clear(); runs_.clear();
    heights_.clear(); heights_.shrink_to_fit();
    transientBytes_ = 0;
}

void ShadowBake::invalidate() {
    releaseTransients();
    objectFb_.reset(); objectView_.reset(); objectImage_.reset();
    maskView_.reset(); maskImage_.reset();
    terrainVbo_.reset(); terrainIbo_.reset(); objectInstances_.reset(); objectUbo_.reset();
    terrainIndexCount_ = 0;
    maskTexels_ = 0; maskMips_ = 0;
    maskInitialized_ = objectInitialized_ = false;
    maskReady_ = objectReady_ = false;
    recorded_ = submitted_ = recordedMask_ = false;
    bakedKey_ = objectKey_ = recordedKey_ = recordedObjectKey_ = 0;
    objectGeneration_ = recordedObjectGeneration_ = 0;
    waitFrames_ = 0;
    loggedWait_ = loggedHeightDrift_ = false;
    bakedHeightVersion_ = 0;
    layout_ = {};
}

// The bake's inputs: the caster set of the mission, the terrain, the sun, the water, the static light matrix. The mask key leaves out the height version
// once a mask exists: a crater or an editor stroke must not rebake the whole map (dirty tiles arrive with P5).
bool ShadowBake::inputsReady(const ShadowBakeInputs& in, uint64_t& key, uint64_t& objectKey) {
    const mc2shadow::StaticCasterSet& set = mc2shadow::staticCasterSet();
    if (set.version() == 0) return false;
    // A map whose objects cast nothing has no prop geometry; its bake is the terrain self-shadow alone.
    if (!set.entries().empty() && (in.vbo == VK_NULL_HANDLE || in.ibo == VK_NULL_HANDLE)) return false;
    const mc2terrain::TerrainFieldStore* store = gos_TerrainFieldStore();
    if (!store || store->mapSide() < 2) return false;
    StaticShadowSpec spec;
    if (!gos_GetStaticShadowSpec(&spec)) return false;
    if (!in.alphaReady && !in.alphaAbsent && !set.entries().empty()) {
        if (++waitFrames_ < kMaxAlphaWaitFrames) return false;
        if (!loggedWait_) { loggedWait_ = true; blog("the alpha-ON array never arrived: baking with foliage casters opaque."); }
    }
    int side = 0;
    float halfMap = 0.0f;
    gos_GetTerrainChunkGridInfo(&side, &halfMap);
    if (side != store->mapSide()) return false;
    float lx = 0.0f, ly = 0.0f, lz = 1.0f;
    gos_GetTerrainLightDir(&lx, &ly, &lz);
    const mc2shadow::Vec3 sun = mc2shadow::clampSunAltitude({lx, ly, lz}, mc2ShadowMinSunAltDeg());
    gosPostProcess* pp = getGosPostProcess();
    const float water = pp ? pp->getWaterElevation() : -1e30f;

    uint64_t h = mc2shadow::fnv1a(nullptr, 0);
    const uint64_t setVersion = set.version();
    h = mc2shadow::fnv1a(&setVersion, sizeof(setVersion), h);
    h = mc2shadow::fnv1a(&side, sizeof(side), h);
    h = mc2shadow::fnv1a(&halfMap, sizeof(halfMap), h);
    h = mc2shadow::fnv1a(&sun, sizeof(sun), h);
    h = mc2shadow::fnv1a(&water, sizeof(water), h);
    const float art[4] = {mc2shadow::kSunAngularRadiusDeg, mc2shadow::kFadeStartWorld, mc2shadow::kFadeEndWorld, mc2shadow::kFadeMax};
    h = mc2shadow::fnv1a(art, sizeof(art), h);
    const uint64_t hv = maskReady_ ? bakedHeightVersion_ : store->heightVersion();
    h = mc2shadow::fnv1a(&hv, sizeof(hv), h);
    if (maskReady_ && store->heightVersion() != bakedHeightVersion_ && !loggedHeightDrift_) {
        loggedHeightDrift_ = true;
        blog("terrain heights changed after the bake (crater or edit): the mask keeps the baked heights until dirty tiles exist (P5).");
    }
    key = h | 1u;
    const uint64_t gen = spec.generation;
    objectKey = mc2shadow::fnv1a(&gen, sizeof(gen), key) | 1u;
    return true;
}

bool ShadowBake::ensureProducts(int maskTexels) {
    if (maskImage_ && maskTexels_ == maskTexels) return true;
    // A new size happens at a map boundary, after the island's idle; within a map the mask is rebaked in place.
    uint32_t mips = 1;
    while ((maskTexels >> mips) >= 1 && mips < 16) ++mips;
    if (!makeImage(maskImage_, maskView_, allocator_, device_, kMaskFmt, static_cast<uint32_t>(maskTexels), mips,
                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT, "shadow.bake.mask")) {
        blog("mask image creation failed.");
        return false;
    }
    maskTexels_ = maskTexels;
    maskMips_ = mips;
    maskInitialized_ = false;
    if (!objectImage_) {
        if (!makeImage(objectImage_, objectView_, allocator_, device_, kObjectFmt, kObjectMapSize, 1,
                       VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, "shadow.bake.objectMap") ||
            !makeFramebuffer(objectFb_, device_, objectPass_.get(), objectView_.get(), kObjectMapSize)) {
            blog("object map creation failed.");
            return false;
        }
        objectInitialized_ = false;
    }
    return true;
}

bool ShadowBake::ensureTransients() {
    if (!windowImage_) {
        if (!makeImage(windowImage_, windowView_, allocator_, device_, kWindowFmt, mc2shadow::kWindowMaxTexels, 1,
                       VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, "shadow.bake.window") ||
            !makeFramebuffer(windowFb_, device_, windowPass_.get(), windowView_.get(), mc2shadow::kWindowMaxTexels)) {
            blog("caster window creation failed.");
            return false;
        }
        const VkFormat fmts[4] = {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32_SFLOAT};
        const char* names[4] = {"shadow.bake.props", "shadow.bake.terrain", "shadow.bake.blur", "shadow.bake.combined"};
        for (int i = 0; i < 4; ++i)
            if (!makeImage(scratch_[i], scratchView_[i], allocator_, device_, fmts[i], mc2shadow::kRegionTexels, 1, VK_IMAGE_USAGE_STORAGE_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT, names[i])) { blog("scratch image creation failed."); return false; }
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = static_cast<VkDeviceSize>(mc2shadow::kTileTexels) * mc2shadow::kTileTexels;
        bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        if (!staging_.create(allocator_, device_, bci, aci, "shadow.bake.staging")) { blog("staging buffer failed."); return false; }
    }
    const VkDeviceSize region = static_cast<VkDeviceSize>(mc2shadow::kRegionTexels) * mc2shadow::kRegionTexels;
    transientBytes_ = static_cast<VkDeviceSize>(mc2shadow::kWindowMaxTexels) * mc2shadow::kWindowMaxTexels * 4 + region * (8 + 4 + 8 + 4) +
                      static_cast<VkDeviceSize>(mc2shadow::kTileTexels) * mc2shadow::kTileTexels;
    return true;
}

// Each type's local box, from the CPU copy of the shared geometry the island uploaded.
bool ShadowBake::computeTypeBounds(const ShadowBakeInputs& in) {
    const void* vboCpu = nullptr;
    const void* iboCpu = nullptr;
    uint64_t vboBytes = 0, iboBytes = 0;
    if (!mc2_static_prop_geometry_cpu(&vboCpu, &vboBytes, &iboCpu, &iboBytes) || !vboCpu || !iboCpu) return false;
    const uint32_t stride = mc2_static_prop_vertex_stride();
    const auto* vb = static_cast<const uint8_t*>(vboCpu);
    const auto* ib = static_cast<const uint32_t*>(iboCpu);
    const uint64_t indexCount = iboBytes / 4, vertexCount = stride ? vboBytes / stride : 0;
    const size_t types = (std::min)(in.typeFirstPacket.size(), in.typePacketCount.size());
    typeBounds_.assign(types, TypeBounds{{1e30f, 1e30f, 1e30f}, {-1e30f, -1e30f, -1e30f}, false});
    for (size_t t = 0; t < types; ++t) {
        TypeBounds& tb = typeBounds_[t];
        for (uint32_t k = 0; k < in.typePacketCount[t]; ++k) {
            const uint32_t pk = in.typeFirstPacket[t] + k;
            if (pk >= in.packets.size()) break;
            const Mc2ShadowPacket& p = in.packets[pk];
            for (uint64_t i = p.firstIndex; i < static_cast<uint64_t>(p.firstIndex) + p.indexCount && i < indexCount; ++i) {
                const uint64_t v = static_cast<uint64_t>(ib[i]) + p.baseVertex;
                if (v >= vertexCount) continue;
                float pos[3];
                std::memcpy(pos, vb + v * stride, sizeof(pos));
                for (int a = 0; a < 3; ++a) { tb.mn[a] = (std::min)(tb.mn[a], pos[a]); tb.mx[a] = (std::max)(tb.mx[a], pos[a]); }
                tb.valid = true;
            }
        }
    }
    return true;
}

void ShadowBake::drawRuns(VkCommandBuffer cb, const ShadowBakeInputs& in, std::span<const InstRun> runs, bool& alphaBound, VkPipeline opaque, VkPipeline alpha) {
    for (const InstRun& run : runs) {
        if (run.typeID >= in.typeFirstPacket.size() || run.typeID >= in.typePacketCount.size()) continue;
        const uint32_t first = in.typeFirstPacket[run.typeID], count = in.typePacketCount[run.typeID];
        for (uint32_t k = 0; k < count; ++k) {
            const uint32_t pk = first + k;
            if (pk >= in.packets.size()) break;
            const Mc2ShadowPacket& pkt = in.packets[pk];
            const bool alphaPacket = in.alphaReady && in.alphaSet != VK_NULL_HANDLE && pk < in.packetAlpha.size() && in.packetAlpha[pk].layer >= 0;
            if (alphaPacket) {
                if (!alphaBound) {
                    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, alpha);
                    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, casterLayout_.get(), 1, 1, &in.alphaSet, 0, nullptr);
                    alphaBound = true;
                }
                const CasterPacketAlpha& pa = in.packetAlpha[pk];
                const struct { float scale[2]; int32_t layer; int32_t pad; } pc = {{pa.scaleX, pa.scaleY}, pa.layer, 0};
                vkCmdPushConstants(cb, casterLayout_.get(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
            } else if (alphaBound) {
                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, opaque);
                alphaBound = false;
            }
            vkCmdDrawIndexed(cb, pkt.indexCount, run.count, pkt.firstIndex, static_cast<int32_t>(pkt.baseVertex), run.firstInstance);
        }
    }
}

bool ShadowBake::update(VkCommandBuffer cb, uint64_t epoch, const ShadowBakeInputs& in) {
    if (!inited_) return false;
    retire(epoch);
    if (recorded_) return false;   // the previous bake has not retired: its descriptors and buffers are still in flight
    uint64_t key = 0, objectKey = 0;
    if (!inputsReady(in, key, objectKey)) return false;
    const bool needMask = key != bakedKey_;
    const bool needObject = needMask || objectKey != objectKey_;
    if (!needMask && !needObject) return false;
    recordedKey_ = key;
    recordedObjectKey_ = objectKey;
    recordedMask_ = needMask;
    if (!recordBake(cb, in)) {
        // A failed input is not retried every frame: the keys are taken; a producer edit (new caster set, sun, map) asks again.
        bakedKey_ = key;
        objectKey_ = objectKey;
        return false;
    }
    recorded_ = true;
    submitted_ = false;
    return true;
}

bool ShadowBake::wantsFrame() const {
    if (!inited_) return false;
    if (recorded_) return true;
    return mc2shadow::staticCasterSet().version() != 0 && !maskReady_;
}

void ShadowBake::markSubmitted(uint64_t epoch) {
    if (!recorded_ || submitted_) return;
    submitted_ = true;
    submittedEpoch_ = epoch;
    if (recordedMask_) { bakedKey_ = recordedKey_; maskReady_ = true; }
    objectKey_ = recordedObjectKey_;
    objectGeneration_ = recordedObjectGeneration_;
    objectReady_ = true;
}

// The island opened this epoch's recording after waiting its parity's fence; on the bake's parity two or more epochs later that fence covered the bake's
// submit, so its timestamps are final and its transients idle.
void ShadowBake::retire(uint64_t epoch) {
    if (!recorded_) return;
    if (!submitted_) { recorded_ = false; return; }   // recorded into a frame that never submitted: the products stay as they were
    if (epoch < submittedEpoch_ + 2 || (epoch & 1u) != (submittedEpoch_ & 1u)) return;
    recorded_ = false;
    submitted_ = false;
    if (queries_ && queryCount_ > 0) {
        // A mask bake wrote every query; an object-map-only rebuild wrote its last two.
        const size_t tiles = tiles_.size();
        const uint32_t tail = static_cast<uint32_t>(tiles * kQueriesPerTile);
        const uint32_t first = recordedMask_ ? 0u : tail + 2;
        std::vector<uint64_t> ts(queryCount_, 0);
        if (vkGetQueryPoolResults(device_, queries_.get(), first, queryCount_ - first, (queryCount_ - first) * sizeof(uint64_t), ts.data() + first,
                                  sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            const double ms = timestampPeriodNs_ * 1e-6;
            auto span = [&](uint32_t a, uint32_t b) { return static_cast<double>(ts[b] - ts[a]) * ms; };
            const double object = span(tail + 2, tail + 3);
            if (recordedMask_) {
                double stage[4] = {0, 0, 0, 0};
                for (size_t t = 0; t < tiles; ++t)
                    for (uint32_t s = 0; s < 4; ++s) stage[s] += span(static_cast<uint32_t>(t * kQueriesPerTile + s), static_cast<uint32_t>(t * kQueriesPerTile + s + 1));
                mc2::fmt::print_err("[SHADOW_BAKE] gpu_ms window={:.2f} pcss={:.2f} march={:.2f} resolve={:.2f} mips={:.2f} object_map={:.2f} total={:.2f} cpu_ms={:.2f} tiles={}\n",
                                    stage[0], stage[1], stage[2], stage[3], span(tail, tail + 1), object, span(0, tail + 3), cpuMs_, mc2::fmt::as_size(tiles));
            } else {
                mc2::fmt::print_err("[SHADOW_BAKE] gpu_ms object_map={:.2f} cpu_ms={:.2f}\n", object, cpuMs_);
            }
        }
    }
    const VkDeviceSize maskBytes = static_cast<VkDeviceSize>(maskTexels_) * maskTexels_ * 4 / 3;
    const VkDeviceSize objectBytes = static_cast<VkDeviceSize>(kObjectMapSize) * kObjectMapSize * 2;
    mc2::fmt::print_err("[SHADOW_BAKE] vram resident_mb={:.1f} (mask {:.1f} + object_map {:.1f}) transient_peak_mb={:.1f} released\n",
                        static_cast<double>(maskBytes + objectBytes) / 1048576.0, static_cast<double>(maskBytes) / 1048576.0,
                        static_cast<double>(objectBytes) / 1048576.0, static_cast<double>(transientBytes_) / 1048576.0);
    recordedMask_ = false;
    releaseTransients();
}

bool ShadowBake::recordBake(VkCommandBuffer cb, const ShadowBakeInputs& in) {
    using clk = std::chrono::steady_clock;
    const clk::time_point t0 = clk::now();
    ZoneScopedN("ShadowBake.Record");
    const mc2terrain::TerrainFieldStore* store = gos_TerrainFieldStore();
    StaticShadowSpec spec;
    if (!store || !gos_GetStaticShadowSpec(&spec)) return false;
    gos_GetTerrainChunkGridInfo(&mapSide_, &halfMap_);
    float lx = 0.0f, ly = 0.0f, lz = 1.0f;
    gos_GetTerrainLightDir(&lx, &ly, &lz);
    sun_ = mc2shadow::clampSunAltitude({lx, ly, lz}, mc2ShadowMinSunAltDeg());
    basis_ = mc2shadow::lightBasis(sun_);
    gosPostProcess* pp = getGosPostProcess();
    waterZ_ = pp ? pp->getWaterElevation() : -1e30f;
    const float tanT = std::tan(mc2shadow::kSunAngularRadiusDeg * 0.01745329252f);
    // The pre-T15 fade strength, scaled by the sun's altitude as SHADOW-ALT-SCALE had it: a high sun's short shadows barely fade.
    const float soft = 1.0f - std::clamp((sun_.z - 0.21f) / (0.57f - 0.21f), 0.0f, 1.0f);
    const float softS = soft * soft * (3.0f - 2.0f * soft);
    fadeMax_ = mc2shadow::kFadeMax * (0.25f + 0.75f * softS);

    // Heights: the visual bake when the map has one (what the terrain draws), else the 128 wu grid.
    const int coarseSide = store->mapSide();
    if (!store->visualHeight().empty() && store->visualSide() == (coarseSide - 1) * 4 + 1) {
        heightSide_ = store->visualSide();
        heightSpacing_ = mc2shadow::kWorldUnitsPerVertex / 4.0f;
        heights_.assign(store->visualHeight().begin(), store->visualHeight().begin() + static_cast<size_t>(heightSide_) * heightSide_);
    } else {
        heightSide_ = coarseSide;
        heightSpacing_ = mc2shadow::kWorldUnitsPerVertex;
        heights_ = store->height();
    }
    if (heights_.size() != static_cast<size_t>(heightSide_) * heightSide_) { blog("height field malformed: no bake."); return false; }
    terrainTop_ = -1e30f;
    for (float h : heights_) terrainTop_ = (std::max)(terrainTop_, h);
    if (recordedMask_) bakedHeightVersion_ = store->heightVersion();

    if (recordedMask_) {
        layout_ = mc2shadow::maskLayout(mapSide_, halfMap_, kMaskTargetTexelWorld, kMaskCapTexels);
        if (layout_.texels == 0) { blog("map too small for a mask: no bake."); return false; }
    }
    if (!ensureProducts(layout_.texels > 0 ? layout_.texels : maskTexels_)) return false;
    typeBounds_.clear();
    if (in.vbo != VK_NULL_HANDLE && !computeTypeBounds(in)) { blog("static-prop geometry not readable: no bake."); return false; }

    // Casters: the gameplay set resolved to registry leaves, each with its world box and light-space box.
    mc2shadow::staticCasterSet().resolve(leaves_);
    std::vector<mc2shadow::LightBox> boxes;
    std::vector<float> tops;
    std::vector<uint32_t> live;
    boxes.reserve(leaves_.size());
    float casterTop = -1e30f;
    for (uint32_t i = 0; i < leaves_.size(); ++i) {
        const GpuStaticPropInstance& leaf = leaves_[i];
        if (leaf.typeID >= typeBounds_.size() || !typeBounds_[leaf.typeID].valid) continue;
        mc2shadow::Vec3 lo, hi;
        instanceBox(leaf.modelMatrix, typeBounds_[leaf.typeID].mn, typeBounds_[leaf.typeID].mx, lo, hi);
        boxes.push_back(mc2shadow::lightBox(basis_, lo, hi));
        tops.push_back(hi.z);
        live.push_back(i);
        casterTop = (std::max)(casterTop, hi.z);
    }
    if (casterTop < -1e29f) casterTop = terrainTop_;

    // Object map instances: every live leaf, sorted by type.
    std::vector<uint32_t> objOrder = live;
    std::sort(objOrder.begin(), objOrder.end(), [this](uint32_t a, uint32_t b) { return leaves_[a].typeID < leaves_[b].typeID; });
    std::vector<GpuStaticPropInstance> objInst;
    objInst.reserve(objOrder.size());
    std::vector<uint32_t> objTypes;
    for (uint32_t i : objOrder) { objInst.push_back(leaves_[i]); objTypes.push_back(leaves_[i].typeID); }
    objectRuns_.clear();
    appendRuns(objectRuns_, objTypes, 0);

    // Tiles: the window each needs and the casters that can reach it.
    tiles_.clear();
    runs_.clear();
    std::vector<GpuStaticPropInstance> tileInst;
    std::vector<BakeTileParams> params;
    const float region = static_cast<float>(mc2shadow::kRegionTexels) * layout_.texelWorld;
    if (recordedMask_) {
        const int tps = layout_.tilesPerSide;
        for (int ty = 0; ty < tps; ++ty) {
            for (int tx = 0; tx < tps; ++tx) {
                const mc2shadow::Rect r = mc2shadow::tileRegionWorld(layout_, tx, ty);
                // The region's receiver height range, from the height samples it covers (one sample of margin).
                float hMin = 1e30f, hMax = -1e30f;
                const float hx0 = -halfMap_, hyN = halfMap_;
                const int gx0 = std::clamp(static_cast<int>(std::floor((r.x0 - hx0) / heightSpacing_)) - 1, 0, heightSide_ - 1);
                const int gx1 = std::clamp(static_cast<int>(std::ceil((r.x1 - hx0) / heightSpacing_)) + 1, 0, heightSide_ - 1);
                const int gy0 = std::clamp(static_cast<int>(std::floor((hyN - r.y1) / heightSpacing_)) - 1, 0, heightSide_ - 1);
                const int gy1 = std::clamp(static_cast<int>(std::ceil((hyN - r.y0) / heightSpacing_)) + 1, 0, heightSide_ - 1);
                for (int gy = gy0; gy <= gy1; ++gy)
                    for (int gx = gx0; gx <= gx1; ++gx) {
                        const float h = (std::max)(heights_[static_cast<size_t>(gx) + static_cast<size_t>(gy) * heightSide_], waterZ_);
                        hMin = (std::min)(hMin, h); hMax = (std::max)(hMax, h);
                    }
                TileWork tw;
                tw.window = mc2shadow::casterWindow(basis_, r, hMin, hMax, casterTop, tanT);
                tw.casterTop = hMax;
                std::vector<uint32_t> picked;
                for (size_t c = 0; c < boxes.size(); ++c)
                    if (mc2shadow::casterInWindow(boxes[c], tw.window)) { picked.push_back(live[c]); tw.casterTop = (std::max)(tw.casterTop, tops[c]); }
                std::sort(picked.begin(), picked.end(), [this](uint32_t a, uint32_t b) { return leaves_[a].typeID < leaves_[b].typeID; });
                std::vector<uint32_t> types;
                for (uint32_t i : picked) types.push_back(leaves_[i].typeID);
                tw.firstRun = static_cast<uint32_t>(runs_.size());
                appendRuns(runs_, types, static_cast<uint32_t>(tileInst.size()));
                tw.runCount = static_cast<uint32_t>(runs_.size()) - tw.firstRun;
                for (uint32_t i : picked) tileInst.push_back(leaves_[i]);
                tiles_.push_back(tw);

                BakeTileParams p{};
                mc2shadow::windowMatrix(basis_, tw.window, p.window);
                p.region[0] = r.x0; p.region[1] = r.y1; p.region[2] = layout_.texelWorld; p.region[3] = waterZ_;
                p.regionInfo[0] = mc2shadow::kRegionTexels;
                p.regionInfo[1] = tx * mc2shadow::kTileTexels - mc2shadow::kTileApron;
                p.regionInfo[2] = ty * mc2shadow::kTileTexels - mc2shadow::kTileApron;
                p.regionInfo[3] = mc2shadow::kTileApron;
                p.sun[0] = sun_.x; p.sun[1] = sun_.y; p.sun[2] = sun_.z; p.sun[3] = tanT;
                p.windowInfo[0] = tw.window.dMin; p.windowInfo[1] = tw.window.dMax;
                p.windowInfo[2] = tw.window.texelWorld; p.windowInfo[3] = static_cast<float>(tw.window.texels);
                p.heightInfo[0] = hx0; p.heightInfo[1] = hyN; p.heightInfo[2] = heightSpacing_; p.heightInfo[3] = static_cast<float>(heightSide_);
                p.fade[0] = mc2shadow::kFadeStartWorld; p.fade[1] = mc2shadow::kFadeEndWorld; p.fade[2] = fadeMax_; p.fade[3] = tw.casterTop;
                p.march[0] = terrainTop_; p.march[1] = 4.0f; p.march[2] = 1.07f; p.march[3] = layout_.span * 1.5f;
                params.push_back(p);
            }
        }
        (void)region;
    }
    binnedInstances_ = static_cast<uint32_t>(tileInst.size());

    // Uploads.
    if (recordedMask_ && !ensureTransients()) return false;
    const uint32_t tileCount = static_cast<uint32_t>(tiles_.size());
    if (!makeHostBuffer(casterUbo_, allocator_, device_, (tileCount + 1) * kSlice, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, "shadow.bake.casterUbo") ||
        !makeHostBuffer(instances_, allocator_, device_, (std::max<size_t>)(tileInst.size(), 1) * sizeof(GpuStaticPropInstance), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "shadow.bake.instances") ||
        !makeHostBuffer(objectInstances_, allocator_, device_, (std::max<size_t>)(objInst.size(), 1) * sizeof(GpuStaticPropInstance), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "shadow.bake.objectInstances")) {
        blog("caster buffers failed.");
        return false;
    }
    {
        std::vector<uint8_t> ubo((tileCount + 1) * kSlice, 0);
        for (uint32_t t = 0; t < tileCount; ++t) {
            CasterUBO u{};
            std::memcpy(u.lightSpace, params[t].window, sizeof(u.lightSpace));
            std::memcpy(ubo.data() + t * kSlice, &u, sizeof(u));
        }
        CasterUBO o{};
        std::memcpy(o.lightSpace, spec.lightSpace, sizeof(o.lightSpace));
        std::memcpy(ubo.data() + tileCount * kSlice, &o, sizeof(o));
        if (!casterUbo_.upload(ubo.data(), ubo.size())) { blog("caster UBO upload failed."); return false; }
        if (!tileInst.empty() && !instances_.upload(tileInst.data(), tileInst.size() * sizeof(GpuStaticPropInstance))) { blog("instance upload failed."); return false; }
        if (!objInst.empty() && !objectInstances_.upload(objInst.data(), objInst.size() * sizeof(GpuStaticPropInstance))) { blog("instance upload failed."); return false; }
    }
    {
        const VkDescriptorBufferInfo ubo{casterUbo_.get(), 0, sizeof(CasterUBO)};
        const VkDescriptorBufferInfo win{instances_.get(), 0, VK_WHOLE_SIZE};
        const VkDescriptorBufferInfo obj{objectInstances_.get(), 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w[4]{};
        for (int i = 0; i < 4; ++i) { w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].descriptorCount = 1; }
        w[0].dstSet = windowCasterSet_; w[0].dstBinding = 0; w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; w[0].pBufferInfo = &ubo;
        w[1].dstSet = windowCasterSet_; w[1].dstBinding = 1; w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[1].pBufferInfo = &win;
        w[2].dstSet = objectCasterSet_; w[2].dstBinding = 0; w[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; w[2].pBufferInfo = &ubo;
        w[3].dstSet = objectCasterSet_; w[3].dstBinding = 1; w[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[3].pBufferInfo = &obj;
        vkUpdateDescriptorSets(device_, 4, w, 0, nullptr);
    }
    if (recordedMask_) {
        if (!makeHostBuffer(tileParams_, allocator_, device_, (std::max<uint32_t>)(tileCount, 1) * kSlice, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, "shadow.bake.tileParams") ||
            !makeHostBuffer(passParams_, allocator_, device_, 3 * kSlice, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, "shadow.bake.passParams") ||
            !makeHostBuffer(heightBuf_, allocator_, device_, heights_.size() * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "shadow.bake.heights")) {
            blog("bake buffers failed.");
            return false;
        }
        std::vector<uint8_t> tp(std::max<uint32_t>(tileCount, 1) * kSlice, 0);
        for (uint32_t t = 0; t < tileCount; ++t) std::memcpy(tp.data() + t * kSlice, &params[t], sizeof(BakeTileParams));
        std::vector<uint8_t> pp3(3 * kSlice, 0);
        for (int32_t m = 0; m < 3; ++m) std::memcpy(pp3.data() + m * kSlice, &m, sizeof(m));
        if (!tileParams_.upload(tp.data(), tp.size()) || !passParams_.upload(pp3.data(), pp3.size()) ||
            !heightBuf_.upload(heights_.data(), heights_.size() * sizeof(float))) { blog("bake upload failed."); return false; }
        const VkDescriptorBufferInfo tile{tileParams_.get(), 0, sizeof(BakeTileParams)};
        const VkDescriptorBufferInfo pass{passParams_.get(), 0, 16};
        const VkDescriptorImageInfo raw{depthRawSampler_.get(), windowView_.get(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        const VkDescriptorImageInfo cmp{depthCompareSampler_.get(), windowView_.get(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        const VkDescriptorBufferInfo hb{heightBuf_.get(), 0, VK_WHOLE_SIZE};
        const VkDescriptorBufferInfo sb{staging_.get(), 0, VK_WHOLE_SIZE};
        VkDescriptorImageInfo si[4];
        for (int i = 0; i < 4; ++i) si[i] = {VK_NULL_HANDLE, scratchView_[i].get(), VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w[10]{};
        for (int i = 0; i < 10; ++i) { w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = computeSet_; w[i].descriptorCount = 1; }
        w[0].dstBinding = 0; w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; w[0].pBufferInfo = &tile;
        w[1].dstBinding = 9; w[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; w[1].pBufferInfo = &pass;
        w[2].dstBinding = 1; w[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[2].pImageInfo = &raw;
        w[3].dstBinding = 2; w[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[3].pImageInfo = &cmp;
        w[4].dstBinding = 3; w[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[4].pBufferInfo = &hb;
        for (int i = 0; i < 4; ++i) { w[5 + i].dstBinding = static_cast<uint32_t>(4 + i); w[5 + i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[5 + i].pImageInfo = &si[i]; }
        w[9].dstBinding = 8; w[9].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[9].pBufferInfo = &sb;
        vkUpdateDescriptorSets(device_, 10, w, 0, nullptr);
    }

    // Timestamps: five per tile, then mips (2) and the object map (2).
    queryCount_ = 0;
    if (timed_) {
        queryCount_ = tileCount * kQueriesPerTile + 4;
        VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qci.queryCount = queryCount_;
        if (!create(queries_, device_, [&](auto* h) { return vkCreateQueryPool(device_, &qci, nullptr, h); })) queryCount_ = 0;
        else vkCmdResetQueryPool(cb, queries_.get(), 0, queryCount_);
    }
    auto stamp = [&](uint32_t q) { if (queryCount_) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries_.get(), q); };

    if (recordedMask_) {
        MC2_VK_GPU_SCOPE(gpuScope, cb, "VKIsl.Shadow.Bake");
        // The mask's every level becomes a transfer target (its last bake, if any, was read by fragment shaders); the scratch images become GENERAL.
        VkImageMemoryBarrier pre[5];
        pre[0] = imageBarrier(maskImage_.get(), VK_IMAGE_ASPECT_COLOR_BIT, 0, maskMips_,
                              maskInitialized_ ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              0, VK_ACCESS_TRANSFER_WRITE_BIT);
        for (int i = 0; i < 4; ++i)
            pre[1 + i] = imageBarrier(scratch_[i].get(), VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
                                      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 5, pre);
        const uint32_t regionGroups = (mc2shadow::kRegionTexels + 7) / 8;
        const VkDeviceSize zero = 0;
        for (uint32_t t = 0; t < tileCount; ++t) {
            const TileWork& tw = tiles_[t];
            const uint32_t q = t * kQueriesPerTile;
            const uint32_t tileOff = t * static_cast<uint32_t>(kSlice);
            stamp(q);
            // The previous tile's reads of the scratch images and its staging copy finish before this tile writes them.
            cmdBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
            VkClearValue clear{};
            clear.depthStencil = {1.0f, 0};
            VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
            rbi.renderPass = windowPass_.get(); rbi.framebuffer = windowFb_.get();
            rbi.renderArea = {{0, 0}, {static_cast<uint32_t>(mc2shadow::kWindowMaxTexels), static_cast<uint32_t>(mc2shadow::kWindowMaxTexels)}};
            rbi.clearValueCount = 1; rbi.pClearValues = &clear;
            vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
            if (tw.runCount > 0) {
                const float side = static_cast<float>(mc2shadow::kWindowMaxTexels);
                const VkViewport vp{0.0f, 0.0f, side, side, 0.0f, 1.0f};
                const VkRect2D sc{{0, 0}, {static_cast<uint32_t>(mc2shadow::kWindowMaxTexels), static_cast<uint32_t>(mc2shadow::kWindowMaxTexels)}};
                vkCmdSetViewport(cb, 0, 1, &vp);
                vkCmdSetScissor(cb, 0, 1, &sc);
                vkCmdSetDepthBias(cb, 0.0f, 0.0f, 0.0f);
                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, windowOpaque_.get());
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, casterLayout_.get(), 0, 1, &windowCasterSet_, 1, &tileOff);
                vkCmdBindVertexBuffers(cb, 0, 1, &in.vbo, &zero);
                vkCmdBindIndexBuffer(cb, in.ibo, 0, VK_INDEX_TYPE_UINT32);
                bool alphaBound = false;
                drawRuns(cb, in, std::span<const InstRun>(runs_.data() + tw.firstRun, tw.runCount), alphaBound, windowOpaque_.get(), windowAlpha_.get());
            }
            vkCmdEndRenderPass(cb);
            stamp(q + 1);
            const uint32_t offs0[2] = {tileOff, 0};
            pcss_.bind(cb, computeSet_, offs0);
            vkCmdDispatch(cb, regionGroups, regionGroups, 1);
            stamp(q + 2);
            march_.bind(cb, computeSet_, offs0);
            vkCmdDispatch(cb, regionGroups, regionGroups, 1);
            stamp(q + 3);
            for (uint32_t mode = 0; mode < 3; ++mode) {
                cmdBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
                const uint32_t offs[2] = {tileOff, mode * static_cast<uint32_t>(kSlice)};
                resolve_.bind(cb, computeSet_, offs);
                if (mode < 2) vkCmdDispatch(cb, regionGroups, regionGroups, 1);
                else vkCmdDispatch(cb, mc2shadow::kTileTexels / 4 / 8, mc2shadow::kTileTexels / 8, 1);
            }
            cmdBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            const int tx = static_cast<int>(t) % layout_.tilesPerSide, ty = static_cast<int>(t) / layout_.tilesPerSide;
            VkBufferImageCopy copy{};
            copy.bufferRowLength = mc2shadow::kTileTexels;
            copy.bufferImageHeight = mc2shadow::kTileTexels;
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageOffset = {tx * mc2shadow::kTileTexels, ty * mc2shadow::kTileTexels, 0};
            copy.imageExtent = {static_cast<uint32_t>(mc2shadow::kTileTexels), static_cast<uint32_t>(mc2shadow::kTileTexels), 1};
            vkCmdCopyBufferToImage(cb, staging_.get(), maskImage_.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            stamp(q + 4);
        }
        // Box mips by 2:1 linear blits, then every level is sampled.
        const uint32_t tail = tileCount * kQueriesPerTile;
        stamp(tail);
        int32_t size = layout_.texels;
        for (uint32_t m = 1; m < maskMips_; ++m) {
            VkImageMemoryBarrier toSrc = imageBarrier(maskImage_.get(), VK_IMAGE_ASPECT_COLOR_BIT, m - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSrc);
            const int32_t next = (std::max)(size / 2, 1);
            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m - 1, 0, 1};
            blit.srcOffsets[1] = {size, size, 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1};
            blit.dstOffsets[1] = {next, next, 1};
            vkCmdBlitImage(cb, maskImage_.get(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, maskImage_.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
            size = next;
        }
        VkImageMemoryBarrier toRead[2];
        uint32_t nRead = 0;
        if (maskMips_ > 1)
            toRead[nRead++] = imageBarrier(maskImage_.get(), VK_IMAGE_ASPECT_COLOR_BIT, 0, maskMips_ - 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT);
        toRead[nRead++] = imageBarrier(maskImage_.get(), VK_IMAGE_ASPECT_COLOR_BIT, maskMips_ - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                             nullptr, nRead, toRead);
        stamp(tail + 1);
        maskInitialized_ = true;
        gpuScope.end();
    }
    if (!recordObjectMap(cb, in, tileCount * kQueriesPerTile + 2)) return false;
    recordedObjectGeneration_ = spec.generation;
    cpuMs_ = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
    if (recordedMask_) {
        mc2::fmt::print_err("[SHADOW_BAKE] recorded mask={}^2 texel_wu={:.3f} tiles={} casters={} binned_instances={} sun=({:.3f},{:.3f},{:.3f}) theta_deg={:.2f} "
                            "fade=({:.0f},{:.0f},{:.3f}) water={:.1f} heights={}^2@{:.0f}wu cpu_ms={:.2f}\n",
                            layout_.texels, layout_.texelWorld, tileCount, mc2::fmt::as_size(leaves_.size()), binnedInstances_, sun_.x, sun_.y, sun_.z,
                            mc2shadow::kSunAngularRadiusDeg, mc2shadow::kFadeStartWorld, mc2shadow::kFadeEndWorld, fadeMax_, waterZ_, heightSide_,
                            heightSpacing_, cpuMs_);
    } else {
        mc2::fmt::print_err("[SHADOW_BAKE] object map re-recorded: generation={} casters={}\n", mc2::fmt::as_u64(spec.generation), mc2::fmt::as_size(leaves_.size()));
    }
    return true;
}

// The object map: the whole height field as the old static map drew it, then every caster, with the engine's static light matrix.
bool ShadowBake::recordObjectMap(VkCommandBuffer cb, const ShadowBakeInputs& in, uint32_t queryBase) {
    const mc2terrain::TerrainFieldStore* store = gos_TerrainFieldStore();
    StaticShadowSpec spec;
    if (!store || !gos_GetStaticShadowSpec(&spec)) return false;
    if (terrainIndexCount_ == 0) {
        const int side = store->mapSide();
        const std::vector<float>& height = store->height();
        const int half = side >> 1;
        std::vector<float> positions(static_cast<size_t>(side) * side * 3);
        for (int ay = 0; ay < side; ++ay)
            for (int ax = 0; ax < side; ++ax) {
                const size_t v = static_cast<size_t>(ax) + static_cast<size_t>(ay) * side;
                positions[v * 3 + 0] = static_cast<float>(ax - half) * mc2shadow::kWorldUnitsPerVertex;
                positions[v * 3 + 1] = static_cast<float>(half - ay) * mc2shadow::kWorldUnitsPerVertex;
                positions[v * 3 + 2] = height[v];
            }
        std::vector<uint32_t> indices;
        indices.reserve(static_cast<size_t>(side - 1) * (side - 1) * 6);
        for (int ay = 0; ay < side - 1; ++ay)
            for (int ax = 0; ax < side - 1; ++ax) {
                const uint32_t v0 = static_cast<uint32_t>(ax + ay * side), v1 = v0 + 1, v3 = v0 + static_cast<uint32_t>(side), v2 = v3 + 1;
                if ((ax & 1) == (ay & 1)) indices.insert(indices.end(), {v0, v1, v2, v0, v2, v3});
                else                      indices.insert(indices.end(), {v0, v1, v3, v1, v2, v3});
            }
        if (!terrainVbo_.createFilled(allocator_, device_, positions.data(), positions.size() * sizeof(float), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, "shadow.bake.terrainVbo") ||
            !terrainIbo_.createFilled(allocator_, device_, indices.data(), indices.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, "shadow.bake.terrainIbo")) {
            blog("object map terrain mesh failed.");
            return false;
        }
        terrainIndexCount_ = static_cast<uint32_t>(indices.size());
    }
    if (queryCount_) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries_.get(), queryBase);
    MC2_VK_GPU_SCOPE(gpuScope, cb, "VKIsl.Shadow.BakeObjectMap");
    VkClearValue clear{};
    clear.depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = objectPass_.get(); rbi.framebuffer = objectFb_.get();
    rbi.renderArea = {{0, 0}, {static_cast<uint32_t>(kObjectMapSize), static_cast<uint32_t>(kObjectMapSize)}};
    rbi.clearValueCount = 1; rbi.pClearValues = &clear;
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport vp{0.0f, 0.0f, static_cast<float>(kObjectMapSize), static_cast<float>(kObjectMapSize), 0.0f, 1.0f};
    const VkRect2D sc{{0, 0}, {static_cast<uint32_t>(kObjectMapSize), static_cast<uint32_t>(kObjectMapSize)}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    const uint32_t objOff = static_cast<uint32_t>(tiles_.size()) * static_cast<uint32_t>(kSlice);
    const VkDeviceSize zero = 0;
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, casterLayout_.get(), 0, 1, &objectCasterSet_, 1, &objOff);
    vkCmdSetDepthBias(cb, 0.0f, 0.0f, 0.0f);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, objectTerrain_.get());
    const VkBuffer tv = terrainVbo_.get();
    vkCmdBindVertexBuffers(cb, 0, 1, &tv, &zero);
    vkCmdBindIndexBuffer(cb, terrainIbo_.get(), 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cb, terrainIndexCount_, 1, 0, 0, 0);
    if (!objectRuns_.empty()) {
        vkCmdSetDepthBias(cb, spec.biasUnits, 0.0f, spec.biasFactor);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, objectOpaque_.get());
        vkCmdBindVertexBuffers(cb, 0, 1, &in.vbo, &zero);
        vkCmdBindIndexBuffer(cb, in.ibo, 0, VK_INDEX_TYPE_UINT32);
        bool alphaBound = false;
        drawRuns(cb, in, objectRuns_, alphaBound, objectOpaque_.get(), objectAlpha_.get());
    }
    vkCmdEndRenderPass(cb);
    gpuScope.end();
    if (queryCount_) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries_.get(), queryBase + 1);
    objectInitialized_ = true;
    return true;
}

}  // namespace mc2vk

#endif  // MC2_VULKAN_ISLAND
