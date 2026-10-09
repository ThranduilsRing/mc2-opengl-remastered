// RenderCore/gpu_abi/GpuLegacyFrameUBO.h
//
// SINGLE SOURCE OF TRUTH for the VK legacy-shape island's per-frame std140 uniform
// block (set 0, binding 0). Hand-written; tools/gpu_abi_gen.py reads the annotated
// struct below and emits, committed and byte-deterministic:
//   RenderCore/gpu_abi/GpuLegacyFrameUBO.layout.gen.h      (C++ sizeof/offsetof asserts,
//                                                           computed from std140 rules)
//   shaders/vulkan/include/legacy_frame_ubo_vk.glsl        (the GLSL uniform block `uFrame`)
// Regenerate: py -3 tools/gpu_abi_gen.py --write      Verify: --check
//
// Consumers: GameOS/gameos/vulkan_legacy_shape_island.cpp (producer) and
// shaders/vulkan/legacy_shape_tier1.{vert,frag} via the generated include. Do NOT
// re-declare this block anywhere else.
#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

// @gpu std140 glsl=LegacyFrameUBO block=uFrame set=0 binding=0
struct LegacyFrameUBO {
    float proj[16];      // @mat4 gos_GetProj2ScreenMat4(): 4 consecutive rows (row-major); the
                         // vertex stage transposes the column-major load back to the row form
    float fogColor[4];   // @vec4 gosRenderer fog_color_ (0 = no fog)
};

#include "GpuLegacyFrameUBO.layout.gen.h"
