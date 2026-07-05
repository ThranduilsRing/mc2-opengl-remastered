//#version 420 (version provided by prefix)

in vec2 TexCoord;
layout(location = 0) out vec4 FragColor;

uniform sampler2D sceneTex;   // unit 0: scene
uniform float exposure;       // exposure multiplier (default 1.0)
uniform vec2 inverseScreenSize; // 1/width, 1/height

// VIEWMODE-POSTPROCESS-PRESENTATION-1: presentation-mode uniforms.
// u_viewMode: 0=Visual (default, byte-identical), 1=ObjectIdDebug.
// u_objectIdTex: GL_R32UI object-ID buffer (usampler2D, not sampler2D).
// Both are only read when MC2_VIEWMODE_FRAMEWORK is active; u_viewMode
// defaults to 0 so the Visual path is never disturbed when the gate is OFF.
uniform int u_viewMode;                // 0=Visual, 1=ObjectIdDebug, 3=Thermal, 5=LowLight
uniform usampler2D u_objectIdTex;     // unit 2: sceneObjectIdTex_ (GL_R32UI)
// GAMEADAPTERS-VISUAL-STATE-BRIDGE: lowest object-ID-buffer index that is a
// mech (engine-bearing). Pixels with (objectId & 0xFFFFF) >= this value are
// mechs and read hot in Thermal. 0 = OID buffer unavailable -> luminance only.
uniform int u_engineIdxBase;

// LOWLIGHT-NIGHTVISION-MVP-1: night-vision tunables (read only when
// u_viewMode == 5). Always set by the composite each frame.
uniform float u_lowLightGain;  // luminance amplification (default 2.5)
uniform vec3  u_lowLightTint;  // green-phosphor tint (default 0.7,1.0,0.6)

// POST-FX-FXAA-1: FXAA post anti-aliasing on the scene color, BEFORE the grade
// below. UI is drawn after the whole composite, so it is never touched. Default
// OFF (u_fxaaEnabled==0 -> single tap = byte-identical); flat (sub-threshold)
// regions also return the center tap unchanged. Tunables (subpix / edge
// thresholds) live-driven from gosPostProcess members. Algorithm: classic
// FXAA edge-direction + 2/4-tap blend (Lottes), the widely-shipped compact form.
uniform int   u_fxaaEnabled;
uniform float u_fxaaSubpix;            // subpixel aliasing removal (0..1)
uniform float u_fxaaEdgeThreshold;     // min local contrast to act on (0.063..0.333)
uniform float u_fxaaEdgeThresholdMin;  // dark-region contrast floor

// POST-FX-CAS-1: AMD FidelityFX Contrast Adaptive Sharpening (public-domain /
// MIT, AMD FidelityFX). Single fullscreen pass on the FINAL graded LDR color:
// gradeColor() below is factored out of main() so CAS can sample and grade the
// 3x3 neighborhood, giving true post-grade sharpening. Gate OFF (u_casEnabled==0)
// -> main() never calls this -> byte-identical. u_casSharpness in 0..1 maps to
// the AMD sharpness knob (0 = softest CAS, 1 = sharpest).
uniform int   u_casEnabled;
uniform float u_casSharpness;    // 0..1

// POST-FX-TONEMAP-V2 (SCOUR-1 #3): optional filmic tonemap on the final
// composited LDR colour, gated by u_tonemapMode (0 = OFF -> byte-identical).
// 1=AgX (Sobotka minimal approx), 2=Hable (Uncharted2 filmic), 3=Reinhard.
// Old ACES was deleted as "wrong-for-RTS"; these are the RTS-neutral picks.
// Applied AFTER grade+CAS, before UI -> no geometry/depth/object-id effect.
uniform int   u_tonemapMode;     // 0 off, 1 AgX, 2 Hable, 3 Reinhard

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

    // Flat region -> leave the pixel exactly as-is.
    if (range < max(u_fxaaEdgeThresholdMin, rangeMax * u_fxaaEdgeThreshold))
        return rgbM;

    float lumaNW = fxaaLuma(texture(sceneTex, uv + vec2(-rcp.x, -rcp.y)).rgb);
    float lumaNE = fxaaLuma(texture(sceneTex, uv + vec2( rcp.x, -rcp.y)).rgb);
    float lumaSW = fxaaLuma(texture(sceneTex, uv + vec2(-rcp.x,  rcp.y)).rgb);
    float lumaSE = fxaaLuma(texture(sceneTex, uv + vec2( rcp.x,  rcp.y)).rgb);

    vec2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));

    // subpix lowers dirReduce -> longer search -> more subpixel smoothing.
    float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * 0.25 * (1.0 - u_fxaaSubpix), 1.0 / 128.0);
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

