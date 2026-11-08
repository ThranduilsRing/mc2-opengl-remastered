// vk_rt_selftest.h -- MC2_VK_RT_SELFTEST=1 GPU proof of the ray-tracing infrastructure (see vk_rt_selftest.cpp). Test hook, renders nothing.
#ifndef MC2_VK_RT_SELFTEST_H
#define MC2_VK_RT_SELFTEST_H

#include <volk.h>

#include <cstdint>

namespace mc2vk {
class VulkanContext;
namespace rt {
// Builds, traces, updates and compacts a one-triangle scene and logs one [VK_RT_SELFTEST v1] line. True only when every stage matched.
bool runSelfTest(const VulkanContext& ctx);
// Traces `count` rays (8 floats each: origin.xyz, tMin, direction.xyz, tMax) against `tlas` through rt_smoke.comp in one waited submit (a Tlas -> Consumer
// barrier first); `out` receives 4 floats per ray (hit, t, primitiveId, instanceCustomIndex). Diagnostic probe for the RT scene; false on any failure.
bool traceRays(const VulkanContext& ctx, VkAccelerationStructureKHR tlas, const float* rays, uint32_t count, float* out);
}  // namespace rt
}  // namespace mc2vk

#endif
