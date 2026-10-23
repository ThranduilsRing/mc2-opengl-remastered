#pragma once
#ifdef MC2_VULKAN_ISLAND
#include "vk_owned.h"
#include "vulkan_ground_lighting.h"
namespace mc2veg {
// Fragment-only subset of the terrain layout: FS uses four SSBOs, VS stays at
// three. Shadows use the shared VFX set 0; this set carries lighting only.
// Reusing the whole terrain layout would exceed Vulkan's minimum floor.
class GroundLighting {
    mc2vk::OwnedDescriptorSetLayout layout_;
    mc2vk::OwnedDescriptorPool pool_;
    VkDescriptorSet sets_[2]{};
    static constexpr uint32_t bindings_[]{0, 2, 3, 4, 11, 12, 13, 16};
public:
    VkDescriptorSetLayout layout() const { return layout_.get(); }
    VkDescriptorSet set(uint32_t parity) const { return sets_[parity]; }
    bool init(VkDevice device) {
        VkDescriptorSetLayoutBinding bindings[8]{};
        for (size_t k = 0; k < 8; ++k) {
            const uint32_t b = bindings_[k];
            const auto type = b == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC :
                b == 4 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER :
                b >= 11 && b <= 13 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC :
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[k] = {b, type, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        }
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        lci.bindingCount = 8; lci.pBindings = bindings;
        if (!mc2vk::create(layout_, device, [&](VkDescriptorSetLayout* out) { return vkCreateDescriptorSetLayout(device, &lci, nullptr, out); })) return false;
        const VkDescriptorPoolSize sizes[]{
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 6},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 6}};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pci.maxSets = 2; pci.poolSizeCount = 4; pci.pPoolSizes = sizes;
        if (!mc2vk::create(pool_, device, [&](VkDescriptorPool* out) { return vkCreateDescriptorPool(device, &pci, nullptr, out); })) return false;
        const VkDescriptorSetLayout layouts[]{layout(), layout()};
        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = pool_.get(); alloc.descriptorSetCount = 2; alloc.pSetLayouts = layouts;
        return vkAllocateDescriptorSets(device, &alloc, sets_) == VK_SUCCESS;
    }
    // Caller must retire this VFX parity before changing its descriptors.
    void copy(VkDevice device, uint32_t parity, const Mc2GroundLightingFrame& source) {
        VkCopyDescriptorSet copies[8]{};
        for (size_t k = 0; k < 8; ++k) {
            copies[k].sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET;
            copies[k].srcSet = source.set; copies[k].srcBinding = bindings_[k];
            copies[k].dstSet = set(parity); copies[k].dstBinding = bindings_[k];
            copies[k].descriptorCount = 1;
        }
        vkUpdateDescriptorSets(device, 0, nullptr, 8, copies);
    }
};
}
#endif
