#pragma once
#include "vk_owned.h"
#include "vk_frame_ring.h"
#include "../../RenderCore/TextureService.h"
#include <array>

namespace mc2vk {
class VulkanContext;
class TextureResidency;
enum class TexSampler : uint32_t {
    RepeatBiased, ClampBiased, ClampBiasedLod16, RepeatBaseLevel,
    ClampAniso8Biased, RepeatBiasedLod0
};
inline constexpr uint32_t kTexSamplerCount = 16;

// Only beginFrame mutates descriptors, after the flight fence and residency pump.
class TextureTable {
public:
    TextureTable(VulkanContext& ctx, TextureResidency& residency, RenderCore::TextureDescriptorTable& slots,
                 uint32_t capacity) : ctx_(ctx), residency_(residency), slots_(slots), capacity_(capacity) {}
    bool init();
    void beginFrame(uint32_t frameSlot);
    uint32_t resolve(RenderCore::TexSlot token);
    VkDescriptorSetLayout layout() const { return layout_.get(); }
    VkDescriptorSet set(uint32_t frameSlot) const { return sets_[frameSlot]; }
    VkSampler sampler(TexSampler kind) const { return samplers_[static_cast<uint32_t>(kind)].get(); }
    void bind(VkCommandBuffer cb, VkPipelineBindPoint point, VkPipelineLayout layout, uint32_t setIndex) const;
private:
    bool rebuildSamplers();
    VulkanContext& ctx_;
    TextureResidency& residency_;
    RenderCore::TextureDescriptorTable& slots_;
    uint32_t capacity_;
    float bias_ = 0;
    uint64_t stale_ = 0;
    uint64_t begunFrame_ = UINT64_MAX;
    OwnedDescriptorSetLayout layout_;
    OwnedDescriptorPool pool_;
    std::array<OwnedSampler, kTexSamplerCount> samplers_;
    std::array<VkDescriptorSet, kMaxFramesInFlight> sets_{};
    std::array<uint64_t, kMaxFramesInFlight> applied_{};
    std::array<bool, kMaxFramesInFlight> initialized_{};
    std::array<std::vector<RenderCore::TexSlot>, kMaxFramesInFlight> tokens_;
};
}
