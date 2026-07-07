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
#define PREC highp

layout(location = 0) in PREC vec4  Color;
layout(location = 1) in PREC vec2  Texcoord;
layout(location = 2) in PREC float FogValue;
layout(location = 3) in PREC float WaterThickness;
layout(location = 4) in PREC vec3  WorldPos;

layout(location = 0) out PREC vec4 FragColor;
layout(location = 1) out PREC vec4 GBuffer1;

// Uniforms (subset needed by the base branch, reflection/HDRI OFF). One std140 UBO.
layout(set = 0, binding = 1, std140) uniform WaterFsParams {
    vec4 fog_color;       // current render-state fog (vec4(0) by water time -> fog off)
    vec4 f0;              // (time, cameraPosX, cameraPosY, cameraPosZ)
    vec4 f1;              // (alphaDepth, ABSORPTION_DENSITY, WATER_MAX_ALPHA, RIPPLE_GAIN)
    vec4 f2;              // (GLINT_GAIN, _pad, _pad, _pad)
    vec4 DEEP_COLOR;      // .xyz
    vec4 SHALLOW_COLOR;   // .xyz
} F;

const float SKY_AMBIENT   = 0.18;
const float WAVE_FREQ     = 0.030;
const float WAVE_SPEED    = 6.0;
const vec3  GLINT_TINT    = vec3(0.82, 0.88, 0.94);
const float GLINT_THRESH  = 0.36;
const float WAVE_FADE_NEAR = 9000.0;
const float WAVE_FADE_FAR  = 40000.0;

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

    PREC vec3  viewVec = camPos - WorldPos;
    //PORT-BEGIN:waterBaseColor
    PREC float waveLOD = 1.0 - smoothstep(WAVE_FADE_NEAR, WAVE_FADE_FAR, length(viewVec));

    PREC float sc = time * WAVE_SPEED * WAVE_FREQ;
    PREC vec2  q0 = WorldPos.xy * WAVE_FREQ        + vec2( 1.00,  0.60) * sc;
    PREC vec2  q1 = WorldPos.xy * WAVE_FREQ * 1.70 + vec2(-0.80, -1.10) * sc;
    PREC float nz = (fbm3(q0) + fbm3(q1)) - 0.875;

    PREC float trans    = clamp(exp(-WaterThickness * ABSORPTION_DENSITY), 0.0, 1.0);
    PREC vec3  waterCol = mix(DEEP_COLOR, SHALLOW_COLOR, trans);

    PREC float shoreBlend = max(alphaDepth, 1.0);
    PREC float shore = smoothstep(-shoreBlend * 0.5, shoreBlend, WaterThickness);
    if (shore <= 0.0) discard;

    PREC vec3  vertexLightRGB = Color.bgra.rgb;
    PREC vec3  col = waterCol * max(vertexLightRGB, vec3(SKY_AMBIENT))
                   + waterCol * SKY_AMBIENT * 0.5;
    PREC float crest = max(nz, 0.0);
    col *= 1.0 + RIPPLE_GAIN * waveLOD * crest;
    PREC float glint = smoothstep(GLINT_THRESH, 0.80, nz);
    col += glint * GLINT_GAIN * waveLOD * GLINT_TINT;
    //PORT-END:waterBaseColor
    // sky-tint OFF (strength 0 default) -> no-op; reflection block OFF -> skipped.

    if (F.fog_color.x > 0.0 || F.fog_color.y > 0.0 || F.fog_color.z > 0.0 || F.fog_color.w > 0.0)
        col = mix(F.fog_color.rgb, col, FogValue);

    PREC float waterA = shore * WATER_MAX_ALPHA;
    // edge-feather OFF (default) -> skipped.
    // COMPOSITE-STAGE PARITY: emit PREMULTIPLIED alpha (col*a, a) into the transparent
    // water-only target (pipeline blend ONE / ONE_MINUS_SRC_ALPHA). GL then premult-over-
    // composites onto MainColor (GL_ONE / GL_ONE_MINUS_SRC_ALPHA) == GL's native one-stage
    // col*a + bg*(1-a) blend, bit-exact. (Straight-alpha two-stage double-applied alpha.)
    FragColor = vec4(col * waterA, waterA);
    GBuffer1  = vec4(0.5, 0.5, 1.0, 0.0);   // rc_gbuffer1_screenShadowEligible(0,0,1)
}
