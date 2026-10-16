// VK terrain island: height snapshot and audit, the colormap / cement / material-normal imports and the shadow image copies.
// Declared in the class headers; shared names in vulkan_terrain_island_internal.h.
#include "vulkan_terrain_island_internal.h"
#include "texture_decode.h"

#ifdef MC2_VULKAN_ISLAND


namespace vk_terrain_detail {

// Copy the store's height field into the VK-owned buffer when the feed version changes
// (map load and every height patch), never per frame.
bool snapshot_height(TerrainIsland* s, const Mc2TerrainTier1Feed& feed) {
    if (feed.heightVersion == s->heightVersionCopied) return true; // unchanged
    if (!feed.heightCpu || feed.heightBytes == 0) return false;
    if (!ensure_height_capacity(s, (VkDeviceSize)feed.heightBytes)) return false;
    // VK-TERRAIN-FENCE-DEFER-1: the height SSBO stays single-instance (version-gated
    // rare rewrite: mission load / crater deform). Under the deferred retire a prior
    // terrain submit may still be reading it — device-idle before rewriting in place.
    // (The old inline post-submit fence wait used to provide this guarantee for free.)
    mc2vk::getOrCreateEngineVulkanContext().waitIdle("terrain_height_snapshot");
    void* p = nullptr;
    if (vmaMapMemory(s->allocator, s->heightBuf.allocation(), &p) != VK_SUCCESS) return false;
    std::memcpy(p, feed.heightCpu, (size_t)feed.heightBytes);
    vmaUnmapMemory(s->allocator, s->heightBuf.allocation());
    s->heightVersionCopied = feed.heightVersion;
    ++g_health.heightSnapshots;
    return true;
}

// ---------------------------------------------------------------------------
// COLORMAP-BASE sub-slice: VK-owned copy-once import of the GL colormap atlas.
// Pattern: read the GL texel
// bytes back on the GL side (RGBA8 -> glGetTexImage(GL_RGBA); BC7 ->
// glGetCompressedTexImage), stage to a VK buffer, vkCmdCopyBufferToImage into a
// sampled VK image, LINEAR/CLAMP_TO_EDGE sampler (mip0). Match VK format to the GL
// upload's channel order / block encoding; do NOT double-encode sRGB.
// ---------------------------------------------------------------------------

// One-shot command buffer on the island's borrowed queue (mirror the spike helper).
VkCommandBuffer begin_oneshot(TerrainIsland* s) {
    VkCommandBufferAllocateInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool = s->cpool.get(); cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbi.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(s->device, &cbi, &cb) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    return cb;
}
void end_oneshot(TerrainIsland* s, VkCommandBuffer cb) {
    vkEndCommandBuffer(cb);
    VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    // VK-QWI-TO-FENCE-1: transient per-submit fence instead of a queue-wide drain
    // (copy-once helper, not per-frame; fail-soft to QWI if fence creation fails).
    mc2vk::OwnedFence fence;
    VkFenceCreateInfo fci{}; fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    mc2vk::create(fence, s->device, [&](auto* h) { return vkCreateFence(s->device, &fci, nullptr, h); });
    const VkFence f = fence.get();
    vkQueueSubmit(s->queue, 1, &si, f);
    if (f != VK_NULL_HANDLE) {
        vkWaitForFences(s->device, 1, &f, VK_TRUE, 5ull * 1000 * 1000 * 1000);
    } else {
        vkQueueWaitIdle(s->queue);
    }
    vkFreeCommandBuffers(s->device, s->cpool.get(), 1, &cb);
}

bool create_colormap_sampler(TerrainIsland* s) {
    if (s->colormapSamp) return true;
    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;   // GL_LINEAR
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; // GL_CLAMP_TO_EDGE
    sci.maxLod = 0.25f;
    return mc2vk::create(s->colormapSamp, s->device, [&](auto* h) { return vkCreateSampler(s->device, &sci, nullptr, h); });
}

// Generic: build a VK sampled image from raw bytes (staging -> copy -> SHADER_READ).
// Frees the previous image/view (passed by ref). Reused by BOTH the colormap and cement
// copy-once imports. Match `fmt` to the GL upload's channel order (RGBA8 storage read
// back as GL_RGBA -> R8G8B8A8_UNORM; do NOT double-encode sRGB).
bool upload_sampled_image(TerrainIsland* s, mc2vk::OwnedImage& outImg, mc2vk::OwnedImageView& outView,
                          VkFormat fmt, uint32_t width, uint32_t height,
                          const void* bytes, VkDeviceSize byteCount) {
    // Free any prior import (idempotent re-import on a per-mission atlas rebuild): the view goes before the image it covers.
    outView.reset();
    outImg.reset();

    // Staging buffer (host-visible), released at the end of the import whatever the path.
    mc2vk::OwnedBuffer stg;
    if (!make_buffer(s, stg, byteCount, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, "terrain.staging")) return false;
    { void* p = nullptr;
      if (vmaMapMemory(s->allocator, stg.allocation(), &p) != VK_SUCCESS) return false;
      std::memcpy(p, bytes, (size_t)byteCount); vmaUnmapMemory(s->allocator, stg.allocation()); }

    // Device-local sampled image.
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D; ici.format = fmt;
    ici.extent = {width, height, 1}; ici.mipLevels = 1; ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo aci{}; aci.usage = VMA_MEMORY_USAGE_AUTO;
    if (!outImg.create(s->allocator, s->device, ici, aci, "terrain.texture")) return false;

    // Copy staging -> image, transition UNDEFINED -> TRANSFER_DST -> SHADER_READ.
    VkCommandBuffer cb = begin_oneshot(s);
    if (cb == VK_NULL_HANDLE) return false;
    VkImageMemoryBarrier bar{};
    bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; bar.image = outImg.get();
    bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    bar.srcAccessMask = 0; bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &bar);
    VkBufferImageCopy rgn{};
    rgn.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rgn.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(cb, stg.get(), outImg.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rgn);
    bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &bar);
    end_oneshot(s, cb);

    // Image view.
    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = outImg.get(); vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = fmt;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return mc2vk::create(outView, s->device, [&](auto* h) { return vkCreateImageView(s->device, &vci, nullptr, h); });
}

