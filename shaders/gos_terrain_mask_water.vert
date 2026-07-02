//#version 430 (provided by makeProgram prefix)
#include <include/terrain_depth_bias.hglsl>  // single-source TERRAIN/WATER_DEPTH_FUDGE
//
// Stage 1c — Mask-water dual-run VS.
//
// Reads the per-frame water mask SSBO (binding 18) and the static
// WaterRecipe SSBO (binding 5). For each of the recipeCount*6 vertices,
// emits a degenerate triangle when the quad's bit is clear in the mask
// (quad is not visible this frame), otherwise reprojects the water surface
// via the same double-projection chain as gos_terrain_water_fast.vert.
//
// Stage 1c dual-run soak: bridge applies glColorMask(GL_FALSE)+glDepthMask(GL_FALSE).
// Stage 1d will enable writes when the legacy water fast path is retired.
//
// Key difference from gos_terrain_water_fast.vert:
//   - No per-frame thin-record SSBO. Index scheme: recipeIdx = gl_VertexID/6.
//   - Reads water mask at binding 18 to cull masked-out quads.
//   - Corner-0 vertexNum = recipes[recipeIdx].ctrl.x (= quadIdx).
//   - Lighting SSBO at binding 2 used for Color (suppressed by glColorMask anyway).

struct WaterRecipe {
    vec4  v01;    // (v0x, v0y, v1x, v1y)
    vec4  v23;    // (v2x, v2y, v3x, v3y)
    vec4  elev;   // (v0e, v1e, v2e, v3e)
    uvec4 ctrl;   // (quadIdx, flags, terrainTypes, waterBits)
                  //   flags: bit0=uvMode (0=BOTTOMRIGHT, 1=BOTTOMLEFT), bit1=hasDetail
                  //   waterBits: 4 bytes packed v0..v3; bit7→-frameCos, else +frameCos
};

layout(std430, binding = 18) readonly buffer WaterMaskBuf {
    uint waterMask[];
};
layout(std430, binding =  5) readonly buffer WaterRecipeBuf {
    WaterRecipe recipes[];
};

struct GpuTerrainLightingOutput {
    uint lightRGB;  // ARGB packed (low 24 bits = RGB, high byte = alpha/unused)
    uint fogRGB;    // fogRGB; high byte = FogValue (0..255)
};
layout(std430, binding =  4) readonly buffer LightingBuf {  // VK-H2-BINDING-SPLIT-1: was binding=2 (collided with gos_terrain.frag ThinRecordBufFrag@2 in same program)
    GpuTerrainLightingOutput lighting[];
};

out vec4  Color;
out vec2  Texcoord;
out float FogValue;

uniform mat4  u_worldToClipGL;
uniform float waterElevation;
uniform float frameCos;

vec4 unpackARGB(uint bits) {
    return vec4(
        float((bits >> 16u) & 0xFFu) / 255.0,
        float((bits >>  8u) & 0xFFu) / 255.0,
        float((bits       ) & 0xFFu) / 255.0,
        float((bits >> 24u) & 0xFFu) / 255.0
    );
}

void main() {
    uint recipeIdx = uint(gl_VertexID) / 6u;
    uint vertInRec = uint(gl_VertexID) % 6u;
    uint triIdx    = vertInRec / 3u;
    uint id        = vertInRec % 3u;

    WaterRecipe rec = recipes[recipeIdx];

    // corner-0 vertexNum = quadIdx = mx + my*mapSide (same as mask bit index).
    uint mapVn = rec.ctrl.x;

    // Mask cull: emit degenerate if this quad is not set in the water mask.
    if ((waterMask[mapVn >> 5u] >> (mapVn & 31u) & 1u) == 0u) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        Color    = vec4(0.0);
        Texcoord = vec2(0.0);
        FogValue = 0.0;
        return;
    }

    uint uvMode = rec.ctrl.y & 1u;

    // Corner index table — identical to gos_terrain_water_fast.vert.
    //   BOTTOMRIGHT (uvMode=0): tri0=corners[0,1,2], tri1=corners[0,2,3]
    //   BOTTOMLEFT  (uvMode=1): tri0=corners[0,1,3], tri1=corners[1,2,3]
    uint cornerIdx;
    if (uvMode == 0u) {
        if (triIdx == 0u)
            cornerIdx = (id == 0u) ? 0u : (id == 1u) ? 1u : 2u;
        else
            cornerIdx = (id == 0u) ? 0u : (id == 1u) ? 2u : 3u;
    } else {
        if (triIdx == 0u)
            cornerIdx = (id == 0u) ? 0u : (id == 1u) ? 1u : 3u;
        else
            cornerIdx = (id == 0u) ? 1u : (id == 1u) ? 2u : 3u;
    }

    // World XY from recipe.
    vec2 vxy;
    if      (cornerIdx == 0u) vxy = rec.v01.xy;
    else if (cornerIdx == 1u) vxy = rec.v01.zw;
    else if (cornerIdx == 2u) vxy = rec.v23.xy;
    else                       vxy = rec.v23.zw;

    // Wave displacement — replicates quad.cpp:689-700 / gos_terrain_water_fast.vert.
    // bit7 set → ourCos = -frameCos; else ourCos = +frameCos.
    uint waterBitsCorner = (rec.ctrl.w >> (cornerIdx * 8u)) & 0xFFu;
    float ourCos = ((waterBitsCorner & 0x80u) != 0u) ? -frameCos : frameCos;
    float wz = ourCos + waterElevation;

    vec3 worldPos = vec3(vxy, wz);

    // Color from lighting SSBO corner-0 (simplified — glColorMask(GL_FALSE) suppresses).
    // Pre-swizzle to BGRA so gos_tex_vertex.frag's `.bgra` double-swizzles back to RGBA.
    uint lightARGB = lighting[mapVn].lightRGB;
    Color    = unpackARGB(lightARGB).bgra;
    Texcoord = vec2(0.5);
    FogValue = 0.0;

    // Double projection chain. z-bias is single-sourced
    // (terrain_depth_bias.hglsl). This is the mask-water VS: its REAL peer
    // is the legacy CPU raster water (RASTER regime, delta 0.0005 = 0.0025),
    // NOT the GPU fast-VS (FAST regime 0.003). The genuine #10 desync was
    // this site's old comment FALSELY claiming it matched the fast VS while
    // emitting 0.0025; fixed by pointing it at the correct shared constant
    // (WATER_DEPTH_FUDGE_RASTER, value unchanged at 0.0025). A 1-constant
    // unification to 0.003 here over-biased water behind low-LOD terrain
    // (TES tiles breaking through at map edges); see header + git 89d7c4f.
    //   terrain: TERRAIN_DEPTH_FUDGE       = 0.002
    //   water:   WATER_DEPTH_FUDGE_RASTER  = 0.0025 (TERRAIN + 0.0005)
    vec4 clip = u_worldToClipGL * vec4(worldPos, 1.0);
    clip.z   += WATER_DEPTH_FUDGE_RASTER * clip.w;
    gl_Position = clip;
}
