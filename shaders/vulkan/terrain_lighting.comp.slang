#include "include/environment_ubo_vk.slang"
// terrain_lighting.comp.slang -- native Vulkan port of shaders/gos_terrain_lighting.comp (step 3.4P).
//
// Per-vertex terrain lighting (sun + ambient + fog + haze; the light list is not read), output lightRGB / fogRGB
// packed 0xAARRGGBB. Same math as the GL producer (quad.cpp:1266-1426 mirror). The GL producer keeps running for
// the CPU consumers (CopyResultsToVertexPool); this one feeds the VK water cull/pack directly on the GPU.
//
// Set 0:
//   b0  StructuredBuffer<GpuTerrainVertexInput>        resident input, indexed by ABSOLUTE vertex number
//   b1  RWStructuredBuffer<GpuTerrainLightingOutput>   output, indexed by ABSOLUTE vertex number
//   b2  ConstantBuffer<LightingParams>                 per-frame scalars
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
    int4   baseRgb;    // x,y,z BaseVertexColor R,G,B, w useFog
    float4 weather;    // rainLightLevel, rendererSW, reserved, reserved
    float4 fogHaze;    // x fogStart, y fogFull, z minHazeDistance, w effective fog full distance (not clipping)
    float4 playArea;   // (-m, -m, m, m)
    float4 camDist;    // xyz camera world pos, w distanceFactor
};

[[vk::binding(0, 0)]] StructuredBuffer<GpuTerrainVertexInput> vertices;
[[vk::binding(1, 0)]] RWStructuredBuffer<GpuTerrainLightingOutput> outputs;
[[vk::binding(2, 0)]] ConstantBuffer<LightingParams> P;

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

    if (P.weather.y == 0)
    {
        float lightIntensity = dot(v.normal, uEnv.sunLiveDir.xyz);
        if ((v.flags & GPU_VERT_SHADOW) != 0u && lightIntensity > 0.2) lightIntensity = 0.2;

        lightr = getLightChannel(lightIntensity, uint(round(uEnv.sunLive.x * 255.0)), uint(round(uEnv.ambLive.x * 255.0)));
        lightg = getLightChannel(lightIntensity, uint(round(uEnv.sunLive.y * 255.0)), uint(round(uEnv.ambLive.y * 255.0)));
        lightb = getLightChannel(lightIntensity, uint(round(uEnv.sunLive.z * 255.0)), uint(round(uEnv.ambLive.z * 255.0)));

        if (P.baseRgb.x != 0 || P.baseRgb.y != 0 || P.baseRgb.z != 0)
        {
            lightr = min(lightr + uint(P.baseRgb.x), 255u);
            lightg = min(lightg + uint(P.baseRgb.y), 255u);
            lightb = min(lightb + uint(P.baseRgb.z), 255u);
        }

        float rain = P.weather.x;
        if (rain < 1.0)
        {
            lightr = uint(float(lightr) * rain);
            lightb = uint(float(lightb) * rain);
            lightg = uint(float(lightg) * rain);
        }

    }

    uint lightRGB_out = (0xFFu << 24) | (lightr << 16) | (lightg << 8) | lightb;
    outputs[vn].lightRGB = lightRGB_out;

    // fogRGB carries the fog byte alone: the water pass reads nothing else of it (no local-light or material bytes).
    uint fogResult = 0xFFu;
    float fogStart = uEnv.fog.w;
    float fogFull  = uEnv.lightingParams.z;
    if (P.weather.y == 0 && P.baseRgb.w != 0)
        fogResult = uint(mc2FogHeightAlpha(v.elevation, fogStart, fogFull));

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
    }

    outputs[vn].fogRGB = fogResult << 24;
}
