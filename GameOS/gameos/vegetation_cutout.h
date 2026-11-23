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
    std::vector<uint8_t> next, grown;
    for (int iteration = 0; iteration < 8; ++iteration) {
        next = rgba;
        grown = known;
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
// mask by ~1/64 of the image (>= 1 texel, the border counts as empty); a canopy keeps a solid core, wire is gone.
// contentEdge: the smaller edge of the image itself when it sits in the corner of a larger array layer. The radius must
// follow the image, not the shared array (one 1024 tree atlas made every 128 canopy in its array erode to nothing).
inline bool isSolidCutout(std::span<const uint8_t> rgba, uint32_t width, uint32_t height, uint32_t contentEdge = 0) {
    if (!width || !height || rgba.size() != static_cast<size_t>(width) * height * 4) return false;
    const uint32_t edge = contentEdge ? std::min(contentEdge, std::min(width, height)) : std::min(width, height);
    const uint32_t r = std::max(1u, edge / 64u);
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

// BUILDING-GLASS-1: a window of an alpha-mixed wall is a hole the 0.5 cutout discards, so the sky or the next building
// shows through it and there is no back face behind it. A hole of the cutout mask that does not reach the content
// edge (a pane inside solid mass), is roughly rectangular and not tiny is glass: opaque near-black. Holes that touch
// the silhouette (notches, antenna gaps) and lattice-sized or ragged ones (chain-link diamonds) stay cut.
// contentW/H: the image's own extent in the top-left of a larger array layer. Returns the texels filled.
inline size_t fillEnclosedGlassHoles(std::span<uint8_t> rgba, uint32_t width, uint32_t height, uint32_t contentW, uint32_t contentH) {
    contentW = std::min(contentW ? contentW : width, width);
    contentH = std::min(contentH ? contentH : height, height);
    if (rgba.size() != static_cast<size_t>(width) * height * 4 || contentW < 8 || contentH < 8) return 0;
    const auto clear = [&](uint32_t x, uint32_t y) { return rgba[(static_cast<size_t>(y) * width + x) * 4 + 3] < 128; };
    std::vector<uint32_t> region(static_cast<size_t>(contentW) * contentH, 0u), stack;   // 0 unseen, 1 outside, 2.. hole id
    const auto at = [&](uint32_t x, uint32_t y) -> uint32_t& { return region[static_cast<size_t>(y) * contentW + x]; };
    uint32_t box[4];
    const auto flood = [&](uint32_t sx, uint32_t sy, uint32_t id) {
        size_t area = 0;
        stack.assign(1, sy * contentW + sx);
        at(sx, sy) = id;
        box[0] = box[2] = sx; box[1] = box[3] = sy;
        while (!stack.empty()) {
            const uint32_t v = stack.back(); stack.pop_back(); ++area;
            const uint32_t x = v % contentW, y = v / contentW;
            box[0] = std::min(box[0], x); box[1] = std::min(box[1], y); box[2] = std::max(box[2], x); box[3] = std::max(box[3], y);
            const int64_t sx = static_cast<int64_t>(x), sy = static_cast<int64_t>(y);
            const int64_t nx[4] = {sx - 1, sx + 1, sx, sx};
            const int64_t ny[4] = {sy, sy, sy - 1, sy + 1};
            for (int k = 0; k < 4; ++k) {
                if (nx[k] < 0 || ny[k] < 0 || nx[k] >= static_cast<int64_t>(contentW) || ny[k] >= static_cast<int64_t>(contentH)) continue;
                const uint32_t ux = static_cast<uint32_t>(nx[k]), uy = static_cast<uint32_t>(ny[k]);
                if (at(ux, uy) || !clear(ux, uy)) continue;
                at(ux, uy) = id;
                stack.push_back(uy * contentW + ux);
            }
        }
        return area;
    };
    for (uint32_t x = 0; x < contentW; ++x) {      // the clear mask reachable from the content border is the outside
        if (clear(x, 0) && !at(x, 0)) flood(x, 0, 1);
        if (clear(x, contentH - 1) && !at(x, contentH - 1)) flood(x, contentH - 1, 1);
    }
    for (uint32_t y = 0; y < contentH; ++y) {
        if (clear(0, y) && !at(0, y)) flood(0, y, 1);
        if (clear(contentW - 1, y) && !at(contentW - 1, y)) flood(contentW - 1, y, 1);
    }
    size_t filled = 0;
    const size_t minArea = std::max<size_t>(24, static_cast<size_t>(contentW) * contentH / 4096);
    uint32_t id = 2;
    for (uint32_t y = 0; y < contentH; ++y)
        for (uint32_t x = 0; x < contentW; ++x) {
            if (at(x, y) || !clear(x, y)) continue;
            const uint32_t mine = id++;
            const size_t area = flood(x, y, mine);
            const size_t boxArea = static_cast<size_t>(box[2] - box[0] + 1) * (box[3] - box[1] + 1);
            if (area < minArea || area * 10 < boxArea * 8) continue;
            // Glass sits in a wall: the ring around the hole must be mostly opaque. An opening in a frame, grating or fence panel
            // (surround mostly clear) is meant to be open and stays cut. Neighbouring windows in a grid may share the ring.
            const int64_t pad = std::clamp<int64_t>((box[2] - box[0] + 1) / 4, 2, 64);
            const int64_t maxX = static_cast<int64_t>(contentW) - 1, maxY = static_cast<int64_t>(contentH) - 1;
            const uint32_t rx0 = static_cast<uint32_t>(std::clamp<int64_t>(static_cast<int64_t>(box[0]) - pad, 0, maxX));
            const uint32_t ry0 = static_cast<uint32_t>(std::clamp<int64_t>(static_cast<int64_t>(box[1]) - pad, 0, maxY));
            const uint32_t rx1 = static_cast<uint32_t>(std::clamp<int64_t>(static_cast<int64_t>(box[2]) + pad, 0, maxX));
            const uint32_t ry1 = static_cast<uint32_t>(std::clamp<int64_t>(static_cast<int64_t>(box[3]) + pad, 0, maxY));
            size_t ring = 0, ringOpaque = 0;
            for (uint32_t yy = ry0; yy <= ry1; ++yy)
                for (uint32_t xx = rx0; xx <= rx1; ++xx) {
                    if (xx >= box[0] && xx <= box[2] && yy >= box[1] && yy <= box[3]) continue;
                    ++ring; ringOpaque += !clear(xx, yy);
                }
            if (!ring || ringOpaque * 10 < ring * 8) continue;
            for (uint32_t yy = box[1]; yy <= box[3]; ++yy)
                for (uint32_t xx = box[0]; xx <= box[2]; ++xx)
                    if (at(xx, yy) == mine) {
                        uint8_t* t = &rgba[(static_cast<size_t>(yy) * width + xx) * 4];
                        t[0] = 8; t[1] = 9; t[2] = 11; t[3] = 255;
                        ++filled;
                    }
        }
    return filled;
}
} // namespace mc2::vegetation
