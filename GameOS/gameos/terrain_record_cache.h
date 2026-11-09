// terrain_record_cache.h -- H8 (terrain-submit-cut): per-patch record cache for the Vulkan terrain island.
//
// WHAT: keeps, per feed patch, the resolved draw (mesh lookups) and the front-to-back order across frames.
// WHY: the island rebuilt every patch's mesh lookup, sorted by distance and re-merged the mesh runs every frame, although
// a still camera and an unchanged LOD give identical results. update() re-resolves only patches whose feed record changed
// (or all of them when the external key changes), re-sorts only when the camera or a patch changed, and bumps generation()
// only when the ordered draw sequence actually differs, so the consumer can skip rebuilding and re-uploading its records.
// Pure standalone logic (no Vulkan): tests/unit/test_terrain_record_cache.cpp.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace mc2::terrain {

template <class Patch, class Draw>
class RecordCache {
public:
    // Returns the number of patches re-resolved this call. `resolve(patch, Draw&)` returns false to drop the patch
    // (no mesh); `distKey(patch)` is the front-to-back sort key (ties break by patch index, a strict total order).
    // `viewKey` identifies the camera position; `externalKey` everything else the resolved draws depend on.
    template <class Resolve, class DistKey>
    uint32_t update(const Patch* patches, uint32_t n, uint64_t externalKey, float camX, float camY,
                    Resolve&& resolve, DistKey&& distKey) {
        uint32_t rebuilt = 0;
        bool changed = false;
        if (!valid_ || externalKey != externalKey_ || entries_.size() != n) {
            entries_.assign(n, Entry{});
            externalKey_ = externalKey;
            valid_ = true;
            changed = true;
        }
        for (uint32_t i = 0; i < n; ++i) {
            Entry& e = entries_[i];
            if (e.valid && std::memcmp(&e.patch, &patches[i], sizeof(Patch)) == 0) continue;
            std::memcpy(&e.patch, &patches[i], sizeof(Patch));
            e.valid = true;
            e.has = resolve(patches[i], e.draw);
            ++rebuilt;
            changed = true;
        }
        const bool camMoved = !haveCam_ || camX != camX_ || camY != camY_;
        if (changed || camMoved) {
            camX_ = camX; camY_ = camY; haveCam_ = true;
            keys_.clear();
            for (uint32_t i = 0; i < n; ++i) keys_.emplace_back(distKey(patches[i]), i);
            std::sort(keys_.begin(), keys_.end());
            order_.clear();
            for (const auto& k : keys_) if (entries_[k.second].has) order_.push_back(k.second);
            if (changed || order_ != lastOrder_) {
                lastOrder_ = order_;
                draws_.clear();
                for (uint32_t i : order_) draws_.push_back(entries_[i].draw);
                ++generation_;
            }
        }
        return rebuilt;
    }

    const std::vector<Draw>& draws() const { return draws_; }
    uint64_t generation() const { return generation_; }

private:
    struct Entry { Patch patch{}; Draw draw{}; bool valid = false; bool has = false; };
    std::vector<Entry> entries_;
    std::vector<std::pair<float, uint32_t>> keys_;
    std::vector<uint32_t> order_, lastOrder_;
    std::vector<Draw> draws_;
    uint64_t externalKey_ = 0;
    uint64_t generation_ = 0;
    float camX_ = 0.0f, camY_ = 0.0f;
    bool valid_ = false, haveCam_ = false;
};

}  // namespace mc2::terrain
