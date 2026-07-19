// vk_shared_light_feed.cpp -- VK-LIGHTFEED-DEDUP-1. See the header for the
// design + ring/slot safety argument. #ifdef MC2_VULKAN_ISLAND (whole TU).

#ifdef MC2_VULKAN_ISLAND

#include <volk.h>
#include <vk_mem_alloc.h>

#include "vk_shared_light_feed.h"
#include "vk_shared_gbuffer_gate.h"   // mc2vkgate::envTriState
#include "gos_frame_context.h"        // g_mc2FrameCounter (engine frame clock)

#include <cstdio>
#include <cstring>
#include <tracy/Tracy.hpp>

namespace {

struct Slot {
    VkBuffer      buf   = VK_NULL_HANDLE;
    VmaAllocation alloc = VK_NULL_HANDLE;
    VkDeviceSize  cap   = 0;
};

constexpr int kSlots = 3;   // >= 3: reuse age always exceeds the 2-frames-in-flight bound

Slot          g_slots[kSlots];
VmaAllocator  g_alloc      = VK_NULL_HANDLE;
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
        s_state = (mc2vkgate::envTriState("MC2_VK_LIGHTFEED_DEDUP") == 0) ? 0 : 1;
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

    if (g_alloc != VK_NULL_HANDLE && g_alloc != allocator) {
        // Shared-context rebirth: the ring's handles belong to a dead allocator.
        // Forget them (destroying via a dead allocator would be worse) and
        // restart. One receipt; in practice the engine context is process-wide.
        static bool s_logged = false;
        if (!s_logged) {
            std::fprintf(stderr, "[VK_LIGHTFEED_DEDUP v1] allocator changed -- ring reset\n");
            std::fflush(stderr);
            s_logged = true;
        }
        for (auto& s : g_slots) s = Slot{};
        g_slot = -1; g_frameValid = false;
    }
    g_alloc = allocator;

    const uint32_t frame = g_mc2FrameCounter;
    if (frame == g_frame) {
        // Second+ consumer this frame: reuse the already-copied slot. If the
        // CPU array GREW between two same-frame island flushes (mid-frame
        // dynamic-light append), fail soft: this caller does its own copy
        // (rare, content-safe; next frame's copy picks up the full array).
        if (!g_frameValid || (VkDeviceSize)cpuBytes > g_frameBytes) { ++g_fallbacks; return false; }
        ++g_hits;
        *outBuf = g_slots[g_slot].buf;
        return true;
    }
    g_frame = frame;
    g_frameValid = false;

    const int slot = (g_slot + 1) % kSlots;   // g_slot starts -1 -> slot 0
    Slot& sl = g_slots[slot];
    if (sl.buf == VK_NULL_HANDLE || (VkDeviceSize)cpuBytes > sl.cap) {
        if (sl.buf != VK_NULL_HANDLE) {
            // Growth destroys a handle a prior frame's submit may reference:
            // full idle first (mirrors the islands' ensure_*_capacity shape;
            // rare -- a few times around mission start as light count ramps).
            vkDeviceWaitIdle(device);
            vmaDestroyBuffer(g_alloc, sl.buf, sl.alloc);
            sl = Slot{};
        }
        const VkDeviceSize cap = (VkDeviceSize)cpuBytes + (VkDeviceSize)cpuBytes / 2 + 4096;
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size  = cap;
        bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
        if (vmaCreateBuffer(g_alloc, &bci, &aci, &sl.buf, &sl.alloc, nullptr) != VK_SUCCESS) {
            sl = Slot{};
            ++g_fallbacks;
            return false;   // fail-soft: caller runs its own copy path
        }
        sl.cap = cap;
    }

    {
        ZoneScopedN("VKIsl.Shared.LightFeed");   // VK-LIGHTFEED-DEDUP-1: the ONE per-frame LightsData CPU->VK copy
        void* p = nullptr;
        if (vmaMapMemory(g_alloc, sl.alloc, &p) != VK_SUCCESS) { ++g_fallbacks; return false; }
        std::memcpy(p, cpuData, (size_t)cpuBytes);
        vmaUnmapMemory(g_alloc, sl.alloc);
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

#endif  // MC2_VULKAN_ISLAND
