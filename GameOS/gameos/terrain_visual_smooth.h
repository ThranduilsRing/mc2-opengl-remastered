// TERRAIN-VALLEY-SMOOTH-1: load-time VISUAL height for any map without a baked sidecar
// (mods, new maps). Same recipe as tools/terrain_beautify/visual_heightfield.py --reauth
// (CLIFF-SMOOTH-1), reduced to what runs in ~100 ms at mission load:
//   4x bilinear upsample (corner-pinned) -> slope-weighted diffusion (steep faces melt toward the
//   continuous surface, flats stay put) -> drift clamp against the bilinear baseline.
// RENDER-ONLY: reads the gameplay elevations const and writes a separate visual grid; the gameplay
// height (MapData blocks, pathing, grounding) is never touched.
#pragma once
#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace mc2terrain {

inline std::optional<std::vector<float>> buildSmoothedVisualHeight(const float* elev, int mapSide)
{
    constexpr int F = 4;                  // visual factor, matches visual_height_4x.r32
    constexpr float kCell = 128.0f;       // world units per coarse cell
    constexpr float kFlatDrift = 4.0f, kCliffDrift = 56.0f;
    constexpr int kPasses = 90;
    if (!elev || mapSide < 3) return std::nullopt;
    const int N = mapSide, V = (N - 1) * F + 1;
    auto E = [&](int x, int y) { return elev[static_cast<size_t>(x) + static_cast<size_t>(y) * N]; };

    // slope weight per coarse vertex: worst one-sided slope to a 4-neighbour, 14..32 deg -> 0..1, then
    // grown one ring (a cliff edge needs its neighbours melted too)
    std::vector<float> cw(static_cast<size_t>(N) * N, 0.0f), cw2(cw.size(), 0.0f);
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            float d = 0.0f;
            if (x > 0) d = std::max(d, std::fabs(E(x, y) - E(x - 1, y)));
            if (x + 1 < N) d = std::max(d, std::fabs(E(x, y) - E(x + 1, y)));
            if (y > 0) d = std::max(d, std::fabs(E(x, y) - E(x, y - 1)));
            if (y + 1 < N) d = std::max(d, std::fabs(E(x, y) - E(x, y + 1)));
            const float deg = std::atan(d / kCell) * (180.0f / 3.14159265f);
            cw[static_cast<size_t>(x) + static_cast<size_t>(y) * N] = std::clamp((deg - 14.0f) / 18.0f, 0.0f, 1.0f);
        }
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            float m = 0.0f;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int xx = std::clamp(x + dx, 0, N - 1), yy = std::clamp(y + dy, 0, N - 1);
                    m = std::max(m, cw[static_cast<size_t>(xx) + static_cast<size_t>(yy) * N]);
                }
            cw2[static_cast<size_t>(x) + static_cast<size_t>(y) * N] = m;
        }

    // bilinear 4x baseline + bilinear-upsampled weight
    std::vector<float> base(static_cast<size_t>(V) * V), w(base.size());
    for (int fy = 0; fy < V; ++fy)
        for (int fx = 0; fx < V; ++fx) {
            const int cx = std::clamp(fx / F, 0, N - 2), cy = std::clamp(fy / F, 0, N - 2);
            const float tx = static_cast<float>(fx - cx * F) / F, ty = static_cast<float>(fy - cy * F) / F;
            auto bl = [&](auto get) {
                return (get(cx, cy) * (1 - tx) + get(cx + 1, cy) * tx) * (1 - ty) +
                       (get(cx, cy + 1) * (1 - tx) + get(cx + 1, cy + 1) * tx) * ty;
            };
            const size_t i = static_cast<size_t>(fx) + static_cast<size_t>(fy) * V;
            base[i] = bl([&](int x, int y) { return E(x, y); });
            w[i] = bl([&](int x, int y) { return cw2[static_cast<size_t>(x) + static_cast<size_t>(y) * N]; });
        }

    // weighted diffusion, ping-pong; borders held (corner-pinned map edge)
    std::vector<float> h = base, nx(base.size());
    for (int p = 0; p < kPasses; ++p) {
        nx = h;
        for (int y = 1; y < V - 1; ++y)
            for (int x = 1; x < V - 1; ++x) {
                const size_t i = static_cast<size_t>(x) + static_cast<size_t>(y) * V;
                const float avg = 0.25f * (h[i - 1] + h[i + 1] + h[i - V] + h[i + V]);
                nx[i] = h[i] + 0.9f * w[i] * (avg - h[i]);
            }
        h.swap(nx);
    }

    // soft drift clamp (C1 tanh) against the baseline: flats <= 4 wu, steep faces <= 56 wu
    for (size_t i = 0; i < h.size(); ++i) {
        const float lim = kFlatDrift + (kCliffDrift - kFlatDrift) * w[i];
        h[i] = base[i] + lim * std::tanh((h[i] - base[i]) / lim);
    }
    return h;
}

} // namespace mc2terrain
