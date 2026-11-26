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
invariant gl_Position;

layout(location = 0) in vec3  a_position;
layout(location = 1) in vec2  a_normalOct;     // s16x2 normalized, octahedral unit normal
layout(location = 2) in vec2  a_uv;
layout(location = 3) in uvec4 a_boneIndices;  // u8x4 -> uvec4
layout(location = 4) in vec4  a_boneWeights;  // u8x4 normalized -> [0,1]
layout(location = 5) in vec2  a_tangentOct;   // s16x2 normalized (NORMALS-1 tangent)
layout(location = 6) in uint  a_aRGBLight;    // BGRA packed (unused)

// Per-frame UBO: generated from RenderCore/gpu_abi/GpuMechFrameUBO.h.
#include "include/mech_frame_ubo_vk.glsl"
#include "include/environment_ubo_vk.glsl"
#include "include/lighting_vk.glsl"

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
#include "../include/gpu_abi_object_lights.gen.hglsl"
#include "../include/axis.hglsl"
#include "../include/mech_render_flags.hglsl"
#define MAX_LIGHTS_IN_WORLD GPU_ABI_OBJECTLIGHTS_LIGHT_TO_WORLD_LEN
#define TG_LIGHT_AMBIENT              0
#define TG_LIGHT_INFINITE            1
#define TG_LIGHT_INFINITEWITHFALLOFF 2
#define TG_LIGHT_POINT               3
#define TG_LIGHT_SPOT                4
#define TG_LIGHT_TERRAIN             5
#define Stuff_SMALL 1e-4

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
#define MC2_ENVIRONMENT_AMBIENT
// One-sun rule (see include/one_sun_vk.glsl; the mech pass carries its own copy of the tail): live direction -> authored direction.
mat3 oneSunRotation() { return mat3(uEnv.sunAuthFromLive[0].xyz, uEnv.sunAuthFromLive[1].xyz, uEnv.sunAuthFromLive[2].xyz); }
//PORT-BEGIN:calc_light
vec3 calc_light(int idx, vec3 normal, vec3 wpos, vec3 base_light, int sunAxisMc2Fix, bool legacyVehicle) {
    const int fillMode = legacyVehicle ? 0 : uFrame.miscI.y;   // MC2_MECH_FILL; ground vehicles keep the original look
    vec3 sunLc = vec3(0.0);
    // Keep only the count local: copying the matrix arrays creates private indexed storage.
    ObjectLights ld;
    ld.numLights = uLights.light[idx].numLights;
    if (ld.numLights.x == 0) return base_light;   // MC2_STATIC_PROP_LIGHTING branch
    vec3 final = base_light;
    vec3 ambient = vec3(0.0);
    int n = min(ld.numLights.x, MAX_LIGHTS_IN_WORLD);
    for (int i = 0; i < n; ++i) {
        int lt = int(uLights.light[idx].light_dir[i].w);
#ifndef MECH_PREVIEW
        // World local lights come from the shared fragment grid, never the
        // capped per-object vertex row. Studio previews keep their own rig.
        if (lt == TG_LIGHT_POINT || lt == TG_LIGHT_SPOT) continue;
#endif
        vec3 lc = uLights.light[idx].light_color[i].xyz;
        if (lt == TG_LIGHT_AMBIENT) {
            if (legacyVehicle) ambient += lc * uEnv.ambRelight.rgb;   // the light row's own ambient, not the environment's; moves like the ground (one-sun rule)
        } else if (lt == TG_LIGHT_INFINITE) {
            // One-sun rule (shaders/vulkan/include/one_sun_vk.glsl): shade against the authored sun direction and undo the live pitch in the row colour.
            // The rotation lives in the MC2 world frame; the row direction is a Stuff vector, so go through the exact swizzles and back.
            vec3 sd = oneSunRotation() * mc2ToRender(uLights.light[idx].light_dir[i].xyz);
            if (sunAxisMc2Fix == 0) sd = renderToMc2(sd);
            float ndlRaw = dot(normal, -sd);
            float ndl = (fillMode >= 2) ? clamp((ndlRaw + 0.25) / 1.25, 0.0, 1.0) : clamp(ndlRaw, 0.0, 1.0);
            lc *= uEnv.pitchUndo.rgb;
            final += incidentLight(vec3(0), ndl * lc, vec3(1), 1.0);
            sunLc = max(sunLc, lc);
        } else if (lt == TG_LIGHT_INFINITEWITHFALLOFF) {
            vec3 lp = uLights.light[idx].light_to_world[i][3].xyz;
            float d = length(wpos - lp); float f;
            if (GetFalloff(d, uLights.light[idx].light_falloff[i].xyz, f)) {
                vec3 sd = (sunAxisMc2Fix != 0) ? mc2ToRender(uLights.light[idx].light_dir[i].xyz) : uLights.light[idx].light_dir[i].xyz;
                float ndl = clamp(dot(normal, -sd), 0.0, 1.0);
                final += ndl * lc * f;
            }
        } else if (lt == TG_LIGHT_POINT) {
            vec3 lp = uLights.light[idx].light_to_world[i][3].xyz;
            vec3 tl = lp - wpos; float d = length(tl); float f;
            if (GetFalloff(d, uLights.light[idx].light_falloff[i].xyz, f) && d > Stuff_SMALL) {
                float ndl = clamp(dot(normal, tl / d), 0.0, 1.0);
                final += ndl * lc * f;
            }
        } else if (lt == TG_LIGHT_SPOT) {
            vec3 lp = uLights.light[idx].light_to_world[i][3].xyz;
            vec3 tl = lp - wpos; float d = length(tl); float f;
            if (GetFalloff(d, uLights.light[idx].light_falloff[i].xyz, f) && d > Stuff_SMALL) {
                float ndl = clamp(dot(normal, tl / d), 0.0, 1.0);
                final += ndl * lc * f;
            }
        }
        // TG_LIGHT_TERRAIN: ignored (spec R2, matches GL calc_light).
    }
    if (fillMode >= 1 && !legacyVehicle) {
        // Hemispheric fill sized from the sun: shade side ~40% of the sun-facing side, sky above, ground bounce below (normal is in the MC2 world frame: z = up, y = north; tests/unit/test_mech_up_axis.cpp).
        float up = clamp(normal.z * 0.5 + 0.5, 0.0, 1.0);
        vec3 fill = sunLc * 0.60 * mix(0.65, 1.0, up);
        final += max(fill - base_light, vec3(0.0));
    }
    return incidentLight(ambient, final, vec3(1), 1.0);
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
layout(location = 10) flat out uint vLegacyVehicle; // renderFlags bit 6: ground vehicle, legacy shading
layout(location = 11) flat out float vTangentW;     // tangent handedness (a_boneIndices.w: 1 = mirrored UV island)
// Per-object motion: unjittered clip of this vertex now and one frame ago (camera reprojection of the
// position minus the object's world translation since last frame). The fragment shader writes the difference.
layout(location = 12) out vec4 vCurClip;
layout(location = 13) out vec4 vPrevClip;

void main() {
    GpuMechInstance inst = uInst.instances[gl_InstanceIndex];

    // Linear-blend skinning (mech.vert:156-170). Stock weights=(1,0,0,0) => single-bone.
    // A PRESKINNED instance's stream was posed by mech_skin.comp: identity here, no bone fetch.
    const bool preskinned = (inst.renderFlags & MECH_RENDER_FLAG_PRESKINNED) != 0u;
    mat4 boneT = mat4(preskinned ? 1.0 : 0.0);
    for (int bi = 0; bi < 4 && !preskinned; ++bi) {
        float w = a_boneWeights[bi];
        if (w > 0.0) {
            GpuMechBone bn = uBones.bones[a_boneIndices[bi] + inst.baseBoneOffset];
            boneT += w * mat4(bn.row0, bn.row1, bn.row2, bn.row3);
        }
    }

    vec4 worldStuff = boneT * vec4(a_position, 1.0);
    vec3 worldMC2 = mc2ToRender(worldStuff.xyz);   // Stuff->MC2 swap
    vec3 nLocal = vec3(a_normalOct.xy, 1.0 - abs(a_normalOct.x) - abs(a_normalOct.y));   // octahedral decode (snorm16x2)
    if (nLocal.z < 0.0)
        nLocal.xy = (1.0 - abs(nLocal.yx)) * vec2(nLocal.x >= 0.0 ? 1.0 : -1.0, nLocal.y >= 0.0 ? 1.0 : -1.0);
    vec3 normalStuff = mat3(boneT) * normalize(nLocal);
    vec3 worldNormal = normalize(mc2ToRender(normalStuff));

    vec4 clip = uFrame.worldToClipGL * vec4(worldMC2, 1.0);
    vCurClip = clip;
    vPrevClip = preskinned ? uFrame.prevFromCurClip * clip : clip;
    if (uFrame.miscI.z > 0) {
        // The same vertex skinned with last frame's bone palette (stored after the current one; miscI.z = bone count): pose motion
        // (root rotation and translation, limbs) in one step. Unkeyed actors carry the current pose there, so they get the camera vector.
        // A pre-skinned stream holds the posed position and keeps the indices and weights: undo the current blend (affine, no mat4
        // inverse: translations reach ~12800 units), then apply the previous one.
        mat4 curBoneT = mat4(0.0);
        mat4 prevBoneT = mat4(0.0);
        for (int bi = 0; bi < 4; ++bi) {
            float w = a_boneWeights[bi];
            if (w > 0.0) {
                const uint idx = a_boneIndices[bi] + inst.baseBoneOffset;
                GpuMechBone bn = uBones.bones[uFrame.miscI.z + idx];
                prevBoneT += w * mat4(bn.row0, bn.row1, bn.row2, bn.row3);
                if (preskinned) {
                    GpuMechBone bc = uBones.bones[idx];
                    curBoneT += w * mat4(bc.row0, bc.row1, bc.row2, bc.row3);
                }
            }
        }
        vec3 prevPos = a_position;
        bool usePrev = true;
        if (preskinned) {
            const mat3 curRot = mat3(curBoneT);
            // Identical matrices (idle actor) or a singular blend: keep the camera vector exactly.
            usePrev = curBoneT != prevBoneT && abs(determinant(curRot)) >= 1e-8;
            if (usePrev) prevPos = inverse(curRot) * (a_position - curBoneT[3].xyz);
        }
        if (usePrev) {
            const vec3 prevWorldMC2 = mc2ToRender((prevBoneT * vec4(prevPos, 1.0)).xyz);
            vPrevClip = uFrame.prevFromCurClip * (uFrame.worldToClipGL * vec4(prevWorldMC2, 1.0));
        }
    }
    clip.z += uFrame.objectDepthBias * clip.w;   // mech.vert:187
    gl_Position = clip;

    // Per-vertex lighting — mech.vert:204-235 (u_lightingMode=1, lightsOut branch).
    // worldNormal is mc2ToRender(Y-up bone-space normal) = MC2 world frame: .z is up, .y is north (tests/unit/test_mech_up_axis.cpp).
    // Ground vehicles keep their legacy lighting: the row's ambient and sun, no environment ambient; point and spot lights apply (stock vehicles were lit by them).
    bool legacyVehicle = (inst.renderFlags & MECH_RENDER_FLAG_LEGACY_VEHICLE) != 0u;
    bool mechFill = !legacyVehicle && uFrame.miscI.y >= 1;   // MC2_MECH_FILL applies to mechs only
    float ambUp = worldNormal.z;
    vec3 environmentAmbient = uEnv.hemiSky.rgb * uEnv.ambRelight.rgb   // moves like the ground (R x authored/live ambient; one-sun rule)
        * mix(0.5, 1.0, clamp(ambUp * 0.5 + 0.5, 0.0, 1.0));
    const uint  kRenderFlagLightsOut = 0x2u;
    bool lightsOut = (inst.renderFlags & kRenderFlagLightsOut) != 0u;
    vec3 baseLight;
    if (lightsOut) {
        baseLight = environmentAmbient;
    } else if (uFrame.lightingReady != 0) {
        vec3 base = legacyVehicle ? vec3(0.0) : environmentAmbient;
        vec3 litRGB = calc_light(int(inst.lightDataIndex), worldNormal, worldMC2, base, (uFrame.sunAxisMc2Fix != 0 || mechFill) ? 1 : 0, legacyVehicle);
        baseLight = clamp(litRGB + inst.aRGBHighlight.rgb * inst.aRGBHighlight.a, 0.0, 1.0);
    } else {
        // LightsData not yet copied — mirror mech.vert's Slice-A passthrough (flat white).
        baseLight = clamp(environmentAmbient + inst.aRGBHighlight.rgb * inst.aRGBHighlight.a, 0.0, 1.0);
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
    if ((inst.renderFlags & MECH_RENDER_FLAG_ORM) != 0u) vImportedMech |= 8u;
    vLegacyVehicle = legacyVehicle ? 1u : 0u;
    vTangentW = (a_boneIndices.w != 0u) ? -1.0 : 1.0;
    {
        vec2 te = a_tangentOct;                                  // s16-normalized [-1,1]
        vec3 tLocal = vec3(te.xy, 1.0 - abs(te.x) - abs(te.y));
        if (tLocal.z < 0.0)
            tLocal.xy = (1.0 - abs(tLocal.yx)) *
                        vec2(tLocal.x >= 0.0 ? 1.0 : -1.0, tLocal.y >= 0.0 ? 1.0 : -1.0);
        tLocal = normalize(tLocal);
        vec3 tStuff = mat3(boneT) * tLocal;
        vec3 tGL    = mc2ToRender(tStuff);       // same swap as worldNormal
        tGL = tGL - worldNormal * dot(worldNormal, tGL);
        float tLen = length(tGL);
        vTangentGL = (tLen > 1e-5) ? (tGL / tLen) : vec3(0.0);
    }
    vWorldPos     = worldMC2;
    vMechSunDirGL = vec3(0.0);
    vMechSunFound = 0;
    if (uFrame.lightingReady != 0) {
        int specIndex = int(inst.lightDataIndex);
        int n_spec = min(uLights.light[specIndex].numLights.x, MAX_LIGHTS_IN_WORLD);
        for (int i = 0; i < n_spec; ++i) {
            int lt = int(uLights.light[specIndex].light_dir[i].w);
            if (lt == TG_LIGHT_INFINITE || lt == TG_LIGHT_INFINITEWITHFALLOFF) {
                vec3 sdStuff  = uLights.light[specIndex].light_dir[i].xyz;
                vMechSunDirGL = mc2ToRender(sdStuff);
                vMechSunFound = 1;
                break;
            }
        }
    }
}
