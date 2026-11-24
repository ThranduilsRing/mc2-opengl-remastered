// TERRAIN-VALLEY-SMOOTH-1: load-time VISUAL height for any map without a baked sidecar
// (mods, new maps). Same recipe as tools/terrain_beautify/visual_heightfield.py --reauth
// (CLIFF-SMOOTH-1), reduced to what runs in ~100 ms at mission load:
//   4x bilinear upsample (corner-pinned) -> slope-weighted diffusion (steep faces melt toward the
//   continuous surface, flats stay put) -> drift clamp against the bilinear baseline.
// Direction-free: slope comes from a Sobel gradient (not the worst of 4 axis neighbours), the cell-aligned
// weight mask is feathered with the same round kernel, and the diffusion uses the isotropic 9-point
// Laplacian, so a valley wall may run diagonally instead of stepping along the 128 wu grid axes.
// RENDER-ONLY: reads the gameplay elevations const and writes a separate visual grid; the gameplay
// height (MapData blocks, pathing, grounding) is never touched.
#pragma once
#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace mc2terrain {

// Deterministic value noise (fixed seed), 0..1, lattice spacing 1.
inline float ravineNoise(float x, float y, unsigned seed)
{
    auto hs = [seed](int i, int j) {
        unsigned h = static_cast<unsigned>(i) * 374761393u + static_cast<unsigned>(j) * 668265263u + seed * 2246822519u;
        h = (h ^ (h >> 13)) * 1274126177u;
        return static_cast<float>((h ^ (h >> 16)) & 0xffffu) / 65535.0f;
    };
    const int i = static_cast<int>(std::floor(x)), j = static_cast<int>(std::floor(y));
    float u = x - i, v = y - j;
    u = u * u * (3 - 2 * u); v = v * v * (3 - 2 * v);
    return (hs(i, j) * (1 - u) + hs(i + 1, j) * u) * (1 - v) + (hs(i, j + 1) * (1 - u) + hs(i + 1, j + 1) * u) * v;
}

