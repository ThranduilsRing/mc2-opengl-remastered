// vulkan_shadow_bake.cpp -- see the header. Compiles to nothing unless MC2_VULKAN_ISLAND is defined.
#ifdef MC2_VULKAN_ISLAND

#include "vulkan_shadow_bake.h"
#include "vulkan_shadow_bake_internal.h"

#include <vk_mem_alloc.h>

#include "../../RenderCore/axis_convert.h"
#include "../../RenderCore/gpu_abi/GpuStaticPropInstance.h"
#include "gos_postprocess.h"
#include "mc2fmt.h"
#include "Gates/gate.h"
#include "gameos.hpp"   // gos_GetUserDataDirectory
#include "shadow_mask_asset.h"
#include "shadow_dirty_tiles.h"
#include "static_caster_set.h"
#include "terrain_field_store.h"
#include "vk_depth_caster_pipeline.h"
#include "vk_gpu_profile.h"
#include "vk_island_util.h"
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

extern void gos_GetTerrainLightDir(float* x, float* y, float* z);
extern void gos_GetTerrainChunkGridInfo(int* mapSide, float* halfMap);
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
    for (const char* name : {"shadow_bake_pcss.comp.spv", "shadow_bake_terrain_march.comp.spv", "shadow_bake_resolve.comp.spv", "shadow_bake_bc4.comp.spv"}) {
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
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 4}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4}};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 4; pci.poolSizeCount = 2; pci.pPoolSizes = sizes;
    if (!create(casterPool_, device_, [&](auto* h) { return vkCreateDescriptorPool(device_, &pci, nullptr, h); })) { blog("caster pool failed."); return false; }
    const VkDescriptorSetLayout four[4] = {casterDsl_.get(), casterDsl_.get(), casterDsl_.get(), casterDsl_.get()};
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = casterPool_.get(); dsai.descriptorSetCount = 4; dsai.pSetLayouts = four;
    VkDescriptorSet sets[4]{};
    if (vkAllocateDescriptorSets(device_, &dsai, sets) != VK_SUCCESS) { blog("caster sets failed."); return false; }
    windowCasterSet_ = sets[0];
    objectCasterSet_ = sets[1];
    incWindowSet_[0] = sets[2];
    incWindowSet_[1] = sets[3];

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
    const VkDescriptorPoolSize csizes[4] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 6}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 6},
                                            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 6}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 12}};
    VkDescriptorPoolCreateInfo cpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    cpci.maxSets = 3; cpci.poolSizeCount = 4; cpci.pPoolSizes = csizes;
    if (!create(computePool_, device_, [&](auto* h) { return vkCreateDescriptorPool(device_, &cpci, nullptr, h); })) { blog("compute pool failed."); return false; }
    const VkDescriptorSetLayout cdsl[3] = {pcss_.setLayout(), pcss_.setLayout(), pcss_.setLayout()};
    VkDescriptorSetAllocateInfo cai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    cai.descriptorPool = computePool_.get(); cai.descriptorSetCount = 3; cai.pSetLayouts = cdsl;
    VkDescriptorSet csets[3]{};
    if (vkAllocateDescriptorSets(device_, &cai, csets) != VK_SUCCESS) { blog("compute set failed."); return false; }
    computeSet_ = csets[0];
    incComputeSet_[0] = csets[1];
    incComputeSet_[1] = csets[2];

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
    if (vkAllocateDescriptorSets(device_, &bai, bsets) != VK_SUCCESS) { blog("BC4 sets failed."); return false; }
    bc4TileSet_ = bsets[0];
    bc4TopSet_ = bsets[1];
    {
        std::array<char, 1024> userDir{};
        if (gos_GetUserDataDirectory(userDir.data(), static_cast<int>(userDir.size())))
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

void ShadowBake::releaseTransients() {
    ZoneScopedN("ShadowBake.ReleaseTransients");
    windowFb_.reset(); windowView_.reset(); windowImage_.reset();
    for (int i = 0; i < 4; ++i) { scratchView_[i].reset(); scratch_[i].reset(); }
    staging_.reset(); tileParams_.reset(); passParams_.reset(); casterUbo_.reset(); instances_.reset(); heightBuf_.reset();
    tileR8View_.reset(); tileR8_.reset(); blockStaging_.reset(); bc4Params_.reset(); transfer_.reset();
    queries_.reset();
    queryCount_ = 0;
    leaves_.clear(); leaves_.shrink_to_fit();
    tiles_.clear(); runs_.clear();
    heights_.clear(); heights_.shrink_to_fit();
    transientBytes_ = 0;
}

void ShadowBake::invalidate() {
    // A map boundary may wait for its disk worker; gameplay frames never wait for it.
    if (cacheStore_.valid()) cacheStore_.get();
    coldPending_ = false; coldNextTile_ = 0;
    cacheLoad_ = {};
    cacheLoadKey_ = 0;
    releaseTransients();
    objectFb_.reset(); objectView_.reset(); objectImage_.reset();
    maskView_.reset(); maskImage_.reset();
    topR8View_.reset(); topR8_.reset();
    topSide_ = topMips_ = 0;
    topInitialized_ = false;
    terrainVbo_.reset(); terrainIbo_.reset(); objectInstances_.reset(); objectUbo_.reset();
    terrainIndexCount_ = 0;
    objectHeightVersion_ = ~0ull;
    typeBounds_.clear();
    maskTexels_ = 0; maskMips_ = 0;
    maskInitialized_ = objectInitialized_ = false;
    maskReady_ = objectReady_ = false;
    recorded_ = submitted_ = recordedMask_ = false;
    releaseIncremental();
    incQueries_.reset();
    incActive_ = heightsMoved_ = eventsSinceDrain_ = false;
    diagRequest_ = diagPending_ = diagForceFull_ = false;
    diagBuf_.reset();
    diagInc_.clear();
    heights_.clear();
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
    const uint64_t delta=set.deltaVersion(), height=store->heightVersion();
    keys.object=mc2shadow::fnv1a(&delta,sizeof(delta),keys.object);
    keys.object=mc2shadow::fnv1a(&height,sizeof(height),keys.object)|1u;
    return keys;
}

bool ShadowBake::ensureProducts(int maskTexels) {
    ZoneScopedN("ShadowBake.Products");
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
    // The coarse top image: mask level kTopLevel (4 texels a sub-tile) and its own mips, the levels whose blocks span more than one sub-tile. Kept with the
    // mask so a later tile rebake (P5) can rebuild the top levels without the rest of the map.
    topSide_ = static_cast<uint32_t>(maskTexels) >> kTopLevel;
    topMips_ = mips - kTopLevel;
    dirty_.reset(maskTexels / mc2shadow::kSubTileTexels);
    if (!makeImage(topR8_, topR8View_, allocator_, device_, kWorkFmt, topSide_, topMips_,
                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT, "shadow.bake.maskTop")) {
        blog("mask top image creation failed.");
        return false;
    }
    topInitialized_ = false;
    if (!objectImage_) {
        if (!makeImage(objectImage_, objectView_, allocator_, device_, kObjectFmt, objectMapSize(), 1,
                       VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, "shadow.bake.objectMap") ||
            !makeFramebuffer(objectFb_, device_, objectPass_.get(), objectView_.get(), objectMapSize())) {
            blog("object map creation failed.");
            return false;
        }
        objectInitialized_ = false;
    }
    return true;
}

bool ShadowBake::ensureTransients() {
    ZoneScopedN("ShadowBake.Transients");
    if (!windowImage_) {
        if (!makeImage(windowImage_, windowView_, allocator_, device_, kWindowFmt, mc2shadow::shadowQuality().windowSide, 1,
                       VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, "shadow.bake.window") ||
            !makeFramebuffer(windowFb_, device_, windowPass_.get(), windowView_.get(), mc2shadow::shadowQuality().windowSide)) {
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
        if (!makeImage(tileR8_, tileR8View_, allocator_, device_, kWorkFmt, mc2shadow::kTileTexels, kTileLevels,
                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT, "shadow.bake.tileLevels")) {
            blog("tile level image creation failed.");
            return false;
        }
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
                    mc2vk::infra::bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, alpha);
                    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, casterLayout_.get(), 1, 1, &in.alphaSet, 0, nullptr);
                    alphaBound = true;
                }
                const CasterPacketAlpha& pa = in.packetAlpha[pk];
                const struct { float scale[2]; int32_t layer; int32_t pad; } pc = {{pa.scaleX, pa.scaleY}, pa.layer, 0};
                vkCmdPushConstants(cb, casterLayout_.get(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
            } else if (alphaBound) {
                mc2vk::infra::bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, opaque);
                alphaBound = false;
            }
            vkCmdDrawIndexed(cb, pkt.indexCount, run.count, pkt.firstIndex, static_cast<int32_t>(pkt.baseVertex), run.firstInstance);
        }
    }
}

