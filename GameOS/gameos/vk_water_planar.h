#pragma once
#include "vulkan_context.h"
#include "vulkan_ground_lighting.h"

namespace mc2::waterplanar {
// Frame-local attachments and projection. Recording callbacks borrow only GPU
// handles and ring spans; the owning water slot retires all view-local state.
struct Target {
    int w = 0, h = 0;
    int divisor = 1; // moving reflection uses a subrectangle of the full-size attachments
    float water = 0;
    const float* mainWorldToClip = nullptr;
    VkImageView color = VK_NULL_HANDLE, depth = VK_NULL_HANDLE;
    VkRect2D scissor{};
    Mc2GroundLightingFrame ground{};
};
bool prepareTerrain(std::unique_ptr<mc2vk::IslandState>& state, Target& target, mc2vk::VulkanContext::RecordFn& record);
bool prewarmProps(std::unique_ptr<mc2vk::IslandState>& state, int w, int h);
bool prepareProps(std::unique_ptr<mc2vk::IslandState>& state, const Target& target, mc2vk::VulkanContext::RecordFn& record);
}
