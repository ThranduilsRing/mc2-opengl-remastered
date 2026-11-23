#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "../shaders/vulkan/hdr_transfer.glsl"

namespace mc2vk::hdr {
inline uint8_t quantizeSdr(double value) {
    return static_cast<uint8_t>(std::lround(std::clamp(std::isfinite(value) ? value : 0.0, 0.0, 1.0) * 255.0));
}
// Match present_encode's premultiplied UI composition, then preview at paper white.
inline uint8_t captureSdr(double scene, uint8_t ui, uint8_t alpha) {
    return quantizeSdr(hdrEncode(hdrUiComposite(std::max(0.0, scene), ui / 255.0, alpha / 255.0)));
}
}
