// vk_upload.cpp -- see vk_upload.h.
#ifdef MC2_VULKAN_ISLAND
#include "vk_upload.h"

#include <cstdio>

#include <vk_mem_alloc.h>

#include "vulkan_context.h"

namespace mc2vk {

UploadBatch::Span UploadBatch::stage(VulkanContext& ctx, VkDeviceSize bytes) {
    Span out;
    if (bytes == 0) bytes = 1;
    ctx.frameRingBuffer();   // the ring exists from here on, so its slot size is known
    if (bytes <= ctx.frameRingRange() / 4) {
        const FrameAlloc a = ctx.frameAlloc(bytes, 16);
        if (a) { out.buffer = a.buffer; out.offset = a.offset; out.ptr = a.ptr; return out; }
    }
    // Larger than a quarter of a ring slot (or the ring is out of memory): a dedicated staging buffer that leaves with the frame.
    OwnedBuffer buf;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    aci.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (!buf.create(ctx.allocator(), ctx.device(), bci, aci, "upload.staging")) return out;
    VmaAllocationInfo info{};
    vmaGetAllocationInfo(ctx.allocator(), buf.allocation(), &info);
    if (!info.pMappedData) return out;
    out.buffer = buf.get();
    out.ptr = info.pMappedData;
    dedicated_.push_back(std::move(buf));
    ++dedicatedStages_;
    return out;
}

bool UploadBatch::build(const VulkanContext& ctx, OwnedCommandPool& pool, VkCommandBuffer& cb) {
    cb = VK_NULL_HANDLE;
    if (recorders_.empty()) return false;
    const VkDevice device = ctx.device();
    bool ok = false;
    do {
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pci.queueFamilyIndex = ctx.graphicsFamily();
        if (!create(pool, device, [&](VkCommandPool* h) { return vkCreateCommandPool(device, &pci, nullptr, h); })) break;
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool.get();
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device, &ai, &cb) != VK_SUCCESS) break;
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) break;
        for (auto& fn : recorders_) fn(cb);
        ok = vkEndCommandBuffer(cb) == VK_SUCCESS;
    } while (false);
    if (!ok) {
        dropped_ += recorders_.size();
        std::fprintf(stderr, "[VK_UPLOAD v1] the batch command buffer could not be built: %zu upload(s) dropped.\n", recorders_.size());
        std::fflush(stderr);
        cb = VK_NULL_HANDLE;
    } else {
        ++flushes_;
    }
    recorders_.clear();
    return ok;
}

}  // namespace mc2vk
#endif  // MC2_VULKAN_ISLAND
