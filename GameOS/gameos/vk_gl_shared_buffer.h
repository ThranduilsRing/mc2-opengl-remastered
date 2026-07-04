// VK-INTEROP-BRIDGE-1 / water-perf STEP 1 -- the BUFFER analog of the proven
// VkImageResource / GlInteropView IMAGE interop pair. Extracted verbatim (same API
// sequence) from the validated probe tools/vk_gl_buffer_interop_probe/main.cpp
// (committed 1425d896): VK ALLOCATES an exportable buffer, GL imports it via
// glNamedBufferStorageMemEXT and writes it, VK reads it GPU-side.
//
// This is JUST the reusable abstraction. It is NOT wired into the water stream yet
// (that is Seam A, the next step). Gate-OFF byte-identical trivially: a new TU that
// nothing calls by default.
//
// VkGlSharedBuffer     = the GL-free, VK-owned exportable BUFFER core (analog of
//                        VkImageResource): raw VK buffer + dedicated exportable
//                        VkDeviceMemory + exported Win32 handle. Survives UNCHANGED
//                        into a future VK<->VK-chain slice (no GL types here).
// GlInteropBufferView  = the GL-boundary adapter (analog of GlInteropView): imports
//                        the exportable VkGlSharedBuffer into GL as a GL buffer object,
//                        LUID match / fail-soft, GL->VK signal / VK->GL wait.
//
// Raw allocation (vkAllocateMemory + VkExportMemoryAllocateInfo + dedicated) is
// MANDATORY -- only a raw dedicated allocation is external-export-capable. The whole
// TU compiles to nothing unless MC2_VULKAN_ISLAND is defined (same discipline as the
// other island TUs).

#ifndef MC2_VK_GL_SHARED_BUFFER_H
#define MC2_VK_GL_SHARED_BUFFER_H

#ifdef MC2_VULKAN_ISLAND

#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <volk.h>
#include <windows.h>  // HANDLE
#include <cstdint>

// Reuse VkFrameSync (fence + signal + vk2gl + parity) from the image pair verbatim --
// the buffer path drives the same double-buffered semaphore/fence sync.
#include "vk_image_resource.h"  // VkFrameSync, findMemType discipline

// ---------------------------------------------------------------------------
// VkGlSharedBuffer -- the GL-free, VK-owned exportable buffer + its VK-side
// double-buffered sync objects. Analog of VkImageResource. (probe main.cpp:254-290)
struct VkGlSharedBuffer {
    VkBuffer          buffer  = VK_NULL_HANDLE;
    VkDeviceMemory    memory  = VK_NULL_HANDLE;   // raw vkAllocateMemory (dedicated, exportable)
    VkDeviceSize      size    = 0;
    VkBufferUsageFlags usage  = 0;
    bool              exportable = false;         // created with VkExternalMemoryBufferCreateInfo
    HANDLE            memWin32 = nullptr;          // valid only if exportable

    // Double-buffered binary semaphore pairs + per-frame fences, identical to
    // VkImageResource. glToVkSem: GL signals, VK submit WAITS. vkToGlSem (P1.5):
    // VK submit SIGNALS, GL (or a future VK consumer) WAITS -- a GPU hand-off.
    VkSemaphore       glToVkSem[2]      = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkSemaphore       vkToGlSem[2]      = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkFence           frameFence[2]     = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    HANDLE            glToVkSemWin32[2] = {nullptr, nullptr};
    HANDLE            vkToGlSemWin32[2] = {nullptr, nullptr};
    uint32_t          frameParity       = 0;

    VkDevice          device = VK_NULL_HANDLE;
};

// Raw-allocated exportable buffer, transliterated from probe main.cpp:254-290. When
// exportable the buffer gets a VkExternalMemoryBufferCreateInfo{OPAQUE_WIN32}, a
// dedicated + exported allocation, and exported Win32 handles for the memory and both
// semaphore slots. `usage` MUST include whatever the eventual GL producer / VK consumer
// needs (e.g. STORAGE_BUFFER | TRANSFER_SRC). Returns false fail-soft on any error (the
// partial resource is torn down internally before returning false).
bool vkGlSharedBufferCreate(VkGlSharedBuffer* r, VkDevice device, VkPhysicalDevice phys,
                            VkDeviceSize size, VkBufferUsageFlags usage, bool exportable);

void vkGlSharedBufferDestroy(VkGlSharedBuffer* r, VkDevice device);

// Advance to the frame's parity slot; wait+reset its fence; return the sync trio.
// Identical semantics to vkImageResourceBeginFrame.
VkFrameSync vkGlSharedBufferBeginFrame(VkGlSharedBuffer* r);

#endif // MC2_VULKAN_ISLAND
#endif // MC2_VK_GL_SHARED_BUFFER_H
