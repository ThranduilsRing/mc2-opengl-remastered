#pragma once
#include <algorithm>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

// Runs depend on packet selection and live materials; bone poses are uploaded
// independently. Resolve each texture once per frame, including hot replacements.
template<class Call> class MechRunCache {
public:
    template<class Resolve>
    bool update(std::span<const Call> calls, uint64_t geometry, Resolve resolve) {
        next_.clear();
        const auto add = [&](uint32_t slot) {
            next_.emplace_back(slot, 0);
        };
        for (const auto& call : calls) {
            add(call.texHandle);
            if (call.aoTexHandle && call.aoTexHandle != UINT32_MAX) add(call.aoTexHandle);
            if (call.normalTexHandle && call.normalTexHandle != UINT32_MAX) add(call.normalTexHandle);
        }
        std::ranges::sort(next_);
        next_.erase(std::unique(next_.begin(), next_.end()), next_.end());
        for (auto& [slot, texture] : next_) texture = resolve(slot);
        const bool changed = !valid_ || geometry_ != geometry || textures_ != next_ ||
            !std::ranges::equal(calls, calls_);
        if (changed) {
            calls_.assign(calls.begin(), calls.end());
            textures_.swap(next_);
            geometry_ = geometry;
            valid_ = true;
        }
        return changed;
    }
    uint64_t texture(uint32_t slot) const {
        const auto& [id, value] = *std::ranges::lower_bound(textures_, slot, {}, &Texture::first);
        return value;
    }
private:
    using Texture = std::pair<uint32_t, uint64_t>;
    // Both buffers retain capacity, including across invalidations.
    std::vector<Texture> textures_, next_;
    std::vector<Call> calls_;
    uint64_t geometry_ = 0;
    bool valid_ = false;
};
