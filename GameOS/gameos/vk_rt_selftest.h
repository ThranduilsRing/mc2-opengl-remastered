// vk_rt_selftest.h -- MC2_VK_RT_SELFTEST=1 GPU proof of the ray-tracing infrastructure (see vk_rt_selftest.cpp). Test hook, renders nothing.
#ifndef MC2_VK_RT_SELFTEST_H
#define MC2_VK_RT_SELFTEST_H

#include <volk.h>

#include <cstdint>
#include <span>

namespace mc2vk {
class VulkanContext;
namespace rt {
// Builds, traces, updates and compacts a one-triangle scene and logs one [VK_RT_SELFTEST v1] line. True only when every stage matched.
bool runSelfTest(const VulkanContext& ctx);
// Traces rays.size()/8 rays (8 floats each: origin.xyz, tMin, direction.xyz, tMax) against `tlas` through rt_smoke.comp in one waited submit (a Tlas -> Consumer
// barrier first); `out` (>= 4 floats per ray) receives 4 floats per ray (hit, t, primitiveId, instanceCustomIndex). Diagnostic probe for the RT scene; false on any failure.
bool traceRays(const VulkanContext& ctx, VkAccelerationStructureKHR tlas, std::span<const float> rays, std::span<float> out);
}  // namespace rt
}  // namespace mc2vk

#endif
