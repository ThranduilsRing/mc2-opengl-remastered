//#version 420 (version provided by prefix)

#define PREC highp

// fbm is inlined here on purpose: the shader include machinery uses a
// backslash path separator on Windows while shader references use forward
// slashes, so pulling in an external fbm helper resolves to nothing silently.
PREC vec3 mod289_3(PREC vec3 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
PREC vec2 mod289_2(PREC vec2 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
PREC vec3 permute(PREC vec3 x) { return mod289_3(((x * 34.0) + 1.0) * x); }

PREC float snoise(PREC vec2 v) {
    const PREC vec4 C = vec4(0.211324865405187, 0.366025403784439, -0.577350269189626, 0.024390243902439);
    PREC vec2 i  = floor(v + dot(v, C.yy));
    PREC vec2 x0 = v - i + dot(i, C.xx);
    PREC vec2 i1 = (x0.x > x0.y) ? vec2(1.0, 0.0) : vec2(0.0, 1.0);
    PREC vec4 x12 = x0.xyxy + C.xxzz;
    x12.xy -= i1;
    i = mod289_2(i);
    PREC vec3 p = permute(permute(i.y + vec3(0.0, i1.y, 1.0)) + i.x + vec3(0.0, i1.x, 1.0));
    PREC vec3 m = max(0.5 - vec3(dot(x0, x0), dot(x12.xy, x12.xy), dot(x12.zw, x12.zw)), 0.0);
    m = m * m; m = m * m;
    PREC vec3 x = 2.0 * fract(p * C.www) - 1.0;
    PREC vec3 h = abs(x) - 0.5;
    PREC vec3 ox = floor(x + 0.5);
    PREC vec3 a0 = x - ox;
    m *= 1.79284291400159 - 0.85373472095314 * (a0 * a0 + h * h);
    PREC vec3 g;
    g.x = a0.x * x0.x + h.x * x0.y;
    g.yz = a0.yz * x12.xz + h.yz * x12.yw;
    return 130.0 * dot(m, g);
}

PREC float fbm(PREC vec2 p, int octaves) {
    PREC float value = 0.0;
    PREC float amplitude = 0.5;
    PREC float frequency = 1.0;
    for (int i = 0; i < octaves; i++) {
        value += amplitude * snoise(p * frequency);
        frequency *= 2.0;
        amplitude *= 0.5;
    }
    return value;
}

#include <include/render_contract.hglsl>

// [RENDER_CONTRACT]
//   Pass:           PostProcess
//   Color0:         RGBA fullscreen result (multiplicative shadow factor)
//   GBuffer1:       READ-ONLY consumer; rc_pixelHandlesOwnShadow is the
//                   canonical threshold definition for the post-shadow mask.
//   ShadowContract: castsStatic=false, castsDynamic=false,
//                   skipsPostScreenShadow=false (this pass IS the
//                   post-screen shadow application)
//   StateContract:  depthTest=false, depthWrite=false, blend=Opaque,
//                   requiresMRT=false (single-attachment fullscreen pass)

in vec2 TexCoord;
layout(location = 0) out PREC vec4 FragColor;

uniform sampler2D sceneDepthTex;
uniform sampler2D sceneNormalTex;
uniform sampler2DShadow shadowMap;
uniform mat4 inverseViewProj;
uniform mat4 lightSpaceMatrix;
// Item 1: dynamic-shadow uniforms swap under MC2_SHADOW_CSM. This shader does
// NOT include shadow.hglsl (it carries its own sampler), so the variant lives
// here too. enableDynamicShadows + the call sites are unchanged either way.
#ifdef MC2_SHADOW_CSM
#ifndef MC2_SHADOW_CSM_MAX
#define MC2_SHADOW_CSM_MAX 3
#endif
uniform sampler2DArrayShadow dynamicShadowArray;
uniform mat4 dynamicCascadeMatrices[MC2_SHADOW_CSM_MAX];
uniform int  dynamicCsmCount;
uniform float dynamicCascadeTexelWorld[MC2_SHADOW_CSM_MAX]; // 2*cRad/mapSize, world units
uniform float csmDepthSpan;          // (farP-nearP) of shared CSM ortho z-row
// Per-cascade shadow resolution: the LAST cascade (dynamicCsmCount-1) lives in a
// SEPARATE lower-res 2D depth texture (see gos_postprocess.cpp / shadow.hglsl).
uniform sampler2DShadow dynamicFullMapShadow;
uniform float dynamicFullMapTexelWorld;
#else
uniform sampler2DShadow dynamicShadowMap;
uniform mat4 dynamicLightSpaceMatrix;
#endif
uniform vec2 screenSize;
uniform int enableShadows;
uniform int enableDynamicShadows;
uniform float shadowSoftness;
uniform int debugMode;    // 0=normal, 1=visualize classification
uniform float time;       // seconds, for animated cloud shadows

// Object (mech/building) self-shadow acne fix. lightDir = GL-world surface->light
// (set in gos_postprocess::runScreenShadow). objNormalBiasScale = normal-offset
// distance in cascade texels; 0 disables the offset (slope-scale term stays on).
uniform vec3  lightDir;
uniform float objNormalBiasScale;
// MECH self-shadow softening (mech pixels only). 1.0 default, 0 = legacy hard.
// Scales the extra PCF penumbra radius and the terminator smoothstep width.
uniform float mechSoft;
// PROP-SHADOW-RECEIVE-1 (MC2_PROP_SHADOW_RECEIVE, default OFF): when 1, static
// prop pixels (GBuffer1.a=0.25 class) ALSO sample the dynamic CSM cascades ->
// building self-shadow + prop-on-prop reception (the cascades already carry the
// full prop caster set). 0 = legacy: props take the static map only.
uniform int propShadowReceive;

const vec2 poissonDisk[8] = vec2[](
    vec2(-0.94201624, -0.39906216),
    vec2( 0.94558609, -0.76890725),
    vec2(-0.09418410, -0.92938870),
    vec2( 0.34495938,  0.29387760),
    vec2(-0.91588581,  0.45771432),
    vec2(-0.81544232, -0.87912464),
    vec2(-0.38277543,  0.27676845),
    vec2( 0.97484398,  0.75648379)
);

float sampleShadowMap(sampler2DShadow smap, vec3 worldPos, mat4 lsMatrix, int numTaps)
{
    vec4 lsPos = lsMatrix * vec4(worldPos, 1.0);
    vec3 projCoords = lsPos.xyz / lsPos.w;
    // z already [0,1]: both static and dynamic light orthos now emit
    // ZERO_TO_ONE clip-z (lockstep with gos_postprocess buildStatic/Dynamic).
    projCoords.xy = projCoords.xy * 0.5 + 0.5;

    if (projCoords.z > 1.0 || projCoords.z < 0.0) return 1.0;
    if (projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0) return 1.0;

    float bias = 0.003;
    float currentDepth = projCoords.z - bias;

    float angle = 6.2831853 * fract(sin(dot(worldPos.xz, vec2(12.9898, 78.233))) * 43758.5453);
    float ca = cos(angle), sa = sin(angle);
    mat2 rot = mat2(ca, sa, -sa, ca);

    vec2 texelSize = 1.0 / vec2(textureSize(smap, 0));
    float radius = max(shadowSoftness, 0.5);
    float shadow = 0.0;
    int taps = clamp(numTaps, 1, 8);
    for (int i = 0; i < taps; i++) {
        vec2 offset = rot * poissonDisk[i] * radius * texelSize;
        shadow += texture(smap, vec3(projCoords.xy + offset, currentDepth));
    }
    shadow /= float(taps);

    return mix(0.4, 1.0, shadow);
}

// Item 1: single entry point for the dynamic shadow lookup. Same signature in
// both variants so the call sites below never change.
#ifdef MC2_SHADOW_CSM
// N = GL-world surface normal of the object pixel (object path only). Used to
// slope-scale the depth bias and normal-offset the sample point off the surface
// to kill self-shadow acne. Terrain/grass/decals never reach this (early-out).
// softScale: 0 = legacy hard object shadow; >0 widens PCF penumbra and floor.
// shadowFloor: lower bound of the mix() (mech raised to subtle vs prop default).
float sampleDynamicShadow(vec3 worldPos, vec3 N, float softScale, float shadowFloor)
{
    float NdotL = max(dot(N, lightDir), 0.0);
    if (NdotL < 0.05) return 1.0;   // back-face guard: faces away from sun have no direct light to occlude (matches shadow.hglsl)
    int count = clamp(dynamicCsmCount, 1, MC2_SHADOW_CSM_MAX);
    for (int c = 0; c < count; ++c) {
        // Per-cascade shadow resolution: the last cascade uses the separate
        // full-map 2D texture + its own world-texel; near cascades the array.
        bool isFullMap = (c == count - 1);
        float cTexelWorld = isFullMap ? dynamicFullMapTexelWorld
                                      : dynamicCascadeTexelWorld[c];

        // NORMAL-OFFSET: push the sample point off the surface along N by a
        // texel-proportional distance (larger at grazing angles). Applied in
        // world space, per-cascade (uses that cascade's world texel size), so
        // the projected coord is consistent. Gated: 0 => no offset.
        float offsetDist = objNormalBiasScale * cTexelWorld * NdotL;
        vec3 samplePos = worldPos + N * offsetDist;

        vec4 lsPos = dynamicCascadeMatrices[c] * vec4(samplePos, 1.0);
        vec3 projCoords = lsPos.xyz / lsPos.w;
        projCoords.xy = projCoords.xy * 0.5 + 0.5;
        if (projCoords.z > 1.0 || projCoords.z < 0.0) continue;
        if (projCoords.xy != clamp(projCoords.xy, 0.0, 1.0)) continue;

        // Stage 3 texel-scaled bias + slope-scale top-up for grazing angles
        // (small; ground bias already reduced — this is object self-shadow only).
        float texelBias = 1.5 * cTexelWorld / max(csmDepthSpan, 1.0);
        float bias = 0.0012 + texelBias + 0.0005 * (1.0 - NdotL);
        float currentDepth = projCoords.z - bias;
        float angle = 6.2831853 * fract(sin(dot(worldPos.xz, vec2(12.9898, 78.233))) * 43758.5453);
        float ca = cos(angle), sa = sin(angle);
        mat2 rot = mat2(ca, sa, -sa, ca);
        vec2 texelSize = isFullMap
            ? (1.0 / vec2(textureSize(dynamicFullMapShadow, 0).xy))
            : (1.0 / vec2(textureSize(dynamicShadowArray, 0).xy));
        // Widen the penumbra for soft object shadows (mech). 8 taps when soft.
        float radius = max(shadowSoftness, 0.5) * (1.0 + 2.0 * softScale);
        int taps = (softScale > 0.0) ? 8 : 4;
        float shadow = 0.0;
        for (int i = 0; i < taps; i++) {
            vec2 offset = rot * poissonDisk[i] * radius * texelSize;
            shadow += isFullMap
                ? texture(dynamicFullMapShadow, vec3(projCoords.xy + offset, currentDepth))
                : texture(dynamicShadowArray, vec4(projCoords.xy + offset, float(c), currentDepth));
        }
        shadow /= float(taps);
        // TERMINATOR SMOOTHSTEP: near grazing (NdotL small) a flat facet pops
        // shadow state as the mech turns. Fade the self-shadow toward lit over
        // a soft band so it ramps instead of flipping. Width scales with soft.
        if (softScale > 0.0) {
            float band = 0.15 + 0.30 * softScale;
            float term = smoothstep(0.0, band, NdotL);
            shadow = mix(1.0, shadow, term);
        }
        return mix(shadowFloor, 1.0, shadow);
    }
    return 1.0;   // outside every cascade -> lit (static map covers far field)
}
#else
// Non-CSM legacy single dynamic map: normal-offset the world position before
// the lookup (slope-scale lives inside sampleShadowMap's fixed bias; the offset
// is the dominant acne fix). texelWorld is unavailable here, so scale the offset
// by a small world constant tied to objNormalBiasScale. Gated: 0 => no offset.
float sampleDynamicShadow(vec3 worldPos, vec3 N, float softScale, float shadowFloor)
{
    float NdotL = max(dot(N, lightDir), 0.0);
    if (NdotL < 0.05) return 1.0;
    vec3 samplePos = worldPos
                   + N * (objNormalBiasScale * 0.5 * NdotL);
    int taps = (softScale > 0.0) ? 8 : 4;
    float s = sampleShadowMap(dynamicShadowMap, samplePos, dynamicLightSpaceMatrix, taps);
    // Remap sampleShadowMap's fixed mix(0.4,1.0) back to a 0..1 visibility so
    // we can re-apply the per-object floor + terminator fade.
    float vis = clamp((s - 0.4) / 0.6, 0.0, 1.0);
    if (softScale > 0.0) {
        float band = 0.15 + 0.30 * softScale;
        float term = smoothstep(0.0, band, NdotL);
        vis = mix(1.0, vis, term);
    }
    return mix(shadowFloor, 1.0, vis);
}
#endif

vec3 reconstructWorldPos(vec2 uv, float depth)
{
    vec2 ndc_xy = uv * 2.0 - 1.0;
    // glClipControl(ZERO_TO_ONE) makes window depth and NDC z share [0, 1];
    // pass through. inverseViewProj inverts the D3D-style matrix natively now.
    float ndc_z = depth;
    vec4 worldPos4 = inverseViewProj * vec4(ndc_xy, ndc_z, 1.0);
    return worldPos4.xyz / worldPos4.w;
}

void main()
{
    PREC vec4 normalData = texture(sceneNormalTex, TexCoord);
    float depth = texture(sceneDepthTex, TexCoord).r;
    bool pixelHandlesOwnShadow = rc_pixelHandlesOwnShadow(normalData);
    // Debug mode: visualize what the shader classifies each pixel as
    if (debugMode == 1) {
        // reverse-Z: far=0, near=1, larger=closer; sky/cleared depth ~0.0.
        if (depth <= 0.0001) {
            FragColor = vec4(0.0, 0.0, 0.0, 1.0);  // black = sky/no depth
        } else if (pixelHandlesOwnShadow) {
            FragColor = vec4(0.4, 0.2, 0.0, 1.0);  // brown = self-shadow-handled (skipped by this pass)
        } else {
            // Non-terrain: reconstruct and show shadow result
            vec3 worldPos = reconstructWorldPos(TexCoord, depth);
            // Decode GBuffer normal (stored Stuff-space n*0.5+0.5) -> GL world.
            vec3 N_stuff = normalize(normalData.rgb * 2.0 - 1.0);
            vec3 N = vec3(-N_stuff.x, N_stuff.z, N_stuff.y);

            bool isProp = rc_isStaticPropNoSelfShadow(normalData);
            float shadow = 1.0;
            if (enableShadows == 1)
                shadow = min(shadow, sampleShadowMap(shadowMap, worldPos, lightSpaceMatrix, 8));
            if (enableDynamicShadows == 1 && (!isProp || propShadowReceive != 0))
                shadow = min(shadow, sampleDynamicShadow(worldPos, N, mechSoft,
                                                         isProp ? 0.4 : 0.55));

            if (shadow < 0.99) {
                FragColor = vec4(0.0, 0.0, shadow, 1.0);  // blue = shadowed
            } else {
                FragColor = vec4(0.0, 0.3, 0.0, 1.0);  // dark green = lit
            }
        }
        return;
    }

    // reverse-Z: far=0, near=1, larger=closer; sky ~0.0 (matches particle_billboard.frag idiom).
    if (depth <= 0.0001) {
        FragColor = vec4(1.0);
        return;
    }

    // Self-shadow-handled pixels (terrain, grass, decals, overlays): the
    // producing shader applied calcShadow + cloud shadows inline. Skip.
    if (pixelHandlesOwnShadow) {
        FragColor = vec4(1.0);
        return;
    }

    // Non-terrain (overlays, objects): reconstruct position for shadow + cloud.
    // Overlay pixels don't write depth — depth buffer holds terrain depth at their position,
    // so worldPos.xy matches the terrain surface below (same cloud UV as terrain inline).
    vec3 worldPos = reconstructWorldPos(TexCoord, depth);
    // Decode GBuffer normal (stored Stuff-space n*0.5+0.5) -> GL world. Object
    // pixels only (terrain/grass/decals already took the early-out above).
    vec3 objN_stuff = normalize(normalData.rgb * 2.0 - 1.0);
    vec3 objN = vec3(-objN_stuff.x, objN_stuff.z, objN_stuff.y);

    // PER-OBJECT-TYPE self-shadow classification (GBuffer1.a mask):
    //   a > 0.5         -> terrain/decal (early-out above, never reaches here)
    //   a in (0.1,0.5]  -> STATIC PROP (building): static map OK, NO dynamic
    //                      self-shadow (CPU never self-shadowed buildings)
    //                      UNLESS propShadowReceive=1 (PROP-SHADOW-RECEIVE-1)
    //   a <= 0.1        -> MECH: keep dynamic self-shadow, SOFTENED
    bool isStaticProp = rc_isStaticPropNoSelfShadow(normalData);

    // Cloud shadows moved to the fullscreen cloud pass (cloud.frag); this pass
    // now applies sun (static + dynamic) shadow only.
    float shadow = 1.0;
    if (enableShadows == 1) {
        // Static shadow map (baked terrain/static occluders) applies to BOTH
        // mechs and static props — buildings receiving cast shadows is fine.
        shadow = min(shadow, sampleShadowMap(shadowMap, worldPos, lightSpaceMatrix, 8));
    }
    if (enableDynamicShadows == 1 && !isStaticProp) {
        // Mech only: keep the dynamic-cascade self-shadow but soften it
        // (wider PCF penumbra + terminator smoothstep + raised floor) so flat
        // low-poly facets fade across the terminator instead of popping.
        float dynShadow = sampleDynamicShadow(worldPos, objN, mechSoft, 0.55);
        shadow = min(shadow, dynShadow);
    } else if (enableDynamicShadows == 1 && propShadowReceive != 0) {
        // PROP-SHADOW-RECEIVE-1 (gate MC2_PROP_SHADOW_RECEIVE, default OFF):
        // static props sample the dynamic CSM cascades too -> self-shadow +
        // prop-on-prop (cascades already contain the full prop caster set;
        // see txmmgr.cpp DynShadowCSM replay). Shares the existing PCF ledger
        // (shadowSoftness / objNormalBiasScale) and reuses the mechSoft
        // terminator smoothstep so flat low-poly facets fade, not pop.
        // Floor 0.4 = the PRESERVE screen-shadow floor (matches the static
        // map's mix(0.4,1.0)); min()-combine (NOT multiply) with the static
        // map caps the combined floor at 0.4 -> no double-darken stacking.
        float dynShadow = sampleDynamicShadow(worldPos, objN, mechSoft, 0.4);
        shadow = min(shadow, dynShadow);
    }
    // Static props (gate OFF): dynamic path skipped entirely -> shadow stays 1.0
    // from the dynamic cascade (matches CPU: no building self-shadow). They keep
    // N·L diffuse (in static_prop.frag) and cloud shadow (separate cloud.frag pass).

    float combined = shadow;
    FragColor = vec4(combined, combined, combined, 1.0);
}
