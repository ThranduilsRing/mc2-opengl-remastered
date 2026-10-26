// VK-P9a WATER-INTEROP -- Vulkan port of the MC2 armed water FS
// (shaders/gos_terrain_water_mdi.frag), BASE-LAYER (o_isWater==1) subset with
// REFLECTION + HDRI OFF (P9a). Writes MRT: FragColor(0) + GBuffer1(1).
//
// PARITY CONTRACT: reproduces the o_isWater==1 base-water branch VERBATIM with
// the reflection/HDRI block skipped (u_waterReflStrength==0 && u_waterRtStrength==0
// && u_waterHdriLod<0 && debug==0 -> the whole `if(...)` reflection block is a
// no-op in the GL FS too, so this is byte-equivalent to the GL default gate cfg).
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
    vec4 f3;              // (screenW, screenH, _pad, _pad)  -- u_waterScreenSize
    vec4 DEEP_COLOR;      // .xyz
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
#include "../include/water_shore_edge.hglsl"
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
const vec3  GLINT_TINT    = vec3(0.82, 0.88, 0.94);
const float GLINT_THRESH  = 0.36;
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
                        asin(clamp(d.z, -1.0, 1.0)) / 3.14159265 + 0.5);
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
PREC float fbm3(PREC vec2 uv){
    PREC float s = 0.0, a = 0.5;
    for (int k = 0; k < 3; ++k){ s += a * vnoise(uv); uv = uv * 2.03 + 11.7; a *= 0.5; }
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

    PREC float sc = time * WAVE_SPEED * WAVE_FREQ;
    PREC vec2  q0 = WorldPos.xy * WAVE_FREQ        + vec2( 1.00,  0.60) * sc;
    PREC vec2  q1 = WorldPos.xy * WAVE_FREQ * 1.70 + vec2(-0.80, -1.10) * sc;
    PREC float nz = (fbm3(q0) + fbm3(q1)) - 0.875;

    // SHORE-FIELD-1: deep/shallow tint from the smooth shore field (mode 3), not the coarse
    // per-vertex thickness -- no more deep-water diamonds around single low vertices.
    // Evaluated for every fragment (uniform control flow) so its screen derivative is defined.
    float fieldThick;
    const bool hasField = wse_fieldThickness(int(F.f4.x + 0.5), int(F.f4.y + 0.5), u_shoreHeightTex, F.f5,
                                             WorldPos.xy, F.f7.x, fieldThick);
    const float fieldThickFw = fwidth(fieldThick);
    PREC float tintThick = hasField ? max(fieldThick, 0.0) : WaterThickness;
    PREC float trans    = clamp(exp(-tintThick * ABSORPTION_DENSITY), 0.0, 1.0);
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

    // Night: the scattered light, sky ambient floor, glint, sky tint and reflected sky take the mission's night
    // dim (f3.z, 0 by day), so open water reads as dark as the ground around it instead of daylight teal.
    PREC float nightSky = 1.0 - clamp(F.f3.z, 0.0, 1.0);
    PREC vec3  vertexLightRGB = Color.bgra.rgb * nightSky;
    PREC vec3  col = waterCol * max(vertexLightRGB, vec3(SKY_AMBIENT * nightSky))
                   + waterCol * SKY_AMBIENT * 0.5 * nightSky;
    PREC float crest = max(nz, 0.0);
    col *= 1.0 + RIPPLE_GAIN * waveLOD * crest;
    PREC float glint = smoothstep(GLINT_THRESH, 0.80, nz);
    col += glint * GLINT_GAIN * waveLOD * GLINT_TINT * nightSky;
    //PORT-END:waterBaseColor
    // Static shadow on the light the surface scatters and glints (the reflected sky is untouched), with the receivers' 0.4 floor.
    if (F.f7.z > 0.5) {
        vec2 maskUv = vec2(WorldPos.x + F.f7.w, F.f7.w - WorldPos.y) / max(F.f7.y, 1.0);
        if (all(greaterThanEqual(maskUv, vec2(0.0))) && all(lessThanEqual(maskUv, vec2(1.0))))
            col *= mix(sunShadowFloor(0.4), 1.0, texture(u_staticMask, maskUv).r);
    }
    // WATER-VISUAL-FIRST-SLICE sky tint (strength 0 default -> mix returns col; GL line 328).
    col *= mix(sunShadowFloor(0.4),1.0,moverVisibility(WorldPos,vec3(0.0,0.0,1.0)));
    col = mix(col, F.f9.xyz * nightSky, clamp(F.f8.w, 0.0, 1.0));

    // ---- RUNG-6.5 WATER-REFLECTION PORT (WATER-SKY-REFLECTION-1 + WATER-HDRI-REFL-1) ----
    // Verbatim port of the GL reflection block (gos_terrain_water_mdi.frag), minus:
    //  - terrain-RT contact sub-block (MC2_WATER_REFLECTION_RT default-OFF ->
    //    u_waterRtStrength==0 -> the GL sub-block is a no-op in the field config;
    //    NOT ported — reflectCol stays = skyReflCol, byte-equivalent to GL default);
    //  - u_waterDebugMode terms (VK FS has no debug-view path; GL evaluates the
    //    block for debug 7/8/9 even at strength 0 but reflMix stays 0 -> Final same).
    // Gate parity: u_waterReflStrength is fed from gos_GetWaterReflStrength()
    // (MC2_WATER_REFLECTION=0 -> 0.0 -> block skipped, same as GL).
    if (u_waterReflStrength > 0.0) {
        PREC vec3  skyReflCol = col;
        PREC vec3  reflectCol = col;
        PREC float reflMix    = 0.0;
        //PORT-BEGIN:waterReflSetup
        // WATER-ASPECT-CORRECT-1/2: frustum aspect fed from GL (== viewport aspect by default,
        // so _ac is a 1.0 no-op; the old hard-coded *3/4 over-corrected 16:9 by 1.33x).
        float vpAspect = u_waterScreenSize.x / max(u_waterScreenSize.y, 1.0);
        float _ac = vpAspect / max(F.f9.w, 0.0001);
        // WATER-GRAZE-REFL-1: fade the wave-normal perturbation by waveLOD.
        PREC vec2  nzGrad     = clamp(vec2(dFdx(nz) * _ac, dFdy(nz)), -2.0, 2.0) * waveLOD;
        PREC vec3  waveNormal = normalize(vec3(nzGrad * REFL_WAVE_SLOPE, 1.0));
        PREC vec3  vdir       = normalize(cameraPos.xyz - WorldPos);  // MC2 Z-up
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
        // (terrain-RT contact sub-block skipped here — see header note above)
        //PORT-BEGIN:waterReflMix
        PREC float fres = REFL_F0 + (1.0 - REFL_F0) * pow(1.0 - max(vdir.z, 0.0), 5.0);
        // WATER-GRAZE-REFL-1: grazing fragments keep full reflection at distance.
        reflMix = clamp(fres * u_waterReflStrength * max(waveLOD, fres), 0.0, REFL_MAX);
        col = mix(col, reflectCol, reflMix);   // reflMix==0 at SH strength 0 -> no-op
        //PORT-END:waterReflMix
    }

    // The same local pools cross the shoreline; apply after sky reflection so
    // the reflected sky cannot erase lamps, then let fog/shore alpha attenuate.
    if (uT.numPoolLights > 0 && nightSky < 1.0) {
        float contact = 0.0;
        col += tlp_terrainPoolLight(WorldPos, uT.numPoolLights, uT.lightGridDim,
            vec2(uT.lightGridMinX, uT.lightGridMinY), vec2(uT.lightGridInvCellX, uT.lightGridInvCellY),
            uT.lightPoolSoft, uT.lightPoolMoverDim, uT.lightPoolMoverScale,
            uT.lightPoolMoverMax, uT.lightPoolCompress, 0.0, contact, vec3(0,0,1), vec3(0,0,1));
    }
    if (F.fog_color.x > 0.0 || F.fog_color.y > 0.0 || F.fog_color.z > 0.0 || F.fog_color.w > 0.0)
        col = mix(F.fog_color.rgb, col, FogValue);

    PREC float waterA = shore * WATER_MAX_ALPHA;
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
    GBuffer1 = rc_gbuffer1_shadowHandled(vec3(0.0,0.0,1.0));
}
