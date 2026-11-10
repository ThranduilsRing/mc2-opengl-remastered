// Debug modes 30..39 show whether |pomOff| exceeds 4^(mode-30) world
// units (red) or 0.25 * 4^(mode-30) pixels (green); blue marks POM.
// Thresholds survive presentation/tonemapping; mode 0 removes these taps.
vec2 mv_pomOffsetMeasure = vec2(0.0);
vec3 mv_detailNormal(vec4 w, float snowWeight, vec2 worldXY, float macroNz, vec2 uvDx, vec2 uvDy) {
    vec2 uv = worldXY * (uT.detailTiling / MATV_WU_PER_TILE);
    vec2 uvRock     = uv * (MATV_TILING.x / 3.0);
    vec2 uvGrass    = uv * MATV_TILING.y;
    vec2 uvDirt     = uv * MATV_TILING.z;
    vec2 uvConcrete = uv * MATV_TILING.w;
    vec2 uvSnow     = uv * MATV_TILING_SNOW;
    // Screen-space derivative AA (GL fwRock/fwGrass/...): fade a layer to 0 as its
    // tiling goes sub-pixel. main() supplies the continuous UV footprint.
    vec2 uvWidth = abs(uvDx) + abs(uvDy);
    float fwRock     = clamp(1.0 - (length(uvWidth * (MATV_TILING.x / 3.0)) - 0.5) * 2.0, 0.0, 1.0);
    float fwGrass    = clamp(1.0 - (length(uvWidth * MATV_TILING.y) - 0.5) * 2.0, 0.0, 1.0);
    float fwDirt     = clamp(1.0 - (length(uvWidth * MATV_TILING.z) - 0.5) * 2.0, 0.0, 1.0);
    float fwConcrete = clamp(1.0 - (length(uvWidth * MATV_TILING.w) - 0.5) * 2.0, 0.0, 1.0);
    float fwSnow     = clamp(1.0 - (length(uvWidth * MATV_TILING_SNOW) - 0.5) * 2.0, 0.0, 1.0);

    // POM follows the continuous material footprint, not eye altitude. Fade to
    // zero before skipping the march; zoom must not introduce a contour band.
    if (fwRock > 0.4) {
        float pomScale = MATV_POM_SCALE * fwRock;
        vec2  pomUV    = uv * dot(MATV_TILING, w);
        vec2  pomOff   = vec2(0.0);
        vec3  camW     = mc2ToRender(uT.cameraPosMC2.xyz);   // the eye in the MC2 world frame (the feed is the Stuff frame)
        float footprintFade = smoothstep(0.4, 0.8, fwRock);
        float slopeFade = smoothstep(0.55, 0.85, abs(macroNz));
        // Scatter roots use the terrain mesh. Keep grass, dirt and snow on that
        // surface rather than sliding their relief underneath the plants.
        float scatterSurface = smoothstep(0.01, 0.05, w.y + w.z + snowWeight);
        float pomStrength = footprintFade * slopeFade * w.x * (1.0 - scatterSurface);
        if (pomStrength > 0.001) {
            vec3 Vw = normalize(camW - v_worldPos);                 // fragment -> eye, MC2 world
            // The detail UVs are top-down planar with no y flip: tangent u = east, bitangent v = north, normal = up.
            vec3 viewDirTS = vec3(Vw.x, Vw.z, Vw.y);
            pomOff = mv_parallax(pomUV, pomScale, w, viewDirTS,
                                 uvDx * (MATV_TILING.x / 3.0), uvDy * (MATV_TILING.x / 3.0)) * fwRock * pomStrength;
        }
        if (kCsmDebugMode >= 30 && kCsmDebugMode <= 39) {
            float rockScale = MATV_TILING.x / 3.0;
            vec2 dx = uvDx * rockScale, dy = uvDy * rockScale;
            float det = dx.x * dy.y - dx.y * dy.x;
            vec2 pixels = abs(det) > 1e-12 ? vec2(dy.y * pomOff.x - dy.x * pomOff.y,
                                                                dx.x * pomOff.y - dx.y * pomOff.x) / det : vec2(0.0);
            mv_pomOffsetMeasure = vec2(length(pomOff) * MATV_WU_PER_TILE / (uT.detailTiling * rockScale), length(pixels));
        }
        uvRock += pomOff; uvGrass += pomOff; uvDirt += pomOff; uvConcrete += pomOff;
    }

    // Anti-tile scale per material (GL :382-385: tiling>=4 full, <=1 off). Near only.
    float atsRock  = mix(0.0, 3.0, clamp((MATV_TILING.x - 1.0) / 3.0, 0.0, 1.0));
    float atsGrass = mix(0.0, 3.0, clamp((MATV_TILING.y - 1.0) / 3.0, 0.0, 1.0));
    float atsDirt  = mix(0.0, 3.0, clamp((MATV_TILING.z - 1.0) / 3.0, 0.0, 1.0));
    float atsConc  = mix(0.0, 3.0, clamp((MATV_TILING.w - 1.0) / 3.0, 0.0, 1.0));

    vec3 dN = vec3(0.0);
    // rock: boost/3 (GL parity — rock detail was too dense/strong). GL :392-397.
    if (w.x > 0.01 && fwRock != 0.0) {
        vec4 s = (atsRock > 0.01 && fwRock > 0.4)
               ? mv_sampleAntiTileArr(MAT_LAYER_ROCK, uvRock, atsRock, uvDx * (MATV_TILING.x / 3.0), uvDy * (MATV_TILING.x / 3.0), fwRock)
               : textureGrad(u_matNormalArray, vec3(uvRock, float(MAT_LAYER_ROCK)), uvDx * (MATV_TILING.x / 3.0), uvDy * (MATV_TILING.x / 3.0));
        dN += w.x * (MATV_NORMAL_BOOST.x / 3.0) * fwRock * (s.rgb * 2.0 - 1.0);
    }
    if (w.y > 0.01 && fwGrass != 0.0) {
        vec4 s = (atsGrass > 0.01 && fwGrass > 0.4)
               ? mv_sampleAntiTileArr(MAT_LAYER_GRASS, uvGrass, atsGrass, uvDx * MATV_TILING.y, uvDy * MATV_TILING.y, fwGrass)
               : textureGrad(u_matNormalArray, vec3(uvGrass, float(MAT_LAYER_GRASS)), uvDx * MATV_TILING.y, uvDy * MATV_TILING.y);
        dN += w.y * MATV_NORMAL_BOOST.y * fwGrass * (s.rgb * 2.0 - 1.0);
        // GRASS-GROUND-NORMAL: a second, larger-scale tap of the turf (0.29x, rotated ~37 deg) so the
        // tuft/dirt-gap clumps vary over ~1300 wu and the 384 wu tile does not repeat. Its own fade.
        const mat2 kGrassMacroRot = mat2(0.8, 0.6, -0.6, 0.8) * 0.29;
        vec2 gmDx = kGrassMacroRot * (uvDx * MATV_TILING.y), gmDy = kGrassMacroRot * (uvDy * MATV_TILING.y);
        float fwGrassMacro = clamp(1.0 - (length(uvWidth * MATV_TILING.y * 0.29) - 0.5) * 2.0, 0.0, 1.0);
        vec2 gm = textureGrad(u_matNormalArray, vec3(kGrassMacroRot * uvGrass, float(MAT_LAYER_GRASS)), gmDx, gmDy).rg * 2.0 - 1.0;
        // Rotate the sampled slope back into terrain space.
        dN.xy += w.y * MATV_NORMAL_BOOST.y * 0.6 * fwGrassMacro * (transpose(mat2(0.8, 0.6, -0.6, 0.8)) * gm);
    }
    if (w.z > 0.01 && fwDirt != 0.0) {
        vec4 s = (atsDirt > 0.01 && fwDirt > 0.4)
               ? mv_sampleAntiTileArr(MAT_LAYER_DIRT, uvDirt, atsDirt, uvDx * MATV_TILING.z, uvDy * MATV_TILING.z, fwDirt)
               : textureGrad(u_matNormalArray, vec3(uvDirt, float(MAT_LAYER_DIRT)), uvDx * MATV_TILING.z, uvDy * MATV_TILING.z);
        dN += w.z * MATV_NORMAL_BOOST.z * fwDirt * (s.rgb * 2.0 - 1.0);
    }
    if (w.w > 0.01 && fwConcrete != 0.0) {
        vec4 s = (atsConc > 0.01 && fwConcrete > 0.4)
               ? mv_sampleAntiTileArr(MAT_LAYER_CONCRETE, uvConcrete, atsConc, uvDx * MATV_TILING.w, uvDy * MATV_TILING.w, fwConcrete)
               : textureGrad(u_matNormalArray, vec3(uvConcrete, float(MAT_LAYER_CONCRETE)), uvDx * MATV_TILING.w, uvDy * MATV_TILING.w);
        dN += w.w * MATV_NORMAL_BOOST.w * fwConcrete * (s.rgb * 2.0 - 1.0);
    }
    if (snowWeight > 0.01 && fwSnow != 0.0) {
        dN += snowWeight * 0.9 * fwSnow *
              (textureGrad(u_matNormalArray, vec3(uvSnow, float(MAT_LAYER_SNOW)),
                           uvDx * MATV_TILING_SNOW, uvDy * MATV_TILING_SNOW).rgb * 2.0 - 1.0);
    }
    return dN;
}
