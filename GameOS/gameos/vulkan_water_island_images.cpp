// VK water island: the HDRI environment and shore height / mask texture imports.
// Declared in the class headers; shared names in vulkan_water_island_internal.h.
#include "vulkan_water_island_internal.h"

#ifdef MC2_VULKAN_ISLAND


namespace vk_water_detail {

// RUNG-6.5 WATER-REFLECTION: COPY-ONCE HDRI equirect import (NOT per-frame).
// Mirrors gos_hdri.cpp's two upload paths:
//   - BC6H .ktx2 sidecar (field default, MC2_HDRI_BC6H on): single mip 0
//     (GL_TEXTURE_MAX_LEVEL=0), byte-copied via glGetCompressedTexImage ->
//     VK_FORMAT_BC6H_{U,S}FLOAT_BLOCK. GL's textureLod at LOD 4 clamps to mip 0
//     on this texture, so a 1-mip VK image is exact parity.
//   - RGBA16F tinyexr fallback: full glGenerateMipmap chain, each level
//     byte-copied via glGetTexImage(GL_HALF_FLOAT) -> VK_FORMAT_R16G16B16A16_SFLOAT.
// COST (honest): one-time ~24MB (BC6H 4K mip0) or ~85MB (RGBA16F 4K + chain) CPU
// staging + one upload submit at first armed-water frame after HDRI ready. Zero
// per-frame cost afterwards. Fail-soft: bind a 1x1 black dummy + patch the FS
// UBO's u_waterHdriLod to -1 (SH-L2 fallback) — same semantics as GL !isHdriReady().
// Not counted in g_forbidden: those counters guard per-frame scene-surface
// copies; this is a one-time content-texture import (same class as the prop
// island's copy-once albedo array).
bool ensure_hdri(WaterIsland* s, unsigned int hdriGlTex) {
    if (s->hdriAttempted && s->hdriGlTexId == hdriGlTex) return s->hdriView != VK_NULL_HANDLE;
    s->hdriAttempted = true;
    s->hdriGlTexId   = hdriGlTex;
    s->hdriRealImported = false;
    // Re-import (tex id changed, e.g. dummy -> real after late HDRI load):
    // drop the old image first. Cheap + rare; idle the device so no pending
    // command buffer still references the outgoing view.
    if (s->hdriView != VK_NULL_HANDLE || s->hdriImage != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(s->device);
        if (s->hdriView)  { vkDestroyImageView(s->device, s->hdriView, nullptr); s->hdriView = VK_NULL_HANDLE; }
        if (s->hdriImage) { vmaDestroyImage(s->allocator, s->hdriImage, s->hdriAlloc); s->hdriImage = VK_NULL_HANDLE; }
    }

    if (s->hdriSampler == VK_NULL_HANDLE) {
        VkSamplerCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.maxLod = VK_LOD_CLAMP_NONE;
        if (vkCreateSampler(s->device, &sci, nullptr, &s->hdriSampler) != VK_SUCCESS) {
            vlog("hdri: vkCreateSampler failed."); return false;
        }
    }

    // Gather GL-side facts + bytes.
    VkFormat fmt = VK_FORMAT_R16G16B16A16_SFLOAT;
    int w = 1, h = 1;
    uint32_t mips = 1;
    std::vector<std::vector<unsigned char>> levels;   // per-mip raw bytes
    std::vector<std::pair<int,int>> levelDims;
    bool haveReal = false;
    if (hdriGlTex != 0) {
        GLint prevTex = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
        glBindTexture(GL_TEXTURE_2D, (GLuint)hdriGlTex);
        GLint ifmt = 0, compressed = 0, w0 = 0, h0 = 0, maxLevel = 1000;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &ifmt);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_COMPRESSED, &compressed);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH,  &w0);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h0);
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, &maxLevel);
        if (w0 > 0 && h0 > 0) {
            if (compressed &&
                (ifmt == GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT ||
                 ifmt == GL_COMPRESSED_RGB_BPTC_SIGNED_FLOAT)) {
                // BC6H path — respect the device feature (MoltenVK/Metal may lack BC).
                VkPhysicalDeviceFeatures feats{};
                vkGetPhysicalDeviceFeatures(s->phys, &feats);
                if (feats.textureCompressionBC) {
                    fmt = (ifmt == GL_COMPRESSED_RGB_BPTC_SIGNED_FLOAT)
                        ? VK_FORMAT_BC6H_SFLOAT_BLOCK : VK_FORMAT_BC6H_UFLOAT_BLOCK;
                    GLint sz = 0;
                    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0,
                        GL_TEXTURE_COMPRESSED_IMAGE_SIZE, &sz);
                    if (sz > 0) {
                        levels.emplace_back((size_t)sz);
                        glGetCompressedTexImage(GL_TEXTURE_2D, 0, levels[0].data());
                        levelDims.emplace_back(w0, h0);
                        w = w0; h = h0; mips = 1;
                        haveReal = (glGetError() == GL_NO_ERROR);
                    }
                } else {
                    vlog("hdri: device lacks textureCompressionBC — SH-L2 fallback.");
                }
            } else if (ifmt == GL_RGBA16F) {
                // RGBA16F + full glGenerateMipmap chain.
                fmt = VK_FORMAT_R16G16B16A16_SFLOAT;
                w = w0; h = h0;
                int lw = w0, lh = h0;
                for (int lvl = 0; lvl <= maxLevel; ++lvl) {
                    GLint tw = 0;
                    glGetTexLevelParameteriv(GL_TEXTURE_2D, lvl, GL_TEXTURE_WIDTH, &tw);
                    if (tw <= 0) break;
                    lw = (w0 >> lvl) > 0 ? (w0 >> lvl) : 1;
                    lh = (h0 >> lvl) > 0 ? (h0 >> lvl) : 1;
                    levels.emplace_back((size_t)lw * lh * 8);
                    glGetTexImage(GL_TEXTURE_2D, lvl, GL_RGBA, GL_HALF_FLOAT,
                                  levels.back().data());
                    levelDims.emplace_back(lw, lh);
                    if (lw == 1 && lh == 1) break;
                }
                mips = (uint32_t)levels.size();
                haveReal = (mips > 0) && (glGetError() == GL_NO_ERROR);
            } else {
                vlog("hdri: unexpected GL internal format 0x%x — SH-L2 fallback.", ifmt);
            }
        }
        glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
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
        if (vmaCreateImage(s->allocator, &ii, &aci, &s->hdriImage, &s->hdriAlloc, nullptr) != VK_SUCCESS) {
            vlog("hdri: vmaCreateImage failed."); return false;
        }
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = s->hdriImage; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1};
        if (vkCreateImageView(s->device, &vi, nullptr, &s->hdriView) != VK_SUCCESS) {
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
    VkBuffer staging = VK_NULL_HANDLE; VmaAllocation stagingAlloc = VK_NULL_HANDLE;
    if (!make_buffer(s, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging, &stagingAlloc)) return false;
    {
        void* p = nullptr;
        if (vmaMapMemory(s->allocator, stagingAlloc, &p) != VK_SUCCESS) {
            vmaDestroyBuffer(s->allocator, staging, stagingAlloc); return false;
        }
        for (size_t i = 0; i < levels.size(); ++i)
            std::memcpy((unsigned char*)p + offs[i], levels[i].data(), levels[i].size());
        vmaUnmapMemory(s->allocator, stagingAlloc);
    }

    const bool ok = submit_once(s, [&](VkCommandBuffer cb) {
        VkImageMemoryBarrier bar{};
        bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        bar.srcAccessMask = 0; bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.image = s->hdriImage;
        bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
        for (uint32_t i = 0; i < mips; ++i) {
            VkBufferImageCopy c{};
            c.bufferOffset = offs[i];
            c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
            c.imageExtent = {(uint32_t)levelDims[i].first, (uint32_t)levelDims[i].second, 1};
            vkCmdCopyBufferToImage(cb, staging, s->hdriImage,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        }
        bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
    });
    vmaDestroyBuffer(s->allocator, staging, stagingAlloc);
    if (!ok) { vlog("hdri: upload submit failed."); return false; }

    s->hdriRealImported = haveReal;
    // VK-WATER-FENCE-DEFER-1: with the deferred retire, a prior water submit may still be in
    // flight — device-idle first (rare: at most twice per island, dummy -> real).
    vkDeviceWaitIdle(s->device);
    write_descriptors(s);   // binding 2 now valid
    vlog("hdri %s import: %dx%d fmt=%d mips=%u glTex=%u (copy-once, zero per-frame cost).",
         haveReal ? "REAL" : "DUMMY", w, h, (int)fmt, mips, hdriGlTex);
    return true;
}

// VK-PARITY-WATER-SHORE-1: copy-once import of one GL shore texture (R32F height
// or RGBA8 mask) into a sampled VK image. Mirrors ensure_hdri's pattern (GL
// read-back on the render thread's current context, one staging upload). When
// glTex is 0 (or the read fails) a 1x1 zero dummy is created instead and
// *real is false. Caller device-idles beforehand.
bool import_shore_tex(WaterIsland* s, unsigned int glTex, bool isFloat,
                      VkImage* img, VmaAllocation* alloc, VkImageView* view, bool* real) {
    const VkFormat fmt = isFloat ? VK_FORMAT_R32_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
    const size_t bpp = 4;
    std::vector<unsigned char> bytes;
    int w = 1, h = 1;
    *real = false;
    if (glTex != 0) {
        GLint prevTex = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
        GLint prevPack = 4; glGetIntegerv(GL_PACK_ALIGNMENT, &prevPack);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glBindTexture(GL_TEXTURE_2D, (GLuint)glTex);
        GLint w0 = 0, h0 = 0, ifmt = 0;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH,  &w0);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h0);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &ifmt);
        const GLint wantFmt = isFloat ? GL_R32F : GL_RGBA8;
        if (w0 > 0 && h0 > 0 && ifmt == wantFmt) {
            bytes.resize((size_t)w0 * (size_t)h0 * bpp);
            if (isFloat) glGetTexImage(GL_TEXTURE_2D, 0, GL_RED,  GL_FLOAT,         bytes.data());
            else         glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, bytes.data());
            if (glGetError() == GL_NO_ERROR) { w = w0; h = h0; *real = true; }
        }
        glPixelStorei(GL_PACK_ALIGNMENT, prevPack);
        glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
    }
    if (!*real) { w = 1; h = 1; bytes.assign(bpp, 0); }

    if (*view)  { vkDestroyImageView(s->device, *view, nullptr); *view = VK_NULL_HANDLE; }
    if (*img)   { vmaDestroyImage(s->allocator, *img, *alloc); *img = VK_NULL_HANDLE; }
    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D; ii.format = fmt;
    ii.extent = {(uint32_t)w, (uint32_t)h, 1};
    ii.mipLevels = 1; ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo aci{}; aci.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateImage(s->allocator, &ii, &aci, img, alloc, nullptr) != VK_SUCCESS) {
        vlog("shore: vmaCreateImage failed."); return false;
    }
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = *img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(s->device, &vi, nullptr, view) != VK_SUCCESS) {
        vlog("shore: vkCreateImageView failed."); return false;
    }
    VkBuffer staging = VK_NULL_HANDLE; VmaAllocation stagingAlloc = VK_NULL_HANDLE;
    if (!make_buffer(s, (VkDeviceSize)bytes.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging, &stagingAlloc)) return false;
    {
        void* p = nullptr;
        if (vmaMapMemory(s->allocator, stagingAlloc, &p) != VK_SUCCESS) {
            vmaDestroyBuffer(s->allocator, staging, stagingAlloc); return false;
        }
        std::memcpy(p, bytes.data(), bytes.size());
        vmaUnmapMemory(s->allocator, stagingAlloc);
    }
    const VkImage dst = *img;
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
        vkCmdCopyBufferToImage(cb, staging, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
    });
    vmaDestroyBuffer(s->allocator, staging, stagingAlloc);
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
    vkDeviceWaitIdle(s->device);
    // R32_SFLOAT linear filtering is an optional format feature; without it the
    // height sidecar is treated as absent (FS degrades 2 -> 1, same as a missing
    // sidecar on GL).
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(s->phys, VK_FORMAT_R32_SFLOAT, &fp);
    const bool r32Linear = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
    if (!r32Linear) vlog("shore: R32_SFLOAT lacks linear filtering -- height shore disabled (mode-1 fallback).");
    const unsigned int hTex = r32Linear ? gos_TerrainLodChunk_WaterShoreHeightTex() : 0u;
    if (!import_shore_tex(s, hTex, true,  &s->shHImage, &s->shHAlloc, &s->shHView, &s->shHReal)) return false;
    if (!import_shore_tex(s, gos_TerrainLodChunk_ShorelineMaskTex(), false,
                          &s->shMImage, &s->shMAlloc, &s->shMView, &s->shMReal)) return false;
    write_descriptors(s);
    vlog("shore import: height=%s mask=%s gen=%u (copy-once, zero per-frame cost).",
         s->shHReal ? "REAL" : "DUMMY", s->shMReal ? "REAL" : "DUMMY", gen);
    return true;
}

}  // namespace vk_water_detail

#endif // MC2_VULKAN_ISLAND
