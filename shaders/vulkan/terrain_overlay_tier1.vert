// VK-PARITY-OVERLAY-1 TERRAIN OVERLAY / DECAL TIER-1 -- shared VS.
// =============================================================================
// Port of shaders/terrain_overlay.vert (the VS GL uses for BOTH the cement/road
// terrain-overlay batch and the crater/footprint decal batch). WorldOverlayVert in
// (28B: wx,wy,wz, u,v, fog, argb BGRA), clip = worldToClipGL * world, then the same
// reverse-Z depth bias (distance-proportional OVERLAY_DEPTH_BIAS_REL, or the legacy
// constant NDC offset behind biasLegacy) and the same behind-camera / past-far reject.
// worldToClipGL comes from the terrain island's dynamic UBO slot 0 (set 0 binding 0 --
// only the leading mat4 is declared here; the block is a prefix of TerrainFrameUBO).
#version 450
#extension GL_GOOGLE_include_directive : require
#include "include/terrain_overlay_vs.glsl"
