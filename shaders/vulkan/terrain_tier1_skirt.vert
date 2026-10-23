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
#extension GL_GOOGLE_include_directive : require

layout(location = 0) in ivec2 localOffset;
layout(location = 1) in int   isSkirtFlag;

#include "include/terrain_frame_ubo_vk.glsl"
#include "include/terrain_patch.glsl"

layout(set = 0, binding = 1, std430) readonly buffer TerrainHeightBuf {
    float heights[];
} uH;

// VK-TERRAIN-VISUALDISPLACE-FAR-1: 4x visual heightfield bake (see terrain_tier1.vert).
layout(set = 0, binding = 10, std430) readonly buffer TerrainVisualHeightBuf {
    float heightsFine[];
};

layout(location = 0) out vec3 v_worldPos;

const float TERRAIN_DEPTH_FUDGE = 0.0;  // LOCKSTEP-MIRROR: terrain_depth_bias.hglsl

float sampleH(int mx, int my) {
    mx = clamp(mx, 0, uT.mapSide - 1);
    my = clamp(my, 0, uT.mapSide - 1);
    return uH.heights[mx + my * uT.mapSide];
}

void main() {
    int mapX = clamp(uP.blockOriginX + localOffset.x, 0, uT.mapSide - 1);
    int mapY = clamp(uP.blockOriginY + localOffset.y, 0, uT.mapSide - 1);
    float h  = sampleH(mapX, mapY);

    if (isSkirtFlag == 0 && uP.edgeStitch != 0) {
        int  Sc = 0, along = 0;
        bool alongX = true;
        if      (localOffset.y == 0             && ((uP.edgeStitch)       & 0xFF) > 0) { Sc = (uP.edgeStitch)       & 0xFF; along = localOffset.x; alongX = true;  }
        else if (localOffset.y == uP.quadCountY && ((uP.edgeStitch >> 8)  & 0xFF) > 0) { Sc = (uP.edgeStitch >> 8)  & 0xFF; along = localOffset.x; alongX = true;  }
        else if (localOffset.x == 0             && ((uP.edgeStitch >> 16) & 0xFF) > 0) { Sc = (uP.edgeStitch >> 16) & 0xFF; along = localOffset.y; alongX = false; }
        else if (localOffset.x == uP.quadCountX && ((uP.edgeStitch >> 24) & 0xFF) > 0) { Sc = (uP.edgeStitch >> 24) & 0xFF; along = localOffset.y; alongX = false; }
        if (Sc > 0 && (along % Sc) != 0) {
            int   c0 = (along / Sc) * Sc;
            int   c1 = c0 + Sc;
            float t  = float(along - c0) / float(Sc);
            float h0, h1;
            if (alongX) {
                int fy = uP.blockOriginY + localOffset.y;
                h0 = sampleH(uP.blockOriginX + c0, fy);
                h1 = sampleH(uP.blockOriginX + c1, fy);
            } else {
                int fx = uP.blockOriginX + localOffset.x;
                h0 = sampleH(fx, uP.blockOriginY + c0);
                h1 = sampleH(fx, uP.blockOriginY + c1);
            }
            h = mix(h0, h1, t);
        }
    }

    // VK-TERRAIN-VISUALDISPLACE-FAR-1 (rung 5): GL's mode-2 branch runs for skirt TOP
    // verts too (terrain_lod_chunk.vert:156 gates on `isSkirtFlag == 0`, and skirt-strip
    // draws share that shader) so the skirt top tracks the DISPLACED surface while the
    // bottom vert stays at the coarse height minus skirtDepth -- exactly what GL renders
    // for a displaced block's skirt. Same VERBATIM non-stitched core as the main VS.
    if (uP.visualMode == 2 && isSkirtFlag == 0) {
        int u_visualSide = uT.visualSide;
        float u_visualDisplaceFar = uT.visualDisplaceFar;
        float hv;
        int fxV, fyV;
        int  Sc2 = 0, along2 = 0; bool alongX2 = true;
        if      (localOffset.y == 0             && ((uP.edgeStitch)       & 0xFF) > 0) { Sc2 = (uP.edgeStitch)       & 0xFF; along2 = localOffset.x; alongX2 = true;  }
        else if (localOffset.y == uP.quadCountY && ((uP.edgeStitch >> 8)  & 0xFF) > 0) { Sc2 = (uP.edgeStitch >> 8)  & 0xFF; along2 = localOffset.x; alongX2 = true;  }
        else if (localOffset.x == 0             && ((uP.edgeStitch >> 16) & 0xFF) > 0) { Sc2 = (uP.edgeStitch >> 16) & 0xFF; along2 = localOffset.y; alongX2 = false; }
        else if (localOffset.x == uP.quadCountX && ((uP.edgeStitch >> 24) & 0xFF) > 0) { Sc2 = (uP.edgeStitch >> 24) & 0xFF; along2 = localOffset.y; alongX2 = false; }
        if (Sc2 > 0 && (along2 % Sc2) != 0) {
            int c0 = (along2 / Sc2) * Sc2;
            int c1 = c0 + Sc2;
            float t2 = float(along2 - c0) / float(Sc2);
            float hv0, hv1;
            if (alongX2) {
                int fyC = uP.blockOriginY + localOffset.y;
                fxV = clamp((uP.blockOriginX + c0) * 4, 0, u_visualSide - 1);
                int fyVc = clamp(fyC * 4, 0, u_visualSide - 1);
                hv0 = heightsFine[fxV + fyVc * u_visualSide];
                int fxV1 = clamp((uP.blockOriginX + c1) * 4, 0, u_visualSide - 1);
                hv1 = heightsFine[fxV1 + fyVc * u_visualSide];
            } else {
                int fxC = uP.blockOriginX + localOffset.x;
                int fxVc = clamp(fxC * 4, 0, u_visualSide - 1);
                int fyV0 = clamp((uP.blockOriginY + c0) * 4, 0, u_visualSide - 1);
                hv0 = heightsFine[fxVc + fyV0 * u_visualSide];
                int fyV1 = clamp((uP.blockOriginY + c1) * 4, 0, u_visualSide - 1);
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

    // Phase 6 parity: skirt bottom verts pulled below the surface.
    //PORT-BEGIN:skirtVertMath
    h -= float(isSkirtFlag) * uP.skirtDepth;
    //PORT-END:skirtVertMath

    float worldX = float(mapX) * 128.0 - uT.halfMap;
    float worldY = uT.halfMap - float(mapY) * 128.0;
    v_worldPos = vec3(worldX, worldY, h);

    vec4 clip = uT.worldToClipGL * vec4(worldX, worldY, h, 1.0);
    clip.z += 2.0 * TERRAIN_DEPTH_FUDGE * clip.w;
    gl_Position = clip;
}
