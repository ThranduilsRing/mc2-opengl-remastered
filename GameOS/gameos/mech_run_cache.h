#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <span>
#include <utility>
#include <vector>

// Runs depend on packet selection and live materials; bone poses are uploaded
// independently. Resolve each texture once per frame, including hot replacements.
template<class Call> class MechRunCache {
public:
    template<class Resolve>
    bool update(std::span<const Call> calls, uint64_t geometry, Resolve resolve) {
        std::map<uint32_t, uint64_t> next;
        const auto add = [&](uint32_t slot) {
            if (!next.contains(slot)) next.emplace(slot, resolve(slot));
        };
        for (const auto& call : calls) {
            add(call.texHandle);
            if (call.aoTexHandle && call.aoTexHandle != UINT32_MAX) add(call.aoTexHandle);
            if (call.normalTexHandle && call.normalTexHandle != UINT32_MAX) add(call.normalTexHandle);
        }
        const bool changed = !valid_ || geometry_ != geometry || textures != next ||
            !std::ranges::equal(calls, calls_);
        if (changed) {
            calls_.assign(calls.begin(), calls.end());
            textures = std::move(next);
            geometry_ = geometry;
            valid_ = true;
        }
        return changed;
    }
    std::map<uint32_t, uint64_t> textures;
private:
    std::vector<Call> calls_;
    uint64_t geometry_ = 0;
    bool valid_ = false;
};
