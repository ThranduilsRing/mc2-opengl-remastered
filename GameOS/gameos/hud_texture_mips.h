#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mc2hud {
// Power-of-two RGBA8 atlas. Keep filtering in cached CPU memory: mapped
// HOST_COHERENT storage can be uncached, making the read/modify/write pass stall.
inline std::vector<uint8_t> premultipliedMips(const std::vector<uint8_t>& rgba, size_t side) {
    const size_t baseBytes = side * side * 4;
    std::vector<uint8_t> result(rgba.begin(), rgba.begin() + baseBytes);
    result.resize((baseBytes * 4 - 4) / 3);
    auto* pixels = result.data();
    for (size_t i = 0; i < baseBytes; i += 4)
        for (size_t c = 0; c < 3; ++c)
            pixels[i+c] = (static_cast<uint32_t>(pixels[i+c]) * pixels[i+3] + 127u) / 255u;
    size_t offset = 0, next = baseBytes;
    for (size_t w = side; w > 1; w /= 2) {
        for (size_t y = 0; y < w/2; ++y) for (size_t x = 0; x < w/2; ++x)
            for (size_t c = 0; c < 4; ++c) {
                const size_t p = offset + ((2*y)*w + 2*x)*4 + c;
                pixels[next + (y*(w/2)+x)*4+c] = (static_cast<uint32_t>(pixels[p]) + pixels[p+4] + pixels[p+w*4] + pixels[p+w*4+4] + 2u) / 4u;
            }
        offset = next; next += (w/2)*(w/2)*4;
    }
    return result;
}
} // namespace mc2hud
