// Detail taps fade by TEXEL footprint, not by the old fwidth thresholds (those only engaged at 0.5-1 tile per pixel, never
// at a reachable view). mv_detailTexelGM = geometric-mean (minor and major axis) texels per pixel of the normal array at
// tiling 1, set in main() from the continuous detail-UV derivatives. weight = 1 - smoothstep(L0, L1, log2(texels/pixel));
// at 0 the tap is skipped. The anti-tile taps are NOT thinned with distance: they follow the unchanged fw rules.
// POM is gone: its offset was already clamped to 1/4 pixel, so it moved nothing visible for 8-16 marched layers.
#define DF_L0 2.0
#define DF_L1 4.0
float mv_detailTexelGM = 0.0;
// GROUND-AO-1: cooked source AO of the grass turf (its normal-texel B channel), weighted by the tap weights; _W = 0 where no layer carries one.
vec3 mv_cavityNormal = vec3(0.0); // Unfaded slopes, used only by the material AO proxy.
float mv_srcAoSum = 0.0, mv_srcAoW = 0.0;
float mv_forestMix = 0.0;   // 0..1 tree-canopy density at this pixel (forest-floor share of grass+dirt; set in main())
float mv_detailFade(float texelsPerPixel) {
    return 1.0 - smoothstep(DF_L0, DF_L1, log2(max(texelsPerPixel, 1e-6)));
}
vec3 mv_detailDecode(vec3 texel) {
    // Top-row-first NormalGL: green points UP the image, while uv.y/world Y
    // increases DOWN its rows. Convert before undoing any UV rotation.
    return texel * vec3(2.0, -2.0, 2.0) + vec3(-1.0, 1.0, -1.0);
}
// The moon keeps its authored albedo. A broad crater/regolith tap survives RTS zoom;
// the same texel-footprint fade and hex tiling as earth keep it filtered and untiled.
vec3 mv_authoredDetailNormal(vec2 worldXY, vec2 uvDx, vec2 uvDy) {
    float scale = MATV_WU_PER_TILE / (uT.normalFlatPeriod * 4.0);
    float fade = mv_detailFade(mv_detailTexelGM * scale);
    if (fade <= 0.0) return vec3(0.0);
    vec2 uv = worldXY * (uT.detailTiling / MATV_WU_PER_TILE) * scale;
    return 0.7 * uT.normalFlatStrength * fade * mv_detailDecode(hexTileGrad(u_matNormalArray,
        float(MAT_LAYER_ROCK), uv, uvDx * scale, uvDy * scale, 1.0, true, vec4(0.5)).rgb);
}
// Desaturated soil falls into the colour classifier's rock remainder. Give its
// flat areas turf relief/AO without changing the authored colour or steep rock.
vec4 mv_groundDetailWeights(vec4 w, float macroNz) {
    float soil = w.x * smoothstep(0.75, 0.95, macroNz);
    return w + vec4(-soil, soil, 0.0, 0.0);
}
vec3 mv_detailNormal(vec4 w, float snowWeight, vec2 worldXY, float macroNz, vec2 uvDx, vec2 uvDy) {
    // Flat rock ground (colour classes that fail the grass/dirt thresholds) also reads as forest floor under trees; slopes stay rock.
    float forestRock = smoothstep(0.75, 0.95, macroNz);
    float wForest = mv_forestMix * (w.y + w.z + w.x * forestRock);
    w.x *= 1.0 - mv_forestMix * forestRock; w.y *= 1.0 - mv_forestMix; w.z *= 1.0 - mv_forestMix;
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
    float dfRock     = mv_detailFade(mv_detailTexelGM * (MATV_TILING.x / 3.0));
    float dfGrass    = mv_detailFade(mv_detailTexelGM * MATV_TILING.y);
    float dfGrassMac = mv_detailFade(mv_detailTexelGM * MATV_TILING.y * 0.29);
    float dfDirt     = mv_detailFade(mv_detailTexelGM * MATV_TILING.z);
    float dfConcrete = mv_detailFade(mv_detailTexelGM * MATV_TILING.w);
    float dfSnow     = mv_detailFade(mv_detailTexelGM * MATV_TILING_SNOW);

    // Anti-tile scale per material (GL :382-385: tiling>=4 full, <=1 off). Near only.
    float atsRock  = mix(0.0, 3.0, clamp((MATV_TILING.x - 1.0) / 3.0, 0.0, 1.0));
    float atsGrass = mix(0.0, 3.0, clamp((MATV_TILING.y - 1.0) / 3.0, 0.0, 1.0));
    float atsDirt  = mix(0.0, 3.0, clamp((MATV_TILING.z - 1.0) / 3.0, 0.0, 1.0));
    float atsConc  = mix(0.0, 3.0, clamp((MATV_TILING.w - 1.0) / 3.0, 0.0, 1.0));

    bool needAo = uT.groundAoMaterial > 0.0;
    vec3 dN = vec3(0.0);
    // rock: boost/3 (GL parity — rock detail was too dense/strong). GL :392-397.
    if (w.x > 0.01 && (fwRock != 0.0 || needAo) && (dfRock > 0.0 || needAo)) {
        vec4 s = (atsRock > 0.01 && fwRock > 0.4)
               ? mv_sampleAntiTileArr(MAT_LAYER_ROCK, uvRock, atsRock, uvDx * (MATV_TILING.x / 3.0), uvDy * (MATV_TILING.x / 3.0), fwRock)
               : textureGrad(u_matNormalArray, vec3(uvRock, float(MAT_LAYER_ROCK)), uvDx * (MATV_TILING.x / 3.0), uvDy * (MATV_TILING.x / 3.0));
        dN += w.x * (MATV_NORMAL_BOOST.x / 3.0) * fwRock * dfRock * mv_detailDecode(s.rgb);
        mv_cavityNormal += w.x * (MATV_NORMAL_BOOST.x / 3.0) * mv_detailDecode(s.rgb);
    }
    // Forest floor (under trees): mv_forestMix is the tree-canopy density mask; it takes that share of the grass, dirt and flat-rock weights
    // (same tiling and boost bound as the turf it replaces).
    if (wForest > 0.01 && (fwGrass != 0.0 || needAo) && (dfGrass > 0.0 || needAo)) {
        vec4 sf = (atsGrass > 0.01 && fwGrass > 0.4)
                ? mv_sampleAntiTileArr(MAT_LAYER_FOREST, uvGrass * 0.8, atsGrass, uvDx * MATV_TILING.y * 0.8, uvDy * MATV_TILING.y * 0.8, fwGrass)
                : textureGrad(u_matNormalArray, vec3(uvGrass * 0.8, float(MAT_LAYER_FOREST)), uvDx * MATV_TILING.y * 0.8, uvDy * MATV_TILING.y * 0.8);
        dN += wForest * MATV_NORMAL_BOOST.y * 1.2 * fwGrass * dfGrass * mv_detailDecode(sf.rgb);
        mv_cavityNormal += wForest * MATV_NORMAL_BOOST.y * 1.2 * mv_detailDecode(sf.rgb);
    }
    if (w.y > 0.01 && (fwGrass != 0.0 || needAo)) {
        if (dfGrass > 0.0 || needAo) {
            vec4 s = (atsGrass > 0.01 && fwGrass > 0.4)
                   ? mv_sampleAntiTileArr(MAT_LAYER_GRASS, uvGrass, atsGrass, uvDx * MATV_TILING.y, uvDy * MATV_TILING.y, fwGrass)
                   : textureGrad(u_matNormalArray, vec3(uvGrass, float(MAT_LAYER_GRASS)), uvDx * MATV_TILING.y, uvDy * MATV_TILING.y);
            dN += w.y * MATV_NORMAL_BOOST.y * fwGrass * dfGrass * mv_detailDecode(s.rgb);
            mv_cavityNormal += w.y * MATV_NORMAL_BOOST.y * mv_detailDecode(s.rgb);
            mv_srcAoSum += w.y * s.b; mv_srcAoW += w.y;
        }
        // GRASS-GROUND-NORMAL: a second, larger-scale tap of the turf (0.29x, rotated ~37 deg) so the
        // tuft/dirt-gap clumps vary over ~1300 wu and the 384 wu tile does not repeat. Its own fade.
        if (dfGrassMac > 0.0 || needAo) {
            const mat2 kGrassMacroRot = mat2(0.8, 0.6, -0.6, 0.8) * 0.29;
            vec2 gmDx = kGrassMacroRot * (uvDx * MATV_TILING.y), gmDy = kGrassMacroRot * (uvDy * MATV_TILING.y);
            float fwGrassMacro = clamp(1.0 - (length(uvWidth * MATV_TILING.y * 0.29) - 0.5) * 2.0, 0.0, 1.0);
            // The restored flat-soil relief must untile its macro and fine taps too.
            vec2 gm = mv_detailDecode(hexTileGrad(u_matNormalArray, float(MAT_LAYER_GRASS), kGrassMacroRot * uvGrass, gmDx, gmDy, 1.0, true, vec4(0.5)).rgb).xy;
            // Rotate the sampled slope back into terrain space.
            dN.xy += w.y * MATV_NORMAL_BOOST.y * 0.6 * fwGrassMacro * dfGrassMac * (transpose(mat2(0.8, 0.6, -0.6, 0.8)) * gm);
            mv_cavityNormal.xy += w.y * MATV_NORMAL_BOOST.y * 0.6 * (transpose(mat2(0.8, 0.6, -0.6, 0.8)) * gm);
        }
    }
    // GROUND-DETAIL-FINE-1 (owner 11-23, base terrain too coarse): one 4.3x finer, rotated tap of the turf and of the dirt, so
    // the mid-zoom ground carries ~90-180 wu detail on top of the 384/768 wu tiles. Each has its own texel-footprint fade
    // (dfFine) and fwidth fade (fwFine), so it vanishes as it goes sub-pixel instead of speckling at distance.
    {
        const float kFine = 4.3;
        const mat2 kFineUnit = mat2(0.6, 0.8, -0.8, 0.6);
        const mat2 kFineRot = kFineUnit * kFine;
        const mat2 kFineBack = transpose(kFineUnit);
        float dfGrassFine = mv_detailFade(mv_detailTexelGM * MATV_TILING.y * kFine);
        float dfDirtFine  = mv_detailFade(mv_detailTexelGM * MATV_TILING.z * kFine);
        if (w.y > 0.01 && (dfGrassFine > 0.0 || needAo)) {
            float fwFine = clamp(1.0 - (length(uvWidth * MATV_TILING.y * kFine) - 0.5) * 2.0, 0.0, 1.0);
            vec2 g = mv_detailDecode(hexTileGrad(u_matNormalArray, float(MAT_LAYER_GRASS), kFineRot * uvGrass,
                                 kFineRot * (uvDx * MATV_TILING.y), kFineRot * (uvDy * MATV_TILING.y), 1.0, true, vec4(0.5)).rgb).xy;
            dN.xy += w.y * MATV_NORMAL_BOOST.y * 0.8 * fwFine * dfGrassFine * (kFineBack * g);
            mv_cavityNormal.xy += w.y * MATV_NORMAL_BOOST.y * 0.8 * (kFineBack * g);
        }
        if (w.z > 0.01 && (dfDirtFine > 0.0 || needAo)) {
            float fwFine = clamp(1.0 - (length(uvWidth * MATV_TILING.z * kFine) - 0.5) * 2.0, 0.0, 1.0);
            vec2 g = mv_detailDecode(hexTileGrad(u_matNormalArray, float(MAT_LAYER_DIRT), kFineRot * uvDirt,
                                 kFineRot * (uvDx * MATV_TILING.z), kFineRot * (uvDy * MATV_TILING.z), 1.0, true, vec4(0.5)).rgb).xy;
            dN.xy += w.z * MATV_NORMAL_BOOST.z * 0.8 * fwFine * dfDirtFine * (kFineBack * g);
            mv_cavityNormal.xy += w.z * MATV_NORMAL_BOOST.z * 0.8 * (kFineBack * g);
        }
    }
    // TERRAIN-NORMAL-UNIFORM: a large-scale rotated tap of the sand (0.31x, ~53 deg) so its own lattice never repeats at mid zoom,
    // the way the turf has its macro tap. Own texel-footprint and fwidth fades.
    float dfDirtMac = mv_detailFade(mv_detailTexelGM * MATV_TILING.z * 0.31);
    if (w.z > 0.01 && (dfDirtMac > 0.0 || needAo)) {
        const mat2 kDirtMacroUnit = mat2(0.6, 0.8, -0.8, 0.6);
        const mat2 kDirtMacroRot = kDirtMacroUnit * 0.31;
        float fwDirtMacro = clamp(1.0 - (length(uvWidth * MATV_TILING.z * 0.31) - 0.5) * 2.0, 0.0, 1.0);
        vec2 dm = mv_detailDecode(hexTileGrad(u_matNormalArray, float(MAT_LAYER_DIRT), kDirtMacroRot * uvDirt,
                                  kDirtMacroRot * (uvDx * MATV_TILING.z), kDirtMacroRot * (uvDy * MATV_TILING.z), 1.0, true, vec4(0.5)).rgb).xy;
        dN.xy += w.z * MATV_NORMAL_BOOST.z * 0.6 * fwDirtMacro * dfDirtMac * (transpose(kDirtMacroUnit) * dm);
        mv_cavityNormal.xy += w.z * MATV_NORMAL_BOOST.z * 0.6 * (transpose(kDirtMacroUnit) * dm);
    }
    if (w.z > 0.01 && (fwDirt != 0.0 || needAo) && (dfDirt > 0.0 || needAo)) {
        vec4 s = (atsDirt > 0.01 && fwDirt > 0.4)
               ? mv_sampleAntiTileArr(MAT_LAYER_DIRT, uvDirt, atsDirt, uvDx * MATV_TILING.z, uvDy * MATV_TILING.z, fwDirt)
               : textureGrad(u_matNormalArray, vec3(uvDirt, float(MAT_LAYER_DIRT)), uvDx * MATV_TILING.z, uvDy * MATV_TILING.z);
        dN += w.z * MATV_NORMAL_BOOST.z * fwDirt * dfDirt * mv_detailDecode(s.rgb);
        mv_cavityNormal += w.z * MATV_NORMAL_BOOST.z * mv_detailDecode(s.rgb);
    }
    if (w.w > 0.01 && (fwConcrete != 0.0 || needAo) && (dfConcrete > 0.0 || needAo)) {
        vec4 s = (atsConc > 0.01 && fwConcrete > 0.4)
               ? mv_sampleAntiTileArr(MAT_LAYER_CONCRETE, uvConcrete, atsConc, uvDx * MATV_TILING.w, uvDy * MATV_TILING.w, fwConcrete)
               : textureGrad(u_matNormalArray, vec3(uvConcrete, float(MAT_LAYER_CONCRETE)), uvDx * MATV_TILING.w, uvDy * MATV_TILING.w);
        dN += w.w * MATV_NORMAL_BOOST.w * fwConcrete * dfConcrete * mv_detailDecode(s.rgb);
        mv_cavityNormal += w.w * MATV_NORMAL_BOOST.w * mv_detailDecode(s.rgb);
    }
    if (snowWeight > 0.01 && (fwSnow != 0.0 || needAo) && (dfSnow > 0.0 || needAo)) {
        dN += snowWeight * 0.9 * uT.normalFlatStrength * fwSnow * dfSnow *
              mv_detailDecode(textureGrad(u_matNormalArray, vec3(uvSnow, float(MAT_LAYER_SNOW)),
                           uvDx * MATV_TILING_SNOW, uvDy * MATV_TILING_SNOW).rgb);
        mv_cavityNormal += snowWeight * 0.9 * uT.normalFlatStrength *
              mv_detailDecode(textureGrad(u_matNormalArray, vec3(uvSnow, float(MAT_LAYER_SNOW)),
                           uvDx * MATV_TILING_SNOW, uvDy * MATV_TILING_SNOW).rgb);
    }
    return dN;
}
