// water_cull_pack.comp.slang -- native Vulkan port of shaders/gpu_driven_water.comp, full-recipe mode (step 3.4P).
//
// One invocation per world-indexed water recipe. Applies the water-eligibility predicate (terrain.cpp narrow-walk
// mirror), projects the four corners through the GL clip matrix, runs the per-triangle depth-admission gate and
// packs a WaterThinRecord (lightRGB / fogRGB from the terrain-lighting buffer, material in the fog low byte).
// The visible count lands in header[0] (atomic); water_draw_args.comp.slang turns it into the indirect draw.
//
// Set 0:
//   b0  StructuredBuffer<WaterRecipe>              mission-static recipes (64 B)
//   b1  StructuredBuffer<GpuTerrainLightingOutput> per-vertex lighting, indexed by absolute vertex number
//   b2  RWStructuredBuffer<WaterThinRecord>        packed output (48 B)
//   b3  ConstantBuffer<WaterCullParams>
//   b4  RWStructuredBuffer<uint>                   header; [0] = visibleCount
// Struct layouts are LOCKSTEP with gos_terrain_water_stream.h (WaterRecipe 64 B, WaterThinRecord 48 B) and
// gos_terrain_lighting.h (GpuTerrainLightingOutput 8 B).

struct WaterRecipe {
    float v0x; float v0y;
    float v1x; float v1y;
    float v2x; float v2y;
    float v3x; float v3y;
    float v0e; float v1e; float v2e; float v3e;
    uint  quadIdx;
    uint  flags;
    uint  terrainTypes;
    uint  waterBits;
};

struct WaterThinRecord {
    uint recipeIdx;
    uint flags;
    uint pad0_;
    uint pad1_;
    uint lightRGB0; uint lightRGB1; uint lightRGB2; uint lightRGB3;
    uint fogRGB0;   uint fogRGB1;   uint fogRGB2;   uint fogRGB3;
};

struct GpuTerrainLightingOutput {
    uint lightRGB;
    uint fogRGB;
};

// std140. Lockstep with VkWaterCullParams in vk_native_producers.h.
// c0..c3 are the four columns of u_worldToClipGL: the raw 16 floats the GL path uploads with GL_FALSE.
struct WaterCullParams {
    float4 c0;
    float4 c1;
    float4 c2;
    float4 c3;
    float4 misc0;   // x waterElevation, y frameCos, z shoreExt, w unused
    int4   misc1;   // x recipeCount, y maxThinRecords, z mapSide, w biasLegacy
};

[[vk::binding(0, 0)]] StructuredBuffer<WaterRecipe> recipes;
[[vk::binding(1, 0)]] StructuredBuffer<GpuTerrainLightingOutput> lighting;
[[vk::binding(2, 0)]] RWStructuredBuffer<WaterThinRecord> thin;
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> header;
[[vk::binding(3, 0)]] ConstantBuffer<WaterCullParams> P;

// shaders/include/terrain_depth_bias.hglsl: WATER_DEPTH_FUDGE_FAST / WATER_DEPTH_FUDGE_FAST_REL.
static const float WATER_DEPTH_FUDGE_FAST     = 0.00025;
static const float WATER_DEPTH_FUDGE_FAST_REL = 0.000631;
static const uint  kFlagBitUvModeBottomLeft   = 0x1u;
static const uint  kWaterThinFlagPzTri1Valid  = 0x1u;
static const uint  kWaterThinFlagPzTri2Valid  = 0x2u;

// terrainTypeToMaterialLocal() in gos_terrain_water_stream.cpp.
uint terrainTypeToMaterial(uint tt)
{
    if (tt == 3u || tt == 8u || tt == 9u || tt == 12u) return 1u;
    if (tt == 2u || tt == 4u) return 2u;
    if (tt == 10u || tt == 13u || tt == 14u || tt == 15u ||
        tt == 16u || tt == 17u || tt == 18u || tt == 19u) return 3u;
    return 0u;
}

float waveOurCos(uint vWaterBits)
{
    return ((vWaterBits & 0x80u) != 0u) ? -P.misc0.y : P.misc0.y;
}

float4 projectWaterClip(float wx, float wy, uint vWaterBits)
{
    float elev = P.misc0.x + waveOurCos(vWaterBits);
    return P.c0 * wx + P.c1 * wy + P.c2 * elev + P.c3;
}

