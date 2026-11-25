// terrain_forest_mask.h - tree-canopy density mask for the terrain forest-floor detail layer.
//
// WHAT: splats tree placements (world x,y pairs) into a small R8 grid laid over the colormap atlas extent: each tree adds a
// canopy-sized disc with a soft falloff; overlapping discs union (1 - prod(1 - a)), so a grove saturates at 255 and an isolated
// tree stays a soft blob. The terrain shader reads it as the forest share of the ground under the canopy.
// WHY: the macro colour cannot tell "under trees" from "dark green hill"; tree placement is the ground truth.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace mc2terrain {

// Atlas convention shared with the colormap: texel (x,y) centre = world (originX + (x+.5)/w*span, originY - (y+.5)/h*span).
// `xy` holds world (x,y) pairs. `out` is resized to w*h.
inline void splatForestMask(std::span<const float> xy, float originX, float originY, float span, int w, int h,
                            float canopyRadius, std::vector<uint8_t>& out) {
    const bool validGrid = w > 0 && h > 0;
    out.assign(validGrid ? static_cast<size_t>(w) * static_cast<size_t>(h) : 0, 0);
    const size_t count = xy.size() / 2;
    if (count == 0 || !validGrid || !(span > 0.0f) || !(canopyRadius > 0.0f)) return;
    const float texPerWuX = static_cast<float>(w) / span, texPerWuY = static_cast<float>(h) / span;
    std::vector<float> clear(out.size(), 1.0f);   // running product of (1 - a)
    const float invR2 = 1.0f / (canopyRadius * canopyRadius);
    for (size_t i = 0; i < count; ++i) {
        const float tx = (xy[2 * i] - originX) * texPerWuX - 0.5f, ty = (originY - xy[2 * i + 1]) * texPerWuY - 0.5f;
        const int x0 = std::max(0, static_cast<int>(std::floor(tx - canopyRadius * texPerWuX)));
        const int x1 = std::min(w - 1, static_cast<int>(std::ceil(tx + canopyRadius * texPerWuX)));
        const int y0 = std::max(0, static_cast<int>(std::floor(ty - canopyRadius * texPerWuY)));
        const int y1 = std::min(h - 1, static_cast<int>(std::ceil(ty + canopyRadius * texPerWuY)));
        for (int y = y0; y <= y1; ++y) for (int x = x0; x <= x1; ++x) {
            const float dx = (static_cast<float>(x) - tx) / texPerWuX, dy = (static_cast<float>(y) - ty) / texPerWuY;
            const float d2 = (dx * dx + dy * dy) * invR2;
            if (d2 >= 1.0f) continue;
            const float u = 1.0f - d2;       // 1 at the trunk, 0 at the canopy edge
            clear[static_cast<size_t>(y) * w + x] *= 1.0f - u * u * (3.0f - 2.0f * u);   // smoothstep falloff
        }
    }
    for (size_t k = 0; k < out.size(); ++k) out[k] = static_cast<uint8_t>(std::lround((1.0f - clear[k]) * 255.0f));
}

}  // namespace mc2terrain
