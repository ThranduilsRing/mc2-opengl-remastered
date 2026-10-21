// vulkan_shadow_bake.h -- the static sun shadow bake of the VK shadow island.
//
// WHAT: two products, both built once per map from CPU-complete inputs (the gameplay caster set, the terrain heights, the clamped sun, the water level):
//   - the visibility MASK: R8 with mips over the terrain grid in world XY, the soft static shadow (props by deterministic PCSS, terrain by a cone march,
//     long prop shadows faded by their blurred blocker distance) evaluated at the visible surface max(terrain, water). Receivers sample it bilinearly.
//   - the OBJECT map: a D16 sun-space depth map of the terrain and every static caster with the engine's static light matrix, for receivers that are not
//     the ground (the screen shadow resolve).
// The mask is baked in tiles of 512^2 texels; each tile rasterises the casters that can reach it into a transient light-space window and filters there.
// Each stage is timed on the GPU and logged as [SHADOW_BAKE]; the transients are released once the bake's submit has retired.
// WHY: trees rode the camera-fit cascades (blocky, sharp, stair-stepped long shadows); a bake pre-filters them once at a resolution the cascades never
// had, and the receivers pay one bilinear tap.
#ifndef MC2_VULKAN_SHADOW_BAKE_H
#define MC2_VULKAN_SHADOW_BAKE_H

#ifdef MC2_VULKAN_ISLAND

#include <volk.h>

#include "shadow_bake_math.h"
#include "vk_compute_pipeline.h"
#include "vk_owned.h"
#include "vulkan_shadow_island_live.h"   // Mc2ShadowPacket

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

struct GpuStaticPropInstance;

namespace mc2vk {

// Per packet of the shared static-prop geometry: the alpha-ON array layer its colour pass samples and the UV sub-region (layer < 0: opaque).
struct CasterPacketAlpha {
    int32_t layer = -1;
    float scaleX = 1.0f;
    float scaleY = 1.0f;
};

// What the island lends a bake: its copy of the static-prop geometry, the per-packet alpha table and the alpha-ON array's descriptor set.
struct ShadowBakeInputs {
    VkBuffer vbo = VK_NULL_HANDLE;
    VkBuffer ibo = VK_NULL_HANDLE;
    std::span<const Mc2ShadowPacket> packets;
    std::span<const uint32_t> typeFirstPacket;   // indexed by typeID
    std::span<const uint32_t> typePacketCount;
    std::span<const CasterPacketAlpha> packetAlpha;
    VkDescriptorSet alphaSet = VK_NULL_HANDLE;
    bool alphaReady = false;    // the alpha-ON array is bound and the table built
    bool alphaAbsent = false;   // the map has no alpha-ON group, so there is nothing to wait for
};

class ShadowBake {
public:
    ShadowBake() = default;
    ShadowBake(const ShadowBake&) = delete;
    ShadowBake& operator=(const ShadowBake&) = delete;

    // `alphaDsl` is the island's set-1 layout (the alpha-ON array). False, logged, when a shader or object cannot be made; the island then runs without.
    bool init(VkDevice device, VmaAllocator allocator, VkPhysicalDevice phys, VkDescriptorSetLayout alphaDsl, std::string_view spvDir);

    // Called once per frame after the island opened the frame's recording. Retires a submitted bake (timings, transients) and records a new one into `cb`
    // when the inputs are ready and differ from the last bake's. True when it recorded (the caller counts a pass and submits).
    bool update(VkCommandBuffer cb, uint64_t epoch, const ShadowBakeInputs& in);
    void markSubmitted(uint64_t epoch);

    // A frame must be opened for the bake even when no caster pass runs: a bake is due, or a submitted one waits to retire.
    bool wantsFrame() const;
    bool hasProducts() const { return static_cast<bool>(maskImage_) || static_cast<bool>(objectImage_) || static_cast<bool>(windowImage_); }

    // The products of the last submitted bake; null / 0 before the first.
    bool ready() const { return maskReady_; }
    VkImageView maskView() const { return maskReady_ ? maskView_.get() : VK_NULL_HANDLE; }
    VkImageView objectView() const { return objectReady_ ? objectView_.get() : VK_NULL_HANDLE; }
    uint64_t objectGeneration() const { return objectReady_ ? objectGeneration_ : 0; }
    const mc2shadow::MaskLayout& layout() const { return layout_; }

    // A map boundary (the caller has idled the device): every product and transient goes; the next map bakes afresh.
    void invalidate();

private:
    struct TileWork {
        mc2shadow::Window window;
        float casterTop = 0.0f;
        uint32_t firstRun = 0, runCount = 0;
    };
    struct InstRun { uint32_t typeID, firstInstance, count; };

