// vk_readback.cpp -- see vk_readback.h.
#ifdef MC2_VULKAN_ISLAND
#include "vk_readback.h"

#include <vk_mem_alloc.h>

#include "vulkan_context.h"

namespace mc2vk {

ReadbackToken ReadbackRing::request(VulkanContext& ctx, VkDeviceSize bytes, uint64_t tag, VkBuffer* dst) {
    if (dst) *dst = VK_NULL_HANDLE;
    if (bytes == 0) return {};
    const ReadbackToken tok = core_.request(tag);
    if (!tok) return {};
    OwnedBuffer& b = buf_[tok.slot];
    if (!b || cap_[tok.slot] < bytes) {
        // A free slot is idle (its consumer saw the ticket complete), so the old buffer can go now.
        b.reset();
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = bytes;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;   // random access = host-cached where the device has it
        if (!b.create(ctx.allocator(), ctx.device(), bci, aci, "readback.slot", static_cast<int>(tok.slot))) { core_.release(tok); return {}; }
        VmaAllocationInfo info{};
        vmaGetAllocationInfo(ctx.allocator(), b.allocation(), &info);
        if (!info.pMappedData) { b.reset(); core_.release(tok); return {}; }
        mapped_[tok.slot] = info.pMappedData;
        cap_[tok.slot] = bytes;
    }
    ++requests_;
    if (dst) *dst = b.get();
    return tok;
}

const void* ReadbackRing::poll(VulkanContext& ctx, ReadbackToken tok, uint64_t expectedTag, bool* staleOut) {
    if (staleOut) *staleOut = false;
    ctx.poll();
    const ReadbackState st = core_.poll(tok, ctx.tracker(), expectedTag);
    if (st == ReadbackState::Stale) {
        ++stale_;
        if (staleOut) *staleOut = true;
        core_.release(tok);
        return nullptr;
    }
    if (st == ReadbackState::Pending) return nullptr;
    vmaInvalidateAllocation(ctx.allocator(), buf_[tok.slot].allocation(), 0, VK_WHOLE_SIZE);   // non-coherent host-cached memory
    return mapped_[tok.slot];
}

const void* ReadbackRing::wait(VulkanContext& ctx, ReadbackToken tok, uint64_t expectedTag, uint64_t timeoutNs) {
    if (!tok) return nullptr;
    ctx.wait(core_.ticketOf(tok), timeoutNs, "readback_wait");
    return poll(ctx, tok, expectedTag);
}

}  // namespace mc2vk
#endif  // MC2_VULKAN_ISLAND
