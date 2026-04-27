//#version 400 (version provided by material prefix)

#define PREC highp

#include <include/shadow.hglsl>
#include <include/noise.hglsl>
#include <include/render_contract.hglsl>

// [RENDER_CONTRACT]
//   Pass:           TerrainBase
//   Color0:         RGBA color, opaque (water/shoreline blends)
//   GBuffer1:       rc_gbuffer1_shadowHandled / rc_gbuffer1_shadowHandled_flatUp
//                   for opaque terrain; rc_gbuffer1_legacyTerrainMaterialAlpha
//                   for water/shoreline (CONTRACT VIOLATION §3.1, F1)
//   ShadowContract: castsStatic=true, castsDynamic=false,
//                   skipsPostScreenShadow=true (binary-true for opaque;
//                   ambiguous for water — see §3.1)
//   StateContract:  depthTest=true, depthWrite=true, blend=Opaque,
//                   requiresMRT=true

in PREC vec4 Color;
in PREC vec2 Texcoord;
in PREC float FogValue;
in PREC float TerrainType;
in PREC vec3 WorldNorm;
in PREC vec3 WorldPos;
in PREC float UndisplacedDepth;

layout (location=0) out PREC vec4 FragColor;
#ifdef MRT_ENABLED
layout (location=1) out PREC vec4 GBuffer1;
#endif

uniform sampler2D tex1;  // colormap
uniform sampler2D tex2;  // detail normal (engine default, fallback)
uniform sampler2D tex3;  // detail displacement (legacy, unused with per-material POM)

// Per-material normal maps (individual sampler2D on units 5-8)
// Alpha channel = displacement map for per-material POM
uniform sampler2D matNormal0;  // rock
uniform sampler2D matNormal1;  // grass
uniform sampler2D matNormal2;  // dirt
uniform sampler2D matNormal3;  // concrete
uniform sampler2D matNormal4;  // snow (optional — 5th slot)

uniform PREC vec4 terrainLightDir;
uniform PREC vec4 detailNormalTiling;
uniform PREC vec4 detailNormalStrength;
uniform PREC vec4 fog_color;
uniform PREC vec4 pomParams;
uniform PREC vec4 cameraPos;
uniform PREC vec4 terrainWorldScale;
uniform PREC vec4 terrainViewDir;
uniform PREC vec4 tessDebug;  // x=mode: 0=off, 1=normals, 2=worldPos
uniform float time;           // elapsed seconds (for cloud shadow animation)
uniform PREC float mapHalfExtent;  // half side length of playable map (0 = disabled)

// --- Distance LOD thresholds (tunable, in MC2 world units) ---
// 1 terrain tile ≈ 128 world units
const float LOD_NEAR       = 4000.0;   // full quality (covers stock zoom)
const float LOD_NEAR_FADE  = 4500.0;   // transition band end
const float LOD_MID        = 5500.0;   // mid quality
const float LOD_MID_FADE   = 6500.0;   // far quality begins
const PREC vec3 kLumaWeights = vec3(0.299, 0.587, 0.114);

// --- Hash / noise for cell bombing ---

PREC vec2 hash22(PREC vec2 p) {
    PREC vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.xx + p3.yz) * p3.zy);
}

// Anti-tiling: blend 3 offset samples to break repetition without seams
PREC vec4 sampleAntiTile(sampler2D tex, PREC vec2 uv, PREC float scale) {
    PREC vec2 off1 = hash22(floor(uv / scale)) * scale;
    PREC vec2 off2 = hash22(floor(uv / scale) + vec2(7.0, 13.0)) * scale;

    PREC vec4 s0 = texture(tex, uv);
    PREC vec4 s1 = texture(tex, uv + off1);
    PREC vec4 s2 = texture(tex, uv + off2);

    // Blend weights from position within cell — always normalized to sum to 1
    PREC vec2 f = fract(uv / scale);
    PREC float w0 = 1.0;
    PREC float w1 = smoothstep(0.2, 0.5, f.x) * smoothstep(0.2, 0.5, f.y);
    PREC float w2 = smoothstep(0.2, 0.5, 1.0 - f.x) * smoothstep(0.2, 0.5, 1.0 - f.y);
    PREC float wTotal = w0 + w1 + w2;

    return (s0 * w0 + s1 * w1 + s2 * w2) / wTotal;
}

// --- Color classification helpers ---

