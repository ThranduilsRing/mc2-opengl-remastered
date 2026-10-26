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
            uint side = uint(max(uT.cementGridSide, 1));
            uint layer = word & 0xffffu;
            vec2 local = fract(vec2(v_rootPos.x, -v_rootPos.y)/max(uT.cementWUPT, 1.0));
            float inset = 0.5*float(side)/float(textureSize(u_cementAtlas, 0).x);
            vec2 atlas = (vec2(layer%side, layer/side)+clamp(local, vec2(inset), vec2(1.0-inset)))/float(side);
            macro = textureLod(u_cementAtlas, atlas, 0.0).rgb * vec3(uT.missionFlatR, uT.missionFlatG, uT.missionFlatB);
        }
    }
    return macro;
}
void main() {
    vec4 texel = texture(uAtlas, vec3(v_uv, float(v_atlasLayer)));
    if (texel.a < 0.5) discard;
    vec3 macro = rootMacro();
    // Preserve card detail while deriving hue AND value from the ground. Sand
    // stays tan, night stays dark; a stable small variation cannot make a ring.
    float cardValue = dot(texel.rgb, vec3(0.299, 0.587, 0.114));
    vec3 albedo = macro * mix(0.75, 1.15, clamp(cardValue*2.0, 0.0, 1.0));
    albedo *= mix(0.96, 1.04, v_vegData.x);
    vec3 rootN = rootNormal();
    vec3 cardN = normalize(gl_FrontFacing ? v_cardNormal : -v_cardNormal);
    vec3 N = normalize(mix(rootN, cardN, 0.30*(1.0-v_vegData.y)));
    float staticS = uT.enableShadows != 0 ? terrainStaticShadow(v_shadowWorld) : 1.0;
    float shadow = min(staticS, terrainDynamicShadow(v_shadowWorld, rootN));
    float normalLight = computeNdotLBand(N, uT.terrainLightDir.xyz);
    // Thin-leaf transmission, bounded and shadowed with direct light once.
    normalLight += 0.08 * max(dot(-cardN, uT.terrainLightDir.xyz), 0.0) * (1.0-v_vegData.y);
    vec3 lit = uT.lightingEnabled != 0 ? shadeGround(albedo, rootN, normalLight, shadow, 0.0, v_worldPos, rootN, false, 0.0) : albedo;
    o_color = vec4(lit, 1.0);
    o_normal = rc_gbuffer1_shadowHandled(N);
}
