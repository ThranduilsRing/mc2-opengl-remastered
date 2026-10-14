// vk_shared_light_feed.cpp -- VK-LIGHTFEED-DEDUP-1. See the header for the
// design + ring/slot safety argument. #ifdef MC2_VULKAN_ISLAND (whole TU).

#ifdef MC2_VULKAN_ISLAND

#include <volk.h>
#include <vk_mem_alloc.h>

#include "vk_shared_light_feed.h"
#include "Gates/gate.h"                // gate::get<> (MC2_VK_LIGHTFEED_DEDUP)
#include "gos_frame_context.h"        // g_mc2FrameCounter (engine frame clock)

#include <cstdio>
#include <cstring>
#include <tracy/Tracy.hpp>

#include "vulkan_context.h"

bool mc2vkSharedLightFeedEnabled()
{
    // Default-ON killswitch (fence-defer template): explicit "0" reverts to the
    // twin per-island copy paths; unset / "1" = dedup armed.
    static int s_state = -1;
    if (s_state == -1)
        s_state = mc2::gate::get<"MC2_VK_LIGHTFEED_DEDUP">() ? 1 : 0;
    return s_state == 1;
}

bool mc2vkSharedLightFeedAcquire(VmaAllocator allocator, VkDevice device,
                                 const void* cpuData, uint64_t cpuBytes,
                                 VkBuffer* outBuf)
{
    if (!mc2vkSharedLightFeedEnabled()) return false;
    mc2vk::VulkanContext& ctx = mc2vk::getOrCreateEngineVulkanContext();
    // The ring lives in the engine context; an island always passes that context's handles. Anything else would put buffers on a device the ring
    // does not release, so it is refused and the caller keeps its own copy.
    if (!ctx.valid() || allocator != ctx.allocator() || device != ctx.device()) return false;
    return ctx.lightFeed().acquire(allocator, device, cpuData, cpuBytes, outBuf);
}

namespace mc2vk {

bool SharedLightFeed::acquire(VmaAllocator allocator, VkDevice device, const void* cpuData, uint64_t cpuBytes, VkBuffer* outBuf)
{
    if (allocator == VK_NULL_HANDLE || device == VK_NULL_HANDLE ||
        cpuData == nullptr || outBuf == nullptr || cpuBytes < 3600ull)
        return false;
    device_ = device;

    const uint32_t frame = g_mc2FrameCounter;
    if (frame == frame_) {
        // Second+ consumer this frame: reuse the already-copied slot. If the
        // CPU array GREW between two same-frame island flushes (mid-frame
        // dynamic-light append), fail soft: this caller does its own copy
        // (rare, content-safe; next frame's copy picks up the full array).
        if (!frameValid_ || (VkDeviceSize)cpuBytes > frameBytes_) { ++fallbacks_; return false; }
        ++hits_;
        *outBuf = slots_[slot_].get();
        return true;
    }
    frame_ = frame;
    frameValid_ = false;

    const int slot = (slot_ + 1) % kSlots;   // slot_ starts -1 -> slot 0
    OwnedBuffer& sl = slots_[slot];
    if (!sl || (VkDeviceSize)cpuBytes > caps_[slot]) {
        if (sl) {
            // Growth destroys a handle a prior frame's submit may reference:
            // full idle first (mirrors the islands' ensure_*_capacity shape;
            // rare -- a few times around mission start as light count ramps).
            vkDeviceWaitIdle(device);
            sl.reset();
        }
        const VkDeviceSize cap = (VkDeviceSize)cpuBytes + (VkDeviceSize)cpuBytes / 2 + 4096;
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size  = cap;
        bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
        if (!sl.create(allocator, device, bci, aci, "lightfeed.ring", slot)) {
            ++fallbacks_;
            return false;   // fail-soft: caller runs its own copy path
        }
        caps_[slot] = cap;
    }

    {
        ZoneScopedN("VKIsl.Shared.LightFeed");   // VK-LIGHTFEED-DEDUP-1: the ONE per-frame LightsData CPU->VK copy
        void* p = sl.map();
        if (!p) { ++fallbacks_; return false; }
        std::memcpy(p, cpuData, (size_t)cpuBytes);
        sl.unmap();
    }

    slot_ = slot;
    frameValid_ = true;
    frameBytes_ = (VkDeviceSize)cpuBytes;
    ++copies_;

    if (copies_ == 1) {
        std::fprintf(stderr,
            "[VK_LIGHTFEED_DEDUP v1] armed: shared per-frame LightsData SSBO "
            "(%d-slot ring, %llu B) -- killswitch MC2_VK_LIGHTFEED_DEDUP=0\n",
            kSlots, (unsigned long long)cpuBytes);
        std::fflush(stderr);
    }
    if (copies_ == 600) {   // ~10 s receipt: hits ~= copies when both islands share
        std::fprintf(stderr,
            "[VK_LIGHTFEED_DEDUP v1] 600 copies: sharedHits=%lu fallbacks=%lu "
            "bytes/frame=%llu\n",
            hits_, fallbacks_, (unsigned long long)frameBytes_);
        std::fflush(stderr);
    }

    *outBuf = sl.get();
    return true;
}

void SharedLightFeed::release()
{
    bool any = false;
    for (const OwnedBuffer& s : slots_) any = any || static_cast<bool>(s);
    if (any) vkDeviceWaitIdle(device_);
    for (OwnedBuffer& s : slots_) s.reset();
    for (VkDeviceSize& c : caps_) c = 0;
    slot_ = -1;
    frame_ = 0xFFFFFFFFu;
    frameValid_ = false;
}

}  // namespace mc2vk

#endif  // MC2_VULKAN_ISLAND
