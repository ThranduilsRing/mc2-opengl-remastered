#ifndef TS_useCement
#define TS_useCement uT.useCement
#endif
#ifndef TS_enableShadows
#define TS_enableShadows uT.enableShadows
#endif
#ifndef TS_concreteDetail
#define TS_concreteDetail uT.concreteDetail
#endif
#include "../../include/fog_policy.hglsl"

// CEMENT-MIP-EDGE-1: antialiased "x < 0.5" for the cement transition cut; fw = the cut form's
// per-pixel footprint, so the edge is one pixel of coverage at any distance instead of a hard stair.
float cCutLt(float x, float fw) {
    float w = max(fw, 1e-4);
    return 1.0 - smoothstep(0.5 - w, 0.5 + w, x);
}

// Reconstruct the contour of the stock 64px concrete mask before pixel coverage.
// Four source texels remove magnified steps; opaque colour keeps its original LOD.
vec4 concreteSample(vec2 uv, float layer, vec2 dx, vec2 dy) {
    float lod = max(log2(float(textureSize(u_cementArt, 0).x) / 16.0), 0.0);
    vec2 lo = vec2(0.5 / 16.0), hi = vec2(1.0) - lo;
    vec2 p = clamp(uv, lo, hi);
    vec4 mask = textureLod(u_cementArt, vec3(p, layer), lod);
    // Explicit alpha gradients keep coverage defined across divergent tile words.
    vec2 ex = vec2(1.0 / 16.0, 0.0), ey = ex.yx;
    float ax = textureLod(u_cementArt, vec3(clamp(p + ex, lo, hi), layer), lod).a
             - textureLod(u_cementArt, vec3(clamp(p - ex, lo, hi), layer), lod).a;
    float ay = textureLod(u_cementArt, vec3(clamp(p + ey, lo, hi), layer), lod).a
             - textureLod(u_cementArt, vec3(clamp(p - ey, lo, hi), layer), lod).a;
    vec2 span = clamp(p + vec2(1.0 / 16.0), lo, hi) - clamp(p - vec2(1.0 / 16.0), lo, hi);
    vec2 gradient = vec2(ax, ay) / span;
    // Continue the contour to the tile boundary instead of flattening it there.
    float a = mask.a + dot(gradient, uv - p);
    float width = max(abs(dot(gradient, dx)) + abs(dot(gradient, dy)), 1.0 / 255.0);
    // The atlas has black transparent texels: recover edge colour before coverage.
    return vec4(mask.rgb / max(mask.a, 1e-4), smoothstep(0.5 - 0.5 * width, 0.5 + 0.5 * width, a));
}

// Permanent road art uses the former overlay's material response on the terrain
// receiver. The caller blends the authored shoulder over shaded ground.
vec3 shadeRoadArt(vec4 art, uint road, uint cw, vec2 dx, vec2 dy) {
    bool adopt = (road & 0x00010000u) != 0u;
    bool cementTile = (cw & 0x80000000u) != 0u;
    bool adoptAlbedo = adopt && cementTile && (cw & 0x40000000u) == 0u;
    vec3 color = art.rgb;
    if (adoptAlbedo) {
        uint layer = cw & 0xffffu;
        vec2 uv = fract(vec2(v_worldPos.x, -v_worldPos.y) / uT.cementWUPT);
        color = textureGrad(u_cementArt, vec3(uv, float(layer)), dx * vec2(1, -1) / uT.cementWUPT, dy * vec2(1, -1) / uT.cementWUPT).rgb;
    } else if (!adopt) {
        color *= vec3(0.82, 0.80, 0.76);
    }
    color *= vec3(uT.missionFlatR, uT.missionFlatG, uT.missionFlatB);
    vec3 normal = vec3(0, 0, 1);
    float shadow = min(TS_enableShadows != 0 ? terrainStaticShadow(v_worldPos) : 1.0,
                       terrainDynamicShadow(v_worldPos, normal));
    vec3 spec = vec3(0);
    if (TS_concreteDetail != 0) {
        MaterialDetail detail = cdEval(adoptAlbedo ? CD_LAYER_PAD : CD_LAYER_ROAD, v_worldPos.xy, dx, dy);
        float diffuse;
        cdShade(detail, normalize(uT.terrainLightDir.xyz), uT.ambientDayGate, diffuse, spec);
        color *= mix(1.0, detail.ao, 0.5) * detail.lum * diffuse;
        normal = detail.n;
    }
    color = max((color + spec) * shadow, vec3(0));
    if (!cementTile) {
        float dist = distance(v_worldPos.xy, mc2ToRender(uT.cameraPosMC2.xyz).xy);
        float fog = clamp(1.0 - exp(-dist * MC2_FOG_EXP_COEFFICIENT * exp(-max(v_worldPos.z, 0.0) * 0.002)), 0.0, 0.70);
        color = mix(color, vec3(0.58, 0.65, 0.75), fog);
    }
    if (uT.numPoolLights > 0) {
        float unusedContact = 0.0;
        color += tlp_terrainPoolLight(v_worldPos, uT.numPoolLights,
            uT.lightGridDim, vec2(uT.lightGridMinX, uT.lightGridMinY),
            vec2(uT.lightGridInvCellX, uT.lightGridInvCellY),
            uT.lightPoolSoft, uT.lightPoolMoverDim, uT.lightPoolMoverScale,
            uT.lightPoolMoverMax, uT.lightPoolCompress, 0.0, unusedContact,
            normal, vec3(0, 0, 1), false, 0.0);
    }
    if (uT.halfMap > 0.0)
        color = mix(color, vec3(0.58, 0.65, 0.75),
            smoothstep(uT.halfMap - mc2FogDistance(256.0), uT.halfMap - mc2FogDistance(32.0), max(abs(v_worldPos.x), abs(v_worldPos.y))));
    return color;
}

// Tile-level road art coverage, smoothly preserving the stock alpha shoulder.
float shadeRoadArtTile(out vec3 outColor) {
    outColor = vec3(0.0);
    vec2 roadDx = dFdx(v_worldPos.xy), roadDy = dFdy(v_worldPos.xy);
    int plane = uT.mapSide * uT.mapSide;
    if (TS_useCement != 0 && uC.cementWordsF.length() >= 2 * plane) {
        ivec2 tile = clamp(ivec2(floor(vec2(v_worldPos.x + uT.halfMap, uT.halfMap - v_worldPos.y) / 128.0)),
                           ivec2(0), ivec2(uT.mapSide - 1));
        int index = tile.x + tile.y * uT.mapSide;
        uint road = uC.cementWordsF[plane + index];
        if ((road & 0x80000000u) != 0u) {
            int layer = int(road & 0xffffu);
            vec2 uv = fract(vec2(v_worldPos.x + uT.halfMap, uT.halfMap - v_worldPos.y) / 128.0);
            vec4 art = textureGrad(u_cementArt, vec3(uv, float(layer)), roadDx * vec2(1, -1) / 128.0, roadDy * vec2(1, -1) / 128.0);
            float coverage = smoothstep(0.3, 0.7, art.a);
            uint cw = uC.cementWordsF[index];
            if ((cw & 0xc0800000u) == 0xc0800000u) {
                vec4 edge = concreteSample(uv, float(layer), roadDx * vec2(1, -1) / 128.0, roadDy * vec2(1, -1) / 128.0);
                coverage = edge.a;
                art.rgb = mix(edge.rgb, art.rgb, step(0.999, art.a));
            }
            if (coverage > 0.0) {
                outColor = shadeRoadArt(art, road, uC.cementWordsF[index], roadDx, roadDy);
                return coverage;
            }
        }
    }
    return 0.0;
}
