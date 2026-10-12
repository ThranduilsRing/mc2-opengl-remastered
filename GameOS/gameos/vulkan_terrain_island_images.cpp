// VK terrain island: height snapshot and audit, the colormap / cement / material-normal imports and the shadow image copies.
// Declared in the class headers; shared names in vulkan_terrain_island_internal.h.
#include "vulkan_terrain_island_internal.h"

#ifdef MC2_VULKAN_ISLAND


namespace vk_terrain_detail {

// Snapshot the GL height SSBO into the VK-owned buffer when the feed version changes.
// Map-load copy (glGetBufferSubData -> vmaMap), NOT a per-frame handoff readback.
bool snapshot_height(TerrainIsland* s, const Mc2TerrainTier1Feed& feed) {
    if (feed.heightVersion == s->heightVersionCopied) return true; // unchanged
    if (feed.glHeightSsbo == 0 || feed.heightBytes == 0) return false;
    if (!ensure_height_capacity(s, (VkDeviceSize)feed.heightBytes)) return false;
    // VK-TERRAIN-FENCE-DEFER-1: the height SSBO stays single-instance (version-gated
    // rare rewrite: mission load / crater deform). Under the deferred retire a prior
    // terrain submit may still be reading it — device-idle before rewriting in place.
    // (The old inline post-submit fence wait used to provide this guarantee for free.)
    vkDeviceWaitIdle(s->device);
    std::vector<uint8_t> scratch((size_t)feed.heightBytes);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, (GLuint)feed.glHeightSsbo);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)feed.heightBytes, scratch.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    void* p = nullptr;
    if (vmaMapMemory(s->allocator, s->heightAlloc, &p) != VK_SUCCESS) return false;
    std::memcpy(p, scratch.data(), (size_t)feed.heightBytes);
    vmaUnmapMemory(s->allocator, s->heightAlloc);
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
    cbi.commandPool = s->cpool; cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbi.commandBufferCount = 1;
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
    VkFence f = VK_NULL_HANDLE;
    VkFenceCreateInfo fci{}; fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateFence(s->device, &fci, nullptr, &f) != VK_SUCCESS) f = VK_NULL_HANDLE;
    vkQueueSubmit(s->queue, 1, &si, f);
    if (f != VK_NULL_HANDLE) {
        vkWaitForFences(s->device, 1, &f, VK_TRUE, 5ull * 1000 * 1000 * 1000);
        vkDestroyFence(s->device, f, nullptr);
    } else {
        vkQueueWaitIdle(s->queue);
    }
    vkFreeCommandBuffers(s->device, s->cpool, 1, &cb);
}

bool create_colormap_sampler(TerrainIsland* s) {
    if (s->colormapSamp != VK_NULL_HANDLE) return true;
    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;   // GL_LINEAR
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; // GL_CLAMP_TO_EDGE
    sci.maxLod = 0.25f;
    return vkCreateSampler(s->device, &sci, nullptr, &s->colormapSamp) == VK_SUCCESS;
}

// Generic: build a VK sampled image from raw bytes (staging -> copy -> SHADER_READ).
// Frees the previous image/view (passed by ref). Reused by BOTH the colormap and cement
// copy-once imports. Match `fmt` to the GL upload's channel order (RGBA8 storage read
// back as GL_RGBA -> R8G8B8A8_UNORM; do NOT double-encode sRGB).
bool upload_sampled_image(TerrainIsland* s, VkImage* outImg, VmaAllocation* outAlloc,
                          VkImageView* outView, VkFormat fmt, uint32_t width, uint32_t height,
                          const void* bytes, VkDeviceSize byteCount) {
    // Free any prior import (idempotent re-import on a per-mission atlas rebuild).
    if (*outView) { vkDestroyImageView(s->device, *outView, nullptr); *outView = VK_NULL_HANDLE; }
    if (*outImg)  { vmaDestroyImage(s->allocator, *outImg, *outAlloc); *outImg = VK_NULL_HANDLE; }

    // Staging buffer (host-visible).
    VkBuffer stg = VK_NULL_HANDLE; VmaAllocation stgAlloc = VK_NULL_HANDLE;
    if (!make_buffer(s, byteCount, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &stg, &stgAlloc, true)) return false;
    { void* p = nullptr;
      if (vmaMapMemory(s->allocator, stgAlloc, &p) != VK_SUCCESS) { vmaDestroyBuffer(s->allocator, stg, stgAlloc); return false; }
      std::memcpy(p, bytes, (size_t)byteCount); vmaUnmapMemory(s->allocator, stgAlloc); }

    // Device-local sampled image.
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D; ici.format = fmt;
    ici.extent = {width, height, 1}; ici.mipLevels = 1; ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo aci{}; aci.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateImage(s->allocator, &ici, &aci, outImg, outAlloc, nullptr) != VK_SUCCESS) {
        vmaDestroyBuffer(s->allocator, stg, stgAlloc); return false;
    }

    // Copy staging -> image, transition UNDEFINED -> TRANSFER_DST -> SHADER_READ.
    VkCommandBuffer cb = begin_oneshot(s);
    if (cb == VK_NULL_HANDLE) { vmaDestroyBuffer(s->allocator, stg, stgAlloc); return false; }
    VkImageMemoryBarrier bar{};
    bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; bar.image = *outImg;
    bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    bar.srcAccessMask = 0; bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &bar);
    VkBufferImageCopy rgn{};
    rgn.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rgn.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(cb, stg, *outImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rgn);
    bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &bar);
    end_oneshot(s, cb);
    vmaDestroyBuffer(s->allocator, stg, stgAlloc);

    // Image view.
    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = *outImg; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = fmt;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(s->device, &vci, nullptr, outView) != VK_SUCCESS) return false;
    return true;
}

