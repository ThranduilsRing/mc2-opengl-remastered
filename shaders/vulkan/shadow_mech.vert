// VK mech shadow-caster VS: depth-only instanced + skinned. LOCKSTEP with
// shaders/shadow_mech.vert (GL). mat4(row0..row3) is intentionally the transpose of
// the Stuff bone matrix so boneT*v == row-vec v*M (see mech.vert) -- do NOT "fix" it.
// instance index = gl_InstanceIndex (firstInstance = run instanceBase), so no
// u_instanceBase uniform is needed. Weighted skinning always on (SKIN-RETIRE-1).
#version 450

layout(location = 0) in vec3 a_position;
layout(location = 3) in uvec4 a_boneIndices;   // R8G8B8A8_UINT @ off 32
layout(location = 4) in vec4  a_boneWeights;   // R8G8B8A8_UNORM @ off 36

layout(set = 0, binding = 0, std140) uniform ShadowFrameUBO {
    mat4 lightSpaceMatrix;
    int  instBase;
    int  _pad0;
    int  _pad1;
    int  _pad2;
} uFrame;

struct GpuMechInstance {
    uint typeLodRecordIndex; uint baseBoneOffset; uint lightDataIndex; uint renderFlags;
    vec4 aRGBHighlight; vec4 fogRGB;
    uint objectIdRaw; uint materialIdx; uint _pad2; uint _pad3;
};
struct GpuMechBone { vec4 row0, row1, row2, row3; };
layout(set = 0, binding = 1, std430) readonly buffer InstanceBuffer { GpuMechInstance instances[]; };
layout(set = 0, binding = 2, std430) readonly buffer BoneBuffer { GpuMechBone bones[]; };

void main() {
    GpuMechInstance inst = instances[gl_InstanceIndex];
    mat4 boneT = mat4(0.0);
    for (int i = 0; i < 4; ++i) {
        float w = a_boneWeights[i];
        if (w > 0.0) {
            GpuMechBone b = bones[a_boneIndices[i] + inst.baseBoneOffset];
            boneT += w * mat4(b.row0, b.row1, b.row2, b.row3);
        }
    }
    vec4 worldStuff = boneT * vec4(a_position, 1.0);
    vec3 worldMC2 = vec3(-worldStuff.x, worldStuff.z, worldStuff.y);
    gl_Position = uFrame.lightSpaceMatrix * vec4(worldMC2, 1.0);
}
