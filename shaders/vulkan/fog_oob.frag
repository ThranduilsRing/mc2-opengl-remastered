// VULKAN-OOB-FOG-ISLAND-1 -- Vulkan port of shaders/fog_oob.frag (OOB-FOG-7
// sea-of-clouds at the map horizon).
//
// Smooth haze fills the out-of-bounds void below the horizon. The uniform and
// sampler layout retains the existing fog controls:
//   set=0,binding=0  depthTex        -> sampler2D (the scene depth, uploaded to a
//                                       D32_SFLOAT image; sampling returns depth in
//                                       .r, same as sampling a GL depth texture's .r)
//   set=0,binding=1  FogOobParams    -> std140 UBO mirroring the GL uniforms.
//                                       (invViewProj packing convention is documented
//                                       at the layout declaration below -- column_major,
//                                       do NOT qualify row_major.)
//
// invViewProj depth convention: reverse-Z [0,1]. pNear uses ndc-depth 1.0 (near),
// pFar uses ndc-depth 0.0 (far) -- identical to the GL shader. No Y-flip is applied
// because the pass reads TexCoord (interpolated [0,1] UV), not gl_FragCoord, and the
// island uploads the depth image so texel (u,v) maps the same way as the GL sample.
#version 450
#extension GL_GOOGLE_include_directive : require
#include "../include/fog_policy.hglsl"

layout(location = 0) in  vec2 TexCoord;
layout(location = 0) out vec4 outFog;

layout(set = 0, binding = 0) uniform sampler2D depthTex;
layout(set = 0, binding = 2) uniform samplerCube skyTex;
layout(set = 0, binding = 3) uniform sampler2D cloudTex;

// std140. Offsets (bytes) match the CPU-side POD in vulkan_oob_fog_island.cpp:
//   invViewProj  @  0  (mat4, 64B)
//   u_fogColor   @ 64  (vec3, 12B)
//   u_fogOpacity @ 76  (float; packs into the vec3's trailing slot in std140)
//   u_time       @ 80  (float)
// column_major (std140 default): the subgraph/island packs invViewProj as RAW
// inverseViewProj_ (column-major CPU bytes, index=col*4+row) -- the same bytes the GL
// net upload yields. GLSL reading them column-major = mul_colmajor is the correct
// unprojection; row_major reads transpose(M) and breaks it (L6.5 transpose regression).
// Same convention as edge_fog.frag / ssao.frag (N0). Do NOT qualify row_major.
layout(set = 0, binding = 1, std140) uniform FogOobParams {
    mat4  invViewProj;
    vec3  u_fogColor;
    float u_fogOpacity;
    float u_time;
    // VK-PARITY-F3: FOG-HORIZON-CLAMP-1 (default ON in GL). @84/88/92.
    int   u_horizonClampEnabled;
    float u_horizonFadeStartSin;
    float u_horizonFadeEndSin;
    vec4 u_cloudSea;
    vec4 u_cloudSun;
    vec4 u_cloudMotion;
    vec4 u_cloudLight;
    vec4 u_cloudAmbient;
};

// ---- Cloud sea: an art-directed billow deck (heightfield) that lies BELOW the map. ----
// World is z-up. u_cloudSea.x is the water level; every crown top stays under it, so the playable map
// reads as land standing above a sea of clouds. Crown heights are baked once on the CPU (billow octaves
// from 2600 down to ~55 world units, tiling every 20800) into a mipped R16 texture; a second, finer
// sample of the same texture adds small cauliflower billows. A ray finds the first crown it dips under
// with slope-limited steps (small steps only near a crown), then is lit from the heightfield normal,
// a short sun-ward shadow walk, height-based occlusion and a silver rim.
float deckShape(vec2 xy, float lod)
{
    vec2 uv = (xy + vec2(7.0, 3.0) * u_cloudMotion.x) * (1.0 / 20800.0);
    return textureLod(cloudTex, uv, lod + 2.0).r;
}

// Mip level matching the pixel footprint at ray distance t (world units per texel at level 0 is ~20.3).
float lodAt(float t) { return clamp(log2(max(t * 0.0007 / 20.3, 1.0)), 0.0, 7.0); }

float gDetail;  // 1: add the fine billow sample
float gJit;     // per-pixel step jitter in 0..1 (interleaved gradient noise)
float gOutside; // 1 where the shaded surface lies beyond the map edge
float gCeil;    // highest crown top (just under the water level)
float gR;       // crown relief: tops sit at the ceiling, gaps gR below it

// The ceiling steps down toward the map edge so the land drops away into the clouds instead of ending on a wall.
float boundaryRadius(vec2 xy)
{
    vec2 q = abs(xy) - u_cloudMotion.y * 0.65;
    return length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) + u_cloudMotion.y * 0.65;
}

float ceilAt(vec2 xy)
{
    float m = boundaryRadius(xy);
    return gCeil - 380.0 * (1.0 - smoothstep(u_cloudMotion.y - 400.0, u_cloudMotion.y + 5000.0, m));
}

