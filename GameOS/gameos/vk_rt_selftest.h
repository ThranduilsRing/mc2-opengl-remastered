// vk_rt_selftest.h -- MC2_VK_RT_SELFTEST=1 GPU proof of the ray-tracing infrastructure (see vk_rt_selftest.cpp). Test hook, renders nothing.
#ifndef MC2_VK_RT_SELFTEST_H
#define MC2_VK_RT_SELFTEST_H

#include <volk.h>

#include <cstdint>
#include <span>

#include "prop_array_buckets.h"

namespace mc2vk {
class VulkanContext;
namespace rt {
// Builds, traces, updates and compacts a one-triangle scene and logs one [VK_RT_SELFTEST v1] line. True only when every stage matched.
bool runSelfTest(const VulkanContext& ctx);
// Traces rays.size()/8 rays (8 floats each: origin.xyz, tMin, direction.xyz, tMax) against `tlas` through rt_smoke.comp in one waited submit (a Tlas -> Consumer
// barrier first); `out` (>= 4 floats per ray) receives 4 floats per ray (hit, t, primitiveId, instanceCustomIndex). Diagnostic probe for the RT scene; false on any failure.
bool traceRays(const VulkanContext& ctx, VkAccelerationStructureKHR tlas, std::span<const float> rays, std::span<float> out);
// The RT scene material probe (shaders/vulkan/rt_scene_probe.comp): `out` gets 8 floats per ray ((hit, t, prim, custom), (row, albedo layer, packet, flags)).
// `forceOpaque` commits every candidate (the reference a cutout hole is measured against). Blocking one-shot.
struct SceneProbeInputs {
    VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
    VkBuffer materials = VK_NULL_HANDLE;
    VkBuffer uvIndex = VK_NULL_HANDLE;
    uint64_t vbo = 0;
    uint32_t stride = 0;
    VkImageView alphaOff[mc2::prop_buckets::kMaxBuckets]{};  // the alpha-OFF group's size buckets (bindings 5, 10..12)
    VkImageView alphaOn[mc2::prop_buckets::kMaxBuckets]{};   // the alpha-ON group's size buckets (bindings 6..9)
};
bool traceSceneRays(const VulkanContext& ctx, const SceneProbeInputs& in, std::span<const float> rays, std::span<float> out, bool forceOpaque);
}  // namespace rt
}  // namespace mc2vk

#endif
