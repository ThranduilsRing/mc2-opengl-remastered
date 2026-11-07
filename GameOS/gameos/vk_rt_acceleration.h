// vk_rt_acceleration.h -- ray-tracing acceleration structures (VK_KHR_acceleration_structure): BLAS / TLAS build with a size query, buffer-device-address
// storage, aligned scratch, update, compaction query and compact copy.
//
// WHAT: the top half is Vulkan-free pure math (alignment, scratch sizing, the compaction decision, the barrier between build phases) and is unit-tested
// without a Vulkan link (tests/unit/test_vk_rt_layout.cpp). The bottom half records builds into a caller's command buffer; nothing here submits.
// ORDER: upload -> BLAS -> (compaction copy) -> TLAS -> consumer. `barrierBetween(from, to)` is the one place that names the stage and access masks of each
// edge; `recordBarrier` issues it through the context's sync2 route.
// LIFETIME: an AccelStructure owns its handle and its backing buffer (OwnedAccelStruct + OwnedBuffer). Retire it through VulkanContext::retire so the GPU
// finishes with it first (markDeferred is provided); a scratch buffer a build used is retired the same way once the build's ticket has been taken.
// Nothing here renders: the lane that adds scene integration consumes these entry points.
#ifndef MC2_VK_RT_ACCELERATION_H
#define MC2_VK_RT_ACCELERATION_H

#include <cstdint>

namespace mc2vk::rt {

constexpr uint64_t alignUp(uint64_t v, uint64_t a) { return a > 1 ? (v + a - 1) / a * a : v; }

// An acceleration structure's offset into its buffer must be a multiple of 256; each one gets a buffer of its own, sized up to that multiple.
inline constexpr uint64_t kAsStorageAlignment = 256;
// VkAccelerationStructureInstanceKHR is 64 bytes and the instance array's address must be 16-byte aligned.
inline constexpr uint32_t kInstanceBytes = 64;
inline constexpr uint64_t kInstanceAddressAlignment = 16;

constexpr uint64_t asStorageBytes(uint64_t asSize) { return alignUp(asSize ? asSize : 1, kAsStorageAlignment); }

// The scratch address handed to a build must be a multiple of minAccelerationStructureScratchOffsetAlignment, and a buffer's own base address only
// promises the buffer alignment, so the buffer is over-allocated by one alignment step and the build uses the aligned address inside it.
constexpr uint64_t scratchAllocBytes(uint64_t needed, uint64_t alignment) { return (needed ? needed : 1) + (alignment > 1 ? alignment : 0); }
constexpr uint64_t scratchAlignedAddress(uint64_t base, uint64_t alignment) { return alignUp(base, alignment); }
constexpr bool scratchFits(uint64_t base, uint64_t allocBytes, uint64_t needed, uint64_t alignment) {
    return scratchAlignedAddress(base, alignment) + needed <= base + allocBytes;
}

// A compaction copy is only worth its second buffer and its copy when it saves at least `minSavedPercent` of the original.
constexpr bool worthCompacting(uint64_t originalSize, uint64_t compactedSize, uint32_t minSavedPercent = 10) {
    return originalSize > 0 && compactedSize > 0 && compactedSize < originalSize && (originalSize - compactedSize) * 100 >= originalSize * minSavedPercent;
}

// The build phases of a frame, in the only legal order (Copy is the optional BLAS compaction between BLAS and TLAS).
enum class Phase { Upload, Blas, Copy, Tlas, Consumer };

constexpr int phaseRank(Phase p) { return static_cast<int>(p); }

// Stage / access masks as plain 64-bit values (the numeric values of the sync2 bits; vk_rt_acceleration.cpp static_asserts each against the Vulkan header).
inline constexpr uint64_t kStageTransfer = 0x1000ull;
inline constexpr uint64_t kStageComputeShader = 0x800ull;
inline constexpr uint64_t kStageRayTracingShader = 0x200000ull;
inline constexpr uint64_t kStageAsBuild = 0x02000000ull;
inline constexpr uint64_t kStageAsCopy = 0x10000000ull;
inline constexpr uint64_t kAccessTransferWrite = 0x1000ull;
inline constexpr uint64_t kAccessAsRead = 0x00200000ull;
inline constexpr uint64_t kAccessAsWrite = 0x00400000ull;

struct Barrier {
    bool valid = false;
    uint64_t srcStage = 0, srcAccess = 0, dstStage = 0, dstAccess = 0;
};

// The execution + memory dependency between two phases. Invalid when `to` does not come after `from` (a reversed or repeated edge is a recording bug).
constexpr Barrier barrierBetween(Phase from, Phase to) {
    Barrier b;
    if (phaseRank(to) <= phaseRank(from) || from == Phase::Consumer) return b;
    switch (from) {
        case Phase::Upload: b.srcStage = kStageTransfer; b.srcAccess = kAccessTransferWrite; break;
        case Phase::Blas:
        case Phase::Tlas: b.srcStage = kStageAsBuild; b.srcAccess = kAccessAsWrite; break;
        case Phase::Copy: b.srcStage = kStageAsCopy; b.srcAccess = kAccessAsWrite; break;
        case Phase::Consumer: return b;
    }
    switch (to) {
        case Phase::Upload: return Barrier{};
        case Phase::Blas:
        case Phase::Tlas: b.dstStage = kStageAsBuild; b.dstAccess = kAccessAsRead | kAccessAsWrite; break;   // inputs and the scratch are read and written
        case Phase::Copy: b.dstStage = kStageAsCopy; b.dstAccess = kAccessAsRead; break;
        case Phase::Consumer: b.dstStage = kStageComputeShader | kStageRayTracingShader; b.dstAccess = kAccessAsRead; break;
    }
    b.valid = true;
    return b;
}

}  // namespace mc2vk::rt

