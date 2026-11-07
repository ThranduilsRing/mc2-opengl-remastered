// vk_rt_selftest.h -- MC2_VK_RT_SELFTEST=1 GPU proof of the ray-tracing infrastructure (see vk_rt_selftest.cpp). Test hook, renders nothing.
#ifndef MC2_VK_RT_SELFTEST_H
#define MC2_VK_RT_SELFTEST_H

#if 1  // always built (MC2_VULKAN_ISLAND is not a define in this tree)
namespace mc2vk {
class VulkanContext;
namespace rt {
// Builds, traces, updates and compacts a one-triangle scene and logs one [VK_RT_SELFTEST v1] line. True only when every stage matched.
bool runSelfTest(const VulkanContext& ctx);
}  // namespace rt
}  // namespace mc2vk
#endif

#endif
