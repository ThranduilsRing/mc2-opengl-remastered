//#version 420 (version provided by prefix)

#define PREC highp

#include <include/noise.hglsl>

in vec2 TexCoord;
layout(location = 0) out PREC vec4 FragColor;

uniform sampler2D sceneDepthTex;
uniform sampler2D sceneColorTex;
uniform vec2 sunScreenPos;
uniform float time;

const float density = 1.0;
const float weight = 0.6;
const float decay = 0.96;
const float exposure = 0.4;
const int numSamples = 48;

void main()
{
    vec2 deltaTexCoord = (TexCoord - sunScreenPos) * density / float(numSamples);
    vec2 sampleCoord = TexCoord;
    float illuminationDecay = 1.0;
    vec3 godrayColor = vec3(0.0);

    for (int i = 0; i < numSamples; i++) {
        sampleCoord -= deltaTexCoord;
        if (sampleCoord.x < 0.0 || sampleCoord.x > 1.0 ||
            sampleCoord.y < 0.0 || sampleCoord.y > 1.0) break;

        float depth = texture(sceneDepthTex, sampleCoord).r;

        // Sky pixels are full light sources
        float lightValue = step(0.999, depth);

        // Bright scene pixels also contribute (sun-lit terrain catches light)
        vec3 sceneColor = texture(sceneColorTex, sampleCoord).rgb;
        float sceneBrightness = dot(sceneColor, vec3(0.2126, 0.7152, 0.0722));
        lightValue = max(lightValue, smoothstep(0.6, 1.0, sceneBrightness) * 0.5);

        // Cloud gap modulation
        vec2 noiseCoord = sampleCoord * 6.0 + vec2(time * 0.015, time * 0.008);
        float cloudGap = fbm(noiseCoord, 3) * 0.5 + 0.5;
        cloudGap = smoothstep(0.25, 0.65, cloudGap);
        lightValue *= cloudGap;

        lightValue *= illuminationDecay * weight;
        godrayColor += vec3(lightValue);
        illuminationDecay *= decay;
    }

    vec3 sunTint = vec3(1.0, 0.92, 0.75);
    godrayColor *= sunTint * exposure;

    // Fade when sun near screen edge or behind camera
    float edgeDist = length(sunScreenPos - vec2(0.5));
    float edgeFade = 1.0 - smoothstep(0.5, 1.0, edgeDist);
    godrayColor *= max(edgeFade, 0.0);

    FragColor = vec4(godrayColor, 1.0);
}
