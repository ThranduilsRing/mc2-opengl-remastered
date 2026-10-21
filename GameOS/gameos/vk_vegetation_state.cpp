// vk_vegetation_state.cpp -- see vk_vegetation_state.h.
#ifdef MC2_VULKAN_ISLAND

#include "vk_vegetation_state.h"

#include "vk_gpu_profile.h"
#include "vk_island_util.h"
#include "vk_one_shot.h"
#include "vulkan_context.h"

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include <vk_mem_alloc.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>

namespace mc2veg {
namespace {

void vlog(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vk_island_util::vlogv("[VEG_GPU] ", fmt, ap);
    va_end(ap);
}

bool createDevice(mc2vk::VulkanContext& ctx, mc2vk::OwnedBuffer& out, VkDeviceSize bytes, VkBufferUsageFlags usage, const char* name, int slot = -1) {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = std::max<VkDeviceSize>(bytes, 16);
    info.usage = usage;
    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    return out.create(ctx.allocator(), ctx.device(), info, alloc, name, slot);
}

}  // namespace

State* State::get(bool create) {
    if (!mc2vk::engineVulkanContextValid()) return nullptr;
    auto& slot = mc2vk::getOrCreateEngineVulkanContext().islandSlot(mc2vk::IslandSlot::Vegetation);
    if (!slot && create) slot = std::make_unique<State>();
    return static_cast<State*>(slot.get());
}

void State::release() {
    if (mc2vk::engineVulkanContextValid()) mc2vk::getOrCreateEngineVulkanContext().retireIsland(mc2vk::IslandSlot::Vegetation);
}

void State::clear(mc2vk::VulkanContext& ctx) {
    ctx.waitIdle("veg_clear");
    instances_.reset();
    regions_.reset();
    for (Slot& s : slot_) s = Slot{};
    instanceCount_ = regionCount_ = 0;
    for (VkBuffer& b : ringBound_) b = VK_NULL_HANDLE;
}

bool State::upload(mc2vk::VulkanContext& ctx, const GpuInstance* instances, uint32_t instanceCount, const GpuRegion* regions, uint32_t regionCount) {
    clear(ctx);
    if (!instances || !regions || instanceCount == 0 || regionCount == 0) return true;   // an empty population is a valid population
    const VkDeviceSize instBytes = VkDeviceSize(instanceCount) * sizeof(GpuInstance), regBytes = VkDeviceSize(regionCount) * sizeof(GpuRegion);
    const VkBufferUsageFlags kStore = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    mc2vk::OwnedBuffer stageInst, stageReg;
    if (!stageInst.createFilled(ctx.allocator(), ctx.device(), instances, instBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, "veg.stage.instances") ||
        !stageReg.createFilled(ctx.allocator(), ctx.device(), regions, regBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, "veg.stage.regions") ||
        !createDevice(ctx, instances_, instBytes, kStore | VK_BUFFER_USAGE_TRANSFER_DST_BIT, "veg.instances") ||
        !createDevice(ctx, regions_, regBytes, kStore | VK_BUFFER_USAGE_TRANSFER_DST_BIT, "veg.regions")) {
        vlog("population upload failed: buffer allocation (instances=%u regions=%u).", instanceCount, regionCount);
        clear(ctx);
        return false;
    }
    for (uint32_t i = 0; i < kSlots; ++i) {
        Slot& s = slot_[i];
        if (!createDevice(ctx, s.scratch, VkDeviceSize(instanceCount) * 4, kStore, "veg.scratch", int(i)) ||
            !createDevice(ctx, s.counts, VkDeviceSize(regionCount) * 4, kStore, "veg.counts", int(i)) ||
            !createDevice(ctx, s.offsets, VkDeviceSize(regionCount) * 4, kStore, "veg.offsets", int(i)) ||
            !createDevice(ctx, s.dense, VkDeviceSize(instanceCount) * 4, kStore, "veg.dense", int(i)) ||
            !createDevice(ctx, s.args, 64, kStore | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, "veg.args", int(i))) {
            vlog("population upload failed: output buffer allocation.");
            clear(ctx);
            return false;
        }
    }
    const bool ok = mc2vk::submitOneShot(ctx, [&](VkCommandBuffer cb) {
        const VkBufferCopy ci{0, 0, instBytes}, cr{0, 0, regBytes};
        vkCmdCopyBuffer(cb, stageInst.get(), instances_.get(), 1, &ci);
        vkCmdCopyBuffer(cb, stageReg.get(), regions_.get(), 1, &cr);
        // Empty draw arguments until the first cull: an indirect draw with instanceCount 0.
        for (Slot& s : slot_) vkCmdFillBuffer(cb, s.args.get(), 0, 64, 0);
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    }, "veg_population_upload");
    if (!ok) {
        vlog("population upload failed: copy submit.");
        clear(ctx);
        return false;
    }
    instanceCount_ = instanceCount;
    regionCount_ = regionCount;
    ++generation_;
    const VkDeviceSize outBytes = VkDeviceSize(instanceCount) * 8 * kSlots + VkDeviceSize(regionCount) * 8 * kSlots;
    ++stats_.uploads;
    stats_.uploadBytes += instBytes + regBytes;
    stats_.instances = instanceCount;
    stats_.regions = regionCount;
    vlog("population upload #%llu: instances=%u regions=%u avg_per_region=%.1f uploaded_bytes=%llu resident_output_bytes=%llu",
         static_cast<unsigned long long>(stats_.uploads), instanceCount, regionCount, double(instanceCount) / double(regionCount),
         static_cast<unsigned long long>(instBytes + regBytes), static_cast<unsigned long long>(outBytes));
    return true;
}

bool State::ensurePipelines(mc2vk::VulkanContext& ctx) {
    if (!pipelinesTried_) {
        pipelinesTried_ = true;
        const auto S = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, SD = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, UD = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        const std::vector<mc2vk::ComputeBinding> bindings{{0, UD}, {1, SD}, {2, S}, {3, S}, {4, S}, {5, S}, {6, S}, {7, S}, {8, S}};
        const std::string dir = vk_island_util::spvDir();
        pipelinesOk_ = cull_.create(ctx.device(), dir + "vegetation_cull.comp.spv", bindings, "veg.cull") &&
                       scan_.create(ctx.device(), dir + "vegetation_scan.comp.spv", bindings, "veg.scan") &&
                       compact_.create(ctx.device(), dir + "vegetation_compact.comp.spv", bindings, "veg.compact") &&
                       sets_.create(ctx.device(), cull_, kSlots, "veg.sets");
        if (!pipelinesOk_) vlog("compute pipelines unavailable: GPU vegetation declined.");
        boundGeneration_ = 0;
    }
    if (!pipelinesOk_ || !ready()) return false;
    if (boundGeneration_ != generation_) {
        // The population was replaced behind a device idle (upload); no submit names these sets now.
        const auto S = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        for (uint32_t i = 0; i < kSlots; ++i) {
            sets_.write(ctx.device(), i, 2, S, instances_.get());
            sets_.write(ctx.device(), i, 3, S, regions_.get());
            sets_.write(ctx.device(), i, 4, S, slot_[i].scratch.get());
            sets_.write(ctx.device(), i, 5, S, slot_[i].counts.get());
            sets_.write(ctx.device(), i, 6, S, slot_[i].offsets.get());
            sets_.write(ctx.device(), i, 7, S, slot_[i].dense.get());
            sets_.write(ctx.device(), i, 8, S, slot_[i].args.get());
        }
        boundGeneration_ = generation_;
    }
    return true;
}

bool State::record(mc2vk::VulkanContext& ctx, VkCommandBuffer cb, uint32_t par, const mc2vk::FrameAlloc& span, VkDeviceSize paramsOff, VkDeviceSize visOff,
                   CullParams params, const uint32_t* visibility, uint32_t visibilityCount) {
    if (par >= kSlots || !span || !ensurePipelines(ctx)) return false;
    const uint32_t gridX = std::min<uint32_t>(regionCount_, 32768u), gridY = (regionCount_ + gridX - 1) / gridX;
    params.counts[0] = regionCount_;
    params.counts[1] = instanceCount_;
    params.counts[2] = visibilityCount;
    params.flags[1] = gridX;
    std::memcpy(static_cast<uint8_t*>(span.ptr) + paramsOff, &params, sizeof(params));
    if (visibilityCount) std::memcpy(static_cast<uint8_t*>(span.ptr) + visOff, visibility, size_t(visibilityCount) * 4u);
    frameUploadBytes_ += sizeof(params) + size_t(visibilityCount) * 4u;
    const VkDescriptorSet set = sets_.set(par);
    if (ringBound_[par] != span.buffer) {
        mc2vk::writeDynamicBinding(ctx.device(), set, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, span.buffer, sizeof(CullParams));
        mc2vk::writeDynamicBinding(ctx.device(), set, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, span.buffer, span.range);
        ringBound_[par] = span.buffer;
    }
    const uint32_t dyn[2] = {static_cast<uint32_t>(span.offset + paramsOff), static_cast<uint32_t>(span.offset + visOff)};
    MC2_VK_GPU_SCOPE(gpuScope, cb, "VKIsl.VegCull");
    const VkPipelineStageFlags cs = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    cull_.bind(cb, set, dyn);
    vkCmdDispatch(cb, gridX, gridY, 1);
    mc2vk::cmdBarrier(cb, cs, VK_ACCESS_SHADER_WRITE_BIT, cs, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    scan_.bind(cb, set, dyn);
    vkCmdDispatch(cb, 1, 1, 1);
    mc2vk::cmdBarrier(cb, cs, VK_ACCESS_SHADER_WRITE_BIT, cs, VK_ACCESS_SHADER_READ_BIT);
    compact_.bind(cb, set, dyn);
    vkCmdDispatch(cb, gridX, gridY, 1);
    mc2vk::cmdBarrier(cb, cs, VK_ACCESS_SHADER_WRITE_BIT,
                      VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | cs,
                      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    return true;
}

bool State::compare(mc2vk::VulkanContext& ctx, const CullParams& params, const uint32_t* visibility, uint32_t visibilityCount, const std::vector<uint32_t>& cpuIds,
                    CompareResult& out) {
    out = CompareResult{};
    if (!ready()) return false;
    const VkDeviceSize visOff = mc2vk::alignUp(sizeof(CullParams), ctx.frameAlignment());
    const mc2vk::FrameAlloc span = ctx.frameAlloc(visOff + std::max<VkDeviceSize>(VkDeviceSize(visibilityCount) * 4u, 16));
    if (!span) return false;
    const VkDeviceSize denseBytes = VkDeviceSize(instanceCount_) * 4u, readBytes = 64 + denseBytes;
    mc2vk::OwnedBuffer host;
    if (!host.createHostWritable(ctx.allocator(), ctx.device(), readBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, "veg.compare.readback")) return false;
    bool recorded = false;
    const bool submitted = mc2vk::submitOneShot(ctx, [&](VkCommandBuffer cb) {
        recorded = record(ctx, cb, 0, span, 0, visOff, params, visibility, visibilityCount);
        if (!recorded) return;
        const VkBufferCopy a{0, 0, 64}, d{0, 64, denseBytes};
        vkCmdCopyBuffer(cb, slot_[0].args.get(), host.get(), 1, &a);
        vkCmdCopyBuffer(cb, slot_[0].dense.get(), host.get(), 1, &d);
        mc2vk::cmdBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    }, "veg_compare");
    if (!submitted || !recorded) return false;
    const auto* mapped = static_cast<const uint32_t*>(host.map());
    if (!mapped) return false;
    vmaInvalidateAllocation(ctx.allocator(), host.allocation(), 0, VK_WHOLE_SIZE);
    const uint32_t gpuCount = std::min(mapped[8], instanceCount_);
    const uint32_t* gpu = mapped + 16;
    out.cpu = uint32_t(cpuIds.size());
    out.gpu = gpuCount;
    // Both lists are in population order, so one merge classifies every id and finds the first order break.
    size_t a = 0, b = 0;
    while (a < cpuIds.size() && b < gpuCount) {
        if (cpuIds[a] == gpu[b]) { ++a; ++b; }
        else if (cpuIds[a] < gpu[b]) { ++out.onlyCpu; ++a; if (out.firstOrderMismatch == ~0u) out.firstOrderMismatch = uint32_t(a - 1); }
        else { ++out.onlyGpu; ++b; if (out.firstOrderMismatch == ~0u) out.firstOrderMismatch = uint32_t(a); }
    }
    out.onlyCpu += uint32_t(cpuIds.size() - a);
    out.onlyGpu += uint32_t(gpuCount - b);
    host.unmap();
    out.ok = true;
    return true;
}

}  // namespace mc2veg

#endif  // MC2_VULKAN_ISLAND
