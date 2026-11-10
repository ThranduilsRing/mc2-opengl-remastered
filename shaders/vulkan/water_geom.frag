// VK-P9a WATER-INTEROP -- Vulkan port of the MC2 armed water FS
// (shaders/gos_terrain_water_mdi.frag), BASE-LAYER (o_isWater==1) subset with
// Writes MRT: FragColor(0) + GBuffer1(1); spatial water reflection over HDRI/SH sky.
//
// PARITY CONTRACT: reproduces the o_isWater==1 base-water branch VERBATIM with
// reflection disabled by the Off quality setting. Sky only uses HDRI/SH; Half-res
// adds current-frame reflected terrain/props with sky fallback for missing hits.
// Material params (DEEP/SHALLOW/ABSORPTION/etc.) are the exact former-const
// defaults seeded by the C++ upload (WATER-TUNING-UI-1). fog_color is uploaded as
// the current render state (CINEMATIC-WATER-WHITE-1 gives vec4(0) by water time).
//
// GBuffer1 = rc_gbuffer1_screenShadowEligible(vec3(0,0,1)) = vec4(0.5,0.5,1.0,0.0)
// -- value inlined (render_contract.hglsl) so the VK shader needs no GL include.
#version 450
#extension GL_GOOGLE_include_directive : require
#define PREC highp
#define TERRAIN_RESOURCE_SET 1
#define TERRAIN_OBJECT_RECEIVER 1
#include "include/terrain_frame_vk.glsl"
#include "../include/render_contract.hglsl"

layout(location = 0) in PREC vec4  Color;
layout(location = 1) in PREC vec2  Texcoord;
layout(location = 2) in PREC float FogValue;
layout(location = 3) in PREC float WaterThickness;
layout(location = 4) in PREC vec3  WorldPos;

layout(location = 0) out PREC vec4 FragColor;
layout(location = 1) out PREC vec4 GBuffer1;

// Uniforms (base branch + rung-6.5 reflection). One std140 UBO.
layout(set = 0, binding = 1, std140) uniform WaterFsParams {
    vec4 fog_color;       // current render-state fog (vec4(0) by water time -> fog off)
    vec4 f0;              // (time, cameraPosX, cameraPosY, cameraPosZ)
    vec4 f1;              // (alphaDepth, ABSORPTION_DENSITY, WATER_MAX_ALPHA, RIPPLE_GAIN)
    vec4 f2;              // (GLINT_GAIN, u_waterReflStrength, u_skyYaw, u_waterHdriLod)
    vec4 f3;              // (screenW, screenH, nightDim, animatedShore)
    vec4 DEEP_COLOR;      // .xyz color, .w Half-res reflection enabled
    vec4 SHALLOW_COLOR;   // .xyz
    // VK-PARITY-WATER-SHORE-1 (WATER-SHORE-EDGE-1): shore-feather inputs.
    vec4 f4;              // (u_shoreMode, u_hasShoreHeight, u_hasShoreMask, u_shoreFeatherWu)
    vec4 f5;              // u_shoreHeightBounds (topLeftX, topLeftY, spanX, spanY)
    vec4 f6;              // u_shoreMaskBounds   (topLeftX, topLeftY, sizeX, sizeY)
    vec4 f7;              // (waterElevation, mask span (wu), mask bound (0/1), terrain grid halfMap)
    // VK-PARITY-F3 (water brightness): GL-default features missing in VK.
    vec4 f8;              // (u_waterEdgeFeather, u_waterEdgeFeatherStrength, u_halfMap, u_waterSkyTintStrength)
    vec4 f9;              // (u_waterSkyTintColor.xyz, u_waterProjAspect)
} F;

// RUNG-6.5 WATER-REFLECTION PORT: HDRI equirect (copy-once import of GL's
// gos_hdri texture — BC6H mip0 sidecar in the field config, RGBA16F+mips
// fallback). Sampled only when u_waterHdriLod >= 0 (island patches it to -1
// when the import is unavailable -> SH-L2 fallback, matching GL semantics).
layout(set = 0, binding = 2) uniform sampler2D u_hdri;

