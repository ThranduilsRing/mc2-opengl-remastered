// vk_first_layout.h -- the first layout of the images the Vulkan islands share.
//
// WHAT: every island declares the layout of the shared scene images (colour, normal, object id, depth) from its first frame: COLOR_ATTACHMENT_OPTIMAL or
// DEPTH_STENCIL_ATTACHMENT_OPTIMAL as the `initialLayout` of its render pass and as the `oldLayout` of the present composite's barriers. A VkImageResource
// is created UNDEFINED and nothing else gives it a layout, so those declarations were false until the first frame had written the image, and the object-id
// image (which no island writes) had undefined contents for the whole session.
// WHY: the validation layer reports it as VUID-vkCmdDraw-None-09600; on a driver that uses the layout to choose a compression state the first read is
// undefined behaviour, not only a warning. One submit at creation makes every later declaration true.
#ifndef MC2_VK_FIRST_LAYOUT_H
#define MC2_VK_FIRST_LAYOUT_H

#ifdef MC2_VULKAN_ISLAND

#include <volk.h>

struct VkImageResource;
namespace mc2vk { class VulkanContext; }

enum class SharedImageStart {
    ColorAttachment,          // contents are written by the first owner before anything reads them
    ColorAttachmentCleared,   // a UINT image no island writes: cleared to 0 ("no object") so a reader sees defined data
    DepthStencilAttachment,   // depth and stencil aspects
};

// Transitions `img` from UNDEFINED to the steady layout of `start` (and clears it where `start` says so) with synchronization2 barriers in one
// submit that is complete before this returns. False on any Vulkan failure, with everything the call created released.
bool mc2vkGiveSharedImageFirstLayout(const mc2vk::VulkanContext& ctx, const VkImageResource& img, SharedImageStart start);

#endif  // MC2_VULKAN_ISLAND
#endif  // MC2_VK_FIRST_LAYOUT_H
