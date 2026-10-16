// vk_first_layout.cpp -- see vk_first_layout.h.

#ifdef MC2_VULKAN_ISLAND

#include "vk_first_layout.h"

#include "vk_image_resource.h"
#include "vk_one_shot.h"
#include "vulkan_context.h"

#include <cstdint>
#include <cstdio>

namespace {

VkImageMemoryBarrier2 imageBarrier(const VkImageResource& img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                                   VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                                   VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img.image.get();
    b.subresourceRange = {aspect, 0, 1, 0, 1};
    return b;
}

void record(const mc2vk::VulkanContext& ctx, VkCommandBuffer cb, const VkImageMemoryBarrier2& barrier) {
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    ctx.cmdPipelineBarrier2(cb, dep);
}

}  // namespace

bool mc2vkGiveSharedImageFirstLayout(const mc2vk::VulkanContext& ctx, const VkImageResource& img, SharedImageStart start) {
    if (ctx.device() == VK_NULL_HANDLE || img.image.get() == VK_NULL_HANDLE) return false;
    const bool ok = mc2vk::submitOneShot(ctx, [&](VkCommandBuffer cb) {
        constexpr VkPipelineStageFlags2 kColorOut = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        constexpr VkAccessFlags2 kColorRw = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        switch (start) {
            case SharedImageStart::ColorAttachment:
                record(ctx, cb, imageBarrier(img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                             VK_PIPELINE_STAGE_2_NONE, 0, kColorOut, kColorRw));
                break;
            case SharedImageStart::ColorAttachmentCleared: {
                record(ctx, cb, imageBarrier(img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                             VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT));
                const VkClearColorValue zero{};   // the four uint32 members: "no object"
                const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                vkCmdClearColorImage(cb, img.image.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &all);
                record(ctx, cb, imageBarrier(img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                             VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, kColorOut, kColorRw));
                break;
            }
            case SharedImageStart::DepthStencilAttachment:
                record(ctx, cb, imageBarrier(img, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                             VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                                             VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                                             VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT));
                break;
        }
    }, "first_layout_timeout");
    if (!ok) std::fprintf(stderr, "[VK_FIRST_LAYOUT] the first-layout submit failed (start=%d)\n", static_cast<int>(start));
    return ok;
}

#endif  // MC2_VULKAN_ISLAND
