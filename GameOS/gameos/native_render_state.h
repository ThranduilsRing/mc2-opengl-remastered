#pragma once
#include <cstdint>
namespace RenderCore { enum class RenderResourceId : uint16_t; }
namespace mc2vk {
// Native pipelines have no persistent global framebuffer binding to query.
// These check the real destination owners and Vulkan validation health.
bool nativeRenderTargetAvailable(RenderCore::RenderResourceId target);
bool nativeRenderHealthy();
}
