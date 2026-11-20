// Terrain-owned filtered visual height field for the fragment's fine-normal fetch.
// terrain_tier1.frag evaluates cliffVisualHeight(p) 16 times per fragment; the value is camera independent,
// so it is baked once per bake version: out[(y + 1) * (V + 2) + (x + 1)] = cliffVisualHeight(raw, x, y) for
// x, y in -1..V (a one-cell apron; each tap clamps to the bake exactly like cliffRawHeight in the shader).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>

namespace terrain_fine_height {
#include "../../shaders/vulkan/include/terrain_cliff_surface.glsl"

inline size_t apronCount(int side) { return static_cast<size_t>(side + 2) * static_cast<size_t>(side + 2); }

inline void bake(const float* raw, int side, float* out) {
    const auto tap = [raw, side](int x, int y) {
        return raw[static_cast<size_t>(std::clamp(y, 0, side - 1)) * static_cast<size_t>(side) + static_cast<size_t>(std::clamp(x, 0, side - 1))];
    };
    for (int y = -1; y <= side; ++y)
        for (int x = -1; x <= side; ++x)
            *out++ = cliffVisualHeight(tap, x, y);
}
} // namespace terrain_fine_height
