//#version 420 (version provided by prefix)

#define PREC highp

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

void main()
{
    PREC vec4 normalData = texture(sceneNormalTex, TexCoord);
    if (normalData.a > 0.5) {
        FragColor = vec4(1.0);
        return;
    }

    float depth = texture(sceneDepthTex, TexCoord).r;
    if (depth >= 1.0) {
        FragColor = vec4(1.0);
        return;
    }

    vec2 ndc_xy = TexCoord * 2.0 - 1.0;
    float ndc_z = depth * 2.0 - 1.0;
    vec4 clipPos = vec4(ndc_xy, ndc_z, 1.0);
    vec4 worldPos4 = inverseViewProj * clipPos;
    vec3 worldPos = worldPos4.xyz / worldPos4.w;

    float shadow = 1.0;
    if (enableShadows == 1) {
        shadow = min(shadow, sampleShadowMap(shadowMap, worldPos, lightSpaceMatrix, 8));
    }

    if (enableDynamicShadows == 1) {
        float dynShadow = sampleShadowMap(dynamicShadowMap, worldPos, dynamicLightSpaceMatrix, 4);
        shadow = min(shadow, dynShadow);
    }

    FragColor = vec4(shadow, shadow, shadow, 1.0);
}
