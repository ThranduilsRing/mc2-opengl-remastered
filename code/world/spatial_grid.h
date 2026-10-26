#pragma once
#include "entity_registry.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace mc2::world {
// Sparse uniform cells, with query results in the caller's legacy view order.
// An AABB is deliberately conservative; consumers retain their exact predicates.
class SpatialGrid {
public:
    struct Candidate { unsigned ordinal; EntityId id; };
private:
    struct Entry { int x, y; unsigned ordinal; EntityId id; };
    std::vector<Entry> entries_;
    std::vector<Candidate> unbounded_;
    static constexpr float cellWidth = 512.0f;
    static bool cell(float p, int& result) {
        const double c = std::floor(double(p) / cellWidth);
        if (!std::isfinite(c) || c <= std::numeric_limits<int>::min() ||
            c >= std::numeric_limits<int>::max()) return false;
        result = static_cast<int>(c);
        return true;
    }
public:
    void clear() { entries_.clear(); unbounded_.clear(); }
    void add(float x, float y, unsigned ordinal, EntityId id) {
        int cx, cy;
        if (!cell(x, cx) || !cell(y, cy)) unbounded_.push_back({ordinal, id});
        else entries_.push_back({cx, cy, ordinal, id});
    }
    void finish() {
        std::sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) {
            if (a.y != b.y) return a.y < b.y;
            if (a.x != b.x) return a.x < b.x;
            return a.ordinal < b.ordinal;
        });
    }
    std::vector<Candidate> query(float x, float y, float radius) const {
        std::vector<Candidate> result = unbounded_;
        int x0, y0, x1, y1;
        // One cell of padding also covers float rounding in legacy distance math.
        const bool bounded = radius >= 0 && cell(x - radius, x0) && cell(x + radius, x1) &&
            cell(y - radius, y0) && cell(y + radius, y1);
        if (!bounded || int64_t(y1) - y0 + 3 > int64_t(entries_.size())) {
            for (const auto& e : entries_) result.push_back({e.ordinal, e.id});
        } else {
            --x0; ++x1; --y0; ++y1;
            for (int64_t row = y0; row <= y1; ++row) {
                auto it = std::lower_bound(entries_.begin(), entries_.end(), std::pair{int(row), x0},
                    [](const Entry& e, const auto& key) {
                        return e.y < key.first || (e.y == key.first && e.x < key.second);
                    });
                for (; it != entries_.end() && it->y == row && it->x <= x1; ++it)
                    result.push_back({it->ordinal, it->id});
            }
        }
        std::sort(result.begin(), result.end(), [](const Candidate& a, const Candidate& b) { return a.ordinal < b.ordinal; });
        return result;
    }
};
}
