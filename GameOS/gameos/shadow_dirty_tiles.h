// shadow_dirty_tiles.h -- the static shadow mask's dirty sub-tiles: which 128^2-texel pieces of the mask a change can touch, queued and drained under
// a per-frame budget.
//
// WHAT: a world-XY rectangle (a caster's old or new shadow footprint, or an edited height patch grown down-sun) becomes the set of sub-tiles it
// overlaps, plus one sub-tile of margin (the blurred fade and the 3x3 resolve read a little beyond a texel). The queue keeps each sub-tile once, in
// arrival order, and hands out at most a budget's worth per frame. Pure CPU math; a doctest pins it.
// WHY: a destroyed building or a felled tree changes a few hundred texels of a map-sized mask. Recomputing the sub-tiles it touches, in place and over
// a frame or two, replaces the old full-map rebake (a waitIdle and a 64 MB redraw on every destroy).
#pragma once

#include "shadow_bake_math.h"

#include <cstdint>
#include <deque>
#include <vector>

namespace mc2shadow {

inline constexpr int kSubTileTexels = 128;

struct SubTile {
    int x = 0, y = 0;   // in sub-tiles, row 0 north
    bool operator==(const SubTile& o) const { return x == o.x && y == o.y; }
};

// The sub-tiles a world rect overlaps, grown by `marginSubTiles` on every side and clipped to the mask.
inline std::vector<SubTile> subTilesForRect(const MaskLayout& m, Rect r, int marginSubTiles = 1) {
    std::vector<SubTile> out;
    if (m.texels <= 0 || m.texelWorld <= 0.0f) return out;
    const float st = static_cast<float>(kSubTileTexels) * m.texelWorld;
    const int n = m.texels / kSubTileTexels;
    const auto clampi = [n](int v) { return v < 0 ? 0 : (v >= n ? n - 1 : v); };
    if (r.x1 < m.originX || r.x0 > m.originX + m.span || r.y0 > m.originYNorth || r.y1 < m.originYNorth - m.span) return out;
    const int x0 = clampi(static_cast<int>(std::floor((r.x0 - m.originX) / st)) - marginSubTiles);
    const int x1 = clampi(static_cast<int>(std::floor((r.x1 - m.originX) / st)) + marginSubTiles);
    const int y0 = clampi(static_cast<int>(std::floor((m.originYNorth - r.y1) / st)) - marginSubTiles);
    const int y1 = clampi(static_cast<int>(std::floor((m.originYNorth - r.y0) / st)) + marginSubTiles);
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) out.push_back({x, y});
    return out;
}

// The world rect sub-tile s computes (apron included), the sub-tile analogue of tileRegionWorld.
inline Rect subTileRegionWorld(const MaskLayout& m, SubTile s) {
    const float d = m.texelWorld;
    const float west = m.originX + static_cast<float>(s.x * kSubTileTexels - kTileApron) * d;
    const float north = m.originYNorth - static_cast<float>(s.y * kSubTileTexels - kTileApron) * d;
    const float size = static_cast<float>(kSubTileTexels + 2 * kTileApron) * d;
    return {west, north - size, west + size, north};
}

class DirtyQueue {
public:
    void reset(int subTilesPerSide) {
        side_ = subTilesPerSide;
        queued_.assign(static_cast<size_t>(side_) * static_cast<size_t>(side_), 0);
        order_.clear();
    }
    // Returns how many sub-tiles were newly queued.
    size_t add(const std::vector<SubTile>& tiles) {
        size_t added = 0;
        for (const SubTile& t : tiles) {
            if (t.x < 0 || t.y < 0 || t.x >= side_ || t.y >= side_) continue;
            uint8_t& q = queued_[static_cast<size_t>(t.y) * static_cast<size_t>(side_) + static_cast<size_t>(t.x)];
            if (q) continue;
            q = 1;
            order_.push_back(t);
            ++added;
        }
        return added;
    }
    // Up to `budget` sub-tiles in arrival order, removed from the queue.
    std::vector<SubTile> take(size_t budget) {
        std::vector<SubTile> out;
        while (!order_.empty() && out.size() < budget) {
            const SubTile t = order_.front();
            order_.pop_front();
            queued_[static_cast<size_t>(t.y) * static_cast<size_t>(side_) + static_cast<size_t>(t.x)] = 0;
            out.push_back(t);
        }
        return out;
    }
    size_t size() const { return order_.size(); }
    bool empty() const { return order_.empty(); }

private:
    int side_ = 0;
    std::vector<uint8_t> queued_;
    std::deque<SubTile> order_;
};

// Until uploads succeed, an aborted batch remains work for a later frame.
class DirtyBatch {
public:
    DirtyBatch(DirtyQueue& queue, size_t budget) : queue_(queue), tiles(queue.take(budget)) {}
    ~DirtyBatch() { if (!committed_) queue_.add(tiles); }
    DirtyBatch(const DirtyBatch&) = delete;
    DirtyBatch& operator=(const DirtyBatch&) = delete;
    void commit() { committed_ = true; }
private:
    DirtyQueue& queue_;
    bool committed_ = false;
public:
    const std::vector<SubTile> tiles;
};

}  // namespace mc2shadow
