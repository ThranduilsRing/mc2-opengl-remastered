// CEMENT-MIP-EDGE-1: antialiased "x < 0.5" for the cement transition cut; fw = the cut form's
// per-pixel footprint, so the edge is one pixel of coverage at any distance instead of a hard stair.
float cCutLt(float x, float fw) {
    float w = max(fw, 1e-4);
    return 1.0 - smoothstep(0.5 - w, 0.5 + w, x);
}

// Permanent road art uses the former overlay's material response on the terrain
// receiver. Alpha remains binary; rejected texels expose the original ground.
vec3 shadeRoadArt(vec4 art, uint road, uint cw, vec2 dx, vec2 dy) {
    bool adopt = (road & 0x00010000u) != 0u;
    bool cementTile = (cw & 0x80000000u) != 0u;
    bool adoptAlbedo = adopt && cementTile && (cw & 0x01000000u) == 0u;
    vec3 color = art.rgb;
    if (adoptAlbedo) {
        int grid = max(uT.cementGridSide, 1);
        uint layer = cw & 0xffffu;
        vec2 uv = fract(vec2(v_worldPos.x, -v_worldPos.y) / uT.cementWUPT);
        if (uT.cementAtlasSeamFix != 0) {
            float inset = 0.5 * float(grid) / float(textureSize(u_cementAtlas, 0).x);
            uv = clamp(uv, vec2(inset), vec2(1.0 - inset));
        }
        color = textureLod(u_cementAtlas, (vec2(int(layer) % grid, int(layer) / grid) + uv) / float(grid), 0.0).rgb;
    } else if (!adopt) {
        color *= vec3(0.82, 0.80, 0.76);
    }
    color *= vec3(uT.missionFlatR, uT.missionFlatG, uT.missionFlatB);
    vec3 normal = vec3(0, 0, 1);
    float shadow = min(uT.enableShadows != 0 ? terrainStaticShadow(v_worldPos) : 1.0,
                       terrainDynamicShadow(v_worldPos, normal));
    vec3 spec = vec3(0);
    if (uT.concreteDetail != 0) {
        MaterialDetail detail = cdEval(adoptAlbedo ? CD_LAYER_PAD : CD_LAYER_ROAD, v_worldPos.xy, dx, dy);
        float diffuse;
        cdShade(detail, normalize(uT.terrainLightDir.xyz), uT.ambientDayGate, diffuse, spec);
        color *= mix(1.0, detail.ao, 0.5) * detail.lum * diffuse;
        normal = detail.n;
    }
    color = max((color + spec) * shadow, vec3(0));
    if (!cementTile) {
        float dist = distance(v_worldPos.xy, uT.cameraPosMC2.xy);
        float fog = clamp(1.0 - exp(-dist * 0.00006 * exp(-max(v_worldPos.z, 0.0) * 0.002)), 0.0, 0.70);
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
            smoothstep(uT.halfMap - 256.0, uT.halfMap - 32.0, max(abs(v_worldPos.x), abs(v_worldPos.y))));
    return color;
}

// Tile-level road art early out: true when this fragment is permanent road art (color set).
bool shadeRoadArtTile(out vec3 outColor) {
    vec2 roadDx = dFdx(v_worldPos.xy), roadDy = dFdy(v_worldPos.xy);
    int plane = uT.mapSide * uT.mapSide;
    if (uT.useCement != 0 && uC.cementWordsF.length() >= 2 * plane) {
        ivec2 tile = clamp(ivec2(floor(vec2(v_worldPos.x + uT.halfMap, uT.halfMap - v_worldPos.y) / 128.0)),
                           ivec2(0), ivec2(uT.mapSide - 1));
        int index = tile.x + tile.y * uT.mapSide;
        uint road = uC.cementWordsF[plane + index];
        if ((road & 0x80000000u) != 0u) {
            int grid = max(uT.cementGridSide, 1);
            int layer = int(road & 0xffffu);
            // Match the original quad UV endpoints, including the half-texel inset.
            vec2 uv = mix(vec2(0.0078125), vec2(0.9921875),
                fract(vec2(v_worldPos.x + uT.halfMap, uT.halfMap - v_worldPos.y) / 128.0));
            vec4 art = textureLod(u_cementAtlas, (vec2(layer % grid, layer / grid) + uv) / float(grid), 0.0);
            if (art.a >= 0.5) {
                outColor = shadeRoadArt(art, road, uC.cementWordsF[index], roadDx, roadDy);
                return true;
            }
        }
    }
    return false;
}
