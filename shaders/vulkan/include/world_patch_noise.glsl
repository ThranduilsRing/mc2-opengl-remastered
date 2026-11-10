// world_patch_noise.glsl -- the shared world-space ground patch field (owner ref 3: a green/olive sward
// mottled with dry straw patches at about 5-30 m). Self-contained (no other include), so the terrain
// fragment shader and the vegetation shaders colour ground and blades from the SAME field.
//
//   vec2 worldPatchField(vec2 worldXY)
//       worldXY = MC2 world X/Y in world units (terrain v_worldPos.xy; 4 wu ~= 1 m).
//       .x = dryness 0..1 (0 = green sward, 1 = dry straw patch), contrast-shaped value-noise fbm, 3 octaves at
//            WPN_BASE_WU, /2, /4 (120, 60, 30 wu = about 30, 15, 7.5 m features).
//       .y = hue jitter 0..1 inside the green sward (olive vs yellow-green), one octave at WPN_BASE_WU * 0.55.
//   vec3 worldPatchColor(vec2 field, float extraDry)
//       palette colour (colormap/albedo space) for that field; extraDry 0..1 pushes toward straw (coast, beaches).
//
// Pure functions of world position: no time term (never temporal), stable under camera motion.
#ifndef WORLD_PATCH_NOISE_GLSL
#define WORLD_PATCH_NOISE_GLSL

const float WPN_BASE_WU = 120.0;                         // largest patch wavelength, world units
const vec3  WPN_OLIVE        = vec3(0.260, 0.270, 0.140);   // olive green sward
const vec3  WPN_YELLOW_GREEN = vec3(0.310, 0.310, 0.155);   // yellow-green sward
const vec3  WPN_STRAW        = vec3(0.500, 0.450, 0.310);   // dry straw patch

float wpn_hash(vec2 p) {
    // Integer-lattice hash, sin-free (stable across drivers at large world coordinates).
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float wpn_value(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = wpn_hash(i);
    float b = wpn_hash(i + vec2(1.0, 0.0));
    float c = wpn_hash(i + vec2(0.0, 1.0));
    float d = wpn_hash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

vec2 worldPatchField(vec2 worldXY) {
    vec2 p = worldXY / WPN_BASE_WU;
    // Rotate each octave so the value-noise lattice never lines up with the terrain grid.
    const mat2 R = mat2(0.80, 0.60, -0.60, 0.80);
    float n = 0.45 * wpn_value(p);
    p = R * p * 2.03 + vec2(17.1, 3.7);
    n += 0.32 * wpn_value(p);
    p = R * p * 2.01 + vec2(-5.3, 11.9);
    n += 0.23 * wpn_value(p);
    // Contrast shape: connected straw patches and connected green; the finer octaves rag the edges.
    float dry = smoothstep(0.48, 0.57, n);
    float hue = wpn_value(R * worldXY / (WPN_BASE_WU * 0.55) + vec2(41.0, -23.0));
    return vec2(dry, hue);
}

vec3 worldPatchColor(vec2 field, float extraDry) {
    vec3 sward = mix(WPN_OLIVE, WPN_YELLOW_GREEN, field.y);
    float dry = clamp(field.x + extraDry * (1.0 - field.x), 0.0, 1.0);
    return mix(sward, WPN_STRAW, dry);
}

#endif
