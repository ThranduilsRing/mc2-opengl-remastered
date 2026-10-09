// VK-P14 MECH TIER-1 — skinned instanced mech VS.
// =============================================================================
// SUB-SLICE 1: skinning + flat color (proven — silhouette parity).
// VK-MECH-MATERIAL-LIGHTING-1: adds per-vertex calc_light Lambert (the GL default
// mech path, u_lightingMode=1) so the FS can do albedo * v_litColor + highlight,
// matching mech.frag/mech.vert when MC2_MECH_VIEWUNIFORMS + MC2_STANDARD_LIT_V1 are
// OFF (both default-OFF). NOT the PBR/StandardLit GGX path — that requires the
// viewuniforms variant and is not the default deploy target.
//
// Port of shaders/mech.vert's skinning + world->clip + calc_light, into the VK
// shared-GBuffer co-write pipeline. Linear-blend skinning over 4 bone weights
// (mech.vert:156-180), then the Stuff->MC2 axis swap worldMC2 = vec3(-x, z, y).
//
// Bone convention (mech.vert:5-7): GpuMechBone stores Stuff LinearMatrix4D rows;
// mat4(row0..row3) fills GLSL COLUMNS => boneT is the TRANSPOSE of the Stuff
// matrix and boneT * vec4(pos,1) is row-vector math. Do NOT "fix" the transpose.
#version 450
#extension GL_GOOGLE_include_directive : require

layout(location = 0) in vec3  a_position;
layout(location = 1) in vec3  a_normal;
layout(location = 2) in vec2  a_uv;
layout(location = 3) in uvec4 a_boneIndices;  // u8x4 -> uvec4
layout(location = 4) in vec4  a_boneWeights;  // u8x4 normalized -> [0,1]
layout(location = 5) in vec2  a_tangentOct;   // s16x2 normalized (NORMALS-1 tangent)
layout(location = 6) in uint  a_aRGBLight;    // BGRA packed (unused)

layout(set = 0, binding = 0, std140) uniform MechFrameUBO {
    mat4  worldToClipGL;   // gos_GetMechFixBMVPMat4() — GL_FALSE (column-major load)
    float objectDepthBias; // OBJECT_DEPTH_BIAS
    int   materialReady;   // 1 = FS samples the copy-once albedo
    int   lightingReady;   // 1 = calc_light valid (LightsData copied); else flat white
    int   _pad1;
    vec4  fogColorRGB;     // g_scene.fogColor.rgb (.a unused)
    // VK-PARITY-MECH-1 FS shading block (VS ignores; layout lockstep with the FS +
    // vulkan_mech_island.cpp MechFrameUBO).
    vec4  cameraWorldPos;
    vec4  shadeA;
    vec4  shadeB;
    vec4  shadeC;
    vec4  poolF;
    ivec4 poolI;
    ivec4 miscI;
} uFrame;

// Per-instance record (64B): generated from RenderCore/gpu_abi/GpuMechInstance.h.
// fogRGB.a is the per-actor haze factor.
#include "../include/gpu_abi_mech_instance.gen.hglsl"
layout(set = 0, binding = 1, std430) readonly buffer InstanceBuffer {
    GpuMechInstance instances[];
} uInst;

// Per-frame bone palette (GpuMechBone 64B; 4 explicit rows).
struct GpuMechBone { vec4 row0, row1, row2, row3; };
layout(set = 0, binding = 2, std430) readonly buffer BoneBuffer {
    GpuMechBone bones[];
} uBones;

// ---------------------------------------------------------------------------
// LightsData — verbatim port of shaders/include/lighting.hglsl's ObjectLights +
// calc_light (the 4-param Slice-2.C kernel). MAX_LIGHTS_IN_WORLD=32 (LIGHT-ABI-
// WIDEN-STAGE0-1 cap). Copied per-frame from the GL SSBO (binding 20). std430 ==
// the GL side's std430 LightsData; byte-identical layout (mat4/vec4-array+ivec4).
// ---------------------------------------------------------------------------
#define MAX_LIGHTS_IN_WORLD 32
#define TG_LIGHT_AMBIENT              0
#define TG_LIGHT_INFINITE            1
#define TG_LIGHT_INFINITEWITHFALLOFF 2
#define TG_LIGHT_POINT               3
#define TG_LIGHT_SPOT                4
#define TG_LIGHT_TERRAIN             5
#define Stuff_SMALL 1e-4

