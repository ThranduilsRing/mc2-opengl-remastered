// VK static-only port of shaders/shadow_screen.frag.
// Dynamic/CSM leg intentionally omitted; C++ declines when dynamic shadows are live.
#version 450

#define PREC highp

bool rc_pixelHandlesOwnShadow(PREC vec4 gbuf1) {
    return gbuf1.a > 0.5;
}

layout(location = 0) in  vec2 TexCoord;
layout(location = 0) out PREC vec4 FragColor;

layout(set = 0, binding = 0) uniform sampler2D sceneDepthTex;
layout(set = 0, binding = 1) uniform sampler2D sceneNormalTex;
layout(set = 0, binding = 2) uniform sampler2DShadow shadowMap;

layout(std140, set = 0, binding = 3) uniform ScreenShadowStaticParams {
    mat4 inverseViewProj;
    mat4 lightSpaceMatrix;
    vec2 screenSize;
    int  enableShadows;
    int  debugMode;
    float shadowSoftness;
    float _pad0;
    float _pad1;
    float _pad2;
} u;

#define inverseViewProj u.inverseViewProj
#define lightSpaceMatrix u.lightSpaceMatrix
#define screenSize u.screenSize
#define enableShadows u.enableShadows
#define debugMode u.debugMode
#define shadowSoftness u.shadowSoftness

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
    projCoords.xy = projCoords.xy * 0.5 + 0.5;

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
    float ndc_z = depth;
    vec4 worldPos4 = inverseViewProj * vec4(ndc_xy, ndc_z, 1.0);
    return worldPos4.xyz / worldPos4.w;
}

void main()
{
    PREC vec4 normalData = texture(sceneNormalTex, TexCoord);
    float depth = texture(sceneDepthTex, TexCoord).r;
    bool pixelHandlesOwnShadow = rc_pixelHandlesOwnShadow(normalData);

    if (debugMode == 1) {
        if (depth <= 0.0001) {
            FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        } else if (pixelHandlesOwnShadow) {
            FragColor = vec4(0.4, 0.2, 0.0, 1.0);
        } else {
            vec3 worldPos = reconstructWorldPos(TexCoord, depth);
            float shadow = (enableShadows == 1)
                ? sampleShadowMap(shadowMap, worldPos, lightSpaceMatrix, 8)
                : 1.0;
            FragColor = (shadow < 0.99)
                ? vec4(0.0, 0.0, shadow, 1.0)
                : vec4(0.0, 0.3, 0.0, 1.0);
        }
        return;
    }

    if (depth <= 0.0001 || pixelHandlesOwnShadow) {
        FragColor = vec4(1.0);
        return;
    }

    vec3 worldPos = reconstructWorldPos(TexCoord, depth);
    float shadow = 1.0;
    if (enableShadows == 1)
        shadow = min(shadow, sampleShadowMap(shadowMap, worldPos, lightSpaceMatrix, 8));
    FragColor = vec4(shadow, shadow, shadow, 1.0);
}
