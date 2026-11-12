// terrain_lighting.comp.slang -- native Vulkan port of shaders/gos_terrain_lighting.comp (step 3.4P).
//
// Per-vertex terrain lighting (sun + ambient + point/spot/terrain lights + fog + haze), output lightRGB / fogRGB
// packed 0xAARRGGBB. Same math as the GL producer (quad.cpp:1266-1426 mirror). The GL producer keeps running for
// the CPU consumers (CopyResultsToVertexPool); this one feeds the VK water cull/pack directly on the GPU.
//
// Set 0:
//   b0  StructuredBuffer<GpuTerrainVertexInput>        resident input, indexed by ABSOLUTE vertex number
//   b1  StructuredBuffer<GpuTerrainLight>              lights [2..N-1] of the terrain light list
//   b2  RWStructuredBuffer<GpuTerrainLightingOutput>   output, indexed by ABSOLUTE vertex number
//   b3  ConstantBuffer<LightingParams>                 per-frame scalars
// Struct layouts are LOCKSTEP with GameOS/gameos/gos_terrain_lighting.h (32 B / 48 B / 8 B).

#include "../include/fog_policy.hglsl"

struct GpuTerrainVertexInput {
    float2 xy;
    float  elevation;
    float  hazeFactor;   // dead (stride only)
    float3 normal;
    uint   flags;
};

struct GpuTerrainLight {
    float3 position;
    uint   lightType;
    float3 color;
    float  closeDistance;
    float  farDistance;
    float  oneOverDistance;
    float  _pad1;
    float  intensity;
};

struct GpuTerrainLightingOutput {
    uint lightRGB;
    uint fogRGB;
};

// std140. Lockstep with VkTerrainLightingParams in vk_native_producers.h.
struct LightingParams {
    int4   counts;     // x numVertices (absolute bound), y numLights, z vertexOffset (vertex number of element 0), w windowCount
    int4   sunRgb;     // x lightRed, y lightGreen, z lightBlue, w rendererSW
    int4   ambRgb;     // x ambientRed, y ambientGreen, z ambientBlue, w lighteningLevel
    int4   baseRgb;    // x,y,z BaseVertexColor R,G,B, w useFog
    float4 sunDirRain; // xyz sun light direction, w rainLightLevel
    float4 fogHaze;    // x fogStart, y fogFull, z minHazeDistance, w effective fog full distance (not clipping)
    float4 playArea;   // (-m, -m, m, m)
    float4 camDist;    // xyz camera world pos, w distanceFactor
};

[[vk::binding(0, 0)]] StructuredBuffer<GpuTerrainVertexInput> vertices;
[[vk::binding(1, 0)]] StructuredBuffer<GpuTerrainLight> lights;
[[vk::binding(2, 0)]] RWStructuredBuffer<GpuTerrainLightingOutput> outputs;
[[vk::binding(3, 0)]] ConstantBuffer<LightingParams> P;

static const uint TG_LIGHT_POINT_GPU   = 1u;
static const uint TG_LIGHT_SPOT_GPU    = 2u;
static const uint TG_LIGHT_TERRAIN_GPU = 3u;
static const uint GPU_VERT_SHADOW      = 0x00000001u;

uint getLightChannel(float intensity, uint lightChan, uint ambientChan)
{
    if (intensity > 0.0)
    {
        int r = int(float(lightChan) * intensity + float(ambientChan));
        if (r >= 255) return 255u;
        if (r < 0)    return 0u;
        return uint(r);
    }
    return ambientChan;
}

