// OOB-FOG-7: sea-of-clouds at the map horizon.
//
// Uses 3D FBM sampled directly on the unit sphere (worldDir) — no 2D UV
// projection, no seams, no directional bias, no diagonal streaks.
// Solid cloud base with gentle spherical FBM surface variation.
//
// MC2 / invWorldToClipGL frame: worldDir.z < 0 = sky up, worldDir.z > 0 = down.
//
// Gate:  MC2_OOB_FOG (default ON, "0" to disable).
// Color: MC2_OOB_FOG_COLOR=r,g,b

in vec2 TexCoord;
layout(location = 0) out vec4 outFog;

uniform sampler2D depthTex;
uniform mat4      invViewProj;
uniform vec3      u_fogColor;
uniform float     u_fogOpacity;
uniform float     u_time;

// SKYBOX-FOG-EXCLUDE-1/2 (gate MC2_SKYBOX_FOG_EXCLUDE, default OFF -> u_skyExcludeEnabled=0,
// byte-identical to legacy). When enabled, true-sky pixels are tagged stencil=1 by
// the HDRI skybox's stencil-tag pass (shaders/hdri_skybox_stencil_tag.frag --
// elevation test equivalent to this shader's worldDir.z < -0.22 band top) and
// FEATHER-excluded here (v2: modulate by the retained worldDir fade instead of
// the v1 hard zero, so any tag/fog frame mismatch degrades to a smooth rolloff,
// not a seam). The worldDir.z band stays as the fallback for non-HDRI/no-skybox
// scenes (stencil never gets tagged then, so stencilTex reads 0 everywhere and
// this branch is a no-op).
// usampler2D: GL_DEPTH_STENCIL_TEXTURE_MODE=GL_STENCIL_INDEX views return raw
// unsigned stencil index values (0..255), not normalized floats.
uniform usampler2D stencilTex;
uniform int        u_skyExcludeEnabled;

// FOG-HORIZON-CLAMP-1: elevation profile. In the invWorldToClipGL frame,
// worldDir.z < 0 = looking up (sky), z == 0 = horizon, z > 0 = looking down.
// elevSin = -worldDir.z is the sine of the view ray's elevation above the
// horizon. The desired shape: FULL fog at/below the horizon (covers the OOB
// terrain/water to the sides and down) and a smooth fade to ZERO within a small
// elevation band just above the horizon, leaving the sky above CLEAR. The CPU
// precomputes the band edges as sines (u_horizonFadeStartSin < u_horizonFadeEndSin)
// so this stays trig-free. u_horizonClampEnabled == 0 restores the legacy
// worldDir.z 0.22 exclusion band (kill-switch MC2_FOG_HORIZON_CLAMP=0).
uniform int   u_horizonClampEnabled;
uniform float u_horizonFadeStartSin;
uniform float u_horizonFadeEndSin;

// ---- 3D value noise ----
// Sampling worldDir (unit sphere) in 3D eliminates all UV projection
// artifacts: no seams, no directional tilt, no vertical streaks.

float hash31(vec3 p) {
    p = fract(p * vec3(127.1, 311.7, 74.7));
    return fract(sin(dot(p, vec3(269.5, 183.3, 246.1))) * 43758.5453);
}

float vnoise3(vec3 p) {
    vec3 i = floor(p); vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(
        mix(mix(hash31(i),              hash31(i+vec3(1,0,0)), f.x),
            mix(hash31(i+vec3(0,1,0)),  hash31(i+vec3(1,1,0)), f.x), f.y),
        mix(mix(hash31(i+vec3(0,0,1)),  hash31(i+vec3(1,0,1)), f.x),
            mix(hash31(i+vec3(0,1,1)),  hash31(i+vec3(1,1,1)), f.x), f.y),
        f.z);
}

float fbm3D(vec3 p) {
    float v = 0.0, a = 0.5;
    for (int i = 0; i < 5; i++) {
        v += vnoise3(p) * a;
        p  = p * 2.1 + vec3(1.7, 0.9, 1.4);
        a *= 0.5;
    }
    return v;
}

void main()
{
    float rawDepth = texture(depthTex, TexCoord).r;
    if (rawDepth > 0.0001) { outFog = vec4(0.0); return; }

    vec2 ndc = TexCoord * 2.0 - 1.0;
    vec4 pNear = invViewProj * vec4(ndc, 1.0, 1.0);
    vec4 pFar  = invViewProj * vec4(ndc, 0.0, 1.0);
    vec3 worldDir = normalize(pFar.xyz / pFar.w - pNear.xyz / pNear.w);

    // FOG-HORIZON-CLAMP-1: elevation-angle profile (primary shape).
    //   elevSin = -worldDir.z  (positive above horizon, negative below).
    //   full fog for elevSin <= startSin (horizon and below),
    //   smooth fade to zero across [startSin, endSin], clear above endSin.
    // Legacy fallback (u_horizonClampEnabled == 0): the old worldDir.z 0.22 band,
    // which faded IN from below the horizon and let fog survive up to ~12deg.
    float skyFade;
    if (u_horizonClampEnabled != 0) {
        float elevSin = -worldDir.z;
        if (elevSin >= u_horizonFadeEndSin) { outFog = vec4(0.0); return; }
        skyFade = 1.0 - smoothstep(u_horizonFadeStartSin, u_horizonFadeEndSin, elevSin);
    } else {
        // Legacy: worldDir.z < 0 = sky. Fade in from just-above-horizon.
        if (worldDir.z < -0.22) { outFog = vec4(0.0); return; }
        skyFade = smoothstep(-0.22, -0.01, worldDir.z);
    }

    // SKYBOX-FOG-EXCLUDE-2 (feathered): where the stencil tag says "true sky",
    // roll the fog off across the SAME worldDir.z band the fade above uses,
    // instead of the v1 hard zero. The tag pass now marks deep sky by an
    // elevation test equivalent to this shader's band top (worldDir.z < -0.22
    // here), so tagged pixels already sit at skyFade == 0 and this factor is
    // a no-op when the two reconstructions agree exactly; when they disagree
    // slightly (different matrices/frames), a binary cut would show as a hard
    // seam -- the feather degrades that to a smooth extra rolloff. Gate OFF
    // (u_skyExcludeEnabled == 0, default) -> factor stays 1.0, stencil never
    // sampled -> byte-identical to legacy.
    float skyExclude = 1.0;
    if (u_skyExcludeEnabled != 0) {
        uint stencilVal = texture(stencilTex, TexCoord).r;
        if (stencilVal != 0u) skyExclude = skyFade;
    }

    // Slow horizontal drift for cloud animation. Shift worldDir in XY plane.
    vec3 p3 = worldDir * 4.5 + vec3(u_time * 0.008, u_time * 0.002, 0.0);

    // 3D FBM on the sphere — naturally isotropic, no UV artifacts.
    float n = fbm3D(p3);

    // Solid cloud base + gentle surface variation from noise.
    float base   = 0.86;
    float ripple = (n - 0.50) * 0.28;
    float alpha  = clamp((base + ripple) * skyFade * skyExclude * u_fogOpacity, 0.0, 1.0);

    // Sunlit tops bright, shadowed hollows slightly grey.
    float lit = smoothstep(0.38, 0.68, n);
    vec3  col = mix(u_fogColor * 0.78, u_fogColor * 1.05, lit);

    outFog = vec4(clamp(col, 0.0, 1.0), alpha);
}
