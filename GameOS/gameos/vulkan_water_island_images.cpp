// VK water island: the HDRI environment and shore height / mask texture imports.
// Declared in the class headers; shared names in vulkan_water_island_internal.h.
#include "vulkan_water_island_internal.h"
#include "gameos.hpp"



namespace vk_water_detail {

bool init_reflection(WaterIsland* s) {
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image.imageType=VK_IMAGE_TYPE_2D; image.format=VK_FORMAT_R16G16B16A16_SFLOAT;
    image.extent={static_cast<uint32_t>(s->w),static_cast<uint32_t>(s->h),1};   // full resolution
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
    // Prewarm can run before water presence is known; dry maps need no depth.
    if (!s->mapHasWater || s->planarDepthImage) return true;
    // Coverage must not alias the reflection sampler bound by the water draw.
    if (!s->coverageImage.create(s->allocator,s->device,image,allocation,"water.reflection.coverage")) return false;
    view.image=s->coverageImage.get(); view.format=image.format;
    if (!mc2vk::create(s->coverageView,s->device,[&](VkImageView* out){return vkCreateImageView(s->device,&view,nullptr,out);})) return false;
    // D32 adds 31.6 MiB at 3840x2160; the existing RGBA16F colour is reused.
    image.format=VK_FORMAT_D32_SFLOAT;
    image.usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!s->planarDepthImage.create(s->allocator,s->device,image,allocation,"water.reflection.depth")) return false;
    view.image=s->planarDepthImage.get(); view.format=image.format;
    view.subresourceRange.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;
    if (!mc2vk::create(s->planarDepthView,s->device,[&](VkImageView* out){return vkCreateImageView(s->device,&view,nullptr,out);})) {
        s->planarDepthImage.reset();
        return false;
    }
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(s->w) * s->h * sizeof(float);
    if (!make_buffer(s, s->maskHorizontal, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "water.mask.horizontal") ||
        !make_buffer(s, s->maskDepth, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, "water.mask.depth")) return false;
    VkDescriptorSetLayoutBinding bindings[3]{};
    for (uint32_t i=0; i<3; ++i) bindings[i]={i, i==0 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dsl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dsl.bindingCount=3; dsl.pBindings=bindings;
    if (!mc2vk::create(s->maskDsl,s->device,[&](auto* h){return vkCreateDescriptorSetLayout(s->device,&dsl,nullptr,h);})) return false;
    const VkDescriptorPoolSize sizes[2]{{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2}};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets=1; pool.poolSizeCount=2; pool.pPoolSizes=sizes;
    if (!mc2vk::create(s->maskPool,s->device,[&](auto* h){return vkCreateDescriptorPool(s->device,&pool,nullptr,h);})) return false;
    const VkDescriptorSetLayout layout=s->maskDsl.get();
    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool=s->maskPool.get(); alloc.descriptorSetCount=1; alloc.pSetLayouts=&layout;
    if (vkAllocateDescriptorSets(s->device,&alloc,&s->maskSet)!=VK_SUCCESS) return false;
    const VkDescriptorImageInfo imageInfo{VK_NULL_HANDLE,s->coverageView.get(),VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorBufferInfo buffers[2]{{s->maskHorizontal.get(),0,bytes},{s->maskDepth.get(),0,bytes}};
    VkWriteDescriptorSet writes[3]{};
    for (uint32_t i=0; i<3; ++i) {
        writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[i].dstSet=s->maskSet; writes[i].dstBinding=i;
        writes[i].descriptorCount=1; writes[i].descriptorType=bindings[i].descriptorType;
        if (i==0) writes[i].pImageInfo=&imageInfo; else writes[i].pBufferInfo=&buffers[i-1];
    }
    vkUpdateDescriptorSets(s->device,3,writes,0,nullptr);
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(int32_t)};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount=1; pl.pSetLayouts=&layout; pl.pushConstantRangeCount=1; pl.pPushConstantRanges=&push;
    if (!mc2vk::create(s->maskLayout,s->device,[&](auto* h){return vkCreatePipelineLayout(s->device,&pl,nullptr,h);})) return false;
    mc2vk::OwnedShaderModule shader(s->device,load_spv(s->device,spv_dir()+"water_reflection_mask.comp.spv"));
    if (!shader) return false;
    VkComputePipelineCreateInfo compute{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    compute.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,shader.get(),"main",nullptr};
    compute.layout=s->maskLayout.get();
    if (!mc2vk::create(s->maskCompute,s->device,[&](auto* h){return mc2vk::infra::compute(s->device,VK_NULL_HANDLE,1,&compute,nullptr,h);})) return false;
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

