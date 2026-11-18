#pragma once
#include <cstddef>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>
#include <vector>

// tierOf must use snapshots taken immediately after each instance's advance:
// another instance (including a dynamic one) can share and move its state byte.
template<class Instance, size_t Tiers>
struct PropLodTierCache {
    std::vector<uint8_t> emitted;
    std::array<std::vector<Instance>, Tiers> tiers;

    template<class TierOf, class Retype>
    bool refresh(std::span<const Instance> store, bool rebound, TierOf tierOf, Retype retype) {
        bool changed = rebound || emitted.size() != store.size();
        for (size_t i = 0; i < store.size(); ++i)
            changed |= i >= emitted.size() || emitted[i] != tierOf(i);
        if (!changed) return false;
        emitted.resize(store.size());
        for (auto& tier : tiers) tier.clear();
        for (size_t i = 0; i < store.size(); ++i) {
            const uint8_t tier = tierOf(i);
            emitted[i] = tier;
            tiers[tier].push_back(retype(store[i], tier));
        }
        return true;
    }
};

// Rebuild the ordered feed in place. Unchanged blocks already contain exactly
// the bytes the consumer needs; changed blocks and new tails are copied normally.
template<class Instance>
void appendPropFeed(std::vector<Instance>& flat, size_t& cursor, std::span<const Instance> next) {
    static_assert(std::is_trivially_copyable_v<Instance>);
    const size_t available = flat.size() - cursor;
    const size_t reuse = available < next.size() ? available : next.size();
    if (reuse && std::memcmp(flat.data() + cursor, next.data(), reuse * sizeof(Instance)) != 0)
        std::memcpy(flat.data() + cursor, next.data(), reuse * sizeof(Instance));
    if (reuse < next.size()) flat.insert(flat.end(), next.begin() + reuse, next.end());
    cursor += next.size();
}

struct PropRecordCacheKey {
    uint64_t scene = 0, material = 0, resource = 0;
    bool operator==(const PropRecordCacheKey&) const = default;
};

// Packet ranges stay fixed while actor counts and their instance bases change.
template<class Run, class Command, size_t Lists>
void patchPropRun(Run& run, std::vector<Command> (&commands)[Lists],
                  const uint32_t* listStart, uint32_t first, uint32_t count) {
    auto& [runFirst, runCount, ranges, padding] = run;
    runFirst = first;
    runCount = count;
    for (size_t list = 0; list < Lists; ++list) {
        const auto begin = ranges[list][0] - listStart[list];
        for (uint32_t p = begin; p < begin + ranges[list][1]; ++p) {
            commands[list][p].firstInstance = first;
            commands[list][p].instanceCount = count;
        }
    }
}

// Draw metadata depends on ordered type runs, not transforms, light indices,
// tint or fog. Keep its generation separate from the full instance upload.
struct PropTypeGeneration {
    uint64_t generation = 0, topology = 0;
    std::vector<uint64_t> types, runTypes;
    struct TypeIdentity { uint64_t operator()(uint32_t type) const { return type; } };

    template<class Instance, class Identity = TypeIdentity>
    uint64_t update(std::span<const Instance> instances, Identity identity = {}) {
        bool changed = generation == 0 || types.size() != instances.size();
        bool topologyChanged = topology == 0;
        size_t run = 0;
        types.resize(instances.size());
        uint64_t key = 0;
        for (size_t i = 0; i < instances.size(); ++i) {
            // Resolve the recipe/object ID once per contiguous type run.
            if (i == 0 || instances[i].typeID != instances[i - 1].typeID) {
                key = identity(instances[i].typeID);
                topologyChanged |= run >= runTypes.size() || runTypes[run] != key;
                if (run == runTypes.size()) runTypes.push_back(key);
                else runTypes[run] = key;
                ++run;
            }
            changed |= types[i] != key;
            types[i] = key;
        }
        topologyChanged |= run != runTypes.size();
        runTypes.resize(run);
        if (topologyChanged) ++topology;
        if (changed) ++generation;
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