// Colormap-atlas wrapper over the generic uploader.
bool upload_colormap_image(TerrainIsland* s, VkFormat fmt, uint32_t width, uint32_t height,
                           const void* bytes, VkDeviceSize byteCount) {
    return upload_sampled_image(s, s->colormapImg, s->colormapView,
                                fmt, width, height, bytes, byteCount);
}

// Create a 1x1 opaque placeholder so binding 2 is ALWAYS a valid descriptor even
// before/without the real atlas (colormapReady stays false -> FS uses olive).
bool ensure_dummy_colormap(TerrainIsland* s) {
    if (!create_colormap_sampler(s)) return false;
    if (s->colormapView) return true; // something already bound
    const uint8_t px[4] = {107, 102, 71, 255}; // ~olive, RGBA8; irrelevant (flag=0)
    if (!upload_colormap_image(s, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, px, sizeof(px))) return false;
    s->colormapReady = false;
    return true;
}

// Copy-once import of the colormap atlas from the terrain field store (RGBA8 texels or BC7 blocks, the bytes GL uploaded), again
// whenever the store's version changes (a per-mission rebuild). Sets s->colormapReady on success. Fail-soft: leaves the dummy
// bound (colormapReady=false) so the FS falls back to olive.
void import_colormap(TerrainIsland* s, const Mc2TerrainTier1Feed& feed) {
    if (!feed.colormapBytes || feed.colormapVersion == 0) return;                          // atlas not built yet -> stay olive
    if (feed.colormapVersion == s->colormapVersionImported && s->colormapReady) return;     // already imported
    const VkFormat fmt = feed.colormapVkFormat == 146u ? VK_FORMAT_BC7_SRGB_BLOCK
                       : feed.colormapVkFormat == 145u ? VK_FORMAT_BC7_UNORM_BLOCK
                       : VK_FORMAT_R8G8B8A8_UNORM;
    if (upload_colormap_image(s, fmt, (uint32_t)feed.colormapWidth, (uint32_t)feed.colormapHeight, feed.colormapBytes,
                              (VkDeviceSize)feed.colormapByteCount)) {
        s->colormapVersionImported = feed.colormapVersion;
        s->colormapReady = true;
        write_descriptors(s);
        vlog("COLORMAP-BASE: imported the colormap atlas %dx%d format %u from the terrain field store (version %llu).",
             feed.colormapWidth, feed.colormapHeight, feed.colormapVkFormat, (unsigned long long)feed.colormapVersion);
    }
    // On failure the dummy stays bound (colormapReady=false) -> FS uses olive.
}