struct ObjectLights {
    mat4 light_to_world[MAX_LIGHTS_IN_WORLD];
    vec4 light_dir[MAX_LIGHTS_IN_WORLD];
    vec4 light_color[MAX_LIGHTS_IN_WORLD];
    vec4 light_falloff[MAX_LIGHTS_IN_WORLD];
    ivec4 numLights;
};
layout(set = 0, binding = 4, std430) readonly buffer LightsData {
    ObjectLights light[];
} uLights;

bool GetFalloff(float len, vec3 fd, out float falloff) {
    if (len <= fd.x) { falloff = 1.0; return true; }
    if (len >= fd.y) { falloff = 0.0; return false; }
    falloff = (fd.y - len) * fd.z;
    return true;
}

// mech.vert calls calc_light with MC2_STATIC_PROP_LIGHTING defined -> the
// numLights==0 early-return preserves base_light (not vec3(1)). Match that.
//PORT-BEGIN:calc_light
vec3 calc_light(int idx, vec3 normal, vec3 wpos, vec3 base_light) {
    ObjectLights ld = uLights.light[idx];
    if (ld.numLights.x == 0) return base_light;   // MC2_STATIC_PROP_LIGHTING branch
    vec3 final = base_light;
    vec3 ambient = vec3(0.0);
    int n = min(ld.numLights.x, MAX_LIGHTS_IN_WORLD);
    for (int i = 0; i < n; ++i) {
        int lt = int(ld.light_dir[i].w);
        vec3 lc = ld.light_color[i].xyz;
        if (lt == TG_LIGHT_AMBIENT) {
            ambient += lc;
        } else if (lt == TG_LIGHT_INFINITE) {
            float ndl = clamp(dot(normal, -ld.light_dir[i].xyz), 0.0, 1.0);
            final += ndl * lc;
        } else if (lt == TG_LIGHT_INFINITEWITHFALLOFF) {
            vec3 lp = ld.light_to_world[i][3].xyz;
            float d = length(wpos - lp); float f;
            if (GetFalloff(d, ld.light_falloff[i].xyz, f)) {
                float ndl = clamp(dot(normal, -ld.light_dir[i].xyz), 0.0, 1.0);
                final += ndl * lc * f;
            }
        } else if (lt == TG_LIGHT_POINT) {
            vec3 lp = ld.light_to_world[i][3].xyz;
            vec3 tl = lp - wpos; float d = length(tl); float f;
            if (GetFalloff(d, ld.light_falloff[i].xyz, f) && d > Stuff_SMALL) {
                float ndl = clamp(dot(normal, tl / d), 0.0, 1.0);
                final += ndl * lc * f;
            }
        } else if (lt == TG_LIGHT_SPOT) {
            vec3 lp = ld.light_to_world[i][3].xyz;
            vec3 tl = lp - wpos; float d = length(tl); float f;
            if (GetFalloff(d, ld.light_falloff[i].xyz, f) && d > Stuff_SMALL) {
                float ndl = clamp(dot(normal, tl / d), 0.0, 1.0);
                final += ndl * lc * f;
            }
        }
        // TG_LIGHT_TERRAIN: ignored (spec R2, matches GL calc_light).
    }
    return final + ambient;
}
//PORT-END:calc_light

layout(location = 0) out vec2 vUv;
layout(location = 1) out vec3 vNormalGL;   // MC2/GL-world normal (swapped), for GBuffer1
layout(location = 2) out vec3 vLitColor;   // per-vertex Lambert lit color (v_litColor.rgb)
layout(location = 3) out vec4 vHighlight;  // inst.aRGBHighlight forwarded to FS
layout(location = 4) out vec4 vFog;        // .rgb=g_scene fog color, .a=per-actor haze
// VK-PARITY-MECH-1: the mech.vert MC2_USE_VIEW_UNIFORMS + MC2_IMPORTED_MECH_MATERIAL
// varyings (both compiled into the GL default variant).
layout(location = 5) out vec3 vWorldPos;            // worldMC2 (== GL v_worldPos)
layout(location = 6) flat out vec3 vMechSunDirGL;   // light_dir.xyz Stuff->GL swapped
layout(location = 7) flat out int  vMechSunFound;
layout(location = 8) out vec3 vTangentGL;           // NORMALS-1 world tangent
layout(location = 9) flat out uint vImportedMech;   // renderFlags >> 3 & 7

