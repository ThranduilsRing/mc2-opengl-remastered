// VK-P9a WATER-INTEROP -- Vulkan port of the MC2 armed water fast-path vertex
// shader (shaders/gos_terrain_water_fast_mdi.vert), BASE LAYER ONLY.
//
// PARITY CONTRACT (bit-exact vs the GL water VS):
//   - Procedural, VERTEX-LESS: geometry is derived from gl_VertexIndex indexing
//     into the per-frame thin-record SSBO (6 verts/record = 2 triangles), each
//     thin record -> a WaterRecipe (mission-static SSBO). SAME expansion as the
//     GL VS (vid%6 -> triIdx/id, vid/6 -> thinIdx). VERIFIED against
//     gos_terrain_water_fast_mdi.vert:152-190.
//   - MVP: worldToClipGL is a column_major std140 mat4 packed from the SAME 16
//     raw floats GL uploads (glUniformMatrix4fv GL_FALSE). clip = M*worldPos;
//     clip.z += WATER_DEPTH_FUDGE_FAST*clip.w (reverse-Z pre-divide). SAME rule
//     WATER-MVP-ORACLE-1 proved bit-exact (commit 4d30d93e5; oracle shader in the
//     archive/vk-probe-fixtures tag). Do NOT row_major, do
//     NOT transpose. WATER_DEPTH_FUDGE_FAST=0.00025 passed in via UBO (lockstep).
//   - BASE LAYER ONLY: the GL FS discards o_isWater==2 (detail/spray), so P9a
//     renders only the base layer (isWater=1, the old gl_DrawIDARB==0 draw).
//     uvScale=oneOverTF, uvOffset=(cloudOffsetX,cloudOffsetY), detailMode=0.
//   - Reflection/HDRI OFF (P9a): the FS base branch runs its camera-independent
//     path; those inputs are gated to 0/absent by the C++ upload.
//
// Everything below mirrors the GL VS math VERBATIM. Any single index/stride
// drift = geometry garbage; this file is the load-bearing parity surface.
#version 450

// --- Recipe SSBO (mission-static). std430 layout mirrors WaterRecipe (64B).
//     GL binding 5; here set0/binding5 for descriptor-set clarity. ---
struct WaterRecipe {
    vec4  v01;   // (v0x,v0y, v1x,v1y)
    vec4  v23;   // (v2x,v2y, v3x,v3y)
    vec4  elev;  // (v0e,v1e,v2e,v3e)
    uvec4 ctrl;  // (quadIdx, flags, terrainTypes, waterBits)
};
layout(std430, set = 0, binding = 5) readonly buffer WaterRecipeBuf {
    WaterRecipe recipes[];
};

// --- Per-frame thin record SSBO (48B). GL binding 6 -> set0/binding6. ---
struct WaterThinRecord {
    uvec4 ctrl;      // (recipeIdx, flags, _pad, _pad)
    uvec4 lightRGB;  // corner0..3 ARGB
    uvec4 fogRGB;    // corner0..3 fogRGB; .w byte = FogValue
};
layout(std430, set = 0, binding = 6) readonly buffer WaterThinBuf {
    WaterThinRecord thinRecs[];
};
const uint kPzTri1ValidBit = 0x1u;
const uint kPzTri2ValidBit = 0x2u;

// --- Uniforms (were flat GL uniforms; here one std140 UBO at set0/binding0).
//     worldToClipGL: column_major (std140 default) -- SAME raw 16 floats as GL
//     GL_FALSE upload (WATER-MVP-ORACLE-1). ---
layout(set = 0, binding = 0, std140) uniform WaterVsParams {
    mat4  worldToClipGL;
    vec4  p0;   // (waterElevation, alphaDepth, mapTopLeftX, mapTopLeftY)
    vec4  p1;   // (frameCos, maxMinUV, uvScale, uvOffsetX)
    vec4  p2;   // (uvOffsetY, waterDepthFudgeFast, alphaEdgeByte, alphaMiddleByte)
    vec4  p3;   // (alphaDeepByte, shoreFlat (mode 3 flat plane, 0/1), shoreDepthK, _pad)
} U;

// HORIZON-WAVE-1: the vertex stage reads the camera position (f0.yzw, MC2 world, the same space as
// WorldPos) from the FS parameter block; only its leading vec4s are declared here (std140 prefix).
layout(set = 0, binding = 1, std140) uniform WaterFsParams {
    vec4 fog_color;
    vec4 f0;    // (time, cameraPosX, cameraPosY, cameraPosZ)
} F;

