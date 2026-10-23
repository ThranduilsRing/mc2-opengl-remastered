#pragma once
#include "../../RenderCore/vk_resource_graph.h"
#include <volk.h>

namespace mc2vk::graph {
struct Binding {
    VkImage image;
    VkImageSubresourceRange range;
};
inline void recordBarriers(VkCommandBuffer cb, const Plan& plan, unsigned pass,
                           const std::vector<Binding>& bindings) {
    for (const auto& edge : plan.barriers[pass]) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = edge.before.access;
        b.dstAccessMask = edge.after.access;
        b.oldLayout = VkImageLayout(edge.before.layout);
        b.newLayout = VkImageLayout(edge.after.layout);
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = bindings[edge.image].image;
        b.subresourceRange = bindings[edge.image].range;
        vkCmdPipelineBarrier(cb, edge.before.stages ? edge.before.stages : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             edge.after.stages, 0, 0, nullptr, 0, nullptr, 1, &b);
    }
}
} // namespace mc2vk::graph
