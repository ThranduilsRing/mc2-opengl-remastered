#pragma once

#include <algorithm>
#include <cstddef>
#include <mutex>
#include <vector>

namespace mc2 {

// Sim mutations enqueue IDs; presentation drains after the simulation joins.
class TerrainDirtyQueue {
    std::mutex mutex_;
    std::vector<unsigned char> marked_;
    std::vector<long> pending_;
public:
    void reset(std::size_t handleCount) {
        const std::lock_guard lock(mutex_);
        marked_.assign(handleCount, 0);
        pending_.clear();
    }
    void mark(long handle) {
        if (handle < 0) return;
        const std::lock_guard lock(mutex_);
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
        {
            const std::lock_guard lock(mutex_);
            result.swap(pending_);
            for (long handle : result) marked_[handle] = 0;
        }
        // Keep the terrain handle order used by the old presentation walk.
        std::sort(result.begin(), result.end());
        return result;
    }
    void markAll() {
        const std::lock_guard lock(mutex_);
        for (std::size_t i = 1; i < marked_.size(); ++i) {
            if (marked_[i]) continue;
            marked_[i] = 1;
            pending_.push_back(static_cast<long>(i));
        }
    }
};

inline TerrainDirtyQueue& terrainDirtyQueue() {
    static TerrainDirtyQueue queue;
    return queue;
}

} // namespace mc2
