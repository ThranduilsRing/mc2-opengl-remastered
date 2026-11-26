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
// Depth was established by the matching alpha-tested prepass; this pass never writes it.
layout(early_fragment_tests) in;

// Per-frame UBO: generated from RenderCore/gpu_abi/GpuMechFrameUBO.h.
#include "include/mech_frame_ubo_vk.glsl"
#include "include/environment_ubo_vk.glsl"
#include "include/lighting_vk.glsl"

#define MC2_TEX_SET 2
#include "include/texture_table_vk.glsl"
#define uAlbedo MC2_TEX(uRun.albedoAo & 65535u, MC2_SAMP_REPEAT_BIASED)
#define uMechAo MC2_TEX(uRun.albedoAo >> 16, MC2_SAMP_REPEAT_BIASED)
#define uMechNrm MC2_TEX(uRun.normal, MC2_SAMP_REPEAT_BIASED)

#define PREC highp
// MECH_PREVIEW (mech_preview.frag.spv, baked from this source with -DMECH_PREVIEW=1): the
// mech-lab panel pass. No terrain set 1 (no light pools, no ground grid), and the HDR
// result is exposed and tonemapped in-shader because the RGBA8 panel target gets no post.
#ifndef MECH_PREVIEW
#undef TERRAIN_RESOURCE_SET
#define TERRAIN_RESOURCE_SET 1
#define TERRAIN_OBJECT_RECEIVER 1
#include "include/terrain_frame_vk.glsl"
#endif
layout(push_constant) uniform MechRunPC {
    uint materialFlags;    // bit0 = ALPHA_TEST (mech.frag ALPHA_TEST_BIT)
    uint poolFirst;
    uint poolCount;
    uint albedoAo;
    uint normal;
} uRun;
#ifndef MECH_PREVIEW
#define MC2_OBJECT_TERRAIN_GRID 1
#include "../include/object_light_pool.hglsl"
#endif
#include "../include/mech_shading.hglsl"

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
layout(location = 10) flat in uint vLegacyVehicle;
layout(location = 11) flat in float vTangentW;

#include "include/mech_material_vk.glsl"

layout(location = 0) out vec4 oColor;     // MRT0
layout(location = 1) out vec4 oGBuffer1;  // MRT1
#ifndef MECH_PREVIEW
layout(location = 12) in vec4 vCurClip;
layout(location = 13) in vec4 vPrevClip;
layout(location = 2) out vec2 oMotion;    // per-object motion vector (previousUV - currentUV, unjittered)
#endif

#ifndef MECH_PREVIEW
// Authored-ORM GGX (same lobe as prop_lighting_compat_vk.glsl propAuthoredColor). Added to display-space rgb like
// msh_blinnSpecular (no sRGB decode). kernelVar is the Kaplanyan-Hoffman normal variance (water_geom.frag:585-587).
vec3 mechGgxSpecular(vec3 N, vec3 V, vec3 negSunDir, vec3 albedo, float metallic, float roughness, float kernelVar, float strength)
{
    vec3 L = normalize(-negSunDir);
    float ndl = dot(N, L);
    vec3 H = L + V;
    if (ndl <= 0.0 || dot(H, H) < 1e-8) return vec3(0.0);
    H = normalize(H);
    float nh = max(dot(N, H), 0.0), nv = max(dot(N, V), 1e-3);
    float r = max(roughness, 0.15);
    float a2 = clamp(r * r * r * r + min(2.0 * kernelVar, 0.18), 0.0, 0.36);
    float d = nh * nh * (a2 - 1.0) + 1.0;
    float D = a2 / max(3.14159265 * d * d, 1e-5);
    float k = (r + 1.0) * (r + 1.0) / 8.0;
    float G = nv / (nv * (1.0 - k) + k) * ndl / (ndl * (1.0 - k) + k);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 F = F0 + (1.0 - F0) * pow(1.0 - clamp(dot(V, H), 0.0, 1.0), 5.0);
    return F * (D * G * ndl / max(4.0 * nv * ndl, 1e-4)) * strength;
}
#endif

