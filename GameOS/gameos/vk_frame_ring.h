// vk_frame_ring.h -- per-frame transient memory: one host-visible buffer, two slot regions, linear bump allocation.
//
// Frame F bumps inside slot F % 2 and the slot is reset when frame F begins, after the context has seen the ticket of frame F-2 complete (so the GPU
// is done with everything the slot's previous user wrote). The caller gets {buffer, offset, cpu pointer} and binds the buffer at that offset (vertex
// buffers, dynamic-offset descriptors, copy sources). A request that does not fit grows the ring: a larger buffer replaces it and the old one is
// retired through the deferred-destroy queue, so what this frame already handed out stays valid until the frame completes.
//
// FrameRingCore is the offset arithmetic with no Vulkan type (doctest); FrameRing adds the buffer.

#ifndef MC2_VK_FRAME_RING_H
#define MC2_VK_FRAME_RING_H

#include <cstdint>

#ifdef MC2_VULKAN_ISLAND
#include <volk.h>
#include "vk_owned.h"
#endif

namespace mc2vk {

class FrameRingCore {
public:
    static constexpr uint32_t kSlots = 2;

    // New capacity per slot: every slot starts empty (a replaced buffer is fresh memory).
    void setCapacity(uint64_t perSlot) { cap_ = perSlot; head_[0] = head_[1] = 0; }
    // Frame F opens: its slot restarts at zero.
    void beginFrame(uint64_t frameNo) { slot_ = static_cast<uint32_t>(frameNo % kSlots); head_[slot_] = 0; }
    // Returns false when the request does not fit; otherwise *ringOffset is the byte offset in the whole buffer.
    bool alloc(uint64_t size, uint64_t align, uint64_t* ringOffset) {
        if (align == 0) align = 1;
        const uint64_t aligned = (head_[slot_] + align - 1) / align * align;
        if (aligned + size > cap_) return false;
        *ringOffset = static_cast<uint64_t>(slot_) * cap_ + aligned;
        head_[slot_] = aligned + size;
        if (head_[slot_] > peak_) peak_ = head_[slot_];
        return true;
    }
    uint64_t peak() const { return peak_; }   // the most any one frame has used of its slot
    uint64_t capacity() const { return cap_; }
    uint64_t used() const { return head_[slot_]; }
    uint32_t slot() const { return slot_; }
    // The capacity a request of `size` needs: at least double, big enough for the request in one slot.
    uint64_t grownCapacity(uint64_t size) const {
        uint64_t c = cap_ ? cap_ * 2 : 1;
        while (c < size * 2) c *= 2;
        return c;
    }

private:
    uint64_t cap_ = 0;
    uint64_t head_[kSlots] = {0, 0};
    uint64_t peak_ = 0;
    uint32_t slot_ = 0;
};

#ifdef MC2_VULKAN_ISLAND
class VulkanContext;

constexpr VkDeviceSize alignUp(VkDeviceSize v, VkDeviceSize a) { return (v + a - 1) / a * a; }

// Writes one dynamic-offset buffer binding of `set` at `buffer` from offset 0; the draw passes the offset at bind time. `type` is
// VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC (an explicit range: uniform ranges are device-limited) or VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC. A dynamic
// descriptor range must never be VK_WHOLE_SIZE (its dynamic offset would have to be 0): storage bindings use FrameAlloc::range, a range that stays in
// the buffer from any offset the ring hands out.
inline void writeDynamicBinding(VkDevice device, VkDescriptorSet set, uint32_t binding, VkDescriptorType type, VkBuffer buffer, VkDeviceSize range) {
    VkDescriptorBufferInfo info{buffer, 0, range};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.dstBinding = binding;
    w.descriptorCount = 1;
    w.descriptorType = type;
    w.pBufferInfo = &info;
    vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
}

struct FrameAlloc {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    void* ptr = nullptr;
    VkDeviceSize size = 0;
    VkDeviceSize range = 0;   // a dynamic-descriptor range valid from every offset of this buffer (the ring is one slot longer than its slots)
    explicit operator bool() const { return buffer != VK_NULL_HANDLE && ptr != nullptr; }
};

class FrameRing {
public:
    // Opens frame `frameNo`. The caller (the context) has already made sure frame F-2 completed.
    void beginFrame(uint64_t frameNo) { core_.beginFrame(frameNo); }
    // Never blocks. An empty FrameAlloc means the allocation failed (no device memory); callers fail soft.
    FrameAlloc alloc(VulkanContext& ctx, VkDeviceSize size, VkDeviceSize align);
    // The buffer the next allocation will come from (created on first use). Dynamic-offset descriptors point at it; a frame that allocated from a
    // different buffer than a set was written for rewrites that set (growth replaces the buffer).
    VkBuffer ensureBuffer(VulkanContext& ctx);
    VkDeviceSize bindRange() const { return core_.capacity(); }
    void release() { buffer_.reset(); mapped_ = nullptr; core_.setCapacity(0); }
    uint64_t capacityPerSlot() const { return core_.capacity(); }
    uint64_t peakPerSlot() const { return core_.peak(); }
    uint32_t growths() const { return growths_; }

private:
    bool grow(VulkanContext& ctx, VkDeviceSize perSlot);
    FrameRingCore core_;
    OwnedBuffer buffer_;
    uint8_t* mapped_ = nullptr;
    uint32_t growths_ = 0;
};
#endif

}  // namespace mc2vk

#endif  // MC2_VK_FRAME_RING_H