// ---------------------------------------------------------------------------
// CEMENT/ROAD atlas sub-slice: copy-once import of the GL cement/road overlay atlas
// (g_cementAtlasGLTex, runtime-assembled RGBA8, GL_REPEAT wrap) + a REPEAT sampler
// (binding 3), plus a map-load snapshot of the per-vertex cement-word SSBO (GL binding
// 25 -> here binding 4). REUSES the colormap copy-once primitive verbatim; the ONLY
// deltas are (a) a REPEAT (not CLAMP) sampler — cement tiles wrap via fract() UV — and
// (b) the extra cement-word SSBO snapshot fed to the FS's cement branch.
// ---------------------------------------------------------------------------

bool create_cement_sampler(TerrainIsland* s) {
    if (s->cementSamp) return true;
    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;   // GL_LINEAR
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    // ★CEMENT: GL_REPEAT wrap (gos_terrain_indirect.cpp:1291-1292) — cement tiles wrap.
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.maxLod = 0.25f;
    return mc2vk::create(s->cementSamp, s->device, [&](auto* h) { return vkCreateSampler(s->device, &sci, nullptr, h); });
}

// 1x1 placeholder so binding 3 is ALWAYS a valid descriptor before the real atlas
// imports (cementReady stays false -> FS ignores the cement branch -> colormap base).
bool ensure_dummy_cement(TerrainIsland* s) {
    if (!create_cement_sampler(s)) return false;
    if (s->cementView) return true; // already bound
    const uint8_t px[4] = {128, 128, 128, 255}; // neutral grey; irrelevant (useCement=0)
    if (!upload_sampled_image(s, s->cementImg, s->cementView,
                              VK_FORMAT_R8G8B8A8_UNORM, 1, 1, px, sizeof(px))) return false;
    s->cementReady = false;
    return true;
}

// Copy-once import of the cement atlas from the terrain field store (RGBA8, the texels GL uploaded), again whenever the store's
// version changes (a per-mission rebuild). Fail-soft: on failure the dummy stays bound and the FS uses the colormap base.
void import_cement(TerrainIsland* s, const Mc2TerrainTier1Feed& feed) {
    if (!feed.cementAtlasRgba || feed.cementAtlasSide <= 0 || feed.cementAtlasVersion == 0) return;   // atlas not built yet
    if (feed.cementAtlasVersion == s->cementVersionImported && s->cementReady) return;              // already imported
    const uint32_t side = static_cast<uint32_t>(feed.cementAtlasSide);
    if (upload_sampled_image(s, s->cementImg, s->cementView, VK_FORMAT_R8G8B8A8_UNORM, side, side,
                             feed.cementAtlasRgba, static_cast<VkDeviceSize>(side) * side * 4u)) {
        s->cementVersionImported = feed.cementAtlasVersion;
        s->cementReady = true;
        write_descriptors(s);
        vlog("CEMENT/ROAD: imported the cement atlas %ux%u from the terrain field store (version %llu).", side, side,
             static_cast<unsigned long long>(feed.cementAtlasVersion));
    }
    // On failure the dummy stays bound (cementReady=false) -> FS uses colormap base.
}

