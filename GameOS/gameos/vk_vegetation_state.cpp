// vk_vegetation_state.cpp -- see vk_vegetation_state.h.
#ifdef MC2_VULKAN_ISLAND

#include "vk_vegetation_state.h"

#include "vk_gpu_profile.h"
#include "vk_island_util.h"
#include "vk_one_shot.h"
#include "vulkan_context.h"

#include "mc2fmt.h"
#include "RenderCore/GpuResidencyPolicy.h"
#include "Gates/render_options.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace mc2veg {
bool State::exists() {
    return mc2vk::engineVulkanContextValid() && mc2vk::getOrCreateEngineVulkanContext().islandSlot(mc2vk::IslandSlot::Vegetation) != nullptr;
}

State& State::instance() {
    auto& slot = mc2vk::getOrCreateEngineVulkanContext().islandSlot(mc2vk::IslandSlot::Vegetation);
    if (!slot) slot = std::make_unique<State>();
    return static_cast<State&>(*slot);
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
    for (VkBuffer& b : drawRingBound_) b = VK_NULL_HANDLE;
    lastSurvivors_ = 0;
}

uint32_t State::strideFor(uint64_t instances, uint64_t regions) {
    return RenderCore::vegetationStride(instances, regions, kSlots, RenderCore::gpuResidencyPolicy(mc2::options::current().shadowQuality).vegetation, mc2vk::infra::headroom());
}

