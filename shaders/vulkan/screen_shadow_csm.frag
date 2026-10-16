// VK CSM port of shaders/shadow_screen.frag normal path.
// Requires same-frame VK CSM array/full-map views from vulkan_shadow_island_live.
#version 450
#extension GL_GOOGLE_include_directive : require
#include "../include/axis.hglsl"

#define PREC highp
#define MC2_SHADOW_CSM_MAX 3

bool rc_pixelHandlesOwnShadow(PREC vec4 gbuf1) {
    return gbuf1.a > 0.5;
}

bool rc_isStaticPropNoSelfShadow(PREC vec4 gbuf1) {
    return gbuf1.a > 0.1 && gbuf1.a <= 0.5;
}

// Alpha-test cutout props (trees, fences): excluded from the prop receive opt-in because the
// shadow depth pass writes them as solid slabs.
bool rc_isAlphaTestProp(PREC vec4 gbuf1) {
    return gbuf1.a > 0.1 && gbuf1.a <= 0.22;
}

layout(location = 0) in  vec2 TexCoord;
layout(location = 0) out PREC vec4 FragColor;

layout(set = 0, binding = 0) uniform sampler2D sceneDepthTex;
layout(set = 0, binding = 1) uniform sampler2D sceneNormalTex;
layout(set = 0, binding = 2) uniform sampler2DShadow shadowMap;
layout(set = 0, binding = 4) uniform sampler2DArrayShadow dynamicShadowArray;
layout(set = 0, binding = 5) uniform sampler2DShadow dynamicFullMapShadow;
layout(set = 0, binding = 6) uniform sampler2DArray dynamicShadowArrayRaw;

layout(std140, set = 0, binding = 3) uniform ScreenShadowCsmParams {
    mat4 inverseViewProj;
    mat4 lightSpaceMatrix;
    mat4 dynamicCascadeMatrices[MC2_SHADOW_CSM_MAX];
    vec4 dynamicCascadeTexelWorld;
    vec2 screenSize;
    int  enableShadows;
    int  enableDynamicShadows;
    int  dynamicCsmCount;
    int  debugMode;
    float shadowSoftness;
    float csmDepthSpan;
    float dynamicFullMapTexelWorld;
    float objNormalBiasScale;
    vec4 lightDir;
    float mechSoft;
    int   propShadowReceive;
} u;

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

    float currentDepth = projCoords.z - 0.003;
    float angle = 6.2831853 * fract(sin(dot(worldPos.xz, vec2(12.9898, 78.233))) * 43758.5453);
    float ca = cos(angle), sa = sin(angle);
    mat2 rot = mat2(ca, sa, -sa, ca);
    vec2 texelSize = 1.0 / vec2(textureSize(smap, 0));
    float radius = max(u.shadowSoftness, 0.5);
    float shadow = 0.0;
    int taps = clamp(numTaps, 1, 8);
    for (int i = 0; i < taps; i++) {
        vec2 offset = rot * poissonDisk[i] * radius * texelSize;
        shadow += texture(smap, vec3(projCoords.xy + offset, currentDepth));
    }
    return mix(0.4, 1.0, shadow / float(taps));
}

