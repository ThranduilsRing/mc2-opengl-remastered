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
#extension GL_EXT_control_flow_attributes : require
#ifdef WATER_EARLY_TESTS
// Fine-surface coverage comes from scene depth; its shore discard is dead.
layout(early_fragment_tests) in;
#endif
layout(constant_id = 11) const int kReflectionCoverage = 0;
layout(constant_id = 13) const int kWaterDbgPass = 0;
#define PREC highp
#define TERRAIN_RESOURCE_SET 1
#define TERRAIN_OBJECT_RECEIVER 1
#include "include/terrain_frame_vk.glsl"
#include "../include/render_contract.hglsl"
layout(set = 1, binding = 1, std430) readonly buffer TerrainHeightBuf { float heights[]; } uH;
layout(set = 1, binding = 10, std430) readonly buffer TerrainVisualHeightBuf { float heightsFine[]; };
float sampleH(int x, int y) {
    return uH.heights[clamp(x, 0, uT.mapSide - 1) + clamp(y, 0, uT.mapSide - 1) * uT.mapSide];
}
#include "include/terrain_visual_surface.glsl"
layout(set = 0, binding = 14, std430) readonly buffer WaterShoreSurface { float shoreSurface[]; };
float waterBakedHeight(ivec2 p) {
    p = clamp(p, ivec2(0), ivec2(uT.visualSide - 1));
    return shoreSurface[p.x + p.y * uT.visualSide];
}
float waterSurfaceHeight(int fx, int fy) {
    float fine = waterBakedHeight(ivec2(fx, fy));
    if (hiresBlendW() == 1.0 && hiresEdgeW(fx, fy) == 1.0) return fine;
    int cx = min(fx / 4, uT.mapSide - 2), cy = min(fy / 4, uT.mapSide - 2);
    float u = float(fx - cx * 4) * 0.25, v = float(fy - cy * 4) * 0.25;
    float h00 = sampleH(cx, cy), h10 = sampleH(cx + 1, cy);
    float h01 = sampleH(cx, cy + 1), h11 = sampleH(cx + 1, cy + 1);
    float hc = (v >= u) ? h00 + (h11 - h01) * u + (h01 - h00) * v
                        : h00 + (h10 - h00) * u + (h11 - h10) * v;
    return mix(hc, fine, hiresBlendW() * hiresEdgeW(fx, fy));
}
vec4 waterSurfaceRow(ivec2 p) {
    vec4 result = vec4(waterBakedHeight(p), waterBakedHeight(p + ivec2(1, 0)),
                       waterBakedHeight(p + ivec2(2, 0)), waterBakedHeight(p + ivec2(3, 0)));
    // At full displacement, interior vertices have no coarse-surface contribution.
    if (hiresBlendW() == 1.0 && hiresEdgeW(p.x, p.y) == 1.0 && hiresEdgeW(p.x + 3, p.y) == 1.0)
        return result;
    [[unroll]] for (int x = 0; x < 4; ++x) {
        int fx = p.x + x, fy = p.y;
        int cx = min(fx / 4, uT.mapSide - 2), cy = min(fy / 4, uT.mapSide - 2);
        float u = float(fx - cx * 4) * 0.25, v = float(fy - cy * 4) * 0.25;
        float h00 = sampleH(cx, cy), h10 = sampleH(cx + 1, cy);
        float h01 = sampleH(cx, cy + 1), h11 = sampleH(cx + 1, cy + 1);
        float hc = (v >= u) ? h00 + (h11 - h01) * u + (h01 - h00) * v
                            : h00 + (h10 - h00) * u + (h11 - h10) * v;
        result[x] = mix(hc, result[x], hiresBlendW() * hiresEdgeW(fx, fy));
    }
    return result;
}
void waterSurfaceCache(ivec2 p, out mat4 raw, out mat4 surface) {
    [[unroll]] for (int y = 0; y < 4; ++y) {
        ivec2 q = p + ivec2(0, y);
        raw[y] = vec4(cliffRawHeight(q), cliffRawHeight(q + ivec2(1, 0)),
                      cliffRawHeight(q + ivec2(2, 0)), cliffRawHeight(q + ivec2(3, 0)));
        surface[y] = waterSurfaceRow(q);
    }
}
vec4 waterCell(mat4 rows, ivec2 k) {
    // Constant component accesses avoid dynamically indexing a per-fragment array.
    vec4 top = k.y == 0 ? rows[0] : (k.y == 1 ? rows[1] : (k.y == 3 ? rows[3] : rows[2]));
    vec4 bottom = k.y == 0 ? rows[1] : (k.y == 1 ? rows[2] : rows[3]);
    if (k.x == 3) return vec4(top.ww, bottom.ww);
    return k.x == 0 ? vec4(top.xy, bottom.xy)
                   : (k.x == 1 ? vec4(top.yz, bottom.yz) : vec4(top.zw, bottom.zw));
}
float waterHeightCell(vec4 r, vec4 h, vec2 t) {
    float rawTL = r.x, rawTR = r.y, rawBL = r.z, rawBR = r.w;
    if (abs(rawTR - rawBL) < abs(rawTL - rawBR)) {
        float tr = h.y, bl = h.z;
        if (t.x + t.y <= 1.0) {
            float tl = h.x;
            return tl + (tr - tl) * t.x + (bl - tl) * t.y;
        }
        float br = h.w;
        return br + (bl - br) * (1.0 - t.x) + (tr - br) * (1.0 - t.y);
    }
    float a = h.x, d = h.w;
    float b = t.y >= t.x ? h.z : h.y;
    return t.y >= t.x ? a + (d - b) * t.x + (b - a) * t.y
                      : a + (b - a) * t.x + (d - b) * t.y;
}
float waterFineHeight(vec2 worldXY);
float waterCachedHeight(vec2 worldXY, ivec2 origin, mat4 raw, mat4 surface) {
    // Keep each tap's coordinate arithmetic and triangle interpolation identical.
    vec2 grid = clamp(vec2(worldXY.x + uT.halfMap, uT.halfMap - worldXY.y) / 32.0,
                      vec2(0.0), vec2(uT.visualSide - 1));
    ivec2 p = min(ivec2(floor(grid)), ivec2(uT.visualSide - 2));
    vec2 t = grid - vec2(p);
    ivec2 k = p - origin;
    if (any(lessThan(k, ivec2(0))) || any(greaterThan(k, ivec2(3)))
        || (k.x == 3 && t.x != 0.0) || (k.y == 3 && t.y != 0.0))
        return waterFineHeight(worldXY);
    return waterHeightCell(waterCell(raw, k), waterCell(surface, k), t);
}
mat4 waterRawCache(ivec2 p) {
    mat4 rows;
    [[unroll]] for (int y = 0; y < 4; ++y)
        rows[y] = vec4(cliffRawHeight(p + ivec2(0, y)), cliffRawHeight(p + ivec2(1, y)),
                       cliffRawHeight(p + ivec2(2, y)), cliffRawHeight(p + ivec2(3, y)));
    return rows;
}
float waterRawCenter(vec2 worldXY) {
    vec2 grid = clamp(vec2(worldXY.x + uT.halfMap, uT.halfMap - worldXY.y) / 32.0,
                      vec2(0.0), vec2(uT.visualSide - 1));
    ivec2 p = min(ivec2(floor(grid)), ivec2(uT.visualSide - 2));
    vec4 r = vec4(cliffRawHeight(p), cliffRawHeight(p + ivec2(1, 0)),
                  cliffRawHeight(p + ivec2(0, 1)), cliffRawHeight(p + ivec2(1, 1)));
    return waterHeightCell(r, r, grid - vec2(p));
}
float waterFineHeight(vec2 worldXY) {
    vec2 grid = clamp(vec2(worldXY.x + uT.halfMap, uT.halfMap - worldXY.y) / 32.0,
                      vec2(0.0), vec2(uT.visualSide - 1));
    ivec2 p = min(ivec2(floor(grid)), ivec2(uT.visualSide - 2));
    vec2 t = grid - vec2(p);
    // Patch indices choose the flatter diagonal from RAW bake heights.
    float rawTL = cliffRawHeight(p), rawTR = cliffRawHeight(p + ivec2(1, 0));
    float rawBL = cliffRawHeight(p + ivec2(0, 1)), rawBR = cliffRawHeight(p + ivec2(1, 1));
    if (abs(rawTR - rawBL) < abs(rawTL - rawBR)) {
        float tr = waterSurfaceHeight(p.x + 1, p.y), bl = waterSurfaceHeight(p.x, p.y + 1);
        if (t.x + t.y <= 1.0) {
            float tl = waterSurfaceHeight(p.x, p.y);
            return tl + (tr - tl) * t.x + (bl - tl) * t.y;
        }
        float br = waterSurfaceHeight(p.x + 1, p.y + 1);
        return br + (bl - br) * (1.0 - t.x) + (tr - br) * (1.0 - t.y);
    }
    float a = waterSurfaceHeight(p.x, p.y), d = waterSurfaceHeight(p.x + 1, p.y + 1);
    float b = t.y >= t.x ? waterSurfaceHeight(p.x, p.y + 1) : waterSurfaceHeight(p.x + 1, p.y);
    return t.y >= t.x ? a + (d - b) * t.x + (b - a) * t.y
                      : a + (b - a) * t.x + (d - b) * t.y;
}

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
    vec4 SHALLOW_COLOR;   // .xyz color, .w baked reflection weather scale B
    // VK-PARITY-WATER-SHORE-1 (WATER-SHORE-EDGE-1): shore-feather inputs.
    vec4 f4;              // (u_shoreMode, u_hasShoreHeight, u_hasShoreMask, u_shoreFeatherWu)
    vec4 f5;              // u_shoreHeightBounds (topLeftX, topLeftY, spanX, spanY)
    vec4 f6;              // u_shoreMaskBounds   (topLeftX, topLeftY, sizeX, sizeY)
    vec4 f7;              // (waterElevation, mask span (wu), mask bound (0/1), terrain grid halfMap)
    // VK-PARITY-F3 (water brightness): GL-default features missing in VK.
    vec4 f8;              // (u_waterEdgeFeather, u_waterEdgeFeatherStrength, u_halfMap, u_waterSkyTintStrength)
    vec4 f9;              // (u_waterSkyTintColor.xyz, waterWaveBreakup: the island run overwrites projAspect, unread by this shader)
    vec4 bake;            // world span, halfMap, baked reflection weather scale RG
    vec4 debug;           // x attribution (z = baked-reflection march steps, 0 = 8, from Reflection quality); x: 0 normal; 1 march; 2 HDRI; 3 waves; 4 movers; 5 shore; 6 post-discard
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
layout(set = 0, binding = 9) uniform sampler2D u_waterReflectHeight;
layout(set = 0, binding = 17) uniform sampler2DArray u_wavePackets;
layout(set = 0, binding = 18) uniform sampler2D u_shoreFoam;
#include "../include/water_fft.hglsl"

