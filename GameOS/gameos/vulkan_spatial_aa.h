#pragma once
#ifdef MC2_VULKAN_ISLAND
#include <memory>
#include <volk.h>
struct VkImageResource;
namespace mc2vk {
class VulkanContext;
// Resources belong to the present chain. prepare/reset require all users retired; record adds no submit or CPU wait.
class SpatialAA {
public:
    SpatialAA();
    ~SpatialAA();
    bool prepare(VulkanContext& ctx, const VkImageResource& output, int mode, VkImageView motion = VK_NULL_HANDLE);
    void record(VkCommandBuffer cb, const VkImageResource& output);
    void submitted();
    void reset();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    int failedMode_ = -1;
    uint32_t failedW_ = 0, failedH_ = 0;
};
}
#endif
