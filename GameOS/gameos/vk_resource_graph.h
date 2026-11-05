#pragma once
#include "../../RenderCore/vk_resource_graph.h"
#include "vk_barrier_batch_vk.h"
#include <volk.h>

namespace mc2vk::graph {
struct Binding {
    VkImage image;
    VkImageSubresourceRange range;
    VkBuffer buffer = VK_NULL_HANDLE;   // set for a graph Image with buffer = true
};
// The sync2 emitter: every barrier of the pass goes out in ONE vkCmdPipelineBarrier2 (NONE source for a fresh image).
inline void recordBarriers2(const VulkanContext& ctx, VkCommandBuffer cb, const Plan& plan, unsigned pass,
                            const std::vector<Binding>& bindings) {
    sync::BarrierBatch batch;
    for (const auto& edge : plan.barriers[pass]) {
        const Binding& bind = bindings[edge.image];
        if (bind.buffer != VK_NULL_HANDLE) {
            sync::BufferBarrierRec r;
            r.buffer = reinterpret_cast<uint64_t>(bind.buffer);
            r.srcStages = edge.before.stages; r.srcAccess = edge.before.access;
            r.dstStages = edge.after.stages; r.dstAccess = edge.after.access;
            batch.buffer(r);
            continue;
        }
        sync::ImageBarrierRec r;
        r.image = reinterpret_cast<uint64_t>(bind.image);
        r.aspect = bind.range.aspectMask;
        r.baseMip = bind.range.baseMipLevel + edge.baseMip;
        r.mipCount = edge.mipCount;
        r.baseLayer = bind.range.baseArrayLayer;
        r.layerCount = bind.range.layerCount;
        r.before = edge.before;
        r.after = edge.after;
        batch.image(r);
    }
    sync::flush(ctx, cb, batch);
}
// The legacy emitter (one vkCmdPipelineBarrier per edge); kept for the SMAA consumer until it moves to recordBarriers2.
inline void recordBarriers(VkCommandBuffer cb, const Plan& plan, unsigned pass,
                           const std::vector<Binding>& bindings) {
    for (const auto& edge : plan.barriers[pass]) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = VkAccessFlags(edge.before.access);
        b.dstAccessMask = VkAccessFlags(edge.after.access);
        b.oldLayout = VkImageLayout(edge.before.layout);
        b.newLayout = VkImageLayout(edge.after.layout);
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = bindings[edge.image].image;
        b.subresourceRange = bindings[edge.image].range;
        vkCmdPipelineBarrier(cb, edge.before.stages ? VkPipelineStageFlags(edge.before.stages) : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VkPipelineStageFlags(edge.after.stages), 0, 0, nullptr, 0, nullptr, 1, &b);
    }
}
} // namespace mc2vk::graph