// Colormap-atlas wrapper over the generic uploader.
bool upload_colormap_image(TerrainIsland* s, VkFormat fmt, uint32_t width, uint32_t height,
                           const void* bytes, VkDeviceSize byteCount) {
    return upload_sampled_image(s, &s->colormapImg, &s->colormapAlloc, &s->colormapView,
                                fmt, width, height, bytes, byteCount);
}

// Create a 1x1 opaque placeholder so binding 2 is ALWAYS a valid descriptor even
// before/without the real atlas (colormapReady stays false -> FS uses olive).
bool ensure_dummy_colormap(TerrainIsland* s) {
    if (!create_colormap_sampler(s)) return false;
    if (s->colormapView != VK_NULL_HANDLE) return true; // something already bound
    const uint8_t px[4] = {107, 102, 71, 255}; // ~olive, RGBA8; irrelevant (flag=0)
    if (!upload_colormap_image(s, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, px, sizeof(px))) return false;
    s->colormapReady = false;
    return true;
}

// Copy-once import of the GL colormap atlas. Re-imports only when the GL tex name
// changes (per-mission rebuild). Sets s->colormapReady on success. Fail-soft: leaves
// the dummy bound (colormapReady=false) so the FS falls back to olive.
void import_colormap(TerrainIsland* s, const Mc2TerrainTier1Feed& feed) {
    const uint32_t glTex = feed.glColormapTex;
    if (glTex == 0) return;                       // atlas not built yet -> stay olive
    if (glTex == s->colormapTexImported && s->colormapReady) return; // already imported

    // Query the REAL internalformat of the GL atlas (RGBA8 vs BC7 UNORM/SRGB).
    glBindTexture(GL_TEXTURE_2D, (GLuint)glTex);
    GLint w = 0, h = 0, internalFmt = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &internalFmt);
    if (w <= 0 || h <= 0) { glBindTexture(GL_TEXTURE_2D, 0); return; }

    bool ok = false;
    if (internalFmt == GL_COMPRESSED_RGBA_BPTC_UNORM ||
        internalFmt == GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM) {
        // BC7: copy the compressed blocks verbatim. VK format per UNORM/SRGB.
        GLint compBytes = 0;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_COMPRESSED_IMAGE_SIZE, &compBytes);
        if (compBytes > 0) {
            std::vector<uint8_t> blocks((size_t)compBytes);
            glGetCompressedTexImage(GL_TEXTURE_2D, 0, blocks.data());
            const VkFormat vkFmt = (internalFmt == GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM)
                                 ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK;
            ok = upload_colormap_image(s, vkFmt, (uint32_t)w, (uint32_t)h, blocks.data(), (VkDeviceSize)compBytes);
        }
    } else {
        // RGBA8 path: the GL atlas is RGBA8 storage (BGRA source swizzled to RGBA on
        // upload). Read it back as RGBA -> VK_FORMAT_R8G8B8A8_UNORM (channel order
        // preserved end-to-end; do NOT feed BGRA to an R8G8B8A8 image).
        std::vector<uint8_t> px((size_t)w * (size_t)h * 4u);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        ok = upload_colormap_image(s, VK_FORMAT_R8G8B8A8_UNORM, (uint32_t)w, (uint32_t)h,
                                   px.data(), (VkDeviceSize)px.size());
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    if (ok) {
        s->colormapTexImported = glTex;
        s->colormapReady = true;
        write_descriptors(s);
        static bool s_firstCm = false;
        if (!s_firstCm) {
            vlog("COLORMAP-BASE: imported GL colormap atlas gltex=%u %dx%d internalFmt=0x%x "
                 "(RGBA8->R8G8B8A8 or BC7 verbatim); VK terrain base color now real.",
                 glTex, w, h, (unsigned)internalFmt);
            s_firstCm = true;
        }
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
    if (s->cementSamp != VK_NULL_HANDLE) return true;
    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;   // GL_LINEAR
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    // ★CEMENT: GL_REPEAT wrap (gos_terrain_indirect.cpp:1291-1292) — cement tiles wrap.
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.maxLod = 0.25f;
    return vkCreateSampler(s->device, &sci, nullptr, &s->cementSamp) == VK_SUCCESS;
}

// 1x1 placeholder so binding 3 is ALWAYS a valid descriptor before the real atlas
// imports (cementReady stays false -> FS ignores the cement branch -> colormap base).
bool ensure_dummy_cement(TerrainIsland* s) {
    if (!create_cement_sampler(s)) return false;
    if (s->cementView != VK_NULL_HANDLE) return true; // already bound
    const uint8_t px[4] = {128, 128, 128, 255}; // neutral grey; irrelevant (useCement=0)
    if (!upload_sampled_image(s, &s->cementImg, &s->cementAlloc, &s->cementView,
                              VK_FORMAT_R8G8B8A8_UNORM, 1, 1, px, sizeof(px))) return false;
    s->cementReady = false;
    return true;
}

// Copy-once import of the GL cement atlas. Re-imports only when the GL tex name changes
// (per-mission rebuild). The cement atlas is ALWAYS RGBA8 storage (runtime-assembled;
// never BC7), uploaded from BGRA source swizzled to RGBA8 on upload — so reading it back
// as GL_RGBA -> VK_FORMAT_R8G8B8A8_UNORM preserves channel order end-to-end, IDENTICAL
// to the colormap RGBA8 path. Fail-soft: on failure the dummy stays bound.
void import_cement(TerrainIsland* s, const Mc2TerrainTier1Feed& feed) {
    const uint32_t glTex = feed.glCementAtlasTex;
    if (glTex == 0) return;                     // atlas not built yet -> cement stays off
    if (glTex == s->cementTexImported && s->cementReady) return; // already imported

    glBindTexture(GL_TEXTURE_2D, (GLuint)glTex);
    GLint w = 0, h = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
    if (w <= 0 || h <= 0) { glBindTexture(GL_TEXTURE_2D, 0); return; }
    std::vector<uint8_t> px((size_t)w * (size_t)h * 4u);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    if (upload_sampled_image(s, &s->cementImg, &s->cementAlloc, &s->cementView,
                             VK_FORMAT_R8G8B8A8_UNORM, (uint32_t)w, (uint32_t)h,
                             px.data(), (VkDeviceSize)px.size())) {
        s->cementTexImported = glTex;
        s->cementReady = true;
        write_descriptors(s);
        static bool s_firstCem = false;
        if (!s_firstCem) {
            vlog("CEMENT/ROAD: imported GL cement atlas gltex=%u %dx%d (RGBA8->R8G8B8A8, "
                 "REPEAT wrap); VK terrain concrete apron now matches GL.", glTex, w, h);
            s_firstCem = true;
        }
    }
    // On failure the dummy stays bound (cementReady=false) -> FS uses colormap base.
}

// ---------------------------------------------------------------------------
// VK-TERRAIN-DETAIL: 2D_ARRAY sampled-image uploader (RGBA8, all layers at level 0).
// Mirrors upload_sampled_image but with arrayLayers=layerCount and a 2D_ARRAY view.
// `bytes` is tightly packed layer-major (layer0 WxH RGBA8, then layer1, ...), exactly
// what glGetTexImage(GL_TEXTURE_2D_ARRAY, level0) returns.
bool upload_sampled_image_array(TerrainIsland* s, VkImage* outImg, VmaAllocation* outAlloc,
                                VkImageView* outView, VkFormat fmt, uint32_t width,
                                uint32_t height, uint32_t layers,
                                const void* bytes, VkDeviceSize byteCount) {
    if (*outView) { vkDestroyImageView(s->device, *outView, nullptr); *outView = VK_NULL_HANDLE; }
    if (*outImg)  { vmaDestroyImage(s->allocator, *outImg, *outAlloc); *outImg = VK_NULL_HANDLE; }

    VkBuffer stg = VK_NULL_HANDLE; VmaAllocation stgAlloc = VK_NULL_HANDLE;
    if (!make_buffer(s, byteCount, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &stg, &stgAlloc, true)) return false;
    { void* p = nullptr;
      if (vmaMapMemory(s->allocator, stgAlloc, &p) != VK_SUCCESS) { vmaDestroyBuffer(s->allocator, stg, stgAlloc); return false; }
      std::memcpy(p, bytes, (size_t)byteCount); vmaUnmapMemory(s->allocator, stgAlloc); }

    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D; ici.format = fmt;
    ici.extent = {width, height, 1}; ici.mipLevels = 1; ici.arrayLayers = layers;
    ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo aci{}; aci.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateImage(s->allocator, &ici, &aci, outImg, outAlloc, nullptr) != VK_SUCCESS) {
        vmaDestroyBuffer(s->allocator, stg, stgAlloc); return false;
    }

    VkCommandBuffer cb = begin_oneshot(s);
    if (cb == VK_NULL_HANDLE) { vmaDestroyBuffer(s->allocator, stg, stgAlloc); return false; }
    VkImageMemoryBarrier bar{};
    bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; bar.image = *outImg;
    bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    bar.srcAccessMask = 0; bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &bar);
    // One copy region covering all layers (tightly packed layer-major, level 0).
    VkBufferImageCopy rgn{};
    rgn.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers};
    rgn.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(cb, stg, *outImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rgn);
    bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &bar);
    end_oneshot(s, cb);
    vmaDestroyBuffer(s->allocator, stg, stgAlloc);

    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = *outImg; vci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY; vci.format = fmt;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    if (vkCreateImageView(s->device, &vci, nullptr, outView) != VK_SUCCESS) return false;
    return true;
}

