#pragma once
// VEG-CEMENT-1: pure curves that turn the overlay bake's sub-cell hard-surface
// coverage into vegetation density and crack weeds. Header-only so the offline
// doctest tier can drive them without the engine.
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mc2::vegetation {

// Match terrain_tier1.frag's cement-word ownership and analytic 0.5 cuts.
// Fetch returns zero outside the map. Coverage uses world-space texel centres;
// screen derivative antialiasing belongs to the renderer, not placement.
template<class Fetch>
[[nodiscard]] float cementWordCoverage(int row, int col, float u, float v,
                                      float atlasAlpha, Fetch wordAt) {
    const uint32_t word = wordAt(row, col);
    if (!(word & 0x80000000u)) return 0.0f;
    if (!(word & 0x40000000u)) return 1.0f;
    if (word & 0x00800000u) return atlasAlpha;
    const auto has = [&](int dr, int dc) { return (wordAt(row + dr, col + dc) & 0x80000000u) != 0; };
    const int cfg = ((has(0,-1) && has(-1,0) && has(-1,-1)) ? 8 : 0)
                  | ((has(0,1) && has(-1,0) && has(-1,1)) ? 4 : 0)
                  | ((has(0,1) && has(1,0) && has(1,1)) ? 2 : 0)
                  | ((has(0,-1) && has(1,0) && has(1,-1)) ? 1 : 0);
    const auto cut = [](float x) { return x < 0.5f ? 1.0f : 0.0f; };
    switch (cfg) {
    case 0: return 0; case 15: return 1;
    case 8: return cut(u+v); case 4: return cut(1-u+v);
    case 2: return cut(2-u-v); case 1: return cut(u+1-v);
    case 7: return 1-cut(u+v); case 11: return 1-cut(1-u+v);
    case 13: return 1-cut(2-u-v); case 14: return 1-cut(u+1-v);
    case 12: return cut(v); case 6: return 1-cut(u);
    case 3: return 1-cut(v); case 9: return cut(u);
    case 10: return std::max(cut(u+v), cut(2-u-v));
    case 5: return std::max(cut(1-u+v), cut(u+1-v));
    }
    return 0;
}

[[nodiscard]] inline float renderedHardSurfaceCoverage(float cement, float overlay, bool overlayDrawn) {
    return std::max(cement, overlayDrawn ? overlay : 0.0f);
}

inline constexpr float kHardFalloffLo   = 0.15f;  // coverage where cover starts thinning
inline constexpr float kHardFalloffHi   = 0.60f;  // coverage where normal cover is gone
inline constexpr float kWeedMinCoverage = 0.50f;  // weeds only live on real hard surface
inline constexpr float kWeedDensity     = 0.16f;  // fraction of full density kept as weeds
inline constexpr unsigned kWeedFlagBit  = 0x40u;  // atlasFrame bit 6 (bits 0-5 are frame + card role)

[[nodiscard]] inline float smoothstep01(float lo, float hi, float x) noexcept {
    const float t = std::clamp((x - lo) / (hi - lo), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Normal-cover density multiplier: 1 on open ground, 0 well inside a hard surface.
[[nodiscard]] inline float coverToDensityMul(float cov) noexcept {
    return 1.0f - smoothstep01(kHardFalloffLo, kHardFalloffHi, cov);
}

// Crack/edge term in [0,1] from the coverage gradient magnitude (coverage per
// world unit) and the distance to the nearest paving seam (world units).
[[nodiscard]] inline float crackTerm(float gradPerWu, float seamDistWu) noexcept {
    const float edge = std::clamp(gradPerWu * 10.0f, 0.0f, 1.0f);   // 0.1/wu == a full-ramp edge
    const float seam = (seamDistWu < 2.0f) ? 0.6f : 0.0f;
    return std::max(edge, seam);
}

// Probability that an instance that lost the cover test becomes a crack weed.
[[nodiscard]] inline float weedKeepProbability(float cov, float crack) noexcept {
    if (cov < kWeedMinCoverage) return 0.0f;
    return kWeedDensity * (0.15f + 0.85f * std::clamp(crack, 0.0f, 1.0f));
}

enum class HardSurfacePlacement { Cover, TransitionCover, Weed, Reject };

// Sample the visible paving footprint independently of the terrain's cell class.
// Sampler and hash parameters keep the placement rule testable without the engine.
template<class Coverage, class Hash>
[[nodiscard]] HardSurfacePlacement hardSurfacePlacement(float wx, float wy, float tileWu,
                                                       Coverage coverage, Hash hash) {
    const float cov = coverage(wx, wy);
    if (cov <= 0.0f) return HardSurfacePlacement::Cover;
    const float hc = hash(wx * 53.7f + 17.0f, wy * 91.3f + 3.0f);
    if (hc < coverToDensityMul(cov))
        return cov > kHardFalloffLo ? HardSurfacePlacement::TransitionCover : HardSurfacePlacement::Cover;

    constexpr float kTap = 6.0f;
    const float gx = (coverage(wx + kTap, wy) - coverage(wx - kTap, wy)) / (2.0f * kTap);
    const float gy = (coverage(wx, wy + kTap) - coverage(wx, wy - kTap)) / (2.0f * kTap);
    const float grad = std::sqrt(gx * gx + gy * gy);
    const float sx = std::fabs(std::fmod(wx, tileWu));
    const float sy = std::fabs(std::fmod(wy, tileWu));
    const float seam = std::min({sx, tileWu - sx, sy, tileWu - sy});
    const float hw = hash(wy * 71.9f + 5.0f, wx * 29.3f + 11.0f);
    return hw < weedKeepProbability(cov, crackTerm(grad, seam))
        ? HardSurfacePlacement::Weed : HardSurfacePlacement::Reject;
}

}  // namespace mc2::vegetation
