// Dynamic rendering helpers (VK roadmap #3): the scene passes begin/end through VK_KHR_dynamic_rendering (core 1.3 or the KHR route the device
// profile chose; the facility is required, see vk_device_profile.h) instead of a VkRenderPass + VkFramebuffer pair. A pass that used to leave its
// attachments in the same layout it found them in (initialLayout == finalLayout == *_ATTACHMENT_OPTIMAL) needs nothing but the stage/access
// dependency its VkSubpassDependency carried; passes with a real layout change (UNDEFINED -> READ_ONLY) record their own image barrier.
#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>

#include "vulkan_context.h"

namespace mc2vk::dynrender {

inline bool formatHasStencil(VkFormat f) {
    return f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D16_UNORM_S8_UINT ||
           f == VK_FORMAT_S8_UINT;
}
inline bool formatHasDepth(VkFormat f) { return f != VK_FORMAT_S8_UINT && f != VK_FORMAT_UNDEFINED; }

inline VkRenderingAttachmentInfo attachment(VkImageView view, VkImageLayout layout, VkAttachmentLoadOp load, VkAttachmentStoreOp store,
                                            const VkClearValue* clear = nullptr) {
    VkRenderingAttachmentInfo a{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    a.imageView = view; a.imageLayout = layout;
    a.resolveMode = VK_RESOLVE_MODE_NONE;
    a.loadOp = load; a.storeOp = store;
    if (clear) a.clearValue = *clear;
    return a;
}

// Pipeline-side description of a pass: attach to VkGraphicsPipelineCreateInfo::pNext, leave renderPass VK_NULL_HANDLE. The caller keeps the format array
// alive until the create returns (vk_render_infra copies it for hot reload).
inline VkPipelineRenderingCreateInfo pipelineInfo(uint32_t colorCount, const VkFormat* colorFormats, VkFormat depthStencil) {
    VkPipelineRenderingCreateInfo r{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    r.colorAttachmentCount = colorCount; r.pColorAttachmentFormats = colorFormats;
    r.depthAttachmentFormat = formatHasDepth(depthStencil) ? depthStencil : VK_FORMAT_UNDEFINED;
    r.stencilAttachmentFormat = formatHasStencil(depthStencil) ? depthStencil : VK_FORMAT_UNDEFINED;
    return r;
}

// One vkCmdBeginRendering: colour attachments (may be none), optional depth (+ the same view as stencil when its format has a stencil aspect, with the
// same load/store ops the old attachment description gave both aspects).
struct Pass {
    VkRect2D area{};
    uint32_t colorCount = 0;
    VkRenderingAttachmentInfo color[4]{};
    bool hasDepth = false;
    VkRenderingAttachmentInfo depth{};
    bool hasStencil = false;
    VkRenderingAttachmentInfo stencil{};

    void setDepth(const VkRenderingAttachmentInfo& d, VkFormat fmt) {
        hasDepth = true; depth = d;
        if (formatHasStencil(fmt)) { hasStencil = true; stencil = d; }
    }
};

inline void begin(VulkanContext& ctx, VkCommandBuffer cb, const Pass& p) {
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = p.area; ri.layerCount = 1;
    ri.colorAttachmentCount = p.colorCount; ri.pColorAttachments = p.colorCount ? p.color : nullptr;
    ri.pDepthAttachment = p.hasDepth ? &p.depth : nullptr;
    ri.pStencilAttachment = p.hasStencil ? &p.stencil : nullptr;
    ctx.cmdBeginRendering(cb, ri);
}
inline void end(VulkanContext& ctx, VkCommandBuffer cb) { ctx.cmdEndRendering(cb); }

// The stage/access dependency of the VkSubpassDependency a converted pass carried (an execution + memory barrier, no layout change).
inline void memoryBarrier(VulkanContext& ctx, VkCommandBuffer cb, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                          VkAccessFlags2 dstAccess) {
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = srcStage; mb.srcAccessMask = srcAccess; mb.dstStageMask = dstStage; mb.dstAccessMask = dstAccess;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1; dep.pMemoryBarriers = &mb;
    ctx.cmdPipelineBarrier2(cb, dep);
}

// A depth-only pass's layout move (the initial/final layout change of the VkRenderPass it replaces, which dynamic rendering does not do) as one
// synchronization2 image barrier on the single depth-aspect mip/layer the pass renders into.
inline void depthImageBarrier(VulkanContext& ctx, VkCommandBuffer cb, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                              VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage; b.srcAccessMask = srcAccess; b.dstStageMask = dstStage; b.dstAccessMask = dstAccess;
    b.oldLayout = oldLayout; b.newLayout = newLayout;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image; b.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1; dep.pImageMemoryBarriers = &b;
    ctx.cmdPipelineBarrier2(cb, dep);
}

// The colour-aspect counterpart (single mip/layer): the layout move of a colour attachment the old render pass did through initial/finalLayout.
inline void colorImageBarrier(VulkanContext& ctx, VkCommandBuffer cb, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                              VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage; b.srcAccessMask = srcAccess; b.dstStageMask = dstStage; b.dstAccessMask = dstAccess;
    b.oldLayout = oldLayout; b.newLayout = newLayout;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1; dep.pImageMemoryBarriers = &b;
    ctx.cmdPipelineBarrier2(cb, dep);
}

// A depth-only pass over one view: depth attachment only, `load` as given (CLEAR with `clearDepth`, or LOAD), STORE.
inline void beginDepthOnly(VulkanContext& ctx, VkCommandBuffer cb, VkImageView view, VkFormat fmt, uint32_t w, uint32_t h, VkAttachmentLoadOp load,
                           float clearDepth = 1.0f) {
    VkClearValue cv{}; cv.depthStencil = {clearDepth, 0};
    Pass p;
    p.area = {{0, 0}, {w, h}};
    p.setDepth(attachment(view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, load, VK_ATTACHMENT_STORE_OP_STORE, &cv), fmt);
    begin(ctx, cb, p);
}

// The shared scene pass in its LOAD form (colour0 + normal + depth/stencil, every aspect LOAD then STORE): what the render pass of the props, mech,
// sky, vfx, overlay... islands was. The memory barrier is the external dependency those passes carried (VkSubpassDependency, no layout change).
// A LOAD/STORE pass over `colorCount` colour views (a VK_NULL_HANDLE entry is an unused slot: the pipelines carry format UNDEFINED there, the
// VK_ATTACHMENT_UNUSED of the old subpass) and, when `depth` is not null, a depth/stencil view. No barrier: the caller records its own.
inline void beginLoad(VulkanContext& ctx, VkCommandBuffer cb, uint32_t w, uint32_t h, uint32_t colorCount, const VkImageView* colors, VkImageView depth,
                      VkFormat depthFormat) {
    Pass p;
    p.area = {{0, 0}, {w, h}};
    p.colorCount = colorCount;
    for (uint32_t i = 0; i < colorCount; ++i) {
        if (colors[i] != VK_NULL_HANDLE)
            p.color[i] = attachment(colors[i], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE);
        else
            p.color[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;   // imageView NULL: unused slot
    }
    if (depth != VK_NULL_HANDLE)
        p.setDepth(attachment(depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE), depthFormat);
    begin(ctx, cb, p);
}

// The external dependency of the scene LOAD render passes: the previous pass's colour/depth writes against this pass's attachment reads/writes.
inline void sceneLoadBarrier(VulkanContext& ctx, VkCommandBuffer cb) {
    memoryBarrier(ctx, cb, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                  VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
}

// Scene pass: barrier + beginLoad over (colour0, normal[, third slot]) + depth.
inline void beginSceneLoad(VulkanContext& ctx, VkCommandBuffer cb, uint32_t w, uint32_t h, VkImageView color, VkImageView normal, VkImageView depth,
                           VkFormat depthFormat) {
    sceneLoadBarrier(ctx, cb);
    const VkImageView views[2] = {color, normal};
    beginLoad(ctx, cb, w, h, 2, views, depth, depthFormat);
}

}  // namespace mc2vk::dynrender