bool create_matnormal_sampler(TerrainIsland* s) {
    if (s->matNormalSamp != VK_NULL_HANDLE) return true;
    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;   // GL_LINEAR (mag)
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    // GL_REPEAT wrap (gameos_graphics.cpp:2174-2175) — material detail normals tile.
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.maxLod = 0.25f;  // base level only (VK detail port samples level 0, no anti-tile mips)
    return vkCreateSampler(s->device, &sci, nullptr, &s->matNormalSamp) == VK_SUCCESS;
}

// 1x1x1 flat-normal placeholder so binding 6 is ALWAYS a valid descriptor before the
// real array imports (matNormalReady stays false -> FS uses macro-slope-only normal).
bool ensure_dummy_matnormal(TerrainIsland* s) {
    if (!create_matnormal_sampler(s)) return false;
    if (s->matNormalView != VK_NULL_HANDLE) return true;
    const uint8_t px[4] = {128, 128, 255, 255}; // (0,0,1) tangent-space flat normal
    if (!upload_sampled_image_array(s, &s->matNormalImg, &s->matNormalAlloc, &s->matNormalView,
                                    VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, px, sizeof(px))) return false;
    s->matNormalReady = false;
    s->matNormalLayers = 1;
    return true;
}

// Copy-once import of the GL terrain_normal_array_tex_ (9-layer RGBA8 GL_TEXTURE_2D_ARRAY,
// GL_REPEAT). Re-imports only when the GL tex name changes (per-mission rebuild). The GL
// array is RGBA8 storage (glTexImage3D GL_RGBA8); read back level 0 all layers as GL_RGBA
// -> VK_FORMAT_R8G8B8A8_UNORM (channel order preserved). Level-0 only (VK detail port does
// not use anti-tile mips). Fail-soft: on failure the dummy stays bound.
void import_matnormal(TerrainIsland* s, const Mc2TerrainTier1Feed& feed) {
    const uint32_t glTex = feed.glMatNormalArrayTex;
    if (glTex == 0) return;                        // array not built yet -> macro-slope only
    if (glTex == s->matNormalTexImported && s->matNormalReady) return; // already imported

    glBindTexture(GL_TEXTURE_2D_ARRAY, (GLuint)glTex);
    // Read the ARRAY's base level (GL_TEXTURE_BASE_LEVEL may be >0 after mip trimming);
    // query dimensions at level 0 (glTexImage3D allocates the full array at level 0).
    GLint w = 0, h = 0, layers = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_WIDTH,  &w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_HEIGHT, &h);
    glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_DEPTH,  &layers);
    if (w <= 0 || h <= 0 || layers <= 0) { glBindTexture(GL_TEXTURE_2D_ARRAY, 0); return; }
    std::vector<uint8_t> px((size_t)w * (size_t)h * (size_t)layers * 4u);
    glGetTexImage(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

    if (upload_sampled_image_array(s, &s->matNormalImg, &s->matNormalAlloc, &s->matNormalView,
                                   VK_FORMAT_R8G8B8A8_UNORM, (uint32_t)w, (uint32_t)h,
                                   (uint32_t)layers, px.data(), (VkDeviceSize)px.size())) {
        s->matNormalTexImported = glTex;
        s->matNormalLayers = (uint32_t)layers;
        s->matNormalReady = true;
        write_descriptors(s);
        static bool s_firstMn = false;
        if (!s_firstMn) {
            vlog("VK-TERRAIN-DETAIL: imported GL matNormalArray gltex=%u %dx%d x%d layers "
                 "(RGBA8->R8G8B8A8_2D_ARRAY, REPEAT); VK terrain grass now perturbed like GL.",
                 glTex, w, h, layers);
            s_firstMn = true;
        }
    }
    // On failure the dummy stays bound (matNormalReady=false) -> FS uses macro-slope only.
}

