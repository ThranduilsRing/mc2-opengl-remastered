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

// VMA allocator handle (forward-declared; the .cpp includes vk_mem_alloc.h).
struct VmaAllocator_T;
typedef struct VmaAllocator_T* VmaAllocator;

// Killswitch resolver: MC2_VK_LIGHTFEED_DEDUP explicit "0" disables (twin
// per-island copies restored); anything else = ON. Latched on first call.
bool mc2vkSharedLightFeedEnabled();

// Copy-once-per-frame acquire. On success *outBuf is a host-visible STORAGE
// buffer holding cpuBytes of this frame's LightsData (copied by the first
// caller this frame). Returns false fail-soft -- killswitch off, allocator or
// buffer trouble, or the CPU array grew between two same-frame callers -- and
// the caller must fall back to its own per-island copy path.
bool mc2vkSharedLightFeedAcquire(VmaAllocator allocator, VkDevice device,
                                 const void* cpuData, uint64_t cpuBytes,
                                 VkBuffer* outBuf);

#endif  // MC2_VULKAN_ISLAND
#endif  // MC2_VK_SHARED_LIGHT_FEED_H
