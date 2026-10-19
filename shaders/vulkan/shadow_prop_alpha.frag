// Static-prop shadows use the colour core pass's LOD-aware alpha cutout.
// Minified partial coverage belongs to the blended fringe and must not cast solid card shadows.
// The array, layer and UV sub-region match the colour pass.
#version 450

layout(set = 1, binding = 0) uniform sampler2DArray uAlbedo;

layout(push_constant) uniform PerPacket {
    vec2 uvScale;
    int  layer;
    int  _pad;
} pc;

layout(location = 0) in vec2 v_uv;

void main() {
    vec2 uv = fract(v_uv) * pc.uvScale;
    vec2 size = vec2(textureSize(uAlbedo, 0).xy);
    vec2 dx = dFdx(uv) * size;
    vec2 dy = dFdy(uv) * size;
    float lod = max(0.5 * log2(max(max(dot(dx, dx), dot(dy, dy)), 1e-12)), 0.0);
    vec4 texel = textureLod(uAlbedo, vec3(uv, float(pc.layer)), lod);
    float cut = mix(0.5, 0.9, clamp(lod * 0.5, 0.0, 1.0));
    if (texel.a < cut) discard;
}
