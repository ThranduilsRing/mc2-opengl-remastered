#pragma once
#ifdef MC2_VULKAN_ISLAND
#include "vk_compute_pipeline.h"

namespace mc2vk {
// Exact lit/shadow proofs for uniform static-map regions; mixed tiles retain the dense receiver.
class StaticVisibilityTiles {
public:
    bool init(VkDevice device, VmaAllocator allocator, std::string_view spvDir);
    bool record(VkCommandBuffer cb, VkImageView depth, uint32_t side);
    void markSubmitted() { ready_ = pending_; pending_ = false; }
    void invalidate() { ready_ = pending_ = false; }
    VkImageView view() const { return ready_ ? view_.get() : VK_NULL_HANDLE; }
private:
    VkDevice device_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = nullptr;
    uint32_t side_ = 0;
    bool initialized_ = false, ready_ = false, pending_ = false;
    OwnedImage image_;
    OwnedImageView view_;
    OwnedSampler sampler_;
    ComputePipeline pipe_;
    OwnedDescriptorPool pool_;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
};
} // namespace mc2vk
#endif