// ---------------------------------------------------------------------------
// VK-TERRAIN-DETAIL: 2D_ARRAY sampled-image uploader (RGBA8, all layers at level 0).
// Mirrors upload_sampled_image but with arrayLayers=layerCount and a 2D_ARRAY view.
// `bytes` is tightly packed layer-major (layer0 WxH RGBA8, then layer1, ...), exactly
// what glGetTexImage(GL_TEXTURE_2D_ARRAY, level0) returns.
bool upload_sampled_image_array(TerrainIsland* s, mc2vk::OwnedImage& outImg, mc2vk::OwnedImageView& outView,
                                VkFormat fmt, uint32_t width,
                                uint32_t height, uint32_t layers,
                                const void* bytes, VkDeviceSize byteCount) {
    outView.reset();
    outImg.reset();

    mc2vk::OwnedBuffer stg;
    if (!make_buffer(s, stg, byteCount, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, "terrain.staging")) return false;
    { void* p = nullptr;
      if (vmaMapMemory(s->allocator, stg.allocation(), &p) != VK_SUCCESS) return false;
      std::memcpy(p, bytes, (size_t)byteCount); vmaUnmapMemory(s->allocator, stg.allocation()); }

    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D; ici.format = fmt;
    ici.extent = {width, height, 1}; ici.mipLevels = 1; ici.arrayLayers = layers;
    ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo aci{}; aci.usage = VMA_MEMORY_USAGE_AUTO;
    if (!outImg.create(s->allocator, s->device, ici, aci, "terrain.texture")) return false;

    VkCommandBuffer cb = begin_oneshot(s);
    if (cb == VK_NULL_HANDLE) return false;
    VkImageMemoryBarrier bar{};
    bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; bar.image = outImg.get();
    bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    bar.srcAccessMask = 0; bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &bar);
    // One copy region covering all layers (tightly packed layer-major, level 0).
    VkBufferImageCopy rgn{};
    rgn.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers};
    rgn.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(cb, stg.get(), outImg.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rgn);
    bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &bar);
    end_oneshot(s, cb);

    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = outImg.get(); vci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY; vci.format = fmt;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    return mc2vk::create(outView, s->device, [&](auto* h) { return vkCreateImageView(s->device, &vci, nullptr, h); });
}

bool create_matnormal_sampler(TerrainIsland* s) {
    if (s->matNormalSamp) return true;
    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;   // GL_LINEAR (mag)
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    // GL_REPEAT wrap (gameos_graphics.cpp:2174-2175) — material detail normals tile.
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.maxLod = 0.25f;  // base level only (VK detail port samples level 0, no anti-tile mips)
    return mc2vk::create(s->matNormalSamp, s->device, [&](auto* h) { return vkCreateSampler(s->device, &sci, nullptr, h); });
}

// 1x1x1 flat-normal placeholder so binding 6 is ALWAYS a valid descriptor before the
// real array imports (matNormalReady stays false -> FS uses macro-slope-only normal).
bool ensure_dummy_matnormal(TerrainIsland* s) {
    if (!create_matnormal_sampler(s)) return false;
    if (s->matNormalView) return true;
    const uint8_t px[4] = {128, 128, 255, 255}; // (0,0,1) tangent-space flat normal
    if (!upload_sampled_image_array(s, s->matNormalImg, s->matNormalView,
                                    VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, px, sizeof(px))) return false;
    s->matNormalReady = false;
    s->matNormalLayers = 1;
    return true;
}

// Copy-once import of the terrain detail-normal array (level 0, one layer per slot), assembled from the slot textures' own texels
// and dropped after the upload, so no CPU copy of the array outlives it; again whenever the GL array is rebuilt (its name is the
// change key). Absent optional slots get the flat normal, as in GL's array. The VK detail port samples no anti-tile mips.
// Fail-soft: a slot that is not RGBA8 at slot 0's size is rejected with a log line and the dummy stays bound.
void import_matnormal(TerrainIsland* s, const Mc2TerrainTier1Feed& feed) {
    const uint32_t glTex = feed.glMatNormalArrayTex;
    if (glTex == 0) return;                        // array not built yet -> macro-slope only
    if (glTex == s->matNormalTexImported && s->matNormalReady) return; // already imported
    if (glTex == s->matNormalTexRejected) return;
    constexpr uint32_t kSlots = 9;
    GosTexels slots[kSlots];
    for (uint32_t i = 0; i < kSlots; ++i) {
        if (feed.matNormalHandles[i] == 0) continue;   // 0 = the invalid gos handle
        slots[i] = gos_GetTextureTexels(feed.matNormalHandles[i]);
        if (slots[i].status == GosTexels::Status::Pending) return;   // retried next frame
    }
    const uint32_t w = slots[0].width, h = slots[0].height;
    const size_t layerBytes = (size_t)w * h * 4u;
    for (uint32_t i = 0; i < kSlots; ++i) {
        const bool absent = feed.matNormalHandles[i] == 0;
        if (absent && i >= 5) continue;            // GL requires slots 0-4 only
        if (absent || slots[i].status != GosTexels::Status::Ready || slots[i].vkFormat != mc2tex::kVkFormatRgba8Unorm ||
            slots[i].width != w || slots[i].height != h || w == 0 || h == 0 || !slots[i].bytes ||
            slots[i].bytes->size() != layerBytes) {
            vlog("VK-TERRAIN-DETAIL: detail-normal slot %u rejected (%ux%u fmt=%u, slot 0 %ux%u); macro slope only.",
                 i, slots[i].width, slots[i].height, slots[i].vkFormat, w, h);
            s->matNormalTexRejected = glTex;
            return;
        }
    }
    std::vector<uint8_t> px(layerBytes * kSlots);
    for (uint32_t i = 0; i < kSlots; ++i) {
        uint8_t* dst = px.data() + (size_t)i * layerBytes;
        if (slots[i].bytes) {
            std::memcpy(dst, slots[i].bytes->data(), layerBytes);
        } else {
            for (size_t t = 0; t < layerBytes; t += 4) { dst[t] = 128; dst[t + 1] = 128; dst[t + 2] = 255; dst[t + 3] = 255; }
        }
    }
    if (upload_sampled_image_array(s, s->matNormalImg, s->matNormalView, VK_FORMAT_R8G8B8A8_UNORM, w, h, kSlots, px.data(),
                                   (VkDeviceSize)px.size())) {
        s->matNormalTexImported = glTex;
        s->matNormalLayers = kSlots;
        s->matNormalReady = true;
        write_descriptors(s);
        vlog("VK-TERRAIN-DETAIL: imported the detail-normal array %ux%u x%u layers from the slot textures.", w, h, kSlots);
    }
    // On failure the dummy stays bound (matNormalReady=false) -> FS uses macro-slope only.
}

