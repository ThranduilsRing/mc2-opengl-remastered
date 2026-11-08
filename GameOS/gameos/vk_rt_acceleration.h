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

#include <cstddef>
#include <cstdint>
#include <span>

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
// A held scratch allocation of `capacityBytes` serves a build needing `needed` bytes when the over-allocated size fits (any base address then aligns).
constexpr bool scratchReusable(uint64_t capacityBytes, uint64_t needed, uint64_t alignment) {
    return capacityBytes > 0 && scratchAllocBytes(needed, alignment) <= capacityBytes;
}

// BATCHED BLAS BUILDS: several builds in one vkCmdBuildAccelerationStructuresKHR, each on its own scratch sub-range (no barrier inside a call, so the
// sub-ranges must not overlap). Builds are taken in order into groups whose summed sub-ranges stay within `budget` (a group always holds at least one
// build); a group is one build call, and consecutive groups reuse the arena behind a Blas -> Tlas barrier. Writes each build's sub-range offset from the
// aligned arena base (a multiple of `alignment`) and its group; returns the largest group span, the scratch to reserve. `offsets` and `groups` hold at
// least needed.size() entries.
constexpr uint64_t scratchBatchPlan(std::span<const uint64_t> needed, uint64_t alignment, uint64_t budget, std::span<uint64_t> offsets,
                                    std::span<uint32_t> groups) {
    uint64_t span = 0, peak = 0;
    uint32_t group = 0;
    for (size_t i = 0; i < needed.size(); ++i) {
        const uint64_t bytes = alignUp(needed[i], alignment);   // 0: an empty geometry (no build), takes no room
        if (span > 0 && bytes > 0 && span + bytes > budget) {
            ++group;
            span = 0;
        }
        offsets[i] = span;
        groups[i] = group;
        span += bytes;
        peak = span > peak ? span : peak;
    }
    return peak;
}
inline constexpr uint64_t kScratchBatchBudget = 64ull << 20;   // per build call; a map's terrain tiles take a few groups

// A compaction copy is only worth its second buffer and its copy when it saves at least `minSavedPercent` of the original.
constexpr bool worthCompacting(uint64_t originalSize, uint64_t compactedSize, uint32_t minSavedPercent = 10) {
    return originalSize > 0 && compactedSize > 0 && compactedSize < originalSize && (originalSize - compactedSize) * 100 >= originalSize * minSavedPercent;
}

// Build-flag bits as plain values (VkBuildAccelerationStructureFlagBitsKHR; vk_rt_acceleration.cpp static_asserts them against the Vulkan header).
inline constexpr uint32_t kBuildAllowUpdate = 0x1u;
inline constexpr uint32_t kBuildAllowCompaction = 0x2u;

// Compaction is legal only for a structure built with ALLOW_COMPACTION, and never for an ALLOW_UPDATE one on AMD proprietary (the compacted copy of an
// updatable BLAS comes back as a structure every ray misses there). Skinned movers are ALLOW_UPDATE and never compact on any driver.
constexpr bool compactionAllowed(uint32_t buildFlags, bool amdProprietary) {
    return (buildFlags & kBuildAllowCompaction) != 0 && !((buildFlags & kBuildAllowUpdate) != 0 && amdProprietary);
}

// What a BLAS was built from, per geometry. A refit (BUILD_MODE_UPDATE) is legal only when every field of every geometry matches the build and the
// structure was built with ALLOW_UPDATE: counts, formats, stride, geometry flags, and the index source (same counts on a different mesh section = rebuild).
struct GeometrySignature {
    uint32_t vertexCount = 0;
    uint32_t triangleCount = 0;
    uint32_t vertexFormat = 0;
    uint32_t indexType = 0;
    uint32_t vertexStride = 0;
    uint32_t geometryFlags = 0;
    uint64_t indexSource = 0;
    constexpr bool operator==(const GeometrySignature&) const = default;
};

enum class RefitAction { Build, Refit };
// A refitted BVH degrades as the pose drifts from the build pose; after this many refits the structure is rebuilt (one fast build every ~2 s at 60 Hz).
inline constexpr uint32_t kRefitsBeforeRebuild = 120;

