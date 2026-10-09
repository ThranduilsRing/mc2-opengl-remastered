// RenderCore/gpu_abi/GpuObjectLights.h
//
// SINGLE SOURCE OF TRUTH for the per-object light record (std430, 3600 B at 32
// lights) that the engine uploads into the LightsData SSBO (GL binding 20, copied
// per frame into the Vulkan prop/mech islands). Hand-written; tools/gpu_abi_gen.py
// reads the annotated struct below and emits, committed and byte-deterministic:
//   RenderCore/gpu_abi/GpuObjectLights.layout.gen.h        (C++ sizeof/offsetof asserts,
//                                                           computed from std430 rules)
//   shaders/include/gpu_abi_object_lights.gen.hglsl        (the GLSL struct ObjectLights
//                                                           plus the array-length constants)
// Regenerate: py -3 tools/gpu_abi_gen.py --write      Verify: --check
//
// Consumers: mclib TG_HWLightsData (derives from this record), the GL light SSBO
// uploader, the light-grid builder, and every shader that reads LightsData
// (lighting.hglsl, lightgrid_build.comp, the Vulkan prop and mech vertex stages) via
// the generated .hglsl. The light cap and the record stride are derived from this one
// declaration; widening the cap changes the stride, and a hand-copied cap or stride
// anywhere else silently corrupts every record after the first. Do NOT re-declare the
// struct or the cap anywhere else.
//
// The GLSL members keep their snake_case names (glsl=); numLights_ plus its 3 pad
// lanes are the GLSL ivec4 numLights tail.
#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

// Per-object light cap == the ObjectLights array length. Distinct from the global
// scene light pool size in mclib/tgl.h (MAX_LIGHTS_IN_WORLD, 1024).
constexpr uint32_t kGpuObjectLightsMax = 32;

// @gpu std430 glsl=ObjectLights cpp_align=any
struct GpuObjectLights {
    float   lightToWorld[kGpuObjectLightsMax][16];  // @mat4xN glsl=light_to_world
    float   lightDir[kGpuObjectLightsMax][4];       // @vec4xN glsl=light_dir; w = light type
    float   lightColor[kGpuObjectLightsMax][4];     // @vec4xN glsl=light_color
    float   lightFalloff[kGpuObjectLightsMax][4];   // @vec4xN glsl=light_falloff; x=close y=far z=oneOver
    int32_t numLights_;                             // @ivec4tail glsl=numLights
    int32_t pad[3];                                 // @fold
};

#include "GpuObjectLights.layout.gen.h"