// Snapshot the GL cement-word SSBO into the VK-owned buffer when the feed version
// changes. Map-load copy (glGetBufferSubData -> vmaMap), mirroring snapshot_height.
bool snapshot_cement(TerrainIsland* s, const Mc2TerrainTier1Feed& feed) {
    if (feed.glCementSsbo == 0 || feed.cementBytes == 0) return false;  // not live yet
    if (feed.cementVersion == s->cementVersionCopied && s->cementWordsReady) return true;
    // Ensure capacity (grow-only, like the height buffer).
    if (s->cementBuf == VK_NULL_HANDLE || (VkDeviceSize)feed.cementBytes > s->cementCapBytes) {
        if (s->cementBuf != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(s->device);
            vmaDestroyBuffer(s->allocator, s->cementBuf, s->cementBufAlloc);
            s->cementBuf = VK_NULL_HANDLE;
        }
        VkDeviceSize cap = (VkDeviceSize)feed.cementBytes + feed.cementBytes / 4 + 4096;
        if (!make_buffer(s, cap, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &s->cementBuf, &s->cementBufAlloc, true))
            return false;
        s->cementCapBytes = cap;
        s->cementVersionCopied = UINT64_MAX; // force re-snapshot into the new buffer
        write_descriptors(s);
    }
    // VK-TERRAIN-FENCE-DEFER-1: single-instance rare rewrite (map-load version bump)
    // — device-idle before writing in place (see snapshot_height).
    vkDeviceWaitIdle(s->device);
    std::vector<uint8_t> scratch((size_t)feed.cementBytes);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, (GLuint)feed.glCementSsbo);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)feed.cementBytes, scratch.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    void* p = nullptr;
    if (vmaMapMemory(s->allocator, s->cementBufAlloc, &p) != VK_SUCCESS) return false;
    std::memcpy(p, scratch.data(), (size_t)feed.cementBytes);
    vmaUnmapMemory(s->allocator, s->cementBufAlloc);
    s->cementVersionCopied = feed.cementVersion;
    s->cementWordsReady = true;
    return true;
}

