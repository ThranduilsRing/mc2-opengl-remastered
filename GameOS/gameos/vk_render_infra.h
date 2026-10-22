#pragma once
#ifdef MC2_VULKAN_ISLAND
// Game-side load hook; no Vulkan types cross into the simulation target.
void mc2PrewarmMissionPipelines();
#endif
#ifdef MC2_VULKAN
#include <volk.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <span>

namespace mc2vk::infra {
void init(VkDevice device, VkPhysicalDevice physical);
void shutdown();
void frame();
void prewarm(int width, int height);
void saveCache();
VkPipelineCache cacheHandle();
void deviceLost();
VkResult graphics(VkDevice, VkPipelineCache, uint32_t, const VkGraphicsPipelineCreateInfo*, const VkAllocationCallbacks*, VkPipeline*);
VkResult compute(VkDevice, VkPipelineCache, uint32_t, const VkComputePipelineCreateInfo*, const VkAllocationCallbacks*, VkPipeline*);
void shader(VkShaderModule, std::string_view path, std::span<const uint32_t> code);
void forgetShader(VkShaderModule);
void bind(VkCommandBuffer, VkPipelineBindPoint, VkPipeline);
uint64_t adoptPipeline(VkDevice, VkPipeline);
VkPipeline currentPipeline(uint64_t token);
void destroyOwnedPipeline(uint64_t token);
void deferPipeline(uint64_t token);
void allocation(uint64_t key, uint64_t bytes, std::string_view name, bool raw = false);
void release(uint64_t key);
void defer(uint64_t key);
template<class Owner> void retiring(Owner& owner) {
    if constexpr(requires { owner.markDeferred(); }) owner.markDeferred();
    else if constexpr(requires { owner->markDeferred(); }) { if(owner) owner->markDeferred(); }
    else if constexpr(requires { owner.begin(); owner.end(); }) { for(auto& item:owner) retiring(item); }
}
bool admit(uint64_t bytes);
uint32_t shadowTier(uint32_t requested, uint64_t bytesPerTexel, uint32_t copies = 1);
std::string ledgerJson();
void panel();
void breadcrumb(std::string_view kind, std::string_view name, uint64_t hash = 0, VkCommandBuffer cb = VK_NULL_HANDLE);
// Crash-path safe: copies preformatted snapshots, never calls the driver or allocates.
void crashSnapshot(const char* folder);
}
#endif
