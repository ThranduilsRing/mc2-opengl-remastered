// VK-POST-SCREENFX-NATIVE-1 (rung 8.3) -- Vulkan port of shaders/shoreline.frag
// (fullscreen shoreline-foam pass, multiplicative DST_COLOR/ZERO -- values >1
// BRIGHTEN water pixels at the land/water boundary).
//
// MATH is a VERBATIM port (proven offline by tools/glsl_port_diff.py pairs
// shorelineNoise -- vs shaders/include/noise.hglsl, which the GL frag
// #includes -- and shorelineMain vs shaders/shoreline.frag). ONLY the
// uniform/sampler plumbing changed, with #define aliases keeping the ported
// block text byte-identical:
//   set=0,binding=0  sceneDepthTex   -> sampler2D (declared by the GL source
//                    but never sampled; kept + bound for interface parity)
//   set=0,binding=1  sceneNormalTex  -> sampler2D (SHARED GBuffer1 -- the .a
//                    water tag 0.15..0.35 is what the pass keys on)
//   set=0,binding=2  ShorelineFxParams -> std140 UBO (screenSize, time).
//
// Convention matches ssao.frag: TexCoord = interpolated [0,1] screen UV from
// the fullscreen triangle; shared images are the very texels GL samples.
#version 450

#define PREC highp

layout(location = 0) in  vec2 TexCoord;
layout(location = 0) out PREC vec4 FragColor;

layout(set = 0, binding = 0) uniform sampler2D sceneDepthTex;
layout(set = 0, binding = 1) uniform sampler2D sceneNormalTex;

// std140 offsets (cross-checked offline by tools/check_ubo_layout.py label
// screenfx_shore against the CPU POD in vulkan_post_screenfx_native.cpp):
//   fxScreenSize @ 0  (vec2)
//   fxTime       @ 8
layout(set = 0, binding = 2, std140) uniform ShorelineFxParams {
    vec2  fxScreenSize;
    float fxTime;
} u;

// Aliases so the ported GL block below stays textually VERBATIM.
#define screenSize u.fxScreenSize
#define time       u.fxTime

//PORT-BEGIN:shorelineNoise
PREC vec3 mod289_3(PREC vec3 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
PREC vec2 mod289_2(PREC vec2 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
PREC vec3 permute(PREC vec3 x) { return mod289_3(((x * 34.0) + 1.0) * x); }

PREC float snoise(PREC vec2 v) {
    const PREC vec4 C = vec4(0.211324865405187,   // (3.0 - sqrt(3.0)) / 6.0
                             0.366025403784439,   // 0.5 * (sqrt(3.0) - 1.0)
                            -0.577350269189626,   // -1.0 + 2.0 * C.x
                             0.024390243902439);  // 1.0 / 41.0

    PREC vec2 i  = floor(v + dot(v, C.yy));
    PREC vec2 x0 = v - i + dot(i, C.xx);

    PREC vec2 i1 = (x0.x > x0.y) ? vec2(1.0, 0.0) : vec2(0.0, 1.0);
    PREC vec4 x12 = x0.xyxy + C.xxzz;
    x12.xy -= i1;

    i = mod289_2(i);
    PREC vec3 p = permute(permute(i.y + vec3(0.0, i1.y, 1.0))
                                      + i.x + vec3(0.0, i1.x, 1.0));

    PREC vec3 m = max(0.5 - vec3(dot(x0, x0), dot(x12.xy, x12.xy), dot(x12.zw, x12.zw)), 0.0);
    m = m * m;
    m = m * m;

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

// Fractal Brownian Motion — layered noise for natural-looking patterns
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
//PORT-END:shorelineNoise

//PORT-BEGIN:shorelineMain
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
//PORT-END:shorelineMain
