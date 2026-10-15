// vulkan_shadow_mech_casters.h -- the mech casters of the VK shadow cascades.
//
// WHAT: draws the skinned mechs into whichever depth map the shadow island is rendering (the dynamic map, a cascade layer, the full-map cascade), in the same
// render pass as the static-prop casters, so a receiver that samples the cascades sees mech shadows as GL's mech flushShadow gives them.
// WHY: the island's own pipeline reads one packed position stream and a model matrix per instance; a mech is a bone-blended mesh with its own vertex layout,
// bone palette and instance record, so it needs its own pipeline. Nothing here owns the render pass or the UBO: the island lends them, and the casters
// follow the island's lifetime (a member released before the device).
//
// The geometry is copied once per rebuild of the batcher's bytes; the instances, bones and run list are copied once per frame (the first pass of the frame
// stages them, the later passes of the same frame reuse them), because every pass of one frame draws the same mechs. The per-frame buffers exist twice, one
// set per frame parity: the island's frame fence retires the set's previous use before the first pass of a frame stages into it, so no pass ever waits for the
// GPU.
#ifndef MC2_VULKAN_SHADOW_MECH_CASTERS_H
#define MC2_VULKAN_SHADOW_MECH_CASTERS_H

#ifdef MC2_VULKAN_ISLAND

#include <volk.h>

#include "vk_owned.h"
#include "vulkan_mech_island.h"   // Mc2MechDrawRun

#include <cstdint>
#include <string_view>
#include <vector>

struct VmaAllocator_T;
typedef struct VmaAllocator_T* VmaAllocator;

namespace mc2vk {

class ShadowMechCasters {
public:
    // Builds the pipeline for `pass` (a depth-only pass; the lent render pass must outlive this object's use) and a descriptor set per frame parity over
    // `frameUbo`, the island's ShadowFrameUBO (the light-space matrix; bound with a dynamic offset, `uboRange` bytes per slice). False, with the reason
    // logged, when a shader module or an object cannot be made.
    bool init(VkDevice device, VmaAllocator allocator, VkRenderPass pass, VkBuffer frameUbo, VkDeviceSize uboRange, std::string_view spvDir);

    // Pulls this frame's mech feed into the buffers of parity `epoch & 1`. The first call of an epoch does the work; later calls of the same epoch return at once.
    // The caller has retired the previous use of that parity (the island's frame fence). A feed that is not ready, or whose run list does not fit its own
    // buffers, leaves nothing to draw this epoch.
    void stage(uint64_t epoch);

    bool hasCasters() const { return !runs_.empty() && stagedEpoch_ != kNoEpoch; }

    // Binds the pipeline and draws every run into the open render pass of `cb`; `uboOffset` is this pass's slice of the frame UBO. The caller has set the
    // viewport, the scissor and the depth bias.
    void record(VkCommandBuffer cb, uint32_t parity, uint32_t uboOffset) const;

    // Mech instances drawn by the last staged epoch (for the health line).
    uint32_t stagedInstances() const { return stagedInstances_; }

private:
    static constexpr uint64_t kNoEpoch = ~0ull;

    bool ensureBuffer(OwnedBuffer& buffer, VkDeviceSize& capacity, VkDeviceSize bytes, VkBufferUsageFlags usage, std::string_view name);
    void writeDescriptors(uint32_t parity);

    VkDevice device_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = nullptr;
    VkBuffer frameUbo_ = VK_NULL_HANDLE;
    VkDeviceSize uboRange_ = 0;
    uint32_t vertexStride_ = 0;
    bool inited_ = false;

    uint64_t stagedEpoch_ = kNoEpoch;
    uint32_t geomGen_ = 0;
    bool geomUploaded_ = false;
    uint32_t stagedInstances_ = 0;
    uint64_t iboIndexCount_ = 0;
    VkDeviceSize vboCap_ = 0, iboCap_ = 0, instCap_[2] = {0, 0}, boneCap_[2] = {0, 0};
    std::vector<Mc2MechDrawRun> runs_;   // the runs that passed validation this epoch

    // Declared in creation order: released in reverse, so the pipeline goes before its layout and shaders, the sets with their pool before the layout.
    OwnedBuffer vbo_, ibo_, instBuf_[2], boneBuf_[2];
    OwnedDescriptorSetLayout dsl_;
    OwnedDescriptorPool pool_;
    VkDescriptorSet set_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};   // allocated from pool_, freed with it
    OwnedPipelineLayout layout_;
    OwnedShaderModule vert_, frag_;
    OwnedPipeline pipeline_;
};

}  // namespace mc2vk

#endif  // MC2_VULKAN_ISLAND
#endif  // MC2_VULKAN_SHADOW_MECH_CASTERS_H