// VK-TERRAIN-VISUALDISPLACE-FAR-1: snapshot the GL 4x visual-heightfield bake SSBO
// (binding 26) into the VK-owned buffer when the feed version changes. Version-gated
// mission-load copy (glGetBufferSubData -> vmaMap), mirroring snapshot_cement — the
// bake is uploaded once per mission (gos_TerrainLodChunk_UploadVisualHeightFull), so
// this is a cheap no-op on every other frame. Fail-soft: on any failure
// visualHeightReady stays false -> UBO visualMode forced 0 -> VS draws undisplaced.
bool snapshot_visual_height(TerrainIsland* s, const Mc2TerrainTier1Feed& feed) {
    if (feed.glVisualHeightSsbo == 0 || feed.visualHeightBytes == 0) return false;  // bake not live
    if (feed.visualHeightVersion == s->visualHeightVersionCopied && s->visualHeightReady) return true;
    // Ensure capacity (grow-only, like the cement buffer).
    if (s->visualHeightBuf == VK_NULL_HANDLE || (VkDeviceSize)feed.visualHeightBytes > s->visualHeightCapBytes) {
        if (s->visualHeightBuf != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(s->device);
            vmaDestroyBuffer(s->allocator, s->visualHeightBuf, s->visualHeightAlloc);
            s->visualHeightBuf = VK_NULL_HANDLE;
        }
        VkDeviceSize cap = (VkDeviceSize)feed.visualHeightBytes + feed.visualHeightBytes / 4 + 4096;
        if (!make_buffer(s, cap, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &s->visualHeightBuf, &s->visualHeightAlloc, true))
            return false;
        s->visualHeightCapBytes = cap;
        s->visualHeightVersionCopied = UINT64_MAX; // force re-snapshot into the new buffer
        write_descriptors(s);
    }
    // VK-TERRAIN-FENCE-DEFER-1: single-instance rare rewrite (once-per-mission bake)
    // — device-idle before writing in place (see snapshot_height).
    vkDeviceWaitIdle(s->device);
    std::vector<uint8_t> scratch((size_t)feed.visualHeightBytes);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, (GLuint)feed.glVisualHeightSsbo);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)feed.visualHeightBytes, scratch.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    void* p = nullptr;
    if (vmaMapMemory(s->allocator, s->visualHeightAlloc, &p) != VK_SUCCESS) return false;
    std::memcpy(p, scratch.data(), (size_t)feed.visualHeightBytes);
    vmaUnmapMemory(s->allocator, s->visualHeightAlloc);
    s->visualHeightVersionCopied = feed.visualHeightVersion;
    s->visualHeightReady = true;
    static bool s_first = false;
    if (!s_first) {
        vlog("VK-TERRAIN-VISUALDISPLACE-FAR-1: snapshotted GL visual-height bake "
             "(V=%d bytes=%llu ver=%llu); VK far bands now Z-swap like GL mode 2.",
             feed.visualSide, (unsigned long long)feed.visualHeightBytes,
             (unsigned long long)feed.visualHeightVersion);
        s_first = true;
    }
    return true;
}

