// vk_shared_light_feed.cpp -- VK-LIGHTFEED-DEDUP-1. See the header for the design. #ifdef MC2_VULKAN_ISLAND (whole TU).

#ifdef MC2_VULKAN_ISLAND

#include <volk.h>

#include "vk_shared_light_feed.h"
#include "Gates/gate.h"                // gate::get<> (MC2_VK_LIGHTFEED_DEDUP)

#include <cstdio>
#include <cstring>
#include <tracy/Tracy.hpp>

#include "vulkan_context.h"
#include "mc2_diag_verbose.h"

bool mc2vkSharedLightFeedEnabled()
{
    // Default-ON killswitch (fence-defer template): explicit "0" reverts to the per-island copy paths; unset / "1" = dedup armed.
    static int s_state = -1;
    if (s_state == -1)
        s_state = mc2::gate::get<"MC2_VK_LIGHTFEED_DEDUP">() ? 1 : 0;
    return s_state == 1;
}

mc2vk::FrameAlloc mc2vkSharedLightFeedAcquire(const void* cpuData, uint64_t cpuBytes)
{
    if (!mc2vkSharedLightFeedEnabled()) return {};
    mc2vk::VulkanContext& ctx = mc2vk::getOrCreateEngineVulkanContext();
    if (!ctx.valid()) return {};
    return ctx.lightFeed().acquire(ctx, cpuData, cpuBytes);
}

namespace mc2vk {

FrameAlloc SharedLightFeed::acquire(VulkanContext& ctx, const void* cpuData, uint64_t cpuBytes)
{
    if (cpuData == nullptr || cpuBytes < 3600ull) return {};

    const uint64_t frame = ctx.tracker().frameNo();
    if (frame == frame_) {
        // Second+ consumer this frame: reuse the already-copied span. If the CPU array GREW between two same-frame island flushes (mid-frame dynamic-light
        // append), fail soft: this caller does its own copy (rare, content-safe; next frame's copy picks up the full array).
        if (!span_ || static_cast<VkDeviceSize>(cpuBytes) > bytes_) { ++fallbacks_; return {}; }
        ++hits_;
        return span_;
    }
    frame_ = frame;
    span_ = FrameAlloc{};

    {
        ZoneScopedN("VKIsl.Shared.LightFeed");   // VK-LIGHTFEED-DEDUP-1: the ONE per-frame LightsData CPU->VK copy
        const FrameAlloc a = ctx.frameAlloc(static_cast<VkDeviceSize>(cpuBytes));
        if (!a) { ++fallbacks_; return {}; }   // fail-soft: caller runs its own copy path
        std::memcpy(a.ptr, cpuData, static_cast<size_t>(cpuBytes));
        span_ = a;
    }
    bytes_ = static_cast<VkDeviceSize>(cpuBytes);
    ++copies_;

    if (copies_ == 1 && mc2DiagVerbose()) {
        std::fprintf(stderr,
            "[VK_LIGHTFEED_DEDUP v1] armed: shared per-frame LightsData span in the frame ring (%llu B) -- killswitch MC2_VK_LIGHTFEED_DEDUP=0\n",
            static_cast<unsigned long long>(cpuBytes));
        std::fflush(stderr);
    }
    if (copies_ == 600 && mc2DiagVerbose()) {   // ~10 s receipt: hits ~= copies when both islands share
        std::fprintf(stderr,
            "[VK_LIGHTFEED_DEDUP v1] 600 copies: sharedHits=%lu fallbacks=%lu bytes/frame=%llu\n",
            hits_, fallbacks_, static_cast<unsigned long long>(bytes_));
        std::fflush(stderr);
    }

    return span_;
}

}  // namespace mc2vk

#endif  // MC2_VULKAN_ISLAND
