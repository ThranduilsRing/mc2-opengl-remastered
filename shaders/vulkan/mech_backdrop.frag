#version 450
#extension GL_GOOGLE_include_directive : require
// Mech-lab panel backdrop: a procedural mech-bay interior (steel bulkhead wall with panel seams, overhead light strip,
// perspective floor grid with a hazard line) + vignette + soft contact shadow under the mech feet. No texture assets.
// Parameters ride the preview frame UBO (unused by the preview colour pass): poolF = (feetX, feetY, feetRx, feetRy)
// in viewport uv, y down; poolF is zero-radius when there is no contact shadow.
#include "include/mech_frame_ubo_vk.glsl"
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 oColor;
layout(location = 1) out vec4 oGBuffer1;
float band(float x, float w) { return 1.0 - smoothstep(0.0, w, abs(x)); }
void main() {
    vec2 p = vec2(vUv.x, 1.0 - vUv.y);                       // panel space, y down
    vec2 c = p - vec2(0.5, 0.58);
    float vig = smoothstep(0.95, 0.15, length(c * vec2(1.0, 1.15)));
    float hy = uFrame.shadeC.z > 0.0 ? uFrame.shadeC.z : 0.86;   // wall/floor line: predicted during the fit, latched at the reveal (cpu side)
    vec3 col;
    if (p.y < hy) {
        float t = p.y / hy;
        col = mix(vec3(0.16, 0.19, 0.24), vec3(0.30, 0.34, 0.40), t);
        float pan = abs(fract(p.x * 3.0 + 0.5) - 0.5);                               // 3 bulkhead panels, recessed centres
        col *= 0.80 + 0.30 * smoothstep(0.0, 0.18, pan);
        col *= 1.0 - 0.60 * band(pan, 0.010);                                         // tall panel seams
        col += vec3(0.10, 0.11, 0.13) * (band(t - 0.34, 0.012) + band(t - 0.68, 0.012));   // horizontal seam bands
        col += vec3(1.0, 0.97, 0.88) * band(p.y - 0.045, 0.030) * smoothstep(0.0, 0.35, 0.7 - abs(p.x - 0.5));   // light strip
        col += vec3(0.16, 0.17, 0.19) * smoothstep(0.35, 0.0, p.y);                  // light spill down the wall
    } else {
        float d = (p.y - hy) / max(1.0 - hy, 0.001);         // 0 at the wall, 1 at the panel bottom
        float depth = 1.0 / (0.12 + d);                      // large far away
        float line = max(band(fract((p.x - 0.5) * depth * 0.55 + 0.5) - 0.5, 0.035 * (0.3 + d)), band(fract(depth * 0.9) - 0.5, 0.05));
        col = mix(vec3(0.16, 0.17, 0.20), vec3(0.07, 0.08, 0.10), d);
        col += vec3(0.16, 0.18, 0.22) * line * (1.0 - d * 0.6) * smoothstep(0.0, 0.25, d);   // grid fades into the wall line (no bright cone at the far centre)
        float haz = 1.0 - smoothstep(0.0, 0.012, p.y - hy);   // hazard line where the wall meets the floor
        float stripe = step(0.5, fract((p.x + (p.y - hy) * 6.0) * 24.0));
        col = mix(col, mix(vec3(0.07, 0.055, 0.02), vec3(0.45, 0.32, 0.05), stripe), haz);
    }
    // Soft key-light pool on the floor under the mech.
    vec2 g = (p - uFrame.poolF.xy);
    float pool = exp(-dot(g * vec2(1.0, 2.6), g * vec2(1.0, 2.6)) * 9.0);
    col += vec3(0.16, 0.17, 0.19) * pool * step(0.0001, uFrame.poolF.w);
    col *= mix(0.55, 1.0, vig);
    // Contact shadow: elliptical gaussian at the feet.
    float sh = 0.0;
    if (uFrame.poolF.z > 0.0001 && uFrame.poolF.w > 0.0001) {
        vec2 e = g / uFrame.poolF.zw;
        sh = exp(-dot(e, e) * 2.2);
    }
    col = mix(col, vec3(0.0), sh * 0.85);
    float a = mix(0.90, 0.98, vig);
    a = max(a, sh * 0.85);
    oColor = vec4(col, a);
    oGBuffer1 = vec4(0.0);
}
