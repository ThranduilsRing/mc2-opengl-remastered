#version 450
layout(location = 0) in vec2 TexCoord;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D sceneTex;
layout(push_constant) uniform AAParams { vec4 metrics; } aa;
float fxaaLuma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

vec3 fxaaFilter(vec2 uv, vec2 rcp) {
    vec3  rgbM  = texture(sceneTex, uv).rgb;
    float lumaM = fxaaLuma(rgbM);
    float lumaN = fxaaLuma(texture(sceneTex, uv + vec2(0.0, -rcp.y)).rgb);
    float lumaS = fxaaLuma(texture(sceneTex, uv + vec2(0.0,  rcp.y)).rgb);
    float lumaE = fxaaLuma(texture(sceneTex, uv + vec2( rcp.x, 0.0)).rgb);
    float lumaW = fxaaLuma(texture(sceneTex, uv + vec2(-rcp.x, 0.0)).rgb);

    float rangeMin = min(lumaM, min(min(lumaN, lumaS), min(lumaE, lumaW)));
    float rangeMax = max(lumaM, max(max(lumaN, lumaS), max(lumaE, lumaW)));
    float range    = rangeMax - rangeMin;

    if (range < max(0.0312, rangeMax * 0.125))
        return rgbM;

    float lumaNW = fxaaLuma(texture(sceneTex, uv + vec2(-rcp.x, -rcp.y)).rgb);
    float lumaNE = fxaaLuma(texture(sceneTex, uv + vec2( rcp.x, -rcp.y)).rgb);
    float lumaSW = fxaaLuma(texture(sceneTex, uv + vec2(-rcp.x,  rcp.y)).rgb);
    float lumaSE = fxaaLuma(texture(sceneTex, uv + vec2( rcp.x,  rcp.y)).rgb);

    vec2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));

    float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * 0.25 * (1.0 - 0.25), 1.0 / 128.0);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    const float SPAN_MAX = 8.0;
    dir = clamp(dir * rcpDirMin, vec2(-SPAN_MAX), vec2(SPAN_MAX)) * rcp;

    vec3 rgbA = 0.5 * (
        texture(sceneTex, uv + dir * (1.0 / 3.0 - 0.5)).rgb +
        texture(sceneTex, uv + dir * (2.0 / 3.0 - 0.5)).rgb);
    vec3 rgbB = rgbA * 0.5 + 0.25 * (
        texture(sceneTex, uv + dir * -0.5).rgb +
        texture(sceneTex, uv + dir *  0.5).rgb);

    float lumaB = fxaaLuma(rgbB);
    return (lumaB < rangeMin || lumaB > rangeMax) ? rgbA : rgbB;
}

void main() { outColor = vec4(fxaaFilter(TexCoord, aa.metrics.xy), 1.0); }
