// VULKAN-EDGE-FOG-ISLAND-2a -- Vulkan port of shaders/edge_fog.frag.
//
// Fog policy scales distances and suppresses steep-view range whiteout.
// The map-boundary concealment retains its outside-fill semantics.
// Uniform/sampler plumbing uses explicit Vulkan set/binding layouts:
//   set=0,binding=0  depthTex        -> sampler2D (the scene depth, uploaded to a
//                                       D32_SFLOAT image; sampling returns depth in .r,
//                                       same as sampling a GL depth texture's .r)
//   set=0,binding=1  EdgeFogParams   -> std140 UBO mirroring the GL uniforms. The
//                                       CPU packs invViewProj as the SAME 16 floats
//                                       the GL path uploads (row-major direct upload,
//                                       GL_FALSE) so the unprojection matches GL.
//
// invViewProj depth convention: reverse-Z [0,1]. wNear uses ndc-depth 1.0 (near),
// wFar uses ndc-depth 0.0 (far) -- identical to the GL shader. No Y-flip is applied
// because the pass reads TexCoord (interpolated [0,1] UV), not gl_FragCoord, and the
// island uploads the depth image so texel (u,v) maps the same way as the GL sample.
#version 450
#extension GL_GOOGLE_include_directive : require
#include "../include/fog_policy.hglsl"

layout(location = 0) in  vec2 TexCoord;
layout(location = 0) out vec4 outFog;

layout(set = 0, binding = 0) uniform sampler2D depthTex;

// std140. Offsets (bytes) chosen to match the CPU-side POD in
// vulkan_edge_fog_island.cpp:
//   invViewProj      @  0  (mat4, 64B)
//   u_fogColor       @ 64  (vec3, 12B) + pad float @ 76
//   u_halfExtent     @ 80
//   u_fogStart       @ 84
//   u_fogHeight      @ 88
//   u_fogMax         @ 92
//   u_waterElevation @ 96
//   u_horizonClampEnabled @100, u_horizonFadeStartSin @104, u_horizonFadeEndSin @108
// column_major (std140 default): the CPU packs invViewProj as the SAME 16 raw floats
// the GL path uploads (glUniformMatrix4fv GL_FALSE). inverseViewProj_ is column-major
// on the CPU (index=col*4+row), so GL_FALSE => GLSL reads it column-major => mul_colmajor
// is the CORRECT unprojection (ground truth: GL-Z=-15.39). row_major here reads
// transpose(M) (=669.57) and breaks world reconstruction -- the L6.5 transpose
// regression, proved via oblique fog bookmarks (cliff_side_on/oblique_fog/oblique_horizon
// DIFF; top-down views gate fog to vec4(0) and hid it). Same convention as ssao.frag
// (N0 fix ac96e31d). Do NOT qualify row_major.
layout(set = 0, binding = 1, std140) uniform EdgeFogParams {
    mat4  invViewProj;
    vec3  u_fogColor;
    float _pad0;
    float u_halfExtent;
    float u_fogStart;
    float u_fogHeight;
    float u_fogMax;
    float u_waterElevation;
    // VK-PARITY-F3: FOG-HORIZON-CLAMP-1 (default ON in GL). @100/104/108.
    int   u_horizonClampEnabled;
    float u_horizonFadeStartSin;
    float u_horizonFadeEndSin;
    float u_hazeStart;   // @112 source wu (x1.5 via mc2FogDistance)
    float u_hazeFull;    // @116
    int   u_fogProfile;  // @120 0 = Stock
    float _pad1;         // @124
    // Analytic atmosphere (fid-1): extinction + height fog integrated along the view ray, HG sun in-scatter.
    vec4  u_atmoParams;  // @128 x quality (0 off, 1 low, 2 high), y ground extinction per wu, z height falloff per wu, w HG g
    vec4  u_atmoSun;     // @144 xyz unit toward the sun (world Z up), w in-scatter strength
    vec4  u_atmoColor;   // @160 rgb ambient scatter colour, w base elevation (wu)
};

