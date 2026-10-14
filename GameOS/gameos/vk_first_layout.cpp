// vk_first_layout.cpp -- see vk_first_layout.h.

#ifdef MC2_VULKAN_ISLAND

#include "vk_first_layout.h"

#include "vk_image_resource.h"
#include "vk_owned.h"
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
    const VkDevice device = ctx.device();
    if (device == VK_NULL_HANDLE || img.image.get() == VK_NULL_HANDLE) return false;

    mc2vk::OwnedCommandPool pool;
    mc2vk::OwnedFence fence;
    bool ok = false;
    bool submitted = false;   // the GPU owns the fence and the pool from the submit until the fence signals
    do {
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pci.queueFamilyIndex = ctx.graphicsFamily();
        if (!mc2vk::create(pool, device, [&](VkCommandPool* h) { return vkCreateCommandPool(device, &pci, nullptr, h); })) break;
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool.get();
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer cb = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(device, &ai, &cb) != VK_SUCCESS) break;
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (!mc2vk::create(fence, device, [&](VkFence* h) { return vkCreateFence(device, &fci, nullptr, h); })) break;

        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) break;

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
        if (vkEndCommandBuffer(cb) != VK_SUCCESS) break;

        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        if (vkQueueSubmit(ctx.graphicsQueue(), 1, &si, fence.get()) != VK_SUCCESS) break;
        submitted = true;
        const VkFence fenceHandle = fence.get();
        ok = vkWaitForFences(device, 1, &fenceHandle, VK_TRUE, 5ull * 1000 * 1000 * 1000) == VK_SUCCESS;
    } while (false);

    if (submitted && !ok) {
        // The GPU may still be executing the submit, and the fence and the pool are destroyed when this function returns: wait for the device first. A
        // hung device is turned into a lost one by the driver, which ends the wait.
        std::fprintf(stderr, "[VK_FIRST_LAYOUT] the first-layout fence did not signal in time; waiting for the device before releasing it (start=%d)\n", static_cast<int>(start));
        vkDeviceWaitIdle(device);
    }
    if (!ok) std::fprintf(stderr, "[VK_FIRST_LAYOUT] the first-layout submit failed (start=%d)\n", static_cast<int>(start));
    return ok;
}

#endif  // MC2_VULKAN_ISLAND
