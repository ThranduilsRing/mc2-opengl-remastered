#pragma once

#include <algorithm>
#include <cmath>

namespace gui_window_policy {
inline float scaleForHeight(float height) {
    return std::clamp(height / 1080.0f, 1.0f, 4.0f);
}

// Oversized windows keep their title bar reachable; ordinary windows fit wholly.
inline float clampPosition(float position, float size, float origin, float extent) {
    if (extent <= 0.0f) return position;
    if (!std::isfinite(position)) return origin;
    return std::clamp(position, origin, origin + std::max(0.0f, extent - size));
}
}
