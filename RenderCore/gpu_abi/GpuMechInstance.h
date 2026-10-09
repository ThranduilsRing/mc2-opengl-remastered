// RenderCore/gpu_abi/GpuMechInstance.h
//
// SINGLE SOURCE OF TRUTH for the per-instance mech SSBO record (64 B, std430).
// Hand-written; tools/gpu_abi_gen.py reads the annotated struct below and emits,
// committed and byte-deterministic:
//   RenderCore/gpu_abi/GpuMechInstance.layout.gen.h        (C++ sizeof/offsetof asserts,
//                                                           computed from std430 rules)
//   shaders/include/gpu_abi_mech_instance.gen.hglsl        (the GLSL struct)
// Regenerate: py -3 tools/gpu_abi_gen.py --write      Verify: --check
//
// Consumers: GameOS mech batcher (producer) and the three shaders that stride
// instances[] -- shaders/mech.vert, shaders/shadow_mech.vert and
// shaders/vulkan/mech_tier1.vert -- via the generated .hglsl. A shader-side copy
// that drifts from this record strides every bucket with instanceBase > 0 onto a
// shifted record (garbage bone offsets, collapsed mech parts), so no shader may
// re-declare this struct. Do NOT re-declare it anywhere else.
#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

// @gpu std430 glsl=GpuMechInstance
struct alignas(16) GpuMechInstance {
    uint32_t typeLodRecordIndex;  // resolved Mech3DAppearanceType x LOD record index
    uint32_t baseBoneOffset;      // index into per-frame bone SSBO for this actor's nodes
    uint32_t lightDataIndex;      // into LightsData UBO
    uint32_t renderFlags;         // bit 0: ALPHA_TEST, bit 1: lightsOut, bit 2: isHighlighted
    float    aRGBHighlight[4];    // @vec4 forwarded to FS as v_highlightColor
    float    fogRGB[4];           // @vec4 forwarded to FS as v_fogRGB; .a = per-actor haze factor
    uint32_t objectIdRaw;         // RenderObjectHandle.raw() for the object-id buffer; 0 = invalid
    uint32_t materialIdx;         // index into s_mechMaterialTable; 0 = not assigned
    float    visualDamage01;      // [0,1] composite damage, for ViewMode consumers
    uint32_t visualFlags;         // RenderCore::MechVisualFlagBits
};

#include "GpuMechInstance.layout.gen.h"
