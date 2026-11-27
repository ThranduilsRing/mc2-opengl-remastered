// VK-PARITY-OVERLAY-1 TERRAIN OVERLAY TIER-1 -- FS.
// =============================================================================
// Port of shaders/terrain_overlay.frag (authored-art cement/pad/runway/road overlays and
// the cement transition tiles): decal texture albedo, OVERLAY-CEMENT-ADOPT-1 cement-atlas
// adoption on bare cement tiles, RUNWAY-CLOSE-1 flat-light arm, CEMENT-NIGHT-LIGHT-1
// mission flat light, static x dynamic sun shadow,
// in-shader distance fog (skipped on cement), NIGHT-LIGHT-POOLS additive light pools,
// map-edge haze. Co-writes MRT0 color + MRT1 = shadowHandled flat-up
// (0.5,0.5,1,1) into the shared scene GBuffer, binary-alpha (discard a < 0.5), opaque.
// Shares the terrain island's set 0 (UBO slot 0 + cement/pool/shadow bindings) via
// include/terrain_frame_vk.glsl so the lighting inputs are the terrain's own.
// NOT ported: the surfaceDebugMode / pathTint debug viz.
#version 460
#ifdef MC2_RT_SUN_SHADOWS
#extension GL_EXT_ray_query : require
#endif
#extension GL_GOOGLE_include_directive : require
#ifndef PREC
#define PREC highp
#endif
#include "../include/axis.hglsl"   // mc2ToRender: cameraPosMC2 is in Stuff space
#include "../include/edge_haze.hglsl"
#include "include/terrain_frame_vk.glsl"
#ifdef MC2_RT_SUN_SHADOWS
#ifndef MC2_RT_SUN_SET
#define MC2_RT_SUN_SET 2   // set 1 binding 0 is this pass's tex1
#endif
#include "include/rt_sun_shadow.glsl"
#endif
#include "include/concrete_detail_vk.glsl"

layout(location = 0) in vec3  WorldPos;
layout(location = 1) in vec2  Texcoord;
layout(location = 2) in float FogValue;
layout(location = 3) in vec4  Color;

layout(location = 0) out vec4 FragColor;
layout(location = 1) out vec4 GBuffer1;

layout(set = 1, binding = 0) uniform sampler2D tex1;

layout(push_constant) uniform OverlayPC {
    vec4  fogColor;
    float artFlatExempt;
    float mapHalfExtent;
    int   biasLegacy;
    int   cementAdopt;
    int   seamFix;
    int   _pad0;
    int   _pad1;
    int   _pad2;
} pc;

// GL overlayCementWord(): this fragment's own 128wu cement word, 0 when the adopt path is
// disarmed / the cement words are not resident (uT.useCement == 0).
uint overlayCementWord(vec2 wp) {
    if (pc.cementAdopt == 0 || uT.useCement == 0) return 0u;
    int cx = clamp(int(floor((wp.x + uT.halfMap) / 128.0)), 0, uT.mapSide - 1);
    int cy = clamp(int(floor((uT.halfMap - wp.y) / 128.0)), 0, uT.mapSide - 1);
    return uC.cementWordsF[cx + cy * uT.mapSide];
}

// Cement art sample: one texture-array layer per catalog cell (CEMENT-ARRAY-1).
vec3 overlayCementAlbedo(uint cw, vec2 wp, vec2 dx, vec2 dy) {
    uint cLayerIdx = cw & 0xFFFFu;
    vec2 cTileUV   = fract(vec2(wp.x, -wp.y) / uT.cementWUPT);
    return textureGrad(u_cementArt, vec3(cTileUV, float(cLayerIdx)), dx * vec2(1, -1) / uT.cementWUPT, dy * vec2(1, -1) / uT.cementWUPT).rgb * vec3(uT.missionFlatR, uT.missionFlatG, uT.missionFlatB);
}

