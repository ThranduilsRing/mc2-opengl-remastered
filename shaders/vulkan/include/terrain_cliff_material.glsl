#ifndef TS_colormapReady
#define TS_colormapReady uT.colormapReady
#endif
#ifndef TS_detailReady
#define TS_detailReady uT.detailReady
#endif
// Cliff material (triplanar POM + CC0 rock layer) split out of terrain_tier1.frag to keep it under the shader line ratchet.
// Included mid-file: depends on mv_sampleAntiTileArr, the set-0 resources and uT from terrain_frame_vk.glsl.
// Cliff faces: triplanar parallax-occlusion march of ONE world plane (GL terrain_lod_chunk.frag cliffParallaxPlane). viewPlane is the view
// vector projected on the plane's 2D basis; depth is a world-unit height relative to the cliff tiling. The layer-5 alpha is the cliff
// displacement (1 = peak). textureLod only: the march sits in a non-uniform branch.
const float MATV_CLIFF_TS          = 256.0;   // world units per cliff repeat
const float MATV_CLIFF_POM_DEPTH   = 12.0;
const float MATV_CLIFF_POM_STEPS   = 24.0;
const float MATV_CLIFF_HEIGHT_NORMAL_STRENGTH = 2.0;
float mv_cliffHeight(vec2 uv, float lod) {
    return textureLod(u_matNormalArray, vec3(uv, float(MAT_LAYER_MARBLE_CLIFF)), lod).a;
}
vec2 mv_cliffParallaxPlane(vec2 uv, vec2 viewPlane, float depth, float steps, float lod) {
    if ((kPlanarReflection != 0 && kReflectCutPom != 0) || depth <= 0.0) return uv;
    float nSteps = clamp(steps, 8.0, 32.0);
    float layerDepth = 1.0 / nSteps;
    vec2 P = -viewPlane * depth;   // negative: the parallax shifts toward the eye
    vec2 deltaUV = P * layerDepth;
    float curDepth = 0.0;
    vec2  curUV    = uv;
    float curH = 1.0 - mv_cliffHeight(curUV, lod);
    float prevH = curH;
    for (int i = 0; i < 32; ++i) {
        if (float(i) >= nSteps || curDepth >= curH) break;
        prevH = curH;
        curUV   += deltaUV;
        curDepth += layerDepth;
        curH = 1.0 - mv_cliffHeight(curUV, lod);
    }
    vec2  prevUV = curUV - deltaUV;
    float afterH = curH - curDepth;
    float beforeH = prevH - (curDepth - layerDepth);
    float t = afterH / (afterH - beforeH + 1e-5);
    return mix(curUV, prevUV, clamp(t, 0.0, 1.0));
}

