#pragma once
#include <cmath>

namespace mc2::vegetation {

// Cheap, deterministic hash in [0, 1).  Inputs are world-grid coordinates
// plus a per-instance discriminator so each card within a cell differs.
[[nodiscard]] inline float hashf(float a, float b) noexcept
{
    return fabsf(fmodf(sinf(a * 127.1f + b * 311.7f) * 43758.5f, 1.0f));
}

// Bilinear-interpolated hash on a coarse grid → smooth spatial blobs.
// Returns [0,1].  Used as secondary variation WITHIN terrain-type zones;
// terrain type is still the primary gate.
[[nodiscard]] inline float macroZone(float wx, float wy, float cellSize = 512.0f) noexcept
{
    const float gx = wx / cellSize;
    const float gy = wy / cellSize;
    const int cx = static_cast<int>(gx >= 0.0f ? gx : gx - 1.0f);
    const int cy = static_cast<int>(gy >= 0.0f ? gy : gy - 1.0f);
    float fx = gx - static_cast<float>(cx);
    float fy = gy - static_cast<float>(cy);
    fx = fx * fx * (3.0f - 2.0f * fx);
    fy = fy * fy * (3.0f - 2.0f * fy);
    const float h00 = hashf(static_cast<float>(cx    ), static_cast<float>(cy    ));
    const float h10 = hashf(static_cast<float>(cx + 1), static_cast<float>(cy    ));
    const float h01 = hashf(static_cast<float>(cx    ), static_cast<float>(cy + 1));
    const float h11 = hashf(static_cast<float>(cx + 1), static_cast<float>(cy + 1));
    return h00*(1-fx)*(1-fy) + h10*fx*(1-fy) + h01*(1-fx)*fy + h11*fx*fy;
}

} // namespace mc2::vegetation
