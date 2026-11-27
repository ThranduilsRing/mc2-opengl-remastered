// VK-INTEROP-BRIDGE-1 / T1 -- VkImageResource: the GL-free, VK-owned core of the
// former composable zero-copy GL<->VK image interop SEAM. This is the reusable,
// raw-allocated VkImage + its VK-side double-buffered sync objects (no GL types here).
//
// VK-RUNG8-ITEM2: the EXPORTABLE half (VkExternalMemoryImageCreateInfo /
// VkExportMemoryAllocateInfo / vkGetMemoryWin32HandleKHR / memWin32 / export-sanity
// queries) was DELETED -- it became caller-free once item 6 retired the static-shadow
// substrate. Only the NON-exportable core survives (dedicated raw allocation, image
// view, view). Removing the Win32 external-memory machinery is
// part of clearing the last MoltenVK/Metal-gate blocker. The whole TU compiles to
// the Vulkan renderer interface.

#ifndef MC2_VK_IMAGE_RESOURCE_H
#define MC2_VK_IMAGE_RESOURCE_H


#include <volk.h>
#include <cstdint>


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

    // Cached device handle.
    VkDevice          device = VK_NULL_HANDLE;
    void markDeferred() { if(memory) mc2vk::infra::defer(reinterpret_cast<uint64_t>(memory.get())); }
};

// Raw-allocated (dedicated) native image. Every caller uses Vulkan.
// Returns false fail-soft on any error (nothing to clean up on the caller side; the
// partial resource is torn down internally before returning false).
bool vkImageResourceCreate(VkImageResource* r, VkDevice device, VkPhysicalDevice phys,
                           uint32_t w, uint32_t h, VkFormat format,
                           VkImageUsageFlags usage, std::string_view name = "targets.image");

void vkImageResourceDestroy(VkImageResource* r, VkDevice device);

// How many image resources hold a live image right now. The engine context checks it is zero when it tears the device down: a resource still alive then
// would release its image on a destroyed device.
int vkImageResourceLiveCount();

#endif // MC2_VK_IMAGE_RESOURCE_H