bool ShadowBake::update(VkCommandBuffer cb, uint64_t epoch, const ShadowBakeInputs& in) {
    if (!inited_) return false;
    retire(epoch);
    // Cold tiles only read retained CPU uploads; queue barriers order shared scratch reuse.
    // Continue next frame without waiting two epochs; only the final submit must retire before release.
    if (recorded_ && !(coldPending_ && submitted_ && epoch > submittedEpoch_)) return false;
    if (coldPending_) {
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
    if (recorded_) return true;
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
    if (!submitted_) { if (epoch != recordedEpoch_) recorded_ = false; return; }
    if (epoch < submittedEpoch_ + 2 || (epoch & 1u) != (submittedEpoch_ & 1u)) return;
    recorded_ = false;
    submitted_ = false;
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
            const double object = span(tail + 2, tail + 3);
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
        mc2shadow::shadowQuality().level,double(maskBytes)/1048576.0,double(objectBytes)/1048576.0,
        double(objectBytes)/1048576.0,double(maskBytes+2*objectBytes)/1048576.0,double(transientBytes_)/1048576.0);
    const bool completedMask=recordedMask_;
    recordedMask_ = false;
    // Object-only rebuilds share the dirty path's retained window/encoder images.
    if (completedMask) releaseTransients();
}