// Steep cells: the rock normal is projected from the WORLD axes (triplanar) instead of the top-down per-tile UV, which removes the vertical
// stretch on a cliff face. The shading normal is mixed strongly toward the height gradient of the cliff displacement (its RGB normal is too
// smooth to catch light), and the albedo is darkened by the displacement (groove relief). `N` is the detail-perturbed normal on entry and
// the cliff-blended one on return; `baseColor` takes the groove darkening. macroN is the un-perturbed macro slope normal. The cliff albedo
// re-projection of GL is NOT here: the island carries no material albedo array.
void mv_cliffMaterial(vec3 macroN, vec3 planeLod, float blocked, inout vec3 N, inout vec3 baseColor) {
    float cb = cliffRockWeight(abs(macroN.z)) * blocked;
    if (cb <= 0.01) return;
    const float ts = MATV_CLIFF_TS;
    vec3 wp = v_worldPos;
    vec3 wn = abs(macroN); wn /= (wn.x + wn.y + wn.z + 1e-5);
    vec2 uvX = wp.yz / ts;   // YZ plane
    vec2 uvY = wp.xz / ts;   // XZ plane
    vec2 uvZ = wp.xy / ts;   // XY plane

    vec3  camW = mc2ToRender(uT.cameraPosMC2.xyz);
    vec3  Vw   = normalize(camW - wp);   // fragment -> eye, MC2 world
    float relief = 1.0 - smoothstep(0.0, 4.0, dot(planeLod, wn));
    float depth = MATV_CLIFF_POM_DEPTH / ts * relief;
    float steps = MATV_CLIFF_POM_STEPS;
    vec2 vpX = Vw.yz / max(abs(Vw.x), 0.30);
    vec2 vpY = Vw.xz / max(abs(Vw.y), 0.30);
    vec2 vpZ = Vw.xy / max(abs(Vw.z), 0.30);
    uvX = mix(uvX, mv_cliffParallaxPlane(uvX, vpX, depth, steps, planeLod.x), clamp(wn.x * 1.5, 0.0, 1.0));
    uvY = mix(uvY, mv_cliffParallaxPlane(uvY, vpY, depth, steps, planeLod.y), clamp(wn.y * 1.5, 0.0, 1.0));
    uvZ = mix(uvZ, mv_cliffParallaxPlane(uvZ, vpZ, depth, steps, planeLod.z), clamp(wn.z * 1.5, 0.0, 1.0));

    float hX0 = mv_cliffHeight(uvX, planeLod.x), hY0 = mv_cliffHeight(uvY, planeLod.y), hZ0 = mv_cliffHeight(uvZ, planeLod.z);
    if (kPlanarReflection == 0 || kReflectCutDetail == 0) {
    vec3 nX = textureLod(u_matNormalArray, vec3(uvX, float(MAT_LAYER_MARBLE_CLIFF)), planeLod.x).rgb * 2.0 - 1.0;
    vec3 nY = textureLod(u_matNormalArray, vec3(uvY, float(MAT_LAYER_MARBLE_CLIFF)), planeLod.y).rgb * 2.0 - 1.0;
    vec3 nZ = textureLod(u_matNormalArray, vec3(uvZ, float(MAT_LAYER_MARBLE_CLIFF)), planeLod.z).rgb * 2.0 - 1.0;
    vec3 triTilt = vec3(0.0, nX.x, nX.y) * wn.x
                 + vec3(nY.x, 0.0, nY.y) * wn.y
                 + vec3(nZ.x, nZ.y, 0.0) * wn.z;

    // Difference one texel at each plane's filtered mip, including non-2048 maps.
    vec3 hEps = exp2(planeLod) / float(textureSize(u_matNormalArray, 0).x);
    float hXu = mv_cliffHeight(uvX + vec2(hEps.x, 0.0), planeLod.x), hXv = mv_cliffHeight(uvX + vec2(0.0, hEps.x), planeLod.x);
    float hYu = mv_cliffHeight(uvY + vec2(hEps.y, 0.0), planeLod.y), hYv = mv_cliffHeight(uvY + vec2(0.0, hEps.y), planeLod.y);
    float hZu = mv_cliffHeight(uvZ + vec2(hEps.z, 0.0), planeLod.z), hZv = mv_cliffHeight(uvZ + vec2(0.0, hEps.z), planeLod.z);
    vec3 g = MATV_CLIFF_HEIGHT_NORMAL_STRENGTH / hEps;
    vec3 hnX = normalize(vec3(-(hXu - hX0) * g.x, -(hXv - hX0) * g.x, 1.0));
    vec3 hnY = normalize(vec3(-(hYu - hY0) * g.y, -(hYv - hY0) * g.y, 1.0));
    vec3 hnZ = normalize(vec3(-(hZu - hZ0) * g.z, -(hZv - hZ0) * g.z, 1.0));
    vec3 htTilt = vec3(0.0, hnX.x, hnX.y) * wn.x
                + vec3(hnY.x, 0.0, hnY.y) * wn.y
                + vec3(hnZ.x, hnZ.y, 0.0) * wn.z;
    float hMix = clamp(MATV_CLIFF_HEIGHT_NORMAL_STRENGTH * 0.5, 0.0, 1.0);
    triTilt = mix(triTilt, htTilt, hMix);

    vec3 triN = normalize(macroN + triTilt * relief);
    N = normalize(mix(N, triN, cb));
    }

    float disp = hX0 * wn.x + hY0 * wn.y + hZ0 * wn.z;
    baseColor *= mix(1.0, 0.62 + 0.38 * disp, cb);
}

