#pragma once
// Per-instance distance-LOD hysteresis memory for the static-prop feed. The feed re-selects the LOD of every
// LOD-capable instance each frame; the memory lookup used to be a node-based std::unordered_map (one pointer
// chase and a cache miss per instance per frame). This flat open-addressing table keeps the same semantics
// (operator[] default-inserts 0, erase removes) in one contiguous array.
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
#include <algorithm>
#include <cmath>
#include "StaticPropOrigin.h"
#include "axis_convert.h"

class PropLodStateMap {
public:
    uint8_t& operator[](uint64_t key) {
        return dense(resolve(key));
    }
    // Indices survive hash-table growth and dynamic inserts. Erase/clear
    // invalidate bindings; duplicate keys deliberately share one dense value.
    size_t resolve(uint64_t key) {
        if ((used_ + 1) * 4 > keys_.size() * 3) grow();
        size_t i = home(key);
        while (occupied_[i]) {
            if (keys_[i] == key) return indices_[i];
            i = (i + 1) & mask();
        }
        const size_t index = free_.empty() ? values_.size() : free_.back();
        if (free_.empty()) values_.push_back(0);
        else { free_.pop_back(); values_[index] = 0; }
        keys_[i] = key; indices_[i] = index; occupied_[i] = 1;
        ++used_;
        return index;
    }
    uint8_t& dense(size_t index) { return values_[index]; }
    uint64_t revision() const { return revision_; }
    void erase(uint64_t key) {
        if (keys_.empty()) return;
        size_t i = home(key);
        while (occupied_[i] && keys_[i] != key) i = (i + 1) & mask();
        if (!occupied_[i]) return;
        free_.push_back(indices_[i]);
        ++revision_;
        // Backward-shift deletion keeps probe chains intact without tombstones.
        size_t j = i;
        for (;;) {
            j = (j + 1) & mask();
            if (!occupied_[j]) break;
            const size_t h = home(keys_[j]);
            const bool movable = (i <= j) ? (h <= i || h > j) : (h <= i && h > j);
            if (movable) { keys_[i] = keys_[j]; indices_[i] = indices_[j]; occupied_[i] = occupied_[j]; i = j; }
        }
        occupied_[i] = 0;
        --used_;
    }
    void clear() { keys_.clear(); indices_.clear(); occupied_.clear(); values_.clear(); free_.clear(); used_ = 0; ++revision_; }
    size_t size() const { return used_; }
    const uint8_t* find(uint64_t key) const {
        if (keys_.empty()) return nullptr;
        size_t i = home(key);
        while (occupied_[i]) {
            if (keys_[i] == key) return &values_[indices_[i]];
            i = (i + 1) & mask();
        }
        return nullptr;
    }
private:
    std::vector<uint64_t> keys_;
    std::vector<size_t> indices_;
    std::vector<uint8_t> occupied_;
    std::vector<uint8_t> values_;
    std::vector<size_t> free_;
    uint64_t revision_ = 0;
    size_t used_ = 0;
    size_t mask() const { return keys_.size() - 1; }
    size_t home(uint64_t k) const {   // the key's low bits are the typeID, so mix before masking
        k ^= k >> 33; k *= 0xFF51AFD7ED558CCDull; k ^= k >> 33; k *= 0xC4CEB9FE1A85EC53ull; k ^= k >> 33;
        return static_cast<size_t>(k) & mask();
    }
    void grow() {
        const size_t capacity = keys_.empty() ? 1024 : keys_.size() * 2;
        // Allocate every array before changing this table; dense indices stay stable.
        std::vector<uint64_t> nextKeys(capacity);
        std::vector<size_t> nextIndices(capacity);
        std::vector<uint8_t> nextOccupied(capacity, 0);
        keys_.swap(nextKeys); indices_.swap(nextIndices); occupied_.swap(nextOccupied);
        for (size_t old = 0; old < nextKeys.size(); ++old) if (nextOccupied[old]) {
            size_t i = home(nextKeys[old]);
            while (occupied_[i]) i = (i + 1) & mask();
            keys_[i] = nextKeys[old]; indices_[i] = nextIndices[old]; occupied_[i] = 1;
        }
    }
};

// Rebuilt store order is only an alignment, never an identity. Resolve every
// leaf's original key again after a store edit or state invalidation.
struct PropLodSlots {
    std::vector<size_t> indices;
    uint64_t revision = UINT64_MAX;
    template<class Instance, class Key>
    void bind(PropLodStateMap& state, std::span<const Instance> instances, Key key) {
        if (revision == state.revision() && indices.size() == instances.size()) return;
        indices.clear();
        indices.reserve(instances.size());
        for (const auto& inst : instances) indices.push_back(state.resolve(key(inst)));
        revision = state.revision();
    }
};

// ViewUniforms stores the row-vector world-to-clip matrix in row-major order.
// It consumes MC2 coordinates, just like the prop vertex shader after its axis conversion.
inline float propLodProjectedDistance(const float* m, StaticPropOrigin renderOrigin, float rowY, float viewportScale) {
    const auto p = renderToMc2(renderOrigin);
    return std::abs(m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15]) / rowY * viewportScale;
}

// One hysteresis step: advance `lod` for camera distance d with a +-band so an instance never flickers at a boundary.
inline void propLodAdvance(uint8_t& lod, float d, std::span<const float> dist, float band) {
    const int count = static_cast<int>(dist.size());
    while (lod + 1 < count && d > dist[lod + 1] * (1.0f + band)) ++lod;
    while (lod > 0 && d < dist[lod] * (1.0f - band)) --lod;
}