// Copy the store's cement words into the VK-owned buffer when the feed version changes
// (map load), mirroring snapshot_height. A map without words gets an all-zero field.
bool snapshot_cement(TerrainIsland* s, const Mc2TerrainTier1Feed& feed) {
    if (feed.cementBytes == 0) return false;  // no map yet
    if (feed.cementVersion == s->cementVersionCopied && s->cementWordsReady) return true;
    // Ensure capacity (grow-only, like the height buffer).
    if (!s->cementBuf || (VkDeviceSize)feed.cementBytes > s->cementCapBytes) {
        if (s->cementBuf) {
            mc2vk::getOrCreateEngineVulkanContext().waitIdle("terrain_cement_grow");
            s->cementBuf.reset();
        }
        VkDeviceSize cap = (VkDeviceSize)feed.cementBytes + feed.cementBytes / 4 + 4096;
        if (!make_buffer(s, s->cementBuf, cap, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, "terrain.cementBuf"))
            return false;
        s->cementCapBytes = cap;
        s->cementVersionCopied = UINT64_MAX; // force re-snapshot into the new buffer
        write_descriptors(s);
    }
    // VK-TERRAIN-FENCE-DEFER-1: single-instance rare rewrite (map-load version bump)
    // — device-idle before writing in place (see snapshot_height).
    mc2vk::getOrCreateEngineVulkanContext().waitIdle("terrain_cement_snapshot");
    void* p = nullptr;
    if (vmaMapMemory(s->allocator, s->cementBuf.allocation(), &p) != VK_SUCCESS) return false;
    if (feed.cementCpu) std::memcpy(p, feed.cementCpu, (size_t)feed.cementBytes);
    else std::memset(p, 0, (size_t)feed.cementBytes);
    vmaUnmapMemory(s->allocator, s->cementBuf.allocation());
    s->cementVersionCopied = feed.cementVersion;
    s->cementWordsReady = true;
    return true;
}