void main() {
    GpuMechInstance inst = uInst.instances[gl_InstanceIndex];

    // Linear-blend skinning (mech.vert:156-170). Stock weights=(1,0,0,0) => single-bone.
    mat4 boneT = mat4(0.0);
    for (int bi = 0; bi < 4; ++bi) {
        float w = a_boneWeights[bi];
        if (w > 0.0) {
            GpuMechBone bn = uBones.bones[a_boneIndices[bi] + inst.baseBoneOffset];
            boneT += w * mat4(bn.row0, bn.row1, bn.row2, bn.row3);
        }
    }

    vec4 worldStuff = boneT * vec4(a_position, 1.0);
    vec3 worldMC2 = vec3(-worldStuff.x, worldStuff.z, worldStuff.y);   // Stuff->MC2 swap
    vec3 normalStuff = mat3(boneT) * a_normal;
    vec3 worldNormal = normalize(vec3(-normalStuff.x, normalStuff.z, normalStuff.y));

    vec4 clip = uFrame.worldToClipGL * vec4(worldMC2, 1.0);
    clip.z += uFrame.objectDepthBias * clip.w;   // mech.vert:187
    gl_Position = clip;

    // Per-vertex lighting — mech.vert:204-235 (u_lightingMode=1, lightsOut branch).
    const float kAmbientFloor = 0.35;
    const uint  kRenderFlagLightsOut = 0x2u;
    bool lightsOut = (inst.renderFlags & kRenderFlagLightsOut) != 0u;
    vec3 baseLight;
    if (lightsOut) {
        baseLight = vec3(kAmbientFloor);
    } else if (uFrame.lightingReady != 0) {
        vec3 base = vec3(kAmbientFloor);
        vec3 litRGB = calc_light(int(inst.lightDataIndex), worldNormal, worldMC2, base);
        baseLight = clamp(litRGB + inst.aRGBHighlight.rgb * inst.aRGBHighlight.a, 0.0, 1.0);
    } else {
        // LightsData not yet copied — mirror mech.vert's Slice-A passthrough (flat white).
        baseLight = clamp(vec3(1.0) + inst.aRGBHighlight.rgb * inst.aRGBHighlight.a, 0.0, 1.0);
    }

    vUv       = a_uv;
    vNormalGL = worldNormal;
    vLitColor = baseLight;
    vHighlight = inst.aRGBHighlight;
    vFog      = vec4(uFrame.fogColorRGB.rgb, inst.fogRGB.a);

    // VK-PARITY-MECH-1: port of mech.vert's imported-mech marker + octahedral
    // tangent decode (MC2_IMPORTED_MECH_MATERIAL) and the specular sun finder
    // (MC2_USE_VIEW_UNIFORMS) -- verbatim, same SSBO bytes.
    vImportedMech = (inst.renderFlags >> 3) & 7u;
    {
        vec2 te = a_tangentOct;                                  // s16-normalized [-1,1]
        vec3 tLocal = vec3(te.xy, 1.0 - abs(te.x) - abs(te.y));
        if (tLocal.z < 0.0)
            tLocal.xy = (1.0 - abs(tLocal.yx)) *
                        vec2(tLocal.x >= 0.0 ? 1.0 : -1.0, tLocal.y >= 0.0 ? 1.0 : -1.0);
        tLocal = normalize(tLocal);
        vec3 tStuff = mat3(boneT) * tLocal;
        vec3 tGL    = vec3(-tStuff.x, tStuff.z, tStuff.y);       // same swap as worldNormal
        tGL = tGL - worldNormal * dot(worldNormal, tGL);
        float tLen = length(tGL);
        vTangentGL = (tLen > 1e-5) ? (tGL / tLen) : vec3(0.0);
    }
    vWorldPos     = worldMC2;
    vMechSunDirGL = vec3(0.0);
    vMechSunFound = 0;
    if (uFrame.lightingReady != 0) {
        ObjectLights ld_spec = uLights.light[int(inst.lightDataIndex)];
        int n_spec = min(ld_spec.numLights.x, MAX_LIGHTS_IN_WORLD);
        for (int i = 0; i < n_spec; ++i) {
            int lt = int(ld_spec.light_dir[i].w);
            if (lt == TG_LIGHT_INFINITE || lt == TG_LIGHT_INFINITEWITHFALLOFF) {
                vec3 sdStuff  = ld_spec.light_dir[i].xyz;
                vMechSunDirGL = vec3(-sdStuff.x, sdStuff.z, sdStuff.y);
                vMechSunFound = 1;
                break;
            }
        }
    }
}
