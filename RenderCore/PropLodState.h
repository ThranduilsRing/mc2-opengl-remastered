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
        if ((used_ + 1) * 4 > slots_.size() * 3) grow();
        size_t i = home(key);
        while (slots_[i].used) {
            if (slots_[i].key == key) return slots_[i].val;
            i = (i + 1) & mask();
        }
        slots_[i] = {key, 0, 1};
        ++used_;
        return slots_[i].val;
    }
    void erase(uint64_t key) {
        if (slots_.empty()) return;
        size_t i = home(key);
        while (slots_[i].used && slots_[i].key != key) i = (i + 1) & mask();
        if (!slots_[i].used) return;
        // Backward-shift deletion keeps probe chains intact without tombstones.
        size_t j = i;
        for (;;) {
            j = (j + 1) & mask();
            if (!slots_[j].used) break;
            const size_t h = home(slots_[j].key);
            const bool movable = (i <= j) ? (h <= i || h > j) : (h <= i && h > j);
            if (movable) { slots_[i] = slots_[j]; i = j; }
        }
        slots_[i].used = 0;
        --used_;
    }
    void clear() { slots_.clear(); used_ = 0; }
    size_t size() const { return used_; }
    const uint8_t* find(uint64_t key) const {
        if (slots_.empty()) return nullptr;
        size_t i = home(key);
        while (slots_[i].used) {
            if (slots_[i].key == key) return &slots_[i].val;
            i = (i + 1) & mask();
        }
        return nullptr;
    }
private:
    struct Slot { uint64_t key; uint8_t val; uint8_t used; };
    std::vector<Slot> slots_;
    size_t used_ = 0;
    size_t mask() const { return slots_.size() - 1; }
    size_t home(uint64_t k) const {   // the key's low bits are the typeID, so mix before masking
        k ^= k >> 33; k *= 0xFF51AFD7ED558CCDull; k ^= k >> 33; k *= 0xC4CEB9FE1A85EC53ull; k ^= k >> 33;
        return static_cast<size_t>(k) & mask();
    }
    void grow() {
        std::vector<Slot> old;
        old.swap(slots_);
        slots_.assign(old.empty() ? 1024 : old.size() * 2, Slot{0, 0, 0});
        used_ = 0;
        for (const Slot& s : old) if (s.used) (*this)[s.key] = s.val;
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
