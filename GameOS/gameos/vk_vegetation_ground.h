#pragma once
#include "vk_owned.h"
#include "vk_frame_ring.h"
#include "vulkan_ground_lighting.h"
#include "vk_desc_write_cache.h"
namespace mc2veg {
// Fragment-only subset of the terrain layout: FS uses four SSBOs, VS stays at
// three. Shadows use the shared VFX set 0; this set carries lighting only.
// Reusing the whole terrain layout would exceed Vulkan's minimum floor.
class GroundLighting {
    mc2vk::OwnedDescriptorSetLayout layout_;
    mc2vk::OwnedDescriptorPool pool_;
    VkDescriptorSet sets_[mc2vk::kMaxFramesInFlight]{};
    mc2vk::DescWriteStamp stamps_[mc2vk::kMaxFramesInFlight]{};
    // 19 = LOCAL-LIGHTS-2 local-light shadow atlas, read by the pool loop.
    // 26 = the per-frame mission environment (EnvironmentUBO); the only binding here a vertex stage reads (props and mechs light per vertex).
    static constexpr uint32_t bindings_[]{0, 2, 3, 4, 11, 12, 13, 16, 19, 26};
    static constexpr size_t kN = sizeof(bindings_) / sizeof(bindings_[0]);
public:
    VkDescriptorSetLayout layout() const { return layout_.get(); }
    VkDescriptorSet set(uint32_t parity) const { return sets_[parity]; }
    bool init(VkDevice device) {
        VkDescriptorSetLayoutBinding bindings[kN]{};
        for (size_t k = 0; k < kN; ++k) {
            const uint32_t b = bindings_[k];
            const auto type = b == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC :
                b == 4 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER :
                b >= 11 && b <= 13 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC :
                b == 26 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER :
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[k] = {b, type, 1, b == 26 ? (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT) : VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        }
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        lci.bindingCount = static_cast<uint32_t>(kN); lci.pBindings = bindings;
        if (!mc2vk::create(layout_, device, [&](VkDescriptorSetLayout* out) { return vkCreateDescriptorSetLayout(device, &lci, nullptr, out); })) return false;
        const VkDescriptorPoolSize sizes[]{
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, mc2vk::kMaxFramesInFlight},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, mc2vk::kMaxFramesInFlight},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 3 * mc2vk::kMaxFramesInFlight},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 * mc2vk::kMaxFramesInFlight},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, mc2vk::kMaxFramesInFlight}};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pci.maxSets = mc2vk::kMaxFramesInFlight; pci.poolSizeCount = 5; pci.pPoolSizes = sizes;
        if (!mc2vk::create(pool_, device, [&](VkDescriptorPool* out) { return vkCreateDescriptorPool(device, &pci, nullptr, out); })) return false;
        VkDescriptorSetLayout layouts[mc2vk::kMaxFramesInFlight]; for (auto& entry : layouts) entry = layout();
        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = pool_.get(); alloc.descriptorSetCount = mc2vk::kMaxFramesInFlight; alloc.pSetLayouts = layouts;
        return MC2_VK_ALLOC_SETS(device, &alloc, sets_) == VK_SUCCESS;
    }
    // Forget every parity's copy (mission reset: the next copy is issued even if the source key repeats).
    void invalidate() { for (auto& s : stamps_) s.invalidate(); }
    // Caller must retire this VFX parity before changing its descriptors. Skipped when this parity already holds the
    // source set's current revision (terrain bumps it on every write to that set).
    void copy(VkDevice device, uint32_t parity, const Mc2GroundLightingFrame& source) {
        if (!stamps_[parity].needsWrite({mc2vk::descKeyWord(source.set), source.revision, 0, 0})) {
            mc2vk::noteDescWrites(0, static_cast<uint32_t>(kN));
            return;
        }
        mc2vk::noteDescWrites(static_cast<uint32_t>(kN), 0);
        VkCopyDescriptorSet copies[kN]{};
        for (size_t k = 0; k < kN; ++k) {
            copies[k].sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET;
            copies[k].srcSet = source.set; copies[k].srcBinding = bindings_[k];
            copies[k].dstSet = set(parity); copies[k].dstBinding = bindings_[k];
            copies[k].descriptorCount = 1;
        }
        vkUpdateDescriptorSets(device, 0, nullptr, static_cast<uint32_t>(kN), copies);
    }
};
}