// RAVINE-WEATHER-1: horizontal weathering of cliff walls, only inside cells the pathing map already blocks.
// A disk median (curvature flow) turns the 128 wu staircase into a flowing course while keeping wall height and
// steepness; a gradient-aligned domain warp (fbm rim wander, downslope-stretched gully notches), a talus fillet and a
// slump factor that varies along the wall make it read as an eroded ravine. Fine vertices within 1.5 fine steps of a
// passable cell are not moved, so walkable ground keeps its height. blocked: cellsSide^2 bytes, row-major, 1 = impassable.
inline void carveRavines(std::vector<float>& h, int V, const std::vector<unsigned char>& blocked, int cellsSide, float waterElev)
{
    const size_t n = static_cast<size_t>(V) * V;
    std::vector<float> d(n), toImp(n);
    for (int y = 0; y < V; ++y)
        for (int x = 0; x < V; ++x) {
            const int r = std::min(y * 3 / 4, cellsSide - 1);
            const int c = std::min(x * 3 / 4, cellsSide - 1);
            const bool imp = blocked[static_cast<size_t>(r) * cellsSide + c] != 0;
            d[static_cast<size_t>(x) + static_cast<size_t>(y) * V] = imp ? 1e9f : 0.0f;
            toImp[static_cast<size_t>(x) + static_cast<size_t>(y) * V] = imp ? 0.0f : 1e9f;
        }
    auto chamfer = [V](std::vector<float>& f) {   // two-pass chamfer distance in fine steps
        for (int pass = 0; pass < 2; ++pass)
            for (int k = 0; k < V * V; ++k) {
                const int i = pass ? V * V - 1 - k : k, x = i % V, y = i / V, s = pass ? 1 : -1;
                if (x + s >= 0 && x + s < V) f[i] = std::min(f[i], f[i + s] + 1.0f);
                if (y + s >= 0 && y + s < V) {
                    f[i] = std::min(f[i], f[i + s * V] + 1.0f);
                    if (x - 1 >= 0) f[i] = std::min(f[i], f[i + s * V - 1] + 1.4142f);
                    if (x + 1 < V) f[i] = std::min(f[i], f[i + s * V + 1] + 1.4142f);
                }
            }
    };
    chamfer(d); chamfer(toImp);
    constexpr int R = 9;   // median radius in fine steps (288 wu): wider than the staircase period
    std::vector<float> med = h, lo = h, hi = h, samp;
    for (int y = 0; y < V; ++y)
        for (int x = 0; x < V; ++x) {
            const size_t i = static_cast<size_t>(x) + static_cast<size_t>(y) * V;
            if (toImp[i] > 7.0f) continue;   // only the band the warp can sample
            samp.clear();
            for (int dy = -R; dy <= R; ++dy)
                for (int dx = -R; dx <= R; ++dx)
                    if (dx * dx + dy * dy <= R * R)
                        samp.push_back(h[static_cast<size_t>(std::clamp(x + dx, 0, V - 1)) + static_cast<size_t>(std::clamp(y + dy, 0, V - 1)) * V]);
            std::nth_element(samp.begin(), samp.begin() + samp.size() / 2, samp.end());
            med[i] = samp[samp.size() / 2];
            const auto [mn, mx] = std::minmax_element(samp.begin(), samp.end());
            lo[i] = *mn; hi[i] = *mx;
        }
    auto at = [&](const std::vector<float>& f, float x, float y) {
        x = std::clamp(x, 0.0f, V - 1.001f); y = std::clamp(y, 0.0f, V - 1.001f);
        const int ix = static_cast<int>(x), iy = static_cast<int>(y);
        const float u = x - ix, v = y - iy;
        const size_t i = static_cast<size_t>(ix) + static_cast<size_t>(iy) * V;
        return (f[i] * (1 - u) + f[i + 1] * u) * (1 - v) + (f[i + V] * (1 - u) + f[i + V + 1] * u) * v;
    };
    const std::vector<float> h0 = h;
    for (int y = 1; y < V - 1; ++y)
        for (int x = 1; x < V - 1; ++x) {
            const size_t i = static_cast<size_t>(x) + static_cast<size_t>(y) * V;
            const float t0 = std::clamp((d[i] - 1.5f) / 3.0f, 0.0f, 1.0f), wgt = t0 * t0 * (3 - 2 * t0);
            if (wgt <= 0.0f) continue;
            const float gx = med[i + 1] - med[i - 1], gy = med[i + V] - med[i - V], gl = std::sqrt(gx * gx + gy * gy) + 1e-3f;
            const float ux = gx / gl, uy = gy / gl, face = std::clamp(gl / 40.0f, 0.0f, 1.0f);
            const float fx = static_cast<float>(x), fy = static_cast<float>(y);
            // rim wander: 3 octaves at 128/64/32 wu
            const float wander = 2.6f * (ravineNoise(fx / 4, fy / 4, 1) - 0.5f) + 1.4f * (ravineNoise(fx / 2, fy / 2, 2) - 0.5f) + 0.8f * (ravineNoise(fx, fy, 3) - 0.5f);
            // gullies: noise stretched along the downslope direction -> narrow notches running down the wall, in patches
            const float along = (-uy * fx + ux * fy) / 2.2f, down = (ux * fx + uy * fy) / 14.0f;
            const float gully = std::pow(std::max(0.0f, 1.0f - 4.0f * std::fabs(ravineNoise(along, down, 4) - 0.5f)), 3.0f) * (ravineNoise(fx / 24, fy / 24, 5) > 0.35f ? 1.0f : 0.0f);
            const float shift = (wander - 3.5f * gully) * face;
            float hw = at(med, fx + ux * shift, fy + uy * shift);
            const float L = lo[i], H = hi[i];
            if (H - L > 24.0f) {   // profile: slumped (concave talus) vs steep rocky stretches, varying along the wall
                const float t = std::clamp((hw - L) / (H - L), 0.0f, 1.0f), slump = ravineNoise(fx / 20, fy / 20, 6);
                hw = L + (H - L) * std::pow(t, 1.0f + 0.9f * slump) + 28.0f * (1.0f - t) * t * (ravineNoise(fx * 0.9f, fy * 0.9f, 7) - 0.5f);   // scree roughness on the apron
            }
            float out = h0[i] + wgt * (hw - h0[i]);
            if (h0[i] > waterElev) out = std::max(out, waterElev + 1.0f);
            else out = std::min(out, waterElev - 1.0f);
            h[i] = out;
        }
}

