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

layout(location = 3) in vec4 v_vegData;
layout(location = 4) in vec2 v_vegAux;
void main() {
    if ((v_flags & 16u) != 0u) {
    // Whole-clump projected-size/distance LOD is resolved before upload.
    vec4 col = texture(uAtlas, v_uv);

    // Alpha test: top/cap cards use softer threshold (0.25) for wispy edges.
    float alphaThresh = mix(0.50, 0.25, v_vegData.z);
    if (col.a < alphaThresh) discard;

    // Desaturate to reduce uniform vivid-green look.
    float lum = dot(col.rgb, vec3(0.299, 0.587, 0.114));
    col.rgb = mix(col.rgb, vec3(lum), 0.30);

    // Per-instance warmth variation: some cards yellower, some cooler.
    col.r += (v_vegData.x - 0.5) * 0.07;
    col.b -= v_vegData.x * 0.05;

    // Lighting. Top/cap cards are slightly darker: horizontal face = less direct sun.
    float NdotL = v_vegData.w;
    float topDim = mix(1.0, 0.80, v_vegData.z);
    col.rgb *= (0.60 + 0.38 * NdotL) * topDim;

    // Root lift.
    col.rgb += vec3(v_vegData.y * 0.08);

    col.rgb = min(vec3(1.0), col.rgb);

    o_color = vec4(col.rgb, 1.0);
    // Sets GBuffer1.a > 0.5 so post-screen-shadow pass skips vegetation.
    o_normal = vec4(0.5,0.5,1.0,1.0);
        return;
    }
    // The billboard sampler repeats; mesh/ribbon UVs retain their CLAMP_TO_EDGE policy.
    vec2 uv = v_uv;
    if ((v_flags & 12u) != 0u) {
        vec2 edge = 0.5 / vec2(textureSize(uAtlas, 0));
        uv = clamp(uv, edge, 1.0 - edge);
    }
    vec4 tex = (v_flags & 32u) != 0u ? vec4(1.0) : textureLod(uAtlas, uv, 0.0);
    if ((v_flags & 12u) != 0u) {
        vec4 c = tex * v_color;
        if ((v_flags & 8u) != 0u) {
            if ((v_flags & 2u) != 0u) {
                if (max(max(c.r, c.g), c.b) <= 0.0001) discard;
            } else if (c.a <= 0.0039) discard;
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
