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

// The reusable, GL-free VK-owned image + its VK-side sync objects.
struct VkImageResource {
    VkImage           image   = VK_NULL_HANDLE;
    VkDeviceMemory    memory  = VK_NULL_HANDLE;   // raw vkAllocateMemory (dedicated)
    VkDeviceSize      memSize = 0;
    VkImageView       view    = VK_NULL_HANDLE;
    uint32_t          w = 0, h = 0;
    VkFormat          format  = VK_FORMAT_UNDEFINED;
    VkImageUsageFlags usage   = 0;

    // Double-buffered binary semaphore PAIRS + per-frame fences (blocker #4 -- the
    // L5 present-semaphore-reuse fix). Frame N uses slot [frameParity]; before slot
    // [parity] is reused two frames later, frameFence[parity] is waited so the prior
    // submit retired and the binary semaphores are un-signalled + safe to re-signal.
    //
    // ITEM-2 (VK-RUNG8-ITEM2): these are plain (NON-exported) binary semaphores. Their
    // Win32 export half was the exportable machinery that only the deleted GL<->VK signal/
    // wait handoff needed; both it and (this rung) the exportable IMAGE half are gone. The
    // binary semaphores + VkFrameSync stay as the shared all-island resource ABI (returned
    // by beginFrame; the direct-chain islands read only fence+parity, nobody signals/waits
    // these semaphores anymore).
    //
    // glToVkSem == the GL->VK direction slot; vkToGlSem == the VK->GL direction slot.
    VkSemaphore       glToVkSem[2]    = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkSemaphore       vkToGlSem[2]      = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkFence           frameFence[2]   = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    uint32_t          frameParity     = 0;        // ^=1 each frame; picks [frameParity]

    // Cached device handle so vkImageResourceBeginFrame (which takes only the
    // resource) can reset the fence without threading VkDevice through every call.
    VkDevice          device = VK_NULL_HANDLE;
};

// Frame sync handed back by beginFrame. Waits + resets the [parity] slot's fence so its
// binary semaphores are safe to re-signal, and returns the fence + slot semaphores +
// parity. The direct-chain islands read only `fence` (CPU serialization) and `parity`;
// `signal`/`vk2gl` are the slot's binary semaphores and are no longer signalled/waited by
// anyone (item 1 deleted the GL<->VK signal/wait handoff; item 8 deleted the present one).
// They remain in the struct as the shared all-island ABI.
struct VkFrameSync { VkFence fence; VkSemaphore signal; VkSemaphore vk2gl; uint32_t parity; };

// Raw-allocated (dedicated) NON-exportable VK image. VK-RUNG8-ITEM2 removed the
// exportable path; every surviving caller is VK-only. The semaphores are plain binary.
// Returns false fail-soft on any error (nothing to clean up on the caller side; the
// partial resource is torn down internally before returning false).
bool vkImageResourceCreate(VkImageResource* r, VkDevice device, VkPhysicalDevice phys,
                           uint32_t w, uint32_t h, VkFormat format,
                           VkImageUsageFlags usage);

void vkImageResourceDestroy(VkImageResource* r, VkDevice device);

// Advance to the frame's parity slot; wait+reset its fence; return the sync trio.
VkFrameSync vkImageResourceBeginFrame(VkImageResource* r);

#endif // MC2_VULKAN_ISLAND
#endif // MC2_VK_IMAGE_RESOURCE_H