float deckTop(vec2 xy, float lod)
{
    float sh = deckShape(xy, lod);
    if (gDetail > 0.0) {
        float fine = textureLod(cloudTex, xy * (4.7 / 20800.0) + vec2(0.31, 0.73), lod + 2.2).r;
        sh += (fine - 0.28) * 0.13 * min(sh * 3.0, 1.0);
        sh += (textureLod(cloudTex, xy * (13.0 / 20800.0) + vec2(0.57, 0.19), lod + 3.7).r - 0.28) * 0.05 * min(sh * 3.0, 1.0);
    }
    sh = clamp(sh, 0.0, 1.0);
    return ceilAt(xy) - gR * (1.0 - sh);
}

vec3 skyAt(vec3 rd, float elev)
{
    vec3 c = u_fogColor;
    if (!isnan(u_time)) {
        float az = atan(rd.y, -rd.x) + u_time;
        float s = sqrt(max(1.0 - elev * elev, 0.0));
        c = textureLod(skyTex, vec3(cos(az) * s, elev, sin(az) * s), 0.0).rgb;
    }
    return c * u_cloudSun.w;
}

float cloudMask(vec3 p, float t)
{
    float wobble = (deckShape(p.xy * 0.4, 4.0) - 0.35) * 1400.0;
    float edge = smoothstep(u_cloudMotion.y - 1000.0, u_cloudMotion.y + 300.0, boundaryRadius(p.xy) + wobble);
    return edge;
}

vec4 cloudLayer(vec3 origin, vec3 rd, float sceneDistance)
{
    gCeil = u_cloudSea.x - 90.0;
    gR = max(u_cloudSea.y, 400.0) * 0.65;
    // Interleaved gradient noise: far less clumped than a white hash.
    gJit = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    float top = gCeil;
    float bottom = gCeil - 380.0 - gR;
    float tMax = min(sceneDistance, 110000.0);
    bool high = u_cloudSea.w > 1.5;

    // The ray's stretch inside the deck slab [bottom, top]; the camera is always above it.
    if (rd.z > -1e-4 || origin.z <= top) return vec4(0.0);
    float t0 = max((top - origin.z) / rd.z, 0.0);
    float t1 = min((bottom - origin.z) / rd.z, t0 + 120000.0);
    t1 = min(t1, tMax);
    if (max(cloudMask(origin + rd * t0, t0), cloudMask(origin + rd * t1, t1)) < 0.003) return vec4(0.0);

    // Empty stretches of the deck dissolve into the sky, never into out-of-map water.
    vec3 horizon = skyAt(rd, 0.0);
    vec4 haze = vec4(horizon, gOutside);
    if (t0 >= t1) return haze;

    gDetail = 1.0;
    int steps = high ? 40 : 24;
    float slope = length(rd.xy) * 1.0 + max(-rd.z, 0.0);
    float t = t0 + (0.5 + gJit) * (4.0 + t0 * 0.002);
    float prevT = t0;
    bool hit = false;
    float th = t1;
    for (int i = 0; i < steps; ++i) {
        vec3 p = origin + rd * t;
        float oct = lodAt(t) + (high ? 0.0 : 1.0);
        float h = p.z - deckTop(p.xy, oct);
        if (h < 0.0) {
            float lo = prevT, hi = t;
            for (int b = 0; b < 4; ++b) {
                float mid = 0.5 * (lo + hi);
                vec3 pmid = origin + rd * mid;
                if (pmid.z < deckTop(pmid.xy, oct)) hi = mid; else lo = mid;
            }
            th = 0.5 * (lo + hi);
            hit = true;
            break;
        }
        prevT = t;
        // Slope-limited step: big strides well above the crowns, fine steps close to them.
        float dt = max(0.7 * h / slope, 10.0 + t * 0.0016);
        t += dt * (0.85 + 0.3 * gJit);
        if (t > t1) break;
    }
    if (!hit) return haze;
    if (th >= sceneDistance) return haze;

    vec3 ph = origin + rd * th;
    float mask = cloudMask(ph, th);
    float oct = lodAt(th) + (high ? 0.0 : 1.0);
    // Smooth fine creases while retaining the sun-facing slopes of the large billows.
    float eps = 3.0 * max(14.0, th * 0.0012) * exp2(oct);
    float hx = deckTop(ph.xy + vec2(eps, 0.0), oct + 1.0) - deckTop(ph.xy - vec2(eps, 0.0), oct + 1.0);
    float hy = deckTop(ph.xy + vec2(0.0, eps), oct + 1.0) - deckTop(ph.xy - vec2(0.0, eps), oct + 1.0);
    vec3 n = normalize(vec3(-hx, -hy, 4.5 * eps));

    vec3 sun = normalize(u_cloudSun.xyz);
    // Soft self-shadow: light scattered through a crown, so a shadowed flank keeps most of its light.
    float shadow = 1.0;
    int sh = high ? 4 : 2;
    gDetail = 0.0;
    for (int k = 0; k < sh; ++k) {
        float d = gR * (0.08 + 0.34 * float(k) * float(k));
        vec3 q = ph + sun * d;
        float over = deckTop(q.xy, 2.0) - q.z;
        shadow *= 1.0 - smoothstep(0.0, gR * 0.35, over) * 0.5;
    }
    float height01 = clamp((ph.z - (ceilAt(ph.xy) - gR)) / gR, 0.0, 1.0);
    float ndl = dot(n, sun);
    vec3 sunCol = u_cloudLight.rgb;
    vec3 skyFill = u_cloudAmbient.rgb;
    float ao = mix(0.62, 1.0, smoothstep(0.0, 0.9, height01));
    vec3 col = skyFill * ao * (0.78 + 0.22 * clamp(n.z, 0.0, 1.0));
    // Wrap diffuse: light bends around a cloud's flank instead of ending on a terminator line.
    float wrap = clamp((ndl + 0.7) / 1.7, 0.0, 1.0);
    col += sunCol * wrap * wrap * mix(0.55, 1.0, shadow) * (0.55 + 0.45 * height01);
    // Silver lining: thin crowns glow toward the sun (forward scatter), brightest at grazing crest edges.
    float rim = pow(1.0 - clamp(dot(n, -rd), 0.0, 1.0), 3.0);
    float fwd = pow(clamp(dot(rd, sun) * 0.5 + 0.5, 0.0, 1.0), 8.0);
    col += sunCol * (rim * 0.35 + 0.25) * fwd * shadow * smoothstep(0.45, 0.95, height01);
    col = col / (1.0 + 0.9 * max(col - 0.72, vec3(0.0))); // roll off sun glare instead of clipping to flat white

    // Aerial perspective hands the far deck to the sky horizon colour.
    float fogK = 1.0 - exp(-th / 50000.0);
    col = mix(col, horizon, fogK * fogK);

    float path = max(min(tMax, t1) - th, 0.0);
    float alpha = (1.0 - exp(-path / 240.0)) * mask * smoothstep(30.0, 350.0, th);
    return vec4(mix(horizon, col, alpha), max(alpha, gOutside));
}


