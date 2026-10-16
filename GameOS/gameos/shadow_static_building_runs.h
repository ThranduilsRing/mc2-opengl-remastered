// shadow_static_building_runs.h -- the per-type instanced draw runs of the VK static shadow map's buildings.
//
// WHAT: given the building instances' type IDs in sorted order and the number of types the static-prop geometry has packets for, the runs
// (type, first instance, count) the static map draws, and how many instances named a type the geometry lacks.
// WHY: on a map without static props the island never uploads its geometry, so the type tables are empty while the registry may still hold
// buildings; every such instance must be dropped rather than index an empty table. Kept free of Vulkan so the unit tests can pin that.
#ifndef MC2_SHADOW_STATIC_BUILDING_RUNS_H
#define MC2_SHADOW_STATIC_BUILDING_RUNS_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace mc2vk {

struct ShadowBuildingRun { uint32_t typeID; uint32_t firstInstance; uint32_t count; };

// `sortedTypeIDs` must be in ascending order; a type at or above `typeCount` has no packets and its instances are counted in `dropped`.
inline std::vector<ShadowBuildingRun> planShadowBuildingRuns(std::span<const uint32_t> sortedTypeIDs, std::size_t typeCount, uint32_t& dropped) {
    std::vector<ShadowBuildingRun> runs;
    dropped = 0;
    const uint32_t n = static_cast<uint32_t>(sortedTypeIDs.size());
    for (uint32_t i = 0; i < n;) {
        const uint32_t tid = sortedTypeIDs[i];
        uint32_t j = i;
        while (j < n && sortedTypeIDs[j] == tid) ++j;
        if (tid < typeCount) runs.push_back({tid, i, j - i});
        else dropped += j - i;
        i = j;
    }
    return runs;
}

}  // namespace mc2vk

#endif  // MC2_SHADOW_STATIC_BUILDING_RUNS_H