// Final graded LDR color at a given UV. Factored out of main() unchanged so that
// CAS (POST-FX-CAS-1) can sample the graded neighborhood. The uv passed here is
// the neighbor location; the grade is position-dependent (vignette / sun glow),
// so it is evaluated per-tap for correctness.
vec3 gradeColor(vec2 uv)
{
    // Scene sample with exposure (no gamma — pipeline is already sRGB).
    // POST-FX-FXAA-1: optionally antialias the scene color first (gate OFF =
    // single tap = byte-identical).
    vec3 color = ((u_fxaaEnabled != 0)
        ? fxaaFilter(uv, inverseScreenSize)
        : texture(sceneTex, uv).rgb) * exposure;

    // --- Sunset filter (subtle) ---
    // Light warm grade + gentle vignette + soft top-of-screen warmth.
    {
        // Gentle unconditional warm push.
        color *= vec3(1.05, 1.01, 0.94);

        // Luminance-based highlight warming / shadow cooling.
        float lum = dot(color, vec3(0.299, 0.587, 0.114));
        vec3 warm = vec3(1.05, 1.00, 0.93);
        vec3 cool = vec3(0.95, 0.98, 1.04);
        vec3 grade = mix(cool, warm, smoothstep(0.10, 0.75, lum));
        color *= grade;

        // Subtle radial vignette.
        vec2 vdir = uv - vec2(0.5);
        float vdist = length(vdir * vec2(1.0, 0.6));
        float vignette = smoothstep(0.85, 0.25, vdist);
        color *= mix(0.82, 1.0, vignette);

        // Soft top-of-screen warm glow.
        float sunBias = pow(smoothstep(0.0, 1.0, 1.0 - uv.y), 1.5);
        color = mix(color, color * vec3(1.10, 1.03, 0.92), sunBias * 0.25);
    }
    return color;
}

// POST-FX-CAS-1: AMD FidelityFX CAS on the graded LDR color. Samples the 3x3
// neighborhood (diamond b/d/e/f/h + corners a/c/g/i), derives an adaptive
// sharpening weight from the local per-channel min/max contrast, and blends the
// center with the diamond ring. Standard AMD ffx_cas reference form.
vec3 casFilter(vec2 uv)
{
    vec2 rcp = inverseScreenSize;
    // 3x3 neighborhood (graded).
    vec3 a = gradeColor(uv + vec2(-rcp.x, -rcp.y));
    vec3 b = gradeColor(uv + vec2( 0.0,   -rcp.y));
    vec3 c = gradeColor(uv + vec2( rcp.x, -rcp.y));
    vec3 d = gradeColor(uv + vec2(-rcp.x,  0.0));
    vec3 e = gradeColor(uv);
    vec3 f = gradeColor(uv + vec2( rcp.x,  0.0));
    vec3 g = gradeColor(uv + vec2(-rcp.x,  rcp.y));
    vec3 h = gradeColor(uv + vec2( 0.0,    rcp.y));
    vec3 i = gradeColor(uv + vec2( rcp.x,  rcp.y));

    // Soft min/max over the 3x3, weighting the diamond taps double (AMD form:
    // min/max of the '+' ring, then include corners at half influence).
    vec3 mnRing = min(min(min(d, e), min(f, b)), h);
    vec3 mnCorner = min(min(a, c), min(g, i));
    vec3 mn = mnRing + min(mnRing, mnCorner);

    vec3 mxRing = max(max(max(d, e), max(f, b)), h);
    vec3 mxCorner = max(max(a, c), max(g, i));
    vec3 mx = mxRing + max(mxRing, mxCorner);

    // Adaptive amplitude: how much room to sharpen, from local contrast.
    vec3 rcpMx = 1.0 / max(mx, vec3(1.0 / 32768.0));
    vec3 amp = clamp(min(mn, 2.0 - mx) * rcpMx, 0.0, 1.0);
    amp = sqrt(amp);

    // Sharpness knob: 0..1 maps to AMD peak -8..-5 (softer..sharper).
    float peak = -1.0 / mix(8.0, 5.0, u_casSharpness);
    vec3 w = amp * peak;

    // Blend center with the diamond ring (b,d,f,h), normalized.
    vec3 rcpWeight = 1.0 / (1.0 + 4.0 * w);
    vec3 outColor = (b * w + d * w + f * w + h * w + e) * rcpWeight;
    return clamp(outColor, 0.0, 1.0);
}

