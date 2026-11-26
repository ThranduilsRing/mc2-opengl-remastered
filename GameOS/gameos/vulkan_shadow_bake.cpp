// vulkan_shadow_bake.cpp -- see the header. Compiles to nothing unless MC2_VULKAN_ISLAND is defined.

#include "vulkan_shadow_bake.h"
#include "vulkan_shadow_bake_internal.h"

#include <vk_mem_alloc.h>

#include "../../RenderCore/axis_convert.h"
#include "../../RenderCore/gpu_abi/GpuStaticPropInstance.h"
#include "gos_postprocess.h"
#include "mc2fmt.h"
#include "diagnostic_trace.h"
#include "Gates/gate.h"
#include "gameos.hpp"   // gos_GetUserDataDirectory
#include "shadow_mask_asset.h"
#include "shadow_dirty_tiles.h"
#include "static_caster_set.h"
#include "terrain_field_store.h"
#include "terrain_diagonal.h"
#include "vk_depth_caster_pipeline.h"
#include "vk_dynamic_rendering.h"
#include "vk_gpu_profile.h"
#include "vk_island_util.h"
#include "hitch_stats.h"
#include "vulkan_color_prop_island.h"
#include "vulkan_context.h"

#include <tracy/Tracy.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include "../../mclib/battle_bench_sink.h"

extern void gos_GetTerrainLightDir(float* x, float* y, float* z);
extern void gos_GetTerrainChunkGridInfo(int* mapSide, float* halfMap);
extern bool terrainVisualDisplaceGateOn();
extern float terrainVisualDisplaceFarParam();
float mc2ShadowMinSunAltDeg();

namespace mc2vk {

using namespace shadow_bake_detail;

bool ShadowBake::init(VkDevice device, VmaAllocator allocator, VkPhysicalDevice phys, VkDescriptorSetLayout alphaDsl, std::string_view spvDir) {
    device_ = device;
    allocator_ = allocator;
    phys_ = phys;
    alphaDsl_ = alphaDsl;
    const std::string dir(spvDir);
    shaderContentHash_ = mc2shadow::fnv1a(nullptr, 0);
    for (const char* name : {"shadow_bake_pcss.comp.spv", "shadow_bake_terrain_march.comp.spv", "shadow_bake_resolve.comp.spv", "shadow_bake_bc4.comp.spv",
                             "shadow_prop.vert.spv", "shadow_prop_alpha.frag.spv"}) {
        std::FILE* file = std::fopen((dir + name).c_str(), "rb");
        if (!file) { blog("bake shader bytes missing: no bake."); return false; }
        std::array<uint8_t, 4096> bytes;
        size_t n;
        while ((n = std::fread(bytes.data(), 1, bytes.size(), file)) != 0)
            shaderContentHash_ = mc2shadow::fnv1a(bytes.data(), n, shaderContentHash_);
        const bool failed = std::ferror(file) != 0;
        std::fclose(file);
        if (failed) return false;
    }
    // The caster layout: set 0 = the per-pass matrix (dynamic UBO) + the instances, set 1 = the island's alpha-ON array, a per-packet push constant.
    VkDescriptorSetLayoutBinding b[2]{};
    b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = 2; lci.pBindings = b;
    if (!create(casterDsl_, device_, [&](auto* h) { return vkCreateDescriptorSetLayout(device_, &lci, nullptr, h); })) { blog("caster layout failed."); return false; }
    const VkDescriptorSetLayout setLayouts[2] = {casterDsl_.get(), alphaDsl_};
    VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ShadowDrawPush)};   // alpha row array address + first row
    if (!drawRing_.init(allocator_, device_, 1u << 16)) { blog("draw ring failed."); return false; }
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 2; plci.pSetLayouts = setLayouts;
    plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
    if (!create(casterLayout_, device_, [&](auto* h) { return vkCreatePipelineLayout(device_, &plci, nullptr, h); })) { blog("caster pipeline layout failed."); return false; }
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2 + kMaxFramesInFlight}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 + kMaxFramesInFlight}};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 2 + kMaxFramesInFlight; pci.poolSizeCount = 2; pci.pPoolSizes = sizes;
    if (!create(casterPool_, device_, [&](auto* h) { return vkCreateDescriptorPool(device_, &pci, nullptr, h); })) { blog("caster pool failed."); return false; }
    VkDescriptorSetLayout four[2 + kMaxFramesInFlight]; for (auto& layout : four) layout = casterDsl_.get();
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = casterPool_.get(); dsai.descriptorSetCount = 2 + kMaxFramesInFlight; dsai.pSetLayouts = four;
    VkDescriptorSet sets[2 + kMaxFramesInFlight]{};
    if (MC2_VK_ALLOC_SETS(device_, &dsai, sets) != VK_SUCCESS) { blog("caster sets failed."); return false; }
    windowCasterSet_ = sets[0];
    objectCasterSet_ = sets[1];
    for (uint32_t slot = 0; slot < kMaxFramesInFlight; ++slot) incWindowSet_[slot] = sets[2 + slot];
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
    if (!createDepthCasterPipeline(objectTerrain_, device_, kObjectFmt, casterLayout_.get(), terrainVert_.get(), opaqueFrag_.get(), vib, tvia)) { blog("object terrain pipeline failed."); return false; }
    // Static-prop casters: the shared static-prop vertex (position at 0, UV at 24 for the alpha cut).
    propVert_ = OwnedShaderModule(device_, vk_island_util::loadSpv(device_, dir + "shadow_prop.vert.spv", nullptr));
    if (!propVert_) { blog("shadow_prop.vert SPIR-V missing: no bake."); return false; }
    VkVertexInputBindingDescription pvib{};
    pvib.binding = 0; pvib.stride = mc2_static_prop_vertex_stride(); pvib.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription pvia[2]{};
    pvia[0].location = 0; pvia[0].binding = 0; pvia[0].format = VK_FORMAT_R32G32B32_SFLOAT; pvia[0].offset = 0;
    pvia[1].location = 2; pvia[1].binding = 0; pvia[1].format = VK_FORMAT_R32G32_SFLOAT;    pvia[1].offset = 24;
    if (!createDepthCasterPipeline(windowOpaque_, device_, kWindowFmt, casterLayout_.get(), propVert_.get(), opaqueFrag_.get(), pvib, pvia) ||
        !createDepthCasterPipeline(windowAlpha_, device_, kWindowFmt, casterLayout_.get(), propVert_.get(), alphaFrag_.get(), pvib, pvia) ||
        !createDepthCasterPipeline(objectOpaque_, device_, kObjectFmt, casterLayout_.get(), propVert_.get(), opaqueFrag_.get(), pvib, pvia) ||
        !createDepthCasterPipeline(objectAlpha_, device_, kObjectFmt, casterLayout_.get(), propVert_.get(), alphaFrag_.get(), pvib, pvia)) {
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
    const VkDescriptorPoolSize csizes[4] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2 * (1 + kMaxFramesInFlight)}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * (1 + kMaxFramesInFlight)},
                                            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * (1 + kMaxFramesInFlight)}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4 * (1 + kMaxFramesInFlight)}};
    VkDescriptorPoolCreateInfo cpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    cpci.maxSets = 1 + kMaxFramesInFlight; cpci.poolSizeCount = 4; cpci.pPoolSizes = csizes;
    if (!create(computePool_, device_, [&](auto* h) { return vkCreateDescriptorPool(device_, &cpci, nullptr, h); })) { blog("compute pool failed."); return false; }
    VkDescriptorSetLayout cdsl[1 + kMaxFramesInFlight]; for (auto& layout : cdsl) layout = pcss_.setLayout();
    VkDescriptorSetAllocateInfo cai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    cai.descriptorPool = computePool_.get(); cai.descriptorSetCount = 1 + kMaxFramesInFlight; cai.pSetLayouts = cdsl;
    VkDescriptorSet csets[1 + kMaxFramesInFlight]{};
    if (MC2_VK_ALLOC_SETS(device_, &cai, csets) != VK_SUCCESS) { blog("compute set failed."); return false; }
    computeSet_ = csets[0];
    for (uint32_t slot = 0; slot < kMaxFramesInFlight; ++slot) incComputeSet_[slot] = csets[1 + slot];
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
    vkGetPhysicalDeviceFormatProperties(phys_, kWorkFmt, &fp);
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if ((fp.optimalTilingFeatures & need) != need) { blog("R8 cannot be blitted with a linear filter here: no bake."); return false; }
    VkFormatProperties bc4{};
    vkGetPhysicalDeviceFormatProperties(phys_, kMaskFmt, &bc4);
    const VkFormatFeatureFlags needBc4 = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if ((bc4.optimalTilingFeatures & needBc4) != needBc4) { blog("BC4 cannot be sampled with a linear filter here: no bake."); return false; }
    if (!bc4_.create(device_, dir + "shadow_bake_bc4.comp.spv",
                     {{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC}, {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER}, {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}},
                     "shadow.bake.bc4")) { blog("BC4 encoder SPIR-V missing: no bake."); return false; }
    const VkDescriptorPoolSize bsizes[3] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2},
                                            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}};
    VkDescriptorPoolCreateInfo bpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    bpci.maxSets = 2; bpci.poolSizeCount = 3; bpci.pPoolSizes = bsizes;
    if (!create(bc4Pool_, device_, [&](auto* h) { return vkCreateDescriptorPool(device_, &bpci, nullptr, h); })) { blog("BC4 pool failed."); return false; }
    const VkDescriptorSetLayout bdsl[2] = {bc4_.setLayout(), bc4_.setLayout()};
    VkDescriptorSetAllocateInfo bai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    bai.descriptorPool = bc4Pool_.get(); bai.descriptorSetCount = 2; bai.pSetLayouts = bdsl;
    VkDescriptorSet bsets[2]{};
    if (MC2_VK_ALLOC_SETS(device_, &bai, bsets) != VK_SUCCESS) { blog("BC4 sets failed."); return false; }
    bc4TileSet_ = bsets[0];
    bc4TopSet_ = bsets[1];
    {
        std::array<char, 1024> userDir{};
        if (gos_GetUserDataDirectory(userDir))
            cacheDir_ = (std::filesystem::path(userDir.data()) / "cache" / "shadow").generic_string();
    }
    inited_ = true;
    return true;
}

bool ShadowBake::prewarmProducts() {
    if (!inited_) return false;
    // Mission prewarm runs after the terrain is published. Allocate the large
    // products at that load barrier rather than on the second gameplay frame.
    if (const auto* store = gos_TerrainFieldStore(); store && store->mapSide() > 1) {
        int side = 0;
        float halfMap = 0.0f;
        gos_GetTerrainChunkGridInfo(&side, &halfMap);
        const auto layout = mc2shadow::maskLayout(side, halfMap, maskTarget(), maskCap());
        if (side == store->mapSide() && layout.texels > 0 && !ensureProducts(layout.texels)) return false;
    }
    return true;
}

bool ShadowBake::prewarmTypeBounds(const ShadowBakeInputs& in) {
    if (!inited_ || !typeBounds_.empty() || in.vbo == VK_NULL_HANDLE) return true;
    ZoneScopedN("ShadowBake.PrewarmGeomHash");
    (void)geometryHash(in);
    return computeTypeBounds(in);
}

void ShadowBake::releaseTransients() {
    ZoneScopedN("ShadowBake.ReleaseTransients");
    windowView_.reset(); windowImage_.reset();
    for (int i = 0; i < 4; ++i) { scratchView_[i].reset(); scratch_[i].reset(); }
    staging_.reset(); tileParams_.reset(); passParams_.reset(); casterUbo_.reset(); instances_.reset(); heightBuf_.reset();
    tileR8View_.reset(); tileR8_.reset(); tileGeneral_ = false; blockStaging_.reset(); bc4Params_.reset(); transfer_.reset();
    queries_.reset();
    queryCount_ = 0;
    leaves_.clear(); leaves_.shrink_to_fit();
    tiles_.clear(); runs_.clear();
    // Keep the CPU height snapshot until map invalidation; object-only rebakes reuse it.
    transientBytes_ = 0;
}

