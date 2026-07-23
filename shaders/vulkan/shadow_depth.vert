// VK-REGION-SHADOW-1 (sub-slice A) -- depth-only INSTANCED shadow-caster vertex
// shader. This is the WRITE side of the shadow pipeline (comparesampler/csmarray
// proved the READ side). Per-instance world transforms come from a STORAGE_BUFFER
// (the instance ring, set1 binding0, bound as STORAGE_BUFFER_DYNAMIC with a dynamic
// offset = slot*capacity*stride) -- inventory hard-case #2. The light-space matrix
// for the active cascade lives in the per-frame UBO (set0 binding0). Vertex
// positions come from a small vertex buffer (a unit quad); the fixture drives a few
// instanced quads at KNOWN world transforms so the projected depth is exactly
// representable and can be validated bit-tight against a CPU reference.
//
// gl_Position = lightSpace[cascade] * instanceTransform * vec4(inPos, 1.0)
//
// Matrices are stored row-major (the SAME convention the GL shadow producer uploads
// with GL_FALSE). GLSL mat4 is column-major, so a row-major matrix M applied as
// M*v in C-math == (v * M_glsl) in GLSL; we reconstruct the row-major multiply
// explicitly to keep the CPU reference and the GPU bit-identical.
#version 450

layout(location = 0) in vec3 inPos;

// Per-frame view/cascade UBO. cascadeIndex selects which lightSpace row-block the
// draw uses (single map == 0; CSM == one draw per layer with a distinct index).
layout(set = 0, binding = 0, std140) uniform ShadowFrameUBO {
    mat4 lightSpace[4];   // row-major (see header); [cascadeIndex] is active
    int  cascadeIndex;
    int  _pad0;
    int  _pad1;
    int  _pad2;
} uFrame;

// Instance ring (STORAGE_BUFFER_DYNAMIC). Each instance carries a row-major 4x4
// world transform (16 floats). std430 tightly packs the float array.
struct InstanceRec {
    vec4 row0;
    vec4 row1;
    vec4 row2;
    vec4 row3;
};
layout(set = 1, binding = 0, std430) readonly buffer InstanceRing {
    InstanceRec instances[];
} uInstances;

// Row-major mat4 (as 4 rows) times a column vector -> column vector.
vec4 rowMajorMul(vec4 r0, vec4 r1, vec4 r2, vec4 r3, vec4 v) {
    return vec4(dot(r0, v), dot(r1, v), dot(r2, v), dot(r3, v));
}

void main() {
    InstanceRec inst = uInstances.instances[gl_InstanceIndex];
    vec4 world = rowMajorMul(inst.row0, inst.row1, inst.row2, inst.row3, vec4(inPos, 1.0));

    int c = uFrame.cascadeIndex;
    mat4 ls = uFrame.lightSpace[c];
    // lightSpace[c] is stored row-major as 16 linear floats. GLSL fills a mat4 from
    // linear memory COLUMN-major, so GLSL's ls[k] == the k-th group of 4 linear
    // floats == logical ROW k of the row-major matrix. Hence the logical rows are
    // simply ls[0..3]; apply them as a row-major multiply (matching the CPU ref).
    gl_Position = rowMajorMul(ls[0], ls[1], ls[2], ls[3], world);
}
