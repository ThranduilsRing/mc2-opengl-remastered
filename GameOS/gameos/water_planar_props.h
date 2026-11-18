#pragma once

#include "water_planar_view.h"
#include "../../RenderCore/hzb_prop_bounds.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <cstring>
#include <span>
#include <vector>

namespace mc2::waterplanar {

// Bound every packet of a finalized type, including alpha cards. Invalid or
// absent geometry stays unknown so reflection culling remains conservative.
template <class Geometry>
std::vector<mc2hzb::PropBounds> geometryBounds(const Geometry& geom,
        std::span<const std::byte> vertices, std::span<const uint32_t> indices) {
    std::vector<mc2hzb::PropBounds> result(geom.typeCount);
    if (geom.vertexStride < 3 * sizeof(float) || !geom.packets ||
        !geom.typeFirstPacket || !geom.typePacketCount) return result;
    for (uint32_t t = 0; t < geom.typeCount; ++t) {
        auto& b = result[t];
        for (int a = 0; a < 3; ++a) {
            b.lo[a] = (std::numeric_limits<float>::max)();
            b.hi[a] = -(std::numeric_limits<float>::max)();
        }
        bool invalid = false;
        for (uint32_t p = 0; p < geom.typePacketCount[t] && !invalid; ++p) {
            const uint64_t pk = static_cast<uint64_t>(geom.typeFirstPacket[t]) + p;
            if (pk >= geom.packetCount) { invalid = true; break; }
            const auto& packet = geom.packets[pk];
            if (static_cast<uint64_t>(packet.firstIndex) + packet.indexCount > indices.size()) {
                invalid = true; break;
            }
            for (uint32_t k = 0; k < packet.indexCount; ++k) {
                const uint64_t vi = static_cast<uint64_t>(packet.baseVertex) + indices[packet.firstIndex + k];
                if (vi >= vertices.size() / geom.vertexStride) { invalid = true; break; }
                float pos[3];
                std::memcpy(pos, vertices.data() + vi * geom.vertexStride, sizeof(pos));
                for (int a = 0; a < 3; ++a) {
                    if (!std::isfinite(pos[a])) invalid = true;
                    b.lo[a] = (std::min)(b.lo[a], pos[a]);
                    b.hi[a] = (std::max)(b.hi[a], pos[a]);
                }
                b.valid = true;
            }
        }
        b.valid = b.valid && !invalid;
    }
    return result;
}

struct WorldPropBounds { std::array<float, 3> lo{}, hi{}; bool valid = false; };

inline WorldPropBounds propWorldBounds(const GpuStaticPropInstance& instance,
                                        std::span<const mc2hzb::PropBounds> bounds) {
    // Missing geometry bounds retain the instance. The reflected pass must
    // never use main-view HZB results to decide whether a palm is present.
    if (instance.typeID >= bounds.size() || !bounds[instance.typeID].valid) return {};
    const auto& b = bounds[instance.typeID];
    std::array<float, 3> lo{}, hi{};
    for (int axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(b.lo[axis]) || !std::isfinite(b.hi[axis]) || b.lo[axis] > b.hi[axis]) return {};
        lo[axis] = (std::numeric_limits<float>::max)();
        hi[axis] = -(std::numeric_limits<float>::max)();
    }
    for (int corner = 0; corner < 8; ++corner) {
        const float p[4] = {corner & 1 ? b.hi[0] : b.lo[0], corner & 2 ? b.hi[1] : b.lo[1],
                            corner & 4 ? b.hi[2] : b.lo[2], 1.0f};
        struct Point { float x, y, z; } stuff{};
        float xyz[3]{};
        // Stuff-memcpy matrix: the translation is [3],[7],[11], as in the
        // prop vertex shader's row-vector multiply, not GLM's column [12..14].
        for (int axis = 0; axis < 3; ++axis)
            for (int k = 0; k < 4; ++k) xyz[axis] += p[k] * instance.modelMatrix[axis * 4 + k];
        stuff = {xyz[0], xyz[1], xyz[2]};
        const auto world = mc2ToRender(stuff);
        const float values[] = {world.x, world.y, world.z};
        for (int axis = 0; axis < 3; ++axis) {
            if (!std::isfinite(values[axis])) return {};
            lo[axis] = (std::min)(lo[axis], values[axis]);
            hi[axis] = (std::max)(hi[axis], values[axis]);
        }
    }
    // World-space leaf displacement in color_prop_tier1.vert.
    lo[0] -= 1.5f; hi[0] += 1.5f;
    lo[1] -= 0.9f; hi[1] += 0.9f;
    return {lo, hi, true};
}

