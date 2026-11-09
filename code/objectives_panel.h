#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mc2::objectives_panel {

// Preserve the stock panel through 1080p. Above that, grow with the square
// root of the usable HUD canvas height instead of doubling at 4K.
inline float scaleForCanvasHeight(int height) {
    return height > 1080 ? std::sqrt(1080.f / static_cast<float>(height)) : 1.f;
}

// The stock objective tiles have black baked into RGB with alpha=255.
// Key only the private marker copy; the shared HUD atlas contains real black art.
inline void clearMarkerBackground(uint8_t* rgba) {
    if (rgba[0] == 0 && rgba[1] == 0 && rgba[2] == 0)
        rgba[3] = 0;
}

} // namespace mc2::objectives_panel
