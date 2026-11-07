#pragma once
#ifdef MC2_VULKAN_ISLAND
#include <memory>
#include <volk.h>
struct VkImageResource;
namespace mc2vk {
class VulkanContext;
class MotionVectors;
// FSR 3.1 native AA: the first temporal consumer.
// Loads the vendored amd_fidelityfx_vk.dll at runtime; with no DLL, no device support or a failed
// dispatch it stays inactive and the spatial AA path runs unchanged. While active it counts as a
// RenderCore::temporal consumer, which turns on the render-feed jitter and the provider mip bias.
// Present-owner object: prepare/record/reset run on the thread that records the composite.
class FsrProvider {
public:
    FsrProvider();
    ~FsrProvider();
    // wanted = the user has AA enabled. True when record() can run this frame.
    bool prepare(VulkanContext& ctx, const VkImageResource& scene, const VkImageResource& depth, bool wanted);
    // Records the dispatch. Needs the motion pass recorded earlier in the same command buffer;
    // scene must be in SHADER_READ_ONLY_OPTIMAL and depth in DEPTH_STENCIL_ATTACHMENT_OPTIMAL
    // (both are returned in that layout). The output stays in GENERAL.
    bool record(VkCommandBuffer cb, const VkImageResource& scene, const VkImageResource& depth,
                const MotionVectors& mv, uint32_t renderFrame);
    VkImageView outputView() const;
    // History epoch change (camera cut, mission load, device reset): the next dispatch resets.
    void resetHistory();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
#endif