float sampleDynamicShadow(vec3 worldPos, vec3 N, float softScale, float shadowFloor)
{
    float NdotL = max(dot(N, u.lightDir.xyz), 0.0);
    if (NdotL < 0.05) return 1.0;
    int count = clamp(u.dynamicCsmCount, 1, MC2_SHADOW_CSM_MAX);
    for (int c = 0; c < count; ++c) {
        bool isFullMap = (c == count - 1);
        float cTexelWorld = isFullMap ? u.dynamicFullMapTexelWorld
                                      : u.dynamicCascadeTexelWorld[c];
        vec3 samplePos = worldPos + N * (u.objNormalBiasScale * cTexelWorld * NdotL);
        vec4 lsPos = u.dynamicCascadeMatrices[c] * vec4(samplePos, 1.0);
        vec3 projCoords = lsPos.xyz / lsPos.w;
        projCoords.xy = projCoords.xy * 0.5 + 0.5;
        if (projCoords.z > 1.0 || projCoords.z < 0.0) continue;
        if (projCoords.xy != clamp(projCoords.xy, 0.0, 1.0)) continue;

        float texelBias = 1.5 * cTexelWorld / max(u.csmDepthSpan, 1.0);
        float currentDepth = projCoords.z - (0.0012 + texelBias + 0.0005 * (1.0 - NdotL));
        float angle = 6.2831853 * fract(sin(dot(worldPos.xz, vec2(12.9898, 78.233))) * 43758.5453);
        float ca = cos(angle), sa = sin(angle);
        mat2 rot = mat2(ca, sa, -sa, ca);
        vec2 texelSize = isFullMap
            ? (1.0 / vec2(textureSize(dynamicFullMapShadow, 0).xy))
            : (1.0 / vec2(textureSize(dynamicShadowArray, 0).xy));
        float radius = max(u.shadowSoftness, 0.5) * (1.0 + 2.0 * softScale);
        int taps = (softScale > 0.0) ? 8 : 4;
        float shadow = 0.0;
        for (int i = 0; i < taps; i++) {
            vec2 offset = rot * poissonDisk[i] * radius * texelSize;
            shadow += isFullMap
                ? texture(dynamicFullMapShadow, vec3(projCoords.xy + offset, currentDepth))
                : texture(dynamicShadowArray, vec4(projCoords.xy + offset, float(c), currentDepth));
        }
        shadow /= float(taps);
        if (softScale > 0.0) {
            float band = 0.15 + 0.30 * softScale;
            shadow = mix(1.0, shadow, smoothstep(0.0, band, NdotL));
        }
        return mix(shadowFloor, 1.0, shadow);
    }
    return 1.0;
}

vec3 reconstructWorldPos(vec2 uv, float depth)
{
    vec2 ndc_xy = uv * 2.0 - 1.0;
    vec4 worldPos4 = u.inverseViewProj * vec4(ndc_xy, depth, 1.0);
    return worldPos4.xyz / worldPos4.w;
}

void main()
{
    PREC vec4 normalData = texture(sceneNormalTex, TexCoord);
    float depth = texture(sceneDepthTex, TexCoord).r;
    if (depth <= 0.0001 || rc_pixelHandlesOwnShadow(normalData)) {
        FragColor = vec4(1.0);
        return;
    }

    vec3 worldPos = reconstructWorldPos(TexCoord, depth);
    vec3 objN_stuff = normalize(normalData.rgb * 2.0 - 1.0);
    vec3 objN = mc2ToRender(objN_stuff);
    bool isStaticProp = rc_isStaticPropNoSelfShadow(normalData);

    float shadow = 1.0;
    if (u.enableShadows == 1)
        shadow = min(shadow, sampleShadowMap(shadowMap, worldPos, u.lightSpaceMatrix, 8));
    if (u.enableDynamicShadows == 1 && !isStaticProp)
        shadow = min(shadow, sampleDynamicShadow(worldPos, objN, u.mechSoft, 0.55));
    else if (u.enableDynamicShadows == 1 && u.propShadowReceive != 0 && !rc_isAlphaTestProp(normalData))
        shadow = min(shadow, sampleDynamicShadow(worldPos, objN, u.mechSoft, 0.4));
    FragColor = vec4(shadow, shadow, shadow, 1.0);
    if (isStaticProp && u.propShadowReceive != 0)
    {
        vec4 lp = u.dynamicCascadeMatrices[0] * vec4(worldPos, 1.0);
        vec3 pc = lp.xyz / lp.w; pc.xy = pc.xy * 0.5 + 0.5;
        float stored = texture(dynamicShadowArrayRaw, vec3(pc.xy, 0.0)).r;
        FragColor = vec4(step(0.5, fract(stored * 200.0)),
                         stored < pc.z ? 1.0 : 0.0,
                         stored < pc.z - 0.0012 ? 1.0 : 0.0, 1.0);
    }
}
