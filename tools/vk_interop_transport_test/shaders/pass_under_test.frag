#version 450
// ==========================================================================
// THE PARAMETERIZABLE "PASS UNDER TEST".
//
// This is a stand-in for a real MC2 postprocess pass (fog / SSAO blend / etc.).
// It samples a fixed golden input texture and applies a deterministic transform.
// The WHOLE POINT of the harness is that this exact shader runs identically on
// BOTH transport paths (readback vs interop) — so the transform can be anything
// deterministic; the transport must not perturb its output by even one bit.
//
// To swap in a different shader class later: replace the body below (and, if the
// I/O format changes, the C++ IMG_FORMAT / GL internal format). Keep it a pure
// function of the sampled input so the two paths stay bit-comparable.
// ==========================================================================
layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 oColor;

layout(set = 0, binding = 0) uniform sampler2D uGolden;

void main() {
    vec4 g = texture(uGolden, vUV);
    // Deterministic "fog-blend"-shaped transform: mix toward a fog color by a
    // spatially varying factor, plus a channel swizzle/scale. Pure & stable.
    float f = clamp(0.5 * (vUV.x + vUV.y), 0.0, 1.0);
    vec3 fogColor = vec3(0.35, 0.42, 0.55);
    vec3 blended  = mix(g.rgb, fogColor, f * 0.5);
    // A little nonlinearity so the transform isn't trivially identity-like.
    blended = blended * (0.75 + 0.25 * g.rgb.gbr);
    oColor = vec4(blended, g.a);
}