void main() {
    vec3 Nshade = normalize(vNormalGL);
#ifndef MECH_PREVIEW
    // Before the discard below so the derivatives stay in uniform control flow.
    vec3 dNx = dFdx(Nshade), dNy = dFdy(Nshade);
    float kernelVar = 0.25 * (dot(dNx, dNx) + dot(dNy, dNy));
#endif
    vec3 rgb;
    if (uFrame.materialReady != 0) {
        // Service images expose a complete mip chain for regenerated paint.
        vec4 tex_color = sampleMechMaterial(uAlbedo, vUv);
        if ((uRun.materialFlags & 1u) != 0u && tex_color.a < 0.5) discard;
        if ((vImportedMech & 1u) != 0u)
            tex_color.rgb = msh_importedAlbedo(tex_color.rgb, uFrame.shadeB.z, uFrame.shadeB.w);

        // Material inputs (SurfaceMaterial); the composition below is the lighting lane's.
        SurfaceMaterial surface = mechSurface(tex_color.rgb, Nshade, vImportedMech, (uRun.materialFlags & 1u) != 0u);
        rgb = tex_color.rgb * vLitColor;
        rgb += vHighlight.rgb * vHighlight.a;
        if (uFrame.miscI.y >= 2 && vLegacyVehicle == 0u) {
            // Mild sky-tinted fresnel rim so silhouettes separate from terrain.
            vec3 Vr = normalize(uFrame.cameraWorldPos.xyz - vWorldPos);
            float rim = pow(1.0 - clamp(dot(Nshade, Vr), 0.0, 1.0), 3.0) * (0.5 + 0.5 * Nshade.z);   // z = up in the MC2 world frame
            rgb += tex_color.rgb * uEnv.hemiSky.rgb * (0.35 * rim);
        }
        if ((vImportedMech & 2u) != 0u)
            rgb *= surface.ao;
        Nshade = surface.normal;
#ifdef MECH_PREVIEW
        {
            // Mech-lab studio rig, fixed relative to the camera (the turntable spins the mech, not the lights):
            // neutral key upper-left, soft fill right, restrained rim, a sky/ground hemisphere ambient and a
            // procedural softbox environment for the specular term.
            vec3 Vs = normalize(uFrame.cameraWorldPos.xyz - vWorldPos);
            vec3 cf = normalize(uFrame.cameraWorldPos.xyz);
            // vWorldPos and Nshade are in the same Z-up MC2 frame as the mission pass.
            vec3 cr = normalize(cross(vec3(0.0, 0.0, 1.0), cf));
            vec3 cu = cross(cf, cr);
            vec3 Lk = normalize(-0.60 * cr + 0.70 * cu + 0.50 * cf);
            vec3 Lf = normalize( 0.80 * cr + 0.05 * cu + 0.55 * cf);
            vec3 Lr = normalize( 0.20 * cr + 0.55 * cu - 0.85 * cf);
            vec3 albedo = tex_color.rgb;
            float nk = max(dot(Nshade, Lk), 0.0);
            float nf = max(dot(Nshade, Lf), 0.0);
            float nr = max(dot(Nshade, Lr), 0.0);
            float hemi = 0.5 + 0.5 * Nshade.z;
            vec3 amb = mix(vec3(0.16), vec3(0.42, 0.44, 0.48), hemi) * 0.65;
            vec3 diff = amb + vec3(1.00, 0.98, 0.95) * 0.85 * nk
                            + vec3(0.75, 0.80, 0.90) * 0.50 * nf
                            + vec3(0.90, 0.95, 1.00) * 0.35 * nr;
            rgb = albedo * diff;
            rgb += vHighlight.rgb * vHighlight.a;
            if ((vImportedMech & 2u) != 0u)
                rgb *= surface.ao;
            float ndv = clamp(dot(Nshade, Vs), 0.0, 1.0);
            vec3 R = reflect(-Vs, Nshade);
            bool authoredOrm = (vImportedMech & 8u) != 0u;
            float rough = clamp(authoredOrm ? surface.roughness : uFrame.shadeA.z, 0.15, 0.9);
            // Stock textures without authored PBR roughness: paint should read as satin.
            if (vImportedMech == 0u) rough = max(rough, 0.60);
            float shin = clamp(2.0 / (rough * rough) - 2.0, 4.0, 400.0);
            float lobe = pow(max(dot(R, Lk), 0.0), shin) * 1.6
                       + pow(max(dot(R, Lr), 0.0), shin * 0.7) * 1.1
                       + pow(max(dot(R, Lf), 0.0), shin * 0.5) * 0.3;
            vec3 envRefl = mix(vec3(0.05, 0.045, 0.04), vec3(0.30, 0.34, 0.42), 0.5 + 0.5 * R.z);
            float fres = 0.05 + 0.95 * pow(1.0 - ndv, 5.0);
            // Authored ORM: f0-weighted (dielectric 0.04), so it keeps the original 2.0 scale; untextured stock paint 0.65.
            vec3 f0 = authoredOrm ? mix(vec3(0.04), albedo, surface.metallic) : vec3(1.0);
            rgb += f0 * (envRefl * (0.25 + fres) + vec3(lobe)) * (0.10 + 0.25 * fres) * (authoredOrm ? 2.0 : 0.65);
        }
#endif


        // Ground vehicles retain their material response and receive shared local pools.
#ifndef MECH_PREVIEW
        if (uFrame.shadeA.y > 0.0 && vMechSunFound != 0 && (vLegacyVehicle == 0u || (vImportedMech & 8u) != 0u)) {
            bool isGlass = msh_isGlass(tex_color.rgb, uFrame.shadeB.x, uFrame.shadeB.y);
            if (uFrame.miscI.x != 0) {
                rgb = isGlass ? vec3(0.0, 1.0, 0.0) : vec3(0.15);
            } else {
                vec3 V = normalize(uFrame.cameraWorldPos.xyz - vWorldPos);
                bool authored = (vImportedMech & 8u) != 0u;
                vec3 f0 = authored ? mix(vec3(0.04), tex_color.rgb, surface.metallic) : vec3(1.0);
                if (authored)
                    rgb += mechGgxSpecular(Nshade, V, vMechSunDirGL, tex_color.rgb, surface.metallic, surface.roughness, kernelVar, uFrame.shadeA.y) * uEnv.sunRelight.rgb;   // takes the sun keys' colour (1 without keys)
                else
                rgb += f0 * msh_blinnSpecular(Nshade, V, vMechSunDirGL, !authored && isGlass,
                                         authored ? surface.roughness : uFrame.shadeA.z, uFrame.shadeA.w, uFrame.shadeA.y);
            }
        }
#endif

#ifndef MECH_PREVIEW
        if (uFrame.poolI.x > 0)
            rgb += tex_color.rgb * olp_objectPoolLight(vWorldPos, uFrame.poolI.x, uFrame.poolI.y,
                                                       uFrame.poolF.x, uFrame.poolF.y,
                                                       uFrame.poolI.z, uFrame.poolI.w,
                                                       uFrame.poolF.z);
#endif

        // Per-actor haze fog (mech.frag:404).
        rgb = mix(rgb, vFog.rgb, vFog.a);
    } else {
        // FLAT silhouette fill — the axis-swap / skinning proof (distinct hue vs props).
        rgb = vec3(0.75, 0.45, 0.25) * (0.6 + 0.4 * vUv.y);
    }
#ifdef MECH_PREVIEW
    // Panel exposure (fogColorRGB.w; the world pass leaves it 0) + a Reinhard shoulder so
    // the spec sheen rolls off instead of clipping on the 8-bit panel target.
    rgb *= max(uFrame.fogColorRGB.w, 0.0) * 0.80; // Preview-only studio exposure.
    // ACES (Narkowicz fit) filmic curve.
    rgb = clamp((rgb * (2.51 * rgb + 0.03)) / (rgb * (2.43 * rgb + 0.59) + 0.14), 0.0, 1.0);
#endif
    oColor = vec4(rgb, 1.0);

    // GBuffer1: MC2/GL-world shading normal, a=0.0 (screen-shadow-eligible).
    oGBuffer1 = vec4(Nshade * 0.5 + 0.5, 0.0);
#ifndef MECH_PREVIEW
    oMotion = (vPrevClip.w > 0.0 && vCurClip.w > 0.0) ? 0.5 * (vPrevClip.xy / vPrevClip.w - vCurClip.xy / vCurClip.w) : vec2(0.0);
#endif
}