// VK-PARITY-WATER-SHORE-1: copy-once imports of the GL shore textures (same CPU
// data the GL water FS samples): R32F hi-res visual height, RGBA8 shoreline mask.
layout(set = 0, binding = 3) uniform sampler2D u_shoreHeightTex;
layout(set = 0, binding = 4) uniform sampler2D u_shoreMask;
// The static shadow bake's mask (R8 + mips over the terrain grid, row 0 north, evaluated at max(terrain, water)): the same texel the ground at
// the shoreline reads, so the shadow runs across the coast without a cut. Read only when F.f7.z says it is bound.
layout(set = 0, binding = 7) uniform sampler2D u_staticMask;
layout(set = 0, binding = 8) uniform sampler2D u_waterReflection;
#include "../include/water_shore_edge.hglsl"
#include "../include/water_reactive.hglsl"
#include "../include/water_shallow.hglsl"

struct ReactiveStamp { vec4 positionRadiusAge; vec4 velocityType; };
layout(set = 0, binding = 13, std430) readonly buffer WaterReactive {
    ReactiveStamp stamps[128];
    uint counts[1024];
    uint indices[8192];
} R;

vec3 reactiveWater(vec2 worldXY, float pixelWidth) {
    ivec2 cell = ivec2(floor(worldXY / 256.0));
    uint tile = (uint(cell.y) & 31u) * 32u + (uint(cell.x) & 31u);
    vec3 result = vec3(0.0);
    for (uint i = 0u; i < min(R.counts[tile], 8u); ++i) {
        ReactiveStamp s = R.stamps[R.indices[tile * 8u + i]];
        vec2 delta = worldXY - s.positionRadiusAge.xy;
        if (dot(delta, delta) < 224.0 * 224.0)
            result += wr_profile(delta, s.velocityType.xy, s.positionRadiusAge.z,
                                 s.positionRadiusAge.w, s.velocityType.z, pixelWidth);
    }
    return vec3(clamp(result.xy, vec2(-kSlopeMax), vec2(kSlopeMax)), min(result.z, kCrestMax));
}
#include "../include/edge_haze.hglsl"   // VK-PARITY-F3: WATER-EDGE-FEATHER-1 (EDGE_HAZE_SKY, edgeHazeAmount)

// Global aliases (assigned in main) so the ported GL blocks below stay
// TEXTUALLY VERBATIM (GL declares these as uniforms at global scope).
float u_skyYaw;
float u_waterHdriLod;
float u_waterReflStrength;
vec2  u_waterScreenSize;
PREC vec4 cameraPos;

const float SKY_AMBIENT   = 0.18;
const float WAVE_FREQ     = 0.030;
const float WAVE_SPEED    = 6.0;
const float WAVE_FADE_NEAR = 9000.0;
const float WAVE_FADE_FAR  = 40000.0;

// Reflection consts — VERBATIM from gos_terrain_water_mdi.frag.
const float REFL_F0         = 0.02;   // Schlick base reflectance (water ~0.02)
const float REFL_MAX        = 0.55;   // WATER-REFL-DEFAULT-ON: raised from 0.30 for visible shiny
const float REFL_WAVE_SLOPE = 0.05;   // fBm-gradient -> wave-normal slope (0.02-0.10)
// WATER-GRAZE-REFL-1 (VERBATIM from gos_terrain_water_mdi.frag): bias the reflected colour
// toward the authored sky/horizon tint at grazing angles.
const float GRAZE_SKY_PULL  = 0.70;
const float GRAZE_SKY_EXP   = 4.0;

//PORT-BEGIN:waterSkyShCoeffs
const vec3 kWaterSkySh[9] = vec3[9](
    vec3( 1.713250528,   1.773145394,   1.652499519  ),
    vec3( 0.5869951003,  0.5782034038,  0.5179309118 ),
    vec3( 0.01179062044, 0.001345766309,-0.008740967483),
    vec3( 1.966354845,   1.856721003,   1.505876116  ),
    vec3( 1.334658751,   1.287703421,   1.077505997  ),
    vec3( 0.01688171049, 0.01641851255, 0.01176750778),
    vec3(-1.016652552,  -0.9684202893, -0.7816944278 ),
    vec3( 0.05064675817, 0.04545421310, 0.03281146747),
    vec3( 1.962057200,   1.850144657,   1.499676879  )
);
//PORT-END:waterSkyShCoeffs

