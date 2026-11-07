#pragma once
#include <memory>
#include <volk.h>
struct VkImageResource;
namespace mc2vk {
class VulkanContext;
// Main-view camera motion by depth reprojection (docs/plans/2026-11-06-temporal-aa.md):
// RG16F at render resolution, UV units, previousUV - currentUV, unjittered, zero on sky
// and on history reset. The present owner retires users before prepare/reset.
class MotionVectors {
public:
    MotionVectors();
    ~MotionVectors();
    bool prepare(VulkanContext& ctx, const VkImageResource& depth);
    // Records the motion pass. Depth must be in DEPTH_STENCIL_ATTACHMENT_OPTIMAL and is
    // returned there. The output ends in SHADER_READ_ONLY_OPTIMAL.
    // objectMotionWritten: the mech island wrote the shared object-motion image this frame (it is then in
    // SHADER_READ_ONLY_OPTIMAL); its pixels override the camera vector.
    void record(VkCommandBuffer cb, const VkImageResource& depth, uint32_t renderFrame, bool objectMotionWritten);
    // Debug inset (ImGui Graphics Options "Motion Vectors") over an RGBA8 target that is in
    // COLOR_ATTACHMENT_OPTIMAL. Draws only when record() ran since the last call.
    bool recordDebug(VkCommandBuffer cb, VkImageView target, uint32_t w, uint32_t h);
    void reset();
    VkImageView view() const;
    // R8 reactive/transparency mask, render resolution, same lifetime and layout as view(). The
    // FSR consumer binds it with view() and the shared scene depth; the pass clears it to 0.
    VkImageView reactiveView() const;
    VkImage image() const;
    VkImage reactiveImage() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