bool ShadowBake::readHeightGrid() {
    const auto* store = gos_TerrainFieldStore();
    if (!store) return false;
    const int coarseSide = store->mapSide();
    const bool visual = !store->visualHeight().empty() && store->visualSide() == (coarseSide - 1) * 4 + 1;
    const int sourceSide = visual ? store->visualSide() : coarseSide;
    heights_ = mc2shadow::maskHeightGrid(visual ? store->visualHeight() : store->height(), sourceSide, layout_.texels);
    if (heights_.empty()) return false;
    heightSide_ = (std::min)(sourceSide, layout_.texels + 1);
    heightSpacing_ = float(coarseSide - 1) * mc2shadow::kWorldUnitsPerVertex / float(heightSide_ - 1);
    return true;
}

bool ShadowBake::recordColdTile(VkCommandBuffer cb, const ShadowBakeInputs& in) {
    const uint32_t t = coldNextTile_++;
    const int tx = int(t) % layout_.tilesPerSide, ty = int(t) / layout_.tilesPerSide;
    const auto& tile = tiles_[t];
    const RegionJob job{mc2shadow::kTileTexels, mc2shadow::kRegionTexels, tx * mc2shadow::kTileTexels, ty * mc2shadow::kTileTexels};
    recordRegion(cb, in, job, tile, std::span<const InstRun>(runs_.data() + tile.firstRun, tile.runCount), computeSet_, windowCasterSet_,
                 t * uint32_t(kSlice), t * uint32_t(kSlice), 0, 0, queryCount_ ? int(t * kQueriesPerTile) : -1);
    coldPending_ = coldNextTile_ < tiles_.size();
    if (!coldPending_) {
        const uint32_t tail = uint32_t(tiles_.size()) * kQueriesPerTile;
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
    if (!readHeightGrid()) { blog("height field malformed: no bake."); return false; }
    terrainTop_ = -1e30f;
    terrainMin_ = 1e30f;
    for (float h : heights_) { terrainTop_ = (std::max)(terrainTop_, h); terrainMin_ = (std::min)(terrainMin_, h); }
    terrainMin_ = (std::max)(terrainMin_, waterZ_);
    if (recordedMask_) bakedHeightVersion_ = store->heightVersion();

    if (!ensureProducts(layout_.texels > 0 ? layout_.texels : maskTexels_)) return false;
    if (typeBounds_.empty() && in.vbo != VK_NULL_HANDLE && !computeTypeBounds(in)) { blog("static-prop geometry not readable: no bake."); return false; }

    // Casters: the gameplay set resolved to registry leaves, each with its world box and light-space box.
    const clk::time_point tResolve = clk::now();
    // Snapshot only on the first mask of this set; subsequent rebakes key and render the live state.
    const auto& casters=mc2shadow::staticCasterSet();
    const uint64_t casterHash=recordedMask_?casters.resolveForBake(bakedSetVersion_, leaves_):casters.resolve(leaves_);
    if (recordedMask_) casters.logDriftSinceLoad(6);
    cpuStage_[0] = std::chrono::duration<double, std::milli>(clk::now() - tResolve).count();
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
    casterTop_ = casterTop;

    // Object receivers always use current gameplay leaves.
    std::vector<GpuStaticPropInstance> objInst;
    casters.resolve(objInst);
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
    objectRuns_.clear();
    appendRuns(objectRuns_, objTypes, 0);

    // A mask whose inputs a cooked asset already covers is uploaded, not baked.
    std::optional<mc2shadow::ShadowMaskAsset> cached;
    bool warm = false;
    double warmReadMs = 0.0;
    if (recordedMask_) {
        const clk::time_point k0 = clk::now();
        contentKey_ = contentKey(in, casterHash);
        cpuStage_[1] = std::chrono::duration<double, std::milli>(clk::now() - k0).count();
        if (cacheLoad_.valid() && cacheLoadKey_ != contentKey_) {
            // The old request has finished (update polls it); a sun/preset edit needs a new key.
            (void)cacheLoad_.get();
        }
        if (!diagForceFull_ && !cacheLoad_.valid()) {
            cacheLoadKey_ = contentKey_;
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

    // The bake owns the full-map object projection. It is paired with the
    // submitted D16 image, independently of camera history or an engine latch.
    const float span=(mapSide_-1)*mc2shadow::kWorldUnitsPerVertex;
    const mc2shadow::Rect full{-halfMap_,halfMap_-span,-halfMap_+span,halfMap_};
    const auto objectWindow=mc2shadow::casterWindow(basis_,full,terrainMin_,terrainTop_,casterTop_,tanT,objectMapSize());
    mc2shadow::windowMatrix(basis_,objectWindow,recordedObjectMatrix_.data());
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
        for (int i = 0; i < 4; ++i) { w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].descriptorCount = 1; }
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
        ZoneScopedN("ShadowBake.Queries");
        queryCount_ = tileCount * kQueriesPerTile + 4;
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
        recordedObjectGeneration_ = recordedObjectKey_;
        if (!recordColdTile(cb, in)) return false;
        gpuScope.end();
    }
    if ((!recordedMask_ || warm) && !recordObjectMap(cb, in, tileCount * kQueriesPerTile + 2)) return false;
    recordedObjectGeneration_ = recordedObjectKey_;
    cpuMs_ = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
    warmReadMs_ = warmReadMs;
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
        mc2::fmt::print_err("[SHADOW_BAKE] object map re-recorded: generation={} casters={}\n", mc2::fmt::as_u64(recordedObjectKey_), mc2::fmt::as_size(leaves_.size()));
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
    std::vector<uint32_t> picked;
    for (size_t c = 0; c < cv.boxes->size(); ++c)
        if (mc2shadow::casterInWindow((*cv.boxes)[c], tw.window)) { picked.push_back((*cv.live)[c]); tw.casterTop = (std::max)(tw.casterTop, (*cv.tops)[c]); }
    const auto& leaves = *cv.leaves;
    std::sort(picked.begin(), picked.end(), [&leaves](uint32_t a, uint32_t b) { return leaves[a].typeID < leaves[b].typeID; });
    std::vector<uint32_t> types;
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
    auto stamp = [&](int k) { if (queryBase >= 0) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries_.get(), static_cast<uint32_t>(queryBase + k)); };
    const uint32_t groups = (static_cast<uint32_t>(job.regionTexels) + 7) / 8;
    const VkDeviceSize zero = 0;
    stamp(0);
    // The previous region's reads of the scratch images and its staging copy finish before this region writes them.
    cmdBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    VkClearValue clear{};
    clear.depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = windowPass_.get(); rbi.framebuffer = windowFb_.get();
    rbi.renderArea = {{0, 0}, {static_cast<uint32_t>(mc2shadow::shadowQuality().windowSide), static_cast<uint32_t>(mc2shadow::shadowQuality().windowSide)}};
    rbi.clearValueCount = 1; rbi.pClearValues = &clear;
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    if (!runs.empty()) {
        const float side = static_cast<float>(mc2shadow::shadowQuality().windowSide);
        const VkViewport vp{0.0f, 0.0f, side, side, 0.0f, 1.0f};
        const VkRect2D sc{{0, 0}, {static_cast<uint32_t>(mc2shadow::shadowQuality().windowSide), static_cast<uint32_t>(mc2shadow::shadowQuality().windowSide)}};
        vkCmdSetViewport(cb, 0, 1, &vp);
        vkCmdSetScissor(cb, 0, 1, &sc);
        vkCmdSetDepthBias(cb, 0.0f, 0.0f, 0.0f);
        mc2vk::infra::bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, windowOpaque_.get());
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, casterLayout_.get(), 0, 1, &windowSet, 1, &casterOff);
        vkCmdBindVertexBuffers(cb, 0, 1, &in.vbo, &zero);
        vkCmdBindIndexBuffer(cb, in.ibo, 0, VK_INDEX_TYPE_UINT32);
        bool alphaBound = false;
        drawRuns(cb, in, runs, alphaBound, windowOpaque_.get(), windowAlpha_.get());
    }
    vkCmdEndRenderPass(cb);
    stamp(1);
    const uint32_t offs0[2] = {paramOff, passBase};
    pcss_.bind(cb, computeSet, offs0);
    vkCmdDispatch(cb, groups, groups, 1);
    stamp(2);
    march_.bind(cb, computeSet, offs0);
    vkCmdDispatch(cb, groups, groups, 1);
    stamp(3);
    for (uint32_t mode = 0; mode < 3; ++mode) {
        cmdBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        const uint32_t offs[2] = {paramOff, passBase + mode * static_cast<uint32_t>(kSlice)};
        resolve_.bind(cb, computeSet, offs);
        if (mode < 2) vkCmdDispatch(cb, groups, groups, 1);
        else vkCmdDispatch(cb, job.tile / 4 / 8, job.tile / 8, 1);
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
            applyLiveEvent(ev.before, ev.after);
            for (const auto* side : {&ev.before, &ev.after}) {
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
            eventsSinceDrain_ = true;
        }
    }
    if (dirty_.empty()) {
        if (incActive_ && epoch >= lastIncEpoch_ + 2) {
            incActive_ = false;
            mc2::fmt::print_err("[SHADOW_BAKE] dirty queue drained: sub_tiles={} batches={} batch_gpu_ms mean={:.3f} max={:.3f} (budget {} sub-tiles a frame)\n",
                                incSubTiles_, incBatches_, incTimed_ ? incSumGpuMs_ / incTimed_ : 0.0, incMaxGpuMs_, kSubTilesPerFrame);
            incSubTiles_ = incBatches_ = incTimed_ = 0;
            incMaxGpuMs_ = incSumGpuMs_ = 0.0;
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
    if (heightsMoved_ || heights_.empty()) {
        ZoneScopedN("ShadowBake.Dirty.ReadHeights");
        if (!readHeightGrid()) return false;
        heightsMoved_ = false;
        ++heightsGen_;
    }
    if (liveStale_) { ZoneScopedN("ShadowBake.Dirty.RebuildLive"); rebuildLive(); }
    if (!ensureTransients() || !ensureIncremental()) return false;
    const uint32_t par = static_cast<uint32_t>(epoch & 1u);
    // This parity's last batch retired when the island waited its fence: its timestamps are final and its buffers and sets free to rewrite.
    if (incQueryUsed_[par]) {
        uint64_t ts[2] = {0, 0};
        if (vkGetQueryPoolResults(device_, incQueries_.get(), par * 2, 2, sizeof(ts), ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
        {
            const double ms = static_cast<double>(ts[1] - ts[0]) * timestampPeriodNs_ * 1e-6;
            incMaxGpuMs_ = (std::max)(incMaxGpuMs_, ms);
            incSumGpuMs_ += ms;
            ++incTimed_;
        }
        incQueryUsed_[par] = false;
    }
    mc2shadow::DirtyBatch pendingBatch(dirty_, kSubTilesPerFrame);
    const auto& batch = pendingBatch.tiles;
    const float tanT = std::tan(mc2shadow::kSunAngularRadiusDeg * 0.01745329252f);
    std::vector<InstRun> runs;
    std::vector<GpuStaticPropInstance> inst;
    std::vector<TileWork> works(batch.size());
    std::vector<uint8_t> tp(kSubTilesPerFrame * kSlice, 0), ubo(kSubTilesPerFrame * kSlice, 0);
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
        if (!makeHostBuffer(incHeights_[par], allocator_, device_, heights_.size() * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "shadow.bake.dirtyHeights") ||
            !incHeights_[par].upload(heights_.data(), heights_.size() * sizeof(float))) return false;
        incHeightsGen_[par] = heightsGen_;
    }
    writeIncrementalSets(par);
    pendingBatch.commit();

    MC2_VK_GPU_SCOPE(gpuScope, cb, "VKIsl.Shadow.BakeDirty");
    vkCmdResetQueryPool(cb, incQueries_.get(), par * 2, 2);
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
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, incQueries_.get(), par * 2);
    ZoneNamedN(zDirtyRec, "ShadowBake.Dirty.Record", true);
    for (size_t i = 0; i < batch.size(); ++i) {
        RegionJob job{mc2shadow::kSubTileTexels, mc2shadow::kSubTileTexels + 2 * mc2shadow::kTileApron, batch[i].x * mc2shadow::kSubTileTexels,
                      batch[i].y * mc2shadow::kSubTileTexels};
        const uint32_t off = static_cast<uint32_t>(parBase + i * kSlice);
        recordRegion(cb, in, job, works[i], std::span<const InstRun>(runs.data() + works[i].firstRun, works[i].runCount), incComputeSet_[par],
                     incWindowSet_[par], off, off, 0, kSliceSubTile, -1);
    }
    recordTopLevels(cb);
    recordMaskToRead(cb, false);
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, incQueries_.get(), par * 2 + 1);
    gpuScope.end();
    incQueryUsed_[par] = true;
    incActive_ = true;
    lastIncEpoch_ = epoch;
    incRecordedEpoch_ = epoch;
    incSubTiles_ += static_cast<uint32_t>(batch.size());
    ++incBatches_;
    return true;
}

