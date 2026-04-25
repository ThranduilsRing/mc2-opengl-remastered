//#version 420 (version provided by prefix)

#define PREC highp

// fbm is inlined here on purpose: the shader include machinery uses a
// backslash path separator on Windows while shader references use forward
// slashes, so pulling in an external fbm helper resolves to nothing silently.
PREC vec3 mod289_3(PREC vec3 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
PREC vec2 mod289_2(PREC vec2 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
PREC vec3 permute(PREC vec3 x) { return mod289_3(((x * 34.0) + 1.0) * x); }

PREC float snoise(PREC vec2 v) {
    const PREC vec4 C = vec4(0.211324865405187, 0.366025403784439, -0.577350269189626, 0.024390243902439);
    PREC vec2 i  = floor(v + dot(v, C.yy));
    PREC vec2 x0 = v - i + dot(i, C.xx);
    PREC vec2 i1 = (x0.x > x0.y) ? vec2(1.0, 0.0) : vec2(0.0, 1.0);
    PREC vec4 x12 = x0.xyxy + C.xxzz;
    x12.xy -= i1;
    i = mod289_2(i);
    PREC vec3 p = permute(permute(i.y + vec3(0.0, i1.y, 1.0)) + i.x + vec3(0.0, i1.x, 1.0));
    PREC vec3 m = max(0.5 - vec3(dot(x0, x0), dot(x12.xy, x12.xy), dot(x12.zw, x12.zw)), 0.0);
    m = m * m; m = m * m;
    PREC vec3 x = 2.0 * fract(p * C.www) - 1.0;
    PREC vec3 h = abs(x) - 0.5;
    PREC vec3 ox = floor(x + 0.5);
    PREC vec3 a0 = x - ox;
    m *= 1.79284291400159 - 0.85373472095314 * (a0 * a0 + h * h);
    PREC vec3 g;
    g.x = a0.x * x0.x + h.x * x0.y;
    g.yz = a0.yz * x12.xz + h.yz * x12.yw;
    return 130.0 * dot(m, g);
}

PREC float fbm(PREC vec2 p, int octaves) {
    PREC float value = 0.0;
    PREC float amplitude = 0.5;
    PREC float frequency = 1.0;
    for (int i = 0; i < octaves; i++) {
        value += amplitude * snoise(p * frequency);
        frequency *= 2.0;
        amplitude *= 0.5;
    }
    return value;
}

in vec2 TexCoord;
layout(location = 0) out PREC vec4 FragColor;

uniform sampler2D sceneDepthTex;
uniform sampler2D sceneNormalTex;
uniform sampler2DShadow shadowMap;
uniform sampler2DShadow dynamicShadowMap;
uniform mat4 inverseViewProj;
uniform mat4 lightSpaceMatrix;
uniform mat4 dynamicLightSpaceMatrix;
uniform vec2 screenSize;
uniform int enableShadows;
uniform int enableDynamicShadows;
uniform float shadowSoftness;
uniform int debugMode;    // 0=normal, 1=visualize classification
uniform float time;       // seconds, for animated cloud shadows

const vec2 poissonDisk[8] = vec2[](
    vec2(-0.94201624, -0.39906216),
    vec2( 0.94558609, -0.76890725),
    vec2(-0.09418410, -0.92938870),
    vec2( 0.34495938,  0.29387760),
    vec2(-0.91588581,  0.45771432),
    vec2(-0.81544232, -0.87912464),
    vec2(-0.38277543,  0.27676845),
    vec2( 0.97484398,  0.75648379)
);

float sampleShadowMap(sampler2DShadow smap, vec3 worldPos, mat4 lsMatrix, int numTaps)
{
    vec4 lsPos = lsMatrix * vec4(worldPos, 1.0);
    vec3 projCoords = lsPos.xyz / lsPos.w;
    projCoords = projCoords * 0.5 + 0.5;

    if (projCoords.z > 1.0 || projCoords.z < 0.0) return 1.0;
    if (projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0) return 1.0;

    float bias = 0.003;
    float currentDepth = projCoords.z - bias;

    float angle = 6.2831853 * fract(sin(dot(worldPos.xz, vec2(12.9898, 78.233))) * 43758.5453);
    float ca = cos(angle), sa = sin(angle);
    mat2 rot = mat2(ca, sa, -sa, ca);

    vec2 texelSize = 1.0 / vec2(textureSize(smap, 0));
    float radius = max(shadowSoftness, 0.5);
    float shadow = 0.0;
    int taps = clamp(numTaps, 1, 8);
    for (int i = 0; i < taps; i++) {
        vec2 offset = rot * poissonDisk[i] * radius * texelSize;
        shadow += texture(smap, vec3(projCoords.xy + offset, currentDepth));
    }
    shadow /= float(taps);

    return mix(0.4, 1.0, shadow);
}

vec3 reconstructWorldPos(vec2 uv, float depth)
{
    vec2 ndc_xy = uv * 2.0 - 1.0;
    float ndc_z = depth * 2.0 - 1.0;
    vec4 worldPos4 = inverseViewProj * vec4(ndc_xy, ndc_z, 1.0);
    return worldPos4.xyz / worldPos4.w;
}

void main()
{
    PREC vec4 normalData = texture(sceneNormalTex, TexCoord);
    float depth = texture(sceneDepthTex, TexCoord).r;
    bool isTerrain = normalData.a > 0.5;
    // Debug mode: visualize what the shader classifies each pixel as
    if (debugMode == 1) {
        if (depth >= 1.0) {
            FragColor = vec4(0.0, 0.0, 0.0, 1.0);  // black = sky/no depth
        } else if (isTerrain) {
            FragColor = vec4(0.4, 0.2, 0.0, 1.0);  // brown = terrain (skipped by this pass)
        } else {
            // Non-terrain: reconstruct and show shadow result
            vec3 worldPos = reconstructWorldPos(TexCoord, depth);

            float shadow = 1.0;
            if (enableShadows == 1)
                shadow = min(shadow, sampleShadowMap(shadowMap, worldPos, lightSpaceMatrix, 8));
            if (enableDynamicShadows == 1)
                shadow = min(shadow, sampleShadowMap(dynamicShadowMap, worldPos, dynamicLightSpaceMatrix, 4));

            if (shadow < 0.99) {
                FragColor = vec4(0.0, 0.0, shadow, 1.0);  // blue = shadowed
            } else {
                FragColor = vec4(0.0, 0.3, 0.0, 1.0);  // dark green = lit
            }
        }
        return;
    }

    if (depth >= 1.0) {
        FragColor = vec4(1.0);
        return;
    }

    // Terrain: inline calcShadow + cloud shadows in gos_terrain.frag handle everything.
    if (isTerrain) {
        FragColor = vec4(1.0);
        return;
    }

    // Non-terrain (overlays, objects): reconstruct position for shadow + cloud.
    // Overlay pixels don't write depth — depth buffer holds terrain depth at their position,
    // so worldPos.xy matches the terrain surface below (same cloud UV as terrain inline).
    vec3 worldPos = reconstructWorldPos(TexCoord, depth);

    // Cloud shadows — same formula as gos_terrain.frag for matching appearance.
    float cloudFactor = 1.0;
    {
        vec2 cloudUV = worldPos.xy * 0.0006 + vec2(time * 0.012, time * 0.005);
        float cloudNoise = fbm(cloudUV, 4) * 0.5 + 0.5;
        cloudFactor = mix(0.85, 1.0, smoothstep(0.3, 0.7, cloudNoise));
    }

    float shadow = 1.0;
    if (enableShadows == 1) {
        shadow = min(shadow, sampleShadowMap(shadowMap, worldPos, lightSpaceMatrix, 8));
    }
    if (enableDynamicShadows == 1) {
        float dynShadow = sampleShadowMap(dynamicShadowMap, worldPos, dynamicLightSpaceMatrix, 4);
        shadow = min(shadow, dynShadow);
    }

    float combined = shadow * cloudFactor;
    FragColor = vec4(combined, combined, combined, 1.0);
}
