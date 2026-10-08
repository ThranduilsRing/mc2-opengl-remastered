// VK-PARITY-OVERLAY-1: decal pass shares the overlay VS (GL: both batches use
// shaders/terrain_overlay.vert); separate basename only so the SPIR-V bake loop emits
// terrain_decal_tier1.vert.spv. Body: include/terrain_overlay_vs.glsl.
#version 450
#extension GL_GOOGLE_include_directive : require
#include "include/terrain_overlay_vs.glsl"
