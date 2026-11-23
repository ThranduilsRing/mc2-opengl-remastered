#pragma once
#include <vulkan/vulkan_core.h>
#include <vector>

namespace mc2vk {
enum class WindowsHdrState { Unknown, Off, On };
// Auto follows the desktop; a failed/non-Windows query is deliberately SDR.
inline bool decideHdr(int policy, bool force, bool offscreen, WindowsHdrState windowsHdrActive, bool surfaceOffersHdr) {
    return force || (policy == 0 && !offscreen && windowsHdrActive == WindowsHdrState::On && surfaceOffersHdr);
}
// Pure WSI policy. The SDR fallback deliberately preserves the historical first-entry choice.
inline VkSurfaceFormatKHR chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats, bool wantHdr) {
    if (wantHdr) {
        for (const auto preferred : {
                 VkSurfaceFormatKHR{VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT},
                 VkSurfaceFormatKHR{VK_FORMAT_A2R10G10B10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT},
                 VkSurfaceFormatKHR{VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT}})
            for (const auto f : formats)
                if (f.format == preferred.format && f.colorSpace == preferred.colorSpace) return f;
    }
    for (const auto f : formats)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) return f;
    return formats.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR} : formats.front();
}
}
