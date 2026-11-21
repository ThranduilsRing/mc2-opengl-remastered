#pragma once
#include "vulkan_context.h"
#include "vulkan_ground_lighting.h"
#include "water_planar_view.h"
#include <optional>

namespace mc2::waterplanar {
// Frame-local attachments and projection. Recording callbacks borrow only GPU
// handles and ring spans; the owning water slot retires all view-local state.
struct Target {
    int w = 0, h = 0;
    int divisor = 1; // moving reflection uses a subrectangle of the full-size attachments
    float water = 0;
    const float* mainWorldToClip = nullptr;
    // Camera-independent bake: use the target projection for props as well.
    bool useTargetView = false;
    const float* cameraPos = nullptr; // optional terrain eye in Stuff/Y-up space
    VkImageView color = VK_NULL_HANDLE, depth = VK_NULL_HANDLE;
    VkRect2D scissor{};
    Mc2GroundLightingFrame ground{};
    // The moving reflection's scissor as an NDC rectangle of its pass (divisor-sized), for culling against it; false when
    // the pass is unscissored or uses the target view (the bake), which keeps the full clip volume.
    std::optional<NdcRect> scissorNdc() const {
        if (!scissor.extent.width || useTargetView) return std::nullopt;
        return scissorNdcRect(scissor.offset.x, scissor.offset.y, static_cast<int>(scissor.extent.width), static_cast<int>(scissor.extent.height),
                              (w + divisor - 1) / divisor, (h + divisor - 1) / divisor);
    }
};
bool prepareTerrain(std::unique_ptr<mc2vk::IslandState>& state, Target& target, mc2vk::VulkanContext::RecordFn& record);
bool prewarmTerrain(std::unique_ptr<mc2vk::IslandState>& state, int w, int h);
bool prewarmProps(std::unique_ptr<mc2vk::IslandState>& state, int w, int h);
bool prepareProps(std::unique_ptr<mc2vk::IslandState>& state, const Target& target, mc2vk::VulkanContext::RecordFn& record);
}
