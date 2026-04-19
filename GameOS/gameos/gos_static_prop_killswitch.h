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
