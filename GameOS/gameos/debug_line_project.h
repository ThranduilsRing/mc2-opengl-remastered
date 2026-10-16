// Projection of a world-space debug segment to window pixels, for the overlay that draws debug primitives.
// The matrix is the scene view-projection exactly as the shaders consume it: column-major, clip = M * (x, y, z, 1).
#pragma once
#include <cmath>

namespace DebugRenderer {

struct ScreenSegment { float x0, y0, x1, y1; };

// Returns false when the segment is entirely behind the near plane or any input is non-finite. A segment that crosses the
// near plane is cut at it, so a line running through the camera still draws its visible part. Window y runs down.
inline bool projectSegment(const float* m, const float a[3], const float b[3], float width, float height, ScreenSegment* out) {
    if (!m || !out || !(width > 0.0f) || !(height > 0.0f)) return false;
    float ca[4], cb[4];
    for (int r = 0; r < 4; ++r) {
        ca[r] = m[r] * a[0] + m[4 + r] * a[1] + m[8 + r] * a[2] + m[12 + r];
        cb[r] = m[r] * b[0] + m[4 + r] * b[1] + m[8 + r] * b[2] + m[12 + r];
    }
    for (int r = 0; r < 4; ++r)
        if (!std::isfinite(ca[r]) || !std::isfinite(cb[r])) return false;
    constexpr float kNearW = 1e-3f;
    const bool aIn = ca[3] > kNearW, bIn = cb[3] > kNearW;
    if (!aIn && !bIn) return false;
    if (aIn != bIn) {
        float* out4 = aIn ? cb : ca;
        const float* in4 = aIn ? ca : cb;
        const float t = (kNearW - in4[3]) / (out4[3] - in4[3]);
        for (int r = 0; r < 4; ++r) out4[r] = in4[r] + (out4[r] - in4[r]) * t;
    }
    out->x0 = (ca[0] / ca[3] * 0.5f + 0.5f) * width;
    out->y0 = (1.0f - (ca[1] / ca[3] * 0.5f + 0.5f)) * height;
    out->x1 = (cb[0] / cb[3] * 0.5f + 0.5f) * width;
    out->y1 = (1.0f - (cb[1] / cb[3] * 0.5f + 0.5f)) * height;
    return true;
}

}  // namespace DebugRenderer