// Returns (rgb colour, alpha) of the atmosphere layer for a view ray ending at the visible surface.
vec4 atmosphereLayer(vec3 camPos, vec3 surfPos)
{
    vec3 rd = surfPos - camPos;
    float d = length(rd);
    if (u_atmoParams.x < 0.5 || d < 1.0) return vec4(0.0);
    rd /= d;
    float k = u_atmoParams.z;
    float x = k * rd.z * d;
    float path = abs(x) < 1e-4 ? d : (1.0 - exp(-x)) / (k * rd.z);
    float camH = max(camPos.z - u_atmoColor.w, 0.0);
    float tau = u_atmoParams.y * exp(-min(k * camH, 30.0)) * path;
    float alpha = 1.0 - exp(-max(tau, 0.0));
    vec3 scatter = u_atmoColor.rgb;
    if (u_atmoParams.x > 1.5) {
        float g = u_atmoParams.w;
        float c = dot(rd, u_atmoSun.xyz);
        float phase = (1.0 - g * g) / pow(max(1.0 + g * g - 2.0 * g * c, 1e-3), 1.5);  // 1 = isotropic
        scatter += vec3(1.0, 0.86, 0.64) * (u_atmoSun.w * phase * clamp(u_atmoSun.z * 2.0, 0.0, 1.0));
    }
    return vec4(min(scatter, vec3(1.2)), alpha);
}

void main()
{
    float rawDepth = texture(depthTex, TexCoord).r;
    // Empty sky belongs to the horizon pass. Projecting it onto a fixed-height
    // plane produces rectangular map-boundary patches above the horizon.
    if (rawDepth <= 0.0) { outFog = vec4(0.0); return; }
    vec2 ndc = TexCoord * 2.0 - 1.0;

    vec4 pNear = invViewProj * vec4(ndc, 1.0, 1.0);
    vec4 pFar  = invViewProj * vec4(ndc, 0.0, 1.0);
    vec3 wNear = pNear.xyz / pNear.w;
    vec3 wFar  = pFar.xyz / pFar.w;

    vec4 wp = invViewProj * vec4(ndc, rawDepth, 1.0);
    vec3 worldPos = wp.xyz / wp.w;

    // Use the visible surface, so a camera below the cloud top still sees haze
    // on boundary cliffs and skirts rather than failing a plane intersection.
    float distFromEdge = u_halfExtent - max(abs(worldPos.x), abs(worldPos.y));

    float innerRamp   = 1.0 - smoothstep(0.0, mc2FogDistance(max(u_fogStart, 768.0)), distFromEdge);
    float outsideFill = step(0.0, -distFromEdge);

    // VK-PARITY-F3: FOG-HORIZON-CLAMP-1 (verbatim from shaders/edge_fog.frag). Full fog
    // at/below the horizon, fading to zero across [startSin,endSin], so the cloud bank
    // never bleeds into the sky. clamp==0 -> factor 1.0 (previous behaviour).
    float horizonFactor = 1.0;
    if (u_horizonClampEnabled != 0) {
        float elevSin = -normalize(wFar - wNear).z;
        horizonFactor = 1.0 - smoothstep(u_horizonFadeStartSin, u_horizonFadeEndSin, elevSin);
    }

    float d = length(worldPos - wNear);
    float rangeHaze = (u_fogProfile == 0)
        ? mc2FogSmooth(mc2FogRamp(d, mc2FogDistance(6000.0), mc2FogDistance(12000.0)))
        : mc2FogSmooth(mc2FogRamp(d, mc2FogDistance(u_hazeStart), mc2FogDistance(u_hazeFull)));
    rangeHaze *= mc2FogRangeViewFactor(-normalize(wFar - wNear).z);
    float stock = clamp(max(max(innerRamp, rangeHaze) * u_fogMax, outsideFill), 0.0, 1.0)
                  * horizonFactor;
    // Profiles 1-3 also fog past haze start above the horizon (horizonFactor not applied to the range term).
    float fogFactor = (u_fogProfile == 0) ? stock : max(stock, rangeHaze * u_fogMax);
    vec4 atmo = atmosphereLayer(wNear, worldPos);
    // Edge layer over the atmosphere layer: A = a2 + a1 (1 - a2), colour weighted by each layer's contribution.
    float outA = fogFactor + atmo.a * (1.0 - fogFactor);
    vec3 outC = outA > 1e-5 ? (u_fogColor * fogFactor + atmo.rgb * atmo.a * (1.0 - fogFactor)) / outA : u_fogColor;
    outFog = vec4(outC, outA);
}
