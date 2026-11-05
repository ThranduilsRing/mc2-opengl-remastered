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
#include <span>
#include "../../GameAdapters/vegetation_budget.h"

namespace mc2vk { class VulkanContext; }

namespace mc2veg {

// LOCKSTEP with PackedVegInstance / VegRegion in vegetation_structs.glsl.
using GpuInstance = mc2::vegetation::PackedInstance;
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

struct PopulationStats {
    uint64_t uploads = 0;        // population uploads since process start (one per uploadInstances)
    uint64_t uploadBytes = 0;    // bytes those uploads moved to the device
    uint64_t instances = 0, regions = 0;
};

// The persistent population. Main thread only.
class State final : public mc2vk::IslandState {
public:
    static constexpr uint32_t kSlots = mc2vk::kMaxFramesInFlight;

    // Whether the engine context holds a state yet, and the state itself (created on first use; needs a valid engine context).
    static bool exists();
    static State& instance();
    // Releases it (renderer shutdown or a population cleared for good).
    static void release();

    // Replaces the population: device-local copies of the instances and regions, and fresh per-parity output buffers sized to it. Waits for the device
    // (load-time work, never per frame). False leaves no population.
    // Keep-every-Nth thinning stride (1 = whole population) that fits the quality preset and the live GPU headroom; 0 = no vegetation fits.
    static uint32_t strideFor(uint64_t instances, uint64_t regions);
    bool upload(mc2vk::VulkanContext& ctx, std::span<const GpuInstance> instances, std::span<const GpuRegion> regions);
    void clear(mc2vk::VulkanContext& ctx);

    bool ready() const { return instanceCount_ != 0 && regionCount_ != 0; }
    uint32_t instanceCount() const { return instanceCount_; }
    uint32_t regionCount() const { return regionCount_; }
    const PopulationStats& stats() const { return stats_; }

    // Records the three cull dispatches into `cb` (outside a render pass): writes `params` and the block-visibility field into the frame-ring `span`
    // (params at paramsOff, visibility at visOff), then cull -> scan -> compact for parity `par`, ending in a barrier to the vertex/indirect/transfer
    // readers. The parity's previous submit must have completed. False when the pipelines cannot be built.
    bool record(mc2vk::VulkanContext& ctx, VkCommandBuffer cb, uint32_t par, const mc2vk::FrameAlloc& span, VkDeviceSize paramsOff, VkDeviceSize visOff,
                CullParams params, std::span<const uint32_t> visibility);
    uint64_t frameUploadBytes() const { return frameUploadBytes_; }
    uint32_t lastSurvivors() const { return lastSurvivors_; }   // survivor count of the parity's previous frame, read back without a wait

    // The draw side. Set 2 of the vegetation graphics pipeline layout: 0 = instances, 1 = dense survivor list, 2 = the frame's cull parameters (dynamic).
    // The layout exists before any population does; the island builds its pipeline layout from it.
    bool ensureDrawLayout(mc2vk::VulkanContext& ctx);
    // Load-time creation of cull/scan/compact pipelines and population bindings.
    bool prewarm(mc2vk::VulkanContext& ctx) { return ensurePipelines(ctx); }
    VkDescriptorSetLayout drawLayout() const { return drawDsl_.get(); }
    VkDescriptorSet drawSet(uint32_t par) const { return par < kSlots ? drawSets_[par] : VK_NULL_HANDLE; }
    // Dynamic offset of the cull parameters inside the ring span passed to the last record() of this parity.
    uint32_t paramsDynamicOffset(uint32_t par) const { return paramsDynOffset_[par]; }
    // The two indirect strips (first vertex 0 and 8) of the parity's survivor arguments.
    void drawIndirect(VkCommandBuffer cb, uint32_t par) const;

    VkBuffer instances() const { return instances_.get(); }
    VkBuffer regions() const { return regions_.get(); }
    VkBuffer counts(uint32_t par) const { return slot_[par].counts.get(); }
    VkBuffer offsets(uint32_t par) const { return slot_[par].offsets.get(); }
    VkBuffer dense(uint32_t par) const { return slot_[par].dense.get(); }
    VkBuffer args(uint32_t par) const { return slot_[par].args.get(); }

private:
    struct Slot {
        mc2vk::OwnedBuffer counts, offsets, dense, args, stats, visList;
        bool statsPending = false;
    };
    bool ensurePipelines(mc2vk::VulkanContext& ctx);
    mc2vk::ComputePipeline region_, cull_, compact_;
    mc2vk::ComputeSets sets_;
    bool pipelinesTried_ = false, pipelinesOk_ = false;
    uint64_t generation_ = 1, boundGeneration_ = 0;
    VkBuffer ringBound_[kSlots] = {};
    uint64_t frameUploadBytes_ = 0;
    uint32_t lastSurvivors_ = 0;
    uint32_t statsReads_ = 0;          // readbacks so far: the visible-survivor smoke line prints every 64th
    bool capLogged_ = false;           // the visible-cap overflow line prints once per population
    uint32_t paramsDynOffset_[kSlots] = {};
    mc2vk::OwnedDescriptorSetLayout drawDsl_;
    mc2vk::OwnedDescriptorPool drawPool_;
    VkDescriptorSet drawSets_[kSlots] = {};
    VkBuffer drawRingBound_[kSlots] = {};
    bool drawTried_ = false;
    mc2vk::OwnedBuffer instances_, regions_;
    Slot slot_[kSlots];
    uint32_t instanceCount_ = 0, regionCount_ = 0;
    PopulationStats stats_;
};

}  // namespace mc2veg

#endif  // MC2_VULKAN_ISLAND
#endif  // MC2_VK_VEGETATION_STATE_H
