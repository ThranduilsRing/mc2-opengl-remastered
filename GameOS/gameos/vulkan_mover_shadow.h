#pragma once
#include "shadow_mover_fit.h"
#include "vk_owned.h"
#include "vk_frame_ring.h"
#include "vk_desc_write_cache.h"
#include "vulkan_mech_island.h"   // Mc2MechDrawRun
#include "vulkan_shadow_island_live.h"
#include "../../RenderCore/gpu_abi/GpuStaticPropInstance.h"
#include <span>
#include <vector>

namespace mc2vk {
class VulkanContext;

// CPU union fit and receiver resources. The shadow island owns the one D16 image
// and lends its render pass to both prop and skinned caster pipelines.
class MoverShadowState {
public:
    bool init(VulkanContext& ctx, bool unionPath);
    void clearGeometry();
    bool cacheGeometry(const Mc2ShadowGeomFeed& feed, const void* vertices, const void* indices);
    bool prepare(int mapSide, const float cameraRows[16], mc2shadow::Vec3 sun,
                 std::span<const GpuStaticPropInstance> props, bool maskReady);
    const std::vector<GpuStaticPropInstance>& props() const { return props_; }
    const mc2shadow::MoverShadowFrame& frame() const { return frame_; }
    // stamp (optional, one per destination set): skips the binding-24 write when its sampler/view/layout did not change.
    // Binding 25 (a fresh frame-ring slice) is written every call.
    bool bind(VkDevice device, VkDescriptorSet set, VkImageView depth, bool ready, DescWriteStamp* stamp = nullptr) const;
    uint32_t actorCount() const { return actorCount_; }
    uint32_t visibleActorCount() const { return static_cast<uint32_t>(spheres_.size()-props_.size()); }

    // Per-frame mech actor spheres (MC2 frame) and the instance -> actor map. Called once per frame before the local-light passes; prepare() reuses it.
    void refreshMechActors(const float cameraRows[16], mc2shadow::Vec3 sun);
    // Per-run draw masks for ShadowMechCasters::record: 1 = some mech of the run can matter to that map. False (mask all ones) when the map is unknown.
    bool runMaskView(const std::vector<Mc2MechDrawRun>& runs, std::vector<uint8_t>& mask) const;
    bool runMaskLight(const std::vector<Mc2MechDrawRun>& runs, const float pos[3], float farDistance, std::vector<uint8_t>& mask) const;

private:
    struct MechActor { mc2shadow::MoverSphere sphere; bool inView; };
    void updateGround();
    std::vector<MechActor> actors_;
    std::vector<int32_t> instActor_;
    bool actorsFresh_ = false;
    struct Bounds { mc2shadow::Vec3 lo, hi; bool valid = false; };
    mc2shadow::MoverSphere propSphere(const GpuStaticPropInstance& instance, const Bounds& bounds) const;
    void appendMechs(const float cameraRows[16], mc2shadow::Vec3 sun, float ground);
    VkDevice device_ = VK_NULL_HANDLE;
    std::vector<Bounds> bounds_;
    std::vector<GpuStaticPropInstance> props_;
    std::vector<mc2shadow::MoverSphere> spheres_;
    std::vector<uint32_t> boneOffsets_;
    mc2shadow::MoverShadowFrame frame_{};
    uint64_t heightVersion_ = ~0ull;
    float ground_ = 0, extent_ = 0, mechRadius_ = 0;
    uint32_t mechGeneration_ = ~0u, actorCount_ = 0;
    // The engine frame ticket covers ALL receivers, unlike the producer's fence.
    FrameAlloc active_{};
    uint64_t activeFrame_=~0ull;
    OwnedBuffer inactive_;
    OwnedImage dummy_;
    OwnedImageView dummyView_;
    OwnedSampler sampler_;
};

// Fixed set-0 bindings 24 (raw depth) and 25 (frame UBO), shared by every
// receiver. Call after retiring the receiver's descriptor set for this frame.
bool bindMoverShadowDescriptors(VkDevice device, VkDescriptorSet set, DescWriteStamp* stamp24 = nullptr);
}