// Output varyings -> water_geom.frag (base-layer subset of gos_terrain_water_mdi.frag).
layout(location = 0) out vec4  Color;
layout(location = 1) out vec2  Texcoord;
layout(location = 2) out float FogValue;
layout(location = 3) out float WaterThickness;
layout(location = 4) out vec3  WorldPos;

vec2 cornerXY(WaterRecipe r, uint c) {
    if (c == 0u) return r.v01.xy;
    if (c == 1u) return r.v01.zw;
    if (c == 2u) return r.v23.xy;
    return r.v23.zw;
}
float cornerElev(WaterRecipe r, uint c) {
    if (c == 0u) return r.elev.x;
    if (c == 1u) return r.elev.y;
    if (c == 2u) return r.elev.z;
    return r.elev.w;
}
uint unpackByte(uint bits, uint c) { return (bits >> (c * 8u)) & 0xFFu; }
uint cornerLightRGB(WaterThinRecord t, uint c) {
    if (c == 0u) return t.lightRGB.x;
    if (c == 1u) return t.lightRGB.y;
    if (c == 2u) return t.lightRGB.z;
    return t.lightRGB.w;
}
uint cornerFogRGB(WaterThinRecord t, uint c) {
    if (c == 0u) return t.fogRGB.x;
    if (c == 1u) return t.fogRGB.y;
    if (c == 2u) return t.fogRGB.z;
    return t.fogRGB.w;
}
vec4 unpackARGB(uint bits) {
    return vec4(float((bits >> 16u) & 0xFFu) / 255.0,
                float((bits >>  8u) & 0xFFu) / 255.0,
                float((bits       ) & 0xFFu) / 255.0,
                float((bits >> 24u) & 0xFFu) / 255.0);
}

