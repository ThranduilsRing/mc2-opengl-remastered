#include "vulkan_static_visibility_tiles.h"
#ifdef MC2_VULKAN_ISLAND
#include "vk_gpu_profile.h"
#include <string>
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include <vk_mem_alloc.h>

namespace mc2vk {
bool StaticVisibilityTiles::init(VkDevice device, VmaAllocator allocator, std::string_view spvDir) {
    device_ = device; allocator_ = allocator;
    if (!pipe_.create(device, std::string(spvDir)+"terrain_static_visibility.comp.spv",
            {{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER},{1,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}}, "terrain.staticVisibility")) return false;
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (!create(sampler_,device,[&](auto* h){return vkCreateSampler(device,&sci,nullptr,h);})) return false;
    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1}};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 1; pci.poolSizeCount = 2; pci.pPoolSizes = sizes;
    if (!create(pool_,device,[&](auto* h){return vkCreateDescriptorPool(device,&pci,nullptr,h);})) return false;
    const VkDescriptorSetLayout layout = pipe_.setLayout();
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool_.get(); ai.descriptorSetCount = 1; ai.pSetLayouts = &layout;
    return vkAllocateDescriptorSets(device,&ai,&set_)==VK_SUCCESS;
}

bool StaticVisibilityTiles::record(VkCommandBuffer cb, VkImageView depth, uint32_t depthSide) {
    ready_ = pending_ = false;
    if (!pipe_ || !set_ || !depth || depthSide==0) return false;
    const uint32_t side = (depthSide+31)/32;
    if (!image_ || side_!=side) {
        view_.reset(); image_.reset(); side_ = 0; initialized_ = false;
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D; ici.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        ici.extent = {side,side,1}; ici.mipLevels = ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
        VmaAllocationCreateInfo aci{}; aci.usage = VMA_MEMORY_USAGE_AUTO;
        if (!image_.create(allocator_,device_,ici,aci,"terrain.staticVisibilityTiles")) return false;
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = image_.get(); vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = ici.format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        if (!create(view_,device_,[&](auto* h){return vkCreateImageView(device_,&vci,nullptr,h);})) return false;
        side_ = side;
    }
    const VkDescriptorImageInfo input{sampler_.get(),depth,VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo output{VK_NULL_HANDLE,view_.get(),VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet writes[2]{};
    for (uint32_t i=0;i<2;++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set_; writes[i].dstBinding = i; writes[i].descriptorCount = 1;
    }
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[0].pImageInfo = &input;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; writes[1].pImageInfo = &output;
    vkUpdateDescriptorSets(device_,2,writes,0,nullptr);
    VkImageMemoryBarrier imageBar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    imageBar.oldLayout = initialized_ ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    imageBar.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    imageBar.srcQueueFamilyIndex = imageBar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imageBar.image = image_.get(); imageBar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    imageBar.srcAccessMask = initialized_ ? VK_ACCESS_SHADER_READ_BIT : 0;
    imageBar.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cb,initialized_ ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,1,&imageBar);
    cmdBarrier(cb,VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT);
    {
        MC2_VK_GPU_SCOPE(scope,cb,"VKIsl.Shadow.StaticVisibility");
        pipe_.bind(cb,set_);
        vkCmdDispatch(cb,side,side,1);
    }
    imageBar.oldLayout = VK_IMAGE_LAYOUT_GENERAL; imageBar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageBar.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; imageBar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0,0,nullptr,0,nullptr,1,&imageBar);
    initialized_ = pending_ = true;
    return true;
}
} // namespace mc2vk
#endif
