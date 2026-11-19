#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace RenderCore {
// Mission-load mover texture prewarm: the first frame draws the player's force and whatever stands near it, so pass 2 of the
// prewarm takes only the non-home movers within `radius` (world units) of any home mover. Everything else uploads lazily
// when it first comes into view.
struct PrewarmMover { float x, y; bool home; };
inline constexpr float kPrewarmNearHomeRadius = 2560.0f;        // 20 terrain vertices (128 units each): about one default view around the force
inline constexpr uint64_t kPrewarmPass2ByteCap = 256ull << 20;   // backstop: pass 2 stops after this many device bytes

// Indices into `movers` of the non-home movers pass 2 should upload, in input order.
inline std::vector<size_t> prewarmNearHome(std::span<const PrewarmMover> movers, float radius = kPrewarmNearHomeRadius) {
    std::vector<size_t> picked;
    const float r2 = radius * radius;
    for (size_t i = 0; i < movers.size(); ++i) {
        if (movers[i].home) continue;
        for (const PrewarmMover& h : movers) {
            if (!h.home) continue;
            const float dx = movers[i].x - h.x, dy = movers[i].y - h.y;
            if (dx * dx + dy * dy <= r2) { picked.push_back(i); break; }
        }
    }
    return picked;
}
}
