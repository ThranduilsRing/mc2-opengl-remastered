// VK-POST-COMPOSITE-NATIVE-1: conservative Visual-mode composite shader.
// This is the first runtime slice: scene color + exposure + existing visual grade.
#version 450

layout(location = 0) in vec2 TexCoord;
layout(location = 0) out vec4 FragColor;

layout(set = 0, binding = 0) uniform sampler2D sceneTex;
layout(set = 0, binding = 1) uniform usampler2D objectIdTex;

layout(push_constant) uniform Params {
    vec4 p0; // exposure, inverse source extent, cubic upscale
    vec4 p1; // encoded reconstruction, reserved.yz, casEnabled
    vec4 p2; // viewMode, lowLightGain, lowLightTint.r, lowLightTint.g
    vec4 p3; // lowLightTint.b, engineIdxBase, casSharpness, tonemapMode
    vec4 p4; // grade.rgb, vignette strength
    vec4 p5; // shadows.rgb, top warmth strength
    vec4 p6; // highlights.rgb, reserved
    vec4 p7; // top warmth.rgb, reserved
} u;

// Profile output is neutral by default. CAS grades each tap consistently.
vec3 gradeSample(vec3 sampleColor, vec2 uv) {
    if (u.p1.x != 0.0) return sampleColor;
    vec3 color = sampleColor * u.p0.x;

    color *= u.p4.rgb;
    float lum = dot(color, vec3(0.299, 0.587, 0.114));
    vec3 warm = u.p6.rgb;
    vec3 cool = u.p5.rgb;
    vec3 grade = mix(cool, warm, smoothstep(0.10, 0.75, lum));
    color *= grade;

    vec2 vdir = uv - vec2(0.5);
    float vdist = length(vdir * vec2(1.0, 0.6));
    float vignette = 1.0 - smoothstep(0.25, 0.85, vdist);
    color *= mix(1.0 - u.p4.w, 1.0, vignette);

    float sunBias = pow(smoothstep(0.0, 1.0, 1.0 - uv.y), 1.5);
    return mix(color, color * u.p7.rgb, sunBias * u.p5.w);
}

vec3 gradeColor(vec2 uv) { return gradeSample(texture(sceneTex, uv).rgb, uv); }

// Separable Catmull-Rom reconstruction: merge the two positive middle weights
// with bilinear filtering, reducing sixteen source texels to nine fetches.
vec3 upscaleColor(vec2 uv) {
    vec2 size = vec2(textureSize(sceneTex, 0));
    vec2 p = uv * size - 0.5;
    vec2 base = floor(p), f = p - base;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2;
    vec2 a = (base - 0.5) / size;
    vec2 b = (base + 0.5 + w2 / w12) / size;
    vec2 c = (base + 2.5) / size;
    vec3 color = texture(sceneTex, vec2(a.x,a.y)).rgb * w0.x * w0.y
               + texture(sceneTex, vec2(b.x,a.y)).rgb * w12.x * w0.y
               + texture(sceneTex, vec2(c.x,a.y)).rgb * w3.x * w0.y
               + texture(sceneTex, vec2(a.x,b.y)).rgb * w0.x * w12.y
               + texture(sceneTex, vec2(b.x,b.y)).rgb * w12.x * w12.y
               + texture(sceneTex, vec2(c.x,b.y)).rgb * w3.x * w12.y
               + texture(sceneTex, vec2(a.x,c.y)).rgb * w0.x * w3.y
               + texture(sceneTex, vec2(b.x,c.y)).rgb * w12.x * w3.y
               + texture(sceneTex, vec2(c.x,c.y)).rgb * w3.x * w3.y;
    return gradeSample(max(color, vec3(0.0)), uv);
}

vec3 reconstructedColor(vec2 uv) {
    return u.p0.w != 0.0 ? upscaleColor(uv) : gradeColor(uv);
}

// Options.cfg PostCAS: contrast adaptive sharpening (AMD FidelityFX CAS) on the graded colour. Adaptive weight from the local min/max of the
// 3x3 neighbourhood, blended over the diamond ring; PostCASSharpness 0..1 maps to the AMD peak -1/8 .. -1/5.
vec3 casFilter(vec2 uv) {
    vec2 rcp = vec2(u.p0.y, u.p0.z);
    vec3 a = reconstructedColor(uv + vec2(-rcp.x, -rcp.y));
    vec3 b = reconstructedColor(uv + vec2( 0.0,   -rcp.y));
    vec3 c = reconstructedColor(uv + vec2( rcp.x, -rcp.y));
    vec3 d = reconstructedColor(uv + vec2(-rcp.x,  0.0));
    vec3 e = reconstructedColor(uv);
    vec3 f = reconstructedColor(uv + vec2( rcp.x,  0.0));
    vec3 g = reconstructedColor(uv + vec2(-rcp.x,  rcp.y));
    vec3 h = reconstructedColor(uv + vec2( 0.0,    rcp.y));
    vec3 i = reconstructedColor(uv + vec2( rcp.x,  rcp.y));

    vec3 mnRing = min(min(min(d, e), min(f, b)), h);
    vec3 mnCorner = min(min(a, c), min(g, i));
    vec3 mn = mnRing + min(mnRing, mnCorner);

    vec3 mxRing = max(max(max(d, e), max(f, b)), h);
    vec3 mxCorner = max(max(a, c), max(g, i));
    vec3 mx = mxRing + max(mxRing, mxCorner);

    vec3 rcpMx = 1.0 / max(mx, vec3(1.0 / 32768.0));
    vec3 amp = clamp(min(mn, 2.0 - mx) * rcpMx, 0.0, 1.0);
    amp = sqrt(amp);

    float peak = -1.0 / mix(8.0, 5.0, u.p3.z);
    vec3 w = amp * peak;

    vec3 rcpWeight = 1.0 / (1.0 + 4.0 * w);
    vec3 outColor = (b * w + d * w + f * w + h * w + e) * rcpWeight;
    return max(outColor, 0.0);
}

