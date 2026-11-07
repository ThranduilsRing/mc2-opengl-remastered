// VK water island: the HDRI environment and shore height / mask texture imports.
// Declared in the class headers; shared names in vulkan_water_island_internal.h.
#include "vulkan_water_island_internal.h"
#include "gameos.hpp"



namespace vk_water_detail {

bool init_reflection(WaterIsland* s) {
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image.imageType=VK_IMAGE_TYPE_2D; image.format=VK_FORMAT_R16G16B16A16_SFLOAT;
    image.extent={static_cast<uint32_t>((s->w+1)/2),static_cast<uint32_t>((s->h+1)/2),1};
    image.mipLevels=1; image.arrayLayers=1; image.samples=VK_SAMPLE_COUNT_1_BIT;
    image.tiling=VK_IMAGE_TILING_OPTIMAL;
    image.usage=VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VmaAllocationCreateInfo allocation{}; allocation.usage=VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (!s->reflectionImage.create(s->allocator,s->device,image,allocation,"water.reflection.half")) return false;
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image=s->reflectionImage.get(); view.viewType=VK_IMAGE_VIEW_TYPE_2D; view.format=image.format;
    view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    if (!mc2vk::create(s->reflectionView,s->device,[&](VkImageView* out){return vkCreateImageView(s->device,&view,nullptr,out);})) return false;
    view.image=s->sDepthRes->image.get(); view.format=s->sDepthRes->format;
    view.subresourceRange.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;
    if (!mc2vk::create(s->reflectionDepthView,s->device,[&](VkImageView* out){return vkCreateImageView(s->device,&view,nullptr,out);})) return false;
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter=sampler.minFilter=VK_FILTER_NEAREST;
    sampler.addressModeU=sampler.addressModeV=sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (!mc2vk::create(s->reflectionSampler,s->device,[&](VkSampler* out){return vkCreateSampler(s->device,&sampler,nullptr,out);})) return false;
    const auto layout=s->dsl.get();
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount=1; pl.pSetLayouts=&layout;
    if (!mc2vk::create(s->reflectionLayout,s->device,[&](VkPipelineLayout* out){return vkCreatePipelineLayout(s->device,&pl,nullptr,out);})) return false;
    s->reflectionShader=mc2vk::OwnedShaderModule(s->device,load_spv(s->device,spv_dir()+"water_reflection.comp.spv"));
    if (!s->reflectionShader) return false;
    VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT; pipeline.stage.module=s->reflectionShader.get(); pipeline.stage.pName="main";
    pipeline.layout=s->reflectionLayout.get();
    return mc2vk::create(s->reflectionPipe,s->device,[&](VkPipeline* out){return mc2vk::infra::compute(s->device,VK_NULL_HANDLE,1,&pipeline,nullptr,out);});
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

}  // namespace vk_water_detail

