// vk_shared_light_feed.h
//
// VK-LIGHTFEED-DEDUP-1 -- ONE shared per-frame LightsData span for every VK island that consumes the game's CPU ObjectLights array (mc2::textures::ready()
// lightData_, the byte source of the GL SSBO upload at renderLists).
//
// Mech and prop consumers share persistent mapped storage per retired engine frame slot.
// Row generations avoid recopying unchanged static rows. Later same-frame consumers reuse
// the first version; a CPU edit makes them fall back to a private frame-ring copy.
// Consumers keep the dynamic-offset binding-4 LightsData ABI.
//
// Killswitch MC2_VK_LIGHTFEED_DEDUP=0 (default ON): acquire() always returns an empty span and each island copies the array into its own frame-ring span.

#ifndef MC2_VK_SHARED_LIGHT_FEED_H
#define MC2_VK_SHARED_LIGHT_FEED_H


#include <volk.h>
#include <cstdint>
#include <vector>

#include "vk_frame_ring.h"
#include "../../mclib/vk_object_lights.h"

namespace mc2vk {

class VulkanContext;

// The one-copy-per-frame bookkeeping. The engine Vulkan context owns the single instance (as a member).
class SharedLightFeed {
public:
    SharedLightFeed() = default;
    SharedLightFeed(const SharedLightFeed&) = delete;
    SharedLightFeed& operator=(const SharedLightFeed&) = delete;

    // Dirty rows update once per frame slot. Empty means the caller copies privately.
    FrameAlloc acquire(VulkanContext& ctx, const mc2::vkLights::Feed& feed);

    // Called after device idle, before the allocator is released.
    void release() {
        frame_ = ~0ull; span_ = FrameAlloc{}; bytes_ = 0;
        for (auto& slot : slots_) slot = Slot{};
    }

private:
    struct Slot {
        OwnedBuffer buffer;
        void* mapped = nullptr;
        VkDeviceSize capacity = 0;
        std::vector<uint64_t> generations;
    };
    Slot slots_[mc2vk::kMaxFramesInFlight];
    uint64_t revision_ = 0;
    uint64_t packetKey_ = 0;
    FrameAlloc    span_;
    uint64_t      frame_ = ~0ull;     // CompletionTracker frame of the last copy attempt
    VkDeviceSize  bytes_ = 0;         // bytes copied for frame_
    unsigned long copies_ = 0, hits_ = 0, fallbacks_ = 0;
};

}  // namespace mc2vk

// Killswitch resolver: MC2_VK_LIGHTFEED_DEDUP explicit "0" disables (per-island copies restored); anything else = ON. Latched on first call.
bool mc2vkSharedLightFeedEnabled();

// Acquire this frame's immutable light version, or fall back to a private copy.
mc2vk::FrameAlloc mc2vkSharedLightFeedAcquire(const mc2::vkLights::Feed& feed);

#endif  // MC2_VK_SHARED_LIGHT_FEED_H
