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
    float centerAO = texture(ssaoTex, TexCoord).r;

    if (centerDepth >= 1.0) {
        FragColor = 1.0;
        return;
    }

    // 5-tap bilateral Gaussian blur (edge-preserving via depth weight)
    // Weights: 0.06, 0.24, 0.40, 0.24, 0.06
    const float weights[5] = float[](0.06, 0.24, 0.40, 0.24, 0.06);
    const float offsets[5] = float[](-2.0, -1.0, 0.0, 1.0, 2.0);

    vec2 dir = blurHorizontal == 1 ? vec2(texelSize.x, 0.0) : vec2(0.0, texelSize.y);

    float totalAO = 0.0;
    float totalWeight = 0.0;

    for (int i = 0; i < 5; i++) {
        vec2 sampleUV = TexCoord + dir * offsets[i];
        float sampleAO = texture(ssaoTex, sampleUV).r;
        float sampleDepth = texture(sceneDepthTex, sampleUV).r;

        // Depth-based edge detection: reduce weight for depth discontinuities
        float depthDiff = abs(centerDepth - sampleDepth);
        float depthWeight = exp(-depthDiff * 1000.0);  // sharp falloff at depth edges

        float w = weights[i] * depthWeight;
        totalAO += sampleAO * w;
        totalWeight += w;
    }

    FragColor = totalAO / max(totalWeight, 0.001);
}
