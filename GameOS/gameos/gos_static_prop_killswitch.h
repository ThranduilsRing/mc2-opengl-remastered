#pragma once

#include <cstdint>

// Global runtime toggle for the GPU static-prop renderer.
// Default false — old CPU path is active until validated.
// Toggled at runtime via RAlt+0 (see gameosmain.cpp).
extern bool g_useGpuStaticProps;

// Resolve a gosTextureHandle (MC2 opaque ID) to the underlying raw GL
// texture name. Returns 0 if the handle is INVALID_TEXTURE_ID or the
// underlying gosTexture is gone. Implemented in gameos_graphics.cpp where
// the gosRenderer class is visible.
uint32_t gos_GetGLTextureId(uint32_t gosHandle);

// Accessors for the terrain projection chain uniforms used by the static
// prop shader. These replicate the terrain/overlay projection pattern
// (see shaders/terrain_overlay.vert). Pointers into a float16 / float4
// buffer; the batcher uploads them unchanged each flush.
const float* gos_GetTerrainViewportVec4();   // (vmx, vmy, vax, vay)
const float* gos_GetProj2ScreenMat4();       // screen-pixel -> NDC (upload GL_TRUE)
const float* gos_GetTerrainMVPMat4();        // axisSwap * worldToClip (upload GL_FALSE)

// Debug-mode cycle for the GPU static prop fragment shader (RAlt+9).
// Modes: 0=normal, 1=addr-gradient, 2=addr-hash, 3=white, 4=argb-only, 5=tex-only.
// Implemented in gos_static_prop_batcher.cpp.
void gos_GpuPropsCycleDebugMode();
int  gos_GpuPropsGetDebugMode();