vec2 bakedReflectionUV(vec2 p) {
    return vec2(p.x + F.bake.y, F.bake.y - p.y) / F.bake.x;
}

vec4 bakedReflection(vec3 origin, vec3 ray) {
    if (ray.z <= 0.001) return vec4(0);
    // A conservative max over the whole map bounds the finite ray segment.
    int lastMip = textureQueryLevels(u_waterReflectHeight) - 1;
    float maximum = texelFetch(u_waterReflectHeight, ivec2(0), lastMip).r;
    if (maximum <= 0) return vec4(0);
    float reach = min(F.bake.x, maximum / ray.z);
    float previousT = 0;
    float previousGap = max(origin.z - F.f7.x, 0.0);
    int steps = F.debug.z > 0.5 ? int(F.debug.z) : 8;   // reflection quality: Low 4, Medium 6, High 8
    for (int i = 1; i <= steps; ++i) {
        // Shorter steps at the receiver retain nearby low banks.
        float fraction = float(i) / float(steps);
        float t = reach * fraction * fraction;
        vec3 p = origin + ray * t;
        vec2 uv = bakedReflectionUV(p.xy);
        if (any(lessThan(uv, vec2(0))) || any(greaterThanEqual(uv, vec2(1)))) break;
        float height = textureLod(u_waterReflectHeight, uv, 0).r;
        float gap = p.z - F.f7.x - height;
        if (height > 0 && gap <= 0) {
            // One secant refinement removes the fixed-step silhouette bands.
            float hit = mix(previousT, t, clamp(previousGap / max(previousGap - gap, 0.001), 0, 1));
            return textureLod(u_waterReflection, bakedReflectionUV((origin + ray * hit).xy), 0);
        }
        previousT = t; previousGap = gap;
    }
    return vec4(0);
}
#include "../include/water_shore_edge.hglsl"
#include "../include/water_reactive.hglsl"
#include "../include/water_shallow.hglsl"