// ---------------------------------------------------------------------------
// VK-TERRAIN-LIGHTING sub-slice: STATIC sun shadow map. VK-SHADOW-COPY-ONCE-1 UPDATE:
// the GL static shadow map is built ONCE PER MISSION (gos_StaticLightMatrixBuilt()
// latch), not per frame -- the original "GL map updates every frame" premise below was
// wrong for the STATIC map (it is correct for the DYNAMIC map, which is genuinely
// re-rendered every frame from moving mechs/buildings and stays copy-per-frame). The
// static copy is now gated on feed.staticShadowGeneration so it only re-copies when the
// GL side actually rebuilt (mission load / re-arm). glGetTexImage on a DEPTH texture is
// NOT a color readback, so it does not trip the co-write path's forbidden-readback
// assert (that counts glGetTexImage/glReadPixels via g_forbidden, which this does not
// bump — it is a distinct depth pull on the shadow FBO's texture, endorsed for a probe).
// ---------------------------------------------------------------------------

// Depth-COMPARE sampler: compareOp=LESS_OR_EQUAL, LINEAR HW PCF, CLAMP_TO_BORDER
// white=1.0 (out-of-map -> lit). Mirrors GL sampler2DShadow.
bool ensure_shadow_sampler(TerrainIsland* s) {
    if (s->shadowSamp != VK_NULL_HANDLE) return true;
    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;   // HW LINEAR PCF (2x2)
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;   // out-of-map compare -> 1.0 (lit)
    sci.compareEnable = VK_TRUE;
    sci.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;            // ref <= stored ? 1 : 0 (proven by spike)
    sci.maxLod = 0.25f;
    return vkCreateSampler(s->device, &sci, nullptr, &s->shadowSamp) == VK_SUCCESS;
}

// (Re)create the VK D32 depth image at `dim` texels (idempotent when unchanged).
// Leaves it in DEPTH SHADER_READ layout so binding 5 is always valid; the per-frame
// copy re-transitions it. Frees the prior image on a size change.
// Slot-parameterized D32 depth-compare image ensure (static uses shadowImg/View/Dim,
// dynamic uses dynShadow*). Leaves DEPTH SHADER_READ so the binding is always valid.
bool ensure_shadow_image_slot(TerrainIsland* s, uint32_t dim,
                              VkImage* img, VmaAllocation* alloc, VkImageView* view, uint32_t* curDim) {
    if (dim == 0) dim = 1;
    if (*img != VK_NULL_HANDLE && *curDim == dim) return true;
    if (*view) { vkDestroyImageView(s->device, *view, nullptr); *view = VK_NULL_HANDLE; }
    if (*img)  { vmaDestroyImage(s->allocator, *img, *alloc); *img = VK_NULL_HANDLE; }

    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D; ici.format = VK_FORMAT_D32_SFLOAT;
    ici.extent = {dim, dim, 1}; ici.mipLevels = 1; ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo aci{}; aci.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateImage(s->allocator, &ici, &aci, img, alloc, nullptr) != VK_SUCCESS) {
        *img = VK_NULL_HANDLE; return false;
    }
    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = *img; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = VK_FORMAT_D32_SFLOAT;
    vci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(s->device, &vci, nullptr, view) != VK_SUCCESS) {
        vmaDestroyImage(s->allocator, *img, *alloc); *img = VK_NULL_HANDLE;
        *view = VK_NULL_HANDLE; return false;
    }
    *curDim = dim;
    VkCommandBuffer cb = begin_oneshot(s);
    if (cb != VK_NULL_HANDLE) {
        VkImageMemoryBarrier bar{};
        bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; bar.image = *img;
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
    return ensure_shadow_image_slot(s, dim, &s->shadowImg, &s->shadowAlloc, &s->shadowView, &s->shadowDim);
}