PREC vec3 rgb2hsv(PREC vec3 c) {
    PREC vec4 K = vec4(0.0, -1.0/3.0, 2.0/3.0, -1.0);
    PREC vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    PREC vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
    PREC float d = q.x - min(q.w, q.y);
    PREC float e = 1.0e-10;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

// MC2 terrain palette — tuned from actual screenshot analysis
PREC vec4 getColorWeights(PREC vec3 color) {
    PREC vec3 hsv = rgb2hsv(color);
    PREC float h = hsv.x;
    PREC float s = hsv.y;
    PREC float v = hsv.z;

    PREC vec4 w = vec4(0.0);

    // Green → grass, brown → dirt, everything else → rock.
    // Concrete weight comes only from TerrainType (cement vertices) later in main();
    // never from colormap, so snow/overlay-whitened tiles fall through to rock.
    w.y = smoothstep(0.10, 0.20, h) * smoothstep(0.10, 0.32, s);   // green
    w.z = smoothstep(0.17, 0.11, h) * smoothstep(0.10, 0.32, s);   // brown
    w.x = 1.0 - max(w.y, w.z);                                     // everything else → rock
    w.w = 0.0;

    PREC float isWater = smoothstep(0.35, 0.45, h);
    w.x += isWater;
    w.y *= (1.0 - isWater);
    w.z *= (1.0 - isWater);

    PREC float total = w.x + w.y + w.z + w.w;
    w = (total < 0.01) ? vec4(1.0, 0.0, 0.0, 0.0) : w / total;
    return w;
}

// --- Per-material displacement sampling ---

const PREC vec4 pomScaleMat = vec4(1.0, 1.0, 2.5, 1.0);  // rock doubled 0.5→1.0 for stronger displacement

PREC float sampleDisplacement(PREC vec2 uv, PREC vec4 weights) {
    PREC float d = 0.0;
    if (weights.x > 0.01) d += weights.x * texture(matNormal0, uv).a;
    if (weights.y > 0.01) d += weights.y * texture(matNormal1, uv).a;
    if (weights.z > 0.01) d += weights.z * 1.0;  // dirt: blank alpha (no POM shift)
    if (weights.w > 0.01) d += weights.w * texture(matNormal3, uv).a;
    return 1.0 - d;
}

// --- POM ---

PREC vec2 parallaxMapping(PREC vec2 uv, PREC vec3 viewDirTS, PREC float scale, PREC vec4 matWeights)
{
    PREC float numLayers = mix(pomParams.z, pomParams.y, max(viewDirTS.y, 0.0));
    PREC float layerDepth = 1.0 / numLayers;
    PREC float currentLayerDepth = 0.0;
    PREC vec2 P = viewDirTS.xz / max(viewDirTS.y, 0.001) * scale;
    PREC vec2 deltaUV = P / numLayers;
    PREC vec2 currentUV = uv;
    PREC float currentDepth = sampleDisplacement(currentUV, matWeights);
    for (int i = 0; i < 64; i++) {
        if (currentLayerDepth >= currentDepth) break;
        currentUV -= deltaUV;
        currentDepth = sampleDisplacement(currentUV, matWeights);
        currentLayerDepth += layerDepth;
    }
    PREC vec2 prevUV = currentUV + deltaUV;
    PREC float afterDepth = currentDepth - currentLayerDepth;
    PREC float beforeDepth = sampleDisplacement(prevUV, matWeights) - currentLayerDepth + layerDepth;
    PREC float weight = afterDepth / (afterDepth - beforeDepth);
    return mix(currentUV, prevUV, weight);
}

void main(void)
{
    // Debug visualizations for tessellation data
    // Distance-based LOD factors (1.0 = full quality, 0.0 = cheapest)
    // cameraPos is in Stuff/MLR space: .x=left/right, .y=elevation, .z=forward
    // WorldPos is in raw MC2 space: .x=east, .y=north, .z=elevation
    // LOD center: camera ground position with altitude boost for zoom-out
    vec2 camGround = vec2(-cameraPos.x, cameraPos.z);
    float hDist = distance(WorldPos.xy, camGround);
    float altBoost = max(cameraPos.y - WorldPos.z, 0.0) * 0.7;
    float camDist = hDist + altBoost;
    float lodNear = 1.0 - smoothstep(LOD_NEAR, LOD_NEAR_FADE, camDist);
    float lodMid  = 1.0 - smoothstep(LOD_MID,  LOD_MID_FADE,  camDist);

    // Screen-space world-unit footprint of one pixel — fwidth-based.
    // Per-triangle-constant (linear varying), so it creates step bands at
    // tessellation LOD boundaries. Safe for FBM/rock (low tiling, subtle fade).
    PREC float worldPixelSize = length(fwidth(WorldPos.xy));

    // FBM breakup fade: scale 0.018 ≈ 1 cycle / 56 WU.
    PREC float breakupFade = 1.0 - smoothstep(0.3, 0.8, worldPixelSize * 0.018);

    // Smooth camera-distance proxy for grass fade — avoids tessellation LOD
    // chunk boundaries that fwidth(WorldPos) would expose at 12x tiling.
    // grassPixelScale 0.0015: tune up to sharpen close grass, down for softer.
    PREC float worldPixelSizeSmooth = camDist * 0.0015;
    PREC float grassFreq = worldPixelSizeSmooth * 0.094;

    // Wider fade band (0.20..1.20 vs 0.3..0.8) so the grass normal transition
    // dissolves gradually rather than crossing the threshold in a tight visible band.
    PREC float grassNormalFade = 1.0 - smoothstep(0.20, 1.20, grassFreq);

    // Legacy probe: reserve negative values for an unconditional "tess frag is running"
    // visual. Positive values are surface debug modes and must flow through normally.
    if (tessDebug.x < -0.5) {
        gl_FragDepth = gl_FragCoord.z;
        FragColor = vec4(1.0, 0.0, 0.0, 1.0);  // SOLID RED = tess frag running
#ifdef MRT_ENABLED
        GBuffer1 = rc_gbuffer1_shadowHandled_flatUp();
#endif
        return;
    }

    PREC vec4 texColor = texture(tex1, Texcoord);
    PREC float waterFlag = smoothstep(0.35, 0.45, rgb2hsv(texColor.rgb).x);
    PREC float materialAlpha = mix(1.0, 0.25, waterFlag);

#ifdef ALPHA_TEST
    if(texColor.a < 0.5)
        discard;
#endif

    int surfaceDebugMode = int(floor(tessDebug.x + 0.5));
    PREC vec4 c = Color.bgra;
    PREC float vertexLum = dot(c.rgb, kLumaWeights);

    // Debug mode 1: depth diagnostic — R=actual rasterized, G=UndisplacedDepth
    // Toggle RAlt+0 to compare old screen-space vs new world-space path.
    // Green > Red = UndisplacedDepth is deeper (correct). Red > Green = potential z-fighting.
    if (surfaceDebugMode == 1) {
        float actual = gl_FragCoord.z;
        float undis  = UndisplacedDepth;
        // Amplify to [0,1] range (terrain depth typically near 1.0, differences small)
        float lo = 0.85;
        float hi = 1.0;
        float r = clamp((actual - lo) / (hi - lo), 0.0, 1.0);
        float g = clamp((undis  - lo) / (hi - lo), 0.0, 1.0);
        FragColor = vec4(r, g, 0.0, 1.0);
        gl_FragDepth = actual;  // don't override depth in diagnostic mode
        return;
    }

    // Debug mode 2: show raw terrain colormap everywhere.
    if (surfaceDebugMode == 2) {
        gl_FragDepth = gl_FragCoord.z;
        FragColor = vec4(texColor.rgb, 1.0);
#ifdef MRT_ENABLED
        GBuffer1 = rc_gbuffer1_shadowHandled_flatUp();
#endif
        return;
    }

    // Smooth colormap classification — tiered by distance
    // Near: 9-tap disc, Mid: 5-tap cross, Far: 1-tap (no blur)
    const PREC float blurRadius = 0.18;
    PREC float r2 = blurRadius * 0.707;
    const PREC float uvMargin = 0.005;
    PREC vec2 uvMin = vec2(uvMargin);
    PREC vec2 uvMax = vec2(1.0 - uvMargin);

    PREC vec3 colAvg;
    if (lodMid < 0.01) {
        // Far: no blur — single sample
        colAvg = texColor.rgb;
    } else if (lodNear < 0.01) {
        // Mid: 5-tap cross only (skip diagonals)
        colAvg = texColor.rgb;
        colAvg += texture(tex1, clamp(Texcoord + vec2( blurRadius, 0.0), uvMin, uvMax)).rgb;
        colAvg += texture(tex1, clamp(Texcoord + vec2(-blurRadius, 0.0), uvMin, uvMax)).rgb;
        colAvg += texture(tex1, clamp(Texcoord + vec2(0.0,  blurRadius), uvMin, uvMax)).rgb;
        colAvg += texture(tex1, clamp(Texcoord + vec2(0.0, -blurRadius), uvMin, uvMax)).rgb;
        colAvg /= 5.0;
    } else {
        // Near: full 9-tap disc
        colAvg = texColor.rgb;
        colAvg += texture(tex1, clamp(Texcoord + vec2( blurRadius, 0.0), uvMin, uvMax)).rgb;
        colAvg += texture(tex1, clamp(Texcoord + vec2(-blurRadius, 0.0), uvMin, uvMax)).rgb;
        colAvg += texture(tex1, clamp(Texcoord + vec2(0.0,  blurRadius), uvMin, uvMax)).rgb;
        colAvg += texture(tex1, clamp(Texcoord + vec2(0.0, -blurRadius), uvMin, uvMax)).rgb;
        colAvg += texture(tex1, clamp(Texcoord + vec2( r2,  r2), uvMin, uvMax)).rgb;
        colAvg += texture(tex1, clamp(Texcoord + vec2(-r2,  r2), uvMin, uvMax)).rgb;
        colAvg += texture(tex1, clamp(Texcoord + vec2( r2, -r2), uvMin, uvMax)).rgb;
        colAvg += texture(tex1, clamp(Texcoord + vec2(-r2, -r2), uvMin, uvMax)).rgb;
        colAvg /= 9.0;
    }
    if (surfaceDebugMode == 3) {
        gl_FragDepth = gl_FragCoord.z;
        FragColor = vec4(colAvg, 1.0);
#ifdef MRT_ENABLED
        GBuffer1 = rc_gbuffer1_shadowHandled_flatUp();
#endif
        return;
    }
    PREC vec4 matWeights = getColorWeights(colAvg);

    // Snow weight: low-sat, mid-to-high-value colormap pixels (white-ish).
    // Wider raw gate catches blur-softened snow edges; sharpen step pushes decisive
    // snow pixels to full strength so snow areas don't fade through a grey no-mans-land.
    PREC vec3 hsvAvg = rgb2hsv(colAvg);
    PREC float snowRaw = smoothstep(0.15, 0.03, hsvAvg.y) * smoothstep(0.42, 0.62, hsvAvg.z);
    PREC float snowWeight = smoothstep(0.25, 0.55, snowRaw);
    // TerrainType: discrete 0/1/2/3 at vertices, interpolated by TES.
    // Boundary patches (cement+terrain vertex mix) have fragments with TerrainType in (2,3).
    // Pure terrain tiles never exceed TerrainType=2, so smoothstep(2.0,3.0) only activates
    // for fragments near cement vertices — blending concrete material into edge patches
    // without touching distant terrain tiles.
    PREC float pureConcrete = smoothstep(2.0, 3.0, TerrainType);
    // Use a stronger curve for color than for material/normal blending so boundary tiles
    // keep the smooth transition shape but visually track the pure cement tone more closely.
    PREC float concreteColorBlend = sqrt(clamp(pureConcrete, 0.0, 1.0));

    matWeights = mix(matWeights, vec4(0.0, 0.0, 0.0, 1.0), pureConcrete);
    // Snow is suppressed on cement tiles (pureConcrete dominates there).
    snowWeight *= (1.0 - pureConcrete);
    // Snow steals from the other weights proportionally so the total across all 5 = 1.
    matWeights *= (1.0 - snowWeight);

    PREC float totalWeights = matWeights.x + matWeights.y + matWeights.z + matWeights.w;
    if (totalWeights > 0.01) {
        matWeights /= totalWeights;
    } else {
        matWeights = vec4(1.0, 0.0, 0.0, 0.0);
    }

    // Far tier: keep only 2 strongest materials to halve normal map samples
    if (lodMid < 0.01) {
        float maxW = max(max(matWeights.x, matWeights.y), max(matWeights.z, matWeights.w));
        vec4 mask = step(maxW * 0.5, matWeights);
        matWeights *= mask;
        float total = matWeights.x + matWeights.y + matWeights.z + matWeights.w;
        if (total > 0.01) matWeights /= total;
    }

    if (surfaceDebugMode == 4) {
        gl_FragDepth = gl_FragCoord.z;
        FragColor = vec4(matWeights.x, matWeights.y, matWeights.z, 1.0);
#ifdef MRT_ENABLED
        GBuffer1 = rc_gbuffer1_shadowHandled_flatUp();
#endif
        return;
    }

    // Per-material tiling (rock, grass, dirt/riverbed, concrete).
    // Rock bumped from 1.0→3.0: at 1 texture per terrain tile, rock normal detail
    // was too stretched to read at RTS zoom on biomes without authored colormap variation.
    const PREC vec4 matTiling = vec4(3.0, 12.0, 1.0, 6.0);
    const PREC float matTilingSnow = 1.0;  // same broad tiling as rock/dirt
    PREC float baseTiling = detailNormalTiling.x;

    // Compute per-material UVs (straight tiling, anti-tiling done at sample time)
    PREC vec2 uvRock     = Texcoord * baseTiling * matTiling.x;
    PREC vec2 uvGrass    = Texcoord * baseTiling * matTiling.y;
    PREC vec2 uvDirt     = Texcoord * baseTiling * matTiling.z;
    PREC vec2 uvConcrete = Texcoord * baseTiling * matTiling.w;
    PREC vec2 uvSnow     = Texcoord * baseTiling * matTilingSnow;

    // POM — full at near, off at far (lodNear fades 1→0)
    if (pomParams.x > 0.0 && lodNear > 0.01) {
        PREC float effectivePomScale = pomParams.x * dot(pomScaleMat, matWeights);
        PREC vec3 viewDir = normalize(vec3(0.15, 0.85, 0.15));
        PREC float pomTiling = dot(matTiling, matWeights);
        PREC vec2 pomUV = Texcoord * baseTiling * pomTiling;
        PREC vec2 pomOffset = parallaxMapping(pomUV, viewDir, effectivePomScale * lodNear, matWeights) - pomUV;
        // Fade POM offset to zero at distance boundary (prevents popping)
        pomOffset *= lodNear;
        uvRock += pomOffset;
        uvGrass += pomOffset;
        uvDirt += pomOffset;
        uvConcrete += pomOffset;
    }

    // Per-material normal strength
    // Effective strength = normalBoost * detailNormalStrength.x (4.0 from C++)
    // Rock 1.3→0.9, grass 1.5→1.1: over-boost was producing grain-like noise at RTS zoom.
    // Dirt 1.1 is the "looks fantastic" reference — do not change it.
    // Concrete 2.5 unchanged — flat surfaces benefit from strong normal definition.
    // Non-const: normalBoost.y is scaled below by the combined grass fade.
    PREC vec4 normalBoost = vec4(0.9, 1.1, 1.1, 2.5);

    // Screen-space derivative AA — fade normals when detail goes sub-pixel
    PREC float fwRock     = clamp(1.0 - (length(fwidth(uvRock))     - 0.5) * 2.0, 0.0, 1.0);
    PREC float fwGrass    = clamp(1.0 - (length(fwidth(uvGrass))    - 0.5) * 2.0, 0.0, 1.0);
    PREC float fwDirt     = clamp(1.0 - (length(fwidth(uvDirt))     - 0.5) * 2.0, 0.0, 1.0);
    PREC float fwConcrete = clamp(1.0 - (length(fwidth(uvConcrete)) - 0.5) * 2.0, 0.0, 1.0);

    // Combine UV-derivative AA with world-space frequency fade for grass.
    // fwGrass catches sub-pixel UV aliasing; grassNormalFade catches projected
    // frequency grain at grazing angles regardless of zoom or LOD tier.
    normalBoost.y *= fwGrass * grassNormalFade;

    // Anti-tile scale — proportional to tiling so low-tiling materials skip it
    // At tiling >= 4, full anti-tiling (3.0); at tiling <= 1, plain sampling
    PREC float atsRock     = mix(0.0, 3.0, clamp((matTiling.x - 1.0) / 3.0, 0.0, 1.0));
    PREC float atsGrass    = mix(0.0, 3.0, clamp((matTiling.y - 1.0) / 3.0, 0.0, 1.0));
    PREC float atsDirt     = mix(0.0, 3.0, clamp((matTiling.z - 1.0) / 3.0, 0.0, 1.0));
    PREC float atsConcrete = mix(0.0, 3.0, clamp((matTiling.w - 1.0) / 3.0, 0.0, 1.0));

    // Sample each material — anti-tiled when near, plain texture when far
    PREC vec3 detailN = vec3(0.0);
    PREC vec4 matSample;
    bool useAntiTile = (lodNear > 0.01);

    if (matWeights.x > 0.01) {
        matSample = (useAntiTile && atsRock > 0.01) ? sampleAntiTile(matNormal0, uvRock, atsRock) : texture(matNormal0, uvRock);
        detailN += matWeights.x * normalBoost.x * fwRock * (matSample.rgb * 2.0 - 1.0);
    }
    if (matWeights.y > 0.01) {
        matSample = (useAntiTile && atsGrass > 0.01) ? sampleAntiTile(matNormal1, uvGrass, atsGrass) : texture(matNormal1, uvGrass);
        detailN += matWeights.y * normalBoost.y * (matSample.rgb * 2.0 - 1.0);
    }
    if (matWeights.z > 0.01) {
        matSample = (useAntiTile && atsDirt > 0.01) ? sampleAntiTile(matNormal2, uvDirt, atsDirt) : texture(matNormal2, uvDirt);
        detailN += matWeights.z * normalBoost.z * fwDirt * (matSample.rgb * 2.0 - 1.0);
    }
    if (matWeights.w > 0.01) {
        matSample = (useAntiTile && atsConcrete > 0.01) ? sampleAntiTile(matNormal3, uvConcrete, atsConcrete) : texture(matNormal3, uvConcrete);
        detailN += matWeights.w * normalBoost.w * fwConcrete * (matSample.rgb * 2.0 - 1.0);
    }
    // Snow normal contribution (broad tiling, moderate strength).
    if (snowWeight > 0.01) {
        PREC vec4 snowSample = texture(matNormal4, uvSnow);
        detailN += snowWeight * 0.9 * (snowSample.rgb * 2.0 - 1.0);
    }
    detailN *= (1.0 - pureConcrete);

    PREC vec3 N;
    N.xy = detailN.xy * detailNormalStrength.x;
    // Clamp normal deflection to prevent extreme angles that cause black snow
    // Max deflection of 0.7 means the normal can tilt ~35 degrees max
    N.xy = clamp(N.xy, -0.75, 0.75);
    N.z = 1.0;
    N = normalize(N);

    PREC float NdotL = dot(N, terrainLightDir.xyz);
    // Floor lowered 0.1→0.02 so shadow-facing bumps can actually read as dark.
    PREC float diffuse = clamp(NdotL, 0.02, 1.0);

    // --- Material color tinting ---
    const PREC vec3 tintRock     = vec3(0.36, 0.37, 0.40);
    const PREC vec3 tintGrass    = vec3(0.35, 0.42, 0.25);
    const PREC vec3 tintDirt     = vec3(0.48, 0.42, 0.33);
    const PREC vec3 tintConcrete = vec3(0.55, 0.53, 0.50);
    const PREC vec3 tintSnow     = vec3(0.75, 0.78, 0.84);  // dimmed cool grey-white

    PREC vec3 materialTint = tintRock * matWeights.x
                           + tintGrass * matWeights.y
                           + tintDirt * matWeights.z
                           + tintConcrete * matWeights.w
                           + tintSnow * snowWeight;

    // Luminance-adaptive tint: dark colormap pixels get far less tint pull so they
    // don't lift to mid-grey. Snow always gets full tint (cool white must pop).
    PREC float colLum = dot(texColor.rgb, kLumaWeights);
    PREC float tintBase = mix(0.18, 0.50, smoothstep(0.1, 0.6, colLum));
    PREC float tintStrength = mix(tintBase, 0.85, snowWeight);
    PREC vec3 baseColor = mix(texColor.rgb, materialTint, tintStrength);
    {
        // Preserve the authored colormap tone for runway/cement.
        // Full concrete definitely comes through this shader path. Use the authored
        // runway/apron colormap directly for solid cement instead of routing it back
        // toward the generic concrete material tint.
        PREC vec3 concreteColor = texColor.rgb;
        baseColor = mix(baseColor, concreteColor, concreteColorBlend);
    }

    // --- Phase 4C: Triplanar cliff mapping ---
    // On steep slopes, darken and shift toward rock color to simulate exposed rock faces
    {
        PREC float slopeZ = abs(WorldNorm.z);
        // Start blending at ~30° slope (0.85), full at ~55° (0.55)
        PREC float cliffBlend = smoothstep(0.85, 0.55, slopeZ);
        if (cliffBlend > 0.01) {
            // Desaturate and darken toward rock tint on cliff faces
            PREC float luma = dot(baseColor, vec3(0.299, 0.587, 0.114));
            PREC vec3 cliffColor = mix(vec3(luma), tintRock, 0.6) * 0.8;
            baseColor = mix(baseColor, cliffColor, cliffBlend * 0.7);
        }
    }

    // World-space break-up noise for non-snow terrain. Two-octave — low frequency
    // for large patches, higher frequency for surface-texture feel. Dialed back ~10%
    // from the debug-time range now that the lighting range fix carries its own weight.
    {
        PREC float lowFreq  = fbm(WorldPos.xy * 0.0035, 3) * 0.5 + 0.5;
        PREC float highFreq = fbm(WorldPos.xy * 0.018,  2) * 0.5 + 0.5;
        PREC float breakupNoise = mix(lowFreq, highFreq, 0.55);
        PREC float breakupMod = mix(0.78, 1.18, breakupNoise);
        PREC float breakupAmount = (1.0 - snowWeight) * breakupFade;
        baseColor *= mix(1.0, breakupMod, breakupAmount);
    }

    c.rgb *= baseColor;
    // Normal map lighting — widened range from (0.55,1.15) to (0.35,1.20)
    // so dark sides of bumps read noticeably darker, creating actual bump contrast.
    PREC float normalLight = mix(0.35, 1.20, diffuse);
    normalLight = mix(normalLight, 1.0, pureConcrete * 0.85);
    c.rgb *= normalLight;

    if (surfaceDebugMode == 5) {
        gl_FragDepth = gl_FragCoord.z;
        FragColor = vec4(vec3(normalLight), 1.0);
#ifdef MRT_ENABLED
        GBuffer1 = rc_gbuffer1_shadowHandled(N);
#endif
        return;
    }

    // Shadow — variable PCF taps by distance.
    // Pass flat up-normal (not detail-perturbed N) so slope-scale bias stays consistent
    // pixel-to-pixel. Using N here caused sprinkle/inverted-shadow patterns on bumpy
    // terrain — bias flipped across neighboring detail-normal deflections, letting
    // some pixels escape shadow while neighbors received it. Overlays already pass
    // vec3(0,0,1) for the same reason.
    const PREC vec3 shadowN = vec3(0.0, 0.0, 1.0);
    int shadowTaps = (lodNear > 0.5) ? 16 : (lodMid > 0.5) ? 8 : 4;
    float staticShadow = calcShadow(WorldPos, shadowN, terrainLightDir.xyz, shadowTaps);
    int dynTaps = (lodNear > 0.5) ? 8 : 4;
    float dynShadow = calcDynamicShadow(WorldPos, shadowN, terrainLightDir.xyz, dynTaps);
    float shadow = staticShadow * dynShadow;
    c.rgb *= shadow;

    // --- Snow sparkle ---
    // High-frequency hashed micro-glints gated by snow weight, light direction, and shadow.
    // Keeps perfectly still when camera is still (hash on world position), matches sun direction.
    if (snowWeight > 0.05) {
        PREC vec2 glintUV = WorldPos.xy * 0.75;
        PREC vec2 gc = floor(glintUV);
        PREC float hash = fract(sin(dot(gc, vec2(12.9898, 78.233))) * 43758.5453);
        PREC float glint = step(0.985, hash);
        // Specular-ish: hot only when normal roughly faces the sun (half-angle cheap approx)
        PREC vec3 viewApprox = normalize(vec3(0.0, 0.0, 1.0));
        PREC vec3 H = normalize(terrainLightDir.xyz + viewApprox);
        PREC float specMask = pow(clamp(dot(N, H), 0.0, 1.0), 48.0);
        PREC float sparkle = glint * specMask * snowWeight * shadow * 0.45;
        c.rgb += vec3(sparkle);
    }

    // --- Phase 4A: Procedural cloud shadows ---
    // Applied inline so cloud UV uses the actual tessellated WorldPos (stable world-space).
    // Terrain overlays use the same cloud expression in terrain_overlay.frag.
    {
        PREC vec2 cloudUV = WorldPos.xy * 0.0006 + vec2(time * 0.012, time * 0.005);
        PREC float cloudNoise = fbm(cloudUV, 4) * 0.5 + 0.5;
        PREC float cloudShadow = smoothstep(0.3, 0.7, cloudNoise);
        if (surfaceDebugMode == 7) {
            gl_FragDepth = gl_FragCoord.z;
            FragColor = vec4(vec3(mix(0.92, 1.0, cloudShadow)), 1.0);
#ifdef MRT_ENABLED
            GBuffer1 = rc_gbuffer1_legacyTerrainMaterialAlpha(N, materialAlpha);
#endif
            return;
        }
        // Cloud shadow: 15% amplitude on non-snow terrain, tapering to 3% on snow so
        // the blowing-snow particles below aren't competing with bulk darkening.
        PREC float cloudLo = mix(0.85, 0.97, snowWeight);
        c.rgb *= mix(cloudLo, 1.0, cloudShadow);

        // Blowing snow: near snow edges the animated FBM drives a subtle additive
        // white glow that reads as windblown particles drifting across the boundary.
        // Peaks at ~mid snowWeight (transition zones); fades out on pure rock or pure snow.
        PREC float snowEdge = smoothstep(0.02, 0.35, snowWeight) *
                              (1.0 - smoothstep(0.75, 1.0, snowWeight));
        PREC vec3  snowParticleTint = vec3(0.98, 0.99, 1.02);
        c.rgb += snowParticleTint * cloudShadow * snowEdge * 0.09 * shadow;
    }

    if (surfaceDebugMode == 6) {
        gl_FragDepth = gl_FragCoord.z;
        FragColor = vec4(vec3(shadow), 1.0);
#ifdef MRT_ENABLED
        GBuffer1 = rc_gbuffer1_legacyTerrainMaterialAlpha(N, materialAlpha);
#endif
        return;
    }

    // --- Phase 4B: Height-based exponential fog ---
    {
        PREC float camDist2D = distance(WorldPos.xy, cameraPos.xy);
        PREC float terrainHeight = WorldPos.z;
        PREC float fogDensity = 0.00006;
        PREC float heightScale = exp(-max(terrainHeight, 0.0) * 0.002);
        PREC float fogAmount = 1.0 - exp(-camDist2D * fogDensity * heightScale);
        fogAmount = clamp(fogAmount, 0.0, 0.70);
        PREC vec3 fogCol = vec3(0.58, 0.65, 0.75);
        c.rgb = mix(c.rgb, fogCol, fogAmount);
    }

    // --- Map-edge haze ---
    // Vanilla MC2 emitted a ring of terrain beyond the playable area and hid it with
    // haze-to-sky. With global fog disabled, those meta-ring tiles (which sample magenta
    // "no-data" texels from the colormap interior) become visible. Apply a short-range
    // haze-to-sky fade across the last ~one-tile band to reproduce the vanilla result
    // without bringing back global distance fog.
    if (mapHalfExtent > 0.0) {
        PREC vec3 edgeSkyCol = vec3(0.58, 0.65, 0.75);
        PREC float chebDist  = max(abs(WorldPos.x), abs(WorldPos.y));
        PREC float edgeStart = mapHalfExtent - 256.0;
        PREC float edgeEnd   = mapHalfExtent - 32.0;
        PREC float edgeHaze  = smoothstep(edgeStart, edgeEnd, chebDist);
        c.rgb = mix(c.rgb, edgeSkyCol, edgeHaze);
    }

    c.a = 1.0;
    FragColor = c;

#ifdef MRT_ENABLED
    GBuffer1 = rc_gbuffer1_legacyTerrainMaterialAlpha(N, materialAlpha);
#endif

    // Write depth for overlay/object depth testing.
    // Use max(UndisplacedDepth, gl_FragCoord.z) so:
    //  - Upward-displaced terrain: writes UndisplacedDepth (deeper = original surface), overlays pass.
    //  - Downward-displaced terrain: writes actual rasterized depth (deeper = no self-occlusion dark patches).
    // Overlays at the undisplaced surface are always shallower than max(), so they always pass GL_LEQUAL.
    gl_FragDepth = clamp(max(UndisplacedDepth, gl_FragCoord.z) + 0.0005, 0.0, 1.0);
}
