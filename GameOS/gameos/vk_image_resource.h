// VK-INTEROP-BRIDGE-1 / T1 -- VkImageResource: the GL-free, VK-owned core of the
// former composable zero-copy GL<->VK image interop SEAM. This is the reusable,
// raw-allocated VkImage + its VK-side double-buffered sync objects (no GL types here).
//
// VK-RUNG8-ITEM2: the EXPORTABLE half (VkExternalMemoryImageCreateInfo /
// VkExportMemoryAllocateInfo / vkGetMemoryWin32HandleKHR / memWin32 / export-sanity
// queries) was DELETED -- it became caller-free once item 6 retired the static-shadow
// substrate. Only the NON-exportable core survives (dedicated raw allocation, image
// view, binary sems + VkFrameSync). Removing the Win32 external-memory machinery is
// part of clearing the last MoltenVK/Metal-gate blocker. The whole TU compiles to
// nothing unless MC2_VULKAN_ISLAND is defined (same discipline as the other island TUs).

#ifndef MC2_VK_IMAGE_RESOURCE_H
#define MC2_VK_IMAGE_RESOURCE_H

#ifdef MC2_VULKAN_ISLAND

#include <volk.h>
#include <cstdint>

#include "../../RenderCore/FrameFenceCore.h"

#include "vk_owned.h"

// The reusable, GL-free VK-owned image + its VK-side sync objects. The resource owns every handle: vkImageResourceDestroy (or the destructor)
// releases them in the reverse of the order the members are declared, so the memory outlives the image bound to it and the fences go first. A
// resource is move-only; islands hold it by pointer or by value.
struct VkImageResource {
    mc2vk::OwnedDeviceMemory memory;               // raw vkAllocateMemory (dedicated); declared first, released last
    mc2vk::OwnedRawImage     image;
    VkDeviceSize      memSize = 0;
    mc2vk::OwnedImageView    view;
    uint32_t          w = 0, h = 0;
    VkFormat          format  = VK_FORMAT_UNDEFINED;
    VkImageUsageFlags usage   = 0;

    // Two fence slots serialize native image reuse. No interop semaphores remain.
    mc2vk::OwnedFence     frameFence[2];
    RenderCore::FrameFenceCore frames;             // parity (^=1 each frame) and whether the slot fences may still be waited (disabled by a failed CancelFrame)

    // Cached device handle so vkImageResourceBeginFrame (which takes only the
    // resource) can reset the fence without threading VkDevice through every call.
    VkDevice          device = VK_NULL_HANDLE;
};

// Native submission completion and slot parity, returned after waiting/resetting the fence.
struct VkFrameSync { VkFence fence; uint32_t parity; };

// Raw-allocated (dedicated) native image. Every caller uses Vulkan.
// Returns false fail-soft on any error (nothing to clean up on the caller side; the
// partial resource is torn down internally before returning false).
bool vkImageResourceCreate(VkImageResource* r, VkDevice device, VkPhysicalDevice phys,
                           uint32_t w, uint32_t h, VkFormat format,
                           VkImageUsageFlags usage);

void vkImageResourceDestroy(VkImageResource* r, VkDevice device);

// How many image resources hold a live image right now. The engine context checks it is zero when it tears the device down: a resource still alive then
// would release its image on a destroyed device.
int vkImageResourceLiveCount();

// Advance to the frame's parity slot; wait+reset its fence; return completion and parity.
VkFrameSync vkImageResourceBeginFrame(VkImageResource* r);
// Undoes a BeginFrame whose frame then recorded and submitted nothing: BeginFrame reset the slot's fence and only a submit signals it again, so a
// caller that returns early without submitting must call this, or the next BeginFrame on that slot waits forever. It re-creates the fence signaled
// (nothing is in flight on it: BeginFrame waited it) and gives the parity back.
// False when the fence could not be re-created (device loss, out of memory): the resource is then DISABLED, BeginFrame returns a null fence and waits on
// nothing, and the caller must stop using the resource (the water island disables itself).
bool vkImageResourceCancelFrame(VkImageResource* r, const VkFrameSync& sync);

#endif // MC2_VULKAN_ISLAND
#endif // MC2_VK_IMAGE_RESOURCE_H
