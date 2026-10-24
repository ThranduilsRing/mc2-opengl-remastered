// vk_compute_pipeline.h -- RAII compute pipeline and descriptor sets for the native Vulkan producers (step 3.4P).
//
// WHAT: `ComputePipeline` owns one shader module, one set-0 descriptor layout (storage / uniform buffers only), the pipeline layout and the compute
// pipeline; `ComputeSets` owns a descriptor pool and N sets of one pipeline's layout. Both are move-only owners built on vk_owned.h: the destructor is the
// only release path and the owners must be destroyed before the device (islands are, ahead of the engine context).
// WHY: before 3.4P no Vulkan compute pipeline existed (the 11 compute shaders were GL). The producers record dispatches into the island's own command
// buffer on the single graphics queue and order them with `cmdBarrier`, so there is no semaphore and no async compute yet.
#ifndef MC2_VK_COMPUTE_PIPELINE_H
#define MC2_VK_COMPUTE_PIPELINE_H

#ifdef MC2_VULKAN_ISLAND

#include <volk.h>

#include "vk_owned.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace mc2vk {

struct ComputeBinding {
    uint32_t         binding;
    VkDescriptorType type;   // VK_DESCRIPTOR_TYPE_STORAGE_BUFFER / UNIFORM_BUFFER, or the _DYNAMIC variant of either (a frame-ring binding)
};

class ComputePipeline {
public:
    // Loads `spvPath` (entry point "main"), builds the set-0 layout from `bindings` and the pipeline. False (and logs through the island log sink) on failure.
    bool create(VkDevice device, std::string_view spvPath, const std::vector<ComputeBinding>& bindings, std::string_view name, bool logFailures = true);
    explicit operator bool() const noexcept { return static_cast<bool>(pipe_); }

    VkPipeline            pipeline() const noexcept { return pipe_.get(); }
    VkPipelineLayout      layout() const noexcept { return layout_.get(); }
    VkDescriptorSetLayout setLayout() const noexcept { return dsl_.get(); }
    const std::vector<ComputeBinding>& bindings() const noexcept { return bindings_; }

    // vkCmdBindPipeline + vkCmdBindDescriptorSets(set 0), with the dynamic offsets of the layout's _DYNAMIC bindings in binding order.
    void bind(VkCommandBuffer cb, VkDescriptorSet set, std::span<const uint32_t> dynamicOffsets = {}) const;
    void dispatch(VkCommandBuffer cb, uint32_t groupsX) const { vkCmdDispatch(cb, groupsX, 1, 1); }

private:
    OwnedShaderModule         module_;
    OwnedDescriptorSetLayout  dsl_;
    OwnedPipelineLayout       layout_;
    OwnedPipeline             pipe_;
    std::vector<ComputeBinding> bindings_;
};

// N descriptor sets of one ComputePipeline's layout, all written through `write`.
class ComputeSets {
public:
    bool create(VkDevice device, const ComputePipeline& pipe, uint32_t count, std::string_view name, bool logFailures = true);
    VkDescriptorSet set(uint32_t i) const noexcept { return i < sets_.size() ? sets_[i] : VK_NULL_HANDLE; }
    // Points set `i` binding `binding` at the whole buffer (descriptor type taken from the pipeline's layout). The caller guarantees no in-flight
    // command buffer uses the set (the producers rewrite only after the slot's fence wait or a device idle).
    void write(VkDevice device, uint32_t i, uint32_t binding, VkDescriptorType type, VkBuffer buffer, VkDeviceSize size = VK_WHOLE_SIZE) const;

private:
    OwnedDescriptorPool       pool_;
    std::vector<VkDescriptorSet> sets_;   // freed with the pool
};

// One global memory barrier between two stages (the producers order whole-buffer hand-offs, so a memory barrier is exact).
void cmdBarrier(VkCommandBuffer cb, VkPipelineStageFlags srcStage, VkAccessFlags srcAccess, VkPipelineStageFlags dstStage, VkAccessFlags dstAccess);

}  // namespace mc2vk

#endif  // MC2_VULKAN_ISLAND
#endif  // MC2_VK_COMPUTE_PIPELINE_H
