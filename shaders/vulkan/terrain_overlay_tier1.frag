// VK-PARITY-OVERLAY-1 TERRAIN OVERLAY TIER-1 -- FS.
// =============================================================================
// Port of shaders/terrain_overlay.frag (authored-art cement/pad/runway/road overlays and
// the cement transition tiles): decal texture albedo, OVERLAY-CEMENT-ADOPT-1 cement-atlas
// adoption on bare cement tiles, RUNWAY-CLOSE-1 flat-light arm, CEMENT-NIGHT-LIGHT-1
// mission flat light, static x dynamic sun shadow (no cement exemption),
// in-shader distance fog (skipped on cement), NIGHT-LIGHT-POOLS additive light pools,
// PAD-AREA-GLOW-1, map-edge haze. Co-writes MRT0 color + MRT1 = shadowHandled flat-up
// (0.5,0.5,1,1) into the shared scene GBuffer, binary-alpha (discard a < 0.5), opaque.
// Shares the terrain island's set 0 (UBO slot 0 + cement/pool/shadow bindings) via
// include/terrain_frame_vk.glsl so the lighting inputs are the terrain's own.
// NOT ported: the surfaceDebugMode / pathTint debug viz.
#version 450
#extension GL_GOOGLE_include_directive : require
#ifndef PREC
#define PREC highp
#endif
#include "include/terrain_frame_vk.glsl"
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

