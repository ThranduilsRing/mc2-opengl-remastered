// vk_rt_descriptors.h -- descriptor layout for a ray-query compute pass (acceleration-structure binding + storage buffers) and the shader binding table
// layout of a ray-tracing pipeline.
//
// WHAT: the top half is Vulkan-free (the binding table the smoke shader and any ray-query pass use, and the SBT region math) and is unit-tested without a
// Vulkan link (tests/unit/test_vk_rt_layout.cpp). The bottom half builds the set layout, pipeline layout, pool and set for that table and writes a TLAS into it.
// WHY here and not in ComputePipeline: that class takes buffer bindings only; an acceleration-structure binding has its own descriptor type and a
// VkWriteDescriptorSetAccelerationStructureKHR write.
#ifndef MC2_VK_RT_DESCRIPTORS_H
#define MC2_VK_RT_DESCRIPTORS_H

#include <array>
#include <cstdint>
#include <span>

namespace mc2vk::rt {

enum class BindingKind : uint8_t { AccelerationStructure, StorageBuffer, SampledImage };   // SampledImage: a combined image sampler

struct BindingDesc {
    uint32_t binding;
    BindingKind kind;
    const char* name;
};

// shaders/vulkan/rt_smoke.comp: set 0. A ray-query pass puts the TLAS at binding 0 and its buffers after it.
inline constexpr std::array<BindingDesc, 3> kSmokeBindings = {{
    {0, BindingKind::AccelerationStructure, "tlas"},
    {1, BindingKind::StorageBuffer, "rays"},       // in: 2 x vec4 per ray (origin.xyz + tMin, direction.xyz + tMax)
    {2, BindingKind::StorageBuffer, "results"},    // out: 1 x vec4 per ray (x = hit flag, y = t, z = primitive id, w = instance custom index)
}};

// shaders/vulkan/rt_scene_probe.comp: the RT scene material probe (vk_rt_scene.cpp, MC2_VK_RT_SELFTEST). Push constants: kSceneProbePushBytes.
inline constexpr std::array<BindingDesc, 13> kSceneProbeBindings = {{
    {0, BindingKind::AccelerationStructure, "tlas"},
    {1, BindingKind::StorageBuffer, "rays"},       // in: as kSmokeBindings
    {2, BindingKind::StorageBuffer, "results"},    // out: 2 x vec4 per ray ((hit, t, primitive id, custom index), (material row, albedo layer, packet / tile, flags))
    {3, BindingKind::StorageBuffer, "materials"},  // scene::MaterialRow[] (32 bytes each)
    {4, BindingKind::StorageBuffer, "uv_index"},   // uint[]: the prop geometries' rebased indices (MaterialRow::firstIndex addresses it)
    {5, BindingKind::SampledImage, "alpha_off"},   // the colour-prop island's alpha-OFF size bucket 0 (buckets 1..3 on 10..12)
    {6, BindingKind::SampledImage, "alpha_on"},    // its alpha-ON size buckets 0..3 (PROP-ARRAY-BUCKETS; the layer names the bucket)
    {7, BindingKind::SampledImage, "alpha_on1"},
    {8, BindingKind::SampledImage, "alpha_on2"},
    {9, BindingKind::SampledImage, "alpha_on3"},
    {10, BindingKind::SampledImage, "alpha_off1"},
    {11, BindingKind::SampledImage, "alpha_off2"},
    {12, BindingKind::SampledImage, "alpha_off3"},
}};
// vbo address (uvec2), vertex stride, mode (0 alpha-tested cutouts, 1 every candidate opaque).
inline constexpr uint32_t kSceneProbePushBytes = 16;

constexpr bool bindingsAreValid(const BindingDesc* table, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i)
        for (uint32_t j = i + 1; j < count; ++j)
            if (table[i].binding == table[j].binding) return false;
    return count > 0 && table[0].binding == 0 && table[0].kind == BindingKind::AccelerationStructure;
}

