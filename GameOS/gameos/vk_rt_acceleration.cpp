// vk_rt_acceleration.cpp -- see vk_rt_acceleration.h.


#include "vk_rt_acceleration.h"

#include "vulkan_context.h"
#include "vk_gpu_profile.h"
#include <tracy/Tracy.hpp>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <format>
#include <utility>

namespace mc2vk::rt {

// The pure half carries the numeric values of the sync2 bits; they must be the real ones.
static_assert(kStageTransfer == VK_PIPELINE_STAGE_2_TRANSFER_BIT);
static_assert(kStageComputeShader == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
static_assert(kStageRayTracingShader == VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR);
static_assert(kStageAsBuild == VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR);
static_assert(kStageAsCopy == VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_COPY_BIT_KHR);
static_assert(kAccessShaderRead == VK_ACCESS_2_SHADER_READ_BIT);
static_assert(kAccessShaderWrite == VK_ACCESS_2_SHADER_WRITE_BIT);
static_assert(kAccessTransferWrite == VK_ACCESS_2_TRANSFER_WRITE_BIT);
static_assert(kAccessAsRead == VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
static_assert(kAccessAsWrite == VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
static_assert(sizeof(VkAccelerationStructureInstanceKHR) == kInstanceBytes);
static_assert(kBuildAllowUpdate == VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR);
static_assert(kBuildAllowCompaction == VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR);

namespace {

thread_local std::string t_lastRecordFailure;
template <class... Args> bool failWith(std::format_string<Args...> fmt, Args&&... args) {
    t_lastRecordFailure = std::format(fmt, std::forward<Args>(args)...);
    return false;
}
// True (and the failure recorded) when the instance array cannot feed a TLAS build.
bool instancesBad(VkDeviceAddress instances, uint32_t count) {
    if (count == 0 || instances == 0 || instances % kInstanceAddressAlignment != 0)
        return !failWith("instances count={} addr=0x{:x} addr%16={}", count, instances, instances % kInstanceAddressAlignment);
    return false;
}

VkBuildAccelerationStructureFlagsKHR toVkFlags(const BuildFlags& f) {
    VkBuildAccelerationStructureFlagsKHR v = f.preferFastTrace ? VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR
                                                               : VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
    if (f.allowUpdate) v |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    if (f.allowCompaction) v |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
    return v;
}

VkAccelerationStructureGeometryKHR trianglesOf(const TriangleGeometry& g) {
    VkAccelerationStructureGeometryKHR geo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geo.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geo.flags = g.opaque ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0;
    geo.geometry.triangles = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
    geo.geometry.triangles.vertexFormat = g.vertexFormat;
    geo.geometry.triangles.vertexData.deviceAddress = g.vertices;
    geo.geometry.triangles.vertexStride = g.vertexStride;
    geo.geometry.triangles.maxVertex = g.vertexCount ? g.vertexCount - 1 : 0;
    geo.geometry.triangles.indexType = g.indices ? g.indexType : VK_INDEX_TYPE_NONE_KHR;
    geo.geometry.triangles.indexData.deviceAddress = g.indices;
    return geo;
}

VkAccelerationStructureGeometryKHR instancesOf(VkDeviceAddress instances) {
    VkAccelerationStructureGeometryKHR geo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geo.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geo.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    geo.geometry.instances = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
    geo.geometry.instances.arrayOfPointers = VK_FALSE;
    geo.geometry.instances.data.deviceAddress = instances;
    return geo;
}

}  // namespace

bool ScratchArena::reserve(const VulkanContext& ctx, VkDeviceSize needed, std::string_view name, VkDeviceAddress& alignedAddress) {
    const uint64_t align = ctx.rtCaps().minAccelerationStructureScratchOffsetAlignment;
    if (!buffer_ || !scratchReusable(capacity_, needed, align)) {
        if (buffer_) stale_.push_back(std::move(buffer_));   // an earlier recorded build may still read it: retired by retireStale
        capacity_ = 0;
        OwnedBuffer fresh;
        const VkDeviceSize bytes = scratchAllocBytes(needed, align);
        if (!fresh.createDeviceLocal(ctx.allocator(), ctx.device(), bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, name,
                                     -1, -1, align))
            return failWith("scratch_alloc bytes={} result={}", bytes, static_cast<int>(takeLastCreateFailure()));
        buffer_ = std::move(fresh);
        capacity_ = bytes;
    }
    alignedAddress = scratchAlignedAddress(buffer_.deviceAddress(), align);
    if (!scratchFits(buffer_.deviceAddress(), capacity_, needed, align))
        return failWith("scratch_fit base=0x{:x} base%align={} align={} capacity={} need={}", buffer_.deviceAddress(), buffer_.deviceAddress() % align, align,
                        capacity_, needed);
    return true;
}

void ScratchArena::retireStale(VulkanContext& ctx) {
    for (OwnedBuffer& b : stale_) ctx.retire(std::move(b));
    stale_.clear();
}

void ScratchArena::retireAll(VulkanContext& ctx) {
    retireStale(ctx);
    if (buffer_) ctx.retire(std::move(buffer_), capacity_);
    capacity_ = 0;
}

namespace {

// Shared by BLAS and TLAS: query the sizes for `geo` x `primitives`, create structure + scratch, record the build (or the update when `update`).
bool recordBuild(const VulkanContext& ctx, VkCommandBuffer cmd, VkAccelerationStructureTypeKHR type, const VkAccelerationStructureGeometryKHR& geo,
                 uint32_t primitives, VkBuildAccelerationStructureFlagsKHR flags, std::string_view name, bool update, AccelStructure& as,
                 ScratchArena& scratch, VkDeviceSize* outUpdateScratch, VkDeviceSize* outSize, bool createNew) {
    VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    info.type = type;
    info.flags = flags;
    info.mode = update ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    info.geometryCount = 1;
    info.pGeometries = &geo;
    VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vkGetAccelerationStructureBuildSizesKHR(ctx.device(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &primitives, &sizes);
    if (outUpdateScratch) *outUpdateScratch = sizes.updateScratchSize;
    if (outSize) *outSize = sizes.accelerationStructureSize;
    // A fresh build goes into an empty `as` only: overwriting a live structure would destroy it while the GPU may still read it. The caller builds the
    // replacement into a new owner, publishes it on success and retires the old one. `as` is assigned only once every allocation has succeeded.
    if (createNew && as) return failWith("dst_not_empty size={}", as.size());
    std::optional<AccelStructure> created;
    if (createNew) {
        created = createAccelStructure(ctx, type, sizes.accelerationStructureSize, name);
        if (!created) return false;   // createAccelStructure recorded why
    }
    VkDeviceAddress scratchAddr = 0;
    if (!scratch.reserve(ctx, update ? sizes.updateScratchSize : sizes.buildScratchSize, name, scratchAddr)) return false;
    if (created) as = std::move(*created);
    info.srcAccelerationStructure = update ? as.get() : VK_NULL_HANDLE;
    info.dstAccelerationStructure = as.get();
    info.scratchData.deviceAddress = scratchAddr;
    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = primitives;
    const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &info, &ranges);
    return true;
}

}  // namespace

std::string takeLastRecordFailure() { return std::exchange(t_lastRecordFailure, std::string{}); }

std::optional<AccelStructure> createAccelStructure(const VulkanContext& ctx, VkAccelerationStructureTypeKHR type, VkDeviceSize size, std::string_view name) {
    if (!ctx.rt().accel) return std::nullopt;
    AccelStructure out;
    const VkDeviceSize bytes = asStorageBytes(size);
    if (!out.buffer_.createDeviceLocal(ctx.allocator(), ctx.device(), bytes,
                                       VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, name, -1, -1,
                                       kAsStorageAlignment)) {
        failWith("as_storage_alloc bytes={} result={}", bytes, static_cast<int>(takeLastCreateFailure()));
        return std::nullopt;
    }
    if (out.buffer_.deviceAddress() % kAsStorageAlignment != 0) {
        failWith("as_storage_align addr=0x{:x} addr%256={}", out.buffer_.deviceAddress(), out.buffer_.deviceAddress() % kAsStorageAlignment);
        return std::nullopt;
    }
    VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    ci.buffer = out.buffer_.get();
    ci.offset = 0;
    ci.size = size;
    ci.type = type;
    const VkDevice device = ctx.device();
    if (!mc2vk::create(out.handle_, device, [&](VkAccelerationStructureKHR* h) { return vkCreateAccelerationStructureKHR(device, &ci, nullptr, h); })) {
        failWith("as_handle size={} result={}", size, static_cast<int>(takeLastCreateFailure()));
        return std::nullopt;
    }
    VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    ai.accelerationStructure = out.handle_.get();
    out.address_ = vkGetAccelerationStructureDeviceAddressKHR(device, &ai);
    out.size_ = size;
    out.top_ = type == VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    return out;
}

bool recordBlasBuild(const VulkanContext& ctx, VkCommandBuffer cmd, const TriangleGeometry& g, const BuildFlags& flags, std::string_view name, AccelStructure& blas,
                     ScratchArena& scratch) {
    if (!ctx.rt().accel || g.triangleCount == 0 || g.vertices == 0) return false;
    const VkAccelerationStructureGeometryKHR geo = trianglesOf(g);
    VkDeviceSize updateScratch = 0, size = 0;
    const VkBuildAccelerationStructureFlagsKHR vkFlags = toVkFlags(flags);
    if (!recordBuild(ctx, cmd, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, geo, g.triangleCount, vkFlags, name, false, blas, scratch, &updateScratch, &size,
                     true))
        return false;
    blas.flags_ = vkFlags;
    blas.updateScratch_ = updateScratch;
    blas.primitiveCount_ = g.triangleCount;
    blas.signature_.assign(1, signatureOf(g));
    return true;
}

bool recordBlasBuildBatch(const VulkanContext& ctx, VkCommandBuffer cmd, const std::vector<TriangleGeometry>& geometries, const BuildFlags& flags,
                          std::string_view name, std::vector<AccelStructure>& out, ScratchArena& scratch, uint32_t* groupsOut, VkDeviceSize* scratchOut) {
    out.clear();
    if (groupsOut) *groupsOut = 0;
    if (scratchOut) *scratchOut = 0;
    if (!ctx.rt().accel) return false;
    const uint32_t n = static_cast<uint32_t>(geometries.size());
    const VkBuildAccelerationStructureFlagsKHR vkFlags = toVkFlags(flags);
    std::vector<VkAccelerationStructureGeometryKHR> geos(n);
    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> infos(n, {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR});
    std::vector<uint64_t> need(n, 0), offsets(n, 0), updateScratch(n, 0);
    std::vector<uint32_t> group(n, 0);
    std::vector<AccelStructure> built(n);
    for (uint32_t i = 0; i < n; ++i) {
        const TriangleGeometry& g = geometries[i];
        if (g.triangleCount == 0 || g.vertices == 0) continue;
        geos[i] = trianglesOf(g);
        VkAccelerationStructureBuildGeometryInfoKHR& info = infos[i];
        info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        info.flags = vkFlags;
        info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        info.geometryCount = 1;
        info.pGeometries = &geos[i];
        VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        vkGetAccelerationStructureBuildSizesKHR(ctx.device(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &g.triangleCount, &sizes);
        std::optional<AccelStructure> created = createAccelStructure(ctx, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, sizes.accelerationStructureSize, name);
        if (!created) return false;   // nothing recorded yet: `built` is destroyed directly
        built[i] = std::move(*created);
        need[i] = sizes.buildScratchSize ? sizes.buildScratchSize : 1;
        updateScratch[i] = sizes.updateScratchSize;
    }
    const uint64_t align = ctx.rtCaps().minAccelerationStructureScratchOffsetAlignment;
    const uint64_t peak = scratchBatchPlan(need, align, kScratchBatchBudget, offsets, group);
    VkDeviceAddress base = 0;
    if (peak > 0 && !scratch.reserve(ctx, peak, name, base)) return false;
    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> callInfos;
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> callRanges;
    std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> callRangePtrs;
    uint32_t calls = 0;
    for (uint32_t i = 0; i < n;) {
        const uint32_t g = group[i];
        callInfos.clear();
        callRanges.clear();
        for (; i < n && group[i] == g; ++i) {
            if (!built[i]) continue;
            infos[i].dstAccelerationStructure = built[i].get();
            infos[i].scratchData.deviceAddress = base + offsets[i];
            callInfos.push_back(infos[i]);
            VkAccelerationStructureBuildRangeInfoKHR r{};
            r.primitiveCount = geometries[i].triangleCount;
            callRanges.push_back(r);
        }
        if (callInfos.empty()) continue;
        callRangePtrs.clear();
        for (const VkAccelerationStructureBuildRangeInfoKHR& r : callRanges) callRangePtrs.push_back(&r);
        if (calls > 0) recordBarrier(ctx, cmd, Phase::Blas, Phase::Tlas);   // the next group reuses the arena
        vkCmdBuildAccelerationStructuresKHR(cmd, static_cast<uint32_t>(callInfos.size()), callInfos.data(), callRangePtrs.data());
        ++calls;
    }
    for (uint32_t i = 0; i < n; ++i) {
        if (!built[i]) continue;
        built[i].flags_ = vkFlags;
        built[i].updateScratch_ = updateScratch[i];
        built[i].primitiveCount_ = geometries[i].triangleCount;
        built[i].signature_.assign(1, signatureOf(geometries[i]));
    }
    out = std::move(built);
    if (groupsOut) *groupsOut = calls;
    if (scratchOut) *scratchOut = peak;
    return true;
}

bool recordBlasUpdate(const VulkanContext& ctx, VkCommandBuffer cmd, const TriangleGeometry& g, AccelStructure& blas, ScratchArena& scratch) {
    if (!blas || blas.isTop()) return false;
    const GeometrySignature now = signatureOf(g);
    if (refitAction(blas.signature_, blas.buildFlagBits(), std::span<const GeometrySignature>(&now, 1), blas.buildFlagBits(), 0, 1) != RefitAction::Refit)
        return false;
    const VkAccelerationStructureGeometryKHR geo = trianglesOf(g);
    return recordBuild(ctx, cmd, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, geo, g.triangleCount, blas.flags_, "blas_update", true, blas, scratch, nullptr,
                       nullptr, false);
}

GeometrySignature signatureOf(const TriangleGeometry& g) {
    GeometrySignature s;
    s.vertexCount = g.vertexCount;
    s.triangleCount = g.triangleCount;
    s.vertexFormat = static_cast<uint32_t>(g.vertexFormat);
    s.indexType = g.indices ? static_cast<uint32_t>(g.indexType) : static_cast<uint32_t>(VK_INDEX_TYPE_NONE_KHR);
    s.vertexStride = g.vertexStride;
    s.geometryFlags = g.opaque ? static_cast<uint32_t>(VK_GEOMETRY_OPAQUE_BIT_KHR) : 0u;
    s.indexSource = g.indices;
    return s;
}

uint32_t AccelStructure::buildFlagBits() const noexcept {
    return static_cast<uint32_t>(flags_ & (VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR));
}

bool recordBlasJobs(const VulkanContext& ctx, VkCommandBuffer cmd, std::span<BlasJob> jobs, const BuildFlags& flags, std::string_view name, ScratchArena& scratch,
                    uint32_t* groupsOut, VkDeviceSize* scratchOut) {
    ZoneScopedN("RT.BLAS.Record");
    MC2_VK_GPU_SCOPE(blasScope, cmd, "RT.BLAS.Build");
    if (groupsOut) *groupsOut = 0;
    if (scratchOut) *scratchOut = 0;
    for (BlasJob& j : jobs) j.ok = false;
    if (!ctx.rt().accel) return false;
    const VkBuildAccelerationStructureFlagsKHR vkFlags = toVkFlags(flags);
    const uint32_t flagBits = static_cast<uint32_t>(flags.allowUpdate ? kBuildAllowUpdate : 0u) | (flags.allowCompaction ? kBuildAllowCompaction : 0u);
    const size_t n = jobs.size();
    size_t totalGeos = 0;
    for (const BlasJob& j : jobs) totalGeos += j.geometries.size();
    // Flat storage: every info's pGeometries / range pointer stays valid until the build calls are recorded.
    std::vector<VkAccelerationStructureGeometryKHR> geos(totalGeos);
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges(totalGeos);
    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> infos(n, {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR});
    std::vector<size_t> firstGeo(n, 0);
    std::vector<uint64_t> need(n, 0), offsets(n, 0), updateScratch(n, 0);
    std::vector<uint32_t> group(n, 0), primitives;
    std::vector<GeometrySignature> sig;
    std::vector<std::vector<GeometrySignature>> sigs(n);
    std::vector<AccelStructure> created(n);
    std::vector<uint8_t> live(n, 0);
    size_t at = 0;
    for (size_t i = 0; i < n; ++i) {
        BlasJob& j = jobs[i];
        firstGeo[i] = at;
        const size_t gc = j.geometries.size();
        at += gc;
        if (!j.blas || gc == 0 || (!j.refit && *j.blas)) continue;
        bool valid = true;
        sig.clear();
        primitives.assign(gc, 0);
        for (size_t k = 0; k < gc; ++k) {
            const TriangleGeometry& g = j.geometries[k];
            if (g.triangleCount == 0 || g.vertices == 0) valid = false;
            geos[firstGeo[i] + k] = trianglesOf(g);
            ranges[firstGeo[i] + k] = VkAccelerationStructureBuildRangeInfoKHR{g.triangleCount, 0, 0, 0};
            primitives[k] = g.triangleCount;
            sig.push_back(signatureOf(g));
        }
        if (!valid) continue;
        if (j.refit && (j.blas->isTop() || refitAction(j.blas->signature_, j.blas->buildFlagBits(), sig, flagBits, 0, 1) != RefitAction::Refit)) continue;
        VkAccelerationStructureBuildGeometryInfoKHR& info = infos[i];
        info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        info.flags = j.refit ? j.blas->flags_ : vkFlags;
        info.mode = j.refit ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        info.geometryCount = static_cast<uint32_t>(gc);
        info.pGeometries = &geos[firstGeo[i]];
        if (j.refit) {
            need[i] = j.blas->updateScratch_ ? j.blas->updateScratch_ : 1;
        } else {
            VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
            vkGetAccelerationStructureBuildSizesKHR(ctx.device(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, primitives.data(), &sizes);
            std::optional<AccelStructure> as = createAccelStructure(ctx, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, sizes.accelerationStructureSize, name);
            if (!as) continue;
            created[i] = std::move(*as);
            need[i] = sizes.buildScratchSize ? sizes.buildScratchSize : 1;
            updateScratch[i] = sizes.updateScratchSize;
            sigs[i] = sig;
        }
        live[i] = 1;
    }
    const uint64_t align = ctx.rtCaps().minAccelerationStructureScratchOffsetAlignment;
    const uint64_t peak = scratchBatchPlan(need, align, kScratchBatchBudget, offsets, group);
    VkDeviceAddress base = 0;
    if (peak > 0 && !scratch.reserve(ctx, peak, name, base)) return false;   // `created` is destroyed directly: nothing recorded
    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> callInfos;
    std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> callRanges;
    uint32_t calls = 0;
    for (size_t i = 0; i < n;) {
        const uint32_t g = group[i];
        callInfos.clear();
        callRanges.clear();
        for (; i < n && group[i] == g; ++i) {
            if (!live[i]) continue;
            AccelStructure& dst = jobs[i].refit ? *jobs[i].blas : created[i];
            infos[i].srcAccelerationStructure = jobs[i].refit ? dst.get() : VK_NULL_HANDLE;
            infos[i].dstAccelerationStructure = dst.get();
            infos[i].scratchData.deviceAddress = base + offsets[i];
            callInfos.push_back(infos[i]);
            callRanges.push_back(&ranges[firstGeo[i]]);
        }
        if (callInfos.empty()) continue;
        if (calls > 0) recordBarrier(ctx, cmd, Phase::Blas, Phase::Tlas);   // the next group reuses the arena
        vkCmdBuildAccelerationStructuresKHR(cmd, static_cast<uint32_t>(callInfos.size()), callInfos.data(), callRanges.data());
        ++calls;
    }
    for (size_t i = 0; i < n; ++i) {
        if (!live[i]) continue;
        jobs[i].ok = true;
        if (jobs[i].refit) continue;
        AccelStructure& out = created[i];
        out.flags_ = vkFlags;
        out.updateScratch_ = updateScratch[i];
        uint32_t prims = 0;
        for (const TriangleGeometry& tg : jobs[i].geometries) prims += tg.triangleCount;
        out.primitiveCount_ = prims;
        out.signature_ = std::move(sigs[i]);
        *jobs[i].blas = std::move(out);
    }
    if (groupsOut) *groupsOut = calls;
    if (scratchOut) *scratchOut = peak;
    return true;
}

bool recordTlasBuild(const VulkanContext& ctx, VkCommandBuffer cmd, VkDeviceAddress instances, uint32_t instanceCount, const BuildFlags& flags, std::string_view name,
                     AccelStructure& tlas, ScratchArena& scratch) {
    ZoneScopedN("RT.TLAS.Record");
    MC2_VK_GPU_SCOPE(tlasScope, cmd, "RT.TLAS.Build");
    if (!ctx.rt().accel) return failWith("no_accel");
    if (instancesBad(instances, instanceCount)) return false;
    const VkAccelerationStructureGeometryKHR geo = instancesOf(instances);
    VkDeviceSize updateScratch = 0, size = 0;
    const VkBuildAccelerationStructureFlagsKHR vkFlags = toVkFlags(flags);
    if (!recordBuild(ctx, cmd, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, geo, instanceCount, vkFlags, name, false, tlas, scratch, &updateScratch, &size, true))
        return false;
    tlas.flags_ = vkFlags;
    tlas.updateScratch_ = updateScratch;
    tlas.primitiveCount_ = instanceCount;
    return true;
}

bool recordTlasRebuild(const VulkanContext& ctx, VkCommandBuffer cmd, VkDeviceAddress instances, uint32_t instanceCount, uint32_t capacity, const BuildFlags& flags,
                       std::string_view name, AccelStructure& tlas, ScratchArena& scratch) {
    ZoneScopedN("RT.TLAS.Record");
    MC2_VK_GPU_SCOPE(tlasScope, cmd, "RT.TLAS.Build");
    if (!ctx.rt().accel) return failWith("no_accel");
    if (instancesBad(instances, instanceCount)) return false;
    if (tlas && !tlas.isTop()) return false;
    const VkAccelerationStructureGeometryKHR geo = instancesOf(instances);
    const VkBuildAccelerationStructureFlagsKHR vkFlags = toVkFlags(flags);
    auto sizesFor = [&](uint32_t primitives) {
        VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        info.flags = vkFlags;
        info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        info.geometryCount = 1;
        info.pGeometries = &geo;
        VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        vkGetAccelerationStructureBuildSizesKHR(ctx.device(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &primitives, &sizes);
        return sizes;
    };
    const VkAccelerationStructureBuildSizesInfoKHR need = sizesFor(instanceCount);
    if (tlas && need.accelerationStructureSize > tlas.size()) return failWith("tlas_small need={} have={}", need.accelerationStructureSize, tlas.size());   // too small: the caller retires it and passes an empty one
    if (!tlas) {
        const VkAccelerationStructureBuildSizesInfoKHR cap = sizesFor(std::max(capacity, instanceCount));
        auto created = createAccelStructure(ctx, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, cap.accelerationStructureSize, name);
        if (!created) return false;
        tlas = std::move(*created);
    }
    VkDeviceAddress scratchAddr = 0;
    if (!scratch.reserve(ctx, need.buildScratchSize, name, scratchAddr)) return false;
    VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    info.flags = vkFlags;
    info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    info.geometryCount = 1;
    info.pGeometries = &geo;
    info.dstAccelerationStructure = tlas.get();
    info.scratchData.deviceAddress = scratchAddr;
    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = instanceCount;
    const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &info, &ranges);
    tlas.flags_ = vkFlags;
    tlas.updateScratch_ = need.updateScratchSize;
    tlas.primitiveCount_ = instanceCount;
    return true;
}

bool recordTlasUpdate(const VulkanContext& ctx, VkCommandBuffer cmd, VkDeviceAddress instances, uint32_t instanceCount, AccelStructure& tlas, ScratchArena& scratch) {
    if (!tlas || !tlas.isTop() || !tlas.canUpdate() || instanceCount != tlas.primitiveCount_ || instances == 0 || instances % kInstanceAddressAlignment != 0)
        return false;
    const VkAccelerationStructureGeometryKHR geo = instancesOf(instances);
    return recordBuild(ctx, cmd, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, geo, instanceCount, tlas.flags_, "tlas_update", true, tlas, scratch, nullptr, nullptr,
                       false);
}

VkAccelerationStructureInstanceKHR makeInstance(VkDeviceAddress blasAddress, const float* transform3x4, uint32_t customIndex, uint8_t mask, uint32_t sbtOffset) {
    VkAccelerationStructureInstanceKHR inst{};
    static constexpr float kIdentity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    std::memcpy(&inst.transform, transform3x4 ? transform3x4 : kIdentity, sizeof(inst.transform));
    // F5: a masked index would alias another object's id (0x1000000 -> terrain). Refuse it: mask 0 and a null reference, never traced.
    assert(instanceCustomIndexFits(customIndex) && instanceCustomIndexFits(sbtOffset) && "instanceCustomIndex / SBT offset are 24 bits");
    if (!instanceCustomIndexFits(customIndex) || !instanceCustomIndexFits(sbtOffset)) return inst;
    inst.instanceCustomIndex = customIndex;
    inst.mask = mask;
    inst.instanceShaderBindingTableRecordOffset = sbtOffset;
    inst.flags = static_cast<uint32_t>(VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR);
    inst.accelerationStructureReference = blasAddress;
    return inst;
}

OwnedQueryPool createCompactionQueryPool(const VulkanContext& ctx, uint32_t count) {
    OwnedQueryPool pool;
    VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
    qi.queryCount = std::max(count, 1u);
    const VkDevice device = ctx.device();
    mc2vk::create(pool, device, [&](VkQueryPool* h) { return vkCreateQueryPool(device, &qi, nullptr, h); });
    return pool;
}

void recordCompactionQuery(const VulkanContext&, VkCommandBuffer cmd, VkQueryPool pool, uint32_t index, const AccelStructure& blas) {
    const VkAccelerationStructureKHR handle = blas.get();
    vkCmdResetQueryPool(cmd, pool, index, 1);
    vkCmdWriteAccelerationStructuresPropertiesKHR(cmd, 1, &handle, VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, pool, index);
}

std::optional<VkDeviceSize> readCompactedSize(const VulkanContext& ctx, VkQueryPool pool, uint32_t index) {
    VkDeviceSize v = 0;
    const VkResult r = vkGetQueryPoolResults(ctx.device(), pool, index, 1, sizeof(v), &v, sizeof(v), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    if (r != VK_SUCCESS || v == 0) return std::nullopt;
    return v;
}

bool recordCompactCopy(const VulkanContext& ctx, VkCommandBuffer cmd, const AccelStructure& src, VkDeviceSize compactedSize, std::string_view name,
                       AccelStructure& dst) {
    // `dst` must be empty: a live structure is never overwritten in place (the caller publishes the copy and retires its old owner).
    if (!src || dst || compactedSize == 0) return false;
    // AMD proprietary (26.9.2, RX 7900 XTX): a compacted copy of a 1-triangle ALLOW_UPDATE BLAS traces as a miss (stage 6 matrix,
    // both PREFER_FAST_TRACE and PREFER_FAST_BUILD; the 512-triangle copies and every non-update copy hit; the 2070 hits all six).
    // Refuse the copy there so the caller keeps the uncompacted structure; one sample size cannot place a primitive floor.
    if (!compactionAllowed(src.buildFlagBits(), ctx.driverId() == VK_DRIVER_ID_AMD_PROPRIETARY)) return false;
    auto created = createAccelStructure(ctx, src.isTop() ? VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR : VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
                                        compactedSize, name);
    if (!created) return false;
    dst = std::move(*created);
    VkCopyAccelerationStructureInfoKHR ci{VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR};
    ci.src = src.get();
    ci.dst = dst.get();
    ci.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
    vkCmdCopyAccelerationStructureKHR(cmd, &ci);
    dst.flags_ = src.flags_;
    dst.updateScratch_ = src.updateScratch_;
    dst.primitiveCount_ = src.primitiveCount_;
    dst.signature_ = src.signature_;
    return true;
}

bool recordBarrier(const VulkanContext& ctx, VkCommandBuffer cmd, Phase from, Phase to) { return recordBarrier(ctx, cmd, barrierBetween(from, to)); }

bool recordBarrier(const VulkanContext& ctx, VkCommandBuffer cmd, const Barrier& b) {
    if (!b.valid) return false;
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = b.srcStage;
    mb.srcAccessMask = b.srcAccess;
    mb.dstStageMask = b.dstStage;
    mb.dstAccessMask = b.dstAccess;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    ctx.cmdPipelineBarrier2(cmd, dep);
    return true;
}

}  // namespace mc2vk::rt

