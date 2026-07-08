// VK-POST-COMPOSITE-NATIVE-1: conservative Visual-mode composite shader.
// This is the first runtime slice: scene color + exposure + existing visual grade.
#version 450

layout(location = 0) in vec2 TexCoord;
layout(location = 0) out vec4 FragColor;

layout(set = 0, binding = 0) uniform sampler2D sceneTex;

layout(push_constant) uniform Params {
    vec4 p0; // exposure, invW, invH, fxaaEnabled
    vec4 p1; // fxaaSubpix, fxaaEdgeThreshold, fxaaEdgeThresholdMin, unused
    vec4 p2; // viewMode, lowLightGain, lowLightTint.r, lowLightTint.g
    vec4 p3; // lowLightTint.b, unused, unused, unused
} u;

float fxaaLuma(vec3 c) { return sqrt(dot(c, vec3(0.299, 0.587, 0.114))); }

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

    if (range < max(u.p1.z, rangeMax * u.p1.y))
        return rgbM;

    float lumaNW = fxaaLuma(texture(sceneTex, uv + vec2(-rcp.x, -rcp.y)).rgb);
    float lumaNE = fxaaLuma(texture(sceneTex, uv + vec2( rcp.x, -rcp.y)).rgb);
    float lumaSW = fxaaLuma(texture(sceneTex, uv + vec2(-rcp.x,  rcp.y)).rgb);
    float lumaSE = fxaaLuma(texture(sceneTex, uv + vec2( rcp.x,  rcp.y)).rgb);

    vec2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));

    float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * 0.25 * (1.0 - u.p1.x), 1.0 / 128.0);
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

void main() {
    vec2 invSize = vec2(u.p0.y, u.p0.z);
    vec3 color = ((u.p0.w != 0.0)
        ? fxaaFilter(TexCoord, invSize)
        : texture(sceneTex, TexCoord).rgb) * u.p0.x;

    color *= vec3(1.05, 1.01, 0.94);
    float lum = dot(color, vec3(0.299, 0.587, 0.114));
    vec3 warm = vec3(1.05, 1.00, 0.93);
    vec3 cool = vec3(0.95, 0.98, 1.04);
    vec3 grade = mix(cool, warm, smoothstep(0.10, 0.75, lum));
    color *= grade;

    vec2 vdir = TexCoord - vec2(0.5);
    float vdist = length(vdir * vec2(1.0, 0.6));
    float vignette = smoothstep(0.85, 0.25, vdist);
    color *= mix(0.82, 1.0, vignette);

    float sunBias = pow(smoothstep(0.0, 1.0, 1.0 - TexCoord.y), 1.5);
    color = mix(color, color * vec3(1.10, 1.03, 0.92), sunBias * 0.25);

    if (int(u.p2.x + 0.5) == 5) {
        float lum = dot(color, vec3(0.299, 0.587, 0.114));
        float boosted = lum * u.p2.y;
        boosted = boosted / (1.0 + boosted * 0.3);
        vec3 nv = boosted * vec3(u.p2.z, u.p2.w, u.p3.x);
        vec2 vd = TexCoord - vec2(0.5);
        float vdist = length(vd * vec2(1.0, 0.75));
        float nvVignette = smoothstep(0.70, 0.20, vdist);
        color = nv * mix(0.65, 1.0, nvVignette);
    }

    FragColor = vec4(color, 1.0);
}
