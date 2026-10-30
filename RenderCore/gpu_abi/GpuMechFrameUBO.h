// RenderCore/gpu_abi/GpuMechFrameUBO.h
//
// SINGLE SOURCE OF TRUTH for the VK mech island's per-frame std140 uniform block
// (set 0, binding 0). Hand-written; tools/gpu_abi_gen.py reads the annotated struct
// below and emits, committed and byte-deterministic:
//   RenderCore/gpu_abi/GpuMechFrameUBO.layout.gen.h        (C++ sizeof/offsetof asserts,
//                                                           computed from std140 rules)
//   shaders/vulkan/include/mech_frame_ubo_vk.glsl          (the GLSL uniform block `uFrame`)
// Regenerate: py -3 tools/gpu_abi_gen.py --write      Verify: --check
//
// Consumers: GameOS/gameos/vulkan_mech_island.cpp (producer) and
// shaders/vulkan/mech_tier1.{vert,frag} via the generated include. Do NOT
// re-declare this block anywhere else.
#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

// @gpu std140 glsl=MechFrameUBO block=uFrame set=0 binding=0
struct MechFrameUBO {
    float   worldToClipGL[16];  // @mat4 gos_GetMechFixBMVPMat4(), uploaded GL_FALSE
    float   objectDepthBias;    // OBJECT_DEPTH_BIAS
    int32_t materialReady;      // 1 = sample the copy-once albedo
    int32_t lightingReady;      // 1 = LightsData copied -> calc_light valid; else flat white
    int32_t sunAxisMc2Fix;      // options.cfg SunAxisFix: 1 = diffuse sun direction takes the Stuff->MC2 swap (GL u_sunAxisMc2Fix)
    float   fogColorRGB[4];     // @vec4 g_scene.fogColor.rgb; .a unused
    // Default mech fragment shading block (the vertex stage ignores it).
    float   cameraWorldPos[4];  // @vec4 GL world (ViewUniforms.cameraWorldPos)
    float   shadeA[4];          // @vec4 ambient, specular, metalRough, glassRough
    float   shadeB[4];          // @vec4 lumaThresh, maxChanThresh, importedGamma, importedAlbedoScale
    float   shadeC[4];          // @vec4 aoStrength, normalStrength, -, -
    float   poolF[4];           // @vec4 moverDim, moverScale, objScale, -
    int32_t poolI[4];           // @ivec4 numPoolLights, soft, moverMax, compress
    int32_t miscI[4];           // @ivec4 specDebugMask, -, -, -
    float   environmentAmbient[4]; // @vec4 mission sky RGB; ground is half the sky
    float   previousWorldToClipGL[16]; // @mat4 unjittered N-1 main-view projection
};

#include "GpuMechFrameUBO.layout.gen.h"
