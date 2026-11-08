#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace mc2::terrain {

// MapData owns the writes. Bulk normal/shadow/water rebuilds advance the epoch;
// local height edits mark only their vertex. Off-screen edits remain pending.
class VertexDirtyFeed {
public:
    void reset(uint32_t count) {
        revisions_.assign(count, 0);
        invalidateAll();
    }
    void mark(uint32_t vertex) {
        if (vertex < revisions_.size()) revisions_[vertex] = ++serial_;
    }
    void invalidateAll() { epoch_ = ++serial_; }
    uint64_t revision(uint32_t vertex) const {
        return std::max(epoch_, revisions_[vertex]);
    }
private:
    std::vector<uint64_t> revisions_;
    uint64_t serial_ = 0, epoch_ = 0;
};
inline VertexDirtyFeed vertexDirtyFeed;

struct VertexRange { uint32_t first, count; };

// Each retired GPU slot retains its own receipt. A skipped frame or failed
// upload must not consume another slot's pending edits.
class VertexUploadRanges {
public:
    void reset(uint32_t count) { uploaded_.assign(count, 0); }
    std::span<const VertexRange> pending(std::span<const uint64_t> versions,
                                         uint32_t first, uint32_t count) {
        ranges_.clear();
        const uint32_t end = first + count;
        for (uint32_t i = first; i < end; ++i) {
            if (versions[i] == uploaded_[i]) continue;
            if (!ranges_.empty() && ranges_.back().first + ranges_.back().count == i)
                ++ranges_.back().count;
            else
                ranges_.push_back({i, 1});
        }
        return ranges_;
    }
    void acknowledge(VertexRange range, std::span<const uint64_t> versions) {
        std::copy_n(versions.begin() + range.first, range.count,
                    uploaded_.begin() + range.first);
    }
private:
    std::vector<uint64_t> uploaded_;
    std::vector<VertexRange> ranges_;
};
} // namespace mc2::terrain