//PORT-BEGIN:waterEvalHdri
PREC vec3 waterEvalHdri(PREC vec3 rdir) {
    float s = sin(u_skyYaw), c = cos(u_skyYaw);
    PREC vec3 d = vec3(rdir.x * c - rdir.y * s,
                       rdir.x * s + rdir.y * c,
                       rdir.z);
    PREC vec2 uv = vec2(atan(d.y, d.x) / (2.0 * 3.14159265) + 0.5,
                        1.0 - (asin(clamp(d.z, -1.0, 1.0)) / 3.14159265 + 0.5));
    PREC vec3 raw = textureLod(u_hdri, uv, u_waterHdriLod).rgb;
    // Reinhard per-channel: physical HDR values [0, inf) -> [0, 1).
    // Keeps blue > red for clear sky (hue preserved), prevents sun/bright-cloud
    // from collapsing to featureless white in the reflection mix.
    return raw / (raw + 1.0);
}
//PORT-END:waterEvalHdri

//PORT-BEGIN:waterEvalSkySh
PREC vec3 waterEvalSkySh(PREC vec3 n){
    const float c1 = 0.429043, c2 = 0.511664, c3 = 0.743125, c4 = 0.886227, c5 = 0.247708;
    return (  c1 * kWaterSkySh[8] * (n.x*n.x - n.z*n.z)
            + c3 * kWaterSkySh[6] * (n.y*n.y)
            + c4 * kWaterSkySh[0]
            - c5 * kWaterSkySh[6]
            + 2.0*c1 * kWaterSkySh[4] * (n.x*n.y)
            + 2.0*c1 * kWaterSkySh[5] * (n.y*n.z)
            + 2.0*c1 * kWaterSkySh[7] * (n.x*n.z)
            + 2.0*c2 * kWaterSkySh[3] *  n.x
            + 2.0*c2 * kWaterSkySh[1] *  n.y
            + 2.0*c2 * kWaterSkySh[2] *  n.z );
}
//PORT-END:waterEvalSkySh

PREC float h21(PREC vec2 ip){
    PREC vec3 q = fract(vec3(ip.xyx) * 0.1031);
    q += dot(q, q.yzx + 33.33);
    return fract((q.x + q.y) * q.z);
}
PREC float vnoise(PREC vec2 uv){
    PREC vec2 i = floor(uv), f = fract(uv);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(h21(i),               h21(i + vec2(1.0, 0.0)), f.x),
               mix(h21(i + vec2(0.0,1.0)), h21(i + vec2(1.0, 1.0)), f.x), f.y);
}
// WATER-WAVES-GLINT: wind-driven wave trains that read from the RTS camera, lit by a GGX sun/moon and lamp specular.
// Analytic sine slopes animated by time only (no history). Each train: (wind dir x, y, wavelength wu, slope amplitude
// A*k); a train's slope variance is slope^2 / 2. Trains below the pixel footprint fade out of the normal and their
// variance moves into the GGX roughness (LEAN-style), so distant water keeps a wide highlight instead of sparkling.
const int   WAVE_TRAINS = 12;
// Two low-amplitude swells (420/640 wu, also carried by the vertex displacement) then ten short-crested components
// (15-55 wu). Directions spread about +-45 degrees around the wind (22 deg), phase i * 2.39996 rad: all deterministic.
const vec4  kWaveTrain[WAVE_TRAINS] = vec4[WAVE_TRAINS](
    vec4(0.985, 0.174, 420.0, 0.030),
    vec4(0.857, 0.515, 640.0, 0.030),
    vec4(0.921, -0.391, 23.1, 0.050),
    vec4(0.842, 0.539, 15.5, 0.050),
    vec4(1.000, -0.031, 52.4, 0.050),
    vec4(0.590, 0.808, 35.3, 0.050),
    vec4(0.943, 0.334, 23.8, 0.050),
    vec4(0.966, -0.257, 16.0, 0.050),
    vec4(0.758, 0.653, 54.0, 0.050),
    vec4(0.994, 0.111, 36.4, 0.050),
    vec4(0.470, 0.883, 24.6, 0.050),
    vec4(0.886, 0.463, 16.5, 0.050));