// VK-TERRAIN-VISUALDISPLACE-FAR-1: copy the store's 4x visual-heightfield bake (the GL
// binding-26 bytes) into the VK-owned buffer when the feed version changes. Version-gated
// mission-load copy, mirroring snapshot_cement — the
// bake is uploaded once per mission (gos_TerrainLodChunk_UploadVisualHeightFull), so
// this is a cheap no-op on every other frame. Fail-soft: on any failure
// visualHeightReady stays false -> UBO visualMode forced 0 -> VS draws undisplaced.
bool snapshot_visual_height(TerrainIsland* s, const Mc2TerrainTier1Feed& feed) {
    if (!feed.visualHeightCpu || feed.visualHeightBytes == 0) return false;  // bake not live
    if (feed.visualHeightVersion == s->visualHeightVersionCopied && s->visualHeightReady) return true;
    // Ensure capacity (grow-only, like the cement buffer).
    if (!s->visualHeightBuf || (VkDeviceSize)feed.visualHeightBytes > s->visualHeightCapBytes) {
        if (s->visualHeightBuf) {
            mc2vk::getOrCreateEngineVulkanContext().waitIdle("terrain_visual_height_grow");
            s->visualHeightBuf.reset();
        }
        VkDeviceSize cap = (VkDeviceSize)feed.visualHeightBytes + feed.visualHeightBytes / 4 + 4096;
        if (!make_buffer(s, s->visualHeightBuf, cap, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, "terrain.visualHeightBuf"))
            return false;
        s->visualHeightCapBytes = cap;
        s->visualHeightVersionCopied = UINT64_MAX; // force re-snapshot into the new buffer
        write_descriptors(s);
    }
    // VK-TERRAIN-FENCE-DEFER-1: single-instance rare rewrite (once-per-mission bake)
    // — device-idle before writing in place (see snapshot_height).
    mc2vk::getOrCreateEngineVulkanContext().waitIdle("terrain_visual_height_snapshot");
    void* p = nullptr;
    if (vmaMapMemory(s->allocator, s->visualHeightBuf.allocation(), &p) != VK_SUCCESS) return false;
    std::memcpy(p, feed.visualHeightCpu, (size_t)feed.visualHeightBytes);
    vmaUnmapMemory(s->allocator, s->visualHeightBuf.allocation());
    s->visualHeightVersionCopied = feed.visualHeightVersion;
    s->visualHeightReady = true;
    static bool s_first = false;
    if (!s_first) {
        vlog("VK-TERRAIN-VISUALDISPLACE-FAR-1: snapshotted the visual-height bake "
             "(V=%d bytes=%llu ver=%llu); VK far bands now Z-swap like GL mode 2.",
             feed.visualSide, (unsigned long long)feed.visualHeightBytes,
             (unsigned long long)feed.visualHeightVersion);
        s_first = true;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Shadow maps the terrain samples: the static sun map (binding 5), the single dynamic map (binding 7) and the cascades (bindings 8, 9). All of them
// are the VK shadow island's images; this island only owns 1x1 dummies that keep each binding valid before the first product.
// ---------------------------------------------------------------------------

// Depth-COMPARE sampler: compareOp=LESS_OR_EQUAL, LINEAR HW PCF, CLAMP_TO_BORDER
// white=1.0 (out-of-map -> lit). Mirrors GL sampler2DShadow.
bool ensure_shadow_sampler(TerrainIsland* s) {
    if (s->shadowSamp) return true;
    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;   // HW LINEAR PCF (2x2)
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;   // out-of-map compare -> 1.0 (lit)
    sci.compareEnable = VK_TRUE;
    sci.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;            // ref <= stored ? 1 : 0 (proven by spike)
    sci.maxLod = 0.25f;
    return mc2vk::create(s->shadowSamp, s->device, [&](auto* h) { return vkCreateSampler(s->device, &sci, nullptr, h); });
}

// A D32 depth-compare image of `dim` texels for one binding's dummy (static: shadowImg/View/Dim, dynamic: dynShadow*), idempotent when unchanged.
// Left in DEPTH SHADER_READ so the binding is always valid.
bool ensure_shadow_image_slot(TerrainIsland* s, uint32_t dim,
                              mc2vk::OwnedImage& img, mc2vk::OwnedImageView& view, uint32_t* curDim) {
    if (dim == 0) dim = 1;
    if (img && *curDim == dim) return true;
    view.reset();   // the view goes before the image it covers
    img.reset();

    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D; ici.format = VK_FORMAT_D32_SFLOAT;
    ici.extent = {dim, dim, 1}; ici.mipLevels = 1; ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo aci{}; aci.usage = VMA_MEMORY_USAGE_AUTO;
    if (!img.create(s->allocator, s->device, ici, aci, "terrain.shadowDepth")) return false;
    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = img.get(); vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = VK_FORMAT_D32_SFLOAT;
    vci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    if (!mc2vk::create(view, s->device, [&](auto* h) { return vkCreateImageView(s->device, &vci, nullptr, h); })) {
        img.reset();
        return false;
    }
    *curDim = dim;
    VkCommandBuffer cb = begin_oneshot(s);
    if (cb != VK_NULL_HANDLE) {
        VkImageMemoryBarrier bar{};
        bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; bar.image = img.get();
        bar.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        bar.srcAccessMask = 0; bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &bar);
        end_oneshot(s, cb);
    }
    write_descriptors(s);
    return true;
}

bool ensure_shadow_image(TerrainIsland* s, uint32_t dim) {
    return ensure_shadow_image_slot(s, dim, s->shadowImg, s->shadowView, &s->shadowDim);
}

// VK-TERRAIN-CSM-NEAR-1: (Re)create a D32 depth-COMPARE ARRAY image at dim x layers
// (idempotent when unchanged). Mirrors ensure_shadow_image_slot but with arrayLayers>1
// and a 2D_ARRAY view (so the frag can bind it as sampler2DArrayShadow).
bool ensure_csm_array_image(TerrainIsland* s, uint32_t dim, uint32_t layers) {
    if (dim == 0) dim = 1;
    if (layers == 0) layers = 1;
    if (s->csmArrayImg && s->csmArrayDim == dim && s->csmArrayLayers == layers)
        return true;
    s->csmArrayView.reset();   // the view goes before the image it covers
    s->csmArrayImg.reset();

    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D; ici.format = VK_FORMAT_D32_SFLOAT;
    ici.extent = {dim, dim, 1}; ici.mipLevels = 1; ici.arrayLayers = layers;
    ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo aci{}; aci.usage = VMA_MEMORY_USAGE_AUTO;
    if (!s->csmArrayImg.create(s->allocator, s->device, ici, aci, "terrain.csmArray")) return false;
    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = s->csmArrayImg.get(); vci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY; vci.format = VK_FORMAT_D32_SFLOAT;
    vci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, layers};
    if (!mc2vk::create(s->csmArrayView, s->device, [&](auto* h) { return vkCreateImageView(s->device, &vci, nullptr, h); })) {
        s->csmArrayImg.reset();
        return false;
    }
    s->csmArrayDim = dim; s->csmArrayLayers = layers;
    VkCommandBuffer cb = begin_oneshot(s);
    if (cb != VK_NULL_HANDLE) {
        VkImageMemoryBarrier bar{};
        bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; bar.image = s->csmArrayImg.get();
        bar.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, layers};
        bar.srcAccessMask = 0; bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &bar);
        end_oneshot(s, cb);
    }
    write_descriptors(s);
    return true;
}