void main() {
    if (pc._pad0 == 2) {
        ivec2 size = textureSize(tex1, 0);
        ivec2 at = clamp(ivec2(floor(Texcoord * vec2(size))), ivec2(0), size - 1);
        vec4 mine = texelFetch(tex1, at, 0);
        if (mine.a < 0.5) discard;
        FragColor = mine;
        GBuffer1 = vec4(0.5, 0.5, 1.0, 1.0); // Unlit mine sprites handle their own lighting.
        return;
    }
    vec4 tex_color = texture(tex1, Texcoord);
    vec2 cdWpDx = dFdx(WorldPos.xy), cdWpDy = dFdy(WorldPos.xy);   // CONCRETE-ROAD-PBR-DETAIL: uniform-flow derivatives
    vec3 missionFlat = vec3(uT.missionFlatR, uT.missionFlatG, uT.missionFlatB);

    uint ovCw          = overlayCementWord(WorldPos.xy);
    bool ovCementTile  = (ovCw & 0x80000000u) != 0u;          // bit 31 valid
    bool ovAuthoredArt = (ovCw & 0x00200000u) != 0u;          // bit 21: authored art on this tile
    bool ovAdoptAlbedo = ovCementTile && !ovAuthoredArt && uT.cementLayerCount > 0;

    vec4 c;
    c.a = tex_color.a;                                         // shape always from the decal texture
    if (ovAdoptAlbedo) {
        c.rgb = overlayCementAlbedo(ovCw, WorldPos.xy, cdWpDx, cdWpDy);
    } else {
        c.rgb = (pc.cementAdopt != 0) ? tex_color.rgb : tex_color.rgb * vec3(0.82, 0.80, 0.76);
        c.rgb *= mix(missionFlat, vec3(1.0), pc.artFlatExempt);   // RUNWAY-CLOSE-1 arm
    }
    if (c.a < 0.5) discard;

    vec3 lightDir3 = normalize(uEnv.sunDirLive.xyz);
    const vec3 shadowN = vec3(0.0, 0.0, 1.0);                  // flat-up, matches GL overlay
    float staticS=uT.enableShadows!=0?terrainStaticShadow(WorldPos):1.0;
#ifdef MC2_RT_SUN_SHADOWS
    if (uT.enableShadows!=0 && WorldPos.z>=uT.shorelineWaterElev)
        staticS=min(staticS,mix(sunShadowFloor(0.4),1.0,rtPropSunVisibility(WorldPos,shadowN,moverFrame.sun.xyz)));
#endif
    float shadow=min(staticS,terrainDynamicShadow(WorldPos,shadowN));
    // CONCRETE-ROAD-PBR-DETAIL: MC2's own overlay colour stays; asphalt (roads, runway) or concrete (adopted cement tiles) detail
    // adds occlusion, relative sun shading and a roughness sheen.
    vec3 cdSpec = vec3(0.0);
    vec3 surfaceN = shadowN;
    if (uT.concreteDetail != 0) {
        MaterialDetail cd = cdEval(ovAdoptAlbedo ? CD_LAYER_PAD : CD_LAYER_ROAD, WorldPos.xy, cdWpDx, cdWpDy);
        float cdDiff; vec3 cdSp;
        cdShade(cd, lightDir3, uT.ambientDayGate, cdDiff, cdSp);
        c.rgb *= mix(1.0, cd.ao, 0.5) * cd.lum * cdDiff;
        cdSpec = cdSp;
        surfaceN = cd.n;
    }
    c.rgb = max(c.rgb * shadow + cdSpec * shadow, vec3(0.0));
    c.rgb *= mix(vec3(1.0), uEnv.sceneLight.x * uEnv.sceneLight.yzw, pc.artFlatExempt); // Live mission light already carries the night profile.

    // Local radiance is composed before the single opaque fog pass.
    if (uT.numPoolLights > 0) {
        float moverContactUnused = 0.0;
        c.rgb += (uEnv.sceneLight.x < 1.0 ? vec3(0.3) : vec3(1.0)) * tlp_terrainPoolLight(WorldPos, uT.numPoolLights,
                                      uT.lightGridDim, vec2(uT.lightGridMinX, uT.lightGridMinY),
                                      vec2(uT.lightGridInvCellX, uT.lightGridInvCellY),
                                      uT.lightPoolSoft, uT.lightPoolMoverDim, uT.lightPoolMoverScale,
                                      uT.lightPoolMoverMax, uT.lightPoolCompress,
                                      0.0, moverContactUnused, surfaceN, shadowN, false, 0.0);
    }
    FragColor = c;
    GBuffer1  = vec4(0.5, 0.5, 1.0, 1.0);                      // rc_gbuffer1_shadowHandled_flatUp
}
