// Cooked BC4 shadow products: key, encoding, transfer and cache persistence.
#ifdef MC2_VULKAN_ISLAND
#include "vulkan_shadow_bake_internal.h"
#include "vulkan_color_prop_island.h"
#include <chrono>
#include <thread>

namespace mc2vk {
using namespace shadow_bake_detail;

// The content key of a mask (P4 disk cache): everything the bake reads, by content, not by process-local versions. The heights, the caster set (handles,
// damage, transforms), the static-prop geometry and its alpha cut-outs, the sun, the water, the layout and the art and evaluator constants.
uint64_t ShadowBake::contentKey(const ShadowBakeInputs& in, uint64_t casterHash) {
    // Each input hashed on its own first, so a key that differs between two runs says which input moved ([SHADOW_BAKE] key parts).
    keyParts_ = {};
    keyParts_[0] = mc2shadow::hashWords(heights_.data(), heights_.size() * sizeof(float));
    keyParts_[1] = casterHash;
    const void* vb = nullptr;
    const void* ib = nullptr;
    uint64_t vbBytes = 0, ibBytes = 0;
    if (in.vbo != VK_NULL_HANDLE && mc2_static_prop_geometry_cpu(&vb, &vbBytes, &ib, &ibBytes) && vb && ib) {
        uint64_t g = mc2shadow::hashWords(vb, static_cast<size_t>(vbBytes));
        g = mc2shadow::hashWords(ib, static_cast<size_t>(ibBytes), g);
        g = mc2shadow::fnv1a(in.packets.data(), in.packets.size_bytes(), g);
        keyParts_[2] = mc2shadow::fnv1a(in.typeFirstPacket.data(), in.typeFirstPacket.size_bytes(), g);
        if (in.alphaReady) keyParts_[3] = mc2shadow::fnv1a(in.packetAlpha.data(), in.packetAlpha.size_bytes());
    }
    const float scalars[13] = {sun_.x, sun_.y, sun_.z, waterZ_, halfMap_, static_cast<float>(mapSide_), static_cast<float>(layout_.texels),
                               maskTarget(), mc2shadow::kSunAngularRadiusDeg, mc2shadow::kFadeStartWorld, mc2shadow::kFadeEndWorld,
                               mc2shadow::kFadeMax, mc2shadow::kTerrainConeScale};
    keyParts_[4] = mc2shadow::fnv1a(scalars, sizeof(scalars));
    const uint32_t versions[5] = {mc2shadow::ShadowMaskAsset::kSchema, mc2shadow::ShadowMaskAsset::kEvaluator, static_cast<uint32_t>(maskCap()),
                                  static_cast<uint32_t>(heightSide_), static_cast<uint32_t>(mc2shadow::shadowQuality().level)};
    keyParts_[5] = mc2shadow::fnv1a(versions, sizeof(versions));
    return mc2shadow::fnv1a(keyParts_.data(), sizeof(keyParts_)) | 1u;
}

// The BC4 encoder's buffers: one tile's level chain plus the top image's chain, and one parameter slice per level (tile, sub-tile, top).
bool ShadowBake::ensureEncoder() {
    if (blockStaging_ && bc4Params_) return true;
    VkDeviceSize off = 0;
    for (uint32_t m = 0; m < kTileLevels; ++m) { tileLevelOff_[m] = off; off += mc2shadow::bc4ChainBytes(mc2shadow::kTileTexels >> m, 1); }
    topLevelOff_.assign(topMips_, 0);
    for (uint32_t k = 0; k < topMips_; ++k) { topLevelOff_[k] = off; off += mc2shadow::bc4ChainBytes((std::max)(topSide_ >> k, 1u), 1); }
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = off;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    if (!blockStaging_.create(allocator_, device_, bci, aci, "shadow.bake.blocks")) { blog("block buffer failed."); return false; }
    const uint32_t slices = 2 * kTileLevels + topMips_;
    if (!makeHostBuffer(bc4Params_, allocator_, device_, slices * kSlice, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, "shadow.bake.bc4Params")) return false;
    std::vector<uint8_t> p(slices * kSlice, 0);
    auto slice = [&](uint32_t i, int32_t side, VkDeviceSize byteOff, int32_t mip) {
        const int32_t v[4] = {side, static_cast<int32_t>(byteOff / 4), mip, 0};
        std::memcpy(p.data() + i * kSlice, v, sizeof(v));
    };
    for (uint32_t m = 0; m < kTileLevels; ++m) {
        slice(m, static_cast<int32_t>(mc2shadow::kTileTexels >> m), tileLevelOff_[m], static_cast<int32_t>(m));
        slice(kSliceSubTile + m, static_cast<int32_t>(mc2shadow::kSubTileTexels >> m), tileLevelOff_[m], static_cast<int32_t>(m));
    }
    for (uint32_t k = 0; k < topMips_; ++k) slice(2 * kTileLevels + k, static_cast<int32_t>((std::max)(topSide_ >> k, 1u)), topLevelOff_[k], static_cast<int32_t>(k));
    if (!bc4Params_.upload(p.data(), p.size())) return false;
    const VkDescriptorBufferInfo par{bc4Params_.get(), 0, 16};
    const VkDescriptorBufferInfo out{blockStaging_.get(), 0, VK_WHOLE_SIZE};
    const VkDescriptorImageInfo tileSrc{depthRawSampler_.get(), tileR8View_ ? tileR8View_.get() : topR8View_.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo topSrc{depthRawSampler_.get(), topR8View_.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w[6]{};
    const VkDescriptorSet sets[2] = {bc4TileSet_, bc4TopSet_};
    for (int k = 0; k < 2; ++k) {
        for (int j = 0; j < 3; ++j) { w[k * 3 + j].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[k * 3 + j].dstSet = sets[k]; w[k * 3 + j].descriptorCount = 1; w[k * 3 + j].dstBinding = static_cast<uint32_t>(j); }
        w[k * 3 + 0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; w[k * 3 + 0].pBufferInfo = &par;
        w[k * 3 + 1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[k * 3 + 1].pImageInfo = k == 0 ? &tileSrc : &topSrc;
        w[k * 3 + 2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[k * 3 + 2].pBufferInfo = &out;
    }
    vkUpdateDescriptorSets(device_, 6, w, 0, nullptr);
    return true;
}

void ShadowBake::encodeLevel(VkCommandBuffer cb, VkDescriptorSet set, uint32_t slice, uint32_t side) {
    const uint32_t off = slice * static_cast<uint32_t>(kSlice);
    bc4_.bind(cb, set, std::span<const uint32_t>(&off, 1));
    const uint32_t groups = ((side + 3) / 4 + 7) / 8;
    vkCmdDispatch(cb, groups, groups, 1);
}

// One tile's (or sub-tile's) resolved texels (the staging words) become its levels 0..5 in the tile image, those levels become BC4 blocks at its place
// in the mask, and its level 5 (4 texels a sub-tile) goes to the top image.
void ShadowBake::recordTileEncode(VkCommandBuffer cb, int originX, int originY, uint32_t T, uint32_t sliceBase) {
    VkImageMemoryBarrier toDst = imageBarrier(tileR8_.get(), VK_IMAGE_ASPECT_COLOR_BIT, 0, kTileLevels, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                              0, VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toDst);
    VkBufferImageCopy copy{};
    copy.bufferRowLength = T;
    copy.bufferImageHeight = T;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {T, T, 1};
    vkCmdCopyBufferToImage(cb, staging_.get(), tileR8_.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    for (uint32_t m = 1; m < kTileLevels; ++m) {
        VkImageMemoryBarrier toSrc = imageBarrier(tileR8_.get(), VK_IMAGE_ASPECT_COLOR_BIT, m - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSrc);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m - 1, 0, 1};
        blit.srcOffsets[1] = {static_cast<int32_t>(T >> (m - 1)), static_cast<int32_t>(T >> (m - 1)), 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1};
        blit.dstOffsets[1] = {static_cast<int32_t>(T >> m), static_cast<int32_t>(T >> m), 1};
        vkCmdBlitImage(cb, tileR8_.get(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tileR8_.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    }
    VkImageMemoryBarrier lastSrc = imageBarrier(tileR8_.get(), VK_IMAGE_ASPECT_COLOR_BIT, kTileLevels - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &lastSrc);
    const uint32_t q = T >> kTopLevel;
    VkImageCopy ic{};
    ic.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, kTopLevel, 0, 1};
    ic.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    ic.dstOffset = {originX >> kTopLevel, originY >> kTopLevel, 0};
    ic.extent = {q, q, 1};
    vkCmdCopyImage(cb, tileR8_.get(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, topR8_.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
    VkImageMemoryBarrier toRead = imageBarrier(tileR8_.get(), VK_IMAGE_ASPECT_COLOR_BIT, 0, kTileLevels, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toRead);
    for (uint32_t m = 0; m < kTileLevels; ++m) encodeLevel(cb, bc4TileSet_, sliceBase + m, T >> m);
    cmdBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy lv[kTileLevels]{};
    for (uint32_t m = 0; m < kTileLevels; ++m) {
        const uint32_t side = T >> m;
        lv[m].bufferOffset = tileLevelOff_[m];
        lv[m].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1};
        lv[m].imageOffset = {originX >> m, originY >> m, 0};
        lv[m].imageExtent = {side, side, 1};
    }
    vkCmdCopyBufferToImage(cb, blockStaging_.get(), maskImage_.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kTileLevels, lv);
}

// The mask levels below the sub-tile grid: the top image's mips, encoded and copied to mask levels kTopLevel+1.. .
void ShadowBake::recordTopLevels(VkCommandBuffer cb) {
    recordTopLevelsMipsOnly(cb);
    if (topMips_ <= 1) return;
    for (uint32_t k = 1; k < topMips_; ++k) encodeLevel(cb, bc4TopSet_, 2 * kTileLevels + k, (std::max)(topSide_ >> k, 1u));
    cmdBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    std::vector<VkBufferImageCopy> lv;
    for (uint32_t k = 1; k < topMips_; ++k) {
        const uint32_t side = (std::max)(topSide_ >> k, 1u);
        VkBufferImageCopy c{};
        c.bufferOffset = topLevelOff_[k];
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, kTopLevel + k, 0, 1};
        c.imageExtent = {side, side, 1};
        lv.push_back(c);
    }
    vkCmdCopyBufferToImage(cb, blockStaging_.get(), maskImage_.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(lv.size()), lv.data());
}

// The BC4 chain's levels as consecutive buffer regions (the cooked asset's layout).
std::vector<VkBufferImageCopy> ShadowBake::chainRegions() const {
    std::vector<VkBufferImageCopy> lv(maskMips_);
    VkDeviceSize off = 0;
    for (uint32_t m = 0; m < maskMips_; ++m) {
        const uint32_t side = (std::max)(static_cast<uint32_t>(maskTexels_) >> m, 1u);
        lv[m] = {};
        lv[m].bufferOffset = off;
        lv[m].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1};
        lv[m].imageExtent = {side, side, 1};
        off += mc2shadow::bc4ChainBytes(side, 1);
    }
    return lv;
}

// Every mask level becomes sampled; a fresh bake is also read back whole into a host buffer the cache store reads once the bake retires.
void ShadowBake::recordMaskToRead(VkCommandBuffer cb, bool readback) {
    VkImageLayout from = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    VkAccessFlags fromAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
    const uint64_t chain = mc2shadow::bc4ChainBytes(static_cast<uint32_t>(maskTexels_), maskMips_);
    if (readback && !cacheDir_.empty() && makeHostBuffer(transfer_, allocator_, device_, chain, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, "shadow.bake.readback")) {
        VkImageMemoryBarrier toSrc = imageBarrier(maskImage_.get(), VK_IMAGE_ASPECT_COLOR_BIT, 0, maskMips_, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                  fromAccess, VK_ACCESS_TRANSFER_READ_BIT);
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSrc);
        const std::vector<VkBufferImageCopy> lv = chainRegions();
        vkCmdCopyImageToBuffer(cb, maskImage_.get(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, transfer_.get(), maskMips_, lv.data());
        cmdBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
        from = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        fromAccess = VK_ACCESS_TRANSFER_READ_BIT;
        readbackPending_ = true;
    }
    VkImageMemoryBarrier toRead = imageBarrier(maskImage_.get(), VK_IMAGE_ASPECT_COLOR_BIT, 0, maskMips_, from, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                               fromAccess, VK_ACCESS_SHADER_READ_BIT);
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &toRead);
    maskInitialized_ = true;
}

// A warm load: the cooked blocks into the mask (every level) and the cooked level kTopLevel, decoded, into the top image.
bool ShadowBake::recordWarmUpload(VkCommandBuffer cb, const mc2shadow::ShadowMaskAsset& asset) {
    const uint32_t topBytes = topSide_ * topSide_;
    const VkDeviceSize total = asset.blocks.size() + topBytes;
    if (!makeHostBuffer(transfer_, allocator_, device_, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, "shadow.bake.upload")) return false;
    std::vector<uint8_t> top(topBytes, 255);
    {   // level kTopLevel, in the chain right after the levels above it
        VkDeviceSize off = 0;
        for (uint32_t m = 0; m < kTopLevel; ++m) off += mc2shadow::bc4ChainBytes((std::max)(static_cast<uint32_t>(maskTexels_) >> m, 1u), 1);
        const uint32_t blocks = topSide_ / 4;
        for (uint32_t by = 0; by < blocks; ++by)
            for (uint32_t bx = 0; bx < blocks; ++bx) {
                std::array<uint8_t, 8> blk{};
                std::memcpy(blk.data(), asset.blocks.data() + off + (static_cast<size_t>(by) * blocks + bx) * 8, 8);
                const auto px = mc2shadow::decodeBc4Block(blk);
                for (uint32_t i = 0; i < 16; ++i) top[(static_cast<size_t>(by) * 4 + i / 4) * topSide_ + bx * 4 + i % 4] = px[i];
            }
    }
    if (!transfer_.upload(asset.blocks.data(), asset.blocks.size()) || !transfer_.upload(top.data(), topBytes, asset.blocks.size())) return false;
    VkImageMemoryBarrier pre[2];
    pre[0] = imageBarrier(maskImage_.get(), VK_IMAGE_ASPECT_COLOR_BIT, 0, maskMips_,
                          maskInitialized_ ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                          VK_ACCESS_TRANSFER_WRITE_BIT);
    pre[1] = imageBarrier(topR8_.get(), VK_IMAGE_ASPECT_COLOR_BIT, 0, topMips_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                          VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, pre);
    const std::vector<VkBufferImageCopy> lv = chainRegions();
    vkCmdCopyBufferToImage(cb, transfer_.get(), maskImage_.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, maskMips_, lv.data());
    VkBufferImageCopy tc{};
    tc.bufferOffset = asset.blocks.size();
    tc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    tc.imageExtent = {topSide_, topSide_, 1};
    vkCmdCopyBufferToImage(cb, transfer_.get(), topR8_.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &tc);
    recordTopLevelsMipsOnly(cb);
    recordMaskToRead(cb, false);
    return true;
}

// The top image's own mips and its sampled layout, without re-encoding (a warm load already has the mask's top levels).
void ShadowBake::recordTopLevelsMipsOnly(VkCommandBuffer cb) {
    for (uint32_t k = 1; k < topMips_; ++k) {
        VkImageMemoryBarrier toSrc = imageBarrier(topR8_.get(), VK_IMAGE_ASPECT_COLOR_BIT, k - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSrc);
        const int32_t from = static_cast<int32_t>((std::max)(topSide_ >> (k - 1), 1u)), to = static_cast<int32_t>((std::max)(topSide_ >> k, 1u));
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, k - 1, 0, 1};
        blit.srcOffsets[1] = {from, from, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, k, 0, 1};
        blit.dstOffsets[1] = {to, to, 1};
        vkCmdBlitImage(cb, topR8_.get(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, topR8_.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    }
    VkImageMemoryBarrier toRead[2];
    uint32_t n = 0;
    if (topMips_ > 1)
        toRead[n++] = imageBarrier(topR8_.get(), VK_IMAGE_ASPECT_COLOR_BIT, 0, topMips_ - 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT);
    toRead[n++] = imageBarrier(topR8_.get(), VK_IMAGE_ASPECT_COLOR_BIT, topMips_ - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, n, toRead);
    topInitialized_ = true;
}

// A retired fresh bake: the read-back chain becomes a cooked asset on disk (the next load of the same inputs uploads it instead of baking).
void ShadowBake::storeReadback() {
    if (!readbackPending_ || !transfer_) return;
    readbackPending_ = false;
    using clk = std::chrono::steady_clock;
    const clk::time_point t0 = clk::now();
    mc2shadow::ShadowMaskAsset asset;
    asset.texels = static_cast<uint32_t>(maskTexels_);
    asset.mips = maskMips_;
    asset.key = contentKey_;
    asset.blocks.resize(static_cast<size_t>(mc2shadow::bc4ChainBytes(asset.texels, asset.mips)));
    void* p = transfer_.map();
    if (!p) return;
    vmaInvalidateAllocation(allocator_, transfer_.allocation(), 0, VK_WHOLE_SIZE);
    std::memcpy(asset.blocks.data(), p, asset.blocks.size());
    transfer_.unmap();
    if (!diagInc_.empty()) {
        compareChains(diagInc_, asset.blocks);
        diagInc_.clear();
    }
    const double copyMs = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
    // The disk write (tens to hundreds of ms) runs off the render thread; the asset is a private copy.
    std::thread([dir = cacheDir_, a = std::move(asset), copyMs]() {
        const auto w0 = std::chrono::steady_clock::now();
        const bool ok = mc2shadow::storeCachedShadowMask(dir, a);
        mc2::fmt::print_err("[SHADOW_BAKE] cook cache store: {} key={:016x} mb={:.1f} render_thread_ms={:.2f} writer_ms={:.2f}\n", ok ? "ok" : "failed",
                            mc2::fmt::as_u64(a.key), static_cast<double>(a.blocks.size()) / 1048576.0, copyMs,
                            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - w0).count());
    }).detach();
}

}  // namespace mc2vk
#endif