void ShadowBake::invalidate() {
    // A map boundary may wait for its disk worker; gameplay frames never wait for it.
    if (cacheStore_.valid()) cacheStore_.get();
    coldPending_ = false; coldNextTile_ = 0;
    cacheLoad_ = {};
    cacheLoadKey_ = 0; cacheLoadInputKey_ = 0;
    releaseTransients();
    objectView_.reset(); objectImage_.reset();
    maskView_.reset(); maskImage_.reset();
    topR8View_.reset(); topR8_.reset();
    mc2vk::bumpDescEpoch();
    topSide_ = topMips_ = 0;
    topInitialized_ = false;
    terrainVbo_.reset(); terrainIbo_.reset(); objectInstances_.reset(); objectUbo_.reset();
    terrainIndexCount_ = 0;
    objectHeightVersion_ = ~0ull;
    typeBounds_.clear();
    geomHashValid_ = false;
    maskTexels_ = 0; maskMips_ = 0;
    maskInitialized_ = objectInitialized_ = false;
    objectDirtyRect_.reset(); objectFullDirty_ = false; terrainRows_.clear();
    maskReady_ = objectReady_ = false;
    recorded_ = submitted_ = recordedMask_ = false;
    releaseIncremental();
    incQueries_.reset();
    incActive_ = heightsMoved_ = eventsSinceDrain_ = objectDirty_ = false;
    objectMapRecords_ = dirtyBatches_ = localTileRebakes_ = statsEpoch_ = 0;
    diagRequest_ = diagPending_ = diagForceFull_ = false;
    diagBuf_.reset();
    diagInc_.clear();
    heights_.clear(); heights_.shrink_to_fit();
    bakedKey_ = objectKey_ = recordedKey_ = recordedObjectKey_ = 0;
    objectGeneration_ = recordedObjectGeneration_ = 0;
    waitFrames_ = 0;
    loggedWait_ = loggedHeightDrift_ = false;
    bakedHeightVersion_ = 0;
    bakedSetVersion_ = 0;
    layout_ = {};
}

// The bake's inputs: the caster set of the mission, the terrain, the sun, the water, the static light matrix. The mask key leaves out the height version
// once a mask exists: a crater or an editor stroke must not rebake the whole map (dirty tiles arrive with P5).
std::optional<ShadowBake::BakeKeys> ShadowBake::inputsReady(const ShadowBakeInputs& in) {
    static const bool enabled=mc2::gate::get<"MC2_SHADOW_ENABLE">();
    const auto* post=getGosPostProcess();
    if (!enabled || !post || !post->shadowsEnabled_) return std::nullopt;
    const mc2shadow::StaticCasterSet& set = mc2shadow::staticCasterSet();
    if (set.version() == 0) return std::nullopt;
    // A map whose objects cast nothing has no prop geometry; its bake is the terrain self-shadow alone.
    if (!set.entries().empty() && (in.vbo == VK_NULL_HANDLE || in.ibo == VK_NULL_HANDLE)) return std::nullopt;
    const mc2terrain::TerrainFieldStore* store = gos_TerrainFieldStore();
    if (!store || store->mapSide() < 2) return std::nullopt;
    if (!in.alphaReady && !in.alphaAbsent && !set.entries().empty()) {
        if (++waitFrames_ < kMaxAlphaWaitFrames) return std::nullopt;
        if (!loggedWait_) { loggedWait_ = true; blog("the alpha-ON array never arrived: baking with foliage casters opaque."); }
    }
    int side = 0;
    float halfMap = 0.0f;
    gos_GetTerrainChunkGridInfo(&side, &halfMap);
    if (side != store->mapSide()) return std::nullopt;
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
    const float art[5] = {mc2shadow::kSunAngularRadiusDeg, mc2shadow::kFadeStartWorld, mc2shadow::kFadeEndWorld, mc2shadow::kFadeMax, mc2shadow::kTerrainConeScale};
    h = mc2shadow::fnv1a(art, sizeof(art), h);
    const uint64_t hv = maskReady_ ? bakedHeightVersion_ : store->heightVersion();
    h = mc2shadow::fnv1a(&hv, sizeof(hv), h);
    if (maskReady_ && store->heightVersion() != bakedHeightVersion_ && !loggedHeightDrift_) {
        loggedHeightDrift_ = true;
        blog("terrain heights changed after the bake (crater or edit): the mask keeps the baked heights until dirty tiles exist (P5).");
    }
    BakeKeys keys;
    keys.mask = h | 1u;
    keys.object = keys.mask;
    // Caster deltas and height patches are consumed by the dirty path. Its
    // drained burst refreshes the object map once, without changing the mask.
    return keys;
}

bool ShadowBake::ensureProducts(int maskTexels) {
    ZoneScopedN("ShadowBake.Products");
    if (maskImage_ && maskTexels_ == maskTexels) return true;
    OwnedImage mask, top, object;
    OwnedImageView maskView, topView, objectView;
    // A new size happens at a map boundary, after the island's idle; within a map the mask is rebaked in place.
    uint32_t mips = 1;
    while ((maskTexels >> mips) >= 1 && mips < 16) ++mips;
    if (!makeImage(mask, maskView, allocator_, device_, kMaskFmt, static_cast<uint32_t>(maskTexels), mips,
                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT, "shadow.bake.mask")) {
        blog("mask image creation failed.");
        return false;
    }
    // The coarse top image: mask level kTopLevel (4 texels a sub-tile) and its own mips, the levels whose blocks span more than one sub-tile. Kept with the
    // mask so a later tile rebake (P5) can rebuild the top levels without the rest of the map.
    const auto topSide = static_cast<uint32_t>(maskTexels) >> kTopLevel;
    const auto topMips = mips - kTopLevel;
    if (!makeImage(top, topView, allocator_, device_, kWorkFmt, topSide, topMips,
                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT, "shadow.bake.maskTop")) {
        blog("mask top image creation failed.");
        return false;
    }
    if (!objectImage_) {
        if (!makeImage(object, objectView, allocator_, device_, kObjectFmt, objectMapSize(), 1,
                       VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, "shadow.bake.objectMap")) {
            blog("object map creation failed.");
            return false;
        }
    }
    // Publish only a complete set; a refused allocation leaves the old size usable.
    if (object) {
        objectView_ = std::move(objectView);
        objectImage_ = std::move(object);
        objectInitialized_ = false;
    }
    maskView_ = std::move(maskView);
    maskImage_ = std::move(mask);
    topR8View_ = std::move(topView);
    topR8_ = std::move(top);
    mc2vk::bumpDescEpoch();   // new mask/object views: descriptor-write caches keyed on the old view handles must rewrite
    maskTexels_ = maskTexels;
    maskMips_ = mips;
    topSide_ = topSide;
    topMips_ = topMips;
    maskInitialized_ = topInitialized_ = false;
    dirty_.reset(maskTexels / mc2shadow::kSubTileTexels);
    return true;
}

bool ShadowBake::ensureTransients() {
    ZoneScopedN("ShadowBake.Transients");
    if (!windowImage_) {
        OwnedImage window, scratch[4], tile;
        OwnedImageView windowView, scratchView[4], tileView;
        OwnedBuffer staging;
        if (!makeImage(window, windowView, allocator_, device_, kWindowFmt, mc2shadow::shadowQuality().windowSide, 1,
                       VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, "shadow.bake.window")) {
            blog("caster window creation failed.");
            return false;
        }
        const VkFormat fmts[4] = {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32_SFLOAT};
        const char* names[4] = {"shadow.bake.props", "shadow.bake.terrain", "shadow.bake.blur", "shadow.bake.combined"};
        for (int i = 0; i < 4; ++i)
            if (!makeImage(scratch[i], scratchView[i], allocator_, device_, fmts[i], mc2shadow::kRegionTexels, 1, VK_IMAGE_USAGE_STORAGE_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT, names[i])) { blog("scratch image creation failed."); return false; }
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = static_cast<VkDeviceSize>(mc2shadow::kTileTexels) * mc2shadow::kTileTexels + 16 +
                   8 * static_cast<VkDeviceSize>(mc2shadow::kRegionTexels) * mc2shadow::kRegionTexels;
        bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        if (!staging.create(allocator_, device_, bci, aci, "shadow.bake.staging")) { blog("staging buffer failed."); return false; }
        if (!makeImage(tile, tileView, allocator_, device_, kWorkFmt, mc2shadow::kTileTexels, kTileLevels,
                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT, "shadow.bake.tileLevels")) {
            blog("tile level image creation failed.");
            return false;
        }
        windowView_ = std::move(windowView);
        windowImage_ = std::move(window);
        for (int i = 0; i < 4; ++i) {
            scratchView_[i] = std::move(scratchView[i]);
            scratch_[i] = std::move(scratch[i]);
        }
        staging_ = std::move(staging);
        tileR8View_ = std::move(tileView);
        tileR8_ = std::move(tile);
        tileGeneral_ = false;
    }
    const VkDeviceSize region = static_cast<VkDeviceSize>(mc2shadow::kRegionTexels) * mc2shadow::kRegionTexels;
    transientBytes_ = static_cast<VkDeviceSize>(mc2shadow::shadowQuality().windowSide) * mc2shadow::shadowQuality().windowSide * 4 + region * (8 + 4 + 8 + 4) +
                      static_cast<VkDeviceSize>(mc2shadow::kTileTexels) * mc2shadow::kTileTexels * 7 / 3;
    return ensureEncoder();
}