constexpr uint64_t alignUpTo(uint64_t v, uint64_t a) { return a > 1 ? (v + a - 1) / a * a : v; }

struct SbtRegion {
    uint64_t offset = 0;                             // bytes from the start of the SBT buffer (a multiple of shaderGroupBaseAlignment)
    uint64_t stride = 0;
    uint64_t size = 0;
};

struct SbtLayout {
    bool valid = false;
    SbtRegion raygen, miss, hit, callable;
    uint64_t totalSize = 0;
};

// Lays out a shader binding table: one raygen record, then `missCount`, `hitCount` and `callableCount` records. Each region starts on
// shaderGroupBaseAlignment; the raygen region's stride equals its size (spec), miss / hit / callable strides are the handle size rounded up to
// shaderGroupHandleAlignment. Invalid when a limit is zero, an alignment is not a power of two, or a stride exceeds maxShaderGroupStride (0 = unchecked).
constexpr SbtLayout computeSbtLayout(uint32_t handleSize, uint32_t handleAlignment, uint32_t baseAlignment, uint32_t missCount, uint32_t hitCount,
                                     uint32_t callableCount, uint32_t maxStride = 0) {
    SbtLayout l;
    auto pow2 = [](uint32_t v) { return v != 0 && (v & (v - 1)) == 0; };
    if (handleSize == 0 || !pow2(handleAlignment) || !pow2(baseAlignment)) return l;
    const uint64_t stride = alignUpTo(handleSize, handleAlignment);
    if (maxStride != 0 && stride > maxStride) return l;
    uint64_t cursor = 0;
    l.raygen = {cursor, alignUpTo(stride, baseAlignment), alignUpTo(stride, baseAlignment)};
    cursor += l.raygen.size;
    auto place = [&](SbtRegion& r, uint32_t count) {
        if (count == 0) return;
        cursor = alignUpTo(cursor, baseAlignment);
        r = {cursor, stride, stride * count};
        cursor += r.size;
    };
    place(l.miss, missCount);
    place(l.hit, hitCount);
    place(l.callable, callableCount);
    l.totalSize = cursor;
    l.valid = true;
    return l;
}

}  // namespace mc2vk::rt


#ifndef MC2_RT_PURE_ONLY  // the unit tests include only the Vulkan-free half above

#include <volk.h>

#include "vk_owned.h"

namespace mc2vk {
class VulkanContext;
}

namespace mc2vk::rt {

// Set layout + pipeline layout (+ `pushBytes` of compute push constants) + one descriptor set for a binding table (kSmokeBindings by default),
// compute-stage visible.
class RayQuerySet {
public:
    bool create(const VulkanContext& ctx, std::span<const BindingDesc> table = kSmokeBindings, uint32_t pushBytes = 0);
    // Writes the TLAS and the two storage buffers (whole range) into the set (kSmokeBindings).
    void write(VkDevice device, VkAccelerationStructureKHR tlas, VkBuffer rays, VkBuffer results) const;
    void writeTlas(VkDevice device, uint32_t binding, VkAccelerationStructureKHR tlas) const;
    void writeBuffer(VkDevice device, uint32_t binding, VkBuffer buffer) const;
    void writeImage(VkDevice device, uint32_t binding, VkSampler sampler, VkImageView view) const;   // SHADER_READ_ONLY_OPTIMAL
    VkDescriptorSetLayout setLayout() const noexcept { return dsl_.get(); }
    VkPipelineLayout pipelineLayout() const noexcept { return layout_.get(); }
    VkDescriptorSet set() const noexcept { return set_; }
    explicit operator bool() const noexcept { return set_ != VK_NULL_HANDLE; }

private:
    OwnedDescriptorSetLayout dsl_;
    OwnedPipelineLayout layout_;
    OwnedDescriptorPool pool_;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
};

}  // namespace mc2vk::rt

#endif  // MC2_RT_PURE_ONLY
#endif  // MC2_VK_RT_DESCRIPTORS_H
