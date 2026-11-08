// vk_rt_acceleration.cpp -- see vk_rt_acceleration.h.


#include "vk_rt_acceleration.h"

#include "vulkan_context.h"

#include <algorithm>
#include <cstring>

namespace mc2vk::rt {

// The pure half carries the numeric values of the sync2 bits; they must be the real ones.
static_assert(kStageTransfer == VK_PIPELINE_STAGE_2_TRANSFER_BIT);
static_assert(kStageComputeShader == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
static_assert(kStageRayTracingShader == VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR);
static_assert(kStageAsBuild == VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR);
static_assert(kStageAsCopy == VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_COPY_BIT_KHR);
static_assert(kAccessTransferWrite == VK_ACCESS_2_TRANSFER_WRITE_BIT);
static_assert(kAccessAsRead == VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
static_assert(kAccessAsWrite == VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
static_assert(sizeof(VkAccelerationStructureInstanceKHR) == kInstanceBytes);

namespace {

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

bool makeScratch(const VulkanContext& ctx, VkDeviceSize needed, std::string_view name, OwnedBuffer& scratch, VkDeviceAddress& alignedAddress) {
    const uint64_t align = ctx.rtCaps().minAccelerationStructureScratchOffsetAlignment;
    if (!scratch.createDeviceLocal(ctx.allocator(), ctx.device(), scratchAllocBytes(needed, align),
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, name))
        return false;
    alignedAddress = scratchAlignedAddress(scratch.deviceAddress(), align);
    return scratchFits(scratch.deviceAddress(), scratchAllocBytes(needed, align), needed, align);
}

// Shared by BLAS and TLAS: query the sizes for `geo` x `primitives`, create structure + scratch, record the build (or the update when `update`).
bool recordBuild(const VulkanContext& ctx, VkCommandBuffer cmd, VkAccelerationStructureTypeKHR type, const VkAccelerationStructureGeometryKHR& geo,
                 uint32_t primitives, VkBuildAccelerationStructureFlagsKHR flags, std::string_view name, bool update, AccelStructure& as,
                 OwnedBuffer& scratch, VkDeviceSize* outUpdateScratch, VkDeviceSize* outSize, bool createNew) {
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
    if (createNew) {
        auto created = createAccelStructure(ctx, type, sizes.accelerationStructureSize, name);
        if (!created) return false;
        as = std::move(*created);
    }
    VkDeviceAddress scratchAddr = 0;
    if (!makeScratch(ctx, update ? sizes.updateScratchSize : sizes.buildScratchSize, name, scratch, scratchAddr)) return false;
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

std::optional<AccelStructure> createAccelStructure(const VulkanContext& ctx, VkAccelerationStructureTypeKHR type, VkDeviceSize size, std::string_view name) {
    if (!ctx.rt().accel) return std::nullopt;
    AccelStructure out;
    const VkDeviceSize bytes = asStorageBytes(size);
    if (!out.buffer_.createDeviceLocal(ctx.allocator(), ctx.device(), bytes,
                                       VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, name))
        return std::nullopt;
    VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    ci.buffer = out.buffer_.get();
    ci.offset = 0;
    ci.size = size;
    ci.type = type;
    const VkDevice device = ctx.device();
    if (!mc2vk::create(out.handle_, device, [&](VkAccelerationStructureKHR* h) { return vkCreateAccelerationStructureKHR(device, &ci, nullptr, h); }))
        return std::nullopt;
    VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    ai.accelerationStructure = out.handle_.get();
    out.address_ = vkGetAccelerationStructureDeviceAddressKHR(device, &ai);
    out.size_ = size;
    out.top_ = type == VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    return out;
}

bool recordBlasBuild(const VulkanContext& ctx, VkCommandBuffer cmd, const TriangleGeometry& g, const BuildFlags& flags, std::string_view name, AccelStructure& blas,
                     OwnedBuffer& scratch) {
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
    return true;
}

bool recordBlasUpdate(const VulkanContext& ctx, VkCommandBuffer cmd, const TriangleGeometry& g, AccelStructure& blas, OwnedBuffer& scratch) {
    if (!blas || blas.isTop() || !blas.canUpdate() || g.triangleCount != blas.primitiveCount_) return false;
    const VkAccelerationStructureGeometryKHR geo = trianglesOf(g);
    return recordBuild(ctx, cmd, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, geo, g.triangleCount, blas.flags_, "blas_update", true, blas, scratch, nullptr,
                       nullptr, false);
}

bool recordTlasBuild(const VulkanContext& ctx, VkCommandBuffer cmd, VkDeviceAddress instances, uint32_t instanceCount, const BuildFlags& flags, std::string_view name,
                     AccelStructure& tlas, OwnedBuffer& scratch) {
    if (!ctx.rt().accel || instanceCount == 0 || instances == 0 || instances % kInstanceAddressAlignment != 0) return false;
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

bool recordTlasUpdate(const VulkanContext& ctx, VkCommandBuffer cmd, VkDeviceAddress instances, uint32_t instanceCount, AccelStructure& tlas, OwnedBuffer& scratch) {
    if (!tlas || !tlas.isTop() || !tlas.canUpdate() || instanceCount != tlas.primitiveCount_ || instances == 0 || instances % kInstanceAddressAlignment != 0)
        return false;
    const VkAccelerationStructureGeometryKHR geo = instancesOf(instances);
    return recordBuild(ctx, cmd, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, geo, instanceCount, tlas.flags_, "tlas_update", true, tlas, scratch, nullptr, nullptr,
                       false);
}

VkAccelerationStructureInstanceKHR makeInstance(VkDeviceAddress blasAddress, const float* transform3x4, uint32_t customIndex, uint8_t mask) {
    VkAccelerationStructureInstanceKHR inst{};
    static constexpr float kIdentity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    std::memcpy(&inst.transform, transform3x4 ? transform3x4 : kIdentity, sizeof(inst.transform));
    inst.instanceCustomIndex = customIndex & 0xFFFFFFu;
    inst.mask = mask;
    inst.instanceShaderBindingTableRecordOffset = 0;
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
    // An ALLOW_UPDATE structure does not survive a COMPACT copy on AMD (every ray misses): compact only structures built without update.
    if (!src || compactedSize == 0 || src.canUpdate()) return false;
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
    return true;
}

bool recordBarrier(const VulkanContext& ctx, VkCommandBuffer cmd, Phase from, Phase to) {
    const Barrier b = barrierBetween(from, to);
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