void main()
{
    float rawDepth = texture(depthTex, TexCoord).r;

    vec2 ndc = TexCoord * 2.0 - 1.0;
    vec4 pNear = invViewProj * vec4(ndc, 1.0, 1.0);
    vec4 pFar  = invViewProj * vec4(ndc, 0.0, 1.0);
    vec3 worldDir = normalize(pFar.xyz / pFar.w - pNear.xyz / pNear.w);
    if (u_cloudSea.w > 0.5) {
        vec3 origin = pNear.xyz / pNear.w;
        // Match Camera's pick ray: reverse-Z zero can have w=0 (infinity).
        vec4 mid = invViewProj * vec4(ndc, 0.5, 1.0);
        vec3 cloudDir = normalize(mid.xyz / mid.w - origin);
        vec4 surface = invViewProj * vec4(ndc, max(rawDepth, 1e-7), 1.0);
        vec3 surf = surface.xyz / surface.w;
        float distance = rawDepth > 0.0 ? length(surf - origin) : 100000.0;
        // Round and dissolve the border water before its rectangular mesh ends.
        // Authored terrain remains a depth occluder, including corner islands.
        float surfM = boundaryRadius(surf.xy);
        float wob = (textureLod(cloudTex, surf.xy * (1.0 / 9000.0), 5.0).r - 0.35) * 500.0;
        float outside = rawDepth > 0.0 ? mc2CloudSeaReplace(surfM, wob, u_cloudMotion.y, 8.0 + distance * 0.0015) : step(0.0, -cloudDir.z - 1e-3);
        if (rawDepth > 0.0 && surf.z > u_cloudSea.x + 2.0) outside = 0.0;
        if (outside > 0.0) distance = 100000.0;
        gOutside = outside > 0.0 ? 1.0 : 0.0;
        outFog = cloudLayer(origin, cloudDir, distance);
        if (rawDepth > 0.0 && outside > 0.0) outFog.a *= outside;
        return;
    }
    if (rawDepth > 0.0) { outFog = vec4(0.0); return; }

    // VK-PARITY-F3: FOG-HORIZON-CLAMP-1 (verbatim from shaders/fog_oob.frag).
    float skyFade;
    if (u_horizonClampEnabled != 0) {
        float elevSin = -worldDir.z;
        if (elevSin >= u_horizonFadeEndSin) { outFog = vec4(0.0); return; }
        skyFade = 1.0 - smoothstep(u_horizonFadeStartSin, u_horizonFadeEndSin, elevSin);
    } else {
        // Legacy: worldDir.z < 0 = sky. Fade in from just-above-horizon.
        if (worldDir.z < -0.22) { outFog = vec4(0.0); return; }
        skyFade = smoothstep(-0.22, -0.01, worldDir.z);
    }

    // Smooth atmosphere at the map horizon. Procedural cloud cells here were
    // magnified into rectangular patches by ground-level camera rays.
    vec3 horizon = u_fogColor;
    if (!isnan(u_time)) {
        float az = atan(worldDir.y, -worldDir.x) + u_time;
        horizon = textureLod(skyTex, vec3(cos(az), 0.0, sin(az)), 0.0).rgb;
    }
    outFog = vec4(horizon, clamp(skyFade * u_fogOpacity, 0.0, 1.0) * mc2FogGroundHorizon(-worldDir.z));
}
