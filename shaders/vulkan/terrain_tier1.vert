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
    // Tail fields (declared to keep std140 offsets LOCKSTEP with the C++ mirror and
    // terrain_tier1.frag; the VS only reads the visual* trio below).
    int   enableDynShadows;
    int   _dynPad0;
    int   _dynPad1;
    mat4  dynamicLightSpaceMatrix;
    vec4  cameraPosMC2;
    int   lodFadeEnabled;
    float skirtDepth;
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
    int   visualSide;          // off 580: fine bake side V (0 = not snapshotted)
    float visualDisplaceFar;   // off 584: MC2_TERRAIN_VISUAL_DISPLACE_FAR fade 0..1
    int   visualMode;          // off 588: 0 = no displace, 2 = far-band Z-swap
    int   edgeStitch;          // off 592: packed N/S/W/E coarser-neighbor stride bytes
    int   quadCountX;          // off 596: max local offset X for edge tests
    int   quadCountY;          // off 600: max local offset Y for edge tests
} uT;

// Height SSBO (VK-owned snapshot of the GL height field, binding 23 semantics).
layout(set = 0, binding = 1, std430) readonly buffer TerrainHeightBuf {
    float heights[];
} uH;

// VK-TERRAIN-VISUALDISPLACE-FAR-1: 4x visual heightfield bake (VK-owned snapshot of
// the GL binding-26 SSBO). Anonymous instance so the ported block can reference
// `heightsFine` textually identical to terrain_lod_chunk.vert.
layout(set = 0, binding = 10, std430) readonly buffer TerrainVisualHeightBuf {
    float heightsFine[];
};

layout(location = 0) out vec3 v_worldPos;

const float TERRAIN_DEPTH_FUDGE = 0.0;  // LOCKSTEP-MIRROR: terrain_depth_bias.hglsl (lockstep with terrain_lod_chunk.vert)

float sampleH(int mx, int my) {
    mx = clamp(mx, 0, uT.mapSide - 1);
    my = clamp(my, 0, uT.mapSide - 1);
    return uH.heights[mx + my * uT.mapSide];
}