bool ensure_dummy_csm(TerrainIsland* s) {
    if (!ensure_shadow_sampler(s)) return false;
    if (!s->csmArrayView) {
        if (!ensure_csm_array_image(s, 1, 1)) return false;
        s->csmArrayReady = false;
    }
    if (!s->csmFullMapView) {
        if (!ensure_shadow_image_slot(s, 1, s->csmFullMapImg, s->csmFullMapView, &s->csmFullMapDim)) return false;
        s->csmFullMapReady = false;
    }
    return true;
}

// DIRECT-VIEW-ONLY near-cascade array (binding 8) + full-map cascade (binding 9). The VK
// shadow island renders both cascades natively into VK-owned D32 depth targets
// (MC2_VK_CSM_ARRAY_INTEROP / MC2_VK_CSM_FULLMAP_INTEROP, under MC2_VK_SHADOW_INTEROP)
// and terrain binds their VkImageViews directly, with no per-frame GL depth readback.
//
// Fail-soft: on ANY decline (csm not built this frame, or the shadow-island direct producer
// not yet ready during the ~1-2 warmup frames before its first CSM product) this returns
// false, the caller forces UBO csmEnabled=0, and the frag falls back to the legacy
// single-map dynamic-shadow path. This is NEVER a hole and NEVER stale: all
// ready/external/direct flags are reset every frame at the top, so a declined frame binds
// the 1x1 dummies (ensure_dummy_csm) and the next ready frame re-points bindings 8/9 at the
// live external views. The old copy-mode gave CSM immediately during warmup via a GL
// readback; the direct-only path accepts 1-2 warmup frames on the legacy path instead.
bool copy_csm_shadows(TerrainIsland* s, const Mc2TerrainTier1Feed& feed) {
    s->csmArrayReady = false;
    s->csmFullMapReady = false;
    s->csmArrayExternalView = VK_NULL_HANDLE;
    s->csmArrayDirect = false;
    s->csmFullMapExternalView = VK_NULL_HANDLE;
    s->csmFullMapDirect = false;
    if (!feed.csmEnabled || feed.csmArraySize <= 0 || feed.csmFullMapSize <= 0 || feed.csmCount <= 1)
        return false;

    // Near-cascade array: direct VK shadow-island view only (warmup-declines to legacy).
    if (!mc2_vk_shadow_island_csm_array_ready()) return false;
    void* arrView = mc2_vk_shadow_island_get_csm_array_view();
    if (arrView == nullptr) return false;
    s->csmArrayExternalView = reinterpret_cast<VkImageView>(arrView);
    s->csmArrayDirect = true;

    // Full-map catch-all cascade: direct VK shadow-island view only.
    if (!mc2_vk_shadow_island_csm_fullmap_ready()) return false;
    void* fmView = mc2_vk_shadow_island_get_csm_fullmap_view();
    if (fmView == nullptr) return false;
    s->csmFullMapExternalView = reinterpret_cast<VkImageView>(fmView);
    s->csmFullMapDirect = true;

    s->csmArrayReady = true;
    s->csmFullMapReady = true;
    return true;
}

