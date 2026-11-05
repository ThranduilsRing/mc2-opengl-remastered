// Static-prop shadows use the colour core pass's LOD-aware alpha cutout.
// Minified partial coverage belongs to the blended fringe and must not cast solid card shadows.
// The array, layer and UV sub-region match the colour pass.
#version 450
#extension GL_EXT_buffer_reference : require

layout(set = 1, binding = 0) uniform sampler2DArray uAlbedo;

// Per-draw rows (16 B, std430) read through buffer device address; the row is packetBase + gl_DrawID (from the vertex stage).
struct AlphaRow { vec2 uvScale; int layer; int _pad; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer AlphaRows { AlphaRow p[]; };
layout(push_constant) uniform DrawPush {
    AlphaRows rows;
    uint packetBase;
    uint _pad;
} pc;

layout(location = 0) in vec2 v_uv;
layout(location = 1) flat in uint v_row;

void main() {
    AlphaRow row = pc.rows.p[v_row];
    vec2 uv = fract(v_uv) * row.uvScale;
    vec2 size = vec2(textureSize(uAlbedo, 0).xy);
    vec2 dx = dFdx(uv) * size;
    vec2 dy = dFdy(uv) * size;
    float lod = max(0.5 * log2(max(max(dot(dx, dx), dot(dy, dy)), 1e-12)), 0.0);
    vec4 texel = textureLod(uAlbedo, vec3(uv, float(row.layer)), lod);
    float cut = mix(0.5, 0.9, clamp(lod * 0.5, 0.0, 1.0));
    if (texel.a < cut) discard;
}
