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

// Border taps belong to the neighbouring map tile, never the opposite edge of
// this array layer. Both sides of a join therefore interpolate the same RGBA.
vec4 concreteTap(ivec2 tile, ivec2 texel, int plane, int lod, int side) {
    ivec2 offset = ivec2(floor(vec2(texel) / float(side)));
    tile += offset;
    if (any(lessThan(tile, ivec2(0))) || any(greaterThanEqual(tile, ivec2(uT.mapSide)))) return vec4(0);
    uint word = uC.cementWordsF[plane + tile.x + tile.y * uT.mapSide];
    if ((word & 0x80000000u) == 0u) return vec4(0);
    return texelFetch(u_cementArt, ivec3(texel - offset * side, int(word & 0xffffu)), lod);
}

// Keep the accepted 16px contour filter; its explicit spatial gradient gives
// pixel coverage even in divergent tile branches.
vec4 concreteSample(vec2 uv, ivec2 tile, int plane, vec2 dx, vec2 dy) {
    int lod = max(int(log2(float(textureSize(u_cementArt, 0).x) / 16.0)), 0);
    int side = textureSize(u_cementArt, lod).x;
    vec2 p = uv * float(side) - 0.5;
    ivec2 t = ivec2(floor(p));
    vec2 f = fract(p);
    vec4 a = concreteTap(tile, t, plane, lod, side);
    vec4 b = concreteTap(tile, t + ivec2(1, 0), plane, lod, side);
    vec4 c = concreteTap(tile, t + ivec2(0, 1), plane, lod, side);
    vec4 d = concreteTap(tile, t + ivec2(1, 1), plane, lod, side);
    vec4 mask = mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
    vec2 gradient = vec2(mix(b.a - a.a, d.a - c.a, f.y),
                         mix(c.a - a.a, d.a - b.a, f.x)) * float(side);
    float width = max(abs(dot(gradient, dx)) + abs(dot(gradient, dy)), 1.0 / 255.0);
    // Erode only pad borders by a few native texels. World coordinates and
    // joined mask taps keep the grass/soil intrusion continuous across tiles.
    float cut = 0.5;
    float grime = 0.0;
    if (plane == 0 && mask.a > 0.0 && mask.a < 1.0) {
        float texelSlope = max(length(gradient) / float(textureSize(u_cementArt, 0).x), 1e-5);
        float noise = 0.5 + 0.5 * snoise(v_worldPos.xy * 0.65);
        cut += texelSlope * (1.0 + 2.0 * noise);
        width = max(width, texelSlope);
        grime = (1.0 - smoothstep(cut, cut + 3.0 * texelSlope, mask.a)) * (0.10 + 0.15 * noise);
    }
    // The atlas has black transparent texels: recover edge colour before coverage.
    return vec4(mask.rgb / max(mask.a, 1e-4) * (1.0 - grime), smoothstep(cut - 0.5 * width, cut + 0.5 * width, mask.a));
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
                vec4 edge = concreteSample(uv, tile, plane, roadDx * vec2(1, -1) / 128.0, roadDy * vec2(1, -1) / 128.0);
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
