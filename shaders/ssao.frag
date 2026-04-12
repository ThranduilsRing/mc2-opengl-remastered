//#version 420 (version provided by prefix)

#define PREC highp

in vec2 TexCoord;
layout(location = 0) out PREC float FragColor;  // single-channel AO

uniform sampler2D sceneDepthTex;   // unit 0: scene depth
uniform sampler2D sceneNormalTex;  // unit 1: normal buffer (rgb=normal*0.5+0.5)
uniform sampler2D noiseTex;        // unit 2: 4x4 rotation noise

uniform mat4 viewProj;            // world -> clip (terrainMVP)
uniform mat4 inverseViewProj;     // clip -> world
uniform vec2 screenSize;          // full-res viewport size
uniform float ssaoRadius;         // world-space AO radius
uniform float ssaoBias;           // depth comparison bias
uniform float ssaoPower;          // contrast exponent

// 16-sample hemisphere kernel: cosine-weighted, distributed from near to far
const int KERNEL_SIZE = 16;
const vec3 ssaoKernel[16] = vec3[](
    vec3( 0.038, 0.051, 0.018),
    vec3(-0.045, 0.035, 0.042),
    vec3( 0.084,-0.027, 0.053),
    vec3(-0.023,-0.088, 0.069),
    vec3( 0.106, 0.062, 0.088),
    vec3(-0.015, 0.124, 0.103),
    vec3( 0.138,-0.098, 0.135),
    vec3(-0.166, 0.018, 0.142),
    vec3( 0.024, 0.193, 0.168),
    vec3( 0.214,-0.065, 0.198),
    vec3(-0.188, 0.176, 0.225),
    vec3( 0.085,-0.268, 0.258),
    vec3( 0.295, 0.142, 0.288),
    vec3(-0.310,-0.092, 0.320),
    vec3( 0.130, 0.348, 0.355),
    vec3(-0.058,-0.390, 0.410)
);

vec3 reconstructWorldPos(vec2 uv, float depth)
{
    vec2 ndc_xy = uv * 2.0 - 1.0;
    float ndc_z = depth * 2.0 - 1.0;
    vec4 wp = inverseViewProj * vec4(ndc_xy, ndc_z, 1.0);
    return wp.xyz / wp.w;
}

void main()
{
    float depth = texture(sceneDepthTex, TexCoord).r;
    if (depth >= 1.0) {
        FragColor = 1.0;
        return;
    }

    vec3 worldPos = reconstructWorldPos(TexCoord, depth);
    vec3 normal = normalize(texture(sceneNormalTex, TexCoord).rgb * 2.0 - 1.0);

    // Random rotation from 4x4 noise texture
    vec2 noiseScale = screenSize / 4.0;
    vec3 randomVec = normalize(texture(noiseTex, TexCoord * noiseScale).rgb * 2.0 - 1.0);

    // Gram-Schmidt: build TBN from normal + random vector
    vec3 tangent = normalize(randomVec - normal * dot(randomVec, normal));
    vec3 bitangent = cross(normal, tangent);
    mat3 TBN = mat3(tangent, bitangent, normal);

    float occlusion = 0.0;
    for (int i = 0; i < KERNEL_SIZE; i++) {
        // Sample position in world space (hemisphere above surface)
        vec3 samplePos = worldPos + TBN * ssaoKernel[i] * ssaoRadius;

        // Project sample to screen space
        vec4 sampleClip = viewProj * vec4(samplePos, 1.0);
        vec3 sampleNDC = sampleClip.xyz / sampleClip.w;
        vec2 sampleUV = sampleNDC.xy * 0.5 + 0.5;
        float sampleProjDepth = sampleNDC.z * 0.5 + 0.5;  // sample's depth if unoccluded

        // Read actual depth at that screen position from buffer
        float bufferDepth = texture(sceneDepthTex, sampleUV).r;

        // Standard SSAO comparison:
        // If buffer depth < sample projected depth → geometry is CLOSER than sample
        // → sample is inside/behind geometry → pixel is occluded
        float depthDiff = sampleProjDepth - bufferDepth;

        // Range check: reject depth discontinuities (object edges vs terrain behind)
        // Only count occlusion when depth difference is small (actual nearby geometry)
        // Large depth jumps = different surfaces, not real occlusion
        float maxDepthRange = 0.005;  // ~0.5% of depth range
        float rangeCheck = 1.0 - smoothstep(0.0, maxDepthRange, abs(depthDiff));

        if (depthDiff > ssaoBias * 0.0001 && depthDiff < maxDepthRange) {
            occlusion += rangeCheck;
        }
    }

    occlusion = 1.0 - (occlusion / float(KERNEL_SIZE));
    FragColor = pow(clamp(occlusion, 0.0, 1.0), ssaoPower);
}
