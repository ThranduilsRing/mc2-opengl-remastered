#pragma once

#include "water_planar_view.h"
#include "vulkan_terrain_island.h"
#include <limits>
#include <vector>

namespace mc2::waterplanar {

// Owns reflected visibility without changing the main-view patches, LOD cache,
// or terrain fields. Borrowed material/height bytes have the source feed's life;
// the returned patch span remains valid until this object's next prepare().
class TerrainView {
public:
    bool prepare(const Mc2TerrainTier1Feed& source, const float* mainWorldToClip, float water) {
        patches_.clear();
        feed_ = {};
        if (!mainWorldToClip || !std::isfinite(water) || !std::isfinite(source.halfMap)
            || source.mapSide < 2 || !source.heightCpu) return false;
        for (int i = 0; i < 16; ++i) if (!std::isfinite(mainWorldToClip[i])) return false;
        const uint64_t side = static_cast<uint64_t>(source.mapSide);
        if (source.heightBytes / sizeof(float) < side * side) return false;
        const bool visual = source.visualSide > 0 && source.visualHeightCpu;
        if (visual && (static_cast<uint64_t>(source.visualSide) != (side - 1) * 4 + 1
            || source.visualHeightBytes / sizeof(float)
                < static_cast<uint64_t>(source.visualSide) * source.visualSide)) return false;
        if (source.visualSide > 0 && !visual) return false;
        if (!std::isfinite(source.visualDisplaceFar)) return false;

        const View view(mainWorldToClip, water);
        for (float f : view.worldToClip) if (!std::isfinite(f)) return false;
        // TerrainFieldStore versions are process-unique. A zero version is
        // unversioned input (e.g. tests): rebuild rather than trusting its cache.
        if (source.heightVersion == 0 || (visual && source.visualHeightVersion == 0)
            || side_ != source.mapSide || height_ != source.heightCpu || heightVersion_ != source.heightVersion
            || visual_ != (visual ? source.visualHeightCpu : nullptr) || visualVersion_ != source.visualHeightVersion) {
            bounds_.clear();
            side_ = 0;
            for (int y = 0; y < source.mapSide - 1; y += 20) {
                for (int x = 0; x < source.mapSide - 1; x += 20) {
                    const int nx = (std::min)(20, source.mapSide - 1 - x);
                    const int ny = (std::min)(20, source.mapSide - 1 - y);
                    float lo = (std::numeric_limits<float>::max)();
                    float hi = -(std::numeric_limits<float>::max)();
                    const auto sampleRange = [&](const float* values, int stride, int ox, int oy, int ex, int ey) {
                        for (int py = oy; py <= ey; ++py) for (int px = ox; px <= ex; ++px) {
                            const float z = values[static_cast<size_t>(py) * stride + px];
                            if (!std::isfinite(z)) return false;
                            lo = (std::min)(lo, z); hi = (std::max)(hi, z);
                        }
                        return true;
                    };
                    if (!sampleRange(source.heightCpu, source.mapSide, x, y, x + nx, y + ny)) return false;
                    // Hires heights are convex blends of the coarse triangle
                    // and the fine bake, including border fade. Their union
                    // bounds every finest-grid vertex for all blend weights.
                    if (visual && !sampleRange(source.visualHeightCpu, source.visualSide,
                                               x * 4, y * 4, (x + nx) * 4, (y + ny) * 4)) return false;
                    bounds_.push_back({x, y, nx, ny, lo, hi});
                }
            }
            side_ = source.mapSide;
            height_ = source.heightCpu; heightVersion_ = source.heightVersion;
            visual_ = visual ? source.visualHeightCpu : nullptr; visualVersion_ = source.visualHeightVersion;
        }
        for (const Bounds& b : bounds_) {
            const std::array<float, 3> lo{b.x * 128.0f - source.halfMap,
                source.halfMap - (b.y + b.ny) * 128.0f, b.lo};
            const std::array<float, 3> hi{(b.x + b.nx) * 128.0f - source.halfMap,
                source.halfMap - b.y * 128.0f, b.hi};
            if (!view.intersects(lo, hi)) continue;
            Mc2TerrainTier1Patch patch{};
            patch.blockOriginX = b.x; patch.blockOriginY = b.y;
            patch.quadCountX = b.nx; patch.quadCountY = b.ny;
            // Uniform finest topology eliminates reflection LOD seams without
            // borrowing main-view morph, stitch or skirt state.
            patch.lodStep = 1;
            patch.visualMode = visual ? 1 : 0;
            patches_.push_back(patch);
        }
        feed_ = source;
        feed_.patches = patches_.data();
        feed_.patchCount = static_cast<uint32_t>(patches_.size());
        std::copy(view.worldToClip.begin(), view.worldToClip.end(), feed_.worldToClipGL);
        // Despite its name this feed field is Stuff/Y-up (see its declaration).
        // Mirror the eye as well as the projection so POM uses the reflected view.
        feed_.cameraPosMC2[1] = wp_mirror_z(source.cameraPosMC2[1], water);
        return true; // an empty, valid reflected view still needs its attachment clear
    }

    const Mc2TerrainTier1Feed& feed() const { return feed_; }

    TerrainView() = default;
    TerrainView(const TerrainView&) = delete;
    TerrainView& operator=(const TerrainView&) = delete;
    TerrainView(TerrainView&&) = delete;
    TerrainView& operator=(TerrainView&&) = delete;

private:
    struct Bounds { int x, y, nx, ny; float lo, hi; };
    Mc2TerrainTier1Feed feed_{};
    std::vector<Mc2TerrainTier1Patch> patches_;
    std::vector<Bounds> bounds_;
    int side_ = 0;
    const float* height_ = nullptr;
    const float* visual_ = nullptr;
    uint64_t heightVersion_ = 0, visualVersion_ = 0;
};

// Uses whole-map inputs even when the main camera has no visible terrain.
// The eventual reflected recorder supplies the current, unmirrored matrix.
bool prepareTerrainView(TerrainView& view, const float* mainWorldToClip, float waterElevation);

} // namespace mc2::waterplanar
