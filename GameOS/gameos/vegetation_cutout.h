#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

namespace mc2::vegetation {
inline float cutoutCoverage(std::span<const uint8_t> rgba, float scale = 1.0f) {
    size_t covered = 0;
    for (size_t i = 3; i < rgba.size(); i += 4)
        if (std::lround(static_cast<float>(rgba[i]) * scale) >= 128) ++covered;
    return rgba.empty() ? 0.0f : static_cast<float>(covered) / static_cast<float>(rgba.size() / 4);
}

// Keep the closest representable mip coverage at the same 0.5 cutout threshold.
// Fully transparent texels remain transparent even when a tiny mip cannot
// represent the requested fractional coverage.
inline void preserveCutoutAlpha(std::span<uint8_t> rgba, float target) {
    float lo = 0.0f, hi = 256.0f, best = 1.0f;
    float error = std::abs(cutoutCoverage(rgba) - target);
    for (int iteration = 0; iteration < 24; ++iteration) {
        const float scale = (lo + hi) * 0.5f;
        const float coverage = cutoutCoverage(rgba, scale);
        if (const float delta = std::abs(coverage - target); delta < error) {
            best = scale; error = delta;
        }
        if (coverage < target) lo = scale; else hi = scale;
    }
    for (size_t i = 3; i < rgba.size(); i += 4)
        rgba[i] = static_cast<uint8_t>(std::clamp(std::lround(static_cast<float>(rgba[i]) * best), 0L, 255L));
}

// Filtering a cutout must find its edge colours outside coverage, rather than
// average the edge with the transparent image's black background.
inline void dilateCutoutRgb(std::vector<uint8_t>& rgba, uint32_t width, uint32_t height) {
    if (rgba.size() != static_cast<size_t>(width) * height * 4) return;
    std::vector<uint8_t> known(static_cast<size_t>(width) * height);
    for (size_t i = 0; i < known.size(); ++i) known[i] = rgba[i * 4 + 3] != 0;
    for (int iteration = 0; iteration < 8; ++iteration) {
        auto next = rgba;
        auto grown = known;
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                const size_t i = static_cast<size_t>(y) * width + x;
                if (known[i]) continue;
                uint32_t sums[3]{}, count = 0;
                const auto add = [&](size_t j) {
                    if (!known[j]) return;
                    for (size_t c = 0; c < 3; ++c) sums[c] += rgba[j * 4 + c];
                    ++count;
                };
                if (x) add(i - 1);
                if (x + 1 < width) add(i + 1);
                if (y) add(i - width);
                if (y + 1 < height) add(i + width);
                if (count) {
                    for (size_t c = 0; c < 3; ++c) next[i * 4 + c] = static_cast<uint8_t>(sums[c] / count);
                    grown[i] = 1;
                }
            }
        }
        rgba.swap(next); known.swap(grown);
    }
}
} // namespace mc2::vegetation