inline std::optional<std::vector<float>> buildSmoothedVisualHeight(const float* elev, int mapSide, float waterElev,
                                                                   const std::vector<unsigned char>* blocked = nullptr, int cellsSide = 0)
{
    constexpr int F = 4;                  // visual factor, matches visual_height_4x.r32
    constexpr float kCell = 128.0f;       // world units per coarse cell
    constexpr float kFlatDrift = 4.0f, kCliffDrift = 72.0f;
    constexpr int kPasses = 110;
    if (!elev || mapSide < 3) return std::nullopt;
    const int N = mapSide, V = (N - 1) * F + 1;
    auto E = [&](int x, int y) { return elev[static_cast<size_t>(x) + static_cast<size_t>(y) * N]; };

    // slope weight per coarse vertex: Sobel gradient magnitude as the equivalent one-cell rise, 14..32 deg -> 0..1,
    // then grown one ring (a cliff edge needs its neighbours melted too)
    std::vector<float> cw(static_cast<size_t>(N) * N, 0.0f), cw2(cw.size(), 0.0f);
    auto Ec = [&](int x, int y) { return E(std::clamp(x, 0, N - 1), std::clamp(y, 0, N - 1)); };
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            const float gx = (Ec(x + 1, y - 1) + 2.0f * Ec(x + 1, y) + Ec(x + 1, y + 1)) - (Ec(x - 1, y - 1) + 2.0f * Ec(x - 1, y) + Ec(x - 1, y + 1));
            const float gy = (Ec(x - 1, y + 1) + 2.0f * Ec(x, y + 1) + Ec(x + 1, y + 1)) - (Ec(x - 1, y - 1) + 2.0f * Ec(x, y - 1) + Ec(x + 1, y - 1));
            const float rise = std::sqrt(gx * gx + gy * gy) * (1.0f / 4.0f);   // a one-cell step of height h reads h
            const float deg = std::atan(rise / kCell) * (180.0f / 3.14159265f);
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
    std::vector<float> pin(cw.size(), 0.0f);
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            bool isMax = true, isMin = true;
            for (int dy = -3; dy <= 3; ++dy)
                for (int dx = -3; dx <= 3; ++dx) {
                    const float o = E(std::clamp(x + dx, 0, N - 1), std::clamp(y + dy, 0, N - 1));
                    if (dx == 0 && dy == 0) continue;
                    isMax = isMax && E(x, y) > o;      // strict: a flat plateau or a cliff foot is not a summit
                    isMin = isMin && E(x, y) < o;
                }
            if (isMax || isMin) pin[static_cast<size_t>(x) + static_cast<size_t>(y) * N] = 1.0f;   // regional summit/pit stays pinned
        }

    // bilinear 4x baseline + bilinear-upsampled weight
    std::vector<float> base(static_cast<size_t>(V) * V), w(base.size()), pinUp(base.size());
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
            pinUp[i] = bl([&](int x, int y) { return pin[static_cast<size_t>(x) + static_cast<size_t>(y) * N]; });
        }

    // The coarse mask is cell-aligned: feather it with the round 9-point kernel (peak restored), then release the pins
    {
        std::vector<float> t(w.size());
        for (int p = 0; p < 16; ++p) {
            t = w;
            for (int y = 1; y < V - 1; ++y)
                for (int x = 1; x < V - 1; ++x) {
                    const size_t i = static_cast<size_t>(x) + static_cast<size_t>(y) * V;
                    t[i] = (4.0f * (w[i - 1] + w[i + 1] + w[i - V] + w[i + V]) + (w[i - V - 1] + w[i - V + 1] + w[i + V - 1] + w[i + V + 1]) + 8.0f * w[i]) / 28.0f;
                }
            w.swap(t);
        }
        for (size_t i = 0; i < w.size(); ++i) w[i] = std::min(1.0f, w[i] * 1.3f) * (1.0f - pinUp[i]);
    }

    // weighted diffusion, ping-pong; borders held (corner-pinned map edge)
    std::vector<float> h = base, nx(base.size());
    for (int p = 0; p < kPasses; ++p) {
        nx = h;
        for (int y = 1; y < V - 1; ++y)
            for (int x = 1; x < V - 1; ++x) {
                const size_t i = static_cast<size_t>(x) + static_cast<size_t>(y) * V;
                const float avg = (4.0f * (h[i - 1] + h[i + 1] + h[i - V] + h[i + V]) + (h[i - V - 1] + h[i - V + 1] + h[i + V - 1] + h[i + V + 1])) * (1.0f / 20.0f);
                nx[i] = h[i] + 0.9f * w[i] * (avg - h[i]);
            }
        h.swap(nx);
    }

    // soft drift clamp (C1 tanh) against the baseline: flats <= 4 wu, steep faces <= 72 wu
    for (size_t i = 0; i < h.size(); ++i) {
        const float lim = kFlatDrift + (kCliffDrift - kFlatDrift) * w[i];
        h[i] = base[i] + lim * std::tanh((h[i] - base[i]) / lim);
        if (base[i] > waterElev) h[i] = std::max(h[i], waterElev + 1.0f);   // never cross the water plane: shores and islands stay put
        else h[i] = std::min(h[i], waterElev - 1.0f);
    }
    if (blocked && cellsSide > 0 && blocked->size() == static_cast<size_t>(cellsSide) * cellsSide) carveRavines(h, V, *blocked, cellsSide, waterElev);
    return h;
}

} // namespace mc2terrain