// Cement atlas sample: same UV math + half-texel inset as terrain_lod_chunk.frag / GL overlay.
vec3 overlayCementAlbedo(uint cw, vec2 wp) {
    uint cLayerIdx = cw & 0xFFFFu;
    int  cGridSide = max(uT.cementGridSide, 1);
    int  cCol = int(cLayerIdx) % cGridSide;
    int  cRow = int(cLayerIdx) / cGridSide;
    vec2 cTileUV   = fract(vec2(wp.x, -wp.y) / uT.cementWUPT);
    vec2 cSampleUV = cTileUV;
    if (pc.seamFix != 0) {
        float cCellPx = float(textureSize(u_cementAtlas, 0).x) / float(cGridSide);
        float cInset  = 0.5 / max(cCellPx, 1.0);
        cSampleUV = clamp(cTileUV, vec2(cInset), vec2(1.0 - cInset));
    }
    vec2 cAtlasUV = (vec2(float(cCol), float(cRow)) + cSampleUV) / float(cGridSide);
    return texture(u_cementAtlas, cAtlasUV).rgb * vec3(uT.missionFlatR, uT.missionFlatG, uT.missionFlatB);
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
    bool ovAuthoredArt = (ovCw & 0x01000000u) != 0u;          // bit 24: authored art on this tile
    bool ovAdoptAlbedo = ovCementTile && !ovAuthoredArt && uT.cementGridSide > 0;

    vec4 c;
    c.a = tex_color.a;                                         // shape always from the decal texture
    if (ovAdoptAlbedo) {
        c.rgb = overlayCementAlbedo(ovCw, WorldPos.xy);
    } else {
        c.rgb = (pc.cementAdopt != 0) ? tex_color.rgb : tex_color.rgb * vec3(0.82, 0.80, 0.76);
        c.rgb *= mix(missionFlat, vec3(1.0), pc.artFlatExempt);   // RUNWAY-CLOSE-1 arm
    }
    if (c.a < 0.5) discard;

    vec3 lightDir3 = normalize(uT.terrainLightDir.xyz);
    const vec3 shadowN = vec3(0.0, 0.0, 1.0);                  // flat-up, matches GL overlay
    LsvkShadowKnobs shKnobs = mv_shadowKnobs();
    float staticShadow = 1.0;
    if (uT.enableShadows != 0)
        staticShadow = lsvk_calcShadow(WorldPos, shadowN, lightDir3, u_staticShadow,
                                       uT.staticLightSpaceMatrix, uT.shadowSoftness, 8, shKnobs);
    float dynamicShadow = 1.0;
    if (uT.csmEnabled != 0) {
        vec3 shSun = lsvk_shadowSun(lightDir3, shKnobs);
        float NdotLdyn = max(dot(shadowN, shSun), 0.0);
        dynamicShadow = (NdotLdyn < 0.05) ? 1.0
                      : mv_calcDynamicShadowCsm(WorldPos, NdotLdyn, clamp(shSun.z, 0.0, 1.0), 4);
    } else if (uT.enableDynShadows != 0) {
        dynamicShadow = lsvk_calcDynamicShadowLegacy(WorldPos, shadowN, lightDir3, u_dynamicShadow,
                                                     uT.dynamicLightSpaceMatrix, uT.shadowSoftness, 4, shKnobs);
    }
    float shadow = staticShadow * dynamicShadow;
    // CONCRETE-ROAD-PBR-DETAIL: MC2's own overlay colour stays; asphalt (roads, runway) or concrete (adopted cement tiles) detail
    // adds occlusion, relative sun shading and a roughness sheen.
    vec3 cdSpec = vec3(0.0);
    if (uT.concreteDetail != 0) {
        MaterialDetail cd = cdEval(ovAdoptAlbedo ? CD_LAYER_PAD : CD_LAYER_ROAD, WorldPos.xy, cdWpDx, cdWpDy);
        float cdDiff; vec3 cdSp;
        cdShade(cd, lightDir3, uT.ambientDayGate, cdDiff, cdSp);
        c.rgb *= mix(1.0, cd.ao, 0.9) * cdDiff;
        cdSpec = cdSp;
    }
    c.rgb = max(c.rgb * shadow + cdSpec * shadow, vec3(0.0));

    // In-shader distance fog (skipped on cement tiles: the shared screen-space fog does it once).
    float camDist2D = distance(WorldPos.xy, uT.cameraPosMC2.xy);
    float heightScale = exp(-max(WorldPos.z, 0.0) * 0.002);
    float fogAmount = clamp(1.0 - exp(-camDist2D * 0.00006 * heightScale), 0.0, 0.70);
    const vec3 fogCol = vec3(0.58, 0.65, 0.75);
    if (!ovCementTile)
        c.rgb = mix(c.rgb, fogCol, fogAmount);

    // NIGHT-LIGHT-POOLS-2b: additive pool AFTER the fog mix (chunk terrain applies no fog to it).
    if (uT.numPoolLights > 0) {
        float moverContactUnused = 0.0;
        c.rgb += tlp_terrainPoolLight(WorldPos, uT.numPoolLights,
                                      uT.lightGridDim, vec2(uT.lightGridMinX, uT.lightGridMinY),
                                      vec2(uT.lightGridInvCellX, uT.lightGridInvCellY),
                                      uT.lightPoolSoft, uT.lightPoolMoverDim, uT.lightPoolMoverScale,
                                      uT.lightPoolMoverMax, uT.lightPoolCompress,
                                      0.0, moverContactUnused);
    }
    // PAD-AREA-GLOW-1 (post-fog, same as the chunk terrain).
    vec3 padGlow = vec3(uT.padGlowR, uT.padGlowG, uT.padGlowB);
    if (uT.useCement != 0 && dot(padGlow, padGlow) > 0.0)
        c.rgb += padGlow * tlp_padGlowMask(WorldPos.xy, uT.halfMap, uT.mapSide, uT.padGlowSpill);

    // Map-edge haze (same ramp as gos_terrain.frag).
    if (pc.mapHalfExtent > 0.0) {
        const vec3 edgeSkyCol = vec3(0.58, 0.65, 0.75);
        float chebDist  = max(abs(WorldPos.x), abs(WorldPos.y));
        float edgeHaze  = smoothstep(pc.mapHalfExtent - 256.0, pc.mapHalfExtent - 32.0, chebDist);
        c.rgb = mix(c.rgb, edgeSkyCol, edgeHaze);
    }

    FragColor = c;
    GBuffer1  = vec4(0.5, 0.5, 1.0, 1.0);                      // rc_gbuffer1_shadowHandled_flatUp
}
