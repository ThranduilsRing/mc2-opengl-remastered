#pragma once
// Vulkan adapter of RenderCore/vk_barrier_batch.h: turns a BarrierBatch into ONE vkCmdPipelineBarrier2 on the context's sync2 route.
#include "../../RenderCore/vk_barrier_batch.h"
#include "vulkan_context.h"

#include <volk.h>

namespace mc2vk::sync {
// Records every barrier of `batch` in one call and clears it. Nothing is recorded for an empty batch.
inline void flush(const VulkanContext& ctx, VkCommandBuffer cb, BarrierBatch& batch) {
    if (batch.empty()) return;
    std::vector<VkImageMemoryBarrier2> images;
    std::vector<VkBufferMemoryBarrier2> buffers;
    images.reserve(batch.images().size());
    buffers.reserve(batch.buffers().size());
    for (const auto& r : batch.images()) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = r.before.stages;
        b.srcAccessMask = r.before.access;
        b.dstStageMask = r.after.stages;
        b.dstAccessMask = r.after.access;
        b.oldLayout = VkImageLayout(r.before.layout);
        b.newLayout = VkImageLayout(r.after.layout);
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = reinterpret_cast<VkImage>(r.image);
        b.subresourceRange = {VkImageAspectFlags(r.aspect), r.baseMip, r.mipCount, r.baseLayer, r.layerCount};
        images.push_back(b);
    }
    for (const auto& r : batch.buffers()) {
        VkBufferMemoryBarrier2 b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        b.srcStageMask = r.srcStages;
        b.srcAccessMask = r.srcAccess;
        b.dstStageMask = r.dstStages;
        b.dstAccessMask = r.dstAccess;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.buffer = reinterpret_cast<VkBuffer>(r.buffer);
        b.offset = r.offset;
        b.size = r.size;
        buffers.push_back(b);
    }
    VkMemoryBarrier2 mem{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    if (const MemoryBarrierRec* m = batch.memoryBarrier()) {
        mem.srcStageMask = m->srcStages;
        mem.srcAccessMask = m->srcAccess;
        mem.dstStageMask = m->dstStages;
        mem.dstAccessMask = m->dstAccess;
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &mem;
    }
    dep.bufferMemoryBarrierCount = static_cast<uint32_t>(buffers.size());
    dep.pBufferMemoryBarriers = buffers.data();
    dep.imageMemoryBarrierCount = static_cast<uint32_t>(images.size());
    dep.pImageMemoryBarriers = images.data();
    ctx.cmdPipelineBarrier2(cb, dep);
    batch.clear();
}

// One-barrier conveniences for the sites that have a single transition (an upload's UNDEFINED -> TRANSFER_DST -> SHADER_READ_ONLY).
inline void cmdImageBarrier(const VulkanContext& ctx, VkCommandBuffer cb, VkImage image, const VkImageSubresourceRange& range,
                            VkImageLayout from, VkPipelineStageFlags2 srcStages, VkAccessFlags2 srcAccess,
                            VkImageLayout to, VkPipelineStageFlags2 dstStages, VkAccessFlags2 dstAccess) {
    BarrierBatch batch;
    ImageBarrierRec r;
    r.image = reinterpret_cast<uint64_t>(image);
    r.aspect = range.aspectMask; r.baseMip = range.baseMipLevel; r.mipCount = range.levelCount;
    r.baseLayer = range.baseArrayLayer; r.layerCount = range.layerCount;
    r.before = {static_cast<uint32_t>(from), srcStages, srcAccess, srcAccess != 0};
    r.after = {static_cast<uint32_t>(to), dstStages, dstAccess, false};
    batch.image(r);
    flush(ctx, cb, batch);
}
inline void cmdMemoryBarrier(const VulkanContext& ctx, VkCommandBuffer cb, VkPipelineStageFlags2 srcStages, VkAccessFlags2 srcAccess,
                             VkPipelineStageFlags2 dstStages, VkAccessFlags2 dstAccess) {
    BarrierBatch batch;
    batch.memory(srcStages, srcAccess, dstStages, dstAccess);
    flush(ctx, cb, batch);
}
}  // namespace mc2vk::sync
