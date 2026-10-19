// Native triangle input to the VFX island's shared frame ring.
// The 64-byte record shares the particle SSBO stride; geometry draws use one
// record per vertex rather than the billboard's six procedural vertices.
#pragma once
#include <cstdint>
#include <cstddef>
#include "vulkan_vfx_island.h"

inline constexpr uint32_t kVfxGeometryAtlas = 0xffffffffu;
inline constexpr uint32_t kVfxVegetationAtlas = 0xfffffffeu;
struct GosTexels;
#ifdef MC2_VULKAN_ISLAND
void mc2_vk_vfx_vegetation_atlas(const GosTexels& atlas);
#else
inline void mc2_vk_vfx_vegetation_atlas(const GosTexels&) {}
#endif
enum VfxGeometryFlags : uint32_t {
    VfxGeometryMesh = 4u,
    VfxGeometryTube = 8u,
    VfxGeometryUntextured = 32u,
    VfxGeometryOneOne = 2u,
};
struct VfxGeometryVertex {
    float position[3]{};
    float pad0{};
    float color[4]{};
    float uv[2]{};
    float pad1{};
    uint32_t flags{};
    float extra[4]{};
};
static_assert(sizeof(VfxGeometryVertex) == 64);
static_assert(offsetof(VfxGeometryVertex, color) == 16 && offsetof(VfxGeometryVertex, uv) == 32);
static_assert(offsetof(VfxGeometryVertex, flags) == 44 && offsetof(VfxGeometryVertex, extra) == 48);