inline bool propIntersects(const View& view, const GpuStaticPropInstance& instance,
                           std::span<const mc2hzb::PropBounds> bounds) {
    const auto b = propWorldBounds(instance, bounds);
    return !b.valid || view.intersects(b.lo, b.hi);
}

// Registry revisions cover recipe/transform writes; geometry bounds are compared
// exactly, including in-place edits. Keep current payloads and independent
// reflected visibility: neither main-view HZB nor shadow flags are reusable here.
class ReflectionPropsCache {
    struct Entry { size_t recipe, range; WorldPropBounds world; };
    // Per-frame cull index: valid-bounds entries bucketed into a grid over
    // world axes 0,1, stored contiguously per cell. Every entry keeps its index
    // in order_, so survivors are emitted in the stable type order via a bitmap.
    struct CellEntry { std::array<float, 3> lo, hi; uint32_t idx, range; };
    struct Cell { std::array<float, 3> lo, hi; float minHiZ; uint32_t begin, end; };
    std::vector<Entry> order_;
    std::vector<CellEntry> cellEntries_;
    std::vector<Cell> cells_;
    std::vector<uint32_t> unbounded_;   // invalid bounds: kept in every view (conservative)
    std::vector<uint64_t> bits_;
    std::vector<uint8_t> rangeMulti_;
    std::vector<mc2hzb::PropBounds> geometry_;
    const void *recipes_ = nullptr, *ranges_ = nullptr;
    size_t recipeCount_ = 0, rangeCount_ = 0;
    uint64_t generation_ = 0;
    bool ready_ = false;

    void buildGrid() {
        cellEntries_.clear(); cells_.clear(); unbounded_.clear();
        constexpr float kMax = (std::numeric_limits<float>::max)();
        size_t valid = 0;
        float cmin[2] = {kMax, kMax}, cmax[2] = {-kMax, -kMax};
        for (size_t i = 0; i < order_.size(); ++i) {
            const auto& w = order_[i].world;
            if (!w.valid) { unbounded_.push_back(static_cast<uint32_t>(i)); continue; }
            ++valid;
            for (int a = 0; a < 2; ++a) {
                const float c = 0.5f * (w.lo[a] + w.hi[a]);
                cmin[a] = (std::min)(cmin[a], c); cmax[a] = (std::max)(cmax[a], c);
            }
        }
        if (valid == 0) return;
        const int g = (std::max)(1, (std::min)(48, static_cast<int>(std::sqrt(static_cast<float>(valid) / 24.0f))));
        float inv[2];
        for (int a = 0; a < 2; ++a) inv[a] = cmax[a] > cmin[a] ? static_cast<float>(g) / (cmax[a] - cmin[a]) : 0.0f;
        auto cellOf = [&](const WorldPropBounds& w) {
            int c[2];
            for (int a = 0; a < 2; ++a)
                c[a] = std::clamp(static_cast<int>((0.5f * (w.lo[a] + w.hi[a]) - cmin[a]) * inv[a]), 0, g - 1);
            return static_cast<size_t>(c[1]) * g + c[0];
        };
        std::vector<uint32_t> start(static_cast<size_t>(g) * g + 1, 0);
        for (const auto& e : order_) if (e.world.valid) ++start[cellOf(e.world) + 1];
        for (size_t c = 1; c < start.size(); ++c) start[c] += start[c - 1];
        cellEntries_.resize(valid);
        std::vector<uint32_t> fill(start.begin(), start.end() - 1);
        for (size_t i = 0; i < order_.size(); ++i) {   // ascending: each cell list stays sorted by idx
            const auto& e = order_[i];
            if (!e.world.valid) continue;
            cellEntries_[fill[cellOf(e.world)]++] = {e.world.lo, e.world.hi, static_cast<uint32_t>(i),
                                                      static_cast<uint32_t>(e.range)};
        }
        for (size_t c = 0; c + 1 < start.size(); ++c) {
            if (start[c] == start[c + 1]) continue;
            Cell cell{{kMax, kMax, kMax}, {-kMax, -kMax, -kMax}, kMax, start[c], start[c + 1]};
            for (uint32_t k = cell.begin; k < cell.end; ++k) {
                for (int a = 0; a < 3; ++a) {
                    cell.lo[a] = (std::min)(cell.lo[a], cellEntries_[k].lo[a]);
                    cell.hi[a] = (std::max)(cell.hi[a], cellEntries_[k].hi[a]);
                }
                cell.minHiZ = (std::min)(cell.minHiZ, cellEntries_[k].hi[2]);
            }
            cells_.push_back(cell);
        }
    }

