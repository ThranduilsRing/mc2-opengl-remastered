//#version 400 (version provided by material prefix)

#define PREC highp

#include <include/shadow.hglsl>
#include <include/noise.hglsl>

in PREC vec4 Color;
in PREC vec2 Texcoord;
in PREC float FogValue;
in PREC float TerrainType;
in PREC vec3 WorldNorm;
in PREC vec3 WorldPos;
in PREC float UndisplacedDepth;

layout (location=0) out PREC vec4 FragColor;
layout (location=1) out PREC vec4 GBuffer1;

uniform sampler2D tex1;  // colormap
uniform sampler2D tex2;  // detail normal (engine default, fallback)
uniform sampler2D tex3;  // detail displacement (legacy, unused with per-material POM)

// Per-material normal maps (individual sampler2D on units 5-8)
// Alpha channel = displacement map for per-material POM
uniform sampler2D matNormal0;  // rock
uniform sampler2D matNormal1;  // grass
uniform sampler2D matNormal2;  // dirt
uniform sampler2D matNormal3;  // concrete

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

// --- Distance LOD thresholds (tunable, in MC2 world units) ---
// 1 terrain tile ≈ 128 world units
const float LOD_NEAR       = 4000.0;   // full quality (covers stock zoom)
const float LOD_NEAR_FADE  = 4500.0;   // transition band end
const float LOD_MID        = 5500.0;   // mid quality
const float LOD_MID_FADE   = 6500.0;   // far quality begins

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

    w.y = smoothstep(0.14, 0.17, h) * smoothstep(0.15, 0.28, s);
    w.z = smoothstep(0.155, 0.13, h) * smoothstep(0.15, 0.28, s);
    w.w = smoothstep(0.18, 0.10, s) * smoothstep(0.50, 0.62, v);
    w.x = smoothstep(0.18, 0.10, s) * smoothstep(0.45, 0.30, v);
    w.x += smoothstep(0.38, 0.28, v) * smoothstep(0.15, 0.25, s);

    PREC float isWater = smoothstep(0.35, 0.45, h);
    w.x += isWater;
    w.y *= (1.0 - isWater);
    w.z *= (1.0 - isWater);

    PREC float total = w.x + w.y + w.z + w.w;
    w = (total < 0.01) ? vec4(0.0, 0.0, 1.0, 0.0) : w / total;
    return w;
}

// --- Per-material displacement sampling ---

const PREC vec4 pomScaleMat = vec4(0.5, 1.0, 2.5, 1.0);

