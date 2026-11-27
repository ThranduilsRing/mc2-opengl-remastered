// RenderCore/gpu_abi/GpuEnvironment.h
//
// SINGLE SOURCE OF TRUTH for the per-frame mission environment block (std140 uniform,
// TERRAIN_RESOURCE_SET binding 26, vertex + fragment). One producer fills it once per frame
// (GameOS/gameos/mission_environment_gpu.cpp, from the resolved MissionEnvironment); terrain,
// overlays, props and mechs read it instead of carrying their own copies.
//
// Why a separate binding and not the TerrainFrameUBO tail: the terrain slot is a 1024 B
// per-patch dynamic-offset stride with 160 B free, the block is 176 B, and every patch
// slot would duplicate it. Why vertex stages see it: props and mechs light per vertex.
//
// tools/gpu_abi_gen.py emits RenderCore/gpu_abi/GpuEnvironment.layout.gen.h and
// shaders/vulkan/include/environment_ubo_vk.glsl. Regenerate: py -3 tools/gpu_abi_gen.py --write
#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

// @gpu std140 glsl=EnvironmentUBO block=uEnv set=0 set_macro=TERRAIN_RESOURCE_SET binding=26
struct EnvironmentUBO {
    // SH-L2 RADIANCE coefficients in the Y-up Stuff basis of RenderCore/IblShCoeffs.h (see the header:
    // evaluate with the evalShL2 constants, which fold in the diffuse cosine kernel). Night blend and the
    // per-mission L0 chroma are already applied. Strength options stay with the class that owns them.
    float   shDiffuse[9][4];  // @vec4xN .w pad each
    float   hemiSky[4];       // @vec4 mission ambient hemisphere (rgb x unitAmbientScale), the fallback without SH; .w = linear ambient-key ratio for prop SH light floors (1 = no keys)
    float   sceneLight[4];    // @vec4 x = prelit ground scale (mission night scale, 1 = day); yzw = live/authored flat-light ratio applied to prelit art (1 = no sun keys)
    // One sun (S4). Live direction/colours drive shadows, clouds and glint; the authored sun drives the shading of ground, props and mechs.
    // Everything below is exactly identity / 1 with no sun keys, at night and with no mission.
    float   sunDirLive[4];        // @vec4 xyz = gos_GetTerrainLightDir (live, not normalised); w = live flat-light z, clamped 0..1
    float   sunAuthFromLive[3][4]; // @vec4xN columns of the rotation live sun direction -> authored direction; [0].w = 1 when an authored sun record exists
    float   pitchUndo[4];         // @vec4 rgb = authored flat light at the authored elevation / at the live elevation (prop sun term, encoded space)
    float   ambRelight[4];        // @vec4 rgb = flat-light ratio x authored ambient / live ambient (mover ambient)
    float   sunRelight[4];        // @vec4 rgb = live sun colour / authored sun colour (glint and specular colour)
    // Camera light inputs. Colours are encoded bytes / 255, directions are MC2 world space.
    float   sunLive[4];           // @vec4 rgb encoded sun; w reserved
    float   sunLiveDir[4];        // @vec4 xyz live direction; w reserved
    float   ambLive[4];           // @vec4 rgb encoded ambient; w reserved
    float   fog[4];               // @vec4 rgb encoded fog; w start distance
    float   cementSky[4];         // @vec4 rgb cement/overlay sky; w water SKY_AMBIENT
    float   lightingParams[4];    // @vec4 mode (0 Fixed, 1 Live), todWeight (0 Fixed), fog full distance, reserved
    float   nightState[4];        // @vec4 night flag, night factor, mission darkness, reserved
};

#include "GpuEnvironment.layout.gen.h"
