// VK-P14 MECH TIER-1 — 2-output GBuffer FS.
// =============================================================================
// Co-writes skinned mechs IN-PLACE into the SHARED scene GBuffer (LOAD-preserve,
// GEQUAL reverse-Z, no blend). A winning mech fragment fully overwrites the pixel.
//
// VK-PARITY-MECH-1: ports the GL DEFAULT mech shading = mech.frag compiled with
// MC2_USE_VIEW_UNIFORMS (MC2_MECH_VIEWUNIFORMS default ON) + MC2_IMPORTED_MECH_MATERIAL
// (always defined), u_standardLitEnabled = 0 (MC2_STANDARD_LIT_V1 default OFF):
//     tex = textureLod(u_tex, uv, 0); alpha-test discard (ALPHA_TEST material bit)
//     imported:  tex.rgb = gamma/albedo-scale              (GAMMA-1)
//     c = tex * v_litColor + highlight                       (per-vertex calc_light)
//     imported+HAS_AO:     c *= mix(1, ao, aoStrength)       (AO-1)
//     imported+HAS_NORMAL: Nshade = normal-mapped            (NORMALS-1)
//     c += hemisphere ambient fill                           (MECH-AMBIENT-1)
//     c += Blinn sun sheen / glass heuristic                 (MECH-SPECULAR-V1)
//     c += tex * object light pools                          (POOL-CONSUMERS-1)
//     c = mix(c, fogColor, haze)
// The terms are the SAME functions as GL: shaders/include/mech_shading.hglsl and
// shaders/include/object_light_pool.hglsl. Knobs come from the same C++ producers
// (gpuMechShadingParams / GetObjectLightPoolParams) via the frame feed.
// NOT ported (GL default-OFF): StandardLit GGX + PBR detail maps + SH-IBL +
// back-fill (all inside the u_standardLitEnabled branch), u_debugMode views.
//
//   location=0 lit color  -> sceneColorTex_  (MRT0, RGBA16F).
//     * material OFF: flat silhouette fill (the skinning/axis-swap proof).
//   location=1 GBuffer1   -> sceneNormalTex_ (MRT1, RGBA16F). Matches mech.frag:
//       rc_gbuffer1_screenShadowEligible(N_gbuf) = vec4(N*0.5+0.5, 0.0), N_gbuf = Nshade.
#version 450
#extension GL_GOOGLE_include_directive : require

// Per-frame UBO: generated from RenderCore/gpu_abi/GpuMechFrameUBO.h.
#include "include/mech_frame_ubo_vk.glsl"

layout(set = 0, binding = 3) uniform sampler2D uAlbedo;   // copy-once slot-0 paint scheme
layout(set = 0, binding = 6) uniform sampler2D uMechAo;   // AO-1 (imported; GL unit 6)
layout(set = 0, binding = 7) uniform sampler2D uMechNrm;  // NORMALS-1 (imported; GL unit 7)

// POOL-CONSUMERS-1: CPU copy of the GL binding-28 pool array (GpuTerrainLight, 48B).
struct PoolLight {
    vec3  position;
    uint  lightType;
    vec3  color;
    float closeDistance;
    float farDistance;
    float oneOverDistance;
    float _plPad1;
    float _plPad2;
};
layout(set = 0, binding = 5, std430) readonly buffer MechPoolLightsVK {
    PoolLight poolLights[];
};
#include "../include/object_light_pool.hglsl"
#include "../include/mech_shading.hglsl"

layout(push_constant) uniform MechRunPC {
    uint materialFlags;    // bit0 = ALPHA_TEST (mech.frag ALPHA_TEST_BIT)
} uRun;

layout(location = 0) in vec2 vUv;
layout(location = 1) in vec3 vNormalGL;
layout(location = 2) in vec3 vLitColor;
layout(location = 3) in vec4 vHighlight;
layout(location = 4) in vec4 vFog;        // .rgb fog color, .a haze factor
layout(location = 5) in vec3 vWorldPos;
layout(location = 6) flat in vec3 vMechSunDirGL;
layout(location = 7) flat in int  vMechSunFound;
layout(location = 8) in vec3 vTangentGL;
layout(location = 9) flat in uint vImportedMech;

layout(location = 0) out vec4 oColor;     // MRT0
layout(location = 1) out vec4 oGBuffer1;  // MRT1

void main() {
    vec3 Nshade = normalize(vNormalGL);
    vec3 rgb;
    if (uFrame.materialReady != 0) {
        // Service images expose a complete mip chain for regenerated paint.
        vec4 tex_color = texture(uAlbedo, vUv);
        if ((uRun.materialFlags & 1u) != 0u && tex_color.a < 0.5) discard;
        if (vImportedMech != 0u)
            tex_color.rgb = msh_importedAlbedo(tex_color.rgb, uFrame.shadeB.z, uFrame.shadeB.w);

        rgb = tex_color.rgb * vLitColor;
        rgb += vHighlight.rgb * vHighlight.a;
        if ((vImportedMech & 2u) != 0u) {
            float ao = textureLod(uMechAo, vUv, 0.0).r;
            rgb *= msh_aoFactor(ao, uFrame.shadeC.x);
        }
        if ((vImportedMech & 4u) != 0u && dot(vTangentGL, vTangentGL) > 1e-8)
            Nshade = msh_normalMapped(Nshade, vTangentGL,
                                      textureLod(uMechNrm, vUv, 0.0).xyz, uFrame.shadeC.y);


        if (uFrame.shadeA.y > 0.0 && vMechSunFound != 0) {
            bool isGlass = msh_isGlass(tex_color.rgb, uFrame.shadeB.x, uFrame.shadeB.y);
            if (uFrame.miscI.x != 0) {
                rgb = isGlass ? vec3(0.0, 1.0, 0.0) : vec3(0.15);
            } else {
                vec3 V = normalize(uFrame.cameraWorldPos.xyz - vWorldPos);
                rgb += msh_blinnSpecular(Nshade, V, vMechSunDirGL, isGlass,
                                         uFrame.shadeA.z, uFrame.shadeA.w, uFrame.shadeA.y);
            }
        }

        if (uFrame.poolI.x > 0)
            rgb += tex_color.rgb * olp_objectPoolLight(vWorldPos, uFrame.poolI.x, uFrame.poolI.y,
                                                       uFrame.poolF.x, uFrame.poolF.y,
                                                       uFrame.poolI.z, uFrame.poolI.w,
                                                       uFrame.poolF.z);

        // Per-actor haze fog (mech.frag:404).
        rgb = mix(rgb, vFog.rgb, vFog.a);
    } else {
        // FLAT silhouette fill — the axis-swap / skinning proof (distinct hue vs props).
        rgb = vec3(0.75, 0.45, 0.25) * (0.6 + 0.4 * vUv.y);
    }
    oColor = vec4(rgb, 1.0);

    // GBuffer1: MC2/GL-world shading normal, a=0.0 (screen-shadow-eligible).
    oGBuffer1 = vec4(Nshade * 0.5 + 0.5, 0.0);
}