bool getFalloff(uint lightIdx, float len, out float falloff)
{
    falloff = 1.0;
    float closeDistance   = lights[lightIdx].closeDistance;
    float farDistance     = lights[lightIdx].farDistance;
    float oneOverDistance = lights[lightIdx].oneOverDistance;
    if (len <= closeDistance) { falloff = 1.0; return true; }
    if (len >= farDistance) return false;
    falloff = (farDistance - len) * oneOverDistance;
    return true;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint id = tid.x;
    if (id >= uint(P.counts.w)) return;
    uint vn = id + uint(P.counts.z);
    if (vn >= uint(P.counts.x)) return;

    GpuTerrainVertexInput v = vertices[vn];

    uint lightr = 255u, lightg = 255u, lightb = 255u;
    uint specR = 0u, specG = 0u, specB = 0u;

    if (P.sunRgb.w == 0)
    {
        float lightIntensity = dot(v.normal, P.sunDirRain.xyz);
        if ((v.flags & GPU_VERT_SHADOW) != 0u && lightIntensity > 0.2) lightIntensity = 0.2;

        lightr = getLightChannel(lightIntensity, uint(P.sunRgb.x), uint(P.ambRgb.x));
        lightg = getLightChannel(lightIntensity, uint(P.sunRgb.y), uint(P.ambRgb.y));
        lightb = getLightChannel(lightIntensity, uint(P.sunRgb.z), uint(P.ambRgb.z));

        if (P.baseRgb.x != 0 || P.baseRgb.y != 0 || P.baseRgb.z != 0)
        {
            lightr = min(lightr + uint(P.baseRgb.x), 255u);
            lightg = min(lightg + uint(P.baseRgb.y), 255u);
            lightb = min(lightb + uint(P.baseRgb.z), 255u);
        }

        float rain = P.sunDirRain.w;
        if (rain < 1.0)
        {
            lightr = uint(float(lightr) * rain);
            lightb = uint(float(lightb) * rain);
            lightg = uint(float(lightg) * rain);
        }

        if (P.ambRgb.w > 0)
        {
            specR = uint(P.ambRgb.w);
            specG = specR;
            specB = specR;
        }

        for (uint i = 0u; i < uint(P.counts.y); i++)
        {
            uint lt = lights[i].lightType;
            if (lt == TG_LIGHT_POINT_GPU || lt == TG_LIGHT_SPOT_GPU || lt == TG_LIGHT_TERRAIN_GPU)
            {
                float3 vertexPos = float3(v.xy.x, v.xy.y, v.elevation);
                float3 vtl = vertexPos - lights[i].position;
                float len = sqrt(dot(vtl, vtl));
                float falloff = 1.0;
                if (getFalloff(i, len, falloff))
                {
                    float red   = lights[i].color.r * lights[i].intensity * 255.0 * falloff;
                    float green = lights[i].color.g * lights[i].intensity * 255.0 * falloff;
                    float blue  = lights[i].color.b * lights[i].intensity * 255.0 * falloff;
                    specR = min(specR + uint(red),   255u);
                    specG = min(specG + uint(green), 255u);
                    specB = min(specB + uint(blue),  255u);
                }
            }
        }
    }

    uint lightRGB_out = (0xFFu << 24) | (lightr << 16) | (lightg << 8) | lightb;
    outputs[vn].lightRGB = lightRGB_out;

    uint fogRGB_out = (0xFFu << 24) | (specR << 16) | (specG << 8) | specB;

    uint fogResult = 0xFFu;
    float fogStart = P.fogHaze.x;
    float fogFull  = P.fogHaze.y;
    if (P.sunRgb.w == 0 && P.baseRgb.w != 0)
    {
        fogResult = uint(mc2FogHeightAlpha(v.elevation, fogStart, fogFull));
        fogRGB_out = (fogResult << 24) | (specR << 16) | (specG << 8) | specB;
    }

    float3 hpos = float3(v.xy.x, v.xy.y, v.elevation);
    float hazeFactor;
    bool outsidePlayArea = (hpos.x <= P.playArea.x) || (hpos.x >= P.playArea.z) ||
                           (hpos.y <= P.playArea.y) || (hpos.y >= P.playArea.w);
    if (outsidePlayArea)
    {
        hazeFactor = 1.0;
    }
    else
    {
        float3 d = hpos - P.camDist.xyz;
        float distanceToEye = sqrt(dot(d, d));
        hazeFactor = mc2FogRamp(distanceToEye, P.fogHaze.z, P.fogHaze.w);
    }
    if (hazeFactor != 0.0)
    {
        float fogFactor = 1.0 - hazeFactor;
        uint distFog = uint(int(fogFactor * 255.0));
        if (distFog < fogResult) fogResult = distFog;
        fogRGB_out = (fogResult << 24) | (specR << 16) | (specG << 8) | specB;
    }

    outputs[vn].fogRGB = fogRGB_out;
}