// The incremental path's own buffers and descriptor sets: per-parity slices of the region parameters, caster matrices and casters, and a set per parity
// (rewritten only on its own parity's frame, after the island waited that parity's fence).
bool ShadowBake::ensureIncremental() {
    if (incParams_) return true;
    incInstanceCap_ = 65536;
    if (!makeHostBuffer(incParams_, allocator_, device_, 2 * kSubTilesPerFrame * kSlice, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, "shadow.bake.dirtyParams") ||
        !makeHostBuffer(incCasterUbo_, allocator_, device_, 2 * kSubTilesPerFrame * kSlice, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, "shadow.bake.dirtyCasterUbo") ||
        !makeHostBuffer(incPass_, allocator_, device_, 3 * kSlice, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, "shadow.bake.dirtyPass") ||
        !makeHostBuffer(incInstances_, allocator_, device_, 2 * incInstanceCap_ * sizeof(GpuStaticPropInstance), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "shadow.bake.dirtyInstances")) {
        blog("dirty-update buffers failed.");
        return false;
    }
    std::vector<uint8_t> pp3(3 * kSlice, 0);
    for (int32_t m = 0; m < 3; ++m) {
        const int32_t pass[4]={m,mc2shadow::shadowQuality().level,0,0};
        std::memcpy(pp3.data()+m*kSlice,pass,sizeof(pass));
    }
    if (!incPass_.upload(pp3.data(), pp3.size())) return false;
    if (!incQueries_) {
        VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qci.queryCount = 4;
        if (!create(incQueries_, device_, [&](auto* h) { return vkCreateQueryPool(device_, &qci, nullptr, h); })) { blog("dirty query pool failed."); return false; }
    }
    incHeightsGen_[0] = incHeightsGen_[1] = 0;
    incQueryUsed_[0] = incQueryUsed_[1] = false;
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
    for (int i = 0; i < 12; ++i) { w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = incComputeSet_[par]; w[i].descriptorCount = 1; }
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
    if (!diagPending_ || epoch < diagEpoch_ + 2 || (epoch & 1u) != (diagEpoch_ & 1u)) return;
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
    incHeights_[0].reset(); incHeights_[1].reset();
    incHeightsGen_[0] = incHeightsGen_[1] = 0;
    incQueryUsed_[0] = incQueryUsed_[1] = false;
}

