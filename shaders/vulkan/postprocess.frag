// VK-POST-COMPOSITE-NATIVE-1: Vulkan port of shaders/postprocess.frag.
// Runtime hook is intentionally separate; this file establishes a baked SPIR-V
// asset with explicit bindings for the future native composite pass.
#version 450

layout(location = 0) in vec2 TexCoord;
layout(location = 0) out vec4 FragColor;

layout(set = 0, binding = 0) uniform sampler2D sceneTex;
layout(set = 0, binding = 1) uniform usampler2D u_objectIdTex;

layout(std140, set = 0, binding = 2) uniform PostCompositeParams {
    vec4 p0; // exposure, invW, invH, viewMode
    vec4 p1; // fxaaEnabled, fxaaSubpix, fxaaEdgeThreshold, fxaaEdgeThresholdMin
    vec4 p2; // engineIdxBase, lowLightGain, unused, unused
    vec4 p3; // lowLightTint.rgb, unused
} u;

float fxaaLuma(vec3 c) { return sqrt(dot(c, vec3(0.299, 0.587, 0.114))); }

vec3 fxaaFilter(vec2 uv, vec2 rcp) {
    vec3 rgbM = texture(sceneTex, uv).rgb;
    float lumaM = fxaaLuma(rgbM);
    float lumaN = fxaaLuma(texture(sceneTex, uv + vec2(0.0, -rcp.y)).rgb);
    float lumaS = fxaaLuma(texture(sceneTex, uv + vec2(0.0,  rcp.y)).rgb);
    float lumaE = fxaaLuma(texture(sceneTex, uv + vec2( rcp.x, 0.0)).rgb);
    float lumaW = fxaaLuma(texture(sceneTex, uv + vec2(-rcp.x, 0.0)).rgb);

    float rangeMin = min(lumaM, min(min(lumaN, lumaS), min(lumaE, lumaW)));
    float rangeMax = max(lumaM, max(max(lumaN, lumaS), max(lumaE, lumaW)));
    float range = rangeMax - rangeMin;

    if (range < max(u.p1.w, rangeMax * u.p1.z))
        return rgbM;

    float lumaNW = fxaaLuma(texture(sceneTex, uv + vec2(-rcp.x, -rcp.y)).rgb);
    float lumaNE = fxaaLuma(texture(sceneTex, uv + vec2( rcp.x, -rcp.y)).rgb);
    float lumaSW = fxaaLuma(texture(sceneTex, uv + vec2(-rcp.x,  rcp.y)).rgb);
    float lumaSE = fxaaLuma(texture(sceneTex, uv + vec2( rcp.x,  rcp.y)).rgb);

    vec2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));

    float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * 0.25 * (1.0 - u.p1.y), 1.0 / 128.0);
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
    float exposure = u.p0.x;
    vec2 inverseScreenSize = u.p0.yz;
    int viewMode = int(u.p0.w);
    int fxaaEnabled = int(u.p1.x);
    int engineIdxBase = int(u.p2.x);
    float lowLightGain = u.p2.y;
    vec3 lowLightTint = u.p3.rgb;

    vec3 color = ((fxaaEnabled != 0)
        ? fxaaFilter(TexCoord, inverseScreenSize)
        : texture(sceneTex, TexCoord).rgb) * exposure;

    {
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
    }

    FragColor = vec4(color, 1.0);

    if (viewMode == 1) {
        uint id = texture(u_objectIdTex, TexCoord).r;
        if (id == 0u) {
            FragColor = vec4(0.1, 0.1, 0.1, 1.0);
        } else {
            float r = float((id       ) & 0xFFu) / 255.0;
            float g = float((id >>  8u) & 0xFFu) / 255.0;
            float b = float((id >> 16u) & 0xFFu) / 255.0;
            FragColor = vec4(r, g, b, 1.0);
        }
    } else if (viewMode == 3) {
        float t = clamp(dot(color, vec3(0.299, 0.587, 0.114)), 0.0, 1.0);
        if (engineIdxBase > 0) {
            uint idx = texture(u_objectIdTex, TexCoord).r & 0xFFFFFu;
            if (idx >= uint(engineIdxBase))
                t = max(t, 0.9);
        }
        vec3 c0 = vec3(0.00, 0.00, 0.10);
        vec3 c1 = vec3(0.30, 0.00, 0.40);
        vec3 c2 = vec3(0.85, 0.10, 0.05);
        vec3 c3 = vec3(1.00, 0.55, 0.00);
        vec3 c4 = vec3(1.00, 0.95, 0.40);
        vec3 c5 = vec3(1.00, 1.00, 1.00);
        vec3 heat;
        if      (t < 0.2) heat = mix(c0, c1, t / 0.2);
        else if (t < 0.4) heat = mix(c1, c2, (t - 0.2) / 0.2);
        else if (t < 0.6) heat = mix(c2, c3, (t - 0.4) / 0.2);
        else if (t < 0.8) heat = mix(c3, c4, (t - 0.6) / 0.2);
        else              heat = mix(c4, c5, (t - 0.8) / 0.2);
        FragColor = vec4(heat, 1.0);
    } else if (viewMode == 5) {
        float lum = dot(color, vec3(0.299, 0.587, 0.114));
        float boosted = lum * lowLightGain;
        boosted = boosted / (1.0 + boosted * 0.3);
        vec3 nv = boosted * lowLightTint;
        vec2 vd = TexCoord - vec2(0.5);
        float vdist = length(vd * vec2(1.0, 0.75));
        float nvVignette = smoothstep(0.70, 0.20, vdist);
        nv *= mix(0.65, 1.0, nvVignette);
        FragColor = vec4(nv, 1.0);
    }
}
