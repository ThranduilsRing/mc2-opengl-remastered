// VK alpha-tested static-prop shadow-caster FS: the twin of shaders/shadow_static_prop.frag (MC2_SHADOW_PROP_ATLAS variant). A foliage or fence card is a textured
// quad with a binary-alpha cutout; writing the whole quad's depth would cast a rectangular shadow, so a texel below the colour pass's 0.5 alpha discards here too.
// The array, the layer and the UV sub-region scale are the ones the colour pass samples for the packet; `fract(uv) * scale` is the colour pass's own derivation.
#version 450

layout(set = 1, binding = 0) uniform sampler2DArray uAlbedo;

layout(push_constant) uniform PerPacket {
    vec2 uvScale;
    int  layer;
    int  _pad;
} pc;

layout(location = 0) in vec2 v_uv;

void main() {
    vec4 texel = texture(uAlbedo, vec3(fract(v_uv) * pc.uvScale, float(pc.layer)));
    if (texel.a < 0.5) discard;
}
