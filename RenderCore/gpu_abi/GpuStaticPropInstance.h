// RenderCore/gpu_abi/GpuStaticPropInstance.h
//
// SINGLE SOURCE OF TRUTH for the per-instance static-prop SSBO record (112 B,
// std430). Hand-written; tools/gpu_abi_gen.py reads the annotated struct below
// and emits, committed and byte-deterministic:
//   RenderCore/gpu_abi/GpuStaticPropInstance.layout.gen.h   (C++ sizeof/offsetof asserts,
//                                                            computed from std430 rules)
//   shaders/include/gpu_abi_static_prop_instance.gen.hglsl  (the GLSL struct)
// Regenerate: py -3 tools/gpu_abi_gen.py --write      Verify: --check
//
// Consumers: GameOS static_prop batcher (producer), Vulkan color/shadow prop
// islands (memcpy'd feed), RenderCore::StaticPropInstanceDesc (alias), and the
// GL/VK shaders static_prop.vert, shadow_static_prop.vert,
// vulkan/color_prop_tier1.vert, vulkan/shadow_prop.vert (via the generated
// .hglsl). Do NOT re-declare this struct anywhere else.
//
// Restricted annotation subset (the generator fails loudly on anything else):
// fixed-width scalars and float[N]; no ctors, methods or bitfields.
//   @mat4  on float[16]: GLSL mat4. C++ holds it row-major, uploaded GL_FALSE and
//          loaded as a column-major mat4; shaders use the row-vector form `v * M`.
//   @vec4  on float[4]:  GLSL vec4.
#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

// @gpu std430 glsl=GpuStaticPropInstance
struct alignas(16) GpuStaticPropInstance {
    float    modelMatrix[16];   // @mat4 shape-to-world, row-major (uploaded GL_FALSE)
    uint32_t typeID;
    uint32_t firstColorOffset;  // into per-frame color SSBO
    uint32_t flags;             // bit 0: lightsOut, bit 1: isWindow, bit 2: isSpotlight
    uint32_t lightDataIndex;    // index into the per-frame light-data UBO populated by
                                // mc2::vkLights::addLightDataStructure(); writer is
                                // submitMultiShape (was _pad0 at offset 76).
    float    aRGBHighlight[4];  // @vec4
    float    fogRGB[4];         // @vec4
};

#include "GpuStaticPropInstance.layout.gen.h"