    // Every member box lies inside the cell box, so when the cell's least
    // favourable corner passes each plane (same float operation order as
    // View::intersects, monotone per term) every member passes bit-for-bit.
    static bool fullyInside(const View& view, const Cell& c) {
        if (wp_clip_distance(c.minHiZ, view.waterElevation) < 0.0f) return false;
        const float lo[3] = {c.lo[0], c.lo[1], (std::max)(c.lo[2], view.waterElevation)};
        for (const auto& p : view.planes) {
            float distance = p[3];
            for (int axis = 0; axis < 3; ++axis)
                distance += p[axis] * (p[axis] >= 0.0f ? lo[axis] : c.hi[axis]);
            if (distance < 0.0f) return false;
        }
        return true;
    }
public:
    template <class Range>
    bool collect(std::span<const GpuStaticPropInstance> recipes, std::span<const Range> ranges,
                 uint64_t generation, const View& view, std::span<const mc2hzb::PropBounds> bounds,
                 std::vector<GpuStaticPropInstance>& out) {
        const bool sameGeometry = geometry_.size() == bounds.size() &&
            std::equal(geometry_.begin(), geometry_.end(), bounds.begin(), [](const auto& a, const auto& b) {
                return a.valid == b.valid && std::memcmp(a.lo, b.lo, sizeof(a.lo)) == 0 &&
                    std::memcmp(a.hi, b.hi, sizeof(a.hi)) == 0;
            });
        const bool rebuild = !ready_ || generation_ != generation || recipes_ != recipes.data() ||
            recipeCount_ != recipes.size() || ranges_ != ranges.data() || rangeCount_ != ranges.size() || !sameGeometry;
        if (rebuild) {
            order_.clear();
            for (size_t r = 0; r < ranges.size(); ++r) {
                const auto& range = ranges[r];
                const size_t begin = range.begin, count = range.count;
                if (count == 0 || begin > recipes.size() || count > recipes.size() - begin || !range.multi()) continue;
                for (size_t i = begin; i < begin + count; ++i)
                    order_.push_back({i, r, propWorldBounds(recipes[i], bounds)});
            }
            // Filtering a stable type order preserves the old sort-after-cull
            // order, including alpha cards and duplicate type IDs across ranges.
            std::stable_sort(order_.begin(), order_.end(), [&](const Entry& a, const Entry& b) {
                return recipes[a.recipe].typeID < recipes[b.recipe].typeID;
            });
            buildGrid();
            geometry_.assign(bounds.begin(), bounds.end());
            recipes_ = recipes.data(); recipeCount_ = recipes.size();
            ranges_ = ranges.data(); rangeCount_ = ranges.size();
            generation_ = generation; ready_ = true;
        }
        // A TGL borrow can expire independently of the recipe revision: resolve
        // it once per range per frame, not once per entry.
        rangeMulti_.assign(ranges.size(), 0);
        for (size_t r = 0; r < ranges.size(); ++r) rangeMulti_[r] = ranges[r].multi() ? 1 : 0;
        bits_.assign((order_.size() + 63) / 64, 0);
        auto set = [&](size_t i) { bits_[i >> 6] |= uint64_t{1} << (i & 63); };
        for (const uint32_t i : unbounded_) if (rangeMulti_[order_[i].range]) set(i);
        for (const auto& cell : cells_) {
            if (!view.intersects(cell.lo, cell.hi)) continue;
            const bool all = fullyInside(view, cell);
            for (uint32_t k = cell.begin; k < cell.end; ++k) {
                const auto& e = cellEntries_[k];
                if (rangeMulti_[e.range] && (all || view.intersects(e.lo, e.hi))) set(e.idx);
            }
        }
        out.clear();
        out.reserve(order_.size());
        for (size_t w = 0; w < bits_.size(); ++w)
            for (uint64_t m = bits_[w]; m; m &= m - 1)
                out.push_back(recipes[order_[(w << 6) + static_cast<size_t>(std::countr_zero(m))].recipe]);
        return rebuild;
    }
};

void preparePropInstances(const View& reflected, std::span<const mc2hzb::PropBounds> bounds,
                          std::vector<GpuStaticPropInstance>& out);

} // namespace mc2::waterplanar
