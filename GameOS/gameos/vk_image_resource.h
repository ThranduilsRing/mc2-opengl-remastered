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
    // submit retired and the binary semaphore is un-signalled + safe to re-signal.
    VkSemaphore       doneSem[2]      = {VK_NULL_HANDLE, VK_NULL_HANDLE};  // producer->consumer
    VkFence           frameFence[2]   = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    HANDLE            doneSemWin32[2] = {nullptr, nullptr};                // valid only if exportable
    uint32_t          frameParity     = 0;        // ^=1 each frame; picks [frameParity]

    // Cached device handle so vkImageResourceBeginFrame (which takes only the
    // resource) can reset the fence without threading VkDevice through every call.
    VkDevice          device = VK_NULL_HANDLE;
};

// VK<->VK chaining sync (meta D). Begins a frame on the [parity] slot: waits +
// resets that slot's fence so its binary semaphore is safe to re-signal, and hands
// back the fence to attach to the frame's submit + the semaphore the producer
// signals + the parity the GL side must mirror. Producer signals `signal`; the next
// VK consumer waits it via submit-wait / barrier -- NO GL, NO CPU stall between VK
// passes. This is the primitive the VK<->VK-chain slice reuses verbatim.
struct VkFrameSync { VkFence fence; VkSemaphore signal; uint32_t parity; };

// Raw-allocated exportable image, transliterated from tools/vk_gl_interop_spike/
// main.cpp:395-440 (NOT vmaCreateImage). When exportable=true the image gets a
// VkExternalMemoryImageCreateInfo{OPAQUE_WIN32}, a dedicated + exported allocation,
// and exported Win32 handles for the memory and both doneSem[] semaphores. Returns
// false fail-soft on any error (nothing to clean up on the caller side; the partial
// resource is torn down internally before returning false).
bool vkImageResourceCreate(VkImageResource* r, VkDevice device, VkPhysicalDevice phys,
                           uint32_t w, uint32_t h, VkFormat format,
                           VkImageUsageFlags usage, bool exportable);

void vkImageResourceDestroy(VkImageResource* r, VkDevice device);

// Advance to the frame's parity slot; wait+reset its fence; return the sync trio.
VkFrameSync vkImageResourceBeginFrame(VkImageResource* r);

#endif // MC2_VULKAN_ISLAND
#endif // MC2_VK_IMAGE_RESOURCE_H
