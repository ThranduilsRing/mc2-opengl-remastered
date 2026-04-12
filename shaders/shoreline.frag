//#version 420 (version provided by prefix)

#define PREC highp

#include <include/noise.hglsl>

in vec2 TexCoord;
layout(location = 0) out PREC vec4 FragColor;

uniform sampler2D sceneDepthTex;
uniform sampler2D sceneNormalTex;
uniform vec2 screenSize;
uniform float time;

void main()
{
    PREC vec4 normalData = texture(sceneNormalTex, TexCoord);
    float myAlpha = normalData.a;

    // Only process water pixels (alpha ~0.25 from terrain or water overlay)
    bool isWater = (myAlpha > 0.15 && myAlpha < 0.35);
    if (!isWater) {
        FragColor = vec4(1.0);  // multiplicative identity — no change
        return;
    }

    vec2 texelSize = 1.0 / screenSize;

    // Sample 8 neighbors at multiple radii to detect land/water boundary
    int landNeighbors = 0;
    const vec2 offsets[8] = vec2[](
        vec2(-1, 0), vec2(1, 0), vec2(0, -1), vec2(0, 1),
        vec2(-1, -1), vec2(1, -1), vec2(-1, 1), vec2(1, 1)
    );

    for (int r = 1; r <= 3; r++) {
        for (int i = 0; i < 8; i++) {
            vec2 sampleUV = TexCoord + offsets[i] * texelSize * float(r) * 2.0;
            float neighborAlpha = texture(sceneNormalTex, sampleUV).a;
            if (neighborAlpha > 0.5) landNeighbors++;  // terrain
        }
    }

    if (landNeighbors == 0) {
        FragColor = vec4(1.0);  // no shore
        return;
    }

    // Shore intensity based on land neighbor count
    float shoreIntensity = float(landNeighbors) / 24.0;
    shoreIntensity = smoothstep(0.1, 0.5, shoreIntensity);

    // Animated foam via FBM
    vec2 foamCoord = TexCoord * screenSize * 0.02;
    float foam = fbm(foamCoord + vec2(time * 0.1, time * 0.05), 3);
    foam = smoothstep(0.0, 0.4, foam * 0.5 + 0.5);

    // Pulsing wave edge
    float wave = sin(time * 2.0 + length(TexCoord - vec2(0.5)) * 50.0) * 0.5 + 0.5;
    foam = mix(foam, 1.0, wave * 0.2);

    // Brighten water at shore (multiplicative > 1.0)
    float foamBrightness = shoreIntensity * foam * 0.4;
    FragColor = vec4(1.0 + foamBrightness, 1.0 + foamBrightness, 1.0 + foamBrightness * 0.9, 1.0);
}
