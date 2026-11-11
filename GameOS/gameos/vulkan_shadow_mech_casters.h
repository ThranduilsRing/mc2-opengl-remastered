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


#include <volk.h>

#include "vk_owned.h"
#include "vk_frame_ring.h"
#include "vulkan_mech_island.h"   // Mc2MechDrawRun
#include "vulkan_mech_skin.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace mc2vk {

class ShadowMechCasters {
public:
    // Builds the pipeline for a depth-only dynamic rendering pass over a `depthFormat` attachment and a descriptor set per frame parity over
    // `frameUbo`, the island's ShadowFrameUBO (the light-space matrix; bound with a dynamic offset, `uboRange` bytes per slice). False, with the reason
    // logged, when a shader module or an object cannot be made.
    bool init(VkDevice device, VmaAllocator allocator, VkFormat depthFormat, VkBuffer frameUbo, VkDeviceSize uboRange, std::string_view spvDir);

    // Pulls this frame's mech feed into the buffers of parity `epoch & 1`. The first call of an epoch does the work; later calls of the same epoch return at once.
    // The caller has retired the previous use of that parity (the island's frame fence). A feed that is not ready, or whose run list does not fit its own
    // buffers, leaves nothing to draw this epoch.
    void stage(uint64_t epoch);

    // Records the once-per-epoch compute pre-skin of the staged mechs into `cb` (no render pass open), ordered before every caster pass of the epoch.
    // Call right after stage(); a no-op when nothing was planned.
    void recordSkin(VkCommandBuffer cb);

    bool hasCasters() const { return !runs_.empty() && stagedEpoch_ != kNoEpoch; }

    // Binds the pipeline and draws every run into the open render pass of `cb`; `uboOffset` is this pass's slice of the frame UBO. The caller has set the
    // viewport, the scissor and the depth bias. `runMask` (one byte per runs() entry, null = all) drops the runs whose mechs cannot reach this map.
    // Returns the draws recorded.
    uint32_t record(VkCommandBuffer cb, uint32_t parity, uint32_t uboOffset, const uint8_t* runMask = nullptr, bool localLight = false, bool* pipelineBound = nullptr);   // pipelineBound: the caller's per-pass flag; set once pipeline_ is bound, and the bind is skipped while it is set (tiles of one render pass)
    const std::vector<Mc2MechDrawRun>& runs() const { return runs_; }

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
    VkBuffer vertexBuf_ = VK_NULL_HANDLE, indexBuf_ = VK_NULL_HANDLE;   // borrowed from the mech island (device-local, shared)
    VkDeviceSize instCap_[kMaxFramesInFlight] = {}, boneCap_[kMaxFramesInFlight] = {};
    std::vector<Mc2MechDrawRun> runs_;   // the runs that passed validation this epoch
    MechSkinPass skin_;                  // compute pre-skin of the staged instances; the casters read its regions through the 0x80 instance flag
    std::vector<uint8_t> skinInstances_; // the instance records uploaded this epoch (the feed copy plus the pre-skin flags)
    uint32_t skinParity_ = 0;
    uint64_t skinVboBytes_ = 0;
    // Indirect commands retain every skin draw, grouped only by vertex buffer.
    // stage() resets the retired frame slot; each shadow map gets its own range.
    OwnedBuffer drawCmds_[kMaxFramesInFlight];
    VkDeviceSize drawCap_[kMaxFramesInFlight] = {};
    VkDeviceSize drawCursor_ = 0;
    uint32_t maxIndirectDraws_ = 0;

    // Declared in creation order: released in reverse, so the pipeline goes before its layout and shaders, the sets with their pool before the layout.
    OwnedBuffer instBuf_[kMaxFramesInFlight], boneBuf_[kMaxFramesInFlight];
    OwnedDescriptorSetLayout dsl_;
    OwnedDescriptorPool pool_;
    VkDescriptorSet set_[kMaxFramesInFlight] = {};   // allocated from pool_, freed with it
    OwnedPipelineLayout layout_;
    OwnedShaderModule vert_, frag_;
    OwnedPipeline pipeline_;
};

}  // namespace mc2vk

#endif  // MC2_VULKAN_SHADOW_MECH_CASTERS_H
