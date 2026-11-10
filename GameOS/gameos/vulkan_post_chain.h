#pragma once
// vulkan_post_chain -- one command buffer, one depth/normal layout round trip, one render pass instance and one submit for the
// native post chain (static screen shadow, cloud/shore, fog). Each island records its draws into the shared chain instead of
// owning a submit; gosPostProcess::flushVkPostChain() ends and submits it once at the end of endScene.
// WHY: three submits, each with a depth ATT->SRO->ATT barrier pair and a color load/store, cost a fixed 0.4-0.8 ms of full-screen
// bandwidth and idle on the laptop (perf-to-1ms H10). Output is unchanged: same draws, same order, same blends.
#include <optional>
#include <cstdint>
#include <volk.h>
#include "vk_render_pass_timer.h"

namespace mc2vk::postchain {

struct Chain {
    VkCommandBuffer cb = VK_NULL_HANDLE;
    bool open = false;
    bool rpOpen = false;
    VkImage depth = VK_NULL_HANDLE;
    VkImage normal = VK_NULL_HANDLE;
    uint64_t* tickets[4] = {};
    int nTickets = 0;
    std::optional<vk_render_pass_timer::CancelUnlessSubmitted> guard;
};

inline Chain& chain() { static Chain c; return c; }

inline void imageBarrier(VkCommandBuffer cb, VkImage image, VkImageAspectFlags aspect,
                         VkImageLayout oldL, VkImageLayout newL, VkAccessFlags srcA, VkAccessFlags dstA,
                         VkPipelineStageFlags srcS, VkPipelineStageFlags dstS)
{
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = oldL; b.newLayout = newL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, 1, 0, 1};
    b.srcAccessMask = srcA; b.dstAccessMask = dstA;
    vkCmdPipelineBarrier(cb, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// Opens the chain on `cbCandidate` (the caller's per-slot buffer, its pool already reset by the caller) when it is not open
// yet. Returns the chain's buffer, or null when begin failed. `normalImage` may be null (fog does not sample it).
inline VkCommandBuffer acquire(VkCommandBuffer cbCandidate, VkImage depthImage, VkImage normalImage)
{
    Chain& c = chain();
    if (c.open) return c.cb;
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cbCandidate, &bi) != VK_SUCCESS) return VK_NULL_HANDLE;
    vk_render_pass_timer::Begin(cbCandidate, gos_render_pass_timer::Pass_Post);
    c.guard.emplace(gos_render_pass_timer::Pass_Post);
    c.cb = cbCandidate; c.open = true; c.rpOpen = false;
    c.depth = depthImage; c.normal = normalImage; c.nTickets = 0;
    imageBarrier(c.cb, c.depth, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                 VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                 VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    if (c.normal)
        imageBarrier(c.cb, c.normal, VK_IMAGE_ASPECT_COLOR_BIT,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    return c.cb;
}

inline void addTicket(uint64_t* t)
{
    Chain& c = chain();
    for (int i = 0; i < c.nTickets; ++i) if (c.tickets[i] == t) return;
    if (c.nTickets < 4) c.tickets[c.nTickets++] = t;
}

// Begins the one scene-color render pass of the chain unless it is already open. All post render passes are the same
// single-attachment LOAD/STORE pass over the shared color0, so any island's pass and framebuffer serve every draw.
inline void beginPass(VkRenderPass rp, VkFramebuffer fb, uint32_t w, uint32_t h)
{
    Chain& c = chain();
    if (c.rpOpen) return;
    VkRenderPassBeginInfo rbi{};
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = rp; rbi.framebuffer = fb;
    rbi.renderArea = {{0, 0}, {w, h}};
    vkCmdBeginRenderPass(c.cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    c.rpOpen = true;
}

}  // namespace mc2vk::postchain