// VK-TERRAIN-CSM-NEAR-1: (Re)create a D32 depth-COMPARE ARRAY image at dim x layers
// (idempotent when unchanged). Mirrors ensure_shadow_image_slot but with arrayLayers>1
// and a 2D_ARRAY view (so the frag can bind it as sampler2DArrayShadow).
bool ensure_csm_array_image(TerrainIsland* s, uint32_t dim, uint32_t layers) {
    if (dim == 0) dim = 1;
    if (layers == 0) layers = 1;
    if (s->csmArrayImg != VK_NULL_HANDLE && s->csmArrayDim == dim && s->csmArrayLayers == layers)
        return true;
    if (s->csmArrayView) { vkDestroyImageView(s->device, s->csmArrayView, nullptr); s->csmArrayView = VK_NULL_HANDLE; }
    if (s->csmArrayImg)  { vmaDestroyImage(s->allocator, s->csmArrayImg, s->csmArrayAlloc); s->csmArrayImg = VK_NULL_HANDLE; }

    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D; ici.format = VK_FORMAT_D32_SFLOAT;
    ici.extent = {dim, dim, 1}; ici.mipLevels = 1; ici.arrayLayers = layers;
    ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo aci{}; aci.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateImage(s->allocator, &ici, &aci, &s->csmArrayImg, &s->csmArrayAlloc, nullptr) != VK_SUCCESS) {
        s->csmArrayImg = VK_NULL_HANDLE; return false;
    }
    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = s->csmArrayImg; vci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY; vci.format = VK_FORMAT_D32_SFLOAT;
    vci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, layers};
    if (vkCreateImageView(s->device, &vci, nullptr, &s->csmArrayView) != VK_SUCCESS) {
        vmaDestroyImage(s->allocator, s->csmArrayImg, s->csmArrayAlloc); s->csmArrayImg = VK_NULL_HANDLE;
        s->csmArrayView = VK_NULL_HANDLE; return false;
    }
    s->csmArrayDim = dim; s->csmArrayLayers = layers;
    VkCommandBuffer cb = begin_oneshot(s);
    if (cb != VK_NULL_HANDLE) {
        VkImageMemoryBarrier bar{};
        bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; bar.image = s->csmArrayImg;
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
    if (s->csmArrayView == VK_NULL_HANDLE) {
        if (!ensure_csm_array_image(s, 1, 1)) return false;
        s->csmArrayReady = false;
    }
    if (s->csmFullMapView == VK_NULL_HANDLE) {
        if (!ensure_shadow_image_slot(s, 1, &s->csmFullMapImg, &s->csmFullMapAlloc,
                                      &s->csmFullMapView, &s->csmFullMapDim)) return false;
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
    if (!feed.csmEnabled || feed.glCsmArrayTex == 0 || feed.csmArraySize <= 0
        || feed.glCsmFullMapTex == 0 || feed.csmFullMapSize <= 0 || feed.csmCount <= 1)
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
    if (s->shadowView == VK_NULL_HANDLE) {
        if (!ensure_shadow_image(s, 1)) return false;
        s->shadowReady = false;
    }
    if (s->dynShadowView == VK_NULL_HANDLE) {
        if (!ensure_shadow_image_slot(s, 1, &s->dynShadowImg, &s->dynShadowAlloc,
                                      &s->dynShadowView, &s->dynShadowDim)) return false;
        s->dynShadowReady = false;
    }
    return true;
}

// Slot-parameterized per-frame depth copy from a GL DEPTH_COMPONENT texture into a VK
// D32 image. Shared by static (binding 5) and dynamic (binding 7) shadow maps. glGetTexImage
// depth readback -> forbidden color counters untouched.
bool copy_shadow_slot(TerrainIsland* s, uint32_t glTex, uint32_t dim,
                      VkImage img, VkBuffer* stage, VmaAllocation* stageAlloc, VkDeviceSize* stageBytes,
                      bool perFrame) {
    // Strict no-interop: never do the per-frame GL depth readback fallback (fail-soft
    // to unshadowed for this frame). Copy-once static-shadow imports (perFrame=false)
    // are still allowed.
    if (perFrame && terrainNoInteropStrict()) return false;
    const VkDeviceSize bytes = (VkDeviceSize)dim * (VkDeviceSize)dim * 4u;  // float32 per texel
    if (*stage == VK_NULL_HANDLE || bytes > *stageBytes) {
        if (*stage != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(s->device);
            vmaDestroyBuffer(s->allocator, *stage, *stageAlloc);
            *stage = VK_NULL_HANDLE;
        }
        if (!make_buffer(s, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, stage, stageAlloc, true))
            return false;
        *stageBytes = bytes;
    }
    void* p = nullptr;
    if (vmaMapMemory(s->allocator, *stageAlloc, &p) != VK_SUCCESS) return false;
    // CSM-BLACKOUT-RECON-1 FIX (same GL depth-readback hazard class): save/restore the
    // active unit's GL_TEXTURE_2D binding instead of stomping it to 0 mid-frame.
    GLint prev2DBind = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev2DBind);
    glBindTexture(GL_TEXTURE_2D, (GLuint)glTex);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, GL_FLOAT, p);
    if (perFrame) terrainPerFrameReadback("copy_shadow_slot");  // H2b: dynamic/csm-fullmap fallback only
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev2DBind);
    vmaUnmapMemory(s->allocator, *stageAlloc);

    VkCommandBuffer cb = begin_oneshot(s);
    if (cb == VK_NULL_HANDLE) return false;
    VkImageMemoryBarrier bar{};
    bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; bar.image = img;
    bar.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    bar.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.srcAccessMask = VK_ACCESS_SHADER_READ_BIT; bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &bar);
    VkBufferImageCopy rgn{};
    rgn.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    rgn.imageExtent = {dim, dim, 1};
    vkCmdCopyBufferToImage(cb, *stage, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rgn);
    bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &bar);
    end_oneshot(s, cb);
    return true;
}

