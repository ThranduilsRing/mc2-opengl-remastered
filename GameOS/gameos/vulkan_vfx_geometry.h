// Native triangle input to the VFX island's shared frame ring.
// The 80-byte record shares the particle SSBO stride; geometry draws use one
// record per vertex rather than the billboard's six procedural vertices.
#pragma once
#include <cstdint>
#include <cstddef>
#include <span>
#include "vulkan_vfx_island.h"

inline constexpr uint32_t kVfxGeometryAtlas = 0xffffffffu;
inline constexpr uint32_t kVfxVegetationAtlas = 0xfffffffeu;
namespace mc2tex { struct TextureArrayCpu; }
#ifdef MC2_VULKAN_ISLAND
void mc2_vk_vfx_vegetation_atlas(const mc2tex::TextureArrayCpu& atlas);
namespace mc2veg { struct CullParams; }
// GPU-culled vegetation draw (vulkan_vfx_island.cpp). False: nothing was submitted this frame.
bool mc2_vk_vfx_vegetation_run(const mc2veg::CullParams& params, const float* worldToClipGL, std::span<const uint32_t> visibility, int w, int h);
#else
inline void mc2_vk_vfx_vegetation_atlas(const mc2tex::TextureArrayCpu&) {}
#endif
// Flags travel in a uint32_t shader field; keep the host constants the same type.
inline constexpr uint32_t VfxGeometryMesh = 4u;
inline constexpr uint32_t VfxGeometryTube = 8u;
inline constexpr uint32_t VfxGeometryUntextured = 32u;
inline constexpr uint32_t VfxGeometryOneOne = 2u;
struct VfxGeometryVertex {
    float position[3]{};
    float pad0{};
    float color[4]{};
    float uv[2]{};
    float pad1{};
    uint32_t flags{};
    float extra[4]{};
    float previousPosition[3]{};
    float previousValid{};
};
static_assert(sizeof(VfxGeometryVertex) == 80);
static_assert(offsetof(VfxGeometryVertex, color) == 16 && offsetof(VfxGeometryVertex, uv) == 32);
static_assert(offsetof(VfxGeometryVertex, flags) == 44 && offsetof(VfxGeometryVertex, extra) == 48);