void main() {
    float waterElevation = U.p0.x;
    float alphaDepth     = U.p0.y;
    vec2  mapTopLeft     = U.p0.zw;
    float frameCos       = U.p1.x;
    float maxMinUV       = U.p1.y;
    float uvScale        = U.p1.z;
    vec2  uvOffset       = vec2(U.p1.w, U.p2.x);
    float fudge          = U.p2.y;

    uint vid          = uint(gl_VertexIndex);
    uint vertInRecord = vid % 6u;
    uint triIdx       = vertInRecord / 3u;
    uint id           = vertInRecord % 3u;
    uint thinIdx      = vid / 6u;

    WaterThinRecord trec = thinRecs[thinIdx];
    uint recipeIdx = trec.ctrl.x;
    uint thinFlags = trec.ctrl.y;
    WaterRecipe rec = recipes[recipeIdx];

    uint pzValid = (triIdx == 0u) ? (thinFlags & kPzTri1ValidBit)
                                  : (thinFlags & kPzTri2ValidBit);
    if (pzValid == 0u) {
        gl_Position    = vec4(0.0, 0.0, -2.0, 1.0);
        Color          = vec4(0.0);
        Texcoord       = vec2(0.0);
        FogValue       = 0.0;
        WaterThickness = 0.0;
        WorldPos       = vec3(0.0);
        return;
    }

    uint flags  = rec.ctrl.y;
    uint uvMode = flags & 1u;

    uint cornerIdx;
    if (uvMode == 0u) {
        if (triIdx == 0u) cornerIdx = (id == 0u) ? 0u : (id == 1u) ? 1u : 2u;
        else              cornerIdx = (id == 0u) ? 0u : (id == 1u) ? 2u : 3u;
    } else {
        if (triIdx == 0u) cornerIdx = (id == 0u) ? 0u : (id == 1u) ? 1u : 3u;
        else              cornerIdx = (id == 0u) ? 1u : (id == 1u) ? 2u : 3u;
    }

    vec2  vxy       = cornerXY(rec, cornerIdx);
    float velev     = cornerElev(rec, cornerIdx);
    uint  waterBits = unpackByte(rec.ctrl.w, cornerIdx);

    WaterThickness = waterElevation - velev;   // negative = above waterline
    float shoreBlendVS = max(alphaDepth, 1.0);
    float waveCos = ((waterBits & 0x80u) != 0u) ? -frameCos : frameCos;
    // HORIZON-WAVE-1: the legacy wave lifts or drops every vertex by +/-frameCos (Terrain::waterAmplitude,
    // 10 wu stock) with a RANDOM sign per vertex (mapdata.cpp RollDice). Seen from above that reads as a
    // gentle bob; seen at a grazing angle the far edge of the plane IS the horizon, and the random
    // per-vertex offsets turn it into a stepped, wavy line. Fade the displacement out as the view ray
    // flattens toward the water plane: steep (gameplay) views keep the full wave, the horizon is flat.
    {
        vec3  toVert   = vec3(vxy, waterElevation) - F.f0.yzw;
        float sinGraze = abs(toVert.z) / max(length(toVert), 1.0);
        waveCos *= smoothstep(0.03, 0.12, sinGraze);
    }
    // Shore mode 3 (U.p3.y != 0): flat animated plane everywhere; the FS carves the
    // shoreline from the bicubic sidecar. Mirrors gos_terrain_water_fast_mdi.vert u_shoreFlat.
    const bool shoreFlat = (U.p3.y != 0.0);
    float wz = shoreFlat ? (waveCos + waterElevation)
             : (WaterThickness >= 0.0)
               ? waveCos + waterElevation
               : min(velev, waterElevation + shoreBlendVS);
    vec3 worldPos = vec3(vxy, wz);
    WorldPos = worldPos;

    float u = (vxy.x - mapTopLeft.x) * uvScale + uvOffset.x;
    float v = (mapTopLeft.y - vxy.y) * uvScale + uvOffset.y;

    // Per-triangle MaxMinUV wrap correction (legacy quad.cpp:2863-2884).
    {
        uint c0, c1, c2;
        if (uvMode == 0u) {
            if (triIdx == 0u) { c0=0u; c1=1u; c2=2u; }
            else              { c0=0u; c1=2u; c2=3u; }
        } else {
            if (triIdx == 0u) { c0=0u; c1=1u; c2=3u; }
            else              { c0=1u; c1=2u; c2=3u; }
        }
        vec2 p0 = cornerXY(rec, c0);
        vec2 p1 = cornerXY(rec, c1);
        vec2 p2 = cornerXY(rec, c2);
        float u0 = (p0.x - mapTopLeft.x) * uvScale + uvOffset.x;
        float u1 = (p1.x - mapTopLeft.x) * uvScale + uvOffset.x;
        float u2 = (p2.x - mapTopLeft.x) * uvScale + uvOffset.x;
        float v0 = (mapTopLeft.y - p0.y) * uvScale + uvOffset.y;
        float v1 = (mapTopLeft.y - p1.y) * uvScale + uvOffset.y;
        float v2 = (mapTopLeft.y - p2.y) * uvScale + uvOffset.y;
        float maxU = max(u0, max(u1, u2));
        float maxV = max(v0, max(v1, v2));
        if (maxU > maxMinUV || maxV > maxMinUV) {
            u -= floor(maxU - (maxMinUV - 1.0));
            v -= floor(maxV - (maxMinUV - 1.0));
        }
    }

    // BASE layer (detailMode==0): alpha owned by FS shore term; opaque sentinel.
    uint lrgb = cornerLightRGB(trec, cornerIdx);
    uint argb = (lrgb & 0x00FFFFFFu) | 0xFF000000u;

    uint frgb = cornerFogRGB(trec, cornerIdx);
    FogValue = float((frgb >> 24u) & 0xFFu) / 255.0;

    Color    = unpackARGB(argb).bgra;
    Texcoord = vec2(u, v);

    // GL clip emit + water reverse-Z depth bias (pre-divide). SAME as GL VS.
    vec4 clip = U.worldToClipGL * vec4(worldPos, 1.0);
    clip.z   += fudge * clip.w;
    // Mode 3 depth-only lift so coarse terrain poking just above the flat plane where the smooth
    // shore field says water does not cut notches into it. SHORE-FIELD-1: the lift is a bounded
    // WORLD distance toward the camera (U.p3.z wu), not the GL proportional clip.z *= 1+K: in
    // reverse-Z that moved the plane ~K of the camera distance (140 wu at a 4.7 km top-down view),
    // so the water won over every low plateau and beach near the waterline.
    if (shoreFlat && U.p3.z > 0.0) {
        vec3 toCam   = F.f0.yzw - worldPos;
        vec4 clipL   = U.worldToClipGL * vec4(worldPos + toCam * (U.p3.z / max(length(toCam), 1.0)), 1.0);
        clip.z = (clipL.z / clipL.w + fudge) * clip.w;
    }
    gl_Position = clip;
}