// TERRAIN-CLIFF-ROCK: CC0 photoscan rock (albedo ratio + height, normal + roughness + AO; u_concreteDetail layers 2/3, cooked by
// tools/mc2texcook/cook_cliff_rock.py) on steep macro faces. The colormap smears vertically there, so the face colour becomes a
// palette tone (colormap taps around the face, pulled toward the mission's rock tint) times the scan's albedo ratio: MC2's colours,
// real rock structure. Height-blended into the slope transition. `wpDx`/`wpDy` = screen derivatives of the world position (uniform flow).
const float CR_TS = 320.0;   // world units per rock repeat
// Terrain-only MOVE rule (EditorData_save.cpp); object footprints never select rock.
float crBlockedTile(ivec2 p) {
    p = clamp(p, ivec2(0), ivec2(uT.mapSide - 2));
    int i = p.x + p.y * uT.mapSide;
    float a = uH.heights[i], b = uH.heights[i + uT.mapSide];
    float c = uH.heights[i + uT.mapSide + 1], d = uH.heights[i + 1];
    float rise = ((p.x + p.y) & 1) == 0 ? max(abs(a-c), max(abs(a-b), abs(a-d)))
                                                   : max(max(abs(a-b), abs(a-d)), max(abs(b-c), abs(b-d)));
    return rise > 90.509668 ? 1.0 : 0.0; // 128 * sin(default CliffTerrainAngle = 45 deg)
}
float crTerrainCliff(vec2 xy) {
    vec2 q = vec2(xy.x + uT.halfMap, uT.halfMap - xy.y) / 128.0 - 0.5;
    ivec2 p = ivec2(floor(q));
    vec2 f = smoothstep(vec2(0.0), vec2(1.0), fract(q));
    return mix(mix(crBlockedTile(p), crBlockedTile(p + ivec2(1,0)), f.x),
               mix(crBlockedTile(p + ivec2(0,1)), crBlockedTile(p + ivec2(1,1)), f.x), f.y);
}
const float CN_TS = 320.0;   // world units per ground-rock repeat (same density as the cliff scan)
// Separate ground-rock asset (mat0).
// Low-contrast height colour preserves the authored palette rather than stamping the cliff scan.
void crNavigable(vec3 macroN, vec3 dx, vec3 dy, float weight, inout vec3 N, inout vec3 color) {
    if (weight <= 0.01) return;
    // Triplanar (steep ground rock no longer smears down the slope), hex-tiled, at the cliff scan's texel density.
    vec3 wp = v_worldPos;
    vec3 w = abs(macroN); w *= w; w *= w; w /= (w.x + w.y + w.z + 1e-5);
    float h = 0.0, used = 0.0; vec3 tilt = vec3(0.0);
    for (int i = 0; i < 3; ++i) {
        if (w[i] < 0.05) continue;
        vec2 uv = (i == 0 ? vec2(wp.y, -wp.z) : i == 1 ? vec2(wp.x, -wp.z) : wp.xy) / CN_TS;
        vec2 du = (i == 0 ? dx.yz : i == 1 ? dx.xz : dx.xy) / CN_TS, dv = (i == 0 ? dy.yz : i == 1 ? dy.xz : dy.xy) / CN_TS;
        vec4 a = hexTileGrad(u_matNormalArray, float(MAT_LAYER_ROCK), uv, du, dv, 1.0, true, vec4(0.5));
        vec2 t = a.rg * 2.0 - 1.0;
        tilt += (i == 0 ? vec3(0.0, t.x, t.y) : i == 1 ? vec3(t.x, 0.0, t.y) : vec3(t.x, t.y, 0.0)) * w[i];
        h += a.a * w[i]; used += w[i];
    }
    h /= max(used, 1e-4); tilt /= max(used, 1e-4);
    color *= mix(1.0, (0.78 + 0.44 * h) * (0.9 + 0.2 * wpn_value(wp.xy / 900.0)), weight);   // macro variation + cliff-like contrast
    N = normalize(mix(N, normalize(macroN + tilt * 0.4), weight * 0.8));
}
void crApply(vec3 macroN, vec3 wpDx, vec3 wpDy, vec3 planeLod, float blocked, inout vec3 N, inout vec3 baseColor) {
    float cb = cliffRockWeight(abs(macroN.z));
    if (cb * blocked <= 0.01) return;
    vec3 wp = v_worldPos;
    vec3 w = abs(macroN); w *= w; w *= w; w /= (w.x + w.y + w.z + 1e-5);
    vec3 ah = vec3(0.0); float h = 0.0; vec3 tilt = vec3(0.0); float ao = 0.0;
    for (int i = 0; i < 3; ++i) {
        float wi = w[i];
        if (wi < 0.05) continue;
        // X plane: (y, -z); Y plane: (x, -z); Z plane: (x, -y). Image rows run top-down, so v = -up keeps the scan upright.
        vec2 uv  = (i == 0 ? vec2(wp.y, -wp.z) : i == 1 ? vec2(wp.x, -wp.z) : vec2(wp.x, -wp.y)) / CR_TS;
        vec2 du  = (i == 0 ? wpDx.yz : i == 1 ? wpDx.xz : wpDx.xy) / CR_TS;
        vec2 dv  = (i == 0 ? wpDy.yz : i == 1 ? wpDy.xz : wpDy.xy) / CR_TS;
        vec4 a = hexTileGrad(u_concreteDetail, 2.0, uv, du, dv, HEX_ROT_CLIFF, false, vec4(0.5));
        vec4 b = hexTileGrad(u_concreteDetail, 3.0, uv, du, dv, HEX_ROT_CLIFF, true, vec4(0.5, 0.5, 0.5, 0.85));
        vec2 t = vec2(b.x * 2.0 - 1.0, b.y * 2.0 - 1.0);   // GL normal: +y = image up = world up (or north on the Z plane)
        tilt += (i == 0 ? vec3(0.0, t.x, t.y) : i == 1 ? vec3(t.x, 0.0, t.y) : vec3(t.x, t.y, 0.0)) * wi;
        ah += a.rgb * wi; h += a.a * wi; ao += b.w * wi;
    }
    float wsum = w.x * step(0.05, w.x) + w.y * step(0.05, w.y) + w.z * step(0.05, w.z);
    ah /= max(wsum, 1e-4); h /= max(wsum, 1e-4); ao /= max(wsum, 1e-4); tilt /= max(wsum, 1e-4);
    float m = clamp((cb - 0.5 + (h - 0.5) * 0.6) * 3.0 + 0.5, 0.0, 1.0) * blocked;
    // Full rock coverage replaces both outputs of the older three-plane POM.
    // Its palette also replaces baseColor when a colormap is bound. Preserve
    // the older material through every transition and the missing-colormap path.
    if ((m < 1.0 || TS_colormapReady == 0) && TS_detailReady >= 2)
        mv_cliffMaterial(macroN, planeLod, blocked, N, baseColor);
    if (m <= 0.0) return;
    // Palette tone: four colormap taps ~160 wu around the face average out the vertical smear.
    vec3 tone = baseColor;
    if (TS_colormapReady != 0) {
        vec2 c = vec2((wp.x - uT.atlasTopLeftX) * uT.atlasOneOverWorldUnits, (uT.atlasTopLeftY - wp.y) * uT.atlasOneOverWorldUnits);
        float o = 160.0 * uT.atlasOneOverWorldUnits;
        tone = 0.25 * (textureLod(u_colormap, c + vec2(o, o), 0.0).rgb + textureLod(u_colormap, c + vec2(-o, o), 0.0).rgb
                     + textureLod(u_colormap, c + vec2(o, -o), 0.0).rgb + textureLod(u_colormap, c + vec2(-o, -o), 0.0).rgb);
        tone *= cliffDelightGain(dot(tone, vec3(0.299, 0.587, 0.114)), cliffReferenceLuma(wp.xy));
    }
    float tl = dot(tone, vec3(0.299, 0.587, 0.114));
    tone = mix(mix(vec3(tl), tone, 0.6), vec3(uT.tintRockR, uT.tintRockG, uT.tintRockB) * (tl / max(dot(vec3(uT.tintRockR, uT.tintRockG, uT.tintRockB), vec3(0.299, 0.587, 0.114)), 0.05)), 0.3) * 0.85;
    vec3 rock = tone * (ah * 2.0) * mix(1.0, ao, 0.8);
    baseColor = mix(baseColor, rock, m);
    N = normalize(mix(N, normalize(macroN + tilt), m));
}
