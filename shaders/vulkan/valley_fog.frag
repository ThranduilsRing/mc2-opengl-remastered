// VALLEY-FOG (ground-fog-4) -- low-lying fog banks filling the low ground of a mission.
//
// The fog is a slab under top(x,y) = fogFloor(x,y) + thickness. The fog floor is the regional low ground baked from the
// height field (TerrainFieldStore::fogFloor), so basins and rivers fill while ridges and tree tops above the top stand clear.
// One fullscreen pass before the edge fog: no raymarch, no history, no fog-of-war cut.
//   per pixel: 6 (Low: 4) fixed samples of the density between the slab entry and the visible surface give the optical depth
//   tau, alpha = 1 - exp(-tau); a noise relief on the top is lit by the live sun and ambient for rolling sunlit tops.
//   set=0,binding=0  depthTex         -> shared scene depth (reverse-Z, 0 = sky)
//   set=0,binding=1  ValleyFogParams  -> std140 UBO (tools/check_ubo_layout.py postfog_valley)
//   set=0,binding=2  FogFloorBuf      -> the fog floor, mapSide^2 floats in the height field layout
//   set=0,binding=3  staticMask      -> the terrain's baked static ground-shadow mask (binding 17 of the ground set)
//   set=1            mc2veg::GroundLighting: uT (halfMap, mapSide, static light matrix) and uEnv (live sun and ambient)
// Output is straight alpha; the pipeline blends SRC_ALPHA / ONE_MINUS_SRC_ALPHA.
#version 450
#extension GL_GOOGLE_include_directive : require
#define PREC highp
#define TERRAIN_RESOURCE_SET 1
#define TERRAIN_OBJECT_RECEIVER 1
#include "include/terrain_frame_vk.glsl"
#include "include/noise2d_vk.glsl"

layout(location = 0) in vec2 TexCoord;
layout(location = 0) out vec4 outFog;

layout(set = 0, binding = 0) uniform sampler2D depthTex;
layout(set = 0, binding = 2, std430) readonly buffer FogFloorBuf { float fogFloor[]; };
layout(set = 0, binding = 3) uniform sampler2D staticMask;
layout(set = 0, binding = 1, std140) uniform ValleyFogParams {
    mat4  invViewProj;   // off 0
    vec4  eye;           // off 64  xyz the camera, w the highest fog floor on the map
    vec4  layer;         // off 80  x thickness, y sigma (per wu), z coverage threshold, w noise scale (wu)
    vec4  shape;         // off 96  x top relief (wu), y soft top (wu), z time (s), w quality (1 low, 2 high)
    vec4  fogColor;      // off 112 rgb, the colour of a sunlit top
    vec4  sunTint;       // off 128 rgb multiplier of the sun term, w static-shadow strength
} fog;

// Bilinear fog floor at world xy, indexed like the terrain heights: grid = (x + halfMap, halfMap - y) / 128.
float fogFloorAt(vec2 xy) {
    int ms = uT.mapSide;
    vec2 g = clamp(vec2(xy.x + uT.halfMap, uT.halfMap - xy.y) / 128.0, vec2(0.0), vec2(ms - 1));
    ivec2 c = ivec2(g), d = min(c + 1, ivec2(ms - 1));
    vec2 f = g - vec2(c);
    return mix(mix(fogFloor[c.y * ms + c.x], fogFloor[c.y * ms + d.x], f.x),
               mix(fogFloor[d.y * ms + c.x], fogFloor[d.y * ms + d.x], f.x), f.y);
}

// 1 inside a bank, 0 in a gap. fbm (std about 0.16) is widened so the +-0.08 band around the threshold is a defined edge.
float bankCoverage(vec2 xy) {
    float n = 0.5 + 1.8 * fbm(xy / fog.layer.w + vec2(0.85, 0.53) * (0.02 * fog.shape.z), 5);
    return smoothstep(fog.layer.z - 0.08, fog.layer.z + 0.08, n);
}

