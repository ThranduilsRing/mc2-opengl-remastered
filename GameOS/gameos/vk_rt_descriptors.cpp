// vk_rt_descriptors.cpp -- see vk_rt_descriptors.h.


#include "vk_rt_descriptors.h"

#include "vulkan_context.h"

#include <vector>

namespace mc2vk::rt {

namespace {
VkDescriptorType typeOf(BindingKind k) {
    switch (k) {
    case BindingKind::AccelerationStructure: return VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    case BindingKind::SampledImage: return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    default: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    }
}
}  // namespace

bool RayQuerySet::create(const VulkanContext& ctx, std::span<const BindingDesc> table, uint32_t pushBytes, VkShaderStageFlags stages) {
    static_assert(bindingsAreValid(kSmokeBindings.data(), static_cast<uint32_t>(kSmokeBindings.size())));
    static_assert(bindingsAreValid(kSceneProbeBindings.data(), static_cast<uint32_t>(kSceneProbeBindings.size())));
    const VkDevice device = ctx.device();
    if (!ctx.rt().rayQuery || !bindingsAreValid(table.data(), static_cast<uint32_t>(table.size()))) return false;

    std::vector<VkDescriptorSetLayoutBinding> b(table.size());
    std::vector<VkDescriptorPoolSize> sizes;
    for (size_t i = 0; i < table.size(); ++i) {
        b[i] = {};
        b[i].binding = table[i].binding;
        b[i].descriptorType = typeOf(table[i].kind);
        b[i].descriptorCount = 1;
        b[i].stageFlags = stages;
        bool found = false;
        for (VkDescriptorPoolSize& s : sizes)
            if (s.type == b[i].descriptorType) { ++s.descriptorCount; found = true; }
        if (!found) sizes.push_back({b[i].descriptorType, 1});
    }
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = static_cast<uint32_t>(b.size());
    lci.pBindings = b.data();
    if (!mc2vk::create(dsl_, device, [&](VkDescriptorSetLayout* h) { return vkCreateDescriptorSetLayout(device, &lci, nullptr, h); })) return false;

    const VkDescriptorSetLayout dsl = dsl_.get();
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &dsl;
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, pushBytes};
    plci.pushConstantRangeCount = pushBytes ? 1u : 0u;
    plci.pPushConstantRanges = pushBytes ? &push : nullptr;
    if (!mc2vk::create(layout_, device, [&](VkPipelineLayout* h) { return vkCreatePipelineLayout(device, &plci, nullptr, h); })) return false;

    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 1;
    pci.poolSizeCount = static_cast<uint32_t>(sizes.size());
    pci.pPoolSizes = sizes.data();
    if (!mc2vk::create(pool_, device, [&](VkDescriptorPool* h) { return vkCreateDescriptorPool(device, &pci, nullptr, h); })) return false;

    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool_.get();
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &dsl;
    return MC2_VK_ALLOC_SETS(device, &ai, &set_) == VK_SUCCESS;
}

void RayQuerySet::write(VkDevice device, VkAccelerationStructureKHR tlas, VkBuffer rays, VkBuffer results) const {
    VkWriteDescriptorSetAccelerationStructureKHR asInfo{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    asInfo.accelerationStructureCount = 1;
    asInfo.pAccelerationStructures = &tlas;
    VkDescriptorBufferInfo raysInfo{rays, 0, VK_WHOLE_SIZE}, resultsInfo{results, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w[3]{};
    for (VkWriteDescriptorSet& x : w) {
        x.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        x.dstSet = set_;
        x.descriptorCount = 1;
    }
    w[0].dstBinding = kSmokeBindings[0].binding;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    w[0].pNext = &asInfo;
    w[1].dstBinding = kSmokeBindings[1].binding;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[1].pBufferInfo = &raysInfo;
    w[2].dstBinding = kSmokeBindings[2].binding;
    w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[2].pBufferInfo = &resultsInfo;
    vkUpdateDescriptorSets(device, 3, w, 0, nullptr);
}

void RayQuerySet::writeTlas(VkDevice device, uint32_t binding, VkAccelerationStructureKHR tlas) const {
    VkWriteDescriptorSetAccelerationStructureKHR asInfo{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    asInfo.accelerationStructureCount = 1;
    asInfo.pAccelerationStructures = &tlas;
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.pNext = &asInfo;
    w.dstSet = set_;
    w.dstBinding = binding;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
}

void RayQuerySet::writeBuffer(VkDevice device, uint32_t binding, VkBuffer buffer) const {
    const VkDescriptorBufferInfo info{buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set_;
    w.dstBinding = binding;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w.pBufferInfo = &info;
    vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
}

void RayQuerySet::writeImage(VkDevice device, uint32_t binding, VkSampler sampler, VkImageView view) const {
    const VkDescriptorImageInfo info{sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set_;
    w.dstBinding = binding;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &info;
    vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
}

}  // namespace mc2vk::rt

