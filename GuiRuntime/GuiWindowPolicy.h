#pragma once

#include <algorithm>
#include <cmath>

namespace gui_window_policy {
// Use the same IO flag selector in production and the capture regression rows.
template<class IO> inline bool capturesKeyboard(const IO& io) { return io.WantTextInput; }
inline float scaleForHeight(float height) {
    return std::clamp(height / 1080.0f, 1.0f, 4.0f);
}

// Leave partially visible windows alone; recover windows wholly off the work area.
inline float clampPosition(float position, float size, float origin, float extent) {
    if (extent <= 0.0f) return position;
    if (!std::isfinite(position)) return origin;
    if (position + size > origin && position < origin + extent) return position;
    return std::clamp(position, origin, origin + std::max(0.0f, extent - size));
}
}