// The live casters (the bake's caster set as the registry has it now) with their light boxes: built once, then edited by each event.
void ShadowBake::rebuildLive() {
    liveStale_ = false;
    std::vector<GpuStaticPropInstance> all;
    mc2shadow::staticCasterSet().resolve(all);
    liveLeaves_.clear(); liveBoxes_.clear(); liveTops_.clear(); liveIdx_.clear();
    for (const GpuStaticPropInstance& leaf : all) appendLive(leaf);
}

void ShadowBake::appendLive(const GpuStaticPropInstance& leaf) {
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
    for (const GpuStaticPropInstance& leaf : before) {
        if (leaf.typeID >= typeBounds_.size() || !typeBounds_[leaf.typeID].valid) continue;
        bool found = false;
        for (size_t j = 0; j < liveLeaves_.size(); ++j) {
            if (liveLeaves_[j].typeID != leaf.typeID || std::memcmp(liveLeaves_[j].modelMatrix, leaf.modelMatrix, sizeof(leaf.modelMatrix)) != 0) continue;
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
    static const bool s_on = mc2::gate::get<"MC2_SHADOW_DIAG">();
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
            const int d = std::abs(int(pa[static_cast<size_t>(k)]) - int(pb[static_cast<size_t>(k)]));
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
                img[o3 + 2] = static_cast<uint8_t>((std::min)(255, std::abs(int(pa[0]) - int(pb[0])) * 4));
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
    if (terrainIndexCount_ == 0 || objectHeightVersion_ != store->heightVersion()) {
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
        objectHeightVersion_ = store->heightVersion();
    }
    if (queryCount_) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries_.get(), queryBase);
    MC2_VK_GPU_SCOPE(gpuScope, cb, "VKIsl.Shadow.BakeObjectMap");
    VkClearValue clear{};
    clear.depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = objectPass_.get(); rbi.framebuffer = objectFb_.get();
    rbi.renderArea = {{0, 0}, {static_cast<uint32_t>(objectMapSize()), static_cast<uint32_t>(objectMapSize())}};
    rbi.clearValueCount = 1; rbi.pClearValues = &clear;
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport vp{0.0f, 0.0f, static_cast<float>(objectMapSize()), static_cast<float>(objectMapSize()), 0.0f, 1.0f};
    const VkRect2D sc{{0, 0}, {static_cast<uint32_t>(objectMapSize()), static_cast<uint32_t>(objectMapSize())}};
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
    vkCmdDrawIndexed(cb, terrainIndexCount_, 1, 0, 0, 0);
    if (!objectRuns_.empty()) {
        vkCmdSetDepthBias(cb, 2.0f, 0.0f, 1.0f);
        mc2vk::infra::bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, objectOpaque_.get());
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