// Options.cfg TonemapMode: filmic curves on the final graded colour (1 AgX, 2 Hable, 3 Reinhard; 0 leaves the colour alone).
vec3 tmAgxContrast(vec3 x) {
    vec3 x2 = x * x; vec3 x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x
         + 0.4298 * x2 + 0.1191 * x - 0.00232;
}
vec3 tmAgx(vec3 c) {
    const mat3 m = mat3(0.842479062253094, 0.0423282422610123, 0.0423756549057051,
                        0.0784335999999992, 0.878468636469772,  0.0784336,
                        0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const mat3 mi = mat3(1.19687900512017,  -0.0528968517574562, -0.0529716355144438,
                        -0.0980208811401368, 1.15190312990417,   -0.0980434501171241,
                        -0.0990297440797205,-0.0989611768448433,  1.15107367264116);
    const float lo = -12.47393, hi = 4.026069;
    c = m * max(c, vec3(0.0));
    c = clamp(log2(max(c, vec3(1e-10))), lo, hi);
    c = (c - lo) / (hi - lo);
    c = tmAgxContrast(c);
    return clamp(mi * c, 0.0, 1.0);
}
vec3 tmHableP(vec3 x) {
    const float A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}
vec3 tmHable(vec3 c) {
    return clamp(tmHableP(c * 2.0) / tmHableP(vec3(11.2)), 0.0, 1.0);
}
vec3 applyTonemap(vec3 c) {
    int mode = int(u.p3.w + 0.5);
    if (mode == 1) return tmAgx(c);
    if (mode == 2) return tmHable(c);
    if (mode == 3) return c / (1.0 + c);
    return c;
}

// Interleaved gradient noise: two deterministic per-pixel hashes (no frame term), combined into zero-mean triangular noise over [-1,1].
// Applied after the tonemap in display encoding so the 8-bit present store breaks the sky gradient's 1/255 contour bands.
float ignTriangle(vec2 p) {
    const vec2 magic = vec2(0.06711056, 0.00583715);
    float n0 = fract(52.9829189 * fract(dot(p, magic)));
    float n1 = fract(52.9829189 * fract(dot(p + vec2(37.0, 17.0), magic)));
    return n0 + n1 - 1.0;
}

void main() {
    vec3 color = (u.p1.w != 0.0) ? casFilter(TexCoord)
               : reconstructedColor(TexCoord);
    if (u.p1.x != 0.0) {
        FragColor = vec4(clamp(color, 0.0, 1.0), 1.0);
        return;
    }
    color = applyTonemap(color);
    color = clamp(color + ignTriangle(floor(gl_FragCoord.xy)) / 255.0, 0.0, 1.0);

    int viewMode = int(u.p2.x + 0.5);
    if (viewMode == 1) {
        uint id = texture(objectIdTex, TexCoord).r;
        if (id == 0u) {
            color = vec3(0.1);
        } else {
            color = vec3(
                float((id       ) & 0xFFu) / 255.0,
                float((id >>  8u) & 0xFFu) / 255.0,
                float((id >> 16u) & 0xFFu) / 255.0);
        }
    } else if (viewMode == 3) {
        float t = clamp(dot(color, vec3(0.299, 0.587, 0.114)), 0.0, 1.0);
        int engineIdxBase = int(u.p3.y + 0.5);
        if (engineIdxBase > 0) {
            uint idx = texture(objectIdTex, TexCoord).r & 0xFFFFFu;
            if (idx >= uint(engineIdxBase))
                t = max(t, 0.9);
        }
        vec3 c0 = vec3(0.00, 0.00, 0.10);
        vec3 c1 = vec3(0.30, 0.00, 0.40);
        vec3 c2 = vec3(0.85, 0.10, 0.05);
        vec3 c3 = vec3(1.00, 0.55, 0.00);
        vec3 c4 = vec3(1.00, 0.95, 0.40);
        vec3 c5 = vec3(1.00, 1.00, 1.00);
        if      (t < 0.2) color = mix(c0, c1, t / 0.2);
        else if (t < 0.4) color = mix(c1, c2, (t - 0.2) / 0.2);
        else if (t < 0.6) color = mix(c2, c3, (t - 0.4) / 0.2);
        else if (t < 0.8) color = mix(c3, c4, (t - 0.6) / 0.2);
        else              color = mix(c4, c5, (t - 0.8) / 0.2);
    } else if (viewMode == 5) {
        float lum = dot(color, vec3(0.299, 0.587, 0.114));
        float boosted = lum * u.p2.y;
        boosted = boosted / (1.0 + boosted * 0.3);
        vec3 nv = boosted * vec3(u.p2.z, u.p2.w, u.p3.x);
        vec2 vd = TexCoord - vec2(0.5);
        float vdist = length(vd * vec2(1.0, 0.75));
        float nvVignette = 1.0 - smoothstep(0.20, 0.70, vdist);
        color = nv * mix(0.65, 1.0, nvVignette);
    }

    FragColor = vec4(color, 1.0);
}