void main() {
    int mapX = clamp(uT.blockOriginX + localOffset.x, 0, uT.mapSide - 1);
    int mapY = clamp(uT.blockOriginY + localOffset.y, 0, uT.mapSide - 1);
    float h  = sampleH(mapX, mapY);

    // VK-TERRAIN-EDGE-STITCH-1: VERBATIM port of terrain_lod_chunk.vert's coarse
    // edge-stitch block for main-patch verts (isSkirtFlag==0 here).
    if (uT.edgeStitch != 0) {
        int  Sc = 0, along = 0;
        bool alongX = true;
        if      (localOffset.y == 0             && ((uT.edgeStitch)       & 0xFF) > 0) { Sc = (uT.edgeStitch)       & 0xFF; along = localOffset.x; alongX = true;  }
        else if (localOffset.y == uT.quadCountY && ((uT.edgeStitch >> 8)  & 0xFF) > 0) { Sc = (uT.edgeStitch >> 8)  & 0xFF; along = localOffset.x; alongX = true;  }
        else if (localOffset.x == 0             && ((uT.edgeStitch >> 16) & 0xFF) > 0) { Sc = (uT.edgeStitch >> 16) & 0xFF; along = localOffset.y; alongX = false; }
        else if (localOffset.x == uT.quadCountX && ((uT.edgeStitch >> 24) & 0xFF) > 0) { Sc = (uT.edgeStitch >> 24) & 0xFF; along = localOffset.y; alongX = false; }
        if (Sc > 0 && (along % Sc) != 0) {
            int   c0 = (along / Sc) * Sc;
            int   c1 = c0 + Sc;
            float t  = float(along - c0) / float(Sc);
            float h0, h1;
            if (alongX) {
                int fy = uT.blockOriginY + localOffset.y;
                h0 = sampleH(uT.blockOriginX + c0, fy);
                h1 = sampleH(uT.blockOriginX + c1, fy);
            } else {
                int fx = uT.blockOriginX + localOffset.x;
                h0 = sampleH(fx, uT.blockOriginY + c0);
                h1 = sampleH(fx, uT.blockOriginY + c1);
            }
            h = mix(h0, h1, t);
        }
    }

    // VK-TERRAIN-VISUALDISPLACE-FAR-1 (rung 5): coarser-band (LOD1+) far-band visual
    // displacement -- pure Z-swap at the SAME vertex density (VERBATIM port of the GL
    // terrain_lod_chunk.vert `u_visualDisplace == 2` non-stitched core). The stitched-
    // edge re-derivation is NOT ported: the capture predicate excludes edge-stitched
    // patches (they stay GL-owned), so it can never fire for a VK-owned patch. Skirt
    // handling matches GL structurally: this is the main-patch VS (isSkirtFlag==0
    // always); the skirt VS ports the same block for its top verts. Local u_* aliases
    // keep the ported lines textually identical to the GL source.
    if (uT.visualMode == 2) {
        int u_visualSide = uT.visualSide;
        float u_visualDisplaceFar = uT.visualDisplaceFar;
        float hv;
        int fxV, fyV;
        int  Sc2 = 0, along2 = 0; bool alongX2 = true;
        if      (localOffset.y == 0             && ((uT.edgeStitch)       & 0xFF) > 0) { Sc2 = (uT.edgeStitch)       & 0xFF; along2 = localOffset.x; alongX2 = true;  }
        else if (localOffset.y == uT.quadCountY && ((uT.edgeStitch >> 8)  & 0xFF) > 0) { Sc2 = (uT.edgeStitch >> 8)  & 0xFF; along2 = localOffset.x; alongX2 = true;  }
        else if (localOffset.x == 0             && ((uT.edgeStitch >> 16) & 0xFF) > 0) { Sc2 = (uT.edgeStitch >> 16) & 0xFF; along2 = localOffset.y; alongX2 = false; }
        else if (localOffset.x == uT.quadCountX && ((uT.edgeStitch >> 24) & 0xFF) > 0) { Sc2 = (uT.edgeStitch >> 24) & 0xFF; along2 = localOffset.y; alongX2 = false; }
        if (Sc2 > 0 && (along2 % Sc2) != 0) {
            int c0 = (along2 / Sc2) * Sc2;
            int c1 = c0 + Sc2;
            float t2 = float(along2 - c0) / float(Sc2);
            float hv0, hv1;
            if (alongX2) {
                int fyC = uT.blockOriginY + localOffset.y;
                fxV = clamp((uT.blockOriginX + c0) * 4, 0, u_visualSide - 1);
                int fyVc = clamp(fyC * 4, 0, u_visualSide - 1);
                hv0 = heightsFine[fxV + fyVc * u_visualSide];
                int fxV1 = clamp((uT.blockOriginX + c1) * 4, 0, u_visualSide - 1);
                hv1 = heightsFine[fxV1 + fyVc * u_visualSide];
            } else {
                int fxC = uT.blockOriginX + localOffset.x;
                int fxVc = clamp(fxC * 4, 0, u_visualSide - 1);
                int fyV0 = clamp((uT.blockOriginY + c0) * 4, 0, u_visualSide - 1);
                hv0 = heightsFine[fxVc + fyV0 * u_visualSide];
                int fyV1 = clamp((uT.blockOriginY + c1) * 4, 0, u_visualSide - 1);
                hv1 = heightsFine[fxVc + fyV1 * u_visualSide];
            }
            hv = mix(hv0, hv1, t2);
        } else {
            //PORT-BEGIN:visualDisplaceFar
            fxV = clamp(mapX * 4, 0, u_visualSide - 1);
            fyV = clamp(mapY * 4, 0, u_visualSide - 1);
            hv = heightsFine[fxV + fyV * u_visualSide];
        }
        h = mix(h, hv, clamp(u_visualDisplaceFar, 0.0, 1.0));
        //PORT-END:visualDisplaceFar
    }

    float worldX = float(mapX) * 128.0 - uT.halfMap;
    float worldY = uT.halfMap - float(mapY) * 128.0;
    v_worldPos = vec3(worldX, worldY, h);

    vec4 clip = uT.worldToClipGL * vec4(worldX, worldY, h, 1.0);
    clip.z += 2.0 * TERRAIN_DEPTH_FUDGE * clip.w;  // FUDGE=0 -> no-op
    gl_Position = clip;
}
