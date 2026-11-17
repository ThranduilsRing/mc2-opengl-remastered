// VK water island: the HDRI environment and shore height / mask texture imports.
// Declared in the class headers; shared names in vulkan_water_island_internal.h.
#include "vulkan_water_island_internal.h"
#include "gameos.hpp"
#include "vulkan_terrain_island.h"
#include "vk_one_shot.h"
#include "vk_barrier_batch_vk.h"



namespace vk_water_detail {

namespace {
struct WavePackets : mc2vk::IslandState {
    float span = 0, halfMap = 0;
    uint32_t size = 1024;
    bool bound[mc2vk::kMaxFramesInFlight]{};
    mc2vk::OwnedImage image;
    mc2vk::OwnedImageView view;
    mc2vk::OwnedSampler sampler;
};

bool bakePackets(WaterIsland* s, WavePackets& b) {
    // At least sixteen intervals per shortest (93 wu) noise cell, including edges.
    while (b.size - 1 < b.span * 16.0f / 93.0f) b.size *= 2;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent = {b.size, b.size, 1}; ci.mipLevels = 1; ci.arrayLayers = 3;
    ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VmaAllocationCreateInfo ai{}; ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (!b.image.create(s->allocator, s->device, ci, ai, "water.wavePackets")) return false;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = b.image.get(); vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY; vi.format = ci.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 3};
    if (!mc2vk::create(b.view, s->device, [&](auto* h) { return vkCreateImageView(s->device, &vi, nullptr, h); })) return false;
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (!mc2vk::create(b.sampler, s->device, [&](auto* h) { return vkCreateSampler(s->device, &si, nullptr, h); })) return false;
    mc2vk::OwnedDescriptorSetLayout dsl;
    mc2vk::OwnedDescriptorPool pool;
    mc2vk::OwnedPipelineLayout layout;
    mc2vk::OwnedPipeline pipe;
    const VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo di{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    di.bindingCount = 2; di.pBindings = bindings;
    if (!mc2vk::create(dsl, s->device, [&](auto* h) { return vkCreateDescriptorSetLayout(s->device, &di, nullptr, h); })) return false;
    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 1; pi.poolSizeCount = 2; pi.pPoolSizes = sizes;
    if (!mc2vk::create(pool, s->device, [&](auto* h) { return vkCreateDescriptorPool(s->device, &pi, nullptr, h); })) return false;
    const VkDescriptorSetLayout raw = dsl.get();
    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = pool.get(); alloc.descriptorSetCount = 1; alloc.pSetLayouts = &raw;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (MC2_VK_ALLOC_SETS(s->device, &alloc, &set) != VK_SUCCESS) return false;
    struct Params { float span, halfMap; uint32_t size, pad; };
    const Params params{b.span, b.halfMap, b.size, 0};
    mc2vk::OwnedBuffer paramsBuffer;
    if (!paramsBuffer.createFilled(s->allocator, s->device, &params, sizeof(params), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, "water.wavePackets.params")) return false;
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 1; li.pSetLayouts = &raw;
    if (!mc2vk::create(layout, s->device, [&](auto* h) { return vkCreatePipelineLayout(s->device, &li, nullptr, h); })) return false;
    mc2vk::OwnedShaderModule shader(s->device, load_spv(s->device, spv_dir() + "water_wave_packets.comp.spv"));
    if (!shader) return false;
    VkComputePipelineCreateInfo pc{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pc.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, shader.get(), "main", nullptr};
    pc.layout = layout.get();
    if (!mc2vk::create(pipe, s->device, [&](auto* h) { return mc2vk::infra::compute(s->device, VK_NULL_HANDLE, 1, &pc, nullptr, h); })) return false;
    const VkDescriptorImageInfo image{VK_NULL_HANDLE, b.view.get(), VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set; write.dstBinding = 0; write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; write.pImageInfo = &image;
    vkUpdateDescriptorSets(s->device, 1, &write, 0, nullptr);
    const VkDescriptorBufferInfo buffer{paramsBuffer.get(), 0, sizeof(params)};
    write.dstBinding = 1; write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.pImageInfo = nullptr; write.pBufferInfo = &buffer;
    vkUpdateDescriptorSets(s->device, 1, &write, 0, nullptr);
    auto& ctx = mc2vk::getOrCreateEngineVulkanContext();
    if (!mc2vk::submitOneShot(ctx, [&](VkCommandBuffer cb) {
        mc2vk::sync::cmdImageBarrier(ctx, cb, b.image.get(), vi.subresourceRange,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_NONE, 0,
            VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        mc2vk::infra::bind(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe.get());
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout.get(), 0, 1, &set, 0, nullptr);
        vkCmdDispatch(cb, (b.size + 7) / 8, (b.size + 7) / 8, 3);
        mc2vk::sync::cmdImageBarrier(ctx, cb, b.image.get(), vi.subresourceRange,
            VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }, "water wave packet bake")) return false;
    vlog("WAVE_PACKETS baked size=%u layers=3 format=RGBA8 span=%.1f halfMap=%.1f MiB=%.2f",
        b.size, b.span, b.halfMap, static_cast<double>(b.size) * b.size * 12 / (1024 * 1024));
    return true;
}
} // namespace

bool prepare_wave_packets(WaterIsland* s, uint32_t par) {
    Mc2TerrainTier1Feed feed{};
    if (!mc2_terrain_tier1_get_load_feed(&feed) || feed.mapSide < 2) return false;
    const float span = 128.0f * (feed.mapSide - 1);
    auto& owner = s->wavePackets;
    if (owner) {
        const auto& b = *static_cast<WavePackets*>(owner.get());
        // No time, weather, camera or frame input: mission dimensions alone define the bake.
        if (b.span != span || b.halfMap != feed.halfMap)
            mc2vk::getOrCreateEngineVulkanContext().retire(std::move(owner));
    }
    if (!owner) {
        auto b = std::make_unique<WavePackets>();
        b->span = span; b->halfMap = feed.halfMap;
        if (!bakePackets(s, *b)) return false;
        owner = std::move(b);
    }
    auto& b = *static_cast<WavePackets*>(owner.get());
    if (!b.bound[par]) {
        const VkDescriptorImageInfo image{b.sampler.get(), b.view.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = s->set[par]; write.dstBinding = 17; write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; write.pImageInfo = &image;
        vkUpdateDescriptorSets(s->device, 1, &write, 0, nullptr);
        b.bound[par] = true;
    }
    return true;
}

bool init_reflection(WaterIsland* s) {
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image.imageType=VK_IMAGE_TYPE_2D; image.format=VK_FORMAT_R16G16B16A16_SFLOAT;
    image.extent={1,1,1}; // descriptor stand-in; the bake owns reflection attachments
    image.mipLevels=1; image.arrayLayers=1; image.samples=VK_SAMPLE_COUNT_1_BIT;
    image.tiling=VK_IMAGE_TILING_OPTIMAL;
    image.usage=VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VmaAllocationCreateInfo allocation{}; allocation.usage=VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.viewType=VK_IMAGE_VIEW_TYPE_2D;
    view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    if (!s->reflectionImage) {
        if (!s->reflectionImage.create(s->allocator,s->device,image,allocation,"water.reflection.full")) return false;
        view.image=s->reflectionImage.get(); view.format=image.format;
        if (!mc2vk::create(s->reflectionView,s->device,[&](VkImageView* out){return vkCreateImageView(s->device,&view,nullptr,out);})) return false;
    }
    return true;
}

// COPY-ONCE HDRI equirect import (not per frame) from the engine's CPU copy, in gos_hdri.cpp's two forms:
//   - BC6H .ktx2 sidecar (field default, MC2_HDRI_BC6H on): mip 0 only. GL's textureLod at LOD 4 clamps to mip 0 on
//     that texture (GL_TEXTURE_MAX_LEVEL=0), so a 1-mip VK image is exact parity.
//   - RGBA16F tinyexr fallback: the full mip chain, built on the CPU with glGenerateMipmap's box filter.
// Cost: one ~24 MB (BC6H 4K mip 0) or ~85 MB (RGBA16F 4K + chain) staging copy and one upload submit, once per load.
// Fail-soft: bind a 1x1 black dummy and patch the FS UBO's u_waterHdriLod to -1 (SH-L2 fallback), as GL does when
// !isHdriReady().
bool ensure_hdri(WaterIsland* s, uint64_t hdriGeneration) {
    // Keyed on the CPU copy's load generation: a reloaded HDRI can come back under the same GL name.
    const uint64_t generation = hdriGeneration;
    if (s->hdriAttempted && s->hdriGeneration == generation) {
        s->hdriRealImported=s->hdriSourceReal && mc2vk::getOrCreateEngineVulkanContext().textures().ready(s->hdriTexture.get());
        return static_cast<bool>(s->hdriTexture);
    }
    s->hdriAttempted = true;
    s->hdriGeneration = generation;
    s->hdriRealImported = false;
    // Re-import (tex id changed, e.g. dummy -> real after late HDRI load):
    // drop the old image first. Cheap + rare; idle the device so no pending
    // command buffer still references the outgoing view.


    if (!s->hdriSampler) {
        VkSamplerCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.maxLod = VK_LOD_CLAMP_NONE;
        mc2vk::infra::materialSampler(sci);
        s->sampBias = sci.mipLodBias;
        if (!mc2vk::create(s->hdriSampler, s->device, [&](VkSampler* h) { return vkCreateSampler(s->device, &sci, nullptr, h); })) {
            vlog("hdri: vkCreateSampler failed."); return false;
        }
    }

    VkFormat fmt = VK_FORMAT_R16G16B16A16_SFLOAT;
    int w = 1, h = 1;
    uint32_t mips = 1;
    std::vector<std::vector<unsigned char>> levels;   // per-mip raw bytes
    std::vector<std::pair<int,int>> levelDims;
    bool haveReal = false;
    if (generation != 0) {
        uint64_t gotGeneration = 0;
        const char* reason = "";
        haveReal = mc2vk::hdriFromCpu(s->phys, fmt, levels, levelDims, gotGeneration, reason);
        if (haveReal) { std::tie(w, h) = levelDims[0]; mips = static_cast<uint32_t>(levels.size()); }
        else vlog("hdri: not imported (%s) -- SH-L2 fallback.", reason);
    }
    if (!haveReal) {
        // 1x1 black RGBA16F dummy (keeps the statically-used binding valid;
        // shader never reads it because the island patches u_waterHdriLod to -1).
        fmt = VK_FORMAT_R16G16B16A16_SFLOAT;
        w = 1; h = 1; mips = 1;
        levels.assign(1, std::vector<unsigned char>(8, 0));
        levelDims.assign(1, {1, 1});
    }

    RenderCore::KtxImage image; image.width=w; image.height=h; image.vkFormat=static_cast<uint32_t>(fmt); image.mipCount=static_cast<int>(mips);
    image.isCompressed=fmt==VK_FORMAT_BC6H_UFLOAT_BLOCK || fmt==VK_FORMAT_BC6H_SFLOAT_BLOCK;
    image.blockSizeBytes=image.isCompressed ? 16u : 0u;
    for (auto& level : levels) { image.mipByteOffsets.push_back(image.pixels.size()); image.pixels.insert(image.pixels.end(),level.begin(),level.end()); }
    RenderCore::TextureKey key; key.source=RenderCore::makeTextureSourceId("hdri-cpu","water",RenderCore::textureContentHash(image.pixels.data(),image.pixels.size()));
    key.format=static_cast<uint32_t>(fmt); key.hints=gosHint_MipmapFilter0;
    auto texture=mc2vk::getOrCreateEngineVulkanContext().textures().acquire(key,image,{static_cast<uint32_t>(w),static_cast<uint32_t>(h),static_cast<uint32_t>(w),static_cast<uint32_t>(h)});
    if (!texture) return false;
    s->hdriTexture=std::move(texture); s->hdriSourceReal=haveReal;
    s->hdriRealImported=false;
    // Real art is published after the service upload ticket; descriptors follow the retired slot.
    write_descriptors(s);   // binding 2 now valid
    vlog("hdri %s import: %dx%d fmt=%d mips=%u source_version=%llu (copy-once, zero per-frame cost).",
         haveReal ? "REAL" : "DUMMY", w, h, static_cast<int>(fmt), mips, static_cast<unsigned long long>(hdriGeneration));
    return true;
}

// VK-PARITY-WATER-SHORE-1: copy-once import of one shore sidecar (R32F height or RGBA8 mask, 4 bytes a texel) from the
// terrain field store's CPU copy into a sampled VK image. With no texels (no sidecar, or height disabled) a 1x1 zero dummy is
// created instead and *real is false. The service owns copy completion and retirement.
bool import_shore_tex(WaterIsland* s, const void* texels, int texelsW, int texelsH, bool isFloat,
                      mc2vk::ResidentTextureRef& texture, bool* real) {
    const bool have=texels && texelsW>0 && texelsH>0;
    static const uint32_t zero=0;
    const auto w=have ? static_cast<uint32_t>(texelsW) : 1u,h=have ? static_cast<uint32_t>(texelsH) : 1u;
    auto ref=mc2vk::getOrCreateEngineVulkanContext().textures().acquirePixels(w,h,isFloat ? VK_FORMAT_R32_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM,
        have ? texels : &zero,static_cast<size_t>(w)*h*4,false);
    if (!ref) return false;
    texture=std::move(ref); *real=have; return true;
}

// The field store is the version authority for both terrain and water. acquirePixels
// gives both consumers references to the same content-addressed mask allocation.
// Requires hdriSampler (created by ensure_hdri) -- shared linear/clamp sampler.
bool ensure_shore(WaterIsland* s) {
    const auto* store = gos_TerrainFieldStore();
    const uint64_t heightVersion = store ? store->waterShoreHeight().version : 0;
    const uint64_t maskVersion = store ? store->shorelineMask().version : 0;
    if (s->shoreAttempted && s->shoreHeightVersion == heightVersion && s->shoreMaskVersion == maskVersion) {
        auto& textures=mc2vk::getOrCreateEngineVulkanContext().textures();
        s->shHReal=s->shHSourceReal && textures.ready(s->shHTexture.get());
        s->shMReal=s->shMSourceReal && textures.ready(s->shMTexture.get()); return true;
    }
    // R32_SFLOAT linear filtering is an optional format feature; without it the
    // height sidecar is treated as absent (FS degrades 2 -> 1, same as a missing
    // sidecar on GL).
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(s->phys, VK_FORMAT_R32_SFLOAT, &fp);
    const bool r32Linear = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
    if (!r32Linear) vlog("shore: R32_SFLOAT lacks linear filtering -- height shore disabled (mode-1 fallback).");
    const mc2terrain::FieldImage<float>* height = store && r32Linear ? &store->waterShoreHeight() : nullptr;
    const mc2terrain::FieldImage<uint8_t>* mask = store ? &store->shorelineMask() : nullptr;
    s->shHSdf = store != nullptr && store->waterShoreHeightKind() == mc2terrain::WaterShoreHeightKind::ShoreSdf;
    if (!import_shore_tex(s, height && !height->texels.empty() ? height->texels.data() : nullptr, height ? height->width : 0,
                          height ? height->height : 0, true, s->shHTexture, &s->shHSourceReal))
        return false;
    if (!import_shore_tex(s, mask && !mask->texels.empty() ? mask->texels.data() : nullptr, mask ? mask->width : 0,
                          mask ? mask->height : 0, false, s->shMTexture, &s->shMSourceReal))
        return false;
    s->shHReal=s->shMReal=false;
    s->shoreAttempted = true;
    s->shoreHeightVersion = heightVersion;
    s->shoreMaskVersion = maskVersion;
    write_descriptors(s);
    vlog("shore import: height=%s mask=%s versions=%llu/%llu mask_handle=%u.",
         s->shHSourceReal ? "REAL" : "DUMMY", s->shMSourceReal ? "REAL" : "DUMMY",
         static_cast<unsigned long long>(heightVersion), static_cast<unsigned long long>(maskVersion), s->shMTexture.get().raw());
    return true;
}

// FSR/AA resolve state changed: rebuild the sampler (the old one retires with the in-flight frames) and mark both parities' sets for one rewrite.
void refresh_hdri_sampler(WaterIsland* s) {
    if (!mc2vk::infra::materialBiasStale(s->sampBias)) return;
    if (s->hdriSampler) {
        VkSamplerCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.maxLod = VK_LOD_CLAMP_NONE;
        mc2vk::infra::materialSampler(sci);
        mc2vk::OwnedSampler fresh;
        if (!mc2vk::create(fresh, s->device, [&](VkSampler* h) { return vkCreateSampler(s->device, &sci, nullptr, h); })) return;
        mc2vk::getOrCreateEngineVulkanContext().retire(std::move(s->hdriSampler));
        s->hdriSampler = std::move(fresh);
        s->sampBias = sci.mipLodBias;
    } else {
        VkSamplerCreateInfo probe{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        mc2vk::infra::materialSampler(probe);
        s->sampBias = probe.mipLodBias;
    }
    mc2vk::bumpDescEpoch();
    write_descriptors(s);
}

}  // namespace vk_water_detail

