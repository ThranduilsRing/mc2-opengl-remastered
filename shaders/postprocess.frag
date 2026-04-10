//#version 420 (version provided by prefix)

in vec2 TexCoord;
layout(location = 0) out vec4 FragColor;

uniform sampler2D sceneTex;   // unit 0: scene
uniform sampler2D bloomTex;   // unit 1: blurred bloom
uniform float exposure;       // exposure multiplier (default 1.0)
uniform int enableBloom;      // 0 = off, 1 = on
uniform int enableFXAA;       // 0 = off, 1 = on
uniform int enableTonemap;    // 0 = off (passthrough), 1 = on
uniform float bloomIntensity; // bloom mix strength
uniform vec2 inverseScreenSize; // 1/width, 1/height

// ACES Filmic tonemapping (Krzysztof Narkowicz fit)
// Note: designed for linear HDR input. Our pipeline is sRGB so this acts
// as a gentle contrast/color curve rather than true HDR compression.
vec3 ACESFilm(vec3 x)
{
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

// Tonemap a single sample (used by FXAA neighbor reads)
vec3 tonemapSample(vec3 color)
{
    if (enableTonemap == 1)
        return ACESFilm(color * exposure);
    return color * exposure;
}

// FXAA 3.11 simplified (Timothy Lottes algorithm)
vec3 applyFXAA_LDR(vec2 uv, vec2 invScreenSize)
{
    float FXAA_SPAN_MAX = 8.0;
    float FXAA_REDUCE_MUL = 1.0 / 8.0;
    float FXAA_REDUCE_MIN = 1.0 / 128.0;

    vec3 rgbNW = tonemapSample(texture(sceneTex, uv + vec2(-1.0, -1.0) * invScreenSize).rgb);
    vec3 rgbNE = tonemapSample(texture(sceneTex, uv + vec2( 1.0, -1.0) * invScreenSize).rgb);
    vec3 rgbSW = tonemapSample(texture(sceneTex, uv + vec2(-1.0,  1.0) * invScreenSize).rgb);
    vec3 rgbSE = tonemapSample(texture(sceneTex, uv + vec2( 1.0,  1.0) * invScreenSize).rgb);
    vec3 rgbM  = tonemapSample(texture(sceneTex, uv).rgb);

    vec3 luma = vec3(0.299, 0.587, 0.114);
    float lumaNW = dot(rgbNW, luma);
    float lumaNE = dot(rgbNE, luma);
    float lumaSW = dot(rgbSW, luma);
    float lumaSE = dot(rgbSE, luma);
    float lumaM  = dot(rgbM,  luma);

    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));

    vec2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));

    float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * 0.25 * FXAA_REDUCE_MUL, FXAA_REDUCE_MIN);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = min(vec2(FXAA_SPAN_MAX), max(vec2(-FXAA_SPAN_MAX), dir * rcpDirMin)) * invScreenSize;

    vec3 rgbA = 0.5 * (
        tonemapSample(texture(sceneTex, uv + dir * (1.0/3.0 - 0.5)).rgb) +
        tonemapSample(texture(sceneTex, uv + dir * (2.0/3.0 - 0.5)).rgb));
    vec3 rgbB = rgbA * 0.5 + 0.25 * (
        tonemapSample(texture(sceneTex, uv + dir * -0.5).rgb) +
        tonemapSample(texture(sceneTex, uv + dir *  0.5).rgb));

    float lumaB = dot(rgbB, luma);
    if (lumaB < lumaMin || lumaB > lumaMax)
        return rgbA;
    else
        return rgbB;
}

void main()
{
    vec3 color;

    // FXAA first — operates on scene texture neighbors for edge detection
    if (enableFXAA == 1) {
        color = applyFXAA_LDR(TexCoord, inverseScreenSize);
    } else {
        // Tonemapping (no gamma — pipeline is already sRGB)
        color = tonemapSample(texture(sceneTex, TexCoord).rgb);
    }

    // Add bloom AFTER FXAA (bloom is soft glow, doesn't need AA)
    if (enableBloom == 1) {
        vec3 bloom = texture(bloomTex, TexCoord).rgb;
        color += bloom * bloomIntensity;
    }

    FragColor = vec4(color, 1.0);
}