#ifdef MC2_VULKAN_ISLAND

#include <volk.h>

#include <string_view>

#include "vk_owned.h"

namespace mc2vk {
class VulkanContext;
}

namespace mc2vk::rt {

struct TriangleGeometry {
    VkDeviceAddress vertices = 0;
    uint32_t vertexStride = 12;
    uint32_t vertexCount = 0;
    VkFormat vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    VkDeviceAddress indices = 0;                     // 0: a non-indexed triangle list
    VkIndexType indexType = VK_INDEX_TYPE_UINT32;
    uint32_t triangleCount = 0;
    bool opaque = true;
};

struct BuildFlags {
    bool allowUpdate = false;                        // the structure can be refit with recordBlasUpdate / an update TLAS build (the topology must not change)
    bool allowCompaction = false;                    // the structure can be queried and compact-copied
    bool preferFastTrace = true;                     // false: prefer fast build (per-frame rebuilds)
};

// One acceleration structure: the handle and the buffer that backs it. Move-only; destroyed handle-first (member order), then the buffer.
class AccelStructure {
public:
    AccelStructure() = default;
    VkAccelerationStructureKHR get() const noexcept { return handle_.get(); }
    VkDeviceAddress address() const noexcept { return address_; }
    VkDeviceSize size() const noexcept { return size_; }
    bool isTop() const noexcept { return top_; }
    bool canUpdate() const noexcept { return (flags_ & VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR) != 0; }
    VkDeviceSize updateScratchSize() const noexcept { return updateScratch_; }
    uint32_t primitiveCount() const noexcept { return primitiveCount_; }
    explicit operator bool() const noexcept { return static_cast<bool>(handle_); }
    // For VulkanContext::retire (the GPU may still read it): marks the backing allocation as retiring in the ownership ledger.
    void markDeferred() { buffer_.markDeferred(); }

private:
    friend bool createAccelStructure(const VulkanContext&, VkAccelerationStructureTypeKHR, VkDeviceSize, std::string_view, AccelStructure&);
    friend bool recordBlasBuild(const VulkanContext&, VkCommandBuffer, const TriangleGeometry&, const BuildFlags&, std::string_view, AccelStructure&, OwnedBuffer&);
    friend bool recordBlasUpdate(const VulkanContext&, VkCommandBuffer, const TriangleGeometry&, AccelStructure&, OwnedBuffer&);
    friend bool recordTlasBuild(const VulkanContext&, VkCommandBuffer, VkDeviceAddress, uint32_t, const BuildFlags&, std::string_view, AccelStructure&,
                                OwnedBuffer&);
    friend bool recordTlasUpdate(const VulkanContext&, VkCommandBuffer, VkDeviceAddress, uint32_t, AccelStructure&, OwnedBuffer&);
    friend bool recordCompactCopy(const VulkanContext&, VkCommandBuffer, const AccelStructure&, VkDeviceSize, std::string_view, AccelStructure&);
    OwnedBuffer buffer_;                             // declared before handle_ so the handle is destroyed first
    OwnedAccelStruct handle_;
    VkDeviceAddress address_ = 0;
    VkDeviceSize size_ = 0;                          // the structure's size (VkAccelerationStructureBuildSizesInfoKHR::accelerationStructureSize)
    VkDeviceSize updateScratch_ = 0;
    VkBuildAccelerationStructureFlagsKHR flags_ = 0;
    uint32_t primitiveCount_ = 0;
    bool top_ = false;
};

// Creates an empty structure of `size` bytes (handle + BDA storage buffer). Used by the build entry points and by the compact copy.
bool createAccelStructure(const VulkanContext& ctx, VkAccelerationStructureTypeKHR type, VkDeviceSize size, std::string_view name, AccelStructure& out);

// Records a BLAS build of one triangle geometry into `cmd`: size query, structure + scratch creation, vkCmdBuildAccelerationStructuresKHR. The geometry
// buffers must carry SHADER_DEVICE_ADDRESS | ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY usage and be visible to the build (see Phase::Upload). `scratch`
// receives the scratch buffer: keep it alive until the submit completes, then retire it. False (nothing recorded) on any creation failure.
bool recordBlasBuild(const VulkanContext& ctx, VkCommandBuffer cmd, const TriangleGeometry& geometry, const BuildFlags& flags, std::string_view name,
                     AccelStructure& blas, OwnedBuffer& scratch);
// Refits `blas` in place (same topology: vertexCount / triangleCount unchanged; the structure was built with allowUpdate). Needs a scratch of updateScratchSize.
bool recordBlasUpdate(const VulkanContext& ctx, VkCommandBuffer cmd, const TriangleGeometry& geometry, AccelStructure& blas, OwnedBuffer& scratch);

// Records a TLAS build over `instanceCount` VkAccelerationStructureInstanceKHR (kInstanceBytes each, 16-byte-aligned address) at `instances`.
bool recordTlasBuild(const VulkanContext& ctx, VkCommandBuffer cmd, VkDeviceAddress instances, uint32_t instanceCount, const BuildFlags& flags,
                     std::string_view name, AccelStructure& tlas, OwnedBuffer& scratch);
bool recordTlasUpdate(const VulkanContext& ctx, VkCommandBuffer cmd, VkDeviceAddress instances, uint32_t instanceCount, AccelStructure& tlas, OwnedBuffer& scratch);

// One instance entry: an identity-or-given 3x4 transform (row-major, 12 floats; null = identity), the BLAS address, custom index and mask 0xFF.
VkAccelerationStructureInstanceKHR makeInstance(VkDeviceAddress blasAddress, const float* transform3x4, uint32_t customIndex = 0, uint8_t mask = 0xFF);

// Compaction. 1: a query pool of `count` compacted-size queries. 2: after the BLAS build (and a Blas -> Blas-read barrier), write the size query.
// 3: once the build's ticket completed, readCompactedSize. 4: recordCompactCopy into a fresh structure of that size.
bool createCompactionQueryPool(const VulkanContext& ctx, uint32_t count, OwnedQueryPool& pool);
void recordCompactionQuery(const VulkanContext& ctx, VkCommandBuffer cmd, VkQueryPool pool, uint32_t index, const AccelStructure& blas);
bool readCompactedSize(const VulkanContext& ctx, VkQueryPool pool, uint32_t index, VkDeviceSize& size);
bool recordCompactCopy(const VulkanContext& ctx, VkCommandBuffer cmd, const AccelStructure& src, VkDeviceSize compactedSize, std::string_view name,
                       AccelStructure& dst);

// Issues the barrier between two phases through the context's sync2 route (a no-op, with false, for an invalid edge).
bool recordBarrier(const VulkanContext& ctx, VkCommandBuffer cmd, Phase from, Phase to);

}  // namespace mc2vk::rt

#endif  // MC2_VULKAN_ISLAND
#endif  // MC2_VK_RT_ACCELERATION_H
