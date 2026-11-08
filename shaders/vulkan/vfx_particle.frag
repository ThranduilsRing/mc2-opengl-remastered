// VK-FIELD-VFX-1 -- textured billboard FS for the VK particle island: the
// default-mode body of shaders/particle_billboard.frag (sprite sample times the
// per-particle tint, colour-key discard, head brighten, low-alpha discard). The
// GL debug views and the soft / lit / distortion / blackbody options are not
// ported; each is off by default in GL.
//
// Straight-alpha output (rgb, a): the pipeline blend factors are
//   alpha    group: SRC_ALPHA / ONE_MINUS_SRC_ALPHA  (matches GL alpha)
//   additive group: SRC_ALPHA / ONE                  (matches GL additive)
// so the FS emits straight color; the src-alpha factor premultiplies in HW.
#version 450

layout(set = 1, binding = 0) uniform sampler2D uAtlas;   // the draw group's sprite page

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 2) flat in uint v_flags;   // bit 0 = head sprite, bit 1 = pure-additive (OneOne) group

layout(location = 0) out vec4 o_color;   // shared scene color0
layout(location = 1) out vec4 o_normal;  // shared scene normal (writemask 0 -> inert)

void main() {
    // Swept tubes advance U per profile beyond 1.0. Clamp only when authored:
    // clamping a repeating trail stretches its edge texel into a flat sheet.
    // Bit 4 carries the tube's MLR texture-address policy; meshes keep clamp.
    vec2 uv = v_uv;
    if ((v_flags & 20u) != 0u) {
        vec2 edge = 0.5 / vec2(textureSize(uAtlas, 0));
        uv = clamp(uv, edge, 1.0 - edge);
    }
    vec4 tex = (v_flags & 32u) != 0u ? vec4(1.0) : textureLod(uAtlas, uv, 0.0);
    if ((v_flags & 12u) != 0u) {
        vec4 c = tex * v_color;
        if ((v_flags & 8u) != 0u) {
            if ((v_flags & 2u) != 0u) {
                if (max(max(c.r, c.g), c.b) <= 0.0001) discard;
            // ONE/srcA and ONE/1-srcA retain RGB even at alpha zero.
            } else if ((v_flags & 1536u) == 0u && c.a <= 0.0039) discard;
            // Bit 1024: ONE/ZERO opaque tube. Alpha is irrelevant; write full coverage.
            if ((v_flags & 1024u) != 0u) c.a = 1.0;
        }
        // Mesh SRC_ALPHA/ONE keeps its authored alpha. Tube ONE/ONE uses RGB.
        o_color = c;
        o_normal = vec4(0.0);
        return;
    }
    // MC2 sprite pages mark transparent texels with magenta (0xFF00FF).
    if (tex.r > 0.9 && tex.g < 0.1 && tex.b > 0.9) discard;
    vec4 c = tex * v_color;
    // Head sprites (the bright leading particle of a beam / trail) draw 1.5x brighter.
    if ((v_flags & 1u) != 0u) c.rgb *= 1.5;
    // OneOne (ONE,ONE) ignores alpha: the MLR path adds the sprite colour whatever the vertex alpha is
    // (the halo cards carry alpha 0), so such a group neither multiplies by nor discards on alpha.
    if ((v_flags & 2u) != 0u) c.a = 1.0;
    else if (c.a < 0.01) discard;
    o_color  = c;
    o_normal = vec4(0.0);   // GL VFX writes nothing to the normal target
}
