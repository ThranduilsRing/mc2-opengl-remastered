// terrain_frame_vk.glsl -- VK-PARITY-OVERLAY-1: the terrain island's set-0 resource
// declarations + lighting/shadow helpers, shared by the VK terrain OVERLAY / DECAL
// fragment shaders (terrain_overlay_tier1.frag, terrain_decal_tier1.frag). The overlay
// pass reuses the terrain island's descriptor set 0 (same TerrainFrameUBO slot layout,
// cement atlas/words, light pool, static/dynamic/CSM shadow maps) so overlays see the
// SAME lighting inputs as the terrain they sit on.
// The fragment-side set-0 resources and shadow helpers live here; the UBO block itself is in
// terrain_frame_ubo_vk.glsl so vertex stages share the same declaration.
// The INCLUDER must enable GL_GOOGLE_include_directive and define PREC.
#ifndef TERRAIN_FRAME_VK_GLSL
#define TERRAIN_FRAME_VK_GLSL
#include "lighting_vk.glsl"

#include "terrain_frame_ubo_vk.glsl"
#include "mover_shadow.glsl"

layout(set = TERRAIN_RESOURCE_SET, binding = 2) uniform sampler2D u_colormap;
// CEMENT/ROAD: the VK-owned copy-once import of the GL cement atlas (REPEAT sampler) +
// the map-load snapshot of the GL per-vertex cement-word SSBO (GL binding 25).
layout(set = TERRAIN_RESOURCE_SET, binding = 3) uniform sampler2D u_cementAtlas;
layout(set = TERRAIN_RESOURCE_SET, binding = 4, std430) readonly buffer TerrainCementBuf {
    uint cementWordsF[];
} uC;

// VK-PARITY-TERRAIN-2: the GL terrain light pool (GL binding 28) + world-XY light grid
// (GL bindings 29/30). Per-parity host-visible copies of the SAME CPU arrays the GL
// producer (gos_terrain_lighting.cpp PackAndDispatch / BuildAndUploadLightGrid) uploads
// each frame. Member names match the GL frag so the shared include body compiles as-is.
struct PoolLight {
    vec3  position;        // MC2 world (same frame as v_worldPos)
    uint  lightType;       // 1=point 2=spot 3=terrain, 0=skip
    vec3  color;           // R/G/B normalized 0..1
    float closeDistance;
    float farDistance;
    float oneOverDistance;
    float _plPad1;         // >0.5 = mover-attached (producer flag)
    float intensity;
};
layout(set = TERRAIN_RESOURCE_SET, binding = 11, std430) readonly buffer TerrainLightPoolVK {
    PoolLight poolLights[];
};
layout(set = TERRAIN_RESOURCE_SET, binding = 12, std430) readonly buffer TerrainLightGridHeaderVK {
    uvec2 lightGridCells[];   // {firstIndex, count} per cell, row-major y*dim + x
};
layout(set = TERRAIN_RESOURCE_SET, binding = 13, std430) readonly buffer TerrainLightGridIndicesVK {
    uint  lightGridIndices[];
};
// LOCAL-LIGHTS-2: local-light shadow atlas (shadow island; D16 depth, plain sampler + gather).
layout(set = TERRAIN_RESOURCE_SET, binding = 19) uniform sampler2D u_localShadowAtlas;
#define MC2_LOCAL_SHADOW_ATLAS 1
#define TLP_CEMENT_WORD(i) uC.cementWordsF[i]
#include "../../include/terrain_light_pool.hglsl"
#ifndef TERRAIN_OBJECT_RECEIVER
#include "ground_shadow.glsl"
#endif
#endif
