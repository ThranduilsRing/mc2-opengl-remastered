// vk_frame_ring.cpp -- see vk_frame_ring.h.
#include "vk_frame_ring.h"

#include <cstdio>
#include <algorithm>

#include <vk_mem_alloc.h>

#include "vulkan_context.h"
#include "mc2_diag_verbose.h"
#include "RenderCore/GpuResidencyPolicy.h"
#include "Gates/render_options.h"
#include "mc2fmt.h"

namespace mc2vk {

namespace {
VkDeviceSize initialPerSlot() { return (4ull*1024*1024)<<mc2::options::current().shadowQuality; }
constexpr VkBufferUsageFlags kUsage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
}  // namespace

bool FrameRing::grow(VulkanContext& ctx, VkDeviceSize perSlot) {
    const auto policy=RenderCore::gpuResidencyPolicy(mc2::options::current().shadowQuality);
    if(perSlot*(FrameRingCore::kSlots+1)>policy.textureUpload) {
        refusedCapacity_ = (std::max)(refusedCapacity_, perSlot);
        if (++policyRefusals_ == 1)
            mc2::fmt::print_err("[VK_RESIDENCY v1] refused=framering bytes={:d} policy={:d} fallback=skip_draw count=1 (repeats counted until recovery)\n",mc2::fmt::as_u64(perSlot*(FrameRingCore::kSlots+1)),mc2::fmt::as_u64(policy.textureUpload));
        return false;
    }
    OwnedBuffer fresh;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = perSlot * (FrameRingCore::kSlots + 1);   // one slot of tail: a dynamic descriptor range of perSlot bytes fits from any offset
    bci.usage = kUsage;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    aci.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    aci.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;   // ReBAR / SAM when the device exposes it
    if (!fresh.create(ctx.allocator(), ctx.device(), bci, aci, "framering")) return false;
    VmaAllocationInfo info{};
    vmaGetAllocationInfo(ctx.allocator(), fresh.allocation(), &info);
    if (!info.pMappedData) return false;
    if (buffer_) ctx.retire(std::move(buffer_), core_.capacity() * (FrameRingCore::kSlots + 1));   // this frame may already have handed out spans of it
    buffer_ = std::move(fresh);
    mapped_ = static_cast<uint8_t*>(info.pMappedData);
    core_.setCapacity(perSlot);
    ++growths_;
    if (policyRefusals_ && perSlot >= refusedCapacity_) {
        mc2::fmt::print_err("[VK_RESIDENCY v1] framering recovered refused_count={:d}\n", mc2::fmt::as_u64(static_cast<unsigned long long>(policyRefusals_)));
        policyRefusals_ = 0; refusedCapacity_ = 0;
    }
    MC2_DIAGFE(stderr, "[VK_FRAMERING v1] capacity=%llu bytes per slot (growth %u).\n", static_cast<unsigned long long>(perSlot), growths_);
    return true;
}

VkBuffer FrameRing::ensureBuffer(VulkanContext& ctx) {
    if (!buffer_ && !grow(ctx, initialPerSlot())) return VK_NULL_HANDLE;
    return buffer_.get();
}

FrameAlloc FrameRing::alloc(VulkanContext& ctx, VkDeviceSize size, VkDeviceSize align) {
    FrameAlloc out;
    if (size == 0) size = 1;
    if (!buffer_ && !grow(ctx, initialPerSlot() > size * 2 ? initialPerSlot() : size * 2)) return out;
    uint64_t off = 0;
    if (!core_.alloc(size, align, &off)) {
        // Growth empties both slots of the new buffer; this frame continues in its own slot there.
        if (!grow(ctx, core_.grownCapacity(size))) return out;
        if (!core_.alloc(size, align, &off)) return out;
    }
    out.buffer = buffer_.get();
    out.offset = off;
    out.ptr = mapped_ + off;
    out.size = size;
    out.range = core_.capacity();
    return out;
}

}  // namespace mc2vk
