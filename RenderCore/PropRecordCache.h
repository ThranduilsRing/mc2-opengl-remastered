#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

struct PropRecordCacheKey {
    uint64_t scene = 0, material = 0, resource = 0;
    bool operator==(const PropRecordCacheKey&) const = default;
};

// Draw metadata depends on ordered type runs, not transforms, light indices,
// tint or fog. Keep its generation separate from the full instance upload.
struct PropTypeGeneration {
    uint64_t generation = 0;
    std::vector<uint32_t> types;

    template<class Instance>
    uint64_t update(std::span<const Instance> instances) {
        bool changed = generation == 0 || types.size() != instances.size();
        if (!changed) {
            for (size_t i = 0; i < instances.size(); ++i)
                if (types[i] != instances[i].typeID) { changed = true; break; }
        }
        if (changed) {
            types.resize(instances.size());
            for (size_t i = 0; i < instances.size(); ++i) types[i] = instances[i].typeID;
            ++generation;
        }
        return generation;
    }
};

// The final feed includes visibility, transient props and camera-selected LODs.
// Registry generation alone cannot identify its contents. Compare exactly (no
// hash collisions), and copy only when the published instance bytes change.
struct PropInstanceGeneration {
    uint64_t generation = 0, registry = 0;
    std::vector<std::byte> bytes;

    uint64_t update(uint64_t registryGeneration, std::span<const std::byte> next) {
        if (generation == 0 || registry != registryGeneration || bytes.size() != next.size() ||
            (!next.empty() && std::memcmp(bytes.data(), next.data(), next.size()) != 0)) {
            bytes.assign(next.begin(), next.end());
            registry = registryGeneration;
            ++generation;
        }
        return generation;
    }
};
