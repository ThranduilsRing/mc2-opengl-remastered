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
    return {2048 * gpuMiB, 256 * gpuMiB, 96 * gpuMiB, 8192, 4096};
}
constexpr uint64_t gpuBudget(uint64_t available, uint64_t ceiling) {
    const uint64_t margin = std::max(32 * gpuMiB, available / 10);
    return std::min(ceiling, available > margin ? available - margin : 0);
}
constexpr bool gpuFits(uint64_t limit, uint64_t live, uint64_t pending, uint64_t bytes) {
    return live <= limit && pending <= limit - live && bytes <= limit - live - pending;
}
}