// 1x1 dummies so bindings 5 (static) and 7 (dynamic) are valid from frame 1.
bool ensure_dummy_shadow(TerrainIsland* s) {
    if (!ensure_shadow_sampler(s)) return false;
    if (!s->shadowView) {
        if (!ensure_shadow_image(s, 1)) return false;
        s->shadowReady = false;
    }
    if (!s->dynShadowView) {
        if (!ensure_shadow_image_slot(s, 1, s->dynShadowImg, s->dynShadowView, &s->dynShadowDim)) return false;
        s->dynShadowReady = false;
    }
    return true;
}

// The static sun shadow map (binding 5): the VK shadow island's own map, used only when its build is of the feed's generation, so the map and the
// static light matrix in the UBO come from the same engine build. Between a rebake and its build (one frame) terrain has no static shadow.
bool bind_static_shadow(TerrainIsland* s, const Mc2TerrainTier1Feed& feed) {
    s->shadowReady = false;
    s->shadowExternalView = VK_NULL_HANDLE;
    if (!feed.lightingEnabled || !feed.shadowsEnabled || feed.staticShadowGeneration == 0) return false;
    if (mc2_vk_shadow_island_static_generation() != feed.staticShadowGeneration) return false;
    void* view = mc2_vk_shadow_island_get_static_view();
    if (view == nullptr) return false;
    s->shadowExternalView = reinterpret_cast<VkImageView>(view);
    s->shadowReady = true;
    return true;
}

// The single dynamic shadow map (binding 7) for frames without cascades (ShadowCSM=FALSE): the VK shadow island's own depth view when it produced one this
// frame, else no dynamic shadow on terrain this frame. There is no GL-copy fallback: it was a per-frame GL depth readback, which the co-write path treats
// as fatal, so the first frame of a ShadowCSM=FALSE mission (shadow island not yet up) latched the whole terrain island off and the VK scene was never
// cleared again (objects smeared over a frame with no terrain).
bool copy_dynamic_shadow(TerrainIsland* s, const Mc2TerrainTier1Feed& feed) {
    s->dynShadowReady = false;
    s->dynShadowExternalView = VK_NULL_HANDLE;
    s->dynShadowDirect = false;
    if (!feed.lightingEnabled || !feed.dynShadowsEnabled) return false;

    // The cascades serve the dynamic shadow whenever the island produced both of them this frame; the single map is then neither produced nor copied.
    if (mc2_vk_shadow_island_csm_array_ready() && mc2_vk_shadow_island_csm_fullmap_ready()) return false;

    // The shadow island recorded this frame's map into its one submit; the same queue and its render-pass dependencies order this read behind the write.
    if (mc2_vk_shadow_island_dyn_shadow_ready()) {
        void* rawView = mc2_vk_shadow_island_get_dyn_shadow_view();
        if (rawView != nullptr) {
            s->dynShadowExternalView = reinterpret_cast<VkImageView>(rawView);
            s->dynShadowDirect = true;
            s->dynShadowReady = true;
            return true;
        }
    }

    // Expected on the first mission's first frames; logged once per process, so a later mission whose map never arrives
    // is silent.
    static bool s_noDynMapLogged = false;
    if (!s_noDynMapLogged) {
        s_noDynMapLogged = true;
        vlog("dynamic shadow: no VK dynamic map yet; terrain has no dynamic shadow this frame");
    }
    return false;
}

} // namespace

#endif // MC2_VULKAN_ISLAND