// Each type's local box, from the CPU copy of the shared geometry the island uploaded.
bool ShadowBake::computeTypeBounds(const ShadowBakeInputs& in) {
    ZoneScopedN("ShadowBake.TypeBounds");
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

void ShadowBake::drawRuns(VkCommandBuffer cb, const ShadowBakeInputs& in, std::span<const InstRun> runs, VkPipeline opaque, VkPipeline alpha) {
    std::vector<ShadowDraw> draws;
    for (const InstRun& run : runs) {
        if (run.typeID >= in.typeFirstPacket.size() || run.typeID >= in.typePacketCount.size()) continue;
        const uint32_t first = in.typeFirstPacket[run.typeID], count = in.typePacketCount[run.typeID];
        for (uint32_t k = 0; k < count; ++k) {
            const uint32_t pk = first + k;
            if (pk >= in.packets.size()) break;
            const Mc2ShadowPacket& pkt = in.packets[pk];
            const bool alphaPacket = in.alphaReady && in.alphaSet != VK_NULL_HANDLE && pk < in.packetAlpha.size() && in.packetAlpha[pk].layer >= 0;
            ShadowDraw d{pkt.indexCount, pkt.firstIndex, pkt.baseVertex, run.firstInstance, run.count, -1, 0.0f, 0.0f};
            if (alphaPacket) { const CasterPacketAlpha& pa = in.packetAlpha[pk]; d.layer = pa.layer; d.scaleX = pa.scaleX; d.scaleY = pa.scaleY; }
            draws.push_back(d);
        }
    }
    // One indirect multi-draw for the opaque packets and one for the alpha-tested ones (the order of a depth-only pass is irrelevant).
    drawRing_.record(cb, casterLayout_.get(), opaque, alpha, in.alphaSet, draws);
}

bool ShadowBake::update(VkCommandBuffer cb, uint64_t epoch, const ShadowBakeInputs& in) {
    if (!inited_) return false;
    drawRing_.begin(static_cast<uint32_t>(epoch % kMaxFramesInFlight));
    mc2_hitch2::Scope hitchScope("shadow.bake_update");
    if (objectMapRecords_ == 0) statsEpoch_ = epoch;
    retire(epoch);
    // Cold tiles only read retained CPU uploads; queue barriers order shared scratch reuse.
    // Continue next frame without waiting two epochs; only the final submit must retire before release.
    if (recorded_ && !(coldPending_ && submitted_ && epoch > submittedEpoch_)) return false;
    if (coldPending_) { mc2_hitch2::noteBake("cold", 0, 0, "miss");
        // Bound warm-up to about 100 frames on a 400-tile map, so the live
        // caster fallback gives way to the cheaper mask promptly.
        for (uint32_t tile = 0; tile < 4 && coldPending_; ++tile)
            if (!recordColdTile(cb, in)) return false;
        recorded_ = true; submitted_ = false; recordedEpoch_ = epoch;
        return true;
    }
    if (cacheLoad_.valid() && cacheLoad_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
    const std::optional<BakeKeys> keys = inputsReady(in);
    if (!keys) return false;
    const uint64_t key = keys->mask, objectKey = keys->object;
    const bool needMask = key != bakedKey_ || diagForceFull_;
    const bool needObject = needMask || objectKey != objectKey_;
    if (!needMask && !needObject) return updateIncremental(cb, epoch, in);
    if (incActive_ && epoch < lastIncEpoch_ + 2) return false;   // a dirty batch still in flight shares the transients
    recordedKey_ = key;
    recordedObjectKey_ = objectKey;
    recordedMask_ = needMask;
    if (!recordBake(cb, in)) {
        if (cacheLoad_.valid()) return false;   // cache I/O is pending, not a failed bake
        // A failed input is not retried every frame: the keys are taken; a producer edit (new caster set, sun, map) asks again.
        bakedKey_ = key;
        objectKey_ = objectKey;
        return false;
    }
    recorded_ = true;
    submitted_ = false;
    recordedEpoch_ = epoch;
    if (recordedMask_) liveStale_ = true;
    return true;
}

bool ShadowBake::wantsFrame() const {
    if (!inited_) return false;
    if (recorded_ || objectDirty_ || incActive_ || mc2shadow::staticCasterSet().hasEvents()) return true;
    return mc2shadow::staticCasterSet().version() != 0 && !maskReady_;
}

void ShadowBake::markSubmitted(uint64_t epoch) {
    if (!recorded_ || submitted_) return;
    submitted_ = true;
    submittedEpoch_ = epoch;
    if (coldPending_) return;
    if (recordedMask_) { bakedKey_ = recordedKey_; bakedSetVersion_ = mc2shadow::staticCasterSet().version(); maskReady_ = true; }
    objectKey_ = recordedObjectKey_;
    objectGeneration_ = recordedObjectGeneration_;
    objectMatrix_ = recordedObjectMatrix_;
    objectReady_ = true;
}

// The island opened this epoch's recording after waiting its parity's fence; on the bake's parity two or more epochs later that fence covered the bake's
// submit, so its timestamps are final and its transients idle.
void ShadowBake::retire(uint64_t epoch) {
    if (!recorded_) return;
    // Recorded into an earlier frame that never submitted: the products stay as they were. Within the recording frame it is still pending.
    if (!submitted_) {
        if (epoch != recordedEpoch_) {
            if (!recordedMask_) { objectDirty_ = true; objectFullDirty_ = true; }
            recorded_ = false;
        }
        return;
    }
    if (epoch < submittedEpoch_ + kMaxFramesInFlight || epoch % kMaxFramesInFlight != submittedEpoch_ % kMaxFramesInFlight) return;
    recorded_ = false;
    submitted_ = false;
    objectDirtyRect_.reset();
    objectFullDirty_ = false;
    if (coldPending_) return;
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
            const double object = span(tail + 2, tail + 5);
            if (mc2_diag::tagRequested("SHADOW_BAKE_SPLIT"))
            mc2::fmt::print_err("[SHADOW_BAKE_SPLIT] object gpu_ms clear={:.3f} terrain={:.3f} casters={:.3f} partial={}\n",
                                span(tail + 2, tail + 3), span(tail + 3, tail + 4), span(tail + 4, tail + 5), objectPartial_);
            if (recordedMask_ && warm_) {
                mc2::fmt::print_err("[SHADOW_BAKE] warm load: gpu_ms upload={:.2f} object_map={:.2f} cpu_ms={:.2f} (disk read {:.2f})\n",
                                    span(0, 1), object, cpuMs_, warmReadMs_);
            } else if (recordedMask_) {
                double stage[4] = {0, 0, 0, 0};
                for (size_t t = 0; t < tiles; ++t)
                    for (uint32_t s = 0; s < 4; ++s) stage[s] += span(static_cast<uint32_t>(t * kQueriesPerTile + s), static_cast<uint32_t>(t * kQueriesPerTile + s + 1));
                mc2::fmt::print_err("[SHADOW_BAKE] gpu_ms window={:.2f} pcss={:.2f} march={:.2f} resolve={:.2f} mips={:.2f} object_map={:.2f} total={:.2f} cpu_ms={:.2f} tiles={}\n",
                                    stage[0], stage[1], stage[2], stage[3], span(tail, tail + 1), object, stage[0] + stage[1] + stage[2] + stage[3] + span(tail, tail + 1) + object, cpuMs_, mc2::fmt::as_size(tiles));
            } else {
                mc2::fmt::print_err("[SHADOW_BAKE] gpu_ms object_map={:.2f} cpu_ms={:.2f}\n", object, cpuMs_);
            }
        }
    }
    if (recordedMask_ && !warm_) storeReadback();
    const VkDeviceSize maskBytes = mc2shadow::bc4ChainBytes(static_cast<uint32_t>(maskTexels_), maskMips_);
    const VkDeviceSize objectBytes = static_cast<VkDeviceSize>(objectMapSize()) * objectMapSize() * 2;
    mc2::fmt::print_err("[SHADOW_BAKE] vram resident_mb={:.1f} (mask {:.1f} + object_map {:.1f}) transient_peak_mb={:.1f} {}\n",
                        static_cast<double>(maskBytes + objectBytes) / 1048576.0, static_cast<double>(maskBytes) / 1048576.0,
                        static_cast<double>(objectBytes) / 1048576.0, static_cast<double>(transientBytes_) / 1048576.0,
                        recordedMask_?"released":"retained for dirty updates");
    mc2::fmt::print_err("[SHADOW_BAKE] ledger preset={} mask_mb={:.2f} object_mb={:.2f} mover_mb={:.2f} resident_mb={:.2f} transient_peak_mb={:.2f}\n",
        mc2shadow::shadowQuality().level,static_cast<double>(maskBytes)/1048576.0,static_cast<double>(objectBytes)/1048576.0,
        static_cast<double>(objectBytes)/1048576.0,static_cast<double>(maskBytes+2*objectBytes)/1048576.0,static_cast<double>(transientBytes_)/1048576.0);
    const bool completedMask=recordedMask_;
    recordedMask_ = false;
    // Object-only rebuilds share the dirty path's retained window/encoder images.
    if (completedMask) releaseTransients();
}

bool ShadowBake::readHeightGrid() {
    const auto* store = gos_TerrainFieldStore();
    if (!store) return false;
    const int coarseSide = store->mapSide();
    const bool visual = terrainVisualDisplaceGateOn() && !store->visualHeight().empty() && store->visualSide() == (coarseSide - 1) * 4 + 1;
    const int sourceSide = visual ? store->visualSide() : coarseSide;
    heights_ = mc2shadow::maskHeightGrid(visual ? store->visualHeight() : store->height(), sourceSide, sourceSide - 1,
                                      visual, store->height(), terrainVisualDisplaceFarParam());
    if (heights_.empty()) return false;
    heightSide_ = sourceSide;
    heightSpacing_ = static_cast<float>(coarseSide - 1) * mc2shadow::kWorldUnitsPerVertex / static_cast<float>(heightSide_ - 1);
    // Second plane: the draw mesh chooses diagonals from RAW bake corners,
    // then the vertex shader applies the crest filter to their heights.
    const size_t count = heights_.size();
    heights_.resize(count * 2, 0.0f);
    if (visual) {
        const auto& raw = store->visualHeight();
        for (int y = 0; y < sourceSide - 1; ++y)
            for (int x = 0; x < sourceSide - 1; ++x) {
                const size_t i = static_cast<size_t>(x) + static_cast<size_t>(y) * sourceSide;
                heights_[count + i] = mc2terrain::quadSplitsTrBl(raw[i], raw[i + 1], raw[i + sourceSide], raw[i + sourceSide + 1]) ? 1.0f : 0.0f;
            }
    }
    heightsMoved_ = false;
    readHeightVersion_ = store->heightVersion();
    ++heightsGen_;
    return true;
}

bool ShadowBake::recordColdTile(VkCommandBuffer cb, const ShadowBakeInputs& in) {
    const uint32_t t = coldNextTile_++;
    const int tx = static_cast<int>(t) % layout_.tilesPerSide, ty = static_cast<int>(t) / layout_.tilesPerSide;
    const auto& tile = tiles_[t];
    const RegionJob job{mc2shadow::kTileTexels, mc2shadow::kRegionTexels, tx * mc2shadow::kTileTexels, ty * mc2shadow::kTileTexels};
    recordRegion(cb, in, job, tile, std::span<const InstRun>(runs_.data() + tile.firstRun, tile.runCount), computeSet_, windowCasterSet_,
                 t * static_cast<uint32_t>(kSlice), t * static_cast<uint32_t>(kSlice), 0, 0, queryCount_ ? static_cast<int>(t * kQueriesPerTile) : -1);
    coldPending_ = coldNextTile_ < tiles_.size();
    if (!coldPending_) {
        const uint32_t tail = static_cast<uint32_t>(tiles_.size()) * kQueriesPerTile;
        if (queryCount_) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries_.get(), tail);
        recordTopLevels(cb);
        recordMaskToRead(cb, true);
        if (queryCount_) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries_.get(), tail + 1);
        maskInitialized_ = true;
    }
    return true;
}

