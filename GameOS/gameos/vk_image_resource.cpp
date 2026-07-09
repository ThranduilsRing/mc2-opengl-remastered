// VK-INTEROP-BRIDGE-1 / T1 -- VkImageResource implementation.
// Transliterated from tools/vk_gl_interop_spike/main.cpp export sequence (395-440),
// semaphore export (446-467), LUID unused here (that lives in the GL adapter). GL-free.

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

bool noGlInteropResourceStrictEnabled() {
    static int cached = -1;
    if (cached < 0) {
        const char* v = std::getenv("MC2_VK_NO_GL_INTEROP_RESOURCE_STRICT");
        cached = (v && v[0] == '1') ? 1 : 0;
    }
    return cached != 0;
}

void noGlInteropStrictAbort(const char* op) {
    std::fprintf(stderr, "[VK_NO_GL_INTEROP_STRICT] abort op=%s\n",
                 op ? op : "(unknown)");
    std::fflush(stderr);
    std::abort();
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
                           VkImageUsageFlags usage, bool exportable) {
    if (!r) return false;
    *r = VkImageResource{};
    r->device = device;
    r->w = w; r->h = h; r->format = format; r->usage = usage; r->exportable = exportable;

    if (exportable && noGlInteropResourceStrictEnabled())
        noGlInteropStrictAbort("vkImageResourceCreate(exportable)");

    const VkExternalMemoryHandleTypeFlagBits memHandleType =
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

    if (exportable) {
        // sanity: is this image config exportable with this handle type? (spike 371-393)
        VkPhysicalDeviceExternalImageFormatInfo eifi{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
        eifi.handleType = memHandleType;
        VkPhysicalDeviceImageFormatInfo2 ifi{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
        ifi.pNext = &eifi;
        ifi.format = format; ifi.type = VK_IMAGE_TYPE_2D;
        ifi.tiling = VK_IMAGE_TILING_OPTIMAL; ifi.usage = usage;
        VkExternalImageFormatProperties eifp{
            VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        VkImageFormatProperties2 ifp2{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
        ifp2.pNext = &eifp;
        VkResult fr = vkGetPhysicalDeviceImageFormatProperties2(phys, &ifi, &ifp2);
        if (fr != VK_SUCCESS) {
            ilog("format 0x%x not exportable (vkGetPhysicalDeviceImageFormatProperties2=%d)",
                 (unsigned)format, (int)fr);
            vkImageResourceDestroy(r, device); return false;
        }
        if (!(eifp.externalMemoryProperties.externalMemoryFeatures &
              VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT)) {
            ilog("OPAQUE_WIN32 export unsupported for format 0x%x", (unsigned)format);
            vkImageResourceDestroy(r, device); return false;
        }
    }

    // ---- create image (external handle type declared when exportable) (spike 395-410) ----
    VkExternalMemoryImageCreateInfo emici{
        VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    emici.handleTypes = memHandleType;

    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    if (exportable) ci.pNext = &emici;
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

    // dedicated allocation (spike 421-433); exported chain only when exportable.
    VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    ded.image = r->image;
    VkExportMemoryAllocateInfo exp{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
    exp.pNext = &ded;
    exp.handleTypes = memHandleType;

    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.pNext = exportable ? (void*)&exp : (void*)&ded;  // dedicated always; export only if asked
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = memIdx;
    if (vkAllocateMemory(device, &mai, nullptr, &r->memory) != VK_SUCCESS) {
        ilog("vkAllocateMemory failed"); vkImageResourceDestroy(r, device); return false;
    }
    if (vkBindImageMemory(device, r->image, r->memory, 0) != VK_SUCCESS) {
        ilog("vkBindImageMemory failed"); vkImageResourceDestroy(r, device); return false;
    }

    // ---- export memory handle (spike 435-440) ----
    if (exportable) {
        // volk.c is compiled without VK_USE_PLATFORM_WIN32_KHR, so its win32 PFN
        // globals are absent -> fetch these two entrypoints directly (spike 350-351).
        auto pGetMemWin32 = (PFN_vkGetMemoryWin32HandleKHR)
            vkGetDeviceProcAddr(device, "vkGetMemoryWin32HandleKHR");
        if (!pGetMemWin32) {
            ilog("vkGetMemoryWin32HandleKHR entry point null");
            vkImageResourceDestroy(r, device); return false;
        }
        VkMemoryGetWin32HandleInfoKHR gh{VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR};
        gh.memory = r->memory;
        gh.handleType = memHandleType;
        if (pGetMemWin32(device, &gh, &r->memWin32) != VK_SUCCESS) {
            ilog("vkGetMemoryWin32HandleKHR failed"); vkImageResourceDestroy(r, device);
            return false;
        }
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
    const VkExternalSemaphoreHandleTypeFlagBits semHandleType =
        VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    PFN_vkGetSemaphoreWin32HandleKHR pGetSemWin32 = nullptr;
    if (exportable) {
        pGetSemWin32 = (PFN_vkGetSemaphoreWin32HandleKHR)
            vkGetDeviceProcAddr(device, "vkGetSemaphoreWin32HandleKHR");
        if (!pGetSemWin32) {
            ilog("vkGetSemaphoreWin32HandleKHR entry point null");
            vkImageResourceDestroy(r, device); return false;
        }
    }
    // Helper: create one (optionally exportable) binary semaphore + export its Win32
    // handle. Same sequence for both directions (GL->VK glToVkSem and VK->GL vkToGlSem).
    auto makeSem = [&](VkSemaphore* sem, HANDLE* win32, const char* tag, int i) -> bool {
        VkExportSemaphoreCreateInfo esci{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
        esci.handleTypes = semHandleType;
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (exportable) sci.pNext = &esci;
        if (vkCreateSemaphore(device, &sci, nullptr, sem) != VK_SUCCESS) {
            ilog("vkCreateSemaphore(%s)[%d] failed", tag, i); return false;
        }
        if (exportable) {
            VkSemaphoreGetWin32HandleInfoKHR gi{
                VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR};
            gi.semaphore = *sem;
            gi.handleType = semHandleType;
            if (pGetSemWin32(device, &gi, win32) != VK_SUCCESS) {
                ilog("vkGetSemaphoreWin32HandleKHR(%s)[%d] failed", tag, i); return false;
            }
        }
        return true;
    };
    for (int i = 0; i < 2; ++i) {
        // GL->VK direction (glToVkSem).
        if (!makeSem(&r->glToVkSem[i], &r->glToVkSemWin32[i], "glToVk", i)) {
            vkImageResourceDestroy(r, device); return false;
        }
        // P1.5: VK->GL direction (vkToGlSem). The VK submit signals it, GL waits it,
        // replacing the CPU fence wait with a GPU semaphore hand-off.
        if (!makeSem(&r->vkToGlSem[i], &r->vkToGlSemWin32[i], "vkToGl", i)) {
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
        // Win32 handle ownership: this resource owns its exported handles; the GL
        // importer duplicated them (spike 482-483). Close ours here. No double-close.
        if (r->glToVkSemWin32[i]) CloseHandle(r->glToVkSemWin32[i]);
        if (r->vkToGlSemWin32[i]) CloseHandle(r->vkToGlSemWin32[i]);
    }
    if (r->view)     vkDestroyImageView(device, r->view, nullptr);
    if (r->image)    vkDestroyImage(device, r->image, nullptr);
    if (r->memory)   vkFreeMemory(device, r->memory, nullptr);
    if (r->memWin32) CloseHandle(r->memWin32);
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
