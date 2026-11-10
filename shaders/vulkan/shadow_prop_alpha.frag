// Static-prop shadows use the colour core pass's LOD-aware alpha cutout.
// Minified partial coverage belongs to the blended fringe and must not cast solid card shadows.
// The array, layer and UV sub-region match the colour pass.
#version 450
#extension GL_EXT_buffer_reference : require
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require

// PROP-ARRAY-BUCKETS: the alpha-ON group's size-bucket arrays; row.layer carries the bucket.
#include "include/prop_array_bucket.glsl"
layout(set = 1, binding = 0) uniform sampler2DArray uAlbedo[PROP_ALPHA_ON_BUCKETS];

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
    const int bucket = propBucketOf(row.layer);
    vec2 uv = fract(v_uv) * row.uvScale;
    vec2 size = vec2(textureSize(uAlbedo[nonuniformEXT(bucket)], 0).xy);
    vec2 dx = dFdx(uv) * size;
    vec2 dy = dFdy(uv) * size;
    float lod = max(0.5 * log2(max(max(dot(dx, dx), dot(dy, dy)), 1e-12)), 0.0);
    vec4 texel = textureLod(uAlbedo[nonuniformEXT(bucket)], vec3(uv, float(propBucketLayer(row.layer))), lod);
    float cut = mix(0.5, 0.9, clamp(lod * 0.5, 0.0, 1.0));
    if (texel.a < cut) discard;
}
