#pragma once
#include <algorithm>
#include <cstdint>

namespace RenderCore {
inline constexpr uint64_t gpuMiB = 1024ull * 1024;
// Logical residency includes host-visible staging, flight copies and retired resources.
struct GpuResidencyPolicy {
    uint64_t total, textureUpload, vegetation;
    uint32_t textureEdge, terrainEdge;
};
constexpr GpuResidencyPolicy gpuResidencyPolicy(int quality) {
    if (quality == 0) return {512 * gpuMiB, 32 * gpuMiB, 0, 2048, 2048};
    if (quality == 1) return {1024 * gpuMiB, 128 * gpuMiB, 32 * gpuMiB, 4096, 4096};
    return {2048 * gpuMiB, 256 * gpuMiB, 512 * gpuMiB, 16384, 4096};
}
constexpr uint64_t gpuBudget(uint64_t available, uint64_t ceiling) {
    const uint64_t margin = (std::max)(32 * gpuMiB, available / 10);
    return (std::min)(ceiling, available > margin ? available - margin : 0);
}
// Device-resident bytes of a vegetation population: instance + region copies plus the per-parity cull outputs. The upload also holds a transient
// 32 B staging copy of every record.
constexpr uint64_t vegetationBytes(uint64_t instances, uint64_t regions, uint32_t slots) {
    const uint64_t n = instances + regions;
    return 32 * n + uint64_t(slots) * (8 * n + 80 + 6 * 65536);
}
// Smallest keep-every-Nth stride (1..64) whose population fits the preset ceiling and the live headroom (staging included); 0 = nothing fits.
constexpr uint32_t vegetationStride(uint64_t instances, uint64_t regions, uint32_t slots, uint64_t cap, uint64_t headroom) {
    for (uint32_t s = 1; s <= 64; ++s) {
        const uint64_t n = (instances + s - 1) / s, r = (std::min)(regions, n), b = vegetationBytes(n, r, slots);
        if (b <= cap && b + 32 * (n + r) <= headroom) return s;
    }
    return 0;
}
constexpr bool gpuFits(uint64_t limit, uint64_t live, uint64_t pending, uint64_t bytes) {
    return live <= limit && pending <= limit - live && bytes <= limit - live - pending;
}
}
