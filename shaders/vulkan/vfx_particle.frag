// VK-FIELD-VFX-1 -- untextured colored soft-billboard FS for the VK particle
// island. The GL path samples a per-group sprite texture and modulates by the
// per-particle color; this UNTEXTURED slice approximates a generic soft sprite
// with a radial alpha falloff so weapon tracers / muzzle flashes / impact glows
// read as round soft blobs (bright combat activity) rather than hard squares.
// Textured sprite fidelity is the documented deferred remainder.
//
// Straight-alpha output (rgb, a): the pipeline blend factors are
//   alpha    group: SRC_ALPHA / ONE_MINUS_SRC_ALPHA  (matches GL alpha)
//   additive group: SRC_ALPHA / ONE                  (matches GL additive)
// so the FS emits straight color; the src-alpha factor premultiplies in HW.
#version 450

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_corner;

layout(location = 0) out vec4 o_color;   // shared scene color0
layout(location = 1) out vec4 o_normal;  // shared scene normal (writemask 0 -> inert)

void main() {
    // Radial falloff: 0 at center, 1 at corner distance. Smooth soft edge.
    float d = length(v_corner - vec2(0.5)) * 2.0;   // 0..~1.414
    float soft = 1.0 - smoothstep(0.55, 1.0, d);
    float a = clamp(v_color.a * soft, 0.0, 1.0);
    if (a <= 0.003) discard;
    o_color  = vec4(v_color.rgb, a);
    o_normal = vec4(0.0);   // GL VFX writes nothing to the normal target
}
