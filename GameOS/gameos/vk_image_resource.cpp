// VK-INTEROP-BRIDGE-1 / T1 -- VkImageResource implementation.
// GL-free, VK-only. VK-RUNG8-ITEM2 deleted the exportable IMAGE half (external-memory
// image create info + export memory alloc + vkGetMemoryWin32HandleKHR + memWin32 +
// export-sanity queries) after item 6 retired its last caller. The semaphores are plain
// binary; only the non-exportable dedicated allocation survives.

#include "vk_image_resource.h"

#ifdef MC2_VULKAN_ISLAND

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

void ilog(const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::printf("[VK_IMAGE_RESOURCE] %s\n", buf);
    std::fflush(stdout);
}

// spike main.cpp:162-172
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

VkImageAspectFlags aspectFor(VkFormat fmt) {
    if (fmt == VK_FORMAT_D32_SFLOAT || fmt == VK_FORMAT_D16_UNORM ||
        fmt == VK_FORMAT_X8_D24_UNORM_PACK32)
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    if (fmt == VK_FORMAT_D24_UNORM_S8_UINT || fmt == VK_FORMAT_D32_SFLOAT_S8_UINT)
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    return VK_IMAGE_ASPECT_COLOR_BIT;
}

} // namespace

bool vkImageResourceCreate(VkImageResource* r, VkDevice device, VkPhysicalDevice phys,
                           uint32_t w, uint32_t h, VkFormat format,
                           VkImageUsageFlags usage) {
    if (!r) return false;
    *r = VkImageResource{};
    r->device = device;
    r->w = w; r->h = h; r->format = format; r->usage = usage;

    // ---- create image (VK-only, non-exportable) ----
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &ci, nullptr, &r->image) != VK_SUCCESS) {
        ilog("vkCreateImage failed"); vkImageResourceDestroy(r, device); return false;
    }

    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(device, r->image, &mr);
    r->memSize = mr.size;

    uint32_t memIdx = 0;
    if (!findMemType(phys, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memIdx)) {
        ilog("no DEVICE_LOCAL memory type"); vkImageResourceDestroy(r, device); return false;
    }

    // dedicated allocation (spike 421-433). VK 1.1 core (VkMemoryDedicatedAllocateInfo).
    VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    ded.image = r->image;

    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.pNext = &ded;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = memIdx;
    if (vkAllocateMemory(device, &mai, nullptr, &r->memory) != VK_SUCCESS) {
        ilog("vkAllocateMemory failed"); vkImageResourceDestroy(r, device); return false;
    }
    if (vkBindImageMemory(device, r->image, r->memory, 0) != VK_SUCCESS) {
        ilog("vkBindImageMemory failed"); vkImageResourceDestroy(r, device); return false;
    }

    // ---- image view ----
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = r->image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = {aspectFor(format), 0, 1, 0, 1};
    if (vkCreateImageView(device, &vci, nullptr, &r->view) != VK_SUCCESS) {
        ilog("vkCreateImageView failed"); vkImageResourceDestroy(r, device); return false;
    }

    // ---- double-buffered binary semaphore pairs + per-frame fences (blocker #4) ----
    // ITEM-2: plain (NON-exported) binary semaphores. Their Win32 export half was the
    // exportable machinery only the deleted GL<->VK signal/wait handoff imported; nobody
    // signals/waits these now. Kept as the shared all-island beginFrame ABI (fence+parity).
    auto makeSem = [&](VkSemaphore* sem, const char* tag, int i) -> bool {
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (vkCreateSemaphore(device, &sci, nullptr, sem) != VK_SUCCESS) {
            ilog("vkCreateSemaphore(%s)[%d] failed", tag, i); return false;
        }
        return true;
    };
    for (int i = 0; i < 2; ++i) {
        if (!makeSem(&r->glToVkSem[i], "glToVk", i)) {
            vkImageResourceDestroy(r, device); return false;
        }
        if (!makeSem(&r->vkToGlSem[i], "vkToGl", i)) {
            vkImageResourceDestroy(r, device); return false;
        }
        // Fence starts SIGNALED so the first vkImageResourceBeginFrame wait returns
        // immediately (no submit has retired on this slot yet).
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (vkCreateFence(device, &fci, nullptr, &r->frameFence[i]) != VK_SUCCESS) {
            ilog("vkCreateFence[%d] failed", i); vkImageResourceDestroy(r, device);
            return false;
        }
    }

    return true;
}

void vkImageResourceDestroy(VkImageResource* r, VkDevice device) {
    if (!r) return;
    for (int i = 0; i < 2; ++i) {
        if (r->frameFence[i])     vkDestroyFence(device, r->frameFence[i], nullptr);
        if (r->glToVkSem[i])      vkDestroySemaphore(device, r->glToVkSem[i], nullptr);
        if (r->vkToGlSem[i])      vkDestroySemaphore(device, r->vkToGlSem[i], nullptr);
    }
    if (r->view)     vkDestroyImageView(device, r->view, nullptr);
    if (r->image)    vkDestroyImage(device, r->image, nullptr);
    if (r->memory)   vkFreeMemory(device, r->memory, nullptr);
    *r = VkImageResource{};
}

VkFrameSync vkImageResourceBeginFrame(VkImageResource* r) {
    const uint32_t p = r->frameParity;
    // Blocker #4: guarantee the prior submit on this slot retired before its binary
    // semaphore is re-signalled. Fence created SIGNALED so frame 0 does not block.
    vkWaitForFences(r->device, 1, &r->frameFence[p], VK_TRUE, UINT64_MAX);
    vkResetFences(r->device, 1, &r->frameFence[p]);
    // signal = GL->VK sem (submit waits it); vk2gl = VK->GL sem (submit signals it, P1.5).
    VkFrameSync s{r->frameFence[p], r->glToVkSem[p], r->vkToGlSem[p], p};
    r->frameParity ^= 1u;
    return s;
}

#endif // MC2_VULKAN_ISLAND
