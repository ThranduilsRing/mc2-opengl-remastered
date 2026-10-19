#pragma once

#include <cstdint>

namespace gpu_driven {

// 16-byte std430-aligned per-bucket header. visibleCount is an atomicAdd
// target in the compute shader. pad slots reserved for future telemetry.
struct GpuDrivenBucketHeader {
    uint32_t visibleCount;
    uint32_t pad0_;
    uint32_t pad1_;
    uint32_t pad2_;
};
static_assert(sizeof(GpuDrivenBucketHeader) == 16,
              "GpuDrivenBucketHeader must be 16 B");

// Env-var cache helpers. All cached at process start via static const bool.
// Pattern matches gos_terrain_lighting::IsEnabled() / IsParityCheckEnabled().
bool IsGlobalEnabled();        // MC2_GPU_DRIVEN: unset OR != "0" -> true (default ON)
bool IsParityEnabled();        // MC2_GPU_DRIVEN_PARITY: == "1" -> true (default OFF)
bool IsTraceEnabled();         // MC2_GPU_DRIVEN_TRACE: == "1" -> true (default OFF)
bool IsWaterEnabled();         // MC2_GPU_DRIVEN_WATER: unset OR != "0" -> true AND IsGlobalEnabled()
bool IsTerrainSolidEnabled();  // MC2_GPU_DRIVEN_TERRAIN_SOLID: same pattern
bool IsOverlayEnabled();       // MC2_GPU_DRIVEN_OVERLAY: same pattern

}  // namespace gpu_driven
