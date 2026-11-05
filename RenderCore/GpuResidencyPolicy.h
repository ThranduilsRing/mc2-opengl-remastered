#pragma once
#include <algorithm>
#include <cstdint>

namespace RenderCore {
inline constexpr uint64_t gpuMiB = 1024ull * 1024;
// Vegetation's packed population model, shared by the upload admission, the stride thinning and the placer's capacity:
// 16 B instances, 32 B regions plus a 16 B per-region cull word pair, the dense visible list (8 B per slot, capped) and the
// card array. The q2 ceiling is the 80 MiB the vegetation design budgets, so a dense mod thins by stride instead of refusing.
inline constexpr uint32_t vegetationVisibleCapacity = 512u * 1024u;
inline constexpr uint64_t vegetationResidentCeiling = 80 * gpuMiB;
// Logical residency includes host-visible staging, flight copies and retired resources.
struct GpuResidencyPolicy {
    uint64_t total, textureUpload, vegetation;
    uint32_t textureEdge, terrainEdge;
};
constexpr GpuResidencyPolicy gpuResidencyPolicy(int quality) {
    // The frame ring needs 96 MiB including flight copies. Its upload admission must
    // fit that allocation while all resources still share the 512 MiB low ceiling.
    if (quality == 0) return {512 * gpuMiB, 128 * gpuMiB, 0, 2048, 2048};
    if (quality == 1) return {1024 * gpuMiB, 128 * gpuMiB, 32 * gpuMiB, 4096, 4096};
    // q2 is bounded by the device (gpuBudget of the device-local heap), not by a fixed 2 GiB: 4K render targets, the shadow
    // bake and mech/light feeds exceeded 2 GiB on 24 GiB cards and were refused. 8 GiB is the sanity ceiling.
    return {8192 * gpuMiB, 256 * gpuMiB, vegetationResidentCeiling, 16384, 4096};
}
constexpr uint64_t gpuBudget(uint64_t available, uint64_t ceiling) {
    // Absolute slack for other processes/driver: 10%, at least 256 MiB, never more than a quarter of a small heap.
    const uint64_t margin = (std::min)((std::max)(256 * gpuMiB, available / 10), available / 4);
    return (std::min)(ceiling, available > margin ? available - margin : 0);
}
// Bytes of the device-local heap the ledger may plan against. With VK_EXT_memory_budget the driver's live budget/usage is
// authoritative. Without it nothing says what other apps hold, so assume half the heap. A unified-memory device (iGPU: the
// "device" heap is system RAM) is further capped to a quarter of that heap.
constexpr uint64_t gpuHeapAvailable(uint64_t size, uint64_t budget, uint64_t usage, bool haveBudgetExt, bool uma) {
    const uint64_t cap = haveBudgetExt ? (std::min)(size, budget) : size / 2;
    uint64_t avail = cap > usage ? cap - usage : 0;
    if (uma) avail = (std::min)(avail, size / 4);
    return avail;
}
// Device-resident bytes of a vegetation population (the model above). The upload also holds a transient staging copy of the
// instances and regions (vegetationStagingBytes).
constexpr uint64_t vegetationBytes(uint64_t instances, uint64_t regions) {
    return 16 * instances + 48 * regions + 8 * (std::min)(instances, static_cast<uint64_t>(vegetationVisibleCapacity)) + 3 * gpuMiB + 160;
}
constexpr uint64_t vegetationStagingBytes(uint64_t instances, uint64_t regions) { return 16 * instances + 32 * regions; }
// Smallest keep-every-Nth stride (1..64) whose population fits the preset ceiling and the live headroom (staging included); 0 = nothing fits.
constexpr uint32_t vegetationStride(uint64_t instances, uint64_t regions, uint64_t cap, uint64_t headroom) {
    for (uint32_t s = 1; s <= 64; ++s) {
        const uint64_t n = (instances + s - 1) / s, r = (std::min)(regions, n), b = vegetationBytes(n, r);
        if (b <= cap && b + vegetationStagingBytes(n, r) <= headroom) return s;
    }
    return 0;
}
constexpr bool gpuFits(uint64_t limit, uint64_t live, uint64_t pending, uint64_t bytes) {
    return live <= limit && pending <= limit - live && bytes <= limit - live - pending;
}
}
