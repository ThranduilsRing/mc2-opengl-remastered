// VK-INTEROP-BRIDGE-1 / T1 -- VkImageResource implementation.
// GL-free, VK-only. VK-RUNG8-ITEM2 deleted the exportable IMAGE half (external-memory
// image create info + export memory alloc + vkGetMemoryWin32HandleKHR + memWin32 +
// export-sanity queries) after item 6 retired its last caller. The semaphores are plain
// binary; only the non-exportable dedicated allocation survives.

#include "vk_image_resource.h"
#include "diagnostic_trace.h"
#include "mc2fmt.h"

#ifdef MC2_VULKAN_ISLAND

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

// Resources whose image is alive; see vkImageResourceLiveCount().
std::atomic<int> g_liveImages{0};

void ilog(const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    mc2::fmt::print_out("[VK_IMAGE_RESOURCE] {}\n", mc2::fmt::safe(buf));
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
                           VkImageUsageFlags usage, std::string_view name) {
    if (!r) return false;
    if (r->image) g_liveImages.fetch_sub(1, std::memory_order_relaxed);   // an overwritten live resource is released here
    *r = VkImageResource{};
    r->device = device;
    r->w = w; r->h = h; r->format = format; r->usage = usage;

    // A failure below leaves the partial resource in `r`: release it so the caller sees an empty resource.
    auto fail = [&](const char* what) {
        ilog("%s failed", what);
        vkImageResourceDestroy(r, device);
        return false;
    };

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
    if (!mc2vk::create(r->image, device, [&](VkImage* out) { return vkCreateImage(device, &ci, nullptr, out); })) return fail("vkCreateImage");
    g_liveImages.fetch_add(1, std::memory_order_relaxed);

    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(device, r->image.get(), &mr);
    r->memSize = mr.size;

    uint32_t memIdx = 0;
    if (!findMemType(phys, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memIdx)) return fail("no DEVICE_LOCAL memory type");

    // dedicated allocation (spike 421-433). VK 1.1 core (VkMemoryDedicatedAllocateInfo).
    VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    ded.image = r->image.get();

    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.pNext = &ded;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = memIdx;
    if(name.find("shadow")!=name.npos && !mc2vk::infra::admit(mr.size)) return fail("shadow residency admission");
    if (!mc2vk::create(r->memory, device, [&](VkDeviceMemory* out) { return vkAllocateMemory(device, &mai, nullptr, out); })) return fail("vkAllocateMemory");
    mc2vk::infra::allocation(reinterpret_cast<uint64_t>(r->memory.get()), mr.size,
        name, true);
    if (vkBindImageMemory(device, r->image.get(), r->memory.get(), 0) != VK_SUCCESS) return fail("vkBindImageMemory");

    // ---- image view ----
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = r->image.get();
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = {aspectFor(format), 0, 1, 0, 1};
    if (!mc2vk::create(r->view, device, [&](VkImageView* out) { return vkCreateImageView(device, &vci, nullptr, out); })) return fail("vkCreateImageView");

    // ---- double-buffered binary semaphore pairs + per-frame fences (blocker #4) ----
    // Fence starts SIGNALED so the first vkImageResourceBeginFrame wait returns
    // immediately (no submit has retired on this slot yet).
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (int i = 0; i < 2; ++i) {
        if (!mc2vk::create(r->frameFence[i], device, [&](VkFence* out) { return vkCreateFence(device, &fci, nullptr, out); })) return fail("vkCreateFence");
    }

    return true;
}

// The caller idles the GPU first when the resource may be in flight (every island teardown does). The owners captured the device they were made on, so the
// argument is not read; it stays so the islands' teardown call sites are unchanged.
void vkImageResourceDestroy(VkImageResource* r, VkDevice) {
    if (!r) return;
    if (r->image) {
        const int live = g_liveImages.fetch_sub(1, std::memory_order_relaxed) - 1;
        if (mc2_diag::tagRequested("VK_SHUTDOWN")) ilog("destroy image owner %ux%u fmt=%d; %d alive after.", r->w, r->h, static_cast<int>(r->format), live);
    }
    // Moved into a local, so the destructor releases the owners in reverse declaration order (a member-wise move-assignment would release forward:
    // the memory before the image bound to it).
    { auto dying = std::move(*r); (void)dying; }
    *r = VkImageResource{};
}

int vkImageResourceLiveCount() { return g_liveImages.load(std::memory_order_relaxed); }

VkFrameSync vkImageResourceBeginFrame(VkImageResource* r) {
    const RenderCore::FrameFenceCore::Begin slot = r->frames.begin();
    // A disabled resource (a failed CancelFrame) has a reset fence nothing will signal: wait on nothing and hand back a null fence.
    if (!slot.mayWait) return VkFrameSync{VK_NULL_HANDLE, slot.slot};
    const uint32_t p = slot.slot;
    // Blocker #4: guarantee the prior submit on this slot retired before its binary
    // semaphore is re-signalled. Fence created SIGNALED so frame 0 does not block.
    const VkFence fence = r->frameFence[p].get();
    vkWaitForFences(r->device, 1, &fence, VK_TRUE, UINT64_MAX);
    vkResetFences(r->device, 1, &fence);
    return VkFrameSync{r->frameFence[p].get(), p};
}

bool vkImageResourceCancelFrame(VkImageResource* r, const VkFrameSync& sync) {
    if (!r || sync.parity > 1u || !sync.fence) return false;
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    const VkDevice device = r->device;
    // Create beside the old fence and swap on success: a failed vkCreateFence (device loss, out of memory) must not empty the owner.
    mc2vk::OwnedFence fresh;
    const bool recreated = mc2vk::create(fresh, device, [&](VkFence* out) { return vkCreateFence(device, &fci, nullptr, out); });
    if (recreated) r->frameFence[sync.parity] = std::move(fresh);
    r->frames.cancel(sync.parity, recreated);   // on failure the resource is disabled: BeginFrame never waits the reset, unsignalled old fence
    return recreated;
}

#endif // MC2_VULKAN_ISLAND