struct ReactiveStamp { vec4 positionRadiusAge; vec4 velocityType; };
layout(set = 0, binding = 13, std430) readonly buffer WaterReactive {
    ReactiveStamp stamps[128];
    uint counts[1024];
    uint indices[32768];   // 1024 tiles x kPerTile (RenderCore/water_reactive.h)
} R;

vec3 reactiveWater(vec2 worldXY, float pixelWidth) {
    ivec2 cell = ivec2(floor(worldXY / 256.0));
    uint tile = (uint(cell.y) & 31u) * 32u + (uint(cell.x) & 31u);
    vec3 result = vec3(0.0);
    for (uint i = 0u; i < min(R.counts[tile], 32u); ++i) {
        ReactiveStamp s = R.stamps[R.indices[tile * 32u + i]];
        vec2 delta = worldXY - s.positionRadiusAge.xy;
        if (dot(delta, delta) < 224.0 * 224.0)
            result += wr_profile(delta, s.velocityType.xy, s.positionRadiusAge.z,
                                 s.positionRadiusAge.w, s.velocityType.z, pixelWidth);
    }
    return wr_combine(result);
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
// Analytic sine slopes with stationary, smoothly hashed short-wave packets (no history). Each train: (wind dir x, y, wavelength wu, slope amplitude
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
const float WAVE_PACKET_MIN  = 0.65;
const float WAVE_PACKET_MAX  = 1.35;
const float WAVE_PACKET_SPAN = 6.0;     // wavelengths per smooth packet, no corner or time-dependent reseeding
const float WATER_ALPHA0     = 0.06;    // GGX alpha of the calm surface
const float RIPPLE_SLOPE_VAR = 0.004;   // slope variance of the fine ripples once they fade with distance
const float SUN_SPEC_GAIN    = 1.0;     // one energy-normalised sun/moon lobe at the default glint gain (0.30)
const float LAMP_SPEC_GAIN   = 1.5;     // lamp/weapon pool-light specular scale at the default glint gain
// WATER-GLOSS-NEAR: water near the camera is seen steeply, where water's 0.02 Schlick reflectance leaves it a matte
// body colour. By day, within NEAR_GLOSS_END of the camera, the reflectance gets a floor (only steep views fall below
// it, so grazing far water keeps its Schlick value) and the calm-surface roughness tightens. Night is untouched.
const float NEAR_GLOSS_START = 6000.0;  // wu from the camera: full near gloss inside
const float NEAR_GLOSS_END   = 12000.0; // wu: no near gloss beyond (far water unchanged)
const float NEAR_REFL_FLOOR  = 0.12;    // reflectance floor for close, steeply viewed water
const float NEAR_ALPHA0      = 0.04;    // GGX alpha of the calm surface close to the camera
// Near gloss belongs to reflected sky and the calm-surface roughness, not an additional sun.
// A second broad GGX lobe added energy across the channel even outside the primary glitter path.
const float NEAR_SKY_LIFT     = 0.4;    // reflected-sky brightening at full near gloss

// waterWaveBreakup (F.f9.w; 0 = legacy path verbatim): static world-XY value noise, smooth, no time term.
float waveBreakNoise(vec2 q){
    vec2 c = floor(q), f = fract(q);
    f = f * f * (3.0 - 2.0 * f);
    vec4 h = fract(sin(vec4(dot(c, vec2(127.1, 311.7)), dot(c + vec2(1, 0), vec2(127.1, 311.7)),
                            dot(c + vec2(0, 1), vec2(127.1, 311.7)), dot(c + vec2(1, 1), vec2(127.1, 311.7)))) * 43758.5453);
    return mix(mix(h.x, h.y, f.x), mix(h.z, h.w, f.x), f.y);
}

PREC vec2 waterWaveSlope(PREC vec2 p, float t, float footprint, float gain, out float unresolvedVar){
    float packetSize = float(textureSize(u_wavePackets, 0).x);
    PREC vec2 s = vec2(0.0);
    unresolvedVar = 0.0;
    const float brk = F.f9.w;
    float brkGain = 1.0, brkRot = 0.0;
    if (brk > 0.0) {
        // Calmer and choppier patches (about 1700 wu) and a few degrees of wind rotation (about 2600 wu), stationary.
        brkGain = 1.0 + brk * (2.0 * waveBreakNoise(p / 1700.0 + vec2(3.7, 9.1)) - 1.0);
        brkRot  = brk * 0.2 * (2.0 * waveBreakNoise(p / 2600.0 + vec2(41.3, 5.9)) - 1.0);
    }
    for (int i = 0; i < WAVE_TRAINS; ++i) {
        vec4  w = kWaveTrain[i];
        float brkA = 1.0;
        if (brk > 0.0 && i >= 2) {
            float cr = cos(brkRot), sr = sin(brkRot);
            w.xy = vec2(w.x * cr - w.y * sr, w.x * sr + w.y * cr);
            brkA = brkGain;
        }
        float k = 6.2831853 / w.z;
        float r = smoothstep(WAVE_RESOLVE_LO, WAVE_RESOLVE_HI, w.z / max(footprint, 1e-3));
        // Independent packet envelopes break the global lattice without noise-gradient normals.
        // Long swells retain their vertex-stage phase and amplitude; unresolved packet energy
        // goes into roughness using the very same local amplitude as the resolved normal.
        float packet = 1.0;
        if (i >= 2) {
            float scale = WAVE_PACKET_SPAN * w.z;
            vec2 offset = float(i) * vec2(19.19, 7.73);
            vec2 lattice = p / scale + offset;
            vec2 origin = floor(vec2(-uT.halfMap) / scale + offset);
            vec2 f = fract(lattice);
            vec2 uv = (floor(lattice) - origin + f * f * (3.0 - 2.0 * f) + 0.5) / packetSize;
            float noise = textureLod(u_wavePackets, vec3(uv, (i - 2) / 4), 0)[(i - 2) % 4];
            packet = mix(WAVE_PACKET_MIN, WAVE_PACKET_MAX, noise);
        }
        float a = w.w * gain * packet * brkA;
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
// WATER-POOL-FUSE-1: ONE walk of the cell's lights returns both terms. Before, the GGX walk and tlp_terrainPoolLight
// (flat diffuse pool, surfaceNormal==macroNormal, no contact shadow, no local-shadow atlas) each re-fetched every light and
// re-ran the same attenuation and spot cone: with 48 mechs firing that doubled the water pass. Both bodies are kept
// term-for-term (tlp_terrainPoolLight in terrain_light_pool.hglsl): `spec` is the lamp specular sum, the return is the
// pool term with the soft-compress / dither tail.
vec3 waterPoolLights(vec3 wp, vec3 N, vec3 V, float alpha2, out vec3 spec){
    spec = vec3(0.0);
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
    vec3 pool = vec3(0.0), poolMover = vec3(0.0);
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
        vec3 plCol = poolLights[i].color * poolLights[i].intensity;
        if (mover) plCol *= uT.lightPoolMoverDim;
        spec += plCol * (falloff * waterGgxSpec(N, V, -vtl / max(len, 1e-3), alpha2));
        if (mover && uT.lightPoolMoverMax != 0) { poolMover = max(poolMover, plCol * falloff); continue; }
        pool += plCol * falloff;
    }
    pool += poolMover;
    if (uT.lightPoolCompress != 0) return lpa_dither(vec3(1.0) - exp(-1.6 * pool));
    return lpa_dither(min(pool, vec3(1.0)));
}

void main(void)
{
    int debugMode = int(F.debug.x);
    if (debugMode == 7) { FragColor = vec4(0.0); GBuffer1 = vec4(0.0); return; }
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
    const bool hasVisual = uT.visualMode != 0 && uT.visualSide > 1;
    // Visual-surface shore coverage is always positive; depth owns its silhouette.
    if (kReflectionCoverage != 0 && hasVisual) { FragColor = vec4(1.0); GBuffer1 = vec4(0.0); return; }
    float fieldThick = 0.0;
    vec2 hn = vec2((WorldPos.x - F.f5.x) / max(F.f5.z, 1e-5),
                   (F.f5.y - WorldPos.y) / max(F.f5.w, 1e-5));
    const bool hasField = int(F.f4.x + 0.5) == 3 && int(F.f4.y + 0.5) != 0
                      && !(hn.x < 0.0 || hn.x > 1.0 || hn.y < 0.0 || hn.y > 1.0);
    if (hasField && !hasVisual)
        fieldThick = F.f7.x - wse_bicubicHeight(u_shoreHeightTex, hn);
    // The cooked SDF predates raster-depth coverage. Its zero crossing can sit on
    // dry land, leaving a shallow strip and a detached foam contour in wet pixels.
    // Read shore inputs from the same filtered fine surface as the terrain.
    mat4 shoreRaw, shoreSurface;
    ivec2 shoreOrigin = ivec2(0);
    float fineCenter = 0.0;
    bool tent = true;
    if (hasVisual && hasField) {
        vec2 grid = clamp(vec2(WorldPos.x - 32.0 + uT.halfMap, uT.halfMap - (WorldPos.y + 32.0)) / 32.0,
                          vec2(0.0), vec2(uT.visualSide - 1));
        shoreOrigin = min(ivec2(floor(grid)), ivec2(uT.visualSide - 2));
        int record = uT.visualSide * uT.visualSide + 2 * (shoreOrigin.x + shoreOrigin.y * uT.visualSide);
        float threshold = heightsFine[record], flat4 = heightsFine[record + 1];
        bool full = debugMode != 5 && (hiresBlendW() != 1.0 || flat4 == 0.0);
        tent = debugMode != 5 && (hiresBlendW() != 1.0 || F.f7.x < threshold); // partial far blend mixes coarse heights: thresholds bound only the fine surface
        if (full && !tent) {
            fineCenter = waterFineHeight(WorldPos.xy);
        } else if (full) {
            waterSurfaceCache(shoreOrigin, shoreRaw, shoreSurface);
            fineCenter = waterCachedHeight(WorldPos.xy, shoreOrigin, shoreRaw, shoreSurface);
        } else if (tent) {
            shoreRaw = waterRawCache(shoreOrigin);
            shoreSurface = shoreRaw;
            fineCenter = waterCachedHeight(WorldPos.xy, shoreOrigin, shoreRaw, shoreSurface);
        } else {
            fineCenter = waterRawCenter(WorldPos.xy);
        }
        fieldThick = F.f7.x - fineCenter;
    }
    // Raster depth still owns coverage, including patch stitching and morph.
    float softnessThick = hasVisual && !hasField ? F.f7.x - waterFineHeight(WorldPos.xy) : fieldThick;
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
    // The fine surface is a 32-unit bilinear grid: its screen gradient jumps per triangle and the contour of its
    // depth is a polygon. Take shoreDepth and its gradient from a 3x3 tent blur of the same height (C1, rounded
    // contours, continuous gradient); coverage and tint keep the raw fine height.
    if (hasVisual && hasField && tent) {
        const float gh = 32.0;
        vec2 wp = WorldPos.xy;
        float h00 = waterCachedHeight(wp + vec2(-gh, -gh), shoreOrigin, shoreRaw, shoreSurface);
        float h10 = waterCachedHeight(wp + vec2(0.0, -gh), shoreOrigin, shoreRaw, shoreSurface);
        float h20 = waterCachedHeight(wp + vec2(gh, -gh), shoreOrigin, shoreRaw, shoreSurface);
        float h01 = waterCachedHeight(wp + vec2(-gh, 0.0), shoreOrigin, shoreRaw, shoreSurface);
        float h11 = fineCenter;
        float h21 = waterCachedHeight(wp + vec2(gh, 0.0), shoreOrigin, shoreRaw, shoreSurface);
        float h02 = waterCachedHeight(wp + vec2(-gh, gh), shoreOrigin, shoreRaw, shoreSurface);
        float h12 = waterCachedHeight(wp + vec2(0.0, gh), shoreOrigin, shoreRaw, shoreSurface);
        float h22 = waterCachedHeight(wp + vec2(gh, gh), shoreOrigin, shoreRaw, shoreSurface);
        shoreDepth = F.f7.x - (h00 + 2.0 * h10 + h20 + 2.0 * h01 + 4.0 * h11 + 2.0 * h21 + h02 + 2.0 * h12 + h22) / 16.0;
        shoreGrad = -vec2((h20 + 2.0 * h21 + h22) - (h00 + 2.0 * h01 + h02),
                          (h02 + 2.0 * h12 + h22) - (h00 + 2.0 * h10 + h20)) / (8.0 * gh);
    }
    if (hasVisual && hasField && !tent) shoreGrad = vec2(0.0);
    float shoreSlope = max(length(shoreGrad), 0.08);
    float shoreDistance = max(shoreDepth, 0.0) / shoreSlope;
    float foamAA = max(max(length(shoreDx), length(shoreDy)), 1.0);

    // WATER-WAVES-GLINT: surface normal = wind wave trains (world-XZ sine slopes, no noise), and the GGX
    // roughness that carries every slope the pixel cannot resolve. The wave/ripple strength follows the ripple gain
    // (default 0.22) and the glint strength follows the glint gain (default 0.30). Low/Potato keeps flat geometry:
    // every train goes to roughness. Before the coverage discard so the normal's derivatives stay defined.
    float      footprint = (uT._lightPad1 != 0) ? 1e9 : max(length(shoreDx), length(shoreDy));
    float      waveVar = 0.0;
    PREC vec2  waveSlope = vec2(0.0);
    if (debugMode != 3) waveSlope = waterWaveSlope(WorldPos.xy, time, footprint, RIPPLE_GAIN / 0.22, waveVar);
    int shorelineLook = max(int(F.f3.w + 0.5) - 1, 0);
    float coastWeight = (1.0 - (shorelineLook == 1 ? smoothstep(170.0, 260.0, shoreDistance)   // coastal look: reaches the shelf break
                                                   : smoothstep(36.0, 100.0, shoreDistance)))
                      * smoothstep(0.04, 0.08, length(shoreGrad));
    float swashPhase = time * 1.05 + dot(WorldPos.xy, vec2(0.003, 0.002));
    float swashFront = 18.0 + 14.0 * sin(swashPhase);
    // Positive phase velocity moves each broken crest toward decreasing shore distance.
    float breakerPhase = shoreDistance * (6.2831853 / 145.0) + swashPhase;
    if (shorelineLook > 0 && debugMode != 3)
        waveSlope += shoreGrad / max(length(shoreGrad), 1e-4) * cos(breakerPhase)
                   * 0.10 * coastWeight * (1.0 - smoothstep(8.0, 24.0, footprint));
    // Coastal look (after gokayfem coastal-water, MIT): eight dispersive ripple trains spread on the golden angle (no
    // wind streaks), each faded as its wavelength nears the pixel footprint; the faded slope goes to the GGX roughness.
    float rippleCrest = 0.0;
    if (shorelineLook == 1 && debugMode != 3) {
        waveSlope *= 0.3; waveVar *= 0.09;
        for (int i = 0; i < 10; ++i) {   // trains 8-9 (L 6.6-4.5) break the near glint into sparkle
            float L = 90.0 * pow(0.69, float(i)), k = 6.2831853 / L, s = 0.055 * pow(0.84, float(i)) * (RIPPLE_GAIN / 0.22);
            vec2  d = vec2(cos(0.7 + 2.39996 * float(i)), sin(0.7 + 2.39996 * float(i)));
            float keep = smoothstep(2.0, 6.0, L / max(footprint, 1e-3));
            float ph = k * dot(d, WorldPos.xy + float(i) * vec2(31.0, 17.0)) - sqrt(WAVE_G * k) * time;
            waveSlope += d * (s * keep * cos(ph));
            if (i < 8) rippleCrest += s * k * keep * sin(ph);   // -Laplacian: crest convexity
            waveVar += 0.5 * s * s * (1.0 - keep * keep);
        }
        rippleCrest /= 0.0675 * max(RIPPLE_GAIN / 0.22, 1e-3);   // sum of s*k over the trains: 1 where every crest meets
    }
    float foldingFoam = 0.0;
    if (shorelineLook == 2 && debugMode != 3 && F.debug.y < 0.5) {
        vec3 dx=(fftShore(WorldPos.xy+vec2(1,0),shoreDepth)-fftShore(WorldPos.xy-vec2(1,0),shoreDepth))*0.5;
        vec3 dy=(fftShore(WorldPos.xy+vec2(0,1),shoreDepth)-fftShore(WorldPos.xy-vec2(0,1),shoreDepth))*0.5;
        float jacobian=(1.0+dx.x)*(1.0+dy.y)-dx.y*dy.x;
        foldingFoam=1.0-smoothstep(0.25,0.85,jacobian);
        float resolved=1.0-smoothstep(3.0,12.0,footprint);
        waveSlope=mix(waveSlope,waveSlope*0.6+vec2(dx.z,dy.z)*resolved,coastWeight);
    }
    PREC vec3  waveN     = normalize(vec3(-waveSlope, 1.0));  // world-XZ wave trains only: no screen-space or noise gradient
    PREC vec3  vdir      = normalize(cameraPos.xyz - WorldPos);  // MC2 Z-up
    // Specular AA: the normal's own screen-space variance (Kaplanyan-Hoffman) on top of the unresolved train variance.
    vec3  dNx = dFdx(waveN), dNy = dFdy(waveN);
    float kernelVar = 0.25 * (dot(dNx, dNx) + dot(dNy, dNy));
    float nearGloss = (1.0 - smoothstep(NEAR_GLOSS_START, NEAR_GLOSS_END, length(viewVec)))
                    * (1.0 - clamp(F.f3.z / 0.6, 0.0, 1.0));
    float alpha0 = mix(WATER_ALPHA0, NEAR_ALPHA0, nearGloss);
    float specAlpha2 = clamp(alpha0 * alpha0
                             + 2.0 * (waveVar + RIPPLE_SLOPE_VAR * (1.0 - waveLOD) * (RIPPLE_GAIN / 0.22))
                             + min(2.0 * kernelVar, 0.18), 0.0, 0.36);
    // Coastal night (f3.z: the mission night dim, 0 by day): moonlight, a soft moon path, a dim desaturated column, faint foam.
    float coastNight = shorelineLook == 1 ? clamp(F.f3.z / 0.6, 0.0, 1.0) : 0.0;
    if (shorelineLook == 1) specAlpha2 = max(specAlpha2, mix(0.008, 0.04, coastNight));   // coastal: no mirror-sharp blobs on the long trains
    PREC float trans   = clamp(exp(-tintThick * ABSORPTION_DENSITY), 0.0, 1.0);
    PREC vec3  waterCol = mix(DEEP_COLOR, SHALLOW_COLOR, trans);
    if (shorelineLook > 0) {
        vec3 lagoon = mix(vec3(0.055, 0.24, 0.23), SHALLOW_COLOR, smoothstep(0.0, 24.0, max(shoreDepth, 0.0)));
        waterCol = mix(waterCol, lagoon, coastWeight * 0.65);
    }
    // Coastal look: the terrain absorbs the seabed per channel (terrain_tier1.frag); the water adds only the light the
    // column scatters back, jade over the shelf turning navy offshore (owner refs E:/mc2-ref/refs/water).
    if (shorelineLook == 1)
        waterCol = mix(vec3(0.03, 0.26, 0.24), vec3(0.008, 0.075, 0.17), smoothstep(16.0, 90.0, tintThick))
                 * (1.0 - exp(-vec3(0.07, 0.022, 0.024) * tintThick * (1.0 + 1.0 / max(vdir.z, 0.25))))
                 * (1.0 - 0.4 * coastNight);
    if (shorelineLook == 1) waterCol = mix(waterCol, dot(waterCol, vec3(0.2126, 0.7152, 0.0722)) * vec3(0.6, 0.9, 1.4), 0.6 * coastNight);

    PREC float shoreBlend = max(alphaDepth, 1.0);
    PREC float shore = smoothstep(-shoreBlend * 0.5, shoreBlend, WaterThickness);
    // WATER-SHORE-EDGE-1 / WATER-SHORE-MASK-1: shared with the GL FS.
    if (hasField || hasVisual) {
        // Retain the authored softness. Only coarse terrain needs the field's edge;
        // a static height-field edge disagrees with stitched/morphed visual patches.
        float edgeAA = hasVisual ? 1.0 : smoothstep(0.0, max(3.0 * fieldThickFw, 1e-3), fieldThick);
        shore = edgeAA * mix(0.55, 1.0, smoothstep(0.0, max(F.f4.w, 0.5), softnessThick));
    } else {
        shore = wse_shoreFeather(shore, int(F.f4.x + 0.5), int(F.f4.y + 0.5), int(F.f4.z + 0.5),
                                 u_shoreHeightTex, u_shoreMask, F.f5, F.f6, F.f4.w,
                                 WorldPos.xy, F.f7.x);
    }
    if (kWaterDbgPass != 1 && shore <= 0.0) discard;
    if (debugMode == 6) { FragColor = vec4(0.0); GBuffer1 = vec4(0.0); return; }
    if (kWaterDbgPass == 1 || kWaterDbgPass == 2) {
        FragColor = vec4(1.0, 0.0, 1.0, 1.0); GBuffer1 = vec4(0.0); return;
    }
    if (kReflectionCoverage != 0) { FragColor = vec4(1.0); GBuffer1 = vec4(0.0); return; }
    // Option A: material-local response. Shore coverage/bands and reflection tracing stay independent.
    float reactivePixelWidth = max(length(shoreDx), length(shoreDy));
    vec3 reactive = (debugMode == 3 || F.debug.y > 0.5) ? vec3(0.0) : reactiveWater(WorldPos.xy, reactivePixelWidth) * waveLOD;

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
    if (F.f3.w > 0.5 && shorelineLook == 0) {
        float coastPhase = dot(WorldPos.xy, vec2(0.003, 0.002));
        washReach = 12.0 + 30.0 * (0.5 + 0.5 * sin(time * 1.25 + coastPhase));
        shoreRipple = sin(shoreDistance * 0.18 + time * 1.8 + coastPhase) * 0.045;
        foamBreakup = 0.8;
    }
    // A flat shelf has no real coast: the slope floor would place the wash contour at a fixed
    // depth far offshore, tracing seabed relief. Fade the wash out where the true slope is below the floor.
    float shelfFade = smoothstep(0.04, 0.08, length(shoreGrad));
    float nearShore = (1.0 - smoothstep(36.0, 100.0, shoreDistance)) * shelfFade;
    float foam = 1.0 - smoothstep(2.0, 6.0 + foamAA, abs(shoreDistance - washReach));
    float wash = (1.0 - smoothstep(0.0, washReach + foamAA, shoreDistance)) * 0.10;
    float foamMix = (foam * 0.42 * foamBreakup + wash) * nearShore;
    if (shorelineLook > 0) {
        foamMix = 0.0;
        shoreRipple = 0.0;
        vec2 beachward = -shoreGrad / max(length(shoreGrad), 1e-4);
        vec2 q = WorldPos.xy / 155.0 - beachward * time * 0.035;
        // Derivatives stay uniform; open water needs neither texture fetches nor foam evaluation.
        vec2 foamDx = dFdx(q), foamDy = dFdy(q);
        // Coastal look: the shelf-edge break sits where the turquoise turns blue, often beyond the coast band.
        float shelfBreak = shorelineLook == 1 ? 17.0 + 7.0 * vnoise(WorldPos.xy / 700.0) : 0.0;
        if (coastWeight > 0.0 || abs(shoreDepth - shelfBreak) < 6.0) {
            vec4 foamTex = textureGrad(u_shoreFoam, fract(q), foamDx, foamDy);
            float lace = textureGrad(u_shoreFoam, fract(q*2.31+0.37), foamDx*2.31, foamDy*2.31).r;
            float brokenPhase = breakerPhase + (lace - 0.5) * 2.4;
            float crest = smoothstep(0.15, 0.92, sin(brokenPhase));
            if (shorelineLook == 2) crest = max(crest, foldingFoam);
            // A long-lived tail behind each breaker collects in shallow pockets, then dissipates.
            float age = fract((brokenPhase - 1.5707963) / 6.2831853);
            float retention = sin(3.14159265 * age) * exp(-age * 3.8) * 2.5
                            * (1.0 - smoothstep(8.0, 55.0, shoreDistance));
            float contact = (1.0 - smoothstep(6.0, 26.0 + foamAA, shoreDistance))
                          * (0.35 + 0.25 * sin(swashPhase - 0.8));
            float holes = smoothstep(0.10, 0.64, foamTex.r * 0.72 + lace * 0.28);
            foamMix = clamp((crest * 0.95 + retention * 0.65 + contact) * holes * max(coastWeight, smoothstep(0.03, 0.07, length(shoreGrad)) * step(abs(shoreDepth - shelfBreak), 6.0)), 0.0, 0.94);
            if (shorelineLook == 1) {
                // Coastal look (owner refs): sparse, thin foam. A bright lacy line at the waterline, and breakers only on
                // the shelf edge offshore, in alongshore sections and wave sets, never a ring around every shore.
                // Close up (refs 5/6) the line widens into a swash band whose ragged edge the lace texture cuts.
                float swashW = mix(5.0, 34.0, 1.0 - smoothstep(1.5, 6.0, footprint)) * (0.8 + 0.2 * sin(swashPhase));
                float edge = (1.0 - smoothstep(0.5 * swashW, swashW + foamAA, shoreDistance + (lace - 0.5) * 0.9 * swashW))
                           * (0.6 + 0.4 * smoothstep(0.25, 0.7, 0.5 + 0.5 * sin(shoreDistance * 0.6 - time * 1.5 + lace * 3.0)));
                float breakDist = shelfBreak / shoreSlope;
                float sets = (0.45 + 0.55 * smoothstep(-0.2, 0.8, sin(time * 0.55 + dot(WorldPos.xy, vec2(0.0021, 0.0013)))))
                           * smoothstep(0.46, 0.60, vnoise(WorldPos.xy / 240.0 + vec2(0.0, 0.02 * time)));   // alongshore sections
                float reef = (1.0 - smoothstep(2.5, 4.0 + 1.5 * foamAA, abs(shoreDistance - breakDist + 3.0 * sin(brokenPhase)) * min(shoreSlope / 0.08, 1.0)))
                           + 0.35 * smoothstep(0.0, 4.0, breakDist - shoreDistance) * exp(-max(breakDist - shoreDistance, 0.0) / 14.0);
                reef *= smoothstep(50.0, 110.0, breakDist);   // only wide shelves break offshore: steep islands get no ring
                foamMix = clamp((edge * (0.5 + 0.8 * vnoise(WorldPos.xy / 160.0))) * holes * coastWeight + reef * sets * mix(holes, 1.0, 0.6) * max(coastWeight, smoothstep(0.03, 0.07, length(shoreGrad)) * step(abs(shoreDepth - shelfBreak), 6.0)), 0.0, 0.94);
            }
        }
        if (shorelineLook == 1 && rippleCrest > 0.65 && coastNight < 0.5) {   // tiny scattered whitecaps where the ripple crests pile up
            float cap = textureGrad(u_shoreFoam, fract(q * 3.1), foamDx * 3.1, foamDy * 3.1).r;
            foamMix = max(foamMix, 0.5 * smoothstep(0.65, 0.95, rippleCrest) * smoothstep(0.55, 0.8, cap)
                                 * smoothstep(0.55, 0.75, vnoise(WorldPos.xy / 900.0)) * (1.0 - coastWeight));
        }
    }
    vec3 foamLight = max(vertexLightRGB, vec3(SKY_AMBIENT * nightSky));
    col *= 1.0 + shoreRipple * nearShore;
    if (shorelineLook == 0) col = mix(col, vec3(0.72, 0.80, 0.78) * foamLight, foamMix);
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
    if (debugMode != 4 && waveLOD > 0.0) {
        float moverLit = moverVisibility(WorldPos,vec3(0.0,0.0,1.0));
        col *= mix(moverShadowFloor(0.4),1.0,moverLit);
        sunVis *= moverLit;
    }
    col = mix(col, F.f9.xyz * nightSky, clamp(F.f8.w, 0.0, 1.0));
    if (shorelineLook == 1) {   // ocean.glsl: sunlight glowing through thin crests seen against the sun
        vec2 toSun = normalize(uT.terrainLightDir.xy + vec2(1e-4, 0.0)), toEye = normalize(vdir.xy + vec2(1e-4, 0.0));
        float back = pow(clamp(-dot(toSun, toEye), 0.0, 1.0), 4.0);
        col += vertexLightRGB * sunVis * vec3(0.05, 0.26, 0.25) * smoothstep(0.55, 0.95, rippleCrest) * (0.03 + 0.5 * back);
    }
    float reflAmount = 0.0;

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
        if (debugMode != 2 && u_waterHdriLod >= 0.0) {
            skyReflCol = waterEvalHdri(rdir);   // rdir in MC2 Z-up, skyYaw applied inside
        } else {
            skyReflCol = clamp(waterEvalSkySh(skyDir) * (1.0 / 3.14159265), 0.0, 2.0);
        }
        // WATER-GRAZE-REFL-1: bias toward the authored sky/horizon tint at grazing angles.
        PREC float grazeW = pow(1.0 - max(vdir.z, 0.0), GRAZE_SKY_EXP);
        skyReflCol = mix(skyReflCol, F.f9.xyz, GRAZE_SKY_PULL * grazeW) * nightSky;
        skyReflCol *= 1.0 + NEAR_SKY_LIFT * nearGloss;   // WATER-GLOSS-NEAR: nearGloss is 0 at night
        if (shorelineLook == 1) skyReflCol *= vec3(0.80, 0.95, 1.03);   // coastal: deep water reads blue, not grey (owner refs)
        reflectCol = skyReflCol;   // default reflection = sky (fallback/base)
        //PORT-END:waterReflSetup
        vec4 geometry=vec4(0);
        if (debugMode != 1 && F.DEEP_COLOR.w < -0.5) {
            geometry = bakedReflection(WorldPos, rdir);
            geometry.rgb *= vec3(F.bake.zw, F.SHALLOW_COLOR.w);
        } else if (F.DEEP_COLOR.w > 0.5) {
            vec2 reflectionUV=gl_FragCoord.xy/u_waterScreenSize + reactive.xy*0.015;
            if (all(greaterThan(reflectionUV,vec2(0))) && all(lessThan(reflectionUV,vec2(1)))) {
                vec2 extent=ceil(u_waterScreenSize/F.DEEP_COLOR.w)/u_waterScreenSize;
                reflectionUV=clamp(reflectionUV*extent,0.5/u_waterScreenSize,extent-0.5/u_waterScreenSize);
                geometry=texture(u_waterReflection,reflectionUV);
            }
        }
        //PORT-BEGIN:waterReflMix
        PREC float fres = REFL_F0 + (1.0 - REFL_F0) * pow(1.0 - max(dot(waveN, vdir), 0.0), 5.0);
        if (shorelineLook == 1) {   // coastal look: exact unpolarised air-to-water Fresnel (IOR 1.333)
            float ci = clamp(dot(waveN, vdir), 0.0, 1.0), ct = sqrt(1.0 - (1.0 - ci * ci) / 1.776889);
            float rs = (ci - 1.333 * ct) / (ci + 1.333 * ct), rp = (1.333 * ci - ct) / (1.333 * ci + ct);
            fres = 0.5 * (rs * rs + rp * rp);
        }
        fres = max(fres, NEAR_REFL_FLOOR * nearGloss);   // WATER-GLOSS-NEAR
        // WATER-GRAZE-REFL-1: grazing fragments keep full reflection at distance.
        reflMix = clamp(fres * u_waterReflStrength * max(waveLOD, fres), 0.0, REFL_MAX);
        // Ripple LOD fades only the legacy sky treatment. Geometry retains
        // Schlick reflectance at distance; coverage fades smoothly to that sky.
        float geometryMix=clamp(fres*u_waterReflStrength,0.0,REFL_MAX);
        float totalMix=mix(reflMix,geometryMix,geometry.a);
        col=col*(1.0-totalMix)+reflectCol*(reflMix*(1.0-geometry.a))+geometry.rgb*geometryMix;
        reflAmount=totalMix;
        //PORT-END:waterReflMix
    }

    // WATER-WAVES-GLINT: sun (moon at night, half strength) specular on the wave normal, after the reflection mix so
    // the glints sit on top of the reflected sky. The sun altitude takes the shadow clamp (mc2_10 authors a horizon sun),
    // and the radiance is the vertex sun light, so sunset tints the glitter path and shadows remove it.
    {
        float glintScale = GLINT_GAIN / 0.30;
        vec3  Ls = uT.terrainLightDir.xyz;
        Ls = normalize(vec3(Ls.xy, max(Ls.z, max(uT.shadowMinSunSin, 0.05))));
        // The vertex light saturates at white, so the sun keys' colour never reached it: sunRelight (live/authored sun colour, 1 without keys) tints the glint.
        vec3  sunRad = Color.bgra.rgb * uEnv.sunRelight.rgb * (1.0 - 0.5 * clamp(F.f3.z / 0.6, 0.0, 1.0));
        if (F.debug.w > 0.0) {
            // waterGlintWarm: blend toward the flat-light chroma instead of adding white onto a blue body (additive glint clips all channels).
            float sg = SUN_SPEC_GAIN * glintScale * sunVis * waterGgxSpec(waveN, vdir, Ls, specAlpha2);
            vec3 f3 = vec3(uT.missionFlatR, uT.missionFlatG, uT.missionFlatB);
            col = mix(col, f3 / max(max(f3.r, max(f3.g, f3.b)), 1e-3) * 0.95, 1.0 - exp(-F.debug.w * sg));
        } else
        col += min(sunRad * (SUN_SPEC_GAIN * glintScale * sunVis * waterGgxSpec(waveN, vdir, Ls, specAlpha2)), vec3(shorelineLook == 1 ? mix(1.6, 0.4, coastNight) : 4.0));
        // The lamp specular and the flat pool term come from one light walk (waterPoolLights), added right after the sun glint.
        if (F.debug.y < 0.5 && uT.numPoolLights > 0 && nightSky < 1.0) {
            vec3 lampSpec;
            vec3 poolTerm = waterPoolLights(WorldPos, waveN, vdir, specAlpha2, lampSpec);
            col += min(lampSpec * (LAMP_SPEC_GAIN * glintScale), vec3(shorelineLook == 1 ? 0.25 : 4.0));   // coastal: no lamp flecks on the fine ripples
            col += poolTerm;
        }
    }

    // (The local pools cross the shoreline too: they were added after the sky reflection above, so the reflected sky
    // cannot erase lamps, and fog/shore alpha below still attenuate them.)
    if (kWaterDbgPass == 3) {
        FragColor = vec4(vec3(dot(col, vec3(0.2126, 0.7152, 0.0722))), 1.0);
        GBuffer1 = vec4(0.5, 0.5, 1.0, 0.875); return;
    }
    if (shorelineLook == 1) {
        // Coastal look: col is premultiplied radiance (in-scatter + reflection + glints) over the absorbed seabed; only
        // the reflected share hides it. Foam covers both; fog scales the added light by its own coverage.
        float foamA = clamp(foamMix, 0.0, 1.0) * shore * (1.0 - 0.65 * coastNight), fogT = 1.0;
        if (F.fog_color.x > 0.0 || F.fog_color.y > 0.0 || F.fog_color.z > 0.0 || F.fog_color.w > 0.0) fogT = FogValue;
        float wa = shore * reflAmount * (1.0 - foamA) + foamA;
        vec3 premul = (col * shore * (1.0 - foamA) + mix(vec3(0.86, 0.90, 0.88), vec3(0.62, 0.66, 0.70), coastNight) * foamLight * foamA) * fogT
                    + F.fog_color.rgb * (1.0 - fogT) * wa;
        if (F.f8.x != 0.0 && F.f8.z > 0.0) {
            float hz = clamp(edgeHazeAmount(WorldPos.xy, F.f8.z) * F.f8.y, 0.0, 1.0);
            premul *= 1.0 - hz; wa *= 1.0 - hz;
        }
        FragColor = vec4(premul, wa);
        GBuffer1 = vec4(0.5, 0.5, 1.0, 0.875);
        return;
    }
    if (F.fog_color.x > 0.0 || F.fog_color.y > 0.0 || F.fog_color.z > 0.0 || F.fog_color.w > 0.0)
        col = mix(F.fog_color.rgb, col, FogValue);

    // WATER-SHALLOW-1: opacity follows the water column (shaders/include/water_shallow.hglsl); deep water is unchanged.
    PREC float waterA = shore * WATER_MAX_ALPHA * ws_column_veil(tintThick);
    if (shorelineLook > 0) {
        // Clear shallows reveal the terrain through the existing premultiplied composite.
        float clearVeil = mix(0.10, 0.80, smoothstep(0.0, 24.0, max(shoreDepth, 0.0)));
        float pulse = 1.0 - smoothstep(swashFront, swashFront + 16.0, shoreDistance);
        clearVeil *= mix(1.0, 0.78, pulse);
        waterA = mix(waterA, shore * WATER_MAX_ALPHA * clearVeil, coastWeight);
        // Surface foam covers reflection and remains visible where the water column is clear.
        float foamA = foamMix * shore;
        col = (col * waterA * (1.0 - foamA) + vec3(0.78, 0.84, 0.80) * foamLight * foamA)
            / max(waterA * (1.0 - foamA) + foamA, 1e-5);
        waterA = waterA * (1.0 - foamA) + foamA;
    }
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
