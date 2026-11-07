#pragma once
#include <volk.h>
#include <array>
#include <optional>
// Read-only current-frame terrain resources. The terrain owns their lifetime.
struct Mc2GroundLightingFrame {
    VkDescriptorSet set = VK_NULL_HANDLE;
    std::array<uint32_t, 4> offsets{}; // terrain UBO, light pool, grid headers/indices
};
std::optional<Mc2GroundLightingFrame> mc2_vk_ground_lighting_frame();
