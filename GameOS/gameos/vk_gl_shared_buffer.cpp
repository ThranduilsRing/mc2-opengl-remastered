// VK-INTEROP-BRIDGE-1 / water-perf STEP 1 -- VkGlSharedBuffer implementation.
// Transliterated from the validated probe tools/vk_gl_buffer_interop_probe/main.cpp
// buffer export sequence (254-290) + the semaphore export (282-290, identical to the
// image pair). GL-free. Mirrors vk_image_resource.cpp shape exactly (buffer analog).

#include "vk_gl_shared_buffer.h"

#ifdef MC2_VULKAN_ISLAND

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

void blog(const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::printf("[VK_GL_SHARED_BUFFER] %s\n", buf);
    std::fflush(stdout);
}

// probe main.cpp:119-124 / vk_image_resource.cpp:26-35 (buffer analog, same body).
bool findMemType(VkPhysicalDevice phys, uint32_t typeBits, VkMemoryPropertyFlags want,
                 uint32_t* out) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & want) == want) { *out = i; return true; }
    }
    return false;
}

} // namespace

bool vkGlSharedBufferCreate(VkGlSharedBuffer* r, VkDevice device, VkPhysicalDevice phys,
                            VkDeviceSize size, VkBufferUsageFlags usage, bool exportable) {
    if (!r || size == 0) return false;
    *r = VkGlSharedBuffer{};
    r->device = device;
    r->size = size; r->usage = usage; r->exportable = exportable;

    const VkExternalMemoryHandleTypeFlagBits memHandleType =
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

    // ---- create buffer (external handle type declared when exportable) (probe 255-262) ----
    VkExternalMemoryBufferCreateInfo embci{
        VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    embci.handleTypes = memHandleType;

    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    if (exportable) bci.pNext = &embci;
    bci.size = size;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &bci, nullptr, &r->buffer) != VK_SUCCESS) {
        blog("vkCreateBuffer failed"); vkGlSharedBufferDestroy(r, device); return false;
    }

    VkMemoryRequirements mr{};
    vkGetBufferMemoryRequirements(device, r->buffer, &mr);
    r->size = mr.size;  // report the actual allocation size (probe uses mr.size for import)

    uint32_t memIdx = 0;
    if (!findMemType(phys, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memIdx)) {
        blog("no DEVICE_LOCAL memory type"); vkGlSharedBufferDestroy(r, device); return false;
    }

    // dedicated allocation (probe 267-274); exported chain only when exportable.
    VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    ded.buffer = r->buffer;
    VkExportMemoryAllocateInfo exp{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
    exp.pNext = &ded;
    exp.handleTypes = memHandleType;

    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.pNext = exportable ? (void*)&exp : (void*)&ded;  // dedicated always; export only if asked
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = memIdx;
    if (vkAllocateMemory(device, &mai, nullptr, &r->memory) != VK_SUCCESS) {
        blog("vkAllocateMemory failed"); vkGlSharedBufferDestroy(r, device); return false;
    }
    if (vkBindBufferMemory(device, r->buffer, r->memory, 0) != VK_SUCCESS) {
        blog("vkBindBufferMemory failed"); vkGlSharedBufferDestroy(r, device); return false;
    }

    // ---- export memory handle (probe 275-280) ----
    if (exportable) {
        // volk.c is compiled without VK_USE_PLATFORM_WIN32_KHR -> fetch directly.
        auto pGetMemWin32 = (PFN_vkGetMemoryWin32HandleKHR)
            vkGetDeviceProcAddr(device, "vkGetMemoryWin32HandleKHR");
        if (!pGetMemWin32) {
            blog("vkGetMemoryWin32HandleKHR entry point null");
            vkGlSharedBufferDestroy(r, device); return false;
        }
        VkMemoryGetWin32HandleInfoKHR gh{VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR};
        gh.memory = r->memory;
        gh.handleType = memHandleType;
        if (pGetMemWin32(device, &gh, &r->memWin32) != VK_SUCCESS) {
            blog("vkGetMemoryWin32HandleKHR failed"); vkGlSharedBufferDestroy(r, device);
            return false;
        }
    }

    // ---- double-buffered binary semaphore pairs + per-frame fences ----
    // (identical to vk_image_resource.cpp:163-214; probe 282-290 for the single-slot form)
    const VkExternalSemaphoreHandleTypeFlagBits semHandleType =
        VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    PFN_vkGetSemaphoreWin32HandleKHR pGetSemWin32 = nullptr;
    if (exportable) {
        pGetSemWin32 = (PFN_vkGetSemaphoreWin32HandleKHR)
            vkGetDeviceProcAddr(device, "vkGetSemaphoreWin32HandleKHR");
        if (!pGetSemWin32) {
            blog("vkGetSemaphoreWin32HandleKHR entry point null");
            vkGlSharedBufferDestroy(r, device); return false;
        }
    }
    auto makeSem = [&](VkSemaphore* sem, HANDLE* win32, const char* tag, int i) -> bool {
        VkExportSemaphoreCreateInfo esci{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
        esci.handleTypes = semHandleType;
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (exportable) sci.pNext = &esci;
        if (vkCreateSemaphore(device, &sci, nullptr, sem) != VK_SUCCESS) {
            blog("vkCreateSemaphore(%s)[%d] failed", tag, i); return false;
        }
        if (exportable) {
            VkSemaphoreGetWin32HandleInfoKHR gi{
                VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR};
            gi.semaphore = *sem;
            gi.handleType = semHandleType;
            if (pGetSemWin32(device, &gi, win32) != VK_SUCCESS) {
                blog("vkGetSemaphoreWin32HandleKHR(%s)[%d] failed", tag, i); return false;
            }
        }
        return true;
    };
    for (int i = 0; i < 2; ++i) {
        if (!makeSem(&r->glToVkSem[i], &r->glToVkSemWin32[i], "glToVk", i)) {
            vkGlSharedBufferDestroy(r, device); return false;
        }
        if (!makeSem(&r->vkToGlSem[i], &r->vkToGlSemWin32[i], "vkToGl", i)) {
            vkGlSharedBufferDestroy(r, device); return false;
        }
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;  // frame 0 wait returns immediately
        if (vkCreateFence(device, &fci, nullptr, &r->frameFence[i]) != VK_SUCCESS) {
            blog("vkCreateFence[%d] failed", i); vkGlSharedBufferDestroy(r, device);
            return false;
        }
    }

    return true;
}

void vkGlSharedBufferDestroy(VkGlSharedBuffer* r, VkDevice device) {
    if (!r) return;
    for (int i = 0; i < 2; ++i) {
        if (r->frameFence[i])     vkDestroyFence(device, r->frameFence[i], nullptr);
        if (r->glToVkSem[i])      vkDestroySemaphore(device, r->glToVkSem[i], nullptr);
        if (r->vkToGlSem[i])      vkDestroySemaphore(device, r->vkToGlSem[i], nullptr);
        // This resource owns its exported handles; the GL importer duplicated them.
        if (r->glToVkSemWin32[i]) CloseHandle(r->glToVkSemWin32[i]);
        if (r->vkToGlSemWin32[i]) CloseHandle(r->vkToGlSemWin32[i]);
    }
    if (r->buffer)   vkDestroyBuffer(device, r->buffer, nullptr);
    if (r->memory)   vkFreeMemory(device, r->memory, nullptr);
    if (r->memWin32) CloseHandle(r->memWin32);
    *r = VkGlSharedBuffer{};
}

VkFrameSync vkGlSharedBufferBeginFrame(VkGlSharedBuffer* r) {
    const uint32_t p = r->frameParity;
    vkWaitForFences(r->device, 1, &r->frameFence[p], VK_TRUE, UINT64_MAX);
    vkResetFences(r->device, 1, &r->frameFence[p]);
    VkFrameSync s{r->frameFence[p], r->glToVkSem[p], r->vkToGlSem[p], p};
    r->frameParity ^= 1u;
    return s;
}

#endif // MC2_VULKAN_ISLAND
