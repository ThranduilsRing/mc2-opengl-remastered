// VK water island: the HDRI environment and shore height / mask texture imports.
// Declared in the class headers; shared names in vulkan_water_island_internal.h.
#include "vulkan_water_island_internal.h"

#ifdef MC2_VULKAN_ISLAND


namespace vk_water_detail {

// COPY-ONCE HDRI equirect import (not per frame) from the engine's CPU copy, in gos_hdri.cpp's two forms:
//   - BC6H .ktx2 sidecar (field default, MC2_HDRI_BC6H on): mip 0 only. GL's textureLod at LOD 4 clamps to mip 0 on
//     that texture (GL_TEXTURE_MAX_LEVEL=0), so a 1-mip VK image is exact parity.
//   - RGBA16F tinyexr fallback: the full mip chain, built on the CPU with glGenerateMipmap's box filter.
// Cost: one ~24 MB (BC6H 4K mip 0) or ~85 MB (RGBA16F 4K + chain) staging copy and one upload submit, once per load.
// Fail-soft: bind a 1x1 black dummy and patch the FS UBO's u_waterHdriLod to -1 (SH-L2 fallback), as GL does when
// !isHdriReady().
bool ensure_hdri(WaterIsland* s, unsigned int hdriGlTex) {
    // Keyed on the CPU copy's load generation: a reloaded HDRI can come back under the same GL name.
    const uint64_t generation = hdriGlTex != 0 ? mc2vk::hdriCpuGeneration() : 0;
    if (s->hdriAttempted && s->hdriGeneration == generation) return static_cast<bool>(s->hdriView);
    s->hdriAttempted = true;
    s->hdriGeneration = generation;
    s->hdriRealImported = false;
    // Re-import (tex id changed, e.g. dummy -> real after late HDRI load):
    // drop the old image first. Cheap + rare; idle the device so no pending
    // command buffer still references the outgoing view.
    if (s->hdriView || s->hdriImage) {
        mc2vk::getOrCreateEngineVulkanContext().waitIdle("water_hdri_reimport");
        s->hdriView.reset();
        s->hdriImage.reset();
    }

    if (!s->hdriSampler) {
        VkSamplerCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.maxLod = VK_LOD_CLAMP_NONE;
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
        if (haveReal) { w = levelDims[0].first; h = levelDims[0].second; mips = static_cast<uint32_t>(levels.size()); }
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

    // Create image + view.
    {
        VkImageCreateInfo ii{};
        ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ii.imageType = VK_IMAGE_TYPE_2D; ii.format = fmt;
        ii.extent = {(uint32_t)w, (uint32_t)h, 1};
        ii.mipLevels = mips; ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo aci{}; aci.usage = VMA_MEMORY_USAGE_AUTO;
        if (!s->hdriImage.create(s->allocator, s->device, ii, aci, "water.hdri")) {
            vlog("hdri: image creation failed."); return false;
        }
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = s->hdriImage.get(); vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1};
        if (!mc2vk::create(s->hdriView, s->device, [&](VkImageView* h) { return vkCreateImageView(s->device, &vi, nullptr, h); })) {
            vlog("hdri: vkCreateImageView failed."); return false;
        }
    }

    // Staging buffer (one allocation, all mips at 16B-aligned offsets).
    std::vector<VkDeviceSize> offs(levels.size());
    VkDeviceSize total = 0;
    for (size_t i = 0; i < levels.size(); ++i) {
        offs[i] = total;
        total += (VkDeviceSize)((levels[i].size() + 15) & ~size_t(15));
    }
    mc2vk::OwnedBuffer staging;   // released on return; submit_once has waited for the copy by then
    if (!make_buffer(s, staging, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, "water.staging")) return false;
    {
        void* p = staging.map();
        if (!p) return false;
        for (size_t i = 0; i < levels.size(); ++i)
            std::memcpy((unsigned char*)p + offs[i], levels[i].data(), levels[i].size());
        staging.unmap();
    }

    const bool ok = submit_once(s, [&](VkCommandBuffer cb) {
        VkImageMemoryBarrier bar{};
        bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        bar.srcAccessMask = 0; bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.image = s->hdriImage.get();
        bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
        for (uint32_t i = 0; i < mips; ++i) {
            VkBufferImageCopy c{};
            c.bufferOffset = offs[i];
            c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
            c.imageExtent = {(uint32_t)levelDims[i].first, (uint32_t)levelDims[i].second, 1};
            vkCmdCopyBufferToImage(cb, staging.get(), s->hdriImage.get(),
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        }
        bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
    });
    if (!ok) { vlog("hdri: upload submit failed."); return false; }

    s->hdriRealImported = haveReal;
    // VK-WATER-FENCE-DEFER-1: with the deferred retire, a prior water submit may still be in
    // flight — device-idle first (rare: at most twice per island, dummy -> real).
    mc2vk::getOrCreateEngineVulkanContext().waitIdle("water_image_import");
    write_descriptors(s);   // binding 2 now valid
    vlog("hdri %s import: %dx%d fmt=%d mips=%u glTex=%u (copy-once, zero per-frame cost).",
         haveReal ? "REAL" : "DUMMY", w, h, (int)fmt, mips, hdriGlTex);
    return true;
}

// VK-PARITY-WATER-SHORE-1: copy-once import of one shore sidecar (R32F height or RGBA8 mask, 4 bytes a texel) from the
// terrain field store's CPU copy into a sampled VK image. With no texels (no sidecar, or height disabled) a 1x1 zero dummy is
// created instead and *real is false. Caller device-idles beforehand.
bool import_shore_tex(WaterIsland* s, const void* texels, int texelsW, int texelsH, bool isFloat,
                      mc2vk::OwnedImage& img, mc2vk::OwnedImageView& view, bool* real) {
    const VkFormat fmt = isFloat ? VK_FORMAT_R32_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
    static const uint32_t kZeroTexel = 0;
    *real = texels != nullptr && texelsW > 0 && texelsH > 0;
    const int w = *real ? texelsW : 1;
    const int h = *real ? texelsH : 1;
    const void* src = *real ? texels : &kZeroTexel;
    const size_t byteCount = (size_t)w * (size_t)h * 4u;

    view.reset();
    img.reset();
    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D; ii.format = fmt;
    ii.extent = {(uint32_t)w, (uint32_t)h, 1};
    ii.mipLevels = 1; ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo aci{}; aci.usage = VMA_MEMORY_USAGE_AUTO;
    if (!img.create(s->allocator, s->device, ii, aci, "water.shore")) {
        vlog("shore: image creation failed."); return false;
    }
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = img.get(); vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (!mc2vk::create(view, s->device, [&](VkImageView* h) { return vkCreateImageView(s->device, &vi, nullptr, h); })) {
        vlog("shore: vkCreateImageView failed."); return false;
    }
    mc2vk::OwnedBuffer staging;   // released on return; submit_once has waited for the copy by then
    if (!make_buffer(s, staging, (VkDeviceSize)byteCount, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, "water.staging")) return false;
    if (!staging.upload(src, (VkDeviceSize)byteCount)) return false;
    const VkImage dst = img.get();
    const bool ok = submit_once(s, [&](VkCommandBuffer cb) {
        VkImageMemoryBarrier bar{};
        bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        bar.srcAccessMask = 0; bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.image = dst;
        bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
        VkBufferImageCopy c{};
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
        vkCmdCopyBufferToImage(cb, staging.get(), dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
    });
    if (!ok) { vlog("shore: upload submit failed."); return false; }
    return true;
}

// (Re)import both shore textures when the GL-side data generation changes.
// Requires hdriSampler (created by ensure_hdri) -- shared linear/clamp sampler.
bool ensure_shore(WaterIsland* s) {
    const unsigned int gen = gos_TerrainLodChunk_WaterShoreDataGeneration();
    if (s->shoreAttempted && s->shoreGen == gen) return true;
    s->shoreAttempted = true;
    s->shoreGen = gen;
    mc2vk::getOrCreateEngineVulkanContext().waitIdle("water_shore_reimport");
    // R32_SFLOAT linear filtering is an optional format feature; without it the
    // height sidecar is treated as absent (FS degrades 2 -> 1, same as a missing
    // sidecar on GL).
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(s->phys, VK_FORMAT_R32_SFLOAT, &fp);
    const bool r32Linear = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
    if (!r32Linear) vlog("shore: R32_SFLOAT lacks linear filtering -- height shore disabled (mode-1 fallback).");
    const mc2terrain::TerrainFieldStore* store = gos_TerrainFieldStore();
    const mc2terrain::FieldImage<float>* height = store && r32Linear ? &store->waterShoreHeight() : nullptr;
    const mc2terrain::FieldImage<uint8_t>* mask = store ? &store->shorelineMask() : nullptr;
    if (!import_shore_tex(s, height && !height->texels.empty() ? height->texels.data() : nullptr, height ? height->width : 0,
                          height ? height->height : 0, true, s->shHImage, s->shHView, &s->shHReal))
        return false;
    if (!import_shore_tex(s, mask && !mask->texels.empty() ? mask->texels.data() : nullptr, mask ? mask->width : 0,
                          mask ? mask->height : 0, false, s->shMImage, s->shMView, &s->shMReal))
        return false;
    write_descriptors(s);
    vlog("shore import: height=%s mask=%s gen=%u (copy-once, zero per-frame cost).",
         s->shHReal ? "REAL" : "DUMMY", s->shMReal ? "REAL" : "DUMMY", gen);
    return true;
}

}  // namespace vk_water_detail

#endif // MC2_VULKAN_ISLAND
