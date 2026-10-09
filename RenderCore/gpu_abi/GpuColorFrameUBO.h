// RenderCore/gpu_abi/GpuColorFrameUBO.h
//
// SINGLE SOURCE OF TRUTH for the VK static-prop color island's per-frame std140
// uniform block (set 0, binding 0). Hand-written; tools/gpu_abi_gen.py reads the
// annotated struct below and emits, committed and byte-deterministic:
//   RenderCore/gpu_abi/GpuColorFrameUBO.layout.gen.h       (C++ sizeof/offsetof asserts,
//                                                           computed from std140 rules)
//   shaders/vulkan/include/color_frame_ubo_vk.glsl         (the GLSL uniform block `uFrame`)
// Regenerate: py -3 tools/gpu_abi_gen.py --write      Verify: --check
//
// Consumers: GameOS/gameos/vulkan_color_prop_island.cpp (producer) and
// shaders/vulkan/color_prop_tier1.{vert,frag} via the generated include. Do NOT
// re-declare this block anywhere else.
//
// Material-disabled paths never read past the first 20 bytes, so the later
// sections default to zero without changing that path. See tools/gpu_abi_gen.py for
// the annotations (@mat4 / @vec4 / @vec4xN / @cpp_only).
#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

// @gpu std140 glsl=ColorFrameUBO block=uFrame set=0 binding=0
struct ColorFrameUBO {
    float   worldToClipGL[16];  // @mat4 gos_GetObjectFixBMVPMat4(), uploaded GL_FALSE
    float   objectDepthBias;    // reverse-Z surface bias (== OBJECT_DEPTH_BIAS)
    int32_t instBase;
    int32_t materialReady;      // 1 = sample the alpha-OFF albedo array
    int32_t materialReadyOn;    // 1 = sample the alpha-ON albedo array (alpha-test discard)
    float   terrainSunMC2[4];   // @vec4 surface->sun in MC2 world; .w pad
    // Ambient + IBL tail: the real ObjectLights ambient and the SH-L2 feed.
    float   iblSh[9][4];        // @vec4xN SH-L2 coefficients, .w pad each
    float   iblShStrength;      // 0.0 = feature off
    int32_t lightingReady;      // 1 = LightsData SSBO copied this frame -> ambientTerm real
    int32_t glassDither;        // 1 = Bayer alpha-test dither (GL u_glassDither parity)
    float   _pad[1];            // @cpp_only pad to a 16 B multiple
};

#include "GpuColorFrameUBO.layout.gen.h"
