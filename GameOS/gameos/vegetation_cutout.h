#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

namespace mc2::vegetation {
inline float cutoutCoverage(std::span<const uint8_t> rgba, float scale = 1.0f, long threshold = 128) {
    size_t covered = 0;
    for (size_t i = 3; i < rgba.size(); i += 4)
        if (std::lround(static_cast<float>(rgba[i]) * scale) >= threshold) ++covered;
    return rgba.empty() ? 0.0f : static_cast<float>(covered) / static_cast<float>(rgba.size() / 4);
}

// Keep the closest representable mip coverage at the same 0.5 cutout threshold.
// Fully transparent texels remain transparent even when a tiny mip cannot
// represent the requested fractional coverage.
inline void preserveCutoutAlpha(std::span<uint8_t> rgba, float target, long threshold = 128) {
    float lo = 0.0f, hi = 256.0f, best = 1.0f;
    float error = std::abs(cutoutCoverage(rgba, 1.0f, threshold) - target);
    for (int iteration = 0; iteration < 24; ++iteration) {
        const float scale = (lo + hi) * 0.5f;
        const float coverage = cutoutCoverage(rgba, scale, threshold);
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
// A missing or undecodable card must not switch off every card: each layer takes
// its own tile, else the nearest present tile of its group of four (grass 0-3,
// shrubs and saplings 4-7), else any present tile. Empty when no tile is present.
inline std::vector<int> cardLayerSources(std::span<const bool> present) {
    std::vector<int> source(present.size(), -1);
    int any = -1;
    for (size_t i = 0; i < present.size(); ++i) if (present[i]) { any = static_cast<int>(i); break; }
    if (any < 0) return {};
    for (size_t i = 0; i < present.size(); ++i) {
        if (present[i]) { source[i] = static_cast<int>(i); continue; }
        const size_t group = i / 4 * 4;
        int best = -1;
        for (size_t d = 1; d < 4 && best < 0; ++d) {
            if (i >= group + d && present[i - d]) best = static_cast<int>(i - d);
            else if (i + d < group + 4 && i + d < present.size() && present[i + d]) best = static_cast<int>(i + d);
        }
        source[i] = best >= 0 ? best : any;
    }
    return source;
}

// PROP-ALPHA-MIP-1: the alpha-ON prop array (tree cards, alpha-mixed buildings, fences) is cut in shaders/vulkan/
// color_prop_tier1.frag (propCoverage) and shadow_prop_alpha.frag at mix(0.5, 0.9, clamp(lod * 0.5, 0, 1)). The
// same cut as an 8-bit alpha threshold for mip level `mip`, so a layer's mips can preserve coverage AT that cut.
inline long propCutThreshold(uint32_t mip) {
    const float t = std::clamp(static_cast<float>(mip) * 0.5f, 0.0f, 1.0f);
    return std::lround((0.5f + 0.4f * t) * 255.0f);
}

// True for a cutout with solid mass (a canopy, an alpha-mixed wall) and false for thin lattice (chain link, wire,
// railings), whose box-filtered mips must keep averaging down to see-through. Data-driven: erode the 0.5-cutout
// mask by ~1/64 of the layer (>= 1 texel, the border counts as empty); a canopy keeps a solid core, wire is gone.
inline bool isSolidCutout(std::span<const uint8_t> rgba, uint32_t width, uint32_t height) {
    if (!width || !height || rgba.size() != static_cast<size_t>(width) * height * 4) return false;
    const uint32_t r = std::max(1u, std::min(width, height) / 64u);
    std::vector<uint8_t> in(static_cast<size_t>(width) * height), tmp(in.size());
    size_t covered = 0;
    for (size_t i = 0; i < in.size(); ++i) { in[i] = rgba[i * 4 + 3] >= 128; covered += in[i]; }
    if (!covered) return false;
    for (uint32_t y = 0; y < height; ++y)          // erode along x
        for (uint32_t x = 0; x < width; ++x) {
            uint8_t keep = x >= r && x + r < width;
            for (uint32_t k = 0; keep && k <= 2 * r; ++k) keep = in[static_cast<size_t>(y) * width + x - r + k];
            tmp[static_cast<size_t>(y) * width + x] = keep;
        }
    size_t core = 0;
    for (uint32_t y = 0; y < height; ++y)          // erode along y
        for (uint32_t x = 0; x < width; ++x) {
            uint8_t keep = y >= r && y + r < height;
            for (uint32_t k = 0; keep && k <= 2 * r; ++k) keep = tmp[static_cast<size_t>(y - r + k) * width + x];
            core += keep;
        }
    return static_cast<float>(core) / static_cast<float>(covered) >= 0.25f;
}
} // namespace mc2::vegetation