PREC float sampleDisplacement(PREC vec2 uv, PREC vec4 weights) {
    PREC float d = 0.0;
    if (weights.x > 0.01) d += weights.x * texture(matNormal0, uv).a;
    if (weights.y > 0.01) d += weights.y * texture(matNormal1, uv).a;
    if (weights.z > 0.01) d += weights.z * texture(matNormal2, uv).a;
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

    if (tessDebug.x > 0.5) {
        FragColor = vec4(1.0, 0.0, 0.0, 1.0);  // SOLID RED = tess frag running
        GBuffer1 = vec4(0.5, 0.5, 1.0, 1.0);
        return;
    }

    PREC vec4 texColor = texture(tex1, Texcoord);

#ifdef ALPHA_TEST
    if(texColor.a < 0.5)
        discard;
#endif

    PREC vec4 c = Color.bgra;

    // Smooth colormap classification — tiered by distance
    // Near: 9-tap disc, Mid: 5-tap cross, Far: 1-tap (no blur)
    const PREC float blurRadius = 0.11;
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
    PREC vec4 matWeights = getColorWeights(colAvg);

    // Far tier: keep only 2 strongest materials to halve normal map samples
    if (lodMid < 0.01) {
        float maxW = max(max(matWeights.x, matWeights.y), max(matWeights.z, matWeights.w));
        vec4 mask = step(maxW * 0.5, matWeights);
        matWeights *= mask;
        float total = matWeights.x + matWeights.y + matWeights.z + matWeights.w;
        if (total > 0.01) matWeights /= total;
    }

#ifdef DEBUG_MATERIALS
    FragColor = vec4(matWeights.x, matWeights.y, matWeights.z, 1.0);
    GBuffer1 = vec4(0.5, 0.5, 1.0, 1.0);
    return;
#endif

    // Per-material tiling (rock, grass, dirt/riverbed, concrete)
    const PREC vec4 matTiling = vec4(1.0, 12.0, 1.0, 6.0);
    PREC float baseTiling = detailNormalTiling.x;

    // Compute per-material UVs (straight tiling, anti-tiling done at sample time)
    PREC vec2 uvRock     = Texcoord * baseTiling * matTiling.x;
    PREC vec2 uvGrass    = Texcoord * baseTiling * matTiling.y;
    PREC vec2 uvDirt     = Texcoord * baseTiling * matTiling.z;
    PREC vec2 uvConcrete = Texcoord * baseTiling * matTiling.w;

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
    // std: rock=21, grass=35, dirt(aerial)=6, concrete=11
    const PREC vec4 normalBoost = vec4(0.6, 1.2, 0.8, 2.5);

    // Screen-space derivative AA — fade normals when detail goes sub-pixel
    PREC float fwRock     = clamp(1.0 - (length(fwidth(uvRock))     - 0.5) * 2.0, 0.0, 1.0);
    PREC float fwGrass    = clamp(1.0 - (length(fwidth(uvGrass))    - 0.5) * 2.0, 0.0, 1.0);
    PREC float fwDirt     = clamp(1.0 - (length(fwidth(uvDirt))     - 0.5) * 2.0, 0.0, 1.0);
    PREC float fwConcrete = clamp(1.0 - (length(fwidth(uvConcrete)) - 0.5) * 2.0, 0.0, 1.0);

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
        detailN += matWeights.y * normalBoost.y * fwGrass * (matSample.rgb * 2.0 - 1.0);
    }
    if (matWeights.z > 0.01) {
        matSample = (useAntiTile && atsDirt > 0.01) ? sampleAntiTile(matNormal2, uvDirt, atsDirt) : texture(matNormal2, uvDirt);
        detailN += matWeights.z * normalBoost.z * fwDirt * (matSample.rgb * 2.0 - 1.0);
    }
    if (matWeights.w > 0.01) {
        matSample = (useAntiTile && atsConcrete > 0.01) ? sampleAntiTile(matNormal3, uvConcrete, atsConcrete) : texture(matNormal3, uvConcrete);
        detailN += matWeights.w * normalBoost.w * fwConcrete * (matSample.rgb * 2.0 - 1.0);
    }

    PREC vec3 N;
    N.xy = detailN.xy * detailNormalStrength.x;
    // Clamp normal deflection to prevent extreme angles that cause black snow
    // Max deflection of 0.7 means the normal can tilt ~35 degrees max
    N.xy = clamp(N.xy, -0.7, 0.7);
    N.z = 1.0;
    N = normalize(N);

    PREC float NdotL = dot(N, terrainLightDir.xyz);
    PREC float diffuse = clamp(NdotL, 0.1, 1.0);

    // --- Material color tinting ---
    const PREC vec3 tintRock     = vec3(0.36, 0.37, 0.40);
    const PREC vec3 tintGrass    = vec3(0.35, 0.42, 0.25);
    const PREC vec3 tintDirt     = vec3(0.48, 0.42, 0.33);
    const PREC vec3 tintConcrete = vec3(0.55, 0.53, 0.50);

    PREC vec3 materialTint = tintRock * matWeights.x
                           + tintGrass * matWeights.y
                           + tintDirt * matWeights.z
                           + tintConcrete * matWeights.w;

    const PREC float tintStrength = 0.70;
    PREC vec3 baseColor = mix(texColor.rgb, materialTint, tintStrength);

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

    c.rgb *= baseColor;

    // Normal map lighting — moderate range for visible detail without black crush
    PREC float normalLight = mix(0.55, 1.15, diffuse);
    c.rgb *= normalLight;

    // Shadow — variable PCF taps by distance
    int shadowTaps = (lodNear > 0.5) ? 16 : (lodMid > 0.5) ? 8 : 4;
    float staticShadow = calcShadow(WorldPos, N, terrainLightDir.xyz, shadowTaps);
    int dynTaps = (lodNear > 0.5) ? 8 : 4;
    float dynShadow = calcDynamicShadow(WorldPos, N, terrainLightDir.xyz, dynTaps);
    float shadow = staticShadow * dynShadow;
    c.rgb *= shadow;

    // --- Phase 4A: Procedural cloud shadows ---
    // Animated FBM noise creates drifting cloud shadow patterns
    {
        // Smaller scale = smaller clouds, more visible pattern
        PREC vec2 cloudUV = WorldPos.xy * 0.0006 + vec2(time * 0.012, time * 0.005);
        PREC float cloudNoise = fbm(cloudUV, 4) * 0.5 + 0.5;  // [0,1]
        PREC float cloudShadow = smoothstep(0.3, 0.7, cloudNoise);
        // Visible darkening: 70% in shadow, 100% in light
        c.rgb *= mix(0.70, 1.0, cloudShadow);
    }

    // --- Phase 4B: Height-based exponential fog ---
    // Atmospheric perspective: thicker in valleys, thinner at altitude
    {
        PREC float camDist2D = distance(WorldPos.xy, cameraPos.xy);
        PREC float terrainHeight = WorldPos.z;
        // Higher density = visible at shorter distances
        PREC float fogDensity = 0.00015;
        PREC float heightScale = exp(-max(terrainHeight, 0.0) * 0.002);
        PREC float fogAmount = 1.0 - exp(-camDist2D * fogDensity * heightScale);
        fogAmount = clamp(fogAmount, 0.0, 0.70);
        PREC vec3 fogCol = vec3(0.58, 0.65, 0.75);
        c.rgb = mix(c.rgb, fogCol, fogAmount);
    }

    FragColor = c;

    GBuffer1 = vec4(N * 0.5 + 0.5, 1.0);

    // Write un-displaced depth so overlays and objects (at original surface height)
    // pass GL_LEQUAL depth test against this terrain fragment.
    // Small positive bias ensures overlays always pass despite float precision differences.
    gl_FragDepth = clamp(UndisplacedDepth + 0.0005, 0.0, 1.0);
}