bool ShadowBake::recordBake(VkCommandBuffer cb, const ShadowBakeInputs& in) {
    using clk = std::chrono::steady_clock;
    const clk::time_point t0 = clk::now();
    ZoneScopedN("ShadowBake.Record");
    const mc2terrain::TerrainFieldStore* store = gos_TerrainFieldStore();
    if (!store) return false;
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
    if (recordedMask_) {
        layout_ = mc2shadow::maskLayout(mapSide_, halfMap_, maskTarget(), maskCap());
        if (layout_.texels == 0) { blog("map too small for a mask: no bake."); return false; }
    }
    // Object-only caster updates reuse the same terrain snapshot as dirty sub-tiles.
    const auto tHeight = clk::now();
    if ((recordedMask_ || heightsMoved_ || heights_.empty() || readHeightVersion_ != store->heightVersion()) && !readHeightGrid()) { blog("height field malformed: no bake."); return false; }
    terrainTop_ = -1e30f;
    terrainMin_ = 1e30f;
    for (size_t i = 0; i < static_cast<size_t>(heightSide_) * heightSide_; ++i) {
        terrainTop_ = (std::max)(terrainTop_, heights_[i]); terrainMin_ = (std::min)(terrainMin_, heights_[i]);
    }
    terrainMin_ = (std::max)(terrainMin_, waterZ_);
    const double heightMs = std::chrono::duration<double, std::milli>(clk::now() - tHeight).count();
    if (recordedMask_) bakedHeightVersion_ = store->heightVersion();

    if (!ensureProducts(layout_.texels > 0 ? layout_.texels : maskTexels_)) return false;
    if (typeBounds_.empty() && in.vbo != VK_NULL_HANDLE && !computeTypeBounds(in)) { blog("static-prop geometry not readable: no bake."); return false; }

    // Casters: the gameplay set resolved to registry leaves, each with its world box and light-space box.
    const clk::time_point tResolve = clk::now();
    // Snapshot only on the first mask of this set; subsequent rebakes key and render the live state.
    mc2shadow::StaticCasterSet& casters=mc2shadow::staticCasterSet();
    if (recordedMask_) casters.logDriftSinceLoad(6);   // what changed since the load snapshot; settled into it below
    if (recordedMask_ && bakedSetVersion_ != casters.version()) casters.settleLoadSnapshot();   // load-time damage/shape swaps land during the first frames
    // An object-map-only rebuild (a caster event after the mask exists) needs neither the mask's caster list, its content hash nor its light boxes: only
    // the object receivers below. Resolving and hashing the whole set twice cost 4-27 ms in one frame (hitch-hunt-1118 row 8).
    uint64_t casterHash = 0;
    if (recordedMask_) {
        casterHash = casters.resolveForBake(bakedSetVersion_, leaves_);
        // Keep the gameplay shadow recipe (tree LOD0), including its cutout canopy.
    }
    cpuStage_[0] = std::chrono::duration<double, std::milli>(clk::now() - tResolve).count();
    std::vector<mc2shadow::LightBox> boxes;
    std::vector<float> tops;
    std::vector<uint32_t> live;
    const uint32_t maskLeaves = recordedMask_ ? static_cast<uint32_t>(leaves_.size()) : 0u;
    boxes.reserve(maskLeaves);
    float casterTop = -1e30f;
    for (uint32_t i = 0; i < maskLeaves; ++i) {
        const GpuStaticPropInstance& leaf = leaves_[i];
        if (leaf.typeID >= typeBounds_.size() || !typeBounds_[leaf.typeID].valid) continue;
        mc2shadow::Vec3 lo, hi;
        instanceBox(leaf.modelMatrix, typeBounds_[leaf.typeID].mn, typeBounds_[leaf.typeID].mx, lo, hi);
        boxes.push_back(mc2shadow::lightBox(basis_, lo, hi));
        tops.push_back(hi.z);
        live.push_back(i);
        casterTop = (std::max)(casterTop, hi.z);
    }
    // Object-only rebuilds skip the box loop; keep casterTop_ = highest caster top (terrain only when none), as base.
    if (recordedMask_ && casterTop < -1e29f) casterTop = terrainTop_;
    casterTop_ = casterTop;

    // Object receivers always use current gameplay leaves.
    std::vector<GpuStaticPropInstance> objInst;
    casters.resolve(objInst, false);
    std::erase_if(objInst, [this](const auto& leaf) {
        return leaf.typeID >= typeBounds_.size() || !typeBounds_[leaf.typeID].valid;
    });
    std::sort(objInst.begin(), objInst.end(), [](const auto& a, const auto& b) { return a.typeID < b.typeID; });
    std::vector<uint32_t> objTypes;
    objTypes.reserve(objInst.size());
    for (const auto& leaf : objInst) {
        objTypes.push_back(leaf.typeID);
        mc2shadow::Vec3 lo, hi;
        instanceBox(leaf.modelMatrix, typeBounds_[leaf.typeID].mn, typeBounds_[leaf.typeID].mx, lo, hi);
        casterTop_ = (std::max)(casterTop_, hi.z);
    }
    if (casterTop_ < -1e29f) casterTop_ = terrainTop_;
    objectRuns_.clear();
    appendRuns(objectRuns_, objTypes, 0);
    const double resolveMs = std::chrono::duration<double, std::milli>(clk::now() - tResolve).count();

    // A mask whose inputs a cooked asset already covers is uploaded, not baked.
    std::optional<mc2shadow::ShadowMaskAsset> cached;
    bool warm = false;
    double warmReadMs = 0.0;
    if (recordedMask_) {
        const clk::time_point k0 = clk::now();
        // The retry after the async cache read was launched sees the same inputs: reuse the key (about 77 ms of hashing) instead of redoing it.
        if (!(cacheLoad_.valid() && cacheLoadInputKey_ == recordedKey_)) contentKey_ = contentKey(in, casterHash);
        cpuStage_[1] = std::chrono::duration<double, std::milli>(clk::now() - k0).count();
        if (cacheLoad_.valid() && cacheLoadKey_ != contentKey_) {
            // The old request has finished (update polls it); a sun/preset edit needs a new key.
            (void)cacheLoad_.get();
        }
        if (!diagForceFull_ && !cacheLoad_.valid()) {
            cacheLoadKey_ = contentKey_;
            cacheLoadInputKey_ = recordedKey_;
            cacheLoad_ = std::async(std::launch::async, [dir = cacheDir_, key = contentKey_] {
                const auto start = clk::now();
                CacheLoad result;
                result.asset = mc2shadow::loadCachedShadowMask(dir, key);
                result.readMs = std::chrono::duration<double, std::milli>(clk::now() - start).count();
                return result;
            });
            return false;
        }
        if (cacheLoad_.valid()) {
            CacheLoad result = cacheLoad_.get();
            cached = std::move(result.asset);
            warmReadMs = result.readMs;
        }
        warm = !diagForceFull_ && cached && cached->texels == static_cast<uint32_t>(layout_.texels) && cached->mips == maskMips_;
        mc2::fmt::print_err("[SHADOW_BAKE] key {:016x} parts heights={:016x} casters={:016x} geometry={:016x} alpha={:016x} scalars={:016x} versions={:016x} "
                            "leaves={} alpha_ready={} cpu_ms resolve={:.1f} key={:.1f}\n",
                            mc2::fmt::as_u64(contentKey_), mc2::fmt::as_u64(keyParts_[0]), mc2::fmt::as_u64(keyParts_[1]), mc2::fmt::as_u64(keyParts_[2]),
                            mc2::fmt::as_u64(keyParts_[3]), mc2::fmt::as_u64(keyParts_[4]), mc2::fmt::as_u64(keyParts_[5]), mc2::fmt::as_size(leaves_.size()),
                            in.alphaReady ? 1 : 0, cpuStage_[0], cpuStage_[1]);
        diagForceFull_ = false;
    }
    warm_ = warm;

    // Tiles: the window each needs and the casters that can reach it.
    const clk::time_point p0 = clk::now();
    tiles_.clear();
    runs_.clear();
    std::vector<GpuStaticPropInstance> tileInst;
    std::vector<ShadowBakeTileParams> params;
    const float region = static_cast<float>(mc2shadow::kRegionTexels) * layout_.texelWorld;
    if (recordedMask_ && !warm) {
        const int tps = layout_.tilesPerSide;
        for (int ty = 0; ty < tps; ++ty) {
            for (int tx = 0; tx < tps; ++tx) {
                const mc2shadow::Rect r = mc2shadow::tileRegionWorld(layout_, tx, ty);
                TileWork tw;
                ShadowBakeTileParams p{};
                planRegion(r, tx * mc2shadow::kTileTexels, ty * mc2shadow::kTileTexels, mc2shadow::kRegionTexels, casterTop, tanT,
                           CasterView{&leaves_, &boxes, &tops, &live}, runs_, tileInst, tw, p);
                tiles_.push_back(tw);
                params.push_back(p);
            }
        }
        (void)region;
    }
    binnedInstances_ = static_cast<uint32_t>(tileInst.size());
    cpuStage_[2] = std::chrono::duration<double, std::milli>(clk::now() - p0).count();
    const auto tRecord = clk::now();

    // The bake owns the full-map object projection. It is paired with the
    // submitted D16 image, independently of camera history or an engine latch.
    const float span=(mapSide_-1)*mc2shadow::kWorldUnitsPerVertex;
    const mc2shadow::Rect full{-halfMap_,halfMap_-span,-halfMap_+span,halfMap_};
    const auto objectWindow=mc2shadow::casterWindow(basis_,full,terrainMin_,terrainTop_,casterTop_,tanT,objectMapSize());
    mc2shadow::windowMatrix(basis_,objectWindow,recordedObjectMatrix_.data());
    objectPartial_ = objectInitialized_ && !recordedMask_ && !heightsMoved_ && !liveStale_ && !objectFullDirty_ && objectDirtyRect_ && recordedObjectMatrix_ == objectMatrix_;
    objectScissor_ = {{0, 0}, {static_cast<uint32_t>(objectMapSize()), static_cast<uint32_t>(objectMapSize())}};
    if (objectPartial_) {
        auto r = *objectDirtyRect_;
        const float t = objectWindow.texelWorld;
        r = {r.x0 - t, r.y0 - t, r.x1 + t, r.y1 + t};
        const int x0 = std::clamp(static_cast<int>(std::floor((r.x0 - objectWindow.lx0) / t)), 0, objectMapSize() - 1);
        const int y0 = std::clamp(static_cast<int>(std::floor((r.y0 - objectWindow.ly0) / t)), 0, objectMapSize() - 1);
        const int x1 = std::clamp(static_cast<int>(std::ceil((r.x1 - objectWindow.lx0) / t)), x0 + 1, objectMapSize());
        const int y1 = std::clamp(static_cast<int>(std::ceil((r.y1 - objectWindow.ly0) / t)), y0 + 1, objectMapSize());
        objectScissor_ = {{x0, y0}, {static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0)}};
        // Cull against the rounded raster rectangle, retaining the existing order within each type.
        const mc2shadow::Rect clip{objectWindow.lx0 + x0 * t, objectWindow.ly0 + y0 * t, objectWindow.lx0 + x1 * t, objectWindow.ly0 + y1 * t};
        objectDirtyRect_ = clip;
        std::erase_if(objInst, [&](const auto& leaf) {
            mc2shadow::Vec3 lo, hi;
            instanceBox(leaf.modelMatrix, typeBounds_[leaf.typeID].mn, typeBounds_[leaf.typeID].mx, lo, hi);
            const auto box = mc2shadow::lightBox(basis_, lo, hi);
            return !clip.overlaps({box.lx0, box.ly0, box.lx1, box.ly1});
        });
        objTypes.clear();
        for (const auto& leaf : objInst) objTypes.push_back(leaf.typeID);
        objectRuns_.clear(); appendRuns(objectRuns_, objTypes, 0);
    }
    // Uploads.
    if (recordedMask_ && !(warm ? ensureEncoder() : ensureTransients())) return false;
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
        std::memcpy(o.lightSpace, recordedObjectMatrix_.data(), sizeof(o.lightSpace));
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
        for (auto & wElem : w) { wElem.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wElem.descriptorCount = 1; }
        w[0].dstSet = windowCasterSet_; w[0].dstBinding = 0; w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; w[0].pBufferInfo = &ubo;
        w[1].dstSet = windowCasterSet_; w[1].dstBinding = 1; w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[1].pBufferInfo = &win;
        w[2].dstSet = objectCasterSet_; w[2].dstBinding = 0; w[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; w[2].pBufferInfo = &ubo;
        w[3].dstSet = objectCasterSet_; w[3].dstBinding = 1; w[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[3].pBufferInfo = &obj;
        vkUpdateDescriptorSets(device_, 4, w, 0, nullptr);
    }
    if (recordedMask_ && !warm) {
        if (!makeHostBuffer(tileParams_, allocator_, device_, (std::max<uint32_t>)(tileCount, 1) * kSlice, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, "shadow.bake.tileParams") ||
            !makeHostBuffer(passParams_, allocator_, device_, 3 * kSlice, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, "shadow.bake.passParams") ||
            !makeHostBuffer(heightBuf_, allocator_, device_, heights_.size() * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "shadow.bake.heights")) {
            blog("bake buffers failed.");
            return false;
        }
        std::vector<uint8_t> tp(std::max<uint32_t>(tileCount, 1) * kSlice, 0);
        for (uint32_t t = 0; t < tileCount; ++t) std::memcpy(tp.data() + t * kSlice, &params[t], sizeof(ShadowBakeTileParams));
        std::vector<uint8_t> pp3(3 * kSlice, 0);
        for (int32_t m = 0; m < 3; ++m) {
        const int32_t pass[4]={m,mc2shadow::shadowQuality().level,0,0};
        std::memcpy(pp3.data()+m*kSlice,pass,sizeof(pass));
    }
        if (!tileParams_.upload(tp.data(), tp.size()) || !passParams_.upload(pp3.data(), pp3.size()) ||
            !heightBuf_.upload(heights_.data(), heights_.size() * sizeof(float))) { blog("bake upload failed."); return false; }
        const VkDescriptorBufferInfo tile{tileParams_.get(), 0, sizeof(ShadowBakeTileParams)};
        const VkDescriptorBufferInfo pass{passParams_.get(), 0, 16};
        const VkDescriptorImageInfo raw{depthRawSampler_.get(), windowView_.get(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        const VkDescriptorImageInfo cmp{depthCompareSampler_.get(), windowView_.get(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        const VkDescriptorBufferInfo hb{heightBuf_.get(), 0, VK_WHOLE_SIZE};
        const VkDescriptorBufferInfo sb{staging_.get(), 0, VK_WHOLE_SIZE};
        VkDescriptorImageInfo si[4];
        for (int i = 0; i < 4; ++i) si[i] = {VK_NULL_HANDLE, scratchView_[i].get(), VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w[10]{};
        for (auto & wElem : w) { wElem.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wElem.dstSet = computeSet_; wElem.descriptorCount = 1; }
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
        ZoneScopedN("ShadowBake.Queries");
        queryCount_ = tileCount * kQueriesPerTile + 6;
        VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qci.queryCount = queryCount_;
        if (!create(queries_, device_, [&](auto* h) { return vkCreateQueryPool(device_, &qci, nullptr, h); })) queryCount_ = 0;
        else vkCmdResetQueryPool(cb, queries_.get(), 0, queryCount_);
    }
    auto stamp = [&](uint32_t q) { if (queryCount_) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries_.get(), q); };

    if (recordedMask_ && warm) {
        stamp(0);
        if (!recordWarmUpload(cb, *cached)) return false;
        stamp(1);
    } else if (recordedMask_) {
        MC2_VK_GPU_SCOPE(gpuScope, cb, "VKIsl.Shadow.Bake");
        // The mask's every level becomes a transfer target (its last bake, if any, was read by fragment shaders); the scratch images become GENERAL.
        VkImageMemoryBarrier pre[6];
        pre[0] = imageBarrier(maskImage_.get(), VK_IMAGE_ASPECT_COLOR_BIT, 0, maskMips_,
                              maskInitialized_ ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              0, VK_ACCESS_TRANSFER_WRITE_BIT);
        for (int i = 0; i < 4; ++i)
            pre[1 + i] = imageBarrier(scratch_[i].get(), VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
                                      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        pre[5] = imageBarrier(topR8_.get(), VK_IMAGE_ASPECT_COLOR_BIT, 0, topMips_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                              VK_ACCESS_TRANSFER_WRITE_BIT);
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 6, pre);
        maskReady_ = objectReady_ = false;
        coldNextTile_ = 0;
        // The object map and its allocations are prepared once, before spreading mask work over frames.
        if (!recordObjectMap(cb, in, tileCount * kQueriesPerTile + 2)) return false;
        coldPending_ = true;
        recordedObjectGeneration_ = objectMapRecords_;
        if (!recordColdTile(cb, in)) return false;
        gpuScope.end();
    }
    if ((!recordedMask_ || warm) && !recordObjectMap(cb, in, tileCount * kQueriesPerTile + 2)) return false;
    recordedObjectGeneration_ = objectMapRecords_;
    cpuMs_ = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
    warmReadMs_ = warmReadMs;
    // Only a pass that records the mask consulted the cache; an object-map-only update has no cache read and must not overwrite the load's answer with "miss".
    if (recordedMask_) mc2_hitch2::stats().bakeCache.store(warm ? "hit" : "miss");
    if (recordedMask_ && warm) {
        mc2::fmt::print_err("[SHADOW_BAKE] warm: mask {}^2 BC4 from the cook cache (key {:016x}, read {:.2f} ms), object map re-recorded, cpu_ms={:.2f}\n",
                            layout_.texels, mc2::fmt::as_u64(contentKey_), warmReadMs, cpuMs_);
    } else if (recordedMask_) {
        mc2::fmt::print_err("[SHADOW_BAKE] recorded mask={}^2 texel_wu={:.3f} tiles={} casters={} binned_instances={} sun=({:.3f},{:.3f},{:.3f}) theta_deg={:.2f} "
                            "fade=({:.0f},{:.0f},{:.3f}) water={:.1f} heights={}^2@{:.0f}wu cpu_ms={:.2f}\n",
                            layout_.texels, layout_.texelWorld, tileCount, mc2::fmt::as_size(leaves_.size()), binnedInstances_, sun_.x, sun_.y, sun_.z,
                            mc2shadow::kSunAngularRadiusDeg, mc2shadow::kFadeStartWorld, mc2shadow::kFadeEndWorld, fadeMax_, waterZ_, heightSide_,
                            heightSpacing_, cpuMs_);
    } else {
        mc2::fmt::print_err("[SHADOW_BAKE] object map re-recorded: generation={} casters={} timings(ms): total {:.3f}, height {:.3f}, resolve {:.3f}, bin {:.3f}, record {:.3f}\n",
                            mc2::fmt::as_u64(recordedObjectGeneration_), mc2::fmt::as_size(objInst.size()), cpuMs_, heightMs, resolveMs, cpuStage_[2],
                            std::chrono::duration<double, std::milli>(clk::now() - tRecord).count());
    }
    return true;
}

// One region's plan: its receiver height range, its caster window, the casters that can reach it (appended to `runs` / `inst`) and its parameters.
void ShadowBake::planRegion(mc2shadow::Rect r, int originX, int originY, int regionTexels, float casterTop, float tanT, const CasterView& cv,
                            std::vector<InstRun>& runs, std::vector<GpuStaticPropInstance>& inst, TileWork& tw, ShadowBakeTileParams& p) const {
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
    tw.window = mc2shadow::casterWindow(basis_, r, hMin, hMax, casterTop, tanT, mc2shadow::shadowQuality().windowSide);
    tw.casterTop = hMax;
    const auto needed = mc2shadow::neededWindowRect(tw.window, ((std::max)(casterTop, hMax) - hMin) / (std::max)(basis_.toward.z, 0.05f), tanT, layout_.texelWorld);
    tw.window.needed = needed;
    // Scratch reused across regions (one planner thread): no per-region allocation once warm.
    thread_local std::vector<uint32_t> picked, types;
    picked.clear(); types.clear();
    for (size_t c = 0; c < cv.boxes->size(); ++c)
        if (mc2shadow::casterInWindow((*cv.boxes)[c], tw.window)) {
            tw.casterTop = (std::max)(tw.casterTop, (*cv.tops)[c]);
            const auto& box = (*cv.boxes)[c];
            if (needed.overlaps({box.lx0, box.ly0, box.lx1, box.ly1})) picked.push_back((*cv.live)[c]);
        }
    const auto& leaves = *cv.leaves;
    std::sort(picked.begin(), picked.end(), [&leaves](uint32_t a, uint32_t b) { return leaves[a].typeID < leaves[b].typeID; });
    for (uint32_t i : picked) types.push_back(leaves[i].typeID);
    tw.firstRun = static_cast<uint32_t>(runs.size());
    appendRuns(runs, types, static_cast<uint32_t>(inst.size()));
    tw.runCount = static_cast<uint32_t>(runs.size()) - tw.firstRun;
    for (uint32_t i : picked) inst.push_back(leaves[i]);

    p = {};
    mc2shadow::windowMatrix(basis_, tw.window, p.window);
    p.region[0] = r.x0; p.region[1] = r.y1; p.region[2] = layout_.texelWorld; p.region[3] = waterZ_;
    p.regionInfo[0] = regionTexels;
    p.regionInfo[1] = originX - mc2shadow::kTileApron;
    p.regionInfo[2] = originY - mc2shadow::kTileApron;
    p.regionInfo[3] = mc2shadow::kTileApron;
    p.sun[0] = sun_.x; p.sun[1] = sun_.y; p.sun[2] = sun_.z; p.sun[3] = tanT;
    p.windowInfo[0] = tw.window.dMin; p.windowInfo[1] = tw.window.dMax;
    p.windowInfo[2] = tw.window.texelWorld; p.windowInfo[3] = static_cast<float>(tw.window.texels);
    p.heightInfo[0] = hx0; p.heightInfo[1] = hyN; p.heightInfo[2] = heightSpacing_; p.heightInfo[3] = static_cast<float>(heightSide_);
    p.fade[0] = mc2shadow::kFadeStartWorld; p.fade[1] = mc2shadow::kFadeEndWorld; p.fade[2] = fadeMax_; p.fade[3] = tw.casterTop;
    p.march[0] = terrainTop_; p.march[1] = 4.0f; p.march[2] = 1.07f; p.march[3] = layout_.span * 1.5f;
}

// One region (a 512 tile of a full bake, or a 128 dirty sub-tile): the caster window, PCSS, terrain march, the resolve, and the BC4 encode into the mask.
// queryBase >= 0 stamps the window / pcss / march / resolve boundaries (full bakes only).
void ShadowBake::recordRegion(VkCommandBuffer cb, const ShadowBakeInputs& in, const RegionJob& job, const TileWork& tw, std::span<const InstRun> runs,
                              VkDescriptorSet computeSet, VkDescriptorSet windowSet, uint32_t paramOff, uint32_t casterOff, uint32_t passBase,
                              uint32_t sliceBase, int queryBase) {
    auto stamp = [&](int k) { if (queryBase >= 0) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        computeSet == computeSet_ ? queries_.get() : incQueries_.get(), static_cast<uint32_t>(queryBase + k)); };
    const uint32_t groups = (static_cast<uint32_t>(job.regionTexels) + 7) / 8;
    const VkDeviceSize zero = 0;
    stamp(0);
    // The previous region's reads of the scratch images and its staging copy finish before this region writes them.
    cmdBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    const uint32_t header[4] = {0, 1, 1, 0};
    vkCmdUpdateBuffer(cb, staging_.get(), static_cast<VkDeviceSize>(mc2shadow::kTileTexels) * mc2shadow::kTileTexels, sizeof(header), header);   // empty penumbra list: indirect args {0,1,1}, count 0
    cmdBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    // Dynamic rendering pass: the window map arrives UNDEFINED (it is cleared) and leaves DEPTH_STENCIL_READ_ONLY; the barriers are the dependencies and
    // layout moves of the render pass this replaced (writes after the earlier compute/fragment reads; the bake's readers see the depth writes).
    mc2vk::VulkanContext& dctx = mc2vk::getOrCreateEngineVulkanContext();
    const uint32_t windowSide = static_cast<uint32_t>(mc2shadow::shadowQuality().windowSide);
    mc2vk::dynrender::depthImageBarrier(dctx, cb, windowImage_.get(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                                        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    const auto& w = tw.window;
    const int x0 = std::clamp(static_cast<int>(std::floor((w.needed.x0 - w.lx0) / w.texelWorld)), 0, static_cast<int>(windowSide) - 1);
    const int y0 = std::clamp(static_cast<int>(std::floor((w.needed.y0 - w.ly0) / w.texelWorld)), 0, static_cast<int>(windowSide) - 1);
    const int x1 = std::clamp(static_cast<int>(std::ceil((w.needed.x1 - w.lx0) / w.texelWorld)), x0 + 1, static_cast<int>(windowSide));
    const int y1 = std::clamp(static_cast<int>(std::ceil((w.needed.y1 - w.ly0) / w.texelWorld)), y0 + 1, static_cast<int>(windowSide));
    const VkRect2D sc{{x0, y0}, {static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0)}};
    mc2vk::dynrender::Pass pass;
    pass.area = sc;
    VkClearValue clear{}; clear.depthStencil = {1.0f, 0};
    pass.setDepth(mc2vk::dynrender::attachment(windowView_.get(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                  VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, &clear), kWindowFmt);
    mc2vk::dynrender::begin(dctx, cb, pass);
    stamp(5);
    if (!runs.empty()) {
        const float side = static_cast<float>(mc2shadow::shadowQuality().windowSide);
        const VkViewport vp{0.0f, 0.0f, side, side, 0.0f, 1.0f};
        vkCmdSetViewport(cb, 0, 1, &vp);
        vkCmdSetScissor(cb, 0, 1, &sc);
        vkCmdSetDepthBias(cb, 0.0f, 0.0f, 0.0f);
        mc2vk::infra::bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, windowOpaque_.get());
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, casterLayout_.get(), 0, 1, &windowSet, 1, &casterOff);
        vkCmdBindVertexBuffers(cb, 0, 1, &in.vbo, &zero);
        vkCmdBindIndexBuffer(cb, in.ibo, 0, VK_INDEX_TYPE_UINT32);
        drawRuns(cb, in, runs, windowOpaque_.get(), windowAlpha_.get());
    }
    mc2vk::dynrender::end(dctx, cb);
    mc2vk::dynrender::depthImageBarrier(dctx, cb, windowImage_.get(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    stamp(1);
    const uint32_t offs0[2] = {paramOff, passBase};
    pcss_.bind(cb, computeSet, offs0);
    vkCmdDispatch(cb, groups, groups, 1);
    // The penumbra filter runs over the listed texels only.
    cmdBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
    const uint32_t offs1[2] = {paramOff, passBase + static_cast<uint32_t>(kSlice)};
    pcss_.bind(cb, computeSet, offs1);
    vkCmdDispatchIndirect(cb, staging_.get(), static_cast<VkDeviceSize>(mc2shadow::kTileTexels) * mc2shadow::kTileTexels);
    stamp(2);
    march_.bind(cb, computeSet, offs0);
    vkCmdDispatch(cb, groups, groups, 1);
    stamp(3);
    for (uint32_t mode = 0; mode < 2; ++mode) {
        cmdBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        const uint32_t offs[2] = {paramOff, passBase + mode * static_cast<uint32_t>(kSlice)};
        resolve_.bind(cb, computeSet, offs);
        if (mode == 0) vkCmdDispatch(cb, groups, groups, 1);
        else vkCmdDispatch(cb, job.tile / 8, job.tile / 8, 1);   // fused combine + tent: one group per 8x8 of the tile
    }
    cmdBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    recordTileEncode(cb, job.originX, job.originY, job.tile, sliceBase);
    stamp(4);
}

// A changed rect grown by the texels whose resolve reads into it.
mc2shadow::Rect ShadowBake::grownForDirty(mc2shadow::Rect r) const {
    const float g = static_cast<float>(kDirtyGrowTexels) * layout_.texelWorld;
    return {r.x0 - g, r.y0 - g, r.x1 + g, r.y1 + g};
}

// The world rectangle a caster's leaves can shadow (their boxes swept down-sun to the lowest receiver), or an empty rect for no leaves.
std::optional<mc2shadow::Rect> ShadowBake::leavesFootprint(const std::vector<GpuStaticPropInstance>& leaves) const {
    std::optional<mc2shadow::Rect> out;
    const float tanT = std::tan(mc2shadow::kSunAngularRadiusDeg * 0.01745329252f);
    for (const GpuStaticPropInstance& leaf : leaves) {
        if (leaf.typeID >= typeBounds_.size() || !typeBounds_[leaf.typeID].valid) continue;
        mc2shadow::Vec3 lo, hi;
        instanceBox(leaf.modelMatrix, typeBounds_[leaf.typeID].mn, typeBounds_[leaf.typeID].mx, lo, hi);
        const mc2shadow::Rect r = mc2shadow::shadowFootprint(lo, hi, sun_, (std::min)(terrainMin_, lo.z), tanT);
        if (!out) out = r;
        else out = mc2shadow::Rect{(std::min)(out->x0, r.x0), (std::min)(out->y0, r.y0), (std::max)(out->x1, r.x1), (std::max)(out->y1, r.y1)};
    }
    return out;
}

// The incremental path (P5): caster events and height patches become dirty sub-tiles, recomputed in place a budget's worth per frame inside the frame's
// own command buffer (no waitIdle, no second mask). True when it recorded a batch.
bool ShadowBake::updateIncremental(VkCommandBuffer cb, uint64_t epoch, const ShadowBakeInputs& in) {
    if (!maskReady_ || layout_.texels == 0) return false;
    // The island asks once per pass, several times a frame: one batch per frame (a second would rewrite descriptor sets this frame already bound).
    if (incRecordedEpoch_ == epoch) return false;
    retireIncremental(epoch);
    const uint32_t par = static_cast<uint32_t>(epoch % kMaxFramesInFlight);
    constexpr uint32_t queryStride = 2 + kQueriesPerTile * kSubTilesPerFrame;
    if (timed_ && incQueryUsed_[par]) {
        uint64_t ts[queryStride] = {};
        const uint32_t count = 2 + incQueryRegions_[par] * kQueriesPerTile;
        if (vkGetQueryPoolResults(device_, incQueries_.get(), par * queryStride, count, count * sizeof(uint64_t), ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            const double scale = timestampPeriodNs_ * 1e-6;
            const double ms = static_cast<double>(ts[1] - ts[0]) * scale;
            incMaxGpuMs_ = (std::max)(incMaxGpuMs_, ms); incSumGpuMs_ += ms; ++incTimed_;
            for (uint32_t i = 0; i < incQueryRegions_[par]; ++i)
                for (uint32_t s = 0; s < 5; ++s) {
                    const uint32_t base = 2 + i * kQueriesPerTile;
                    incStageMs_[s] += static_cast<double>(ts[base + (s == 4 ? 5 : s + 1)] - ts[base + (s == 4 ? 0 : s)]) * scale;
                }
        }
        incQueryUsed_[par] = false;
    }
    mc2shadow::StaticCasterSet& set = mc2shadow::staticCasterSet();
    mc2terrain::TerrainFieldStore* store = gos_TerrainFieldStore();
    // MC2_SHADOW_DIAG: one pass of every sub-tile through the dirty path, so the comparison below covers the whole map.
    if (diagOn() && !diagSwept_) {
        diagSwept_ = true;
        const int n = layout_.texels / mc2shadow::kSubTileTexels;
        std::vector<mc2shadow::SubTile> all;
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) all.push_back({x, y});
        dirty_.add(all);
        eventsSinceDrain_ = true;
    }
    if (set.hasEvents()) {
        for (const auto& ev : set.takeEvents()) {
            objectDirty_ = true;
            applyLiveEvent(ev.before, ev.after);
            for (const auto* side : {&ev.before, &ev.after}) {
                for (const auto& leaf : *side) {
                    if (leaf.typeID >= typeBounds_.size() || !typeBounds_[leaf.typeID].valid) continue;
                    mc2shadow::Vec3 lo, hi;
                    instanceBox(leaf.modelMatrix, typeBounds_[leaf.typeID].mn, typeBounds_[leaf.typeID].mx, lo, hi);
                    const auto b = mc2shadow::lightBox(basis_, lo, hi);
                    if (!objectDirtyRect_) objectDirtyRect_ = mc2shadow::Rect{b.lx0, b.ly0, b.lx1, b.ly1};
                    else {
                        auto& r = *objectDirtyRect_;
                        r = {(std::min)(r.x0, b.lx0), (std::min)(r.y0, b.ly0), (std::max)(r.x1, b.lx1), (std::max)(r.y1, b.ly1)};
                    }
                }
                if (const auto r = leavesFootprint(*side)) {
                    const size_t added = dirty_.add(mc2shadow::subTilesForRect(layout_, grownForDirty(*r), 0));
                    if (eventsLogged_ < 8) {
                        ++eventsLogged_;
                        mc2::fmt::print_err("[SHADOW_BAKE] caster event: {} leaves {} footprint=({:.0f},{:.0f})-({:.0f},{:.0f}) dirty_sub_tiles+={}\n",
                                            side == &ev.before ? "removed" : "added", mc2::fmt::as_size(side->size()), r->x0, r->y0, r->x1, r->y1,
                                            mc2::fmt::as_size(added));
                    }
                }
            }
        }
        eventsSinceDrain_ = true;
    }
    if (store) {
        const auto patches = store->takeHeightPatches();
        if (!patches.empty()) {
            const float tanT = std::tan(mc2shadow::kSunAngularRadiusDeg * 0.01745329252f);
            for (const auto& hp : patches) {
                float top = -1e30f;
                for (int gy = hp.y0; gy <= hp.y1; ++gy)
                    for (int gx = hp.x0; gx <= hp.x1; ++gx) top = (std::max)(top, store->height()[static_cast<size_t>(gx) + static_cast<size_t>(gy) * store->mapSide()]);
                const mc2shadow::Vec3 lo{hp.x0 * mc2shadow::kWorldUnitsPerVertex - halfMap_, halfMap_ - hp.y1 * mc2shadow::kWorldUnitsPerVertex, terrainMin_};
                const mc2shadow::Vec3 hi{hp.x1 * mc2shadow::kWorldUnitsPerVertex - halfMap_, halfMap_ - hp.y0 * mc2shadow::kWorldUnitsPerVertex, (std::max)(top, terrainMin_)};
                dirty_.add(mc2shadow::subTilesForRect(layout_, grownForDirty(mc2shadow::shadowFootprint(lo, hi, sun_, terrainMin_, tanT)), 0));
            }
            heightsMoved_ = true;
            objectDirty_ = true;
            eventsSinceDrain_ = true;
        }
    }
    const auto logDirtyStats = [&] {
        const double n = incTimed_ ? incTimed_ : 1;
        if (mc2_diag::tagRequested("SHADOW_BAKE_SPLIT"))
        mc2::fmt::print_err("[SHADOW_BAKE_SPLIT] dirty gpu_ms clear={:.3f} casters={:.3f} pcss={:.3f} march={:.3f} resolve={:.3f} batches={}\n",
                            incStageMs_[4] / n, (incStageMs_[0] - incStageMs_[4]) / n, incStageMs_[1] / n, incStageMs_[2] / n, incStageMs_[3] / n, incTimed_);
        const uint64_t frames = epoch - statsEpoch_ + 1;
        mc2::fmt::print_err("[SHADOW_BAKE] dirty queue drained: drained={} sub_tiles={} batches={} batch_gpu_ms mean={:.3f} max={:.3f} (budget {} sub-tiles a frame) object_map_records={} dirty_batches={} local_tile_rebakes={} frames={} records_per_1000={:.2f} batches_per_1000={:.2f} local_tiles_per_1000={:.2f}\n",
                            dirty_.empty() && !objectDirty_, incSubTiles_, incBatches_, incTimed_ ? incSumGpuMs_ / incTimed_ : 0.0, incMaxGpuMs_, kSubTilesPerFrame,
                            objectMapRecords_, dirtyBatches_, localTileRebakes_, frames,
                            1000.0 * objectMapRecords_ / frames, 1000.0 * dirtyBatches_ / frames, 1000.0 * localTileRebakes_ / frames);
    };
    const bool statsDue = (epoch - statsEpoch_ + 1) % 1000 == 0;
    if (statsDue) logDirtyStats();
    if (dirty_.empty()) {
        // Reuse the regular object-only submission/retirement path after the
        // last dirty batch has stopped reading the shared caster resources.
        if (objectDirty_ && (!incActive_ || epoch >= lastIncEpoch_ + 2)) {
            recordedKey_ = bakedKey_;
            recordedObjectKey_ = objectKey_;
            recordedMask_ = false;
            if (!recordBake(cb, in)) return false;
            objectDirty_ = false;
            recorded_ = true;
            submitted_ = false;
            recordedEpoch_ = incRecordedEpoch_ = epoch;
            return true;
        }
        if (incActive_ && epoch >= lastIncEpoch_ + 2) {
            incActive_ = false;
            if (!statsDue) logDirtyStats();
            incSubTiles_ = incBatches_ = incTimed_ = 0;
            incMaxGpuMs_ = incSumGpuMs_ = 0.0;
            for (auto& s : incStageMs_) s = 0.0;
            if (eventsSinceDrain_ && diagOn()) diagRequest_ = true;
            eventsSinceDrain_ = false;
        }
        const uint64_t idleFrames=mc2shadow::shadowQuality().level==0?60:kDirtyIdleFrames;
        if (!incActive_ && incParams_ && epoch >= lastIncEpoch_ + idleFrames) {
            releaseTransients();
            releaseIncremental();
            mc2::fmt::print_err("[SHADOW_BAKE] dirty path idle {} frames: transients released\n", idleFrames);
        }
        if (diagRequest_ && !incActive_) { incRecordedEpoch_ = epoch; return recordDiagReadback(cb, epoch); }
        return false;
    }
    ZoneScopedN("ShadowBake.Dirty");
    // CPU state the sub-tiles read: the heights (re-read after a patch) and the live casters (re-resolved after an event).
    if (heightsMoved_ || heights_.empty() || readHeightVersion_ != store->heightVersion()) {
        ZoneScopedN("ShadowBake.Dirty.ReadHeights");
        if (!readHeightGrid()) return false;
    }
    if (liveStale_) { ZoneScopedN("ShadowBake.Dirty.RebuildLive"); objectFullDirty_ = true; rebuildLive(); }
    if (!ensureTransients() || !ensureIncremental()) return false;
    mc2shadow::DirtyBatch pendingBatch(dirty_, kSubTilesPerFrame);
    const auto& batch = pendingBatch.tiles;
    const float tanT = std::tan(mc2shadow::kSunAngularRadiusDeg * 0.01745329252f);
    std::vector<InstRun> runs;
    std::vector<GpuStaticPropInstance> inst;
    std::vector<TileWork> works(batch.size());
    std::vector<uint8_t> tp(batch.size() * kSlice, 0), ubo(batch.size() * kSlice, 0);
    {
    ZoneScopedN("ShadowBake.Dirty.Plan");
    for (size_t i = 0; i < batch.size(); ++i) {
        ShadowBakeTileParams p{};
        planRegion(mc2shadow::subTileRegionWorld(layout_, batch[i]), batch[i].x * mc2shadow::kSubTileTexels, batch[i].y * mc2shadow::kSubTileTexels,
                   mc2shadow::kSubTileTexels + 2 * mc2shadow::kTileApron, casterTop_, tanT, CasterView{&liveLeaves_, &liveBoxes_, &liveTops_, &liveIdx_},
                   runs, inst, works[i], p);
        std::memcpy(tp.data() + i * kSlice, &p, sizeof(p));
        CasterUBO u{};
        std::memcpy(u.lightSpace, p.window, sizeof(u.lightSpace));
        std::memcpy(ubo.data() + i * kSlice, &u, sizeof(u));
    }
    }
    if (inst.size() > incInstanceCap_) {
        blog("a dirty batch has more casters than its buffer: the batch is requeued; increase the instance capacity to make progress.");
        return false;
    }
    const VkDeviceSize parBase = static_cast<VkDeviceSize>(par) * kSubTilesPerFrame * kSlice;
    const VkDeviceSize instBase = static_cast<VkDeviceSize>(par) * incInstanceCap_ * sizeof(GpuStaticPropInstance);
    for (InstRun& r : runs) r.firstInstance += static_cast<uint32_t>(par * incInstanceCap_);
    if (!incParams_.upload(tp.data(), tp.size(), parBase) || !incCasterUbo_.upload(ubo.data(), ubo.size(), parBase) ||
        (!inst.empty() && !incInstances_.upload(inst.data(), inst.size() * sizeof(GpuStaticPropInstance), instBase))) return false;
    if (incHeightsGen_[par] != heightsGen_) {
        ZoneScopedN("ShadowBake.Dirty.HeightUpload");
        if (!makeGpuReadBuffer(incHeights_[par], allocator_, device_, heights_.size() * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "shadow.bake.dirtyHeights") ||
            !incHeights_[par].upload(heights_.data(), heights_.size() * sizeof(float))) return false;
        incHeightsGen_[par] = heightsGen_;
    }
    writeIncrementalSets(par);
    pendingBatch.commit();

    MC2_VK_GPU_SCOPE(gpuScope, cb, "VKIsl.Shadow.BakeDirty");
    if (timed_) vkCmdResetQueryPool(cb, incQueries_.get(), par * queryStride, queryStride);
    // The mask and the top image are rewritten in place: their earlier readers (the terrain, water and overlay passes) finish first.
    VkImageMemoryBarrier pre[6];
    pre[0] = imageBarrier(maskImage_.get(), VK_IMAGE_ASPECT_COLOR_BIT, 0, maskMips_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          0, VK_ACCESS_TRANSFER_WRITE_BIT);
    pre[1] = imageBarrier(topR8_.get(), VK_IMAGE_ASPECT_COLOR_BIT, 0, topMips_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          0, VK_ACCESS_TRANSFER_WRITE_BIT);
    for (int i = 0; i < 4; ++i)
        pre[2 + i] = imageBarrier(scratch_[i].get(), VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
                                  VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 6, pre);
    // Stamped once everything before the batch has finished, so the span is the batch's own GPU time.
    if (timed_) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, incQueries_.get(), par * queryStride);
    ZoneNamedN(zDirtyRec, "ShadowBake.Dirty.Record", true);
    for (size_t i = 0; i < batch.size(); ++i) {
        RegionJob job{mc2shadow::kSubTileTexels, mc2shadow::kSubTileTexels + 2 * mc2shadow::kTileApron, batch[i].x * mc2shadow::kSubTileTexels,
                      batch[i].y * mc2shadow::kSubTileTexels};
        const uint32_t off = static_cast<uint32_t>(parBase + i * kSlice);
        recordRegion(cb, in, job, works[i], std::span<const InstRun>(runs.data() + works[i].firstRun, works[i].runCount), incComputeSet_[par],
                     incWindowSet_[par], off, off, 0, kSliceSubTile, timed_ ? static_cast<int>(par * queryStride + 2 + i * kQueriesPerTile) : -1);
    }
    recordTopLevels(cb);
    recordMaskToRead(cb, false);
    if (timed_) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, incQueries_.get(), par * queryStride + 1);
    gpuScope.end();
    incQueryUsed_[par] = true;
    incQueryRegions_[par] = static_cast<uint32_t>(batch.size());
    incActive_ = true;
    lastIncEpoch_ = epoch;
    incRecordedEpoch_ = epoch;
    incSubTiles_ += static_cast<uint32_t>(batch.size());
    ++incBatches_;
    ++dirtyBatches_;
    mc2_hitch2::noteBake("dirty", 1, 0);
    return true;
}

// The incremental path's own buffers and descriptor sets: per-parity slices of the region parameters, caster matrices and casters, and a set per parity
// (rewritten only on its own parity's frame, after the island waited that parity's fence).
bool ShadowBake::ensureIncremental() {
    if (incParams_) return true;
    // Publish the buffers together, only after every allocation/upload and the
    // query pool succeeded. Residency refusal must leave the next call cold;
    // otherwise incParams_ alone skips creation of the remaining resources.
    OwnedBuffer params, casterUbo, passBuffer, instances;
    incInstanceCap_ = 65536;
    if (!makeGpuReadBuffer(params, allocator_, device_, kMaxFramesInFlight * kSubTilesPerFrame * kSlice, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, "shadow.bake.dirtyParams") ||
        !makeGpuReadBuffer(casterUbo, allocator_, device_, kMaxFramesInFlight * kSubTilesPerFrame * kSlice, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, "shadow.bake.dirtyCasterUbo") ||
        !makeGpuReadBuffer(passBuffer, allocator_, device_, 3 * kSlice, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, "shadow.bake.dirtyPass") ||
        !makeGpuReadBuffer(instances, allocator_, device_, kMaxFramesInFlight * incInstanceCap_ * sizeof(GpuStaticPropInstance), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "shadow.bake.dirtyInstances")) {
        blog("dirty-update buffers failed.");
        return false;
    }
    std::vector<uint8_t> pp3(3 * kSlice, 0);
    for (int32_t m = 0; m < 3; ++m) {
        const int32_t pass[4]={m,mc2shadow::shadowQuality().level,0,0};
        std::memcpy(pp3.data()+m*kSlice,pass,sizeof(pass));
    }
    if (!passBuffer.upload(pp3.data(), pp3.size())) return false;
    if (!incQueries_) {
        VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qci.queryCount = (2 + kQueriesPerTile * kSubTilesPerFrame) * kMaxFramesInFlight;
        if (!create(incQueries_, device_, [&](auto* h) { return vkCreateQueryPool(device_, &qci, nullptr, h); })) { blog("dirty query pool failed."); return false; }
    }
    for (auto& gen : incHeightsGen_) gen = 0;
    for (auto& used : incQueryUsed_) used = false;
    incCasterUbo_ = std::move(casterUbo);
    incPass_ = std::move(passBuffer);
    incInstances_ = std::move(instances);
    incParams_ = std::move(params);
    return true;
}

void ShadowBake::writeIncrementalSets(uint32_t par) {
    const VkDescriptorBufferInfo tile{incParams_.get(), 0, sizeof(ShadowBakeTileParams)};
    const VkDescriptorBufferInfo pass{incPass_.get(), 0, 16};
    const VkDescriptorImageInfo raw{depthRawSampler_.get(), windowView_.get(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo cmp{depthCompareSampler_.get(), windowView_.get(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    const VkDescriptorBufferInfo hb{incHeights_[par].get(), 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo sb{staging_.get(), 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo si[4];
    for (int i = 0; i < 4; ++i) si[i] = {VK_NULL_HANDLE, scratchView_[i].get(), VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorBufferInfo ubo{incCasterUbo_.get(), 0, sizeof(CasterUBO)};
    const VkDescriptorBufferInfo insts{incInstances_.get(), 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w[12]{};
    for (auto & wElem : w) { wElem.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wElem.dstSet = incComputeSet_[par]; wElem.descriptorCount = 1; }
    w[0].dstBinding = 0; w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; w[0].pBufferInfo = &tile;
    w[1].dstBinding = 9; w[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; w[1].pBufferInfo = &pass;
    w[2].dstBinding = 1; w[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[2].pImageInfo = &raw;
    w[3].dstBinding = 2; w[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[3].pImageInfo = &cmp;
    w[4].dstBinding = 3; w[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[4].pBufferInfo = &hb;
    for (int i = 0; i < 4; ++i) { w[5 + i].dstBinding = static_cast<uint32_t>(4 + i); w[5 + i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[5 + i].pImageInfo = &si[i]; }
    w[9].dstBinding = 8; w[9].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[9].pBufferInfo = &sb;
    w[10].dstSet = incWindowSet_[par]; w[10].dstBinding = 0; w[10].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; w[10].pBufferInfo = &ubo;
    w[11].dstSet = incWindowSet_[par]; w[11].dstBinding = 1; w[11].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[11].pBufferInfo = &insts;
    vkUpdateDescriptorSets(device_, 12, w, 0, nullptr);
}

void ShadowBake::retireIncremental(uint64_t epoch) {
    if (!diagPending_ || epoch < diagEpoch_ + kMaxFramesInFlight || epoch % kMaxFramesInFlight != diagEpoch_ % kMaxFramesInFlight) return;
    diagPending_ = false;
    void* p = diagBuf_.map();
    if (p) {
        vmaInvalidateAllocation(allocator_, diagBuf_.allocation(), 0, VK_WHOLE_SIZE);
        diagInc_.assign(static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + mc2shadow::bc4ChainBytes(static_cast<uint32_t>(maskTexels_), maskMips_));
        diagBuf_.unmap();
        diagForceFull_ = true;   // the next update re-bakes the whole mask from the same state, without the cache, and compares
    }
    diagBuf_.reset();
}

void ShadowBake::releaseIncremental() {
    incParams_.reset(); incCasterUbo_.reset(); incPass_.reset(); incInstances_.reset();
    for (auto& heights : incHeights_) heights.reset();
    for (auto& gen : incHeightsGen_) gen = 0;
    for (auto& used : incQueryUsed_) used = false;
}

// The live casters (the bake's caster set as the registry has it now) with their light boxes: built once, then edited by each event.
void ShadowBake::rebuildLive() {
    liveStale_ = false;
    std::vector<GpuStaticPropInstance> all;
    mc2shadow::staticCasterSet().resolve(all, false);
    liveLeaves_.clear(); liveBoxes_.clear(); liveTops_.clear(); liveIdx_.clear();
    for (const GpuStaticPropInstance& leaf : all) appendLive(leaf);
}

void ShadowBake::appendLive(const GpuStaticPropInstance& source) {
    GpuStaticPropInstance leaf = source;
    if (leaf.typeID >= typeBounds_.size() || !typeBounds_[leaf.typeID].valid) return;
    mc2shadow::Vec3 lo, hi;
    instanceBox(leaf.modelMatrix, typeBounds_[leaf.typeID].mn, typeBounds_[leaf.typeID].mx, lo, hi);
    liveIdx_.push_back(static_cast<uint32_t>(liveLeaves_.size()));
    liveLeaves_.push_back(leaf);
    liveBoxes_.push_back(mc2shadow::lightBox(basis_, lo, hi));
    liveTops_.push_back(hi.z);
}

// One caster event applied to the live arrays: its old leaves out (swap-remove), its new leaves in. A leaf that cannot be found marks the arrays
// stale, and the next batch rebuilds them whole.
void ShadowBake::applyLiveEvent(const std::vector<GpuStaticPropInstance>& before, const std::vector<GpuStaticPropInstance>& after) {
    if (liveStale_) return;
    for (const GpuStaticPropInstance& beforeLeaf : before) {
        GpuStaticPropInstance leaf = beforeLeaf;
        if (leaf.typeID >= typeBounds_.size() || !typeBounds_[leaf.typeID].valid) continue;
        bool found = false;
        for (size_t j = 0; j < liveLeaves_.size(); ++j) {
            if (liveLeaves_[j].typeID != leaf.typeID) continue;
            // Match StaticCasterSet::sameLeaves: registry matrices wobble in
            // their last bits when rebuilt from the object's float state.
            bool same = true;
            for (int k = 0; k < 16; ++k) {
                const bool translation = (k == 3 || k == 7 || k == 11);
                if (std::fabs(liveLeaves_[j].modelMatrix[k] - leaf.modelMatrix[k]) > (translation ? 0.75f : 0.01f)) { same = false; break; }
            }
            if (!same) continue;
            liveLeaves_[j] = liveLeaves_.back(); liveLeaves_.pop_back();
            liveBoxes_[j] = liveBoxes_.back(); liveBoxes_.pop_back();
            liveTops_[j] = liveTops_.back(); liveTops_.pop_back();
            liveIdx_.pop_back();   // the index array is the identity
            found = true;
            break;
        }
        if (!found) { liveStale_ = true; ++liveRebuilds_; return; }
    }
    for (const GpuStaticPropInstance& leaf : after) appendLive(leaf);
}

bool ShadowBake::diagOn() {
    static const bool s_on = mc2_diag::tagRequested("SHADOW_BAKE_COMPARE");
    return s_on;
}

// MC2_SHADOW_DIAG: the mask after its dirty updates is read back; once it retires, a fresh bake of the same state is compared with it texel by texel.
bool ShadowBake::recordDiagReadback(VkCommandBuffer cb, uint64_t epoch) {
    diagRequest_ = false;
    const uint64_t chain = mc2shadow::bc4ChainBytes(static_cast<uint32_t>(maskTexels_), maskMips_);
    if (!makeHostBuffer(diagBuf_, allocator_, device_, chain, VK_BUFFER_USAGE_TRANSFER_DST_BIT, "shadow.bake.diag")) return false;
    VkImageMemoryBarrier toSrc = imageBarrier(maskImage_.get(), VK_IMAGE_ASPECT_COLOR_BIT, 0, maskMips_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, VK_ACCESS_TRANSFER_READ_BIT);
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSrc);
    const std::vector<VkBufferImageCopy> lv = chainRegions();
    vkCmdCopyImageToBuffer(cb, maskImage_.get(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, diagBuf_.get(), maskMips_, lv.data());
    cmdBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    VkImageMemoryBarrier toRead = imageBarrier(maskImage_.get(), VK_IMAGE_ASPECT_COLOR_BIT, 0, maskMips_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT);
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toRead);
    diagPending_ = true;
    diagEpoch_ = epoch;
    return true;
}

// Level 0 of two BC4 chains, decoded and compared: the dirty-updated mask against a fresh bake of the same state (the save/load claim: a load bakes the
// loaded state afresh, so the two must agree).
void ShadowBake::compareChains(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) const {
    const size_t blocks = static_cast<size_t>(maskTexels_ / 4) * static_cast<size_t>(maskTexels_ / 4);
    if (a.size() < blocks * 8 || b.size() < blocks * 8) return;
    uint64_t hashA = mc2shadow::hashWords(a.data(), blocks * 8), hashB = mc2shadow::hashWords(b.data(), blocks * 8);
    std::vector<uint32_t> hist(256, 0);
    double sum = 0.0;
    int maxDiff = 0;
    for (size_t i = 0; i < blocks; ++i) {
        std::array<uint8_t, 8> ba{}, bb{};
        std::memcpy(ba.data(), a.data() + i * 8, 8);
        std::memcpy(bb.data(), b.data() + i * 8, 8);
        const auto pa = mc2shadow::decodeBc4Block(ba), pb = mc2shadow::decodeBc4Block(bb);
        for (int k = 0; k < 16; ++k) {
            const int d = std::abs(static_cast<int>(pa[static_cast<size_t>(k)]) - static_cast<int>(pb[static_cast<size_t>(k)]));
            ++hist[static_cast<size_t>(d)];
            sum += d;
            maxDiff = (std::max)(maxDiff, d);
        }
    }
    const uint64_t total = static_cast<uint64_t>(blocks) * 16;
    {   // a quarter-resolution picture of both masks and their difference, next to the cook cache, for a look
        const uint32_t n = static_cast<uint32_t>(maskTexels_), q = n / 4, bpr = n / 4;
        std::vector<uint8_t> img(static_cast<size_t>(q) * q * 3);
        for (uint32_t y = 0; y < q; ++y)
            for (uint32_t x = 0; x < q; ++x) {
                const size_t blk = static_cast<size_t>(y) * bpr + x;   // one block per output texel: its first texel
                std::array<uint8_t, 8> ba{}, bb{};
                std::memcpy(ba.data(), a.data() + blk * 8, 8);
                std::memcpy(bb.data(), b.data() + blk * 8, 8);
                const auto pa = mc2shadow::decodeBc4Block(ba), pb = mc2shadow::decodeBc4Block(bb);
                const size_t o3 = (static_cast<size_t>(y) * q + x) * 3;
                img[o3 + 0] = pa[0];
                img[o3 + 1] = pb[0];
                img[o3 + 2] = static_cast<uint8_t>((std::min)(255, std::abs(static_cast<int>(pa[0]) - static_cast<int>(pb[0])) * 4));
            }
        const std::string path = (std::filesystem::path(cacheDir_) / "diag_dirty_fresh_diff.ppm").string();
        if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
            std::fprintf(f, "P6\n%u %u\n255\n", q, q);
            std::fwrite(img.data(), 1, img.size(), f);
            std::fclose(f);
            mc2::fmt::print_err("[SHADOW_BAKE] diag picture (R dirty, G fresh, B |diff|x4): {}\n", path);
        }
    }
    uint64_t acc = 0;
    int p999 = 0;
    for (int d = 0; d < 256; ++d) { acc += hist[static_cast<size_t>(d)]; if (acc * 1000 >= total * 999) { p999 = d; break; } }
    mc2::fmt::print_err("[SHADOW_BAKE] diag dirty_vs_fresh: hash_dirty={:016x} hash_fresh={:016x} max_abs={} mean_abs={:.4f} p99.9_abs={} texels={}\n",
                        mc2::fmt::as_u64(hashA), mc2::fmt::as_u64(hashB), maxDiff, sum / static_cast<double>(total), p999, mc2::fmt::as_u64(total));
}

// The object map: the whole height field as the old static map drew it, then every caster, with the engine's static light matrix.
bool ShadowBake::recordObjectMap(VkCommandBuffer cb, const ShadowBakeInputs& in, uint32_t queryBase) {
    ZoneScopedN("ShadowBake.ObjectMap");
    const mc2terrain::TerrainFieldStore* store = gos_TerrainFieldStore();
    if (!store) return false;
    if (terrainIndexCount_ == 0 || objectHeightVersion_ != store->heightVersion() || recordedObjectMatrix_ != objectMatrix_) {
        const bool visual = terrainVisualDisplaceGateOn() && !store->visualHeight().empty() && store->visualSide() == (store->mapSide() - 1) * 4 + 1;
        const int side = visual ? store->visualSide() : store->mapSide();
        const auto height = mc2shadow::maskHeightGrid(visual ? store->visualHeight() : store->height(), side, side - 1,
                                                   visual, store->height(), terrainVisualDisplaceFarParam());
        const float spacing = visual ? 32.0f : mc2shadow::kWorldUnitsPerVertex;
        std::vector<float> positions(static_cast<size_t>(side) * side * 3);
        for (int ay = 0; ay < side; ++ay)
            for (int ax = 0; ax < side; ++ax) {
                const size_t v = static_cast<size_t>(ax) + static_cast<size_t>(ay) * side;
                positions[v * 3 + 0] = static_cast<float>(ax) * spacing - halfMap_;
                positions[v * 3 + 1] = halfMap_ - static_cast<float>(ay) * spacing;
                positions[v * 3 + 2] = height[v];
            }
        std::vector<uint32_t> indices;
        indices.reserve(static_cast<size_t>(side - 1) * (side - 1) * 6);
        for (int ay = 0; ay < side - 1; ++ay)
            for (int ax = 0; ax < side - 1; ++ax) {
                const uint32_t v0 = static_cast<uint32_t>(ax + ay * side), v1 = v0 + 1, v3 = v0 + static_cast<uint32_t>(side), v2 = v3 + 1;
                const auto& raw = store->visualHeight();
                const bool trBl = visual && mc2terrain::quadSplitsTrBl(raw[v0], raw[v1], raw[v3], raw[v2]);
                if (visual ? !trBl : (ax & 1) == (ay & 1)) indices.insert(indices.end(), {v0, v1, v2, v0, v2, v3});
                else                      indices.insert(indices.end(), {v0, v1, v3, v1, v2, v3});
            }
        if (!terrainVbo_.createFilled(allocator_, device_, positions.data(), positions.size() * sizeof(float), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, "shadow.bake.terrainVbo") ||
            !terrainIbo_.createFilled(allocator_, device_, indices.data(), indices.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, "shadow.bake.terrainIbo")) {
            blog("object map terrain mesh failed.");
            return false;
        }
        terrainIndexCount_ = static_cast<uint32_t>(indices.size());
        terrainRows_.clear();
        for (int y = 0; y < side - 1; ++y) {
            mc2shadow::Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
            for (int row = y; row <= y + 1; ++row) for (int x = 0; x < side; ++x) {
                const size_t v = static_cast<size_t>(x + row * side) * 3;
                lo.x = (std::min)(lo.x, positions[v]); hi.x = (std::max)(hi.x, positions[v]);
                lo.y = (std::min)(lo.y, positions[v + 1]); hi.y = (std::max)(hi.y, positions[v + 1]);
                lo.z = (std::min)(lo.z, positions[v + 2]); hi.z = (std::max)(hi.z, positions[v + 2]);
            }
            terrainRows_.push_back(mc2shadow::lightBox(basis_, lo, hi));
        }
        objectHeightVersion_ = store->heightVersion();
    }
    if (queryCount_) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries_.get(), queryBase);
    MC2_VK_GPU_SCOPE(gpuScope, cb, "VKIsl.Shadow.BakeObjectMap");
    // Dynamic rendering pass over the object map (same layout moves and dependencies as the window map above).
    mc2vk::VulkanContext& dctx = mc2vk::getOrCreateEngineVulkanContext();
    mc2vk::dynrender::depthImageBarrier(dctx, cb, objectImage_.get(), objectPartial_ ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, objectPartial_ ? VK_ACCESS_2_SHADER_READ_BIT : 0,
                                        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    mc2vk::dynrender::beginDepthOnly(dctx, cb, objectView_.get(), kObjectFmt, static_cast<uint32_t>(objectMapSize()),
                                     static_cast<uint32_t>(objectMapSize()), objectPartial_ ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR);
    if (objectPartial_) {
        VkClearAttachment clear{}; clear.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT; clear.clearValue.depthStencil = {1.0f, 0};
        const VkClearRect rect{objectScissor_, 0, 1};
        vkCmdClearAttachments(cb, 1, &clear, 1, &rect);
    }
    if (queryCount_) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries_.get(), queryBase + 1);
    const VkViewport vp{0.0f, 0.0f, static_cast<float>(objectMapSize()), static_cast<float>(objectMapSize()), 0.0f, 1.0f};
    const VkRect2D sc = objectScissor_;
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    const uint32_t objOff = static_cast<uint32_t>(tiles_.size()) * static_cast<uint32_t>(kSlice);
    const VkDeviceSize zero = 0;
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, casterLayout_.get(), 0, 1, &objectCasterSet_, 1, &objOff);
    vkCmdSetDepthBias(cb, 0.0f, 0.0f, 0.0f);
    mc2vk::infra::bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, objectTerrain_.get());
    const VkBuffer tv = terrainVbo_.get();
    vkCmdBindVertexBuffers(cb, 0, 1, &tv, &zero);
    vkCmdBindIndexBuffer(cb, terrainIbo_.get(), 0, VK_INDEX_TYPE_UINT32);
    if (objectPartial_ && !terrainRows_.empty()) {
        const uint32_t rowIndices = terrainIndexCount_ / static_cast<uint32_t>(terrainRows_.size());
        for (uint32_t y = 0; y < terrainRows_.size(); ++y) {
            const auto& b = terrainRows_[y];
            if (objectDirtyRect_->overlaps({b.lx0, b.ly0, b.lx1, b.ly1}))
                mc2::battlebench::add(mc2::battlebench::DrawShadow), vkCmdDrawIndexed(cb, rowIndices, 1, y * rowIndices, 0, 0);
        }
    } else mc2::battlebench::add(mc2::battlebench::DrawShadow), vkCmdDrawIndexed(cb, terrainIndexCount_, 1, 0, 0, 0);
    if (queryCount_) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries_.get(), queryBase + 2);
    if (!objectRuns_.empty()) {
        vkCmdSetDepthBias(cb, 2.0f, 0.0f, 1.0f);
        mc2vk::infra::bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, objectOpaque_.get());
        vkCmdBindVertexBuffers(cb, 0, 1, &in.vbo, &zero);
        vkCmdBindIndexBuffer(cb, in.ibo, 0, VK_INDEX_TYPE_UINT32);
        drawRuns(cb, in, objectRuns_, objectOpaque_.get(), objectAlpha_.get());
    }
    mc2vk::dynrender::end(dctx, cb);
    mc2vk::dynrender::depthImageBarrier(dctx, cb, objectImage_.get(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    gpuScope.end();
    if (queryCount_) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries_.get(), queryBase + 3);
    objectInitialized_ = true;
    ++objectMapRecords_;
    mc2_hitch2::noteBake(coldPending_ ? std::string_view{} : warm_ && recordedMask_ ? "warm" : "object", 0, 1);
    return true;
}

}  // namespace mc2vk

