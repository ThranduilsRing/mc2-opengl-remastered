// vk_vegetation_state.h -- persistent GPU storage for the vegetation population.
//
// WHAT: the immutable instance list and its region table live in device-local buffers, uploaded once per population (GosVegetation::uploadInstances,
// i.e. per mission load), not per frame. The per-frame survivor buffers (one set per frame parity) sit beside them. The state is a context island slot, so
// it survives a resolution change (the VFX island is rebuilt then) and is released ahead of the device.
// WHY: the CPU walked every instance every frame to classify it and then copied 64 B per survivor into the frame ring. See
// docs/superpowers/strategy and the 2026-10-20 VK optimization frontier design, packets V1-V4.
#ifndef MC2_VK_VEGETATION_STATE_H
#define MC2_VK_VEGETATION_STATE_H

#ifdef MC2_VULKAN_ISLAND

#include <volk.h>

#include "vk_compute_pipeline.h"
#include "vk_frame_ring.h"
#include "vk_owned.h"
#include "vulkan_context.h"   // mc2vk::IslandState

#include <cstdint>
#include <vector>

namespace mc2vk { class VulkanContext; }

namespace mc2veg {

// LOCKSTEP with VegInstance / VegRegion in shaders/vulkan/include/vegetation_cull_common.glsl (std430, 32 B each).
struct GpuInstance {
    float x, y, z, yaw;
    float scale;
    uint32_t atlasFrame;
    float seed;
    uint32_t pad;
};
static_assert(sizeof(GpuInstance) == 32);
struct GpuRegion {
    float x, y, z, radius;
    uint32_t first, last, pad0, pad1;   // [first, last) in the population order
};
static_assert(sizeof(GpuRegion) == 32);

// LOCKSTEP with VegParams in vegetation_cull_common.glsl (std140, 176 B).
struct CullParams {
    float plane[4][4];
    float planeLen[4];
    float clipRow[4];
    float cam[4];
    float misc[4];
    float grid[4];
    uint32_t counts[4];
    uint32_t flags[4];
};
static_assert(sizeof(CullParams) == 176);

// CPU-vs-GPU survivor comparison result (diagnostic scaffolding of packet V2).
struct CompareResult {
    uint32_t cpu = 0, gpu = 0, onlyCpu = 0, onlyGpu = 0, firstOrderMismatch = ~0u;
    bool ok = false;
};

struct PopulationStats {
    uint64_t uploads = 0;        // population uploads since process start (one per uploadInstances)
    uint64_t uploadBytes = 0;    // bytes those uploads moved to the device
    uint64_t instances = 0, regions = 0;
};

// The persistent population. Main thread only.
class State final : public mc2vk::IslandState {
public:
    static constexpr uint32_t kSlots = 2;   // frame parity, as the VFX island

    // The state of the engine context; nullptr when none exists and `create` is false.
    static State* get(bool create);
    // Releases it (renderer shutdown or a population cleared for good).
    static void release();

    // Replaces the population: device-local copies of the instances and regions, and fresh per-parity output buffers sized to it. Waits for the device
    // (load-time work, never per frame). False leaves no population.
    bool upload(mc2vk::VulkanContext& ctx, const GpuInstance* instances, uint32_t instanceCount, const GpuRegion* regions, uint32_t regionCount);
    void clear(mc2vk::VulkanContext& ctx);

    bool ready() const { return instanceCount_ != 0 && regionCount_ != 0; }
    uint32_t instanceCount() const { return instanceCount_; }
    uint32_t regionCount() const { return regionCount_; }
    const PopulationStats& stats() const { return stats_; }

    // Records the three cull dispatches into `cb` (outside a render pass): writes `params` and the block-visibility field into the frame-ring `span`
    // (params at paramsOff, visibility at visOff), then cull -> scan -> compact for parity `par`, ending in a barrier to the vertex/indirect/transfer
    // readers. The parity's previous submit must have completed. False when the pipelines cannot be built.
    bool record(mc2vk::VulkanContext& ctx, VkCommandBuffer cb, uint32_t par, const mc2vk::FrameAlloc& span, VkDeviceSize paramsOff, VkDeviceSize visOff,
                CullParams params, const uint32_t* visibility, uint32_t visibilityCount);
    // Synchronous GPU run of one frame's cull and a comparison with the CPU survivor ids (population indices, CPU walk order). Diagnostic only.
    bool compare(mc2vk::VulkanContext& ctx, const CullParams& params, const uint32_t* visibility, uint32_t visibilityCount, const std::vector<uint32_t>& cpuIds,
                 CompareResult& out);
    uint64_t frameUploadBytes() const { return frameUploadBytes_; }

    VkBuffer instances() const { return instances_.get(); }
    VkBuffer regions() const { return regions_.get(); }
    VkBuffer scratch(uint32_t par) const { return slot_[par].scratch.get(); }
    VkBuffer counts(uint32_t par) const { return slot_[par].counts.get(); }
    VkBuffer offsets(uint32_t par) const { return slot_[par].offsets.get(); }
    VkBuffer dense(uint32_t par) const { return slot_[par].dense.get(); }
    VkBuffer args(uint32_t par) const { return slot_[par].args.get(); }

private:
    struct Slot {
        mc2vk::OwnedBuffer scratch, counts, offsets, dense, args;
    };
    bool ensurePipelines(mc2vk::VulkanContext& ctx);
    mc2vk::ComputePipeline cull_, scan_, compact_;
    mc2vk::ComputeSets sets_;
    bool pipelinesTried_ = false, pipelinesOk_ = false;
    uint64_t generation_ = 1, boundGeneration_ = 0;
    VkBuffer ringBound_[kSlots] = {};
    uint64_t frameUploadBytes_ = 0;
    mc2vk::OwnedBuffer instances_, regions_;
    Slot slot_[kSlots];
    uint32_t instanceCount_ = 0, regionCount_ = 0;
    PopulationStats stats_;
};

}  // namespace mc2veg

#endif  // MC2_VULKAN_ISLAND
#endif  // MC2_VK_VEGETATION_STATE_H
