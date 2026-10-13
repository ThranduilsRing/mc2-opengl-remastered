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

#include <memory>

namespace {

constexpr int kSlots = 3;   // >= 3: reuse age always exceeds the 2-frames-in-flight bound

// The ring of host-visible storage buffers. It owns its VMA allocations: the destructor is the only place they are released, after the GPU is idle, and
// it must run before the allocator it captured is destroyed (mc2vkSharedLightFeedRelease, called from engine teardown ahead of the context).
class LightFeedRing {
public:
    LightFeedRing(VmaAllocator allocator, VkDevice device) : alloc_(allocator), device_(device) {}
    ~LightFeedRing() {
        bool any = false;
        for (const Slot& s : slots_) any = any || s.buf != VK_NULL_HANDLE;
        if (!any) return;
        vkDeviceWaitIdle(device_);
        for (Slot& s : slots_) s.release(alloc_);
    }
    LightFeedRing(const LightFeedRing&) = delete;
    LightFeedRing& operator=(const LightFeedRing&) = delete;

    struct Slot {
        VkBuffer      buf   = VK_NULL_HANDLE;
        VmaAllocation alloc = VK_NULL_HANDLE;
        VkDeviceSize  cap   = 0;
        void release(VmaAllocator a) {
            if (buf != VK_NULL_HANDLE) vmaDestroyBuffer(a, buf, alloc);
            *this = Slot{};
        }
    };

    VmaAllocator allocator() const { return alloc_; }
    VkDevice     device() const { return device_; }
    Slot&        slot(int i) { return slots_[i]; }

private:
    VmaAllocator alloc_;
    VkDevice     device_;
    Slot         slots_[kSlots];
};

std::unique_ptr<LightFeedRing> g_ring;
int           g_slot       = -1;            // last written ring slot
uint32_t      g_frame      = 0xFFFFFFFFu;   // g_mc2FrameCounter of the last copy attempt
bool          g_frameValid = false;         // the copy for g_frame landed
VkDeviceSize  g_frameBytes = 0;             // bytes copied for g_frame

unsigned long g_copies = 0, g_hits = 0, g_fallbacks = 0;

}  // namespace

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
    if (allocator == VK_NULL_HANDLE || device == VK_NULL_HANDLE ||
        cpuData == nullptr || outBuf == nullptr || cpuBytes < 3600ull)
        return false;

    if (g_ring && (g_ring->allocator() != allocator || g_ring->device() != device)) {
        // A different allocator than the ring was built with: only reachable if the shared context were reborn without a release. The ring is
        // released ahead of the context in teardown, so the old allocator is still alive and the buffers are destroyed through it.
        static bool s_logged = false;
        if (!s_logged) {
            std::fprintf(stderr, "[VK_LIGHTFEED_DEDUP v1] allocator changed -- ring reset\n");
            std::fflush(stderr);
            s_logged = true;
        }
        g_ring.reset();
        g_slot = -1; g_frameValid = false;
    }
    if (!g_ring) g_ring = std::make_unique<LightFeedRing>(allocator, device);

    const uint32_t frame = g_mc2FrameCounter;
    if (frame == g_frame) {
        // Second+ consumer this frame: reuse the already-copied slot. If the
        // CPU array GREW between two same-frame island flushes (mid-frame
        // dynamic-light append), fail soft: this caller does its own copy
        // (rare, content-safe; next frame's copy picks up the full array).
        if (!g_frameValid || (VkDeviceSize)cpuBytes > g_frameBytes) { ++g_fallbacks; return false; }
        ++g_hits;
        *outBuf = g_ring->slot(g_slot).buf;
        return true;
    }
    g_frame = frame;
    g_frameValid = false;

    const int slot = (g_slot + 1) % kSlots;   // g_slot starts -1 -> slot 0
    LightFeedRing::Slot& sl = g_ring->slot(slot);
    if (sl.buf == VK_NULL_HANDLE || (VkDeviceSize)cpuBytes > sl.cap) {
        if (sl.buf != VK_NULL_HANDLE) {
            // Growth destroys a handle a prior frame's submit may reference:
            // full idle first (mirrors the islands' ensure_*_capacity shape;
            // rare -- a few times around mission start as light count ramps).
            vkDeviceWaitIdle(device);
            sl.release(allocator);
        }
        const VkDeviceSize cap = (VkDeviceSize)cpuBytes + (VkDeviceSize)cpuBytes / 2 + 4096;
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size  = cap;
        bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
        if (vmaCreateBuffer(allocator, &bci, &aci, &sl.buf, &sl.alloc, nullptr) != VK_SUCCESS) {
            sl = LightFeedRing::Slot{};
            ++g_fallbacks;
            return false;   // fail-soft: caller runs its own copy path
        }
        sl.cap = cap;
        mc2vk::nameObject(device, sl.buf, "lightfeed.ring[%d]", slot);
    }

    {
        ZoneScopedN("VKIsl.Shared.LightFeed");   // VK-LIGHTFEED-DEDUP-1: the ONE per-frame LightsData CPU->VK copy
        void* p = nullptr;
        if (vmaMapMemory(allocator, sl.alloc, &p) != VK_SUCCESS) { ++g_fallbacks; return false; }
        std::memcpy(p, cpuData, (size_t)cpuBytes);
        vmaUnmapMemory(allocator, sl.alloc);
    }

    g_slot = slot;
    g_frameValid = true;
    g_frameBytes = (VkDeviceSize)cpuBytes;
    ++g_copies;

    if (g_copies == 1) {
        std::fprintf(stderr,
            "[VK_LIGHTFEED_DEDUP v1] armed: shared per-frame LightsData SSBO "
            "(%d-slot ring, %llu B) -- killswitch MC2_VK_LIGHTFEED_DEDUP=0\n",
            kSlots, (unsigned long long)cpuBytes);
        std::fflush(stderr);
    }
    if (g_copies == 600) {   // ~10 s receipt: hits ~= copies when both islands share
        std::fprintf(stderr,
            "[VK_LIGHTFEED_DEDUP v1] 600 copies: sharedHits=%lu fallbacks=%lu "
            "bytes/frame=%llu\n",
            g_hits, g_fallbacks, (unsigned long long)g_frameBytes);
        std::fflush(stderr);
    }

    *outBuf = sl.buf;
    return true;
}

void mc2vkSharedLightFeedRelease()
{
    g_ring.reset();
    g_slot = -1;
    g_frame = 0xFFFFFFFFu;
    g_frameValid = false;
}

#endif  // MC2_VULKAN_ISLAND
