// VK-P10 TERRAIN TIER-1 — LOD0 chunk main-patch VS (Vulkan port of the LIVE
// terrain_lod_chunk.vert COARSE path, LOD0/no-stitch/no-displace/no-skirt only).
// =============================================================================
// Reproduces terrain_lod_chunk.vert's plain-coarse branch (the `u_visualDisplace==0`,
// `u_edgeStitch==0`, `isSkirtFlag==0` path) BIT-FOR-BIT:
//   mapX = clamp(blockOriginX + localOffset.x, 0, mapSide-1)
//   mapY = clamp(blockOriginY + localOffset.y, 0, mapSide-1)
//   h    = heights[mapX + mapY*mapSide]
//   worldX =  mapX*128 - halfMap ;  worldY = halfMap - mapY*128
//   clip = worldToClipGL * vec4(worldX, worldY, h, 1)
//   clip.z += 2*TERRAIN_DEPTH_FUDGE*clip.w   (FUDGE=0 -> no-op, kept for lockstep)
//
// The probe is LOD0 MAIN-TRIANGLE ONLY. Skirts, edge-stitch, and visual-displace
// are DEFERRED (the C++ feed only ever emits lodStep==1 non-stitched patches), so
// none of terrain_lod_chunk.vert's stitch/displace branches are ported.
//
// Height SSBO is binding 0 here (a VK-owned snapshot of the GL height SSBO copied
// via interop; see vulkan_terrain_island.cpp). The GL shader used std430 binding 23
// — the value read is identical (float[mapSide*mapSide], row-major).
#version 450

// Attribute 0: ivec2 localOffset (int16 lx,ly), integer format (mirrors the GL VAO
// glVertexAttribIPointer(0, 2, GL_SHORT, stride=4, 0)). VK feeds R16G16_SINT.
layout(location = 0) in ivec2 localOffset;

layout(set = 0, binding = 0, std140) uniform TerrainFrameUBO {
    mat4  worldToClipGL;   // gos_GetTerrainMVPMat4() / dispatch MVP — GL_FALSE (col-major load)
    int   mapSide;
    float halfMap;
    // Per-patch placement (updated per LOD0 patch draw via dynamic UBO offset).
    int   blockOriginX;
    int   blockOriginY;
    // COLORMAP-BASE sub-slice: atlas-UV params (used by the FS; declared here to
    // keep the std140 UBO layout LOCKSTEP with terrain_tier1.frag).
    float atlasTopLeftX;
    float atlasTopLeftY;
    float atlasOneOverWorldUnits;
    int   colormapReady;
    // CEMENT/ROAD sub-slice: cement uniforms (used by the FS; declared here to keep the
    // std140 UBO layout LOCKSTEP with terrain_tier1.frag).
    int   useCement;
    int   cementGridSide;
    float cementWUPT;
    int   useTransitionMask;
    int   cementDiagConnect;
    // VK-TERRAIN-LIGHTING sub-slice (used by the FS; declared here to keep the std140
    // UBO layout LOCKSTEP with terrain_tier1.frag).
    int   _lightPad0;
    int   _lightPad1;
    int   _lightPad2;
    vec4  terrainLightDir;
    mat4  staticLightSpaceMatrix;
    int   lightingEnabled;
    int   enableShadows;
    float shadowSoftness;
    int   _lightPad3;
    // VK-TERRAIN-DETAIL sub-slice (used by the FS; declared here to keep the std140
    // UBO layout LOCKSTEP with terrain_tier1.frag).
    int   detailEnabled;
    int   detailReady;
    float nfhStrength;
    float detailTiling;
    float detailStrength;
} uT;

// Height SSBO (VK-owned snapshot of the GL height field, binding 23 semantics).
layout(set = 0, binding = 1, std430) readonly buffer TerrainHeightBuf {
    float heights[];
} uH;

layout(location = 0) out vec3 v_worldPos;

const float TERRAIN_DEPTH_FUDGE = 0.0;  // lockstep with terrain_lod_chunk.vert

void main() {
    int mapX = clamp(uT.blockOriginX + localOffset.x, 0, uT.mapSide - 1);
    int mapY = clamp(uT.blockOriginY + localOffset.y, 0, uT.mapSide - 1);
    float h  = uH.heights[mapX + mapY * uT.mapSide];

    float worldX = float(mapX) * 128.0 - uT.halfMap;
    float worldY = uT.halfMap - float(mapY) * 128.0;
    v_worldPos = vec3(worldX, worldY, h);

    vec4 clip = uT.worldToClipGL * vec4(worldX, worldY, h, 1.0);
    clip.z += 2.0 * TERRAIN_DEPTH_FUDGE * clip.w;  // FUDGE=0 -> no-op
    gl_Position = clip;
}
