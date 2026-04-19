// shaders/static_prop.vert
// GPU static prop renderer — main vertex shader (Task 9).
// NOTE: no #version directive here — makeProgram() prepends "#version 430\n".

layout(location = 0) in vec3  a_position;
layout(location = 1) in vec3  a_normal;
layout(location = 2) in vec2  a_uv;
layout(location = 3) in uint  a_localVertexID;

struct Instance {
    mat4  modelMatrix;
    uint  typeID;
    uint  firstColorOffset;
    uint  flags;
    uint  _pad0;
    vec4  aRGBHighlight;
    vec4  fogRGB;
};

layout(std430, binding = 0) readonly buffer Instances { Instance i[]; } instances_;
layout(std430, binding = 1) readonly buffer Colors    { uint     c[]; } colors_;

uniform mat4 u_worldToClip;

out vec3  v_normal;
out vec2  v_uv;
flat out uint v_flags;
out vec4  v_highlight;
out vec4  v_fog;
out vec4  v_argb;
flat out uint v_localVertexID;  // for debug address mode

void main() {
    Instance inst = instances_.i[gl_InstanceID];

    vec4 world = inst.modelMatrix * vec4(a_position, 1.0);
    gl_Position = u_worldToClip * world;

    // Baked per-vertex lighting: colorBuffer[firstColorOffset + a_localVertexID].
    uint argbPacked = colors_.c[inst.firstColorOffset + a_localVertexID];
    vec4 argb;
    argb.a = float((argbPacked >> 24) & 0xFFu) / 255.0;
    argb.r = float((argbPacked >> 16) & 0xFFu) / 255.0;
    argb.g = float((argbPacked >>  8) & 0xFFu) / 255.0;
    argb.b = float((argbPacked >>  0) & 0xFFu) / 255.0;
    v_argb = argb;

    v_normal     = mat3(inst.modelMatrix) * a_normal;
    v_uv         = a_uv;
    v_flags      = inst.flags;
    v_highlight  = inst.aRGBHighlight;
    v_fog        = inst.fogRGB;
    v_localVertexID = a_localVertexID;
}
