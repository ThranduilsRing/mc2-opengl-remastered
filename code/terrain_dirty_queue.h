#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace mc2 {

// Presentation IDs only. Populate and drain after the simulation joins.
class TerrainDirtyQueue {
    std::vector<unsigned char> marked_;
    std::vector<long> pending_;
public:
    void reset(std::size_t handleCount) {
        marked_.assign(handleCount, 0);
        pending_.clear();
    }
    void mark(long handle) {
        if (handle < 0 || static_cast<std::size_t>(handle) >= marked_.size()) return;
        if (marked_[handle]) return;
        marked_[handle] = 1;
        pending_.push_back(handle);
    }
    void markBlock(long firstHandle, long count) {
        for (long i = 0; i < count; ++i) mark(firstHandle + i);
    }
    std::vector<long> drain() {
        std::vector<long> result;
        result.swap(pending_);
        // Keep the terrain handle order used by the old presentation walk.
        std::sort(result.begin(), result.end());
        for (long handle : result) marked_[handle] = 0;
        return result;
    }
};

} // namespace mc2
