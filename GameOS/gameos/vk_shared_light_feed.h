// vk_shared_light_feed.h
//
// VK-LIGHTFEED-DEDUP-1 -- ONE shared per-frame LightsData SSBO for every VK
// island that consumes the game's CPU ObjectLights array (mcTextureManager
// lightData_, the byte source of the GL SSBO upload at renderLists).
//
// Before this slice the mech island and the color-prop island each memcpy'd
// the SAME ~3.5 MB array into their own per-parity SSBO every frame
// (VK-ISLAND-TRACY-1 receipts: VKIsl.Mech.LightFeed 0.45 ms/f +
// VKIsl.Props.LightFeed 0.52 ms/f -- twin copies of identical bytes). This
// module owns a small ring of host-visible STORAGE buffers on the SHARED
// engine device; the FIRST island to acquire in a frame performs the single
// copy (Tracy zone VKIsl.Shared.LightFeed), later acquirers in the same frame
// (keyed on g_mc2FrameCounter) get the same VkBuffer for free.
//
// Consumers rebind their binding-4 LightsData descriptor to the returned
// buffer each frame (the ring slot handle changes frame-to-frame); their own
// per-parity lightBuf stays allocated as the killswitch / fail-soft path.
//
// Killswitch MC2_VK_LIGHTFEED_DEDUP=0 (default ON): acquire() always returns
// false and both islands run their pre-slice twin-copy paths unchanged.
//
// Ring/slot safety (why 3 slots need no extra fences): a slot written at
// frame N was last consumed at frame <= N-3 (the ring only advances when a
// copy lands, at most once per frame). Every consuming island submits to the
// ONE shared graphics queue (VulkanContext::graphicsQueue) and bounds its own
// work to <= 2 frames in flight via its 2-slot parity fences
// (vkImageResourceBeginFrame waits the slot's fence BEFORE any [par] staging,
// and every acquire call sits after its caller's beginFrame). By frame N the
// waited fences cover queue submissions that FOLLOW every frame-(N-3)
// submission in submission order, so the frame-(N-3) GPU reads of this slot
// retired before the CPU overwrite. Growth is the harder case (handle
// destruction, not just content): it vkDeviceWaitIdle()s first, mirroring the
// islands' own ensure_*_capacity convention.

#ifndef MC2_VK_SHARED_LIGHT_FEED_H
#define MC2_VK_SHARED_LIGHT_FEED_H

#ifdef MC2_VULKAN_ISLAND

#include <volk.h>
#include <cstdint>

#include "vk_owned.h"

namespace mc2vk {

// The ring of host-visible LightsData buffers and the one-copy-per-frame bookkeeping. The engine Vulkan context owns the single instance (as a member),
// so the ring is released by the context's own destroy() ahead of the allocator and the device it was made on: there is no second teardown call to
// forget and no static owner.
class SharedLightFeed {
public:
    static constexpr int kSlots = 3;   // >= 3: reuse age always exceeds the 2-frames-in-flight bound

    SharedLightFeed() = default;
    ~SharedLightFeed() { release(); }
    SharedLightFeed(const SharedLightFeed&) = delete;
    SharedLightFeed& operator=(const SharedLightFeed&) = delete;

    // Copy-once-per-frame acquire (see the file header). False is fail-soft: the caller runs its own copy path.
    bool acquire(VmaAllocator allocator, VkDevice device, const void* cpuData, uint64_t cpuBytes, VkBuffer* outBuf);

    // Frees every slot after the GPU is idle. Safe when nothing was acquired; a later acquire builds the slots again.
    void release();

private:
    OwnedBuffer  slots_[kSlots];
    VkDeviceSize caps_[kSlots] = {};
    VkDevice     device_       = VK_NULL_HANDLE;   // the device the slots were made on; set by the first acquire
    int          slot_         = -1;               // last written ring slot
    uint32_t     frame_        = 0xFFFFFFFFu;      // g_mc2FrameCounter of the last copy attempt
    bool         frameValid_   = false;            // the copy for frame_ landed
    VkDeviceSize frameBytes_   = 0;                // bytes copied for frame_
    unsigned long copies_ = 0, hits_ = 0, fallbacks_ = 0;
};

}  // namespace mc2vk

// Killswitch resolver: MC2_VK_LIGHTFEED_DEDUP explicit "0" disables (twin
// per-island copies restored); anything else = ON. Latched on first call.
bool mc2vkSharedLightFeedEnabled();

// Copy-once-per-frame acquire. On success *outBuf is a host-visible STORAGE
// buffer holding cpuBytes of this frame's LightsData (copied by the first
// caller this frame). Returns false fail-soft -- killswitch off, allocator or
// buffer trouble, handles that are not the engine context's, or the CPU array
// grew between two same-frame callers -- and the caller must fall back to its
// own per-island copy path.
bool mc2vkSharedLightFeedAcquire(VmaAllocator allocator, VkDevice device,
                                 const void* cpuData, uint64_t cpuBytes,
                                 VkBuffer* outBuf);

#endif  // MC2_VULKAN_ISLAND
#endif  // MC2_VK_SHARED_LIGHT_FEED_H