const float WAVE_G           = 30.0;    // deep-water dispersion in wu/s^2: 760 wu swell ~60 wu/s, 150 wu chop ~27 wu/s
const float WAVE_RESOLVE_LO  = 2.0;     // pixels per wavelength: at or below -> roughness only
const float WAVE_RESOLVE_HI  = 6.0;     // pixels per wavelength: at or above -> fully in the normal
const float WATER_ALPHA0     = 0.06;    // GGX alpha of the calm surface
const float RIPPLE_SLOPE_VAR = 0.004;   // slope variance of the fine ripples once they fade with distance
const float SUN_SPEC_GAIN    = 3.0;     // sun/moon specular scale at the default glint gain (0.30)
const float LAMP_SPEC_GAIN   = 1.5;     // lamp/weapon pool-light specular scale at the default glint gain

PREC vec2 waterWaveSlope(PREC vec2 p, float t, float footprint, float gain, out float unresolvedVar){
    PREC vec2 s = vec2(0.0);
    unresolvedVar = 0.0;
    for (int i = 0; i < WAVE_TRAINS; ++i) {
        vec4  w = kWaveTrain[i];
        float k = 6.2831853 / w.z;
        float r = smoothstep(WAVE_RESOLVE_LO, WAVE_RESOLVE_HI, w.z / max(footprint, 1e-3));
        float a = w.w * gain;
        unresolvedVar += 0.5 * a * a * (1.0 - r * r);
        if (r > 0.0) {
            PREC float ph = k * dot(w.xy, p) - sqrt(WAVE_G * k) * t + float(i) * 2.39996;
            s += w.xy * (a * r * cos(ph));
        }
    }
    return s;
}

// GGX with height-correlated Smith visibility and Schlick Fresnel (water F0): energy-normalised lobe, times N.L.
float waterGgxSpec(vec3 N, vec3 V, vec3 L, float alpha2){
    float NdotL = dot(N, L);
    if (NdotL <= 0.0) return 0.0;
    float NdotV = max(dot(N, V), 1e-3);
    vec3  H     = normalize(L + V);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);
    float d     = NdotH * NdotH * (alpha2 - 1.0) + 1.0;
    float D     = alpha2 / (3.14159265 * d * d);
    float vis   = 0.5 / (NdotL * sqrt(NdotV * NdotV * (1.0 - alpha2) + alpha2)
                       + NdotV * sqrt(NdotL * NdotL * (1.0 - alpha2) + alpha2));
    float Fr    = REFL_F0 + (1.0 - REFL_F0) * pow(1.0 - VdotH, 5.0);
    return D * vis * Fr * NdotL;
}

// Lamp and weapon pool lights reflected by the wave normal: the same grid cell walk and attenuation as the terrain
// pool term, with the GGX lobe toward each light instead of the flat diffuse pool.
vec3 waterLampSpec(vec3 wp, vec3 N, vec3 V, float alpha2){
    int  lo = 0, hi = uT.numPoolLights;
    bool indexed = false;
    if (uT.lightGridDim > 0) {
        ivec2 c = ivec2(floor((wp.xy - vec2(uT.lightGridMinX, uT.lightGridMinY))
                              * vec2(uT.lightGridInvCellX, uT.lightGridInvCellY)));
        if (any(lessThan(c, ivec2(0))) || any(greaterThanEqual(c, ivec2(uT.lightGridDim)))) return vec3(0.0);
        uvec2 cell = lightGridCells[c.y * uT.lightGridDim + c.x];
        lo = int(cell.x);
        hi = int(cell.x + cell.y);
        indexed = true;
    }
    if (TLP_POOL_BUDGET > 0) hi = min(hi, lo + TLP_POOL_BUDGET);
    vec3 s = vec3(0.0);
    for (int k = lo; k < hi; ++k) {
        int i = indexed ? int(lightGridIndices[k]) : k;
        if (i >= uT.numPoolLights || poolLights[i].lightType == 0u) continue;
        vec3  vtl   = wp - poolLights[i].position;
        bool  mover = poolLights[i]._plPad1 > 0.5;
        float nearD = poolLights[i].closeDistance;
        float len, falloff;
        if (!lpa_falloff(vtl, mover, uT.lightPoolMoverScale, uT.lightPoolSoft, nearD,
                         poolLights[i].farDistance, poolLights[i].oneOverDistance, len, falloff)) continue;
        falloff = lpa_spotCone(vtl, poolLights[i].lightType, len, poolLights[i].farDistance, falloff);
        if (falloff <= 0.0) continue;
        vec3 plCol = poolLights[i].color * poolLights[i].intensity * (mover ? uT.lightPoolMoverDim : 1.0);
        s += plCol * (falloff * waterGgxSpec(N, V, -vtl / max(len, 1e-3), alpha2));
    }
    return s;
}