// POST-FX-TONEMAP-V2: filmic tonemap curves (algorithm re-derived from the
// public AgX/Hable/Reinhard references; MIT/public-domain, SCOUR-1 #3).
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
    if (u_tonemapMode == 1) return tmAgx(c);
    if (u_tonemapMode == 2) return tmHable(c);
    if (u_tonemapMode == 3) return c / (1.0 + c);   // Reinhard
    return c;                                        // 0 = off, byte-identical
}

void main()
{
    // POST-FX-CAS-1: gate OFF -> single grade tap (byte-identical). When ON, CAS
    // samples/grades the 3x3 neighborhood and sharpens the graded center.
    vec3 color = (u_casEnabled != 0) ? casFilter(TexCoord) : gradeColor(TexCoord);

    // POST-FX-TONEMAP-V2: optional filmic curve (u_tonemapMode 0 = no-op).
    color = applyTonemap(color);

    FragColor = vec4(color, 1.0);

    // VIEWMODE-POSTPROCESS-PRESENTATION-1: ObjectIdDebug overlay.
    // Overrides FragColor when u_viewMode == 1. All other values fall through
    // to the Visual path above (byte-identical). No sampling of u_objectIdTex
    // occurs in the Visual path — the sampler binding is safe to leave unread.
    if (u_viewMode == 1) {
        uint id = texture(u_objectIdTex, TexCoord).r;
        if (id == 0u) {
            // No object at this pixel — dark grey background
            FragColor = vec4(0.1, 0.1, 0.1, 1.0);
        } else {
            // Hash id to a stable RGB color per object
            float r = float((id       ) & 0xFFu) / 255.0;
            float g = float((id >>  8u) & 0xFFu) / 255.0;
            float b = float((id >> 16u) & 0xFFu) / 255.0;
            FragColor = vec4(r, g, b, 1.0);
        }
    }
    // THERMAL-VIEW-MECH-HOT-1: engine-bearing units (mechs) read HOT, the rest
    // of the scene maps Visual luminance to an iron palette (emissive/bright
    // regions — fire, exhaust, muzzle flash, specular — still read warm; dark
    // terrain reads cool). "Engine = hot" classification uses the object-ID
    // buffer: mech handles occupy index >= u_engineIdxBase (kMechHandleBase),
    // static props/terrain are below it. Real per-unit heat does not exist
    // (USEHEAT compiled out); vehicles render via the static-prop batcher and
    // are not yet distinguishable (vehicles-hot is a documented follow-up).
    // u_engineIdxBase == 0 means the OID buffer is unavailable -> luminance
    // only, identical to the prior placeholder.
    else if (u_viewMode == 3) {
        float t = clamp(dot(color, vec3(0.299, 0.587, 0.114)), 0.0, 1.0);
        if (u_engineIdxBase > 0) {
            // 0xFFFFFu = RenderObjectHandle index-field mask ([19:0]). Mirrors
            // RenderCore::Handle::index() and RenderWorld::kHandleIndexMask (a
            // static_assert in RenderWorld.cpp fails the build if that layout
            // drifts — this GLSL literal can't reference the C++ constant). The
            // test is the GPU mirror of RenderWorld's mech partition classifier:
            // (rawId & mask) >= MechHandleIndexBase() == "this pixel is a mech".
            uint idx = texture(u_objectIdTex, TexCoord).r & 0xFFFFFu;
            if (idx >= uint(u_engineIdxBase))
                t = max(t, 0.9);  // force mech pixels into the hot band
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
    }
    // LOWLIGHT-NIGHTVISION-MVP-1: amplify the dark-adapted Visual luminance and
    // tint toward green phosphor with a stronger NV-tube vignette. Operates on
    // the graded Visual color, so it works regardless of HDR/tonemap state.
    else if (u_viewMode == 5) {
        float lum = dot(color, vec3(0.299, 0.587, 0.114));
        float boosted = lum * u_lowLightGain;
        boosted = boosted / (1.0 + boosted * 0.3); // soft-knee; preserve headroom
        vec3 nv = boosted * u_lowLightTint;
        vec2 vd = TexCoord - vec2(0.5);
        float vdist = length(vd * vec2(1.0, 0.75));
        float nvVignette = smoothstep(0.70, 0.20, vdist);
        nv *= mix(0.65, 1.0, nvVignette);
        FragColor = vec4(nv, 1.0);
    }
}
