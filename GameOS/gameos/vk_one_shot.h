#pragma once
// vk_one_shot.h -- record one command buffer, submit it on the graphics queue and wait for it.
//
// WHY: setup work that must have finished before the next frame (an image's first layout, a query pool's first reset)
// needs a transient pool, a fence and a bounded wait whose failure path waits for the device before the pool and fence
// are released; one implementation keeps that failure path right everywhere.
#ifdef MC2_VULKAN_ISLAND
#include <volk.h>

#include <functional>
#include <string_view>

#include "vk_owned.h"

namespace mc2vk {
class VulkanContext;

// A transient command pool holding one primary command buffer, begun for a single submit. False on any failure (the pool is left to its owner).
bool beginTransientCommandBuffer(const VulkanContext& ctx, OwnedCommandPool& pool, VkCommandBuffer& cb);

// Records with `record` (between begin and end), submits, waits up to 5 s. False on any failure; `why` names the caller
// in the device-wait log line a timed-out submit produces.
bool submitOneShot(const VulkanContext& ctx, const std::function<void(VkCommandBuffer)>& record, std::string_view why);
}  // namespace mc2vk
#endif