// Refit only when the structure exists (`built` non-empty), was built updatable, the flags are unchanged, the geometry signature matches element for
// element, and fewer than `maxRefits` refits ran since its build. Anything else rebuilds.
constexpr RefitAction refitAction(std::span<const GeometrySignature> built, uint32_t builtFlags, std::span<const GeometrySignature> now, uint32_t nowFlags,
                                  uint32_t refitsSinceBuild, uint32_t maxRefits = kRefitsBeforeRebuild) {
    if (built.empty() || (builtFlags & kBuildAllowUpdate) == 0 || builtFlags != nowFlags || built.size() != now.size() || refitsSinceBuild >= maxRefits)
        return RefitAction::Build;
    for (size_t i = 0; i < built.size(); ++i)
        if (!(built[i] == now[i])) return RefitAction::Build;
    return RefitAction::Refit;
}

// VkAccelerationStructureInstanceKHR::instanceCustomIndex is 24 bits: a larger value would silently alias another index, so it is refused, not masked.
inline constexpr uint32_t kMaxInstanceCustomIndex = 0xFFFFFFu;
constexpr bool instanceCustomIndexFits(uint32_t customIndex) { return customIndex <= kMaxInstanceCustomIndex; }

// The build phases of a frame, in the only legal order (Copy is the optional BLAS compaction between BLAS and TLAS).
enum class Phase { Upload, Blas, Copy, Tlas, Consumer };

constexpr int phaseRank(Phase p) { return static_cast<int>(p); }

// Stage / access masks as plain 64-bit values (the numeric values of the sync2 bits; vk_rt_acceleration.cpp static_asserts each against the Vulkan header).
inline constexpr uint64_t kStageTransfer = 0x1000ull;
inline constexpr uint64_t kStageComputeShader = 0x800ull;
inline constexpr uint64_t kStageRayTracingShader = 0x200000ull;
inline constexpr uint64_t kStageAsBuild = 0x02000000ull;
inline constexpr uint64_t kStageAsCopy = 0x10000000ull;
inline constexpr uint64_t kAccessShaderRead = 0x20ull;
inline constexpr uint64_t kAccessShaderWrite = 0x40ull;
inline constexpr uint64_t kAccessTransferWrite = 0x1000ull;
inline constexpr uint64_t kAccessAsRead = 0x00200000ull;
inline constexpr uint64_t kAccessAsWrite = 0x00400000ull;

struct Barrier {
    bool valid = false;
    uint64_t srcStage = 0, srcAccess = 0, dstStage = 0, dstAccess = 0;
};

// The execution + memory dependency between two phases. Invalid when `to` does not come after `from` (a reversed or repeated edge is a recording bug).
// - Copies (vkCmdCopyAccelerationStructureKHR) run in the AS-BUILD stage: the AS-COPY stage bit needs VK_KHR_ray_tracing_maintenance1, which the device
//   profile does not enable (VUID-VkMemoryBarrier2-srcStageMask-10752), so kStageAsCopy is never emitted.
// - Build inputs (vertex / index / instance buffers) are read at the AS-BUILD stage with SHADER_READ; Upload covers a transfer or a compute shader writing them.
// - The consumer is a compute-shader ray query: the ray-tracing-pipeline stage is invalid on a query-only device, and nothing traces from a pipeline yet.
constexpr Barrier barrierBetween(Phase from, Phase to) {
    Barrier b;
    if (phaseRank(to) <= phaseRank(from) || from == Phase::Consumer) return b;
    switch (from) {
        case Phase::Upload: b.srcStage = kStageTransfer | kStageComputeShader; b.srcAccess = kAccessTransferWrite | kAccessShaderWrite; break;
        case Phase::Blas:
        case Phase::Copy:
        case Phase::Tlas: b.srcStage = kStageAsBuild; b.srcAccess = kAccessAsWrite; break;
        case Phase::Consumer: return b;
    }
    switch (to) {
        case Phase::Upload: return Barrier{};
        case Phase::Blas:
        case Phase::Tlas:   // inputs are shader-read; source structures and the scratch are read and written
            b.dstStage = kStageAsBuild;
            b.dstAccess = kAccessAsRead | kAccessAsWrite | (from == Phase::Upload ? kAccessShaderRead : 0);
            break;
        case Phase::Copy: b.dstStage = kStageAsBuild; b.dstAccess = kAccessAsRead | kAccessAsWrite; break;
        case Phase::Consumer: b.dstStage = kStageComputeShader; b.dstAccess = kAccessAsRead; break;
    }
    b.valid = true;
    return b;
}

