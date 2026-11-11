// Opaque cutouts sharing the terrain's current lighting and shadow inputs.
#version 450
#extension GL_GOOGLE_include_directive : require
#define PREC highp
#define TERRAIN_RESOURCE_SET 3
#include "../include/render_contract.hglsl"
#include "include/terrain_frame_vk.glsl"
#include "include/ground_lighting.glsl"
#include "include/world_patch_noise.glsl"   // the ground's patch field and palette (terrain_tier1.frag uses the same)
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
const float kVegShoreDryWU = 70.0;   // the sward is driest at the beach and greens over this height above water
const float kBladeLift = 1.05;       // blades read a little lighter than the ground patch under them (1.15 bleached the field)
void main() {
    vec4 texel = texture(uAtlas, vec3(v_uv, float(v_atlasLayer)));
    // Coarse mips average thin blades toward zero alpha, so a fixed 0.5 cut erases distant grass
    // into speckles; lift alpha with the sampled mip level (spatial, per pixel) to keep its cover.
    float alphaLift = 1.0 + 0.30 * max(textureQueryLod(uAtlas, v_uv).x, 0.0);
    if (texel.a * alphaLift < 0.5) discard;
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
    // Owner 2026-11-10: "grass should only take about 15% of its color from the ground - we
    // need contrast" (shrubs read as ground-coloured blobs on sand). Every card keeps 85% of
    // its own palette hue (was grass/tufts 0.75, woody 0.75); the ground's 15% plus its value
    // (night stays dark) are all it inherits, and a bright ground lifts it at most 1.2x.
    const float kVegOwnHue = 0.85;
    bool woody = v_atlasLayer >= 4u;
    vec3 painted = texel.rgb * clamp(groundValue / kPaletteGround, 0.45, 1.2);
    vec3 albedo = mix(groundTone, painted, kVegOwnHue);
    // Stable per-instance value and warm/cool jitter, so a patch is not one flat tone.
    float jitter = fract(v_vegData.x * 7.31);
    albedo *= mix(0.80, 1.16, jitter) * mix(woody ? vec3(1.08,1.04,0.78) : vec3(1.05,0.98,0.88), vec3(0.93,1.03,1.01), v_vegData.x);
    if ((v_flags & 128u) != 0u) albedo = mix(albedo,macro*vec3(1.03,1.03,1.08),0.65);
    // Grass blades: olive-dark roots under the canopy, light straw tips, and the
    // gust band brightens bent blades (their sheen faces up) so waves read from above.
    float tip = 1.0 - v_vegData.y;
    bool grass = v_atlasLayer < 4u && (v_flags & 192u) == 0u;
    if (grass) {
        // Owner ref 3 (2026-11-10, gameplay camera): the sward is one mottled carpet. Blades take
        // the SAME world patch field and palette as the ground (world_patch_noise.glsl), so a dry
        // patch on the ground is a dry patch in the grass above it; drier toward the shore.
        // Per-blade picks only add close-up grain (refs 1/2).
        float value = dot(albedo, vec3(0.299, 0.587, 0.114));
        float pick = fract(v_vegData.x * 13.71);
        vec2 field = worldPatchField(v_rootPos.xy);
        field.x = clamp(field.x + (pick - 0.5) * 0.20, 0.0, 1.0);
        float shore = 1.0 - smoothstep(6.0, kVegShoreDryWU, v_rootPos.z - uT.shorelineWaterElev);
        // The card keeps blade-to-blade value; straw reads lighter than olive as in the reference.
        float bladeValue = kBladeLift * (value / 0.46);
        vec3 sward = worldPatchColor(field, 0.50 * shore) * bladeValue;
        // A few dry blades and tips up close; kept light so the shared patch field, not per-blade straw, sets the look.
        float dry = pick < 0.05 ? 0.6 : smoothstep(0.85, 1.0, tip) * 0.15;
        albedo = mix(groundTone, mix(sward, WPN_STRAW * bladeValue, dry), kVegOwnHue);   // 15% ground, as for every card
        albedo *= mix(vec3(0.62, 0.68, 0.52), vec3(1.0), smoothstep(0.0, 0.55, tip));
        albedo *= 1.0 + v_vegAux.y * (0.05 + 0.10 * tip);
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
    if (grass) normalLight += 0.10 * tip * tip * abs(dot(cardN, uT.terrainLightDir.xyz));
    vec3 lit = uT.lightingEnabled != 0 ? shadeGround(albedo, rootN, normalLight, shadow, 0.0, v_worldPos, rootN, false, 0.0) : albedo;
    o_color = vec4(lit, 1.0);
    o_normal = rc_gbuffer1_shadowHandled(N);
}