// Extinction per wu at p: full softTop under the local top, zero at and above it.
float valleyDensity(vec3 p) {
    float under = smoothstep(0.0, fog.shape.y, fogFloorAt(p.xy) + fog.layer.x - p.z);
    return under > 0.0 ? fog.layer.y * under * bankCoverage(p.xy) : 0.0;
}

// The lit surface of a bank: the top plus a rolling relief at half the coverage scale.
float fogTopAt(vec2 xy) {
    return fogFloorAt(xy) + fog.shape.x * fbm(xy / (fog.layer.w * 0.5), 3);
}

void main() {
    float rawDepth = texture(depthTex, TexCoord).r;
    if (rawDepth <= 0.0 || fog.layer.x <= 0.0) { outFog = vec4(0.0); return; }
    vec2 ndc = TexCoord * 2.0 - 1.0;
    vec4 nearPoint = fog.invViewProj * vec4(ndc, 1.0, 1.0);
    vec3 eye = nearPoint.xyz / nearPoint.w;
    vec4 hit = fog.invViewProj * vec4(ndc, rawDepth, 1.0);
    vec3 surface = hit.xyz / hit.w;
    // A straight ray whose both ends are above the highest fog top never enters the layer: ridges, tree tops, tall units.
    float maxTop = fog.eye.w + fog.layer.x + fog.shape.x;
    if (min(eye.z, surface.z) > maxTop) { outFog = vec4(0.0); return; }
    vec3 ray = surface - eye;
    float span = length(ray);
    ray /= span;
    // Slab entry (wu from the camera): the camera itself, or where the ray crosses the highest fog top.
    float start = eye.z > maxTop ? (maxTop - eye.z) / ray.z : 0.0;
    int samples = fog.shape.w < 1.5 ? 4 : 6;
    float ds = (span - start) / float(samples);
    float tau = 0.0;
    vec3 lit = eye + ray * start;   // the last sample before the fog turns opaque: the surface the pixel sees
    for (int i = 0; i < samples; ++i) {
        vec3 p = eye + ray * (start + (float(i) + 0.5) * ds);
        float rho = valleyDensity(p);
        if (rho > 0.0 && tau < 1.0) lit = p;
        tau += rho * ds;
    }
    float alpha = 1.0 - exp(-tau);
    if (alpha < 1e-3) { outFog = vec4(0.0); return; }

    // Sun on the top: forward differences of the relief 64 wu apart give the normal; slopes away from the sun sit in shade.
    const float stepWu = 64.0;
    float h0 = fogTopAt(lit.xy);
    vec3 normal = normalize(vec3(h0 - fogTopAt(lit.xy + vec2(stepWu, 0.0)), h0 - fogTopAt(lit.xy + vec2(0.0, stepWu)), stepWu));
    vec3 sunDir = uEnv.sunLiveDir.xyz;
    float ndl = dot(sunDir, sunDir) > 1e-6 ? max(dot(normal, normalize(sunDir)), 0.0) : 0.0;
    vec3 amb = uEnv.ambLive.rgb;
    vec3 light = amb + uEnv.sunLive.rgb * fog.sunTint.rgb * (0.45 + 0.55 * ndl);
    // The terrain's baked static shadow over the bank (one mask tap at the slab entry, indexed like ground_shadow.glsl):
    // faint, fog scatters around it.
    if (fog.sunTint.w > 0.0 && textureSize(staticMask, 0).x > 1) {
        vec3 entry = eye + ray * start;
        vec2 uv = vec2(entry.x + uT.halfMap, uT.halfMap - entry.y) / (128.0 * float(max(uT.mapSide - 1, 1)));
        if (all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0)))) {
            float shadow = texture(staticMask, uv).r;
            light = amb + (light - amb) * mix(1.0, shadow, fog.sunTint.w);
        }
    }
    // A sunlit top is fogColor itself (brighter and bluer than the ground) while the scene light is at least 1; dimmer scenes
    // dim the fog with them. Clamped to the atmosphere layer's HDR white (fog_policy.hglsl atmosphereLayer).
    float white = max(max(light.r, light.g), max(light.b, 1.0));
    outFog = vec4(min(fog.fogColor.rgb * light / white, vec3(1.2)), alpha);
}