// VK-SHADOW-COPY-ONCE-1: copy-once-per-generation of the GL STATIC shadow depth texture
// into the VK D32 image (binding 5). The GL static map is built ONCE per mission
// (gos_StaticLightMatrixBuilt() latch; see gos_postprocess.h/.cpp), not per frame, so
// feed.staticShadowGeneration only bumps on a real (re)build. Skipping the
// glGetTexImage(4096^2 float)->staging->VK-upload on unchanged frames removes a fixed
// per-frame tax with zero pixel change (same bytes end up in the VK image either way).
bool copy_static_shadow(TerrainIsland* s, const Mc2TerrainTier1Feed& feed) {
    if (!feed.lightingEnabled || !feed.shadowsEnabled) { s->shadowReady = false; return false; }
    if (feed.glStaticShadowTex == 0 || feed.staticShadowSize <= 0) { s->shadowReady = false; return false; }
    const uint32_t dim = (uint32_t)feed.staticShadowSize;
    if (!ensure_shadow_image(s, dim)) { s->shadowReady = false; return false; }

    // Already have this generation's content in the VK image (and dim unchanged, else
    // ensure_shadow_image would have recreated it and shadowReady/shadowGenCopied must be
    // re-synced below since a size change invalidates the copied image contents).
    if (s->shadowReady && s->shadowGenCopied == feed.staticShadowGeneration && s->shadowDim == dim)
        return true;

    if (!copy_shadow_slot(s, feed.glStaticShadowTex, dim, s->shadowImg,
                          &s->shadowStage, &s->shadowStageAlloc, &s->shadowStageBytes,
                          /*perFrame=*/false)) {  // copy-once per generation (guarded above)
        s->shadowReady = false;
        return false;
    }
    s->shadowGenCopied = feed.staticShadowGeneration;
    s->shadowReady = true;
    return true;
}

// Per-frame copy of the GL DYNAMIC (building/mech) shadow depth texture into the 2nd VK
// D32 image (binding 7). Same mechanism as static; gated by MC2_VK_TERRAIN_DYNSHADOW feed.
//
// VK-SHADOW-DIRECT-CONSUME-1: when the VK shadow island (MC2_VK_SHADOW_INTEROP) produced
// this frame's dynamic shadow depth, skip the glGetTexImage+vkCmdCopyBufferToImage
// round-trip entirely and point binding 7 at the shadow island's own VkImageView
// (dynShadowExternalView; write_descriptors() picks it up). Falls back to the existing
// GL-copy mechanism whenever the shadow island declined this frame (fail-soft, never a
// hole — matches the umbrella's existing per-frame fail-soft convention). This is a
// dyn-shadow-CONSUME optimization only; the interop gate + producer are unchanged.
bool copy_dynamic_shadow(TerrainIsland* s, const Mc2TerrainTier1Feed& feed) {
    s->dynShadowReady = false;
    s->dynShadowExternalView = VK_NULL_HANDLE;
    s->dynShadowDirect = false;
    if (!feed.lightingEnabled || !feed.dynShadowsEnabled) return false;

    // Direct path: the shadow island already produced+fenced a VK-native depth image
    // this exact frame (same-queue, already retired -- see accessor doc comment).
    if (mc2_vk_shadow_island_dyn_shadow_ready()) {
        void* rawView = mc2_vk_shadow_island_get_dyn_shadow_view();
        if (rawView != nullptr) {
            s->dynShadowExternalView = reinterpret_cast<VkImageView>(rawView);
            s->dynShadowDirect = true;
            s->dynShadowReady = true;
            return true;
        }
    }

    // Fallback: existing GL-copy mechanism (unchanged).
    if (feed.glDynamicShadowTex == 0 || feed.dynamicShadowSize <= 0) return false;
    const uint32_t dim = (uint32_t)feed.dynamicShadowSize;
    if (!ensure_shadow_image_slot(s, dim, &s->dynShadowImg, &s->dynShadowAlloc,
                                  &s->dynShadowView, &s->dynShadowDim)) return false;
    if (!copy_shadow_slot(s, feed.glDynamicShadowTex, dim, s->dynShadowImg,
                          &s->dynShadowStage, &s->dynShadowStageAlloc, &s->dynShadowStageBytes,
                          /*perFrame=*/true)) return false;  // H2b: per-frame fallback (direct-chain declined)
    s->dynShadowReady = true;
    return true;
}

} // namespace

#endif // MC2_VULKAN_ISLAND