// The head of a per-frame rebuild in the frame command buffer: this frame's build inputs (a compute skin or a transfer) AND the previous frame's builds
// of the same structures and scratch (write-after-write) and its ray queries (write-after-read) -> this frame's AS builds. barrierBetween cannot name it
// (it would be a Tlas -> Blas / Consumer -> Blas edge, which it rejects as a reversed edge).
constexpr Barrier frameRebuildBarrier() {
    Barrier b = barrierBetween(Phase::Upload, Phase::Blas);
    b.srcStage |= kStageAsBuild;
    b.srcAccess |= kAccessAsWrite;
    return b;
}

}  // namespace mc2vk::rt


#ifndef MC2_RT_PURE_ONLY  // the unit tests include only the Vulkan-free half above

#include <volk.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "vk_owned.h"

namespace mc2vk {
class VulkanContext;
}

namespace mc2vk::rt {

// The build scratch of one owner (a scene, a self-test), reused across builds. A build that needs more than the held buffer replaces it; the replaced
// buffer may still be read by a build recorded earlier in an unsubmitted command buffer, so it parks in `stale` until the owner hands it to
// VulkanContext::retire (retireStale, after the submit) or retires the whole arena (retireAll, e.g. on map change). Destruction without a retire is only
// for the context's own teardown, after the device idled. Builds that share the arena in one command buffer need a Blas -> Tlas barrier between them
// (an AS-build write -> AS-build read/write dependency on the scratch).
class ScratchArena {
public:
    // The aligned scratch address for a build of `needed` bytes; false on an allocation failure (nothing recorded then).
    bool reserve(const VulkanContext& ctx, VkDeviceSize needed, std::string_view name, VkDeviceAddress& alignedAddress);
    void retireStale(VulkanContext& ctx);
    void retireAll(VulkanContext& ctx);
    VkDeviceSize capacity() const noexcept { return capacity_; }

private:
    OwnedBuffer buffer_;
    VkDeviceSize capacity_ = 0;
    std::vector<OwnedBuffer> stale_;
};

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

class AccelStructure;
// One multi-geometry BLAS build or refit for recordBlasJobs. Build: `blas` must be empty and is created here. Refit: `blas` was built updatable and
// refitAction(blas->signature(), ...) said Refit; a mismatch is refused here as well (ok stays false, nothing recorded for the job).
struct BlasJob {
    std::span<const TriangleGeometry> geometries;
    AccelStructure* blas = nullptr;
    bool refit = false;
    bool ok = false;                                 // out: recorded
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
    // The build-flag bits (kBuildAllowUpdate / kBuildAllowCompaction) and the per-geometry signature it was built from: refitAction's inputs.
    uint32_t buildFlagBits() const noexcept;
    std::span<const GeometrySignature> signature() const noexcept { return signature_; }
    // For VulkanContext::retire (the GPU may still read it): marks the backing allocation as retiring in the ownership ledger.
    void markDeferred() { buffer_.markDeferred(); }

private:
    friend std::optional<AccelStructure> createAccelStructure(const VulkanContext&, VkAccelerationStructureTypeKHR, VkDeviceSize, std::string_view);
    friend bool recordBlasBuild(const VulkanContext&, VkCommandBuffer, const TriangleGeometry&, const BuildFlags&, std::string_view, AccelStructure&, ScratchArena&);
    friend bool recordBlasBuildBatch(const VulkanContext&, VkCommandBuffer, const std::vector<TriangleGeometry>&, const BuildFlags&, std::string_view,
                                     std::vector<AccelStructure>&, ScratchArena&, uint32_t*, VkDeviceSize*);
    friend bool recordBlasUpdate(const VulkanContext&, VkCommandBuffer, const TriangleGeometry&, AccelStructure&, ScratchArena&);
    friend bool recordTlasBuild(const VulkanContext&, VkCommandBuffer, VkDeviceAddress, uint32_t, const BuildFlags&, std::string_view, AccelStructure&,
                                ScratchArena&);
    friend bool recordTlasUpdate(const VulkanContext&, VkCommandBuffer, VkDeviceAddress, uint32_t, AccelStructure&, ScratchArena&);
    friend bool recordTlasRebuild(const VulkanContext&, VkCommandBuffer, VkDeviceAddress, uint32_t, uint32_t, const BuildFlags&, std::string_view,
                                  AccelStructure&, ScratchArena&);
    friend bool recordCompactCopy(const VulkanContext&, VkCommandBuffer, const AccelStructure&, VkDeviceSize, std::string_view, AccelStructure&);
    friend bool recordBlasJobs(const VulkanContext&, VkCommandBuffer, std::span<BlasJob>, const BuildFlags&, std::string_view, ScratchArena&, uint32_t*,
                               VkDeviceSize*);
    OwnedBuffer buffer_;                             // declared before handle_ so the handle is destroyed first
    OwnedAccelStruct handle_;
    VkDeviceAddress address_ = 0;
    VkDeviceSize size_ = 0;                          // the structure's size (VkAccelerationStructureBuildSizesInfoKHR::accelerationStructureSize)
    VkDeviceSize updateScratch_ = 0;
    VkBuildAccelerationStructureFlagsKHR flags_ = 0;
    uint32_t primitiveCount_ = 0;
    bool top_ = false;
    std::vector<GeometrySignature> signature_;       // set by every BLAS build; a refit must match it (refitAction)
};

// Creates an empty structure of `size` bytes (handle + BDA storage buffer). Used by the build entry points and by the compact copy.
std::optional<AccelStructure> createAccelStructure(const VulkanContext& ctx, VkAccelerationStructureTypeKHR type, VkDeviceSize size, std::string_view name);

// Records a BLAS build of one triangle geometry into `cmd`: size query, structure + scratch creation, vkCmdBuildAccelerationStructuresKHR. The geometry
// buffers must carry SHADER_DEVICE_ADDRESS | ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY usage and be visible to the build (see Phase::Upload). Scratch
// comes from `scratch` (reused when big enough; a replaced buffer parks until the owner calls retireStale after the submit). Builds that share one arena
// in a command buffer need a Phase::Blas -> Phase::Tlas barrier between them. False (nothing recorded) on any creation failure.
bool recordBlasBuild(const VulkanContext& ctx, VkCommandBuffer cmd, const TriangleGeometry& geometry, const BuildFlags& flags, std::string_view name,
                     AccelStructure& blas, ScratchArena& scratch);
// Records one BLAS build per geometry (geometries with no triangles get an empty structure) batched per scratchBatchPlan: one
// vkCmdBuildAccelerationStructuresKHR per group, each build on its own scratch sub-range, a Blas -> Tlas barrier between groups. `out` is resized to
// geometries.size() and filled only when every allocation succeeded (else false, nothing recorded, `out` empty). `groupsOut` / `scratchOut`: the build
// calls recorded and the scratch span reserved, for the byte ledger. The caller still owes the Blas -> consumer barrier after the last group.
bool recordBlasBuildBatch(const VulkanContext& ctx, VkCommandBuffer cmd, const std::vector<TriangleGeometry>& geometries, const BuildFlags& flags,
                          std::string_view name, std::vector<AccelStructure>& out, ScratchArena& scratch, uint32_t* groupsOut = nullptr,
                          VkDeviceSize* scratchOut = nullptr);
// Refits `blas` in place (same topology: vertexCount / triangleCount unchanged; the structure was built with allowUpdate). Needs a scratch of updateScratchSize.
// Refused (false, nothing recorded) unless signatureOf(geometry) equals the build's one-geometry signature.
bool recordBlasUpdate(const VulkanContext& ctx, VkCommandBuffer cmd, const TriangleGeometry& geometry, AccelStructure& blas, ScratchArena& scratch);

// The refit signature of one geometry; the index source is the index address (a different mesh section with equal counts still rebuilds).
GeometrySignature signatureOf(const TriangleGeometry& geometry);

// Records every job in `jobs` (per-frame skinned movers: one BLAS per mover, one geometry per draw run) batched per scratchBatchPlan over `scratch`,
// a Blas -> Tlas barrier between groups. A job whose structure cannot be created, or a refit whose signature does not match, is skipped (ok false).
// False (nothing recorded) only when the scratch cannot be reserved. The caller owes the Blas -> Tlas barrier after the last group.
bool recordBlasJobs(const VulkanContext& ctx, VkCommandBuffer cmd, std::span<BlasJob> jobs, const BuildFlags& flags, std::string_view name, ScratchArena& scratch,
                    uint32_t* groupsOut = nullptr, VkDeviceSize* scratchOut = nullptr);

// Records a TLAS build over `instanceCount` VkAccelerationStructureInstanceKHR (kInstanceBytes each, 16-byte-aligned address) at `instances`.
bool recordTlasBuild(const VulkanContext& ctx, VkCommandBuffer cmd, VkDeviceAddress instances, uint32_t instanceCount, const BuildFlags& flags,
                     std::string_view name, AccelStructure& tlas, ScratchArena& scratch);
bool recordTlasUpdate(const VulkanContext& ctx, VkCommandBuffer cmd, VkDeviceAddress instances, uint32_t instanceCount, AccelStructure& tlas, ScratchArena& scratch);
// A full TLAS build (mode BUILD, never an update) that reuses `tlas` in place when it is big enough for `instanceCount`: for a structure rebuilt every frame
// in the frame command buffer, where the caller orders the previous frame's reads before it (frameRebuildBarrier). An empty `tlas` is created sized for
// max(capacity, instanceCount) instances. False, with nothing recorded, when the held structure is too small (the caller retires it and passes an empty one).
bool recordTlasRebuild(const VulkanContext& ctx, VkCommandBuffer cmd, VkDeviceAddress instances, uint32_t instanceCount, uint32_t capacity, const BuildFlags& flags,
                       std::string_view name, AccelStructure& tlas, ScratchArena& scratch);

// One instance entry: an identity-or-given 3x4 transform (row-major, 12 floats; null = identity), the BLAS address, custom index and mask 0xFF.
// A customIndex past 24 bits is refused (instanceCustomIndexFits): the entry comes back with mask 0 and a null BLAS reference, so no ray can hit it.
// `sbtOffset` (24 bits): with no SBT bound (ray query), the RT scene stores the instance's first material row there (row = offset + geometry index).
VkAccelerationStructureInstanceKHR makeInstance(VkDeviceAddress blasAddress, const float* transform3x4, uint32_t customIndex = 0, uint8_t mask = 0xFF,
                                                uint32_t sbtOffset = 0);

// Compaction. 1: a query pool of `count` compacted-size queries. 2: after the BLAS build (and a Blas -> Blas-read barrier), write the size query.
// 3: once the build's ticket completed, readCompactedSize. 4: recordCompactCopy into a fresh structure of that size.
OwnedQueryPool createCompactionQueryPool(const VulkanContext& ctx, uint32_t count);  // empty (false) on failure
void recordCompactionQuery(const VulkanContext& ctx, VkCommandBuffer cmd, VkQueryPool pool, uint32_t index, const AccelStructure& blas);
std::optional<VkDeviceSize> readCompactedSize(const VulkanContext& ctx, VkQueryPool pool, uint32_t index);
bool recordCompactCopy(const VulkanContext& ctx, VkCommandBuffer cmd, const AccelStructure& src, VkDeviceSize compactedSize, std::string_view name,
                       AccelStructure& dst);

// Issues the barrier between two phases through the context's sync2 route (a no-op, with false, for an invalid edge).
bool recordBarrier(const VulkanContext& ctx, VkCommandBuffer cmd, Phase from, Phase to);
bool recordBarrier(const VulkanContext& ctx, VkCommandBuffer cmd, const Barrier& b);

// Which check made the last record call on this thread return false, with its values ("" when none since the last take). A FAIL line prints it.
std::string takeLastRecordFailure();

}  // namespace mc2vk::rt

#endif  // MC2_RT_PURE_ONLY
#endif  // MC2_VK_RT_ACCELERATION_H