// Conservative triangle admission: clipping is the rasterizer's job. Requiring
// every corner inside near Z removed whole water tiles in low cinematic views.
// XY rejection avoids emitting offscreen water quads.
bool triangleVisible(float4 a, float4 b, float4 c)
{
    if (a.w <= 1e-4 && b.w <= 1e-4 && c.w <= 1e-4) return false;
    if (a.x < -a.w && b.x < -b.w && c.x < -c.w) return false;
    if (a.x >  a.w && b.x >  b.w && c.x >  c.w) return false;
    if (a.y < -a.w && b.y < -b.w && c.y < -c.w) return false;
    if (a.y >  a.w && b.y >  b.w && c.y >  c.w) return false;
    return true;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint id = tid.x;
    if (id >= uint(P.misc1.x)) return;

    WaterRecipe r = recipes[id];

    uint w0 = (r.waterBits      ) & 0xFFu;
    uint w1 = (r.waterBits >>  8) & 0xFFu;
    uint w2 = (r.waterBits >> 16) & 0xFFu;
    uint w3 = (r.waterBits >> 24) & 0xFFu;
    bool waterFlagged = ((w0 | w1 | w2 | w3) & 1u) != 0u;
    float thr = P.misc0.x + P.misc0.z;
    bool submergedSand = !waterFlagged && (r.v0e < thr || r.v1e < thr || r.v2e < thr || r.v3e < thr);
    if (!(waterFlagged || submergedSand)) return;

    float4 clip0 = projectWaterClip(r.v0x, r.v0y, w0);
    float4 clip1 = projectWaterClip(r.v1x, r.v1y, w1);
    float4 clip2 = projectWaterClip(r.v2x, r.v2y, w2);
    float4 clip3 = projectWaterClip(r.v3x, r.v3y, w3);

    bool isBottomLeft = (r.flags & kFlagBitUvModeBottomLeft) != 0u;
    bool pzTri1;
    bool pzTri2;
    if (!isBottomLeft)
    {
        pzTri1 = triangleVisible(clip0, clip1, clip2);
        pzTri2 = triangleVisible(clip0, clip2, clip3);
    }
    else
    {
        pzTri1 = triangleVisible(clip0, clip1, clip3);
        pzTri2 = triangleVisible(clip1, clip2, clip3);
    }
    if (!pzTri1 && !pzTri2) return;


    uint ms  = uint(P.misc1.z);
    uint vn0 = r.quadIdx;
    uint vn1 = r.quadIdx + 1u;
    uint vn2 = r.quadIdx + ms + 1u;
    uint vn3 = r.quadIdx + ms;

    // One atomic per wave over the lanes that reach the append; a lane's slot is the wave base plus its rank among them.
    // Count and rank BEFORE the elect branch: a wave op inside `if (WaveIsFirstLane())` sees one active lane, so the atomic
    // added 1 per wave while every lane took base + rank (overlapping slots, header[0] far too small: no water drawn).
    uint waveCount = WaveActiveCountBits(true);
    uint waveRank = WavePrefixCountBits(true);
    uint waveBase = 0u;
    if (WaveIsFirstLane()) InterlockedAdd(header[0], waveCount, waveBase);
    uint outSlot = WaveReadLaneFirst(waveBase) + waveRank;
    if (outSlot >= uint(P.misc1.y)) return;

    uint mtl0 = terrainTypeToMaterial((r.terrainTypes      ) & 0xFFu);
    uint mtl1 = terrainTypeToMaterial((r.terrainTypes >>  8) & 0xFFu);
    uint mtl2 = terrainTypeToMaterial((r.terrainTypes >> 16) & 0xFFu);
    uint mtl3 = terrainTypeToMaterial((r.terrainTypes >> 24) & 0xFFu);

    WaterThinRecord tr;
    tr.recipeIdx = id;
    uint pzFlags = 0u;
    if (pzTri1) pzFlags |= kWaterThinFlagPzTri1Valid;
    if (pzTri2) pzFlags |= kWaterThinFlagPzTri2Valid;
    tr.flags = pzFlags;
    tr.pad0_ = 0u;
    tr.pad1_ = 0u;
    tr.lightRGB0 = lighting[vn0].lightRGB;
    tr.lightRGB1 = lighting[vn1].lightRGB;
    tr.lightRGB2 = lighting[vn2].lightRGB;
    tr.lightRGB3 = lighting[vn3].lightRGB;
    tr.fogRGB0 = (lighting[vn0].fogRGB & 0xFFFFFF00u) | mtl0;
    tr.fogRGB1 = (lighting[vn1].fogRGB & 0xFFFFFF00u) | mtl1;
    tr.fogRGB2 = (lighting[vn2].fogRGB & 0xFFFFFF00u) | mtl2;
    tr.fogRGB3 = (lighting[vn3].fogRGB & 0xFFFFFF00u) | mtl3;
    thin[outSlot] = tr;
}