void main(void)
{
    // o_isWater==1 base-water branch (reflection/HDRI OFF -> skipped).
    float time    = F.f0.x;
    vec3  camPos  = F.f0.yzw;
    float alphaDepth        = F.f1.x;
    float ABSORPTION_DENSITY = F.f1.y;
    float WATER_MAX_ALPHA   = F.f1.z;
    float RIPPLE_GAIN       = F.f1.w;
    float GLINT_GAIN        = F.f2.x;
    vec3  DEEP_COLOR    = F.DEEP_COLOR.xyz;
    vec3  SHALLOW_COLOR = F.SHALLOW_COLOR.xyz;
    // RUNG-6.5: global aliases feeding the verbatim-ported reflection blocks.
    u_waterReflStrength = F.f2.y;
    u_skyYaw            = F.f2.z;
    u_waterHdriLod      = F.f2.w;
    u_waterScreenSize   = F.f3.xy;
    cameraPos           = vec4(camPos, 1.0);

    PREC vec3  viewVec = camPos - WorldPos;
    //PORT-BEGIN:waterBaseColor
    PREC float waveLOD = 1.0 - smoothstep(WAVE_FADE_NEAR, WAVE_FADE_FAR, length(viewVec));


    // SHORE-FIELD-1: deep/shallow tint from the smooth shore field (mode 3), not the coarse
    // per-vertex thickness -- no more deep-water diamonds around single low vertices.
    // Evaluated for every fragment (uniform control flow) so its screen derivative is defined.
    float fieldThick;
    const bool hasField = wse_fieldThickness(int(F.f4.x + 0.5), int(F.f4.y + 0.5), u_shoreHeightTex, F.f5,
                                             WorldPos.xy, F.f7.x, fieldThick);
    const float fieldThickFw = fwidth(fieldThick);
    PREC float tintThick = hasField ? max(fieldThick, 0.0) : WaterThickness;
    // Recover horizontal shore distance from the depth field already sampled above.
    // World-space gradients keep the wash width stable across camera zoom and shore slopes.
    // Derivatives precede coverage discard so helper lanes remain defined at the coast.
    float shoreDepth = hasField ? fieldThick : WaterThickness;
    vec2 shoreDx = dFdx(WorldPos.xy), shoreDy = dFdy(WorldPos.xy);
    vec2 depthD = vec2(dFdx(shoreDepth), dFdy(shoreDepth));
    float shoreDet = shoreDx.x * shoreDy.y - shoreDx.y * shoreDy.x;
    vec2 shoreGrad = vec2(shoreDy.y * depthD.x - shoreDx.y * depthD.y,
                          shoreDx.x * depthD.y - shoreDy.x * depthD.x)
                   / (abs(shoreDet) > 1e-6 ? shoreDet : 1e-6);
    float shoreSlope = max(length(shoreGrad), 0.08);
    float shoreDistance = max(shoreDepth, 0.0) / shoreSlope;
    float foamAA = max((abs(depthD.x) + abs(depthD.y)) / shoreSlope, 1.0);

    // WATER-WAVES-GLINT: surface normal = wind wave trains (world-XZ sine slopes, no noise), and the GGX
    // roughness that carries every slope the pixel cannot resolve. The wave/ripple strength follows the ripple gain
    // (default 0.22) and the glint strength follows the glint gain (default 0.30). Low/Potato keeps flat geometry:
    // every train goes to roughness. Before the coverage discard so the normal's derivatives stay defined.
    float      footprint = (uT._lightPad1 != 0) ? 1e9 : max(length(shoreDx), length(shoreDy));
    float      waveVar;
    PREC vec2  waveSlope = waterWaveSlope(WorldPos.xy, time, footprint, RIPPLE_GAIN / 0.22, waveVar);
    PREC vec3  waveN     = normalize(vec3(-waveSlope, 1.0));  // world-XZ wave trains only: no screen-space or noise gradient
    PREC vec3  vdir      = normalize(cameraPos.xyz - WorldPos);  // MC2 Z-up
    // Specular AA: the normal's own screen-space variance (Kaplanyan-Hoffman) on top of the unresolved train variance.
    vec3  dNx = dFdx(waveN), dNy = dFdy(waveN);
    float kernelVar = 0.25 * (dot(dNx, dNx) + dot(dNy, dNy));
    float specAlpha2 = clamp(WATER_ALPHA0 * WATER_ALPHA0
                             + 2.0 * (waveVar + RIPPLE_SLOPE_VAR * (1.0 - waveLOD) * (RIPPLE_GAIN / 0.22))
                             + min(2.0 * kernelVar, 0.18), 0.0, 0.36);
    PREC float trans   = clamp(exp(-tintThick * ABSORPTION_DENSITY), 0.0, 1.0);
    PREC vec3  waterCol = mix(DEEP_COLOR, SHALLOW_COLOR, trans);

    PREC float shoreBlend = max(alphaDepth, 1.0);
    PREC float shore = smoothstep(-shoreBlend * 0.5, shoreBlend, WaterThickness);
    // WATER-SHORE-EDGE-1 / WATER-SHORE-MASK-1: shared with the GL FS.
    if (hasField) {
        // SHORE-FIELD-1: the coast is the field's zero crossing as a crisp ~3 px antialiased edge.
        // The old depth feather (smoothstep over featherWu of DEPTH) spread hundreds of wu wide on
        // gentle shelves (mc2_17) and let coarse terrain decide the visible line elsewhere. Shallow
        // water keeps some translucency through a 0.55..1 depth softness floor.
        float edgeAA = smoothstep(0.0, max(3.0 * fieldThickFw, 1e-3), fieldThick);
        shore = edgeAA * mix(0.55, 1.0, smoothstep(0.0, max(F.f4.w, 0.5), fieldThick));
    } else {
        shore = wse_shoreFeather(shore, int(F.f4.x + 0.5), int(F.f4.y + 0.5), int(F.f4.z + 0.5),
                                 u_shoreHeightTex, u_shoreMask, F.f5, F.f6, F.f4.w,
                                 WorldPos.xy, F.f7.x);
    }
    if (shore <= 0.0) discard;
    // Option A: material-local response. Shore coverage/bands and reflection tracing stay independent.
    float reactivePixelWidth = max(length(shoreDx), length(shoreDy));
    vec3 reactive = reactiveWater(WorldPos.xy, reactivePixelWidth) * waveLOD;

    // Night: the scattered light, sky ambient floor, glint, sky tint and reflected sky take the mission's night
    // dim (f3.z, 0 by day), so open water reads as dark as the ground around it instead of daylight teal.
    PREC float nightSky = 1.0 - clamp(F.f3.z, 0.0, 1.0);
    PREC vec3  vertexLightRGB = Color.bgra.rgb * nightSky;
    PREC vec3  col = waterCol * max(vertexLightRGB, vec3(SKY_AMBIENT * nightSky))
                   + waterCol * SKY_AMBIENT * 0.5 * nightSky;
    // No crest or glint brightening: highlights come only from the sun/lamp specular on the perturbed normal.
    //PORT-END:waterBaseColor
    // A shallow wash rolls toward the coast and recedes in the SAME water draw.
    // Coverage and depth stay owned by the existing shore field: no new surface to z-fight.
    float washReach = 18.0;
    float shoreRipple = 0.0;
    float foamBreakup = 1.0;
    if (F.f3.w > 0.5) {
        float coastPhase = dot(WorldPos.xy, vec2(0.003, 0.002));
        washReach = 12.0 + 30.0 * (0.5 + 0.5 * sin(time * 1.25 + coastPhase));
        shoreRipple = sin(shoreDistance * 0.18 + time * 1.8 + coastPhase) * 0.045;
        foamBreakup = 0.8;
    }
    float nearShore = 1.0 - smoothstep(36.0, 100.0, shoreDistance);
    float foam = 1.0 - smoothstep(2.0, 6.0 + foamAA, abs(shoreDistance - washReach));
    float wash = (1.0 - smoothstep(0.0, washReach + foamAA, shoreDistance)) * 0.10;
    float foamMix = (foam * 0.42 * foamBreakup + wash) * nearShore;
    vec3 foamLight = max(vertexLightRGB, vec3(SKY_AMBIENT * nightSky));
    col *= 1.0 + shoreRipple * nearShore;
    col = mix(col, vec3(0.72, 0.80, 0.78) * foamLight, foamMix);
    col = mix(col, vec3(0.72, 0.80, 0.78) * foamLight, reactive.z);
    // Static shadow on the light the surface scatters and glints (the reflected sky is untouched), with the receivers' 0.4 floor.
    // sunVis carries the same shadows (no floor) to the sun specular.
    float sunVis = 1.0;
    if (F.f7.z > 0.5) {
        vec2 maskUv = vec2(WorldPos.x + F.f7.w, F.f7.w - WorldPos.y) / max(F.f7.y, 1.0);
        if (all(greaterThanEqual(maskUv, vec2(0.0))) && all(lessThanEqual(maskUv, vec2(1.0))))
        {
            float staticLit = texture(u_staticMask, maskUv).r;
            col *= mix(sunShadowFloor(0.4), 1.0, staticLit);
            sunVis *= staticLit;
        }
    }
    // WATER-VISUAL-FIRST-SLICE sky tint (strength 0 default -> mix returns col; GL line 328).
    // Beyond the ripple fade distance a mover shadow is sub-pixel on water: skip the blocker search + PCF there.
    if (waveLOD > 0.0) {
        float moverLit = moverVisibility(WorldPos,vec3(0.0,0.0,1.0));
        col *= mix(sunShadowFloor(0.4),1.0,moverLit);
        sunVis *= moverLit;
    }
    col = mix(col, F.f9.xyz * nightSky, clamp(F.f8.w, 0.0, 1.0));

    // ---- RUNG-6.5 WATER-REFLECTION PORT (WATER-SKY-REFLECTION-1 + WATER-HDRI-REFL-1) ----
    // Sky setup and Fresnel retain the GL material math; spatial geometry is
    // coverage-blended over the sky before the Fresnel mix below.
    // Gate parity: u_waterReflStrength is fed from gos_GetWaterReflStrength()
    // (MC2_WATER_REFLECTION=0 -> 0.0 -> block skipped, same as GL).
    if (u_waterReflStrength > 0.0) {
        PREC vec3  skyReflCol = col;
        PREC vec3  reflectCol = col;
        PREC float reflMix    = 0.0;
        //PORT-BEGIN:waterReflSetup
        // WATER-WAVES-GLINT: the sky and geometry reflections follow the wave normal (trains + fine ripples);
        // the actor ripple/wake slope (reactive.xy) rides on top of it.
        PREC vec3  waveNormal = normalize(waveN + vec3(reactive.xy, 0.0));
        PREC vec3  rdir       = reflect(-vdir, waveNormal);           // MC2 Z-up
        PREC vec3  skyDir     = vec3(rdir.x, rdir.z, -rdir.y);
        if (u_waterHdriLod >= 0.0) {
            skyReflCol = waterEvalHdri(rdir);   // rdir in MC2 Z-up, skyYaw applied inside
        } else {
            skyReflCol = clamp(waterEvalSkySh(skyDir) * (1.0 / 3.14159265), 0.0, 2.0);
        }
        // WATER-GRAZE-REFL-1: bias toward the authored sky/horizon tint at grazing angles.
        PREC float grazeW = pow(1.0 - max(vdir.z, 0.0), GRAZE_SKY_EXP);
        skyReflCol = mix(skyReflCol, F.f9.xyz, GRAZE_SKY_PULL * grazeW) * nightSky;
        reflectCol = skyReflCol;   // default reflection = sky (fallback/base)
        //PORT-END:waterReflSetup
        vec4 geometry=vec4(0);
        if (F.DEEP_COLOR.w > 0.5) {
            vec2 reflectionUV=gl_FragCoord.xy/u_waterScreenSize + reactive.xy*0.015; // TREE-REFL-SOLID: no wave-normal offset (it tore the reflected canopy); only the local actor ripple bends it
            if (all(greaterThan(reflectionUV,vec2(0))) && all(lessThan(reflectionUV,vec2(1)))) {
                geometry=texture(u_waterReflection,reflectionUV);
            }
        }
        //PORT-BEGIN:waterReflMix
        PREC float fres = REFL_F0 + (1.0 - REFL_F0) * pow(1.0 - max(dot(waveN, vdir), 0.0), 5.0);
        // WATER-GRAZE-REFL-1: grazing fragments keep full reflection at distance.
        reflMix = clamp(fres * u_waterReflStrength * max(waveLOD, fres), 0.0, REFL_MAX);
        // Ripple LOD fades only the legacy sky treatment. Geometry retains
        // Schlick reflectance at distance; coverage fades smoothly to that sky.
        float geometryMix=clamp(fres*u_waterReflStrength,0.0,REFL_MAX);
        float totalMix=mix(reflMix,geometryMix,geometry.a);
        col=col*(1.0-totalMix)+reflectCol*(reflMix*(1.0-geometry.a))+geometry.rgb*geometryMix;
        //PORT-END:waterReflMix
    }

    // WATER-WAVES-GLINT: sun (moon at night, half strength) specular on the wave normal, after the reflection mix so
    // the glints sit on top of the reflected sky. The sun altitude takes the shadow clamp (mc2_10 authors a horizon sun),
    // and the radiance is the vertex sun light, so sunset tints the glitter path and shadows remove it.
    {
        float glintScale = GLINT_GAIN / 0.30;
        vec3  Ls = uT.terrainLightDir.xyz;
        Ls = normalize(vec3(Ls.xy, max(Ls.z, max(uT.shadowMinSunSin, 0.05))));
        vec3  sunRad = Color.bgra.rgb * (1.0 - 0.5 * clamp(F.f3.z / 0.6, 0.0, 1.0));
        col += min(sunRad * (SUN_SPEC_GAIN * glintScale * sunVis * waterGgxSpec(waveN, vdir, Ls, specAlpha2)), vec3(4.0));
        if (uT.numPoolLights > 0 && nightSky < 1.0)
            col += min(waterLampSpec(WorldPos, waveN, vdir, specAlpha2) * (LAMP_SPEC_GAIN * glintScale), vec3(4.0));
    }

    // The same local pools cross the shoreline; apply after sky reflection so
    // the reflected sky cannot erase lamps, then let fog/shore alpha attenuate.
    if (uT.numPoolLights > 0 && nightSky < 1.0) {
        float contact = 0.0;
        col += tlp_terrainPoolLight(WorldPos, uT.numPoolLights, uT.lightGridDim,
            vec2(uT.lightGridMinX, uT.lightGridMinY), vec2(uT.lightGridInvCellX, uT.lightGridInvCellY),
            uT.lightPoolSoft, uT.lightPoolMoverDim, uT.lightPoolMoverScale,
            uT.lightPoolMoverMax, uT.lightPoolCompress, 0.0, contact, vec3(0,0,1), vec3(0,0,1), false, 0.0);
    }
    if (F.fog_color.x > 0.0 || F.fog_color.y > 0.0 || F.fog_color.z > 0.0 || F.fog_color.w > 0.0)
        col = mix(F.fog_color.rgb, col, FogValue);

    // WATER-SHALLOW-1: opacity follows the water column (shaders/include/water_shallow.hglsl); deep water is unchanged.
    PREC float waterA = shore * WATER_MAX_ALPHA * ws_column_veil(tintThick);
    // WATER-EDGE-FEATHER-1 (default ON in GL): tint + dissolve the map-edge rim.
    if (F.f8.x != 0.0 && F.f8.z > 0.0) {
        float h = clamp(edgeHazeAmount(WorldPos.xy, F.f8.z) * F.f8.y, 0.0, 1.0);
        col    = mix(col, EDGE_HAZE_SKY, h);
        waterA *= (1.0 - h);
    }
    // COMPOSITE-STAGE PARITY: emit PREMULTIPLIED alpha (col*a, a) into the transparent
    // water-only target (pipeline blend ONE / ONE_MINUS_SRC_ALPHA). GL then premult-over-
    // composites onto MainColor (GL_ONE / GL_ONE_MINUS_SRC_ALPHA) == GL's native one-stage
    // col*a + bg*(1-a) blend, bit-exact. (Straight-alpha two-stage double-applied alpha.)
    FragColor = vec4(col * waterA, waterA);
    // Water class: still > 0.75 (shadow-handled, terrain-class for screen shadow / shoreline / decals) but distinct,
    // so the motion pass can mark water in the reactive mask. Only A is written (pipeline mask), rgb stays.
    GBuffer1 = vec4(0.5, 0.5, 1.0, 0.875);
}
