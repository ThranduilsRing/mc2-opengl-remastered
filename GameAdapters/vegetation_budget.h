#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mc2::vegetation {
// Both population records and worst-case (one instance) regions are 32 bytes.
// Their per-parity index buffers use fewer bytes and fit the same budget.
inline uint32_t storagePopulationCapacity(uint32_t requested, uint64_t maxStorageRange) {
    return uint32_t(std::min(uint64_t(requested), maxStorageRange / 32u));
}

// Reserve the cap across the whole map before visiting cells. Stopping a dense
// north-to-south scan at the cap can leave the camera's entire region empty.
inline float placementStep(float mapSpan, float authoredStep, int density, int capacity) {
    const int maxClump = std::max(6, density / 2);
    const int side = std::max(1, int(std::sqrt(double(std::max(capacity, 1)) / maxClump)));
    return std::max(authoredStep, mapSpan / float(side + 1));
}
}
