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
    // Tail fields (LOCKSTEP with terrain_tier1.vert / .frag; skirt VS reads visual* only).
    int   _csmPad0;
    int   _csmPad1;
    int   csmEnabled;
    int   csmCount;
    float csmDepthSpan;
    int   csmDebugMode;
    mat4  csmCascadeMatrices[3];
    vec4  csmCascadeTexelWorld;
    float csmFullMapTexelWorld;
    // VK-TERRAIN-VISUALDISPLACE-FAR-1 (rung 5): far-band mode-2 Z-swap params.
    int   visualSide;          // off 580
    float visualDisplaceFar;   // off 584
    int   visualMode;          // off 588
} uT;

layout(set = 0, binding = 1, std430) readonly buffer TerrainHeightBuf {
    float heights[];
} uH;

// VK-TERRAIN-VISUALDISPLACE-FAR-1: 4x visual heightfield bake (see terrain_tier1.vert).
layout(set = 0, binding = 10, std430) readonly buffer TerrainVisualHeightBuf {
    float heightsFine[];
};

layout(location = 0) out vec3 v_worldPos;

const float TERRAIN_DEPTH_FUDGE = 0.0;

void main() {
    int mapX = clamp(uT.blockOriginX + localOffset.x, 0, uT.mapSide - 1);
    int mapY = clamp(uT.blockOriginY + localOffset.y, 0, uT.mapSide - 1);
    float h  = uH.heights[mapX + mapY * uT.mapSide];

    // VK-TERRAIN-VISUALDISPLACE-FAR-1 (rung 5): GL's mode-2 branch runs for skirt TOP
    // verts too (terrain_lod_chunk.vert:156 gates on `isSkirtFlag == 0`, and skirt-strip
    // draws share that shader) so the skirt top tracks the DISPLACED surface while the
    // bottom vert stays at the coarse height minus skirtDepth -- exactly what GL renders
    // for a displaced block's skirt. Same VERBATIM non-stitched core as the main VS.
    if (uT.visualMode == 2 && isSkirtFlag == 0) {
        int u_visualSide = uT.visualSide;
        float u_visualDisplaceFar = uT.visualDisplaceFar;
        float hv;
        int fxV, fyV;
        {
            //PORT-BEGIN:visualDisplaceFar
            fxV = clamp(mapX * 4, 0, u_visualSide - 1);
            fyV = clamp(mapY * 4, 0, u_visualSide - 1);
            hv = heightsFine[fxV + fyV * u_visualSide];
        }
        h = mix(h, hv, clamp(u_visualDisplaceFar, 0.0, 1.0));
        //PORT-END:visualDisplaceFar
    }

    // Phase 6 parity: skirt bottom verts pulled below the surface.
    //PORT-BEGIN:skirtVertMath
    h -= float(isSkirtFlag) * uT.skirtDepth;
    //PORT-END:skirtVertMath

    float worldX = float(mapX) * 128.0 - uT.halfMap;
    float worldY = uT.halfMap - float(mapY) * 128.0;
    v_worldPos = vec3(worldX, worldY, h);

    vec4 clip = uT.worldToClipGL * vec4(worldX, worldY, h, 1.0);
    clip.z += 2.0 * TERRAIN_DEPTH_FUDGE * clip.w;
    gl_Position = clip;
}