bool State::upload(mc2vk::VulkanContext& ctx, std::span<const GpuInstance> instances, std::span<const GpuRegion> regions) {
    clear(ctx);
    if (instances.empty() || regions.empty()) return true;   // an empty population is a valid population
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(ctx.physicalDevice(), &properties);
    const VkDeviceSize maxRange = properties.limits.maxStorageBufferRange;
    const VkDeviceSize instBytes = instances.size_bytes(), regBytes = regions.size_bytes();
    const uint32_t visibleCapacity = uint32_t(std::min(instances.size(), size_t(mc2::vegetation::kVisibleCapacity)));
    const auto policy = RenderCore::gpuResidencyPolicy(mc2::options::current().shadowQuality);
    // Both parity sets, the regions and the cooked eight-layer atlas, against the preset ceiling; the load-time staging
    // overlap is added for the live-headroom admission.
    const uint64_t residentBytes = instBytes + regBytes + uint64_t(visibleCapacity) * 8 + uint64_t(regions.size()) * 16 + 3 * 1024 * 1024 + 160;
    const uint64_t bytes = residentBytes + instBytes + regBytes;
    if (residentBytes > policy.vegetation || !mc2vk::infra::admit(bytes) || properties.limits.maxComputeWorkGroupInvocations < 128 ||
        properties.limits.maxComputeWorkGroupSize[0] < 128 || properties.limits.maxComputeSharedMemorySize < 128 * sizeof(uint32_t)) {
        std::fprintf(stderr, "[VK_RESIDENCY v1] refused=vegetation bytes=%llu policy=%llu fallback=off reason=preset_budget_or_device_limits\n", bytes, policy.vegetation);
        return true;
    }
    if (instBytes > maxRange || regBytes > maxRange || residentBytes > mc2::vegetation::kResidentBudget) {
        mc2::fmt::print_err("[VEG_GPU] population upload failed: storage or residency budget exceeded\n");
        return false;
    }
    const uint32_t instanceCount = static_cast<uint32_t>(instances.size()), regionCount = static_cast<uint32_t>(regions.size());
    const VkBufferUsageFlags kStore = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    mc2vk::OwnedBuffer stageInst, stageReg;
    if (!stageInst.createFilled(ctx.allocator(), ctx.device(), instances.data(), instBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, "veg.stage.instances") ||
        !stageReg.createFilled(ctx.allocator(), ctx.device(), regions.data(), regBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, "veg.stage.regions") ||
        !instances_.createDeviceLocal(ctx.allocator(), ctx.device(), instBytes, kStore | VK_BUFFER_USAGE_TRANSFER_DST_BIT, "veg.instances") ||
        !regions_.createDeviceLocal(ctx.allocator(), ctx.device(), regBytes, kStore | VK_BUFFER_USAGE_TRANSFER_DST_BIT, "veg.regions")) {
        mc2::fmt::print_err("[VEG_GPU] population upload failed: buffer allocation (instances={:d} regions={:d})\n", mc2::fmt::as_u32(instanceCount), mc2::fmt::as_u32(regionCount));
        clear(ctx);
        return false;
    }
    for (uint32_t i = 0; i < kSlots; ++i) {
        Slot& s = slot_[i];
        if (!s.counts.createDeviceLocal(ctx.allocator(), ctx.device(), VkDeviceSize(regionCount) * 4, kStore, "veg.counts", int(i)) ||
            !s.offsets.createDeviceLocal(ctx.allocator(), ctx.device(), VkDeviceSize(regionCount) * 4, kStore, "veg.offsets", int(i)) ||
            !s.dense.createDeviceLocal(ctx.allocator(), ctx.device(), VkDeviceSize(visibleCapacity) * 4, kStore, "veg.dense", int(i)) ||
            !s.stats.createHostWritable(ctx.allocator(), ctx.device(), 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, "veg.stats", int(i)) ||
            !s.args.createDeviceLocal(ctx.allocator(), ctx.device(), 64, kStore | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, "veg.args", int(i))) {
            mc2::fmt::print_err("[VEG_GPU] population upload failed: output buffer allocation\n");
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
        mc2::fmt::print_err("[VEG_GPU] population upload failed: copy submit\n");
        clear(ctx);
        return false;
    }
    instanceCount_ = instanceCount;
    regionCount_ = regionCount;
    ++generation_;
    const VkDeviceSize outBytes = VkDeviceSize(visibleCapacity) * 4 * kSlots + VkDeviceSize(regionCount) * 8 * kSlots;
    ++stats_.uploads;
    stats_.uploadBytes += instBytes + regBytes;
    stats_.instances = instanceCount;
    stats_.regions = regionCount;
    mc2::fmt::print_err("[VEG_GPU] population upload #{:d}: instances={:d} regions={:d} avg_per_region={:d} uploaded_bytes={:d} resident_output_bytes={:d}\n",
        mc2::fmt::as_u64(stats_.uploads), mc2::fmt::as_u32(instanceCount), mc2::fmt::as_u32(regionCount), mc2::fmt::as_u32(instanceCount / regionCount),
        mc2::fmt::as_u64(instBytes + regBytes), mc2::fmt::as_u64(outBytes));
    return true;
}

bool State::ensurePipelines(mc2vk::VulkanContext& ctx) {
    ensureDrawLayout(ctx);
    if (!pipelinesTried_) {
        pipelinesTried_ = true;
        const auto S = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, SD = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, UD = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        const std::vector<mc2vk::ComputeBinding> bindings{{0, UD}, {1, SD}, {2, S}, {3, S}, {5, S}, {6, S}, {7, S}, {8, S}};
        const std::string dir = vk_island_util::spvDir();
        pipelinesOk_ = cull_.create(ctx.device(), dir + "vegetation_cull.comp.spv", bindings, "veg.cull") &&
                       scan_.create(ctx.device(), dir + "vegetation_scan.comp.spv", bindings, "veg.scan") &&
                       compact_.create(ctx.device(), dir + "vegetation_compact.comp.spv", bindings, "veg.compact") &&
                       sets_.create(ctx.device(), cull_, kSlots, "veg.sets");
        if (!pipelinesOk_) mc2::fmt::print_err("[VEG_GPU] compute pipelines unavailable: GPU vegetation declined\n");
        boundGeneration_ = 0;
    }
    if (!pipelinesOk_ || !ready()) return false;
    if (boundGeneration_ != generation_) {
        // The population was replaced behind a device idle (upload); no submit names these sets now.
        const auto S = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        for (uint32_t i = 0; i < kSlots; ++i) {
            sets_.write(ctx.device(), i, 2, S, instances_.get());
            sets_.write(ctx.device(), i, 3, S, regions_.get());
            sets_.write(ctx.device(), i, 5, S, slot_[i].counts.get());
            sets_.write(ctx.device(), i, 6, S, slot_[i].offsets.get());
            sets_.write(ctx.device(), i, 7, S, slot_[i].dense.get());
            sets_.write(ctx.device(), i, 8, S, slot_[i].args.get());
            if (drawSets_[i]) {
                const VkDescriptorBufferInfo infos[2] = {{instances_.get(), 0, VK_WHOLE_SIZE}, {slot_[i].dense.get(), 0, VK_WHOLE_SIZE}};
                VkWriteDescriptorSet w[2]{};
                for (uint32_t b = 0; b < 2; ++b) {
                    w[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    w[b].dstSet = drawSets_[i]; w[b].dstBinding = b; w[b].descriptorCount = 1;
                    w[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[b].pBufferInfo = &infos[b];
                }
                vkUpdateDescriptorSets(ctx.device(), 2, w, 0, nullptr);
            }
        }
        boundGeneration_ = generation_;
    }
    return true;
}

bool State::record(mc2vk::VulkanContext& ctx, VkCommandBuffer cb, uint32_t par, const mc2vk::FrameAlloc& span, VkDeviceSize paramsOff, VkDeviceSize visOff,
                   CullParams params, std::span<const uint32_t> visibility) {
    if (par >= kSlots || !span || !ensurePipelines(ctx)) return false;
    const uint32_t gridX = std::min<uint32_t>(regionCount_, 32768u), gridY = (regionCount_ + gridX - 1) / gridX;
    params.counts[0] = regionCount_;
    params.counts[1] = instanceCount_;
    params.counts[2] = static_cast<uint32_t>(visibility.size());
    params.flags[1] = gridX;
    params.flags[3] = std::min(instanceCount_, mc2::vegetation::kVisibleCapacity);
    std::memcpy(static_cast<uint8_t*>(span.ptr) + paramsOff, &params, sizeof(params));
    if (!visibility.empty()) std::memcpy(static_cast<uint8_t*>(span.ptr) + visOff, visibility.data(), visibility.size_bytes());
    frameUploadBytes_ += sizeof(params) + visibility.size_bytes();
    ensureDrawLayout(ctx);
    if (slot_[par].statsPending) {   // this parity's previous submit retired before the caller got here
        slot_[par].stats.read(&lastSurvivors_, sizeof(lastSurvivors_));
        slot_[par].statsPending = false;
    }
    paramsDynOffset_[par] = static_cast<uint32_t>(span.offset + paramsOff);
    if (drawSets_[par] && drawRingBound_[par] != span.buffer) {
        mc2vk::writeDynamicBinding(ctx.device(), drawSets_[par], 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, span.buffer, sizeof(CullParams));
        drawRingBound_[par] = span.buffer;
    }
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
    // The survivor total, read back by the next frame of this parity (a diagnostic; nothing waits for it).
    const VkBufferCopy total{32, 0, 4};
    vkCmdCopyBuffer(cb, slot_[par].args.get(), slot_[par].stats.get(), 1, &total);
    mc2vk::cmdBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    slot_[par].statsPending = true;
    return true;
}

bool State::ensureDrawLayout(mc2vk::VulkanContext& ctx) {
    if (drawTried_) return drawDsl_ && drawSets_[0];
    drawTried_ = true;
    VkDescriptorSetLayoutBinding b[3]{};
    for (uint32_t i = 0; i < 3; ++i) {
        b[i].binding = i; b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        b[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    }
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = 3; lci.pBindings = b;
    const VkDevice dev = ctx.device();
    if (!mc2vk::create(drawDsl_, dev, [&](VkDescriptorSetLayout* o) { return vkCreateDescriptorSetLayout(dev, &lci, nullptr, o); })) return false;
    const VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * kSlots}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, kSlots}};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = kSlots; pci.poolSizeCount = 2; pci.pPoolSizes = sizes;
    if (!mc2vk::create(drawPool_, dev, [&](VkDescriptorPool* o) { return vkCreateDescriptorPool(dev, &pci, nullptr, o); })) return false;
    const VkDescriptorSetLayout layouts[kSlots] = {drawDsl_.get(), drawDsl_.get()};
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = drawPool_.get(); ai.descriptorSetCount = kSlots; ai.pSetLayouts = layouts;
    if (vkAllocateDescriptorSets(dev, &ai, drawSets_) != VK_SUCCESS) { drawSets_[0] = drawSets_[1] = VK_NULL_HANDLE; return false; }
    boundGeneration_ = 0;   // the static bindings of the new sets are written on the next ensurePipelines
    return true;
}

void State::drawIndirect(VkCommandBuffer cb, uint32_t par) const {
    vkCmdDrawIndirect(cb, slot_[par].args.get(), 0, 1, 16);
    vkCmdDrawIndirect(cb, slot_[par].args.get(), 16, 1, 16);
}

}  // namespace mc2veg

#endif  // MC2_VULKAN_ISLAND
