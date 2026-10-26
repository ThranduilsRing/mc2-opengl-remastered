// vk_hdri_source.h -- the sky HDRI as the Vulkan sky and water islands upload it.
//
// WHAT: reads the engine's CPU copy of the HDRI (gos_GetHdriCpuImage) into the Vulkan format and the per-level bytes and sizes.
// WHY: both islands used to read the GL texture back (glGetCompressedTexImage / glGetTexImage); the CPU copy is the same bytes
// without GL, and one reader keeps the two islands' checks the same.
#pragma once

#ifdef MC2_VULKAN_ISLAND

#include <volk.h>

#include "gos_hdri.h"
#include "Gates/render_options.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace mc2vk {

// The image of the current HDRI, or false with `reason` set: no HDRI, a malformed copy (sizes that do not match the format or
// the mip chain), or a BC6H image on a device without BC support. `generation` is the copy's load generation (0 when false).
inline bool hdriFromCpu(VkPhysicalDevice phys, VkFormat& fmt, std::vector<std::vector<unsigned char>>& levels,
                        std::vector<std::pair<int, int>>& dims, uint64_t& generation, const char*& reason) {
    levels.clear();
    dims.clear();
    generation = 0;
    const HdriCpuImage* img = gos_GetHdriCpuImage();
    if (!img || img->levels.empty()) { reason = "no CPU HDRI"; return false; }
    if (img->levels.size() != img->dims.size()) { reason = "level / size count mismatch"; return false; }
    const int w0 = img->dims[0].first, h0 = img->dims[0].second;
    if (w0 <= 0 || h0 <= 0 || w0 > 16384 || h0 > 16384) { reason = "bad base size"; return false; }
    if (img->format == HdriCpuImage::Format::Rgba16f) {
        for (size_t i = 0; i < img->levels.size(); ++i) {
            const int lw = (w0 >> i) > 0 ? (w0 >> i) : 1, lh = (h0 >> i) > 0 ? (h0 >> i) : 1;
            if (img->dims[i].first != lw || img->dims[i].second != lh || img->levels[i].size() != static_cast<size_t>(lw) * lh * 8) {
                reason = "RGBA16F level does not match the mip chain"; return false;
            }
        }
        fmt = VK_FORMAT_R16G16B16A16_SFLOAT;
    } else {
        const size_t blocks = static_cast<size_t>((w0 + 3) / 4) * static_cast<size_t>((h0 + 3) / 4);
        if (img->levels.size() != 1 || img->levels[0].size() != blocks * 16) { reason = "BC6H mip 0 size mismatch"; return false; }
        VkPhysicalDeviceFeatures feats{};
        vkGetPhysicalDeviceFeatures(phys, &feats);
        if (!feats.textureCompressionBC) { reason = "device lacks textureCompressionBC"; return false; }
        fmt = (img->format == HdriCpuImage::Format::Bc6hSfloat) ? VK_FORMAT_BC6H_SFLOAT_BLOCK : VK_FORMAT_BC6H_UFLOAT_BLOCK;
    }
    // Full 4K/8K sky + water copies and staging exhaust the 512 MiB preset.
    // Reuse authored float mips so Low/Potato never upload those top levels.
    size_t first = 0;
    const int cap = mc2::options::current().baseQuality == 0 ? 512 : 1024;
    if (mc2::options::lowCost() && img->format == HdriCpuImage::Format::Rgba16f)
        while (first + 1 < img->levels.size() && img->dims[first].first > cap) ++first;
    for (size_t i = first; i < img->levels.size(); ++i) levels.emplace_back(img->levels[i].begin(), img->levels[i].end());
    dims.assign(img->dims.begin() + first, img->dims.end());
    generation = img->generation;
    return true;
}

// The load generation of the current CPU HDRI, 0 when there is none: the key an island re-imports on.
inline uint64_t hdriCpuGeneration() {
    const HdriCpuImage* img = gos_GetHdriCpuImage();
    return img ? img->generation : 0;
}

}  // namespace mc2vk

#endif  // MC2_VULKAN_ISLAND
