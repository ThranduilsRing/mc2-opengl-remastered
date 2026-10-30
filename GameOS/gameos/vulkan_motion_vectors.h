#pragma once
#ifdef MC2_VULKAN_ISLAND
#include <memory>
#include <volk.h>
struct VkImageResource;
namespace mc2vk {
class VulkanContext;
// Main-view camera motion, in UV units (previous minus current), at render resolution.
// The present owner retires users before prepare/reset; recording adds no submission.
class MotionVectors {
public:
    MotionVectors();
    ~MotionVectors();
    bool prepare(VulkanContext& ctx, const VkImageResource& depth, VkImageResource& motion);
    void record(VkCommandBuffer cb, const VkImageResource& depth);
    void submitted();
    void reset();
    VkImageView view() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
#endif
