// VK-INTEROP-BRIDGE-1 / T1 -- VkImageResource: the GL-free, VK-owned core of the
// composable zero-copy GL<->VK image interop SEAM (see .claude/VK-INTEROP-BRIDGE-1-PLAN.md,
// "Strategic reshape of the abstraction"). This is the reusable, raw-allocated,
// export-capable VkImage + its VK-side double-buffered sync objects. It survives
// UNCHANGED into the future VK<->VK-chain slice (no GL types here at all).
//
// Raw allocation (vkAllocateMemory + VkExportMemoryAllocateInfo + dedicated) is
// MANDATORY -- NEVER vmaCreateImage -- because only a raw dedicated allocation is
// external-export-capable (blocker #1). The whole TU compiles to nothing unless
// MC2_VULKAN_ISLAND is defined (same discipline as the other island TUs).

#ifndef MC2_VK_IMAGE_RESOURCE_H
#define MC2_VK_IMAGE_RESOURCE_H

#ifdef MC2_VULKAN_ISLAND

// Win32 external-handle structs + KHR entrypoints are gated behind this macro in
// both vulkan_core.h and volk (spike main.cpp:31 does the same before <vulkan.h>).
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <volk.h>
#include <windows.h>  // HANDLE
#include <cstdint>

// The reusable, GL-free VK-owned image + its VK-side sync objects.
struct VkImageResource {
    VkImage           image   = VK_NULL_HANDLE;
    VkDeviceMemory    memory  = VK_NULL_HANDLE;   // raw vkAllocateMemory (dedicated, exportable)
    VkDeviceSize      memSize = 0;
    VkImageView       view    = VK_NULL_HANDLE;
    uint32_t          w = 0, h = 0;
    VkFormat          format  = VK_FORMAT_UNDEFINED;
    VkImageUsageFlags usage   = 0;
    bool              exportable = false;         // created with VkExternalMemoryImageCreateInfo
    HANDLE            memWin32 = nullptr;          // valid only if exportable

    // Double-buffered binary semaphore PAIRS + per-frame fences (blocker #4 -- the
    // L5 present-semaphore-reuse fix). Frame N uses slot [frameParity]; before slot
    // [parity] is reused two frames later, frameFence[parity] is waited so the prior
    // submit retired and the binary semaphores are un-signalled + safe to re-signal.
    //
    // ITEM-2 (VK-RUNG8-ITEM2): these are now plain (NON-exported) binary semaphores.
    // Their Win32 export half (glToVkSemWin32/vkToGlSemWin32 + vkGetSemaphoreWin32HandleKHR)
    // was the exportable machinery that only the deleted GL<->VK signal/wait handoff
    // needed (glInteropView imported them); item 6's static-shadow import needs the
    // exportable IMAGE only. The binary semaphores + VkFrameSync stay as the shared
    // all-island resource ABI (returned by beginFrame; the direct-chain islands read only
    // fence+parity, nobody signals/waits these semaphores anymore).
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

// Raw-allocated exportable image, transliterated from tools/vk_gl_interop_spike/
// main.cpp:395-440 (NOT vmaCreateImage). When exportable=true the image gets a
// VkExternalMemoryImageCreateInfo{OPAQUE_WIN32}, a dedicated + exported allocation, and
// an exported Win32 handle for the MEMORY (item 6's GL image import). The semaphores are
// always plain binary (item 2 deleted their Win32 export). Returns false fail-soft on any
// error (nothing to clean up on the caller side; the partial resource is torn down
// internally before returning false).
bool vkImageResourceCreate(VkImageResource* r, VkDevice device, VkPhysicalDevice phys,
                           uint32_t w, uint32_t h, VkFormat format,
                           VkImageUsageFlags usage, bool exportable);

void vkImageResourceDestroy(VkImageResource* r, VkDevice device);

// Advance to the frame's parity slot; wait+reset its fence; return the sync trio.
VkFrameSync vkImageResourceBeginFrame(VkImageResource* r);

#endif // MC2_VULKAN_ISLAND
#endif // MC2_VK_IMAGE_RESOURCE_H
