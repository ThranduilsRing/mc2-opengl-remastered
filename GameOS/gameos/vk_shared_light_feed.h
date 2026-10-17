// vk_shared_light_feed.h
//
// VK-LIGHTFEED-DEDUP-1 -- ONE shared per-frame LightsData span for every VK island that consumes the game's CPU ObjectLights array (mcTextureManager
// lightData_, the byte source of the GL SSBO upload at renderLists).
//
// The mech and the color-prop island both read the SAME ~3.5 MB array. The first island to acquire in a frame copies it once into the engine frame ring
// (Tracy zone VKIsl.Shared.LightFeed); later acquirers in the same frame get the same span for free. The span is frame-ring memory, so it is valid
// until the frame that asked for it has completed and needs no ring of its own, no per-slot growth and no device idle. Consumers bind it as the
// dynamic-offset binding-4 LightsData descriptor.
//
// Killswitch MC2_VK_LIGHTFEED_DEDUP=0 (default ON): acquire() always returns an empty span and each island copies the array into its own frame-ring span.

#ifndef MC2_VK_SHARED_LIGHT_FEED_H
#define MC2_VK_SHARED_LIGHT_FEED_H

#ifdef MC2_VULKAN_ISLAND

#include <volk.h>
#include <cstdint>

#include "vk_frame_ring.h"

namespace mc2vk {

class VulkanContext;

// The one-copy-per-frame bookkeeping. The engine Vulkan context owns the single instance (as a member).
class SharedLightFeed {
public:
    SharedLightFeed() = default;
    SharedLightFeed(const SharedLightFeed&) = delete;
    SharedLightFeed& operator=(const SharedLightFeed&) = delete;

    // Copy-once-per-frame acquire (see the file header). An empty span is fail-soft: the caller runs its own copy path.
    FrameAlloc acquire(VulkanContext& ctx, const void* cpuData, uint64_t cpuBytes);

    // Forgets the cached span (the ring it points into is being released).
    void release() { frame_ = ~0ull; span_ = FrameAlloc{}; bytes_ = 0; }

private:
    FrameAlloc    span_;
    uint64_t      frame_ = ~0ull;     // CompletionTracker frame of the last copy attempt
    VkDeviceSize  bytes_ = 0;         // bytes copied for frame_
    unsigned long copies_ = 0, hits_ = 0, fallbacks_ = 0;
};

}  // namespace mc2vk

// Killswitch resolver: MC2_VK_LIGHTFEED_DEDUP explicit "0" disables (per-island copies restored); anything else = ON. Latched on first call.
bool mc2vkSharedLightFeedEnabled();

// Copy-once-per-frame acquire on the engine context. Returns the frame-ring span holding cpuBytes of this frame's LightsData (copied by the first caller
// this frame), or an empty span fail-soft: killswitch off, the ring is out of memory, or the CPU array grew between two same-frame callers. The caller
// then copies into its own frame-ring span.
mc2vk::FrameAlloc mc2vkSharedLightFeedAcquire(const void* cpuData, uint64_t cpuBytes);

#endif  // MC2_VULKAN_ISLAND
#endif  // MC2_VK_SHARED_LIGHT_FEED_H
