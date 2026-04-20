// shaders/static_prop.vert — back to simplest projection
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
uniform vec4 u_terrainViewport;
uniform mat4 u_mvp;

out vec3  v_normal;
out vec2  v_uv;
flat out uint v_flags;
out vec4  v_highlight;
out vec4  v_fog;
out vec4  v_argb;
flat out uint v_localVertexID;

void main() {
    Instance inst = instances_.i[gl_InstanceID];
    // u_worldToClip uploaded GL_TRUE: GLSL sees transpose, so `M * v` ==
    // row-vec math == Stuff convention.
    // modelMatrix from SSBO std430 default col-major: GLSL sees same
    // matrix as memory. For row-vec convention (translation in row 3),
    // we need `v * M` in GLSL to apply translation correctly.
    vec4 world = vec4(a_position, 1.0) * inst.modelMatrix;
    // Apply full D3D->GL projection chain (identical to terrain_overlay.vert).
    // u_worldToClip outputs screen-pixel-homogeneous coords (D3D style).
    vec4 clip4 = u_worldToClip * world;
    float rhw  = 1.0 / clip4.w;
    vec3  px;
    px.x = clip4.x * rhw * u_terrainViewport.x + u_terrainViewport.z;
    px.y = clip4.y * rhw * u_terrainViewport.y + u_terrainViewport.w;
    px.z = clip4.z * rhw;
    vec4 ndc = u_mvp * vec4(px, 1.0);
    float absW = abs(clip4.w);
    gl_Position = vec4(ndc.xyz * absW, absW);

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
