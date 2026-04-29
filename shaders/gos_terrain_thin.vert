//#version 430 (version provided by material prefix)

// --- SSBO bindings (must match TerrainQuadThinRecord / TerrainQuadRecipe in gos_terrain_patch_stream.h) ---
struct TerrainQuadThinRecord {
    uvec4 control;    // x=recipeIdx, y=terrainHandle, z=flags(bit0=uvMode,bit1=pzTri1,bit2=pzTri2), w=_pad0
    uvec4 lightRGBs;  // corners 0-3, packed ARGB
    uvec4 fogRGBs;    // corners 0-3, packed frgb
};
layout(std430, binding = 2) readonly buffer ThinRecordBuf {
    TerrainQuadThinRecord thinRecs[];
};

struct TerrainQuadRecipe {
    vec4 worldPos0, worldPos1, worldPos2, worldPos3;
    vec4 worldNorm0, worldNorm1, worldNorm2, worldNorm3;
    vec4 uvData;  // minU, minV, maxU, maxV
};
layout(std430, binding = 1) readonly buffer RecipeBuf {
    TerrainQuadRecipe recipes[];
};

// Output varyings — names MUST match gos_terrain.frag `in` declarations exactly.
out vec4  Color;
out float FogValue;
out vec2  Texcoord;
out float TerrainType;
out vec3  WorldNorm;
out vec3  WorldPos;
out float UndisplacedDepth;

// Uniforms used by this shader
uniform int  ssboRecordBase;     // global record index offset for this draw call
uniform mat4 terrainMVP;         // axisSwap * worldToClip
uniform vec4 terrainViewport;    // (vmx, vmy, vax, vay) for perspective projection
uniform mat4 mvp;                // projection_: screen pixels -> NDC

// Unpack ARGB uint to vec4 each component 0..255 -> 0..1.
vec4 unpackARGB(uint packed) {
    return vec4(
        float((packed >> 16u) & 0xFFu) / 255.0,  // R
        float((packed >>  8u) & 0xFFu) / 255.0,  // G
        float((packed       ) & 0xFFu) / 255.0,  // B
        float((packed >> 24u) & 0xFFu) / 255.0   // A
    );
}

// Get uvec4 component by index 0-3.
uint uvec4Idx(uvec4 v, uint idx) {
    if (idx == 0u) return v.x;
    if (idx == 1u) return v.y;
    if (idx == 2u) return v.z;
    return v.w;
}

void main() {
    uint vid          = uint(gl_VertexID);
    uint vertInRecord = vid % 6u;
    uint triIdx       = vertInRecord / 3u;
    uint id           = vertInRecord % 3u;
    uint recordIdx    = uint(ssboRecordBase) + vid / 6u;

    TerrainQuadThinRecord tr = thinRecs[recordIdx];
    uint flags   = tr.control.z;
    uint uvMode  = flags & 1u;
    uint pzTri1  = (flags >> 1u) & 1u;
    uint pzTri2  = (flags >> 2u) & 1u;
    uint pzValid = (triIdx == 0u) ? pzTri1 : pzTri2;

    // pz-culled triangles: degenerate position (behind near clip, never rasterized).
    if (pzValid == 0u) {
        gl_Position    = vec4(0.0, 0.0, -2.0, 1.0);
        Color          = vec4(0.0);
        FogValue       = 0.0;
        Texcoord       = vec2(0.0);
        TerrainType    = 0.0;
        WorldNorm      = vec3(0.0, 0.0, 1.0);
        WorldPos       = vec3(0.0);
        UndisplacedDepth = 0.0;
        return;
    }

    uint recipeIdx = tr.control.x;
    TerrainQuadRecipe rec = recipes[recipeIdx];

    // Corner index table — same convention as gos_terrain.tesc thin path.
    // TOPRIGHT  (uvMode=0): tri0=corners[0,1,2], tri1=corners[0,2,3]
    // BOTTOMLEFT(uvMode=1): tri0=corners[0,1,3], tri1=corners[1,2,3]
    uint cornerIdx;
    if (uvMode == 0u) {
        if (triIdx == 0u) {
            cornerIdx = (id == 0u) ? 0u : (id == 1u) ? 1u : 2u;
        } else {
            cornerIdx = (id == 0u) ? 0u : (id == 1u) ? 2u : 3u;
        }
    } else {
        if (triIdx == 0u) {
            cornerIdx = (id == 0u) ? 0u : (id == 1u) ? 1u : 3u;
        } else {
            cornerIdx = (id == 0u) ? 1u : (id == 1u) ? 2u : 3u;
        }
    }

    // World position and normal from recipe corners.
    vec4 wp = (cornerIdx == 0u) ? rec.worldPos0
             :(cornerIdx == 1u) ? rec.worldPos1
             :(cornerIdx == 2u) ? rec.worldPos2
             :                    rec.worldPos3;
    vec4 wn = (cornerIdx == 0u) ? rec.worldNorm0
             :(cornerIdx == 1u) ? rec.worldNorm1
             :(cornerIdx == 2u) ? rec.worldNorm2
             :                    rec.worldNorm3;
    vec3 worldPos  = wp.xyz;
    vec3 worldNorm = normalize(wn.xyz);

    // UV reconstruction (verified against quad.cpp actual UV assignment):
    //   corner 0=(minU,minV), corner 1=(maxU,minV), corner 2=(maxU,maxV), corner 3=(minU,maxV)
    float u = (cornerIdx == 1u || cornerIdx == 2u) ? rec.uvData.z : rec.uvData.x;
    float v = (cornerIdx == 0u || cornerIdx == 1u) ? rec.uvData.y : rec.uvData.w;

    // Lighting and fog per corner.
    uint lrgb = uvec4Idx(tr.lightRGBs, cornerIdx);
    uint frgb = uvec4Idx(tr.fogRGBs,   cornerIdx);

    Color       = unpackARGB(lrgb);
    FogValue    = float((frgb >> 24u) & 0xFFu) / 255.0;
    Texcoord    = vec2(u, v);
    TerrainType = float(frgb & 0xFFu);
    WorldNorm   = worldNorm;
    WorldPos    = worldPos;

    // Double-projection — identical to TES, minus displacement (thin path skips it).
    // No displacement => UndisplacedDepth == actual depth.
    vec4 clip = terrainMVP * vec4(worldPos, 1.0);
    float rhw = 1.0 / clip.w;
    vec3 screen;
    screen.x = clip.x * rhw * terrainViewport.x + terrainViewport.z;
    screen.y = clip.y * rhw * terrainViewport.y + terrainViewport.w;
    screen.z = clip.z * rhw;
    vec4 ndc = mvp * vec4(screen, 1.0);
    float absW = abs(clip.w);
    gl_Position      = vec4(ndc.xyz * absW, absW);
    UndisplacedDepth = screen.z * 0.5 + 0.5;
}
