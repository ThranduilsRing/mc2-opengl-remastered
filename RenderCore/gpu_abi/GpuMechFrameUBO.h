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
    int32_t miscI[4];           // @ivec4 specDebugMask, mechFill, boneCount (object-motion tail start; 0 = none), -
    float   environmentAmbient[4]; // @vec4 mission sky RGB; ground is half the sky
    float   prevFromCurClip[16];   // @mat4 previous-frame clip from current clip (camera reprojection), object motion vectors
    // One sun (EnvironmentUBO's tail, copied by the producer): live sun direction -> authored direction rotation (columns) and the
    // authored-sun ratios. Identity / 1 with no keys, at night, with no mission and in the mech lab.
    float   sunAuthFromLive[3][4]; // @vec4xN columns; [0].w = 1 when an authored sun record exists
    float   pitchUndo[4];          // @vec4 rgb
    float   ambRelight[4];         // @vec4 rgb
    float   sunRelight[4];         // @vec4 rgb
    // Dynamic ToD T2: same fill-only tail as EnvironmentUBO; encoded colours, MC2-world directions.
    float   sunLive[4];           // @vec4 rgb encoded sun; w reserved
    float   sunLiveDir[4];        // @vec4 xyz live direction; w reserved
    float   ambLive[4];           // @vec4 rgb encoded ambient; w reserved
    float   sunBake[4];           // @vec4 rgb encoded load-time sun; w reserved
    float   sunBakeDir[4];        // @vec4 xyz load-time direction; w reserved
    float   ambBake[4];           // @vec4 rgb encoded load-time ambient; w reserved
    float   fog[4];               // @vec4 rgb encoded fog; w start distance
    float   cementSky[4];         // @vec4 rgb cement/overlay sky; w water SKY_AMBIENT
    float   edgeHazeSky[4];       // @vec4 rgb EDGE_HAZE_SKY; w reserved
    float   lightingParams[4];    // @vec4 mode (0 Fixed, 1 Live), todWeight (0 Fixed), fog full distance, reserved
};

#include "GpuMechFrameUBO.layout.gen.h"
