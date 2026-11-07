// vk_upload.cpp -- see vk_upload.h.
#include "vk_upload.h"

#include <cstdio>

#include <vk_mem_alloc.h>

#include "hitch_stats.h"
#include "vk_one_shot.h"
#include "vulkan_context.h"
#include "mc2fmt.h"

namespace mc2vk {

UploadBatch::Span UploadBatch::stage(VulkanContext& ctx, VkDeviceSize bytes) {
    Span out;
    if (bytes == 0) bytes = 1;
    mc2_hitch2::stats().uploadBytes.fetch_add(bytes, std::memory_order_relaxed);
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
    mc2_hitch2::noteStaged(bytes);
    return out;
}

bool UploadBatch::build(const VulkanContext& ctx, OwnedCommandPool& pool, VkCommandBuffer& cb) {
    cb = VK_NULL_HANDLE;
    if (recorders_.empty()) return false;
    bool ok = false;
    do {
        if (!beginTransientCommandBuffer(ctx, pool, cb)) break;
        for (auto& fn : recorders_) fn(cb);
        ok = vkEndCommandBuffer(cb) == VK_SUCCESS;
    } while (false);
    if (!ok) {
        dropped_ += recorders_.size();
        mc2::fmt::print_err("[VK_UPLOAD v1] the batch command buffer could not be built: {:d} upload(s) dropped.\n", mc2::fmt::as_size(recorders_.size()));
        std::fflush(stderr);
        cb = VK_NULL_HANDLE;
    } else {
        ++flushes_;
    }
    recorders_.clear();
    return ok;
}

}  // namespace mc2vk
