// VK-TERRAIN-SKIRT (rung 4) — skirt-strip VS, sibling of terrain_tier1.vert.
// =============================================================================
// Reproduces terrain_lod_chunk.vert's skirt path BIT-FOR-BIT for the subset the VK
// island actually owns (LOD0 + coarse non-displaced, non-stitched bands, per the
// TIER1/ALL_LODS capture predicate at gos_terrain_lod_chunk.cpp — s_vkCaptureThisBand
// && !visualDisplaceActive && edgeStitch[i]==0). Because VK-owned patches are NEVER
// stitched or displaced (excluded at capture), the only GL skirt vertex-shader term
// that applies here is the plain pull-down:
//   h -= isSkirtFlag * u_skirtDepth
// (terrain_lod_chunk.vert:196-197, reached only after the displace/stitch branches
// both no-op for a non-displaced non-stitched patch). No stitch-mix / displace-mix
// branches are ported — they cannot fire for any patch this island owns.
//
// Vertex layout mirrors GL's SkirtVertex { int16 lx, ly, isSkirt, _pad } (stride 8):
//   attr0 = ivec2(lx, ly)     (R16G16_SINT, offset 0)
//   attr1 = int  isSkirtFlag  (R16_SINT,    offset 4)
#version 450

layout(location = 0) in ivec2 localOffset;
layout(location = 1) in int   isSkirtFlag;

layout(set = 0, binding = 0, std140) uniform TerrainFrameUBO {
    mat4  worldToClipGL;
    int   mapSide;
    float halfMap;
    int   blockOriginX;
    int   blockOriginY;
    float atlasTopLeftX;
    float atlasTopLeftY;
    float atlasOneOverWorldUnits;
    int   colormapReady;
    int   useCement;
    int   cementGridSide;
    float cementWUPT;
    int   useTransitionMask;
    int   cementDiagConnect;
    int   _lightPad0;
    int   _lightPad1;
    int   _lightPad2;
    vec4  terrainLightDir;
    mat4  staticLightSpaceMatrix;
    int   lightingEnabled;
    int   enableShadows;
    float shadowSoftness;
    int   _lightPad3;
    int   detailEnabled;
    int   detailReady;
    float nfhStrength;
    float detailTiling;
    float detailStrength;
    int   enableDynShadows;
    int   _dynPad0;
    int   _dynPad1;
    mat4  dynamicLightSpaceMatrix;
    vec4  cameraPosMC2;
    int   lodFadeEnabled;
    // VK-TERRAIN-SKIRT: skirt pull-down depth, packed into the existing pad tail
    // (was _pad[43], off 340..511) so the 512B dynamic-alignment slot does NOT grow.
    // LOCKSTEP with vulkan_terrain_island.cpp TerrainFrameUBO.skirtDepth.
    float skirtDepth;   // off 340
} uT;

layout(set = 0, binding = 1, std430) readonly buffer TerrainHeightBuf {
    float heights[];
} uH;

layout(location = 0) out vec3 v_worldPos;

const float TERRAIN_DEPTH_FUDGE = 0.0;

void main() {
    int mapX = clamp(uT.blockOriginX + localOffset.x, 0, uT.mapSide - 1);
    int mapY = clamp(uT.blockOriginY + localOffset.y, 0, uT.mapSide - 1);
    float h  = uH.heights[mapX + mapY * uT.mapSide];

    // Phase 6 parity: skirt bottom verts pulled below the surface.
    h -= float(isSkirtFlag) * uT.skirtDepth;

    float worldX = float(mapX) * 128.0 - uT.halfMap;
    float worldY = uT.halfMap - float(mapY) * 128.0;
    v_worldPos = vec3(worldX, worldY, h);

    vec4 clip = uT.worldToClipGL * vec4(worldX, worldY, h, 1.0);
    clip.z += 2.0 * TERRAIN_DEPTH_FUDGE * clip.w;
    gl_Position = clip;
}
