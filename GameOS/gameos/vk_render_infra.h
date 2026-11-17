#pragma once
#include <cstdint>
#include <string_view>
// Layout does not depend on MC2_VULKAN: no Vulkan type is a member, so every target sees the same class.
namespace mc2vk::infra {
// Holds admission across driver allocation; a failed allocation returns its reservation.
class Admission {
public:
    Admission(uint64_t bytes, std::string_view name);
    ~Admission();
    Admission(const Admission&) = delete;
    Admission& operator=(const Admission&) = delete;
    explicit operator bool() const { return bytes_ != 0; }
    void commit(uint64_t key, std::string_view name, bool raw = false);
private:
    uint64_t bytes_ = 0;
};
}
// Game-side load hook; no Vulkan types cross into the simulation target.
void mc2PrewarmMissionPipelines();
#ifdef MC2_VULKAN
#include <volk.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <span>

namespace mc2vk::infra {
void init(VkDevice device, VkPhysicalDevice physical);
void materialSampler(VkSamplerCreateInfo& info);
bool materialBiasStale(float usedBias);   // the sampler was built at usedBias; the effective resolve state has since changed
void materialBiasTick();                  // once per present frame: logs [VK_PRESENT] material_mip_bias on every change
void shutdown();
void frame();
void prewarm(int width, int height);
void saveCache();
VkPipelineCache cacheHandle();
void deviceLost();
// True once any thread saw VK_ERROR_DEVICE_LOST. The device never comes back: callers stop queue work and the host exits at the next frame top.
bool deviceLostLatched();
VkResult graphics(VkDevice, VkPipelineCache, uint32_t, const VkGraphicsPipelineCreateInfo*, const VkAllocationCallbacks*, VkPipeline*, uint64_t frame = UINT64_MAX);
VkResult compute(VkDevice, VkPipelineCache, uint32_t, const VkComputePipelineCreateInfo*, const VkAllocationCallbacks*, VkPipeline*, uint64_t frame = UINT64_MAX);
void shader(VkShaderModule, std::string_view path, std::span<const uint32_t> code);
void forgetShader(VkShaderModule);
void bind(VkCommandBuffer, VkPipelineBindPoint, VkPipeline);
// Dynamic full-target viewport carrying this frame's temporal-AA jitter (zero with no consumer); scissor stays baked.
void setSceneViewport(VkCommandBuffer, uint32_t w, uint32_t h);
uint64_t adoptPipeline(VkDevice, VkPipeline);
VkPipeline currentPipeline(uint64_t token);
void destroyOwnedPipeline(uint64_t token);
void deferPipeline(uint64_t token);
void release(uint64_t key);
void defer(uint64_t key);
template<class Owner> void retiring(Owner& owner) {
    if constexpr(requires { owner.markDeferred(); }) owner.markDeferred();
    else if constexpr(requires { owner->markDeferred(); }) { if(owner) owner->markDeferred(); }
    else if constexpr(requires { owner.begin(); owner.end(); }) { for(auto& item:owner) retiring(item); }
}
bool admit(uint64_t bytes);
uint64_t headroom();   // budget - live - pending, clamped at 0
bool imageSupported(const VkImageCreateInfo&, std::string_view name);
uint32_t imageLimit();
uint32_t shadowTier(uint32_t requested, uint64_t bytesPerTexel, uint32_t copies = 1);
std::string ledgerJson();
void panel();
void breadcrumb(std::string_view kind, std::string_view name, uint64_t hash = 0, VkCommandBuffer cb = VK_NULL_HANDLE, uint64_t frame = UINT64_MAX);
// Crash-path safe: copies preformatted snapshots, never calls the driver or allocates.
void crashSnapshot(std::string_view folder);
}
#endif
