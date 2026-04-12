//#version 420 (version provided by prefix)

#define PREC highp

in vec2 TexCoord;
layout(location = 0) out PREC float FragColor;

uniform sampler2D ssaoTex;         // unit 0: raw SSAO (single channel)
uniform sampler2D sceneDepthTex;   // unit 1: scene depth (for edge-preserving)
uniform vec2 texelSize;            // 1/width, 1/height of SSAO texture
uniform int blurHorizontal;        // 1 = horizontal, 0 = vertical

void main()
{
    float centerDepth = texture(sceneDepthTex, TexCoord).r;

    if (centerDepth >= 1.0) {
        FragColor = 1.0;
        return;
    }

    // 9-tap bilateral Gaussian blur (edge-preserving via depth weight)
    const float weights[9] = float[](0.028, 0.066, 0.124, 0.179, 0.206, 0.179, 0.124, 0.066, 0.028);
    const float offsets[9] = float[](-4.0, -3.0, -2.0, -1.0, 0.0, 1.0, 2.0, 3.0, 4.0);

    vec2 dir = blurHorizontal == 1 ? vec2(texelSize.x, 0.0) : vec2(0.0, texelSize.y);

    float totalAO = 0.0;
    float totalWeight = 0.0;

    for (int i = 0; i < 9; i++) {
        vec2 sampleUV = TexCoord + dir * offsets[i];
        float sampleAO = texture(ssaoTex, sampleUV).r;
        float sampleDepth = texture(sceneDepthTex, sampleUV).r;

        // Soft depth gating: preserve edges but blur smoothly across flat terrain
        float depthDiff = abs(centerDepth - sampleDepth);
        float depthWeight = exp(-depthDiff * 200.0);

        float w = weights[i] * depthWeight;
        totalAO += sampleAO * w;
        totalWeight += w;
    }

    FragColor = totalAO / max(totalWeight, 0.001);
}
