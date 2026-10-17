// vk_compute_pipeline.cpp -- see vk_compute_pipeline.h.

#ifdef MC2_VULKAN_ISLAND

#include "vk_compute_pipeline.h"

#include "vk_island_util.h"
#include "vulkan_context.h"

#include <cstdarg>
#include <cstdio>
#include <string>

namespace mc2vk {

namespace {
void clog(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vk_island_util::vlogv("[VK_COMPUTE] ", fmt, ap);
    va_end(ap);
}
}  // namespace

bool ComputePipeline::create(VkDevice device, std::string_view spvPath, const std::vector<ComputeBinding>& bindings, std::string_view name) {
    bindings_ = bindings;
    module_ = OwnedShaderModule(device, vk_island_util::loadSpv(device, spvPath, clog));
    if (!module_) return false;

    std::vector<VkDescriptorSetLayoutBinding> b(bindings.size());
    for (size_t i = 0; i < bindings.size(); ++i) {
        b[i] = {};
        b[i].binding = bindings[i].binding;
        b[i].descriptorType = bindings[i].type;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo lci{};
    lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    lci.bindingCount = static_cast<uint32_t>(b.size());
    lci.pBindings = b.data();
    if (!mc2vk::create(dsl_, device, [&](VkDescriptorSetLayout* h) { return vkCreateDescriptorSetLayout(device, &lci, nullptr, h); })) {
        clog("%.*s: vkCreateDescriptorSetLayout failed.", static_cast<int>(name.size()), name.data());
        return false;
    }
    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    const VkDescriptorSetLayout dslHandle = dsl_.get();
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &dslHandle;
    if (!mc2vk::create(layout_, device, [&](VkPipelineLayout* h) { return vkCreatePipelineLayout(device, &plci, nullptr, h); })) {
        clog("%.*s: vkCreatePipelineLayout failed.", static_cast<int>(name.size()), name.data());
        return false;
    }
    VkComputePipelineCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = module_.get();
    cpci.stage.pName = "main";
    cpci.layout = layout_.get();
    if (!mc2vk::create(pipe_, device, [&](VkPipeline* h) { return vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, nullptr, h); })) {
        clog("%.*s: vkCreateComputePipelines failed.", static_cast<int>(name.size()), name.data());
        return false;
    }
    nameObject(device, pipe_.get(), name);
    return true;
}

void ComputePipeline::bind(VkCommandBuffer cb, VkDescriptorSet set, const uint32_t* dynamicOffsets, uint32_t dynamicCount) const {
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_.get());
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout_.get(), 0, 1, &set, dynamicCount, dynamicOffsets);
}

bool ComputeSets::create(VkDevice device, const ComputePipeline& pipe, uint32_t count, std::string_view name) {
    uint32_t nType[4] = {0, 0, 0, 0};   // storage, uniform, storage dynamic, uniform dynamic
    for (const ComputeBinding& b : pipe.bindings()) {
        switch (b.type) {
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:         ++nType[1]; break;
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: ++nType[2]; break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC: ++nType[3]; break;
            default:                                        ++nType[0]; break;
        }
    }
    const VkDescriptorType kTypes[4] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,
                                        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC};
    VkDescriptorPoolSize sizes[4]{};
    uint32_t nSizes = 0;
    for (int t = 0; t < 4; ++t)
        if (nType[t]) sizes[nSizes++] = {kTypes[t], nType[t] * count};
    VkDescriptorPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pci.maxSets = count;
    pci.poolSizeCount = nSizes;
    pci.pPoolSizes = sizes;
    if (!mc2vk::create(pool_, device, [&](VkDescriptorPool* h) { return vkCreateDescriptorPool(device, &pci, nullptr, h); })) {
        clog("%.*s: vkCreateDescriptorPool failed.", static_cast<int>(name.size()), name.data());
        return false;
    }
    std::vector<VkDescriptorSetLayout> layouts(count, pipe.setLayout());
    sets_.assign(count, VK_NULL_HANDLE);
    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = pool_.get();
    dsai.descriptorSetCount = count;
    dsai.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(device, &dsai, sets_.data()) != VK_SUCCESS) {
        clog("%.*s: vkAllocateDescriptorSets failed.", static_cast<int>(name.size()), name.data());
        sets_.clear();
        return false;
    }
    return true;
}

void ComputeSets::write(VkDevice device, uint32_t i, uint32_t binding, VkDescriptorType type, VkBuffer buffer, VkDeviceSize size) const {
    if (i >= sets_.size() || buffer == VK_NULL_HANDLE) return;
    VkDescriptorBufferInfo info{buffer, 0, size};
    VkWriteDescriptorSet w{};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = sets_[i];
    w.dstBinding = binding;
    w.descriptorCount = 1;
    w.descriptorType = type;
    w.pBufferInfo = &info;
    vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
}

void cmdBarrier(VkCommandBuffer cb, VkPipelineStageFlags srcStage, VkAccessFlags srcAccess, VkPipelineStageFlags dstStage, VkAccessFlags dstAccess) {
    VkMemoryBarrier mb{};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = srcAccess;
    mb.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(cb, srcStage, dstStage, 0, 1, &mb, 0, nullptr, 0, nullptr);
}

}  // namespace mc2vk

#endif  // MC2_VULKAN_ISLAND
