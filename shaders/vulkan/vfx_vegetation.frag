// Opaque cutouts sharing the terrain's current lighting and shadow inputs.
#version 450
#extension GL_GOOGLE_include_directive : require
#define PREC highp
#define TERRAIN_RESOURCE_SET 3
#include "../include/render_contract.hglsl"
#include "include/terrain_frame_vk.glsl"
#include "include/ground_lighting.glsl"
layout(set = 1, binding = 0) uniform sampler2DArray uAtlas;
layout(set = 3, binding = 16) uniform sampler2D u_macroGradient;
layout(location = 0) in vec2 v_uv;
layout(location = 2) flat in uint v_flags;
layout(location = 3) in vec4 v_vegData;
layout(location = 4) in vec2 v_vegAux;   // y = travelling gust strength (grass only, vegetation_vk.glsl)
layout(location = 5) in vec3 v_cardNormal;
layout(location = 6) in vec3 v_worldPos;
layout(location = 7) flat in vec3 v_rootPos;
layout(location = 8) flat in uint v_atlasLayer;
layout(location = 9) in vec3 v_shadowWorld;
layout(location = 0) out vec4 o_color;
layout(location = 1) out vec4 o_normal;

vec3 rootNormal() {
    vec2 cell = vec2(v_rootPos.x + uT.halfMap, uT.halfMap - v_rootPos.y) / 128.0;
    return normalize(vec3(textureLod(u_macroGradient, (cell + 1.5) / float(uT.mapSide + 2), 0.0).rg, 1.0));
}
vec3 rootMacro() {
    vec2 uv = vec2((v_rootPos.x-uT.atlasTopLeftX)*uT.atlasOneOverWorldUnits,
                  (uT.atlasTopLeftY-v_rootPos.y)*uT.atlasOneOverWorldUnits);
    vec3 macro = uT.colormapReady != 0 ? textureLod(u_colormap, uv, 0.0).rgb : vec3(0.4, 0.45, 0.25);
    // P1 weeds are rooted in the pad/road. Sample its atlas material where a
    // cement word exists; baked-overlay-only cells keep their local macro tint.
    if ((v_flags & 64u) != 0u && uT.useCement != 0 && uT.mapSide > 0) {
        ivec2 cell = clamp(ivec2(floor(vec2(v_rootPos.x+uT.halfMap, uT.halfMap-v_rootPos.y)/128.0)), ivec2(0), ivec2(uT.mapSide-1));
        uint word = uC.cementWordsF[cell.x+cell.y*uT.mapSide];
        if ((word & 0x80000000u) != 0u) {
            uint layer = word & 0xffffu;
            vec2 local = fract(vec2(v_rootPos.x, -v_rootPos.y)/max(uT.cementWUPT, 1.0));
            macro = textureLod(u_cementArt, vec3(local, float(layer)), 0.0).rgb * vec3(uT.missionFlatR, uT.missionFlatG, uT.missionFlatB);
        }
    }
    return macro;
}
// Smooth value noise for the world-space sward colour patches (spatial, frame-stable).
float vegHash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float vegPatchNoise(vec2 p) {
    vec2 c = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(vegHash(c), vegHash(c + vec2(1.0, 0.0)), f.x),
               mix(vegHash(c + vec2(0.0, 1.0)), vegHash(c + vec2(1.0, 1.0)), f.x), f.y);
}
const float kVegShoreDryWU = 70.0;   // the sward is driest at the beach and greens over this height above water
void main() {
    vec4 texel = texture(uAtlas, vec3(v_uv, float(v_atlasLayer)));
    if (texel.a < 0.5) discard;
    vec3 macro = rootMacro();
    // Cards are painted in the mission palette (data/vegetation/palettes/mc2_01.json):
    // their own hue and value separate species and contrast with the ground. The
    // ground sets exposure (sand stays bright, night stays dark) and keeps a share
    // of its hue so other biomes still read; kPaletteGround is the ground luma the
    // cards were painted against.
    const float kPaletteGround = 0.34;
    float cardValue = dot(texel.rgb, vec3(0.299, 0.587, 0.114));
    float groundValue = dot(macro, vec3(0.299, 0.587, 0.114));
    vec3 groundTone = macro * mix(0.75, 1.15, clamp(cardValue*2.0, 0.0, 1.0));
    vec3 painted = texel.rgb * clamp(groundValue / kPaletteGround, 0.45, 1.6);
    vec3 albedo = mix(groundTone, painted, 0.75);
    // Stable per-instance value and warm/cool jitter, so a patch is not one flat tone.
    float jitter = fract(v_vegData.x * 7.31);
    albedo *= mix(0.80, 1.16, jitter) * mix(vec3(1.05,0.98,0.88), vec3(0.93,1.03,1.01), v_vegData.x);
    if ((v_flags & 128u) != 0u) albedo = mix(albedo,macro*vec3(1.03,1.03,1.08),0.65);
    // Grass blades: olive-dark roots under the canopy, light straw tips, and the
    // gust band brightens bent blades (their sheen faces up) so waves read from above.
    float tip = 1.0 - v_vegData.y;
    bool grass = v_atlasLayer < 4u && (v_flags & 192u) == 0u;
    if (grass) {
        // Owner ref 3 (2026-11-10, gameplay camera): the sward is one mottled carpet whose
        // colour comes in large soft world-space patches (olive green, yellow-green, dry
        // straw), drier toward the shore. Per-blade picks only add close-up grain (refs 1/2).
        float value = dot(albedo, vec3(0.299, 0.587, 0.114));
        float pick = fract(v_vegData.x * 13.71);
        float patchTone = 0.62 * vegPatchNoise(v_rootPos.xy / 170.0) + 0.38 * vegPatchNoise(v_rootPos.xy / 48.0 + 17.3);
        float shore = 1.0 - smoothstep(6.0, kVegShoreDryWU, v_rootPos.z - uT.shorelineWaterElev);
        float dryness = clamp((patchTone - 0.30) * 1.6 + 0.55 * shore + (pick - 0.5) * 0.25, 0.0, 1.0);
        const vec3 kOlive = vec3(0.34, 0.38, 0.17), kYellowGreen = vec3(0.47, 0.49, 0.21), kStraw = vec3(0.60, 0.52, 0.32);
        vec3 hue = dryness < 0.5 ? mix(kOlive, kYellowGreen, dryness * 2.0) : mix(kYellowGreen, kStraw, dryness * 2.0 - 1.0);
        // The card keeps blade-to-blade value; straw reads lighter than olive as in the reference.
        vec3 sward = hue * (value / 0.40);
        float dry = pick < 0.10 ? 0.8 : smoothstep(0.80, 1.0, tip) * 0.5;      // a few dry blades and tips up close
        albedo = mix(sward, kStraw * (value / 0.40) * 1.08, dry);
        albedo *= mix(vec3(0.62, 0.68, 0.52), vec3(1.0), smoothstep(0.0, 0.55, tip));
        albedo *= 1.0 + v_vegAux.y * (0.10 + 0.22 * tip);
        // Field wildflowers: white heads (a few yellow) on the tops of short stalks.
        if ((v_flags & 12u) == 12u && tip > 0.70) {
            float exposure = clamp(groundValue / kPaletteGround, 0.45, 1.6);
            albedo = (pick < 0.15 ? vec3(0.78, 0.62, 0.16) : vec3(0.86, 0.86, 0.80)) * exposure;
        }
    }
    vec3 rootN = rootNormal();
    vec3 cardN = normalize(gl_FrontFacing ? v_cardNormal : -v_cardNormal);
    vec3 N = normalize(mix(rootN, cardN, 0.30*(1.0-v_vegData.y)));
    float staticS = uT.enableShadows != 0 ? terrainStaticShadow(v_shadowWorld) : 1.0;
    float shadow = min(staticS, terrainDynamicShadow(v_shadowWorld, rootN));
    float normalLight = computeNdotLBand(N, uT.terrainLightDir.xyz);
    // Thin-leaf transmission, bounded and shadowed with direct light once.
    normalLight += 0.08 * max(dot(-cardN, uT.terrainLightDir.xyz), 0.0) * (1.0-v_vegData.y);
    // Grass tips are thin enough to glow when backlit (either card face).
    if (grass) normalLight += 0.22 * tip * tip * abs(dot(cardN, uT.terrainLightDir.xyz));
    vec3 lit = uT.lightingEnabled != 0 ? shadeGround(albedo, rootN, normalLight, shadow, 0.0, v_worldPos, rootN, false, 0.0) : albedo;
    o_color = vec4(lit, 1.0);
    o_normal = rc_gbuffer1_shadowHandled(N);
}
