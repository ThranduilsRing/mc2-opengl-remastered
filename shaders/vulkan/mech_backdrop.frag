#version 450
#extension GL_GOOGLE_include_directive : require
// Mech-lab panel backdrop: studio gradient + vignette + floor glow + soft contact shadow under the mech feet.
// Parameters ride the preview frame UBO (unused by the preview colour pass): poolF = (feetX, feetY, feetRx, feetRy)
// in viewport uv, y down; poolF is zero-radius when there is no contact shadow. Alpha is kept below 1 toward the
// edges so the lab slot grid underneath stays readable.
#include "include/mech_frame_ubo_vk.glsl"
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 oColor;
layout(location = 1) out vec4 oGBuffer1;
void main() {
    vec2 p = vec2(vUv.x, 1.0 - vUv.y);                       // panel space, y down
    vec2 c = p - vec2(0.5, 0.58);
    float vig = smoothstep(0.95, 0.15, length(c * vec2(1.0, 1.15)));
    vec3 top = vec3(0.030, 0.036, 0.046);
    vec3 bot = vec3(0.085, 0.092, 0.104);
    vec3 col = mix(top, bot, smoothstep(0.0, 1.0, p.y));
    // Soft key-light pool on the floor behind/under the mech.
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
    float a = mix(0.50, 0.92, vig);
    a = max(a, sh * 0.85);
    oColor = vec4(col, a);
    oGBuffer1 = vec4(0.0);
}