    struct BakeKeys { uint64_t mask = 0, object = 0; };
    std::optional<BakeKeys> inputsReady(const ShadowBakeInputs& in);
    bool ensureProducts(int maskTexels);
    bool ensureTransients();
    bool computeTypeBounds(const ShadowBakeInputs& in);
    bool recordBake(VkCommandBuffer cb, const ShadowBakeInputs& in);
    bool recordObjectMap(VkCommandBuffer cb, const ShadowBakeInputs& in, uint32_t queryBase);
    void drawRuns(VkCommandBuffer cb, const ShadowBakeInputs& in, std::span<const InstRun> runs, bool& alphaBound, VkPipeline opaque, VkPipeline alpha);
    void retire(uint64_t epoch);
    void releaseTransients();

    VkDevice device_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = nullptr;
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout alphaDsl_ = VK_NULL_HANDLE;
    bool inited_ = false;
    bool timed_ = false;
    double timestampPeriodNs_ = 1.0;

    // The bake state: a recorded bake is pending until its submit retires (its descriptors and transients are not touched before that).
    uint64_t bakedKey_ = 0, objectKey_ = 0, recordedKey_ = 0, recordedObjectKey_ = 0;
    bool recorded_ = false, submitted_ = false, recordedMask_ = false;
    uint64_t submittedEpoch_ = 0, recordedEpoch_ = 0;
    bool maskReady_ = false, objectReady_ = false;
    uint64_t objectGeneration_ = 0, recordedObjectGeneration_ = 0;
    uint32_t waitFrames_ = 0;
    bool loggedWait_ = false, loggedHeightDrift_ = false;
    uint64_t bakedHeightVersion_ = 0;

    // This bake's inputs (CPU).
    mc2shadow::MaskLayout layout_;
    mc2shadow::LightBasis basis_;
    mc2shadow::Vec3 sun_;
    float waterZ_ = 0.0f, terrainTop_ = 0.0f, halfMap_ = 0.0f, fadeMax_ = 0.0f;
    int mapSide_ = 0, heightSide_ = 0;
    float heightSpacing_ = 0.0f;
    std::vector<float> heights_;
    std::vector<GpuStaticPropInstance> leaves_;
    struct TypeBounds { float mn[3], mx[3]; bool valid; };
    std::vector<TypeBounds> typeBounds_;
    std::vector<TileWork> tiles_;
    std::vector<InstRun> runs_;
    std::vector<InstRun> objectRuns_;
    uint32_t binnedInstances_ = 0;
    double cpuMs_ = 0.0;
    VkDeviceSize transientBytes_ = 0;

    // Declared in creation order (released in reverse).
    OwnedRenderPass windowPass_, objectPass_;
    OwnedDescriptorSetLayout casterDsl_;
    OwnedPipelineLayout casterLayout_;
    OwnedShaderModule propVert_, terrainVert_, opaqueFrag_, alphaFrag_;
    OwnedPipeline windowOpaque_, windowAlpha_, objectOpaque_, objectAlpha_, objectTerrain_;
    OwnedDescriptorPool casterPool_;
    VkDescriptorSet windowCasterSet_ = VK_NULL_HANDLE, objectCasterSet_ = VK_NULL_HANDLE;   // freed with casterPool_
    ComputePipeline pcss_, march_, resolve_;
    OwnedDescriptorPool computePool_;
    VkDescriptorSet computeSet_ = VK_NULL_HANDLE;   // allocated from computePool_ with pcss_'s layout (the three layouts are identical)
    OwnedSampler depthRawSampler_, depthCompareSampler_;
    OwnedQueryPool queries_;
    uint32_t queryCount_ = 0;

    // Products.
    OwnedImage maskImage_;
    OwnedImageView maskView_;
    int maskTexels_ = 0;
    uint32_t maskMips_ = 0;
    bool maskInitialized_ = false;
    OwnedImage objectImage_;
    OwnedImageView objectView_;
    OwnedFramebuffer objectFb_;
    bool objectInitialized_ = false;
    OwnedBuffer terrainVbo_, terrainIbo_;
    uint32_t terrainIndexCount_ = 0;
    OwnedBuffer objectInstances_, objectUbo_;

    // Transients (one bake).
    OwnedImage windowImage_;
    OwnedImageView windowView_;
    OwnedFramebuffer windowFb_;
    OwnedImage scratch_[4];   // props, terrain, blur, combined
    OwnedImageView scratchView_[4];
    OwnedBuffer staging_, tileParams_, passParams_, casterUbo_, instances_, heightBuf_;
};

}  // namespace mc2vk

#endif  // MC2_VULKAN_ISLAND
#endif  // MC2_VULKAN_SHADOW_BAKE_H
