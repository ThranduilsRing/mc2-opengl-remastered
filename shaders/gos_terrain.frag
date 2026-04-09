//#version 400 (version provided by material prefix)

#define PREC highp

in PREC vec4 Color;
in PREC vec2 Texcoord;
in PREC float FogValue;
in PREC float TerrainType;

layout (location=0) out PREC vec4 FragColor;

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

const PREC vec4 pomScaleMat = vec4(0.5, 1.0, 1.5, 1.0);

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
    PREC vec4 texColor = texture(tex1, Texcoord);

#ifdef ALPHA_TEST
    if(texColor.a < 0.5)
        discard;
#endif

    PREC vec4 c = Color.bgra;

    // Smooth colormap classification — 5-tap box filter for coherent zones
    // Larger radius = bigger material zones, less noisy splotching
    const PREC float blurRadius = 0.02;
    PREC vec3 colAvg = texColor.rgb;
    colAvg += texture(tex1, Texcoord + vec2( blurRadius, 0.0)).rgb;
    colAvg += texture(tex1, Texcoord + vec2(-blurRadius, 0.0)).rgb;
    colAvg += texture(tex1, Texcoord + vec2(0.0,  blurRadius)).rgb;
    colAvg += texture(tex1, Texcoord + vec2(0.0, -blurRadius)).rgb;
    colAvg *= 0.2;
    PREC vec4 matWeights = getColorWeights(colAvg);

#ifdef DEBUG_MATERIALS
    FragColor = vec4(matWeights.x, matWeights.y, matWeights.z, 1.0);
    return;
#endif

    // Per-material tiling (rock, grass, dirt/pebbles, concrete)
    const PREC vec4 matTiling = vec4(8.0, 12.0, 24.0, 6.0);
    PREC float baseTiling = detailNormalTiling.x;

    // Compute per-material UVs (straight tiling, anti-tiling done at sample time)
    PREC vec2 uvRock     = Texcoord * baseTiling * matTiling.x;
    PREC vec2 uvGrass    = Texcoord * baseTiling * matTiling.y;
    PREC vec2 uvDirt     = Texcoord * baseTiling * matTiling.z;
    PREC vec2 uvConcrete = Texcoord * baseTiling * matTiling.w;

    // POM
    if (pomParams.x > 0.0) {
        PREC float effectivePomScale = pomParams.x * dot(pomScaleMat, matWeights);
        PREC vec3 viewDir = normalize(vec3(0.15, 0.85, 0.15));
        PREC float pomTiling = dot(matTiling, matWeights);
        PREC vec2 pomUV = Texcoord * baseTiling * pomTiling;
        PREC vec2 pomOffset = parallaxMapping(pomUV, viewDir, effectivePomScale, matWeights) - pomUV;
        uvRock += pomOffset;
        uvGrass += pomOffset;
        uvDirt += pomOffset;
        uvConcrete += pomOffset;
    }

    // Per-material normal strength
    // Effective strength = normalBoost * detailNormalStrength.x (4.0 from C++)
    // std: rock=21, grass(new)=35, dirt(pebbles)=50, concrete=11
    const PREC vec4 normalBoost = vec4(2.0, 1.2, 0.6, 2.5);

    // Screen-space derivative AA — fade normals when detail goes sub-pixel
    PREC float fwRock     = clamp(1.0 - (length(fwidth(uvRock))     - 0.5) * 2.0, 0.0, 1.0);
    PREC float fwGrass    = clamp(1.0 - (length(fwidth(uvGrass))    - 0.5) * 2.0, 0.0, 1.0);
    PREC float fwDirt     = clamp(1.0 - (length(fwidth(uvDirt))     - 0.5) * 2.0, 0.0, 1.0);
    PREC float fwConcrete = clamp(1.0 - (length(fwidth(uvConcrete)) - 0.5) * 2.0, 0.0, 1.0);

    // Anti-tile scale — how large the blend regions are (in tiled UV units)
    // Larger = more pattern breakup, smaller = more coherent but more repetition
    const PREC float antiTileScale = 3.0;

    // Sample each material with anti-tiling blend
    PREC vec3 detailN = vec3(0.0);
    PREC vec4 matSample;
    if (matWeights.x > 0.01) {
        matSample = sampleAntiTile(matNormal0, uvRock, antiTileScale);
        detailN += matWeights.x * normalBoost.x * fwRock * (matSample.rgb * 2.0 - 1.0);
    }
    if (matWeights.y > 0.01) {
        matSample = sampleAntiTile(matNormal1, uvGrass, antiTileScale);
        detailN += matWeights.y * normalBoost.y * fwGrass * (matSample.rgb * 2.0 - 1.0);
    }
    if (matWeights.z > 0.01) {
        matSample = sampleAntiTile(matNormal2, uvDirt, antiTileScale);
        detailN += matWeights.z * normalBoost.z * fwDirt * (matSample.rgb * 2.0 - 1.0);
    }
    if (matWeights.w > 0.01) {
        matSample = sampleAntiTile(matNormal3, uvConcrete, antiTileScale);
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
    const PREC vec3 tintRock     = vec3(0.40, 0.38, 0.35);
    const PREC vec3 tintGrass    = vec3(0.35, 0.42, 0.25);
    const PREC vec3 tintDirt     = vec3(0.45, 0.38, 0.28);
    const PREC vec3 tintConcrete = vec3(0.55, 0.53, 0.50);

    PREC vec3 materialTint = tintRock * matWeights.x
                           + tintGrass * matWeights.y
                           + tintDirt * matWeights.z
                           + tintConcrete * matWeights.w;

    const PREC float tintStrength = 0.35;
    PREC vec3 baseColor = mix(texColor.rgb, materialTint, tintStrength);

    c.rgb *= baseColor;

    // Normal map lighting — moderate range for visible detail without black crush
    PREC float normalLight = mix(0.55, 1.15, diffuse);
    c.rgb *= normalLight;

    if(fog_color.x>0.0 || fog_color.y>0.0 || fog_color.z>0.0 || fog_color.w>0.0)
        c.rgb = mix(fog_color.rgb, c.rgb, FogValue);

    FragColor = c;
}
