// VK-P11 STATIC-PROP COLOR — TIER-1 (parity) 2-output GBuffer FS.
// =============================================================================
// Co-writes props IN-PLACE into the SHARED scene GBuffer attachments (no side-image,
// no composite): the VK renderpass LOADs the GL-authored color(MRT0) + normal(MRT1)
// + shared depth, depth-tests GEQUAL reverse-Z, and only overwrites where the prop
// wins the depth test. No blend (opaque, colorWriteMask 0xF, a=1) — a winning prop
// fragment fully replaces the terrain GBuffer pixel exactly as the GL prop pass does.
//
//   location=0 lit color  -> sceneColorTex_  (MRT0, RGBA16F).
//     * MC2_VK_PROP_MATERIAL OFF (or albedo not ready / vLayer<0): flat teal
//       fail-soft teal placeholder, never crashes/holes.
//     * ON: samples the REAL alpha-OFF albedo GL_TEXTURE_2D_ARRAY (VK copy-once
//       import of s_texArrayOff) at float(vLayer) and multiplies it by the
//       VERTEX-BAKED lit color vArgb (the ACTUAL GL default prop model — NOT PBR):
//         oColor = albedo * vArgb   (mirrors static_prop.frag:346
//                                    FragColor = tex_color * vec4(v_argb.rgb, a)).
//       a=1 (opaque, in-place — NOT premult).
//       ★PARITY: vArgb reproduces the SHAPE of the GL default term
//       (base_light seed from the per-vertex a_aRGBLight + ambient + terrain-sun*NdotL,
//       see color_prop_tier1.vert). The per-type magic-color SSBO and the ObjectLights
//       UBO (ambient + per-actor sun color) are NOT fed to the island, so the ambient
//       and sun color are stand-in constants — this is the correct MODEL with an
//       approximate light, NOT byte-parity. computePBR is intentionally NOT used here
//       (props are vertex-baked; the GL GGX block is gated OFF by default). Remaining
//       diff after the screen shadow pass = the ambient/sun-color feed gap + dynamic
//       screen-space shadow.
//   location=1 GBuffer1    -> sceneNormalTex_ (MRT1, RGBA16F). BIT-ADJACENT to the GL
//                            producer static_prop.frag:657-661, which selects between
//                            TWO classes off the packet alpha-test bit:
//                              solid      rc_gbuffer1_screenShadowEligible_noSelfShadow(N)
//                                         = vec4(normalize(N)*0.5+0.5, 0.25)
//                              alpha-test ..._noSelfShadow_alphaTest(N)
//                                         = vec4(normalize(N)*0.5+0.5, 0.1875)
//                            ★N is the STUFF-SPACE (un-swapped) normal from the VS.
//                            Both are the static-prop shadow class (static-map shadow
//                            OK, no dynamic-cascade self-shadow); shadow_screen.frag
//                            gates rc_isStaticPropNoSelfShadow (0.1 < a <= 0.5), which
//                            matches both. 0.1875 exists ONLY so the default-OFF
//                            MC2_PROP_SHADOW_RECEIVE opt-in can exclude lattices.
//                            VK-GBUFFER1-CENSUS-1 made VK emit both; see main().
//
// ===== CENSUS WITNESS / DECOY -- DO NOT DELETE (VK-GBUFFER1-CENSUS-1) =========
// The next line is a COMMENT that quotes a complete, semicolon-terminated
// attachment-1 write whose value is deliberately NOT this shader's and is NOT in
// scripts/check-render-contract-gbuffer1.allowlist:
//
//     oGBuffer1 = vec4(0.0, 0.0, 0.0, 0.9);
//
// scripts/check-render-contract-gbuffer1.sh asserts this file censuses to EXACTLY
// ONE attachment-1 write. If its `//` comment stripper ever breaks, this decoy is
// counted as code: the count becomes 2 AND the unlisted 0.9 value trips the
// forward ratchet, so the check fails LOUDLY instead of quietly scanning prose as
// if it were live code. A scanner that has gone blind reports a clean tree, which
// is indistinguishable from a correct one until someone trusts it.
// Deleting these lines, or adding a second real write here, fails the check.
// =============================================================================
#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_buffer_reference : require
layout(constant_id = 0) const int kBaseQuality = 2; // Potato / Low / Medium+High
// 1 = the opaque core pipeline: its packets have no alpha-ON layer, no per-packet alpha-test bit and are not
// fringe rows, so every discard below is dead for them. Compiling the discards out lets the driver run early Z.
layout(constant_id = 2) const int kOpaqueOnly = 0;
layout(constant_id = 10) const int kPlanarReflection = 0;
layout(constant_id = 12) const int kReflectCutDetail = 1;
layout(constant_id = 13) const int kReflectCutPools = 1;
layout(constant_id = 14) const int kReflectCutMovers = 1;

// Per-frame UBO: generated from RenderCore/gpu_abi/GpuColorFrameUBO.h.
#include "include/color_frame_ubo_vk.glsl"

// PROP-ARRAY-BUCKETS: each albedo group is one array per size bucket; vLayer / vLayerOn carry the bucket.
#include "include/prop_array_bucket.glsl"
// VK-P11 STATIC-PROP MATERIAL: alpha-OFF albedo arrays (copy-once import).
layout(set = 0, binding = 3) uniform sampler2DArray uAlbedo[PROP_ALPHA_ON_BUCKETS];
// VK-P11 STATIC-PROP MATERIAL alpha-ON: alpha-MIXED prop arrays (copy-once import of
// s_texArrayOn). Sampled with a 0.5 alpha-test discard, matching GL static_prop.frag.
layout(set = 0, binding = 6) uniform sampler2DArray uAlbedoOn[PROP_ALPHA_ON_BUCKETS];

layout(location = 0) in vec2 vUv;
layout(location = 1) in vec3 vNormalStuff;
flat layout(location = 2) in int  vLayer;
layout(location = 3) in vec3 vArgb;   // VERTEX-BAKED lit color (GL default prop model)
flat layout(location = 4) in int  vLayerOn;  // alpha-ON albedo array layer (-1 = none)
flat layout(location = 5) in vec2 vUvScale;  // FENCE-UVSCALE-1: this packet's uvScaleX/Y
layout(location = 6) in vec3 vWorldPos;
flat layout(location = 7) in vec3 vSunDirStuff;
flat layout(location = 8) in vec3 vSunColor;
flat layout(location = 9) in vec4 vHighlight;   // objective / selection highlight (static_prop.frag: c.rgb += rgb * a)
layout(location = 10) in vec3 vMaterialSeed;
flat layout(location = 11) in vec3 vMaterialAmbient;
flat layout(location = 12) in vec3 vMaterialSun;
flat layout(location = 13) in uint vMaterialFlags;
layout(location = 14) in vec4 vTree;
flat layout(location = 15) in uvec2 vWindow;
flat layout(location = 16) in uvec3 vTeam;

// This draw's PropPacket row (VS: packetBase + gl_DrawID); the packet table the include declares is read through it.
flat layout(location = 17) in uint vPacketIdx;
#include "include/prop_packet_vk.glsl"

layout(location = 0) out vec4 oColor;     // MRT0 lit color
layout(location = 1) out vec4 oGBuffer1;  // MRT1 normal + shadow class
layout(location = 2) out uint oObjectId;   // MRT2 R32_UINT object id (RenderWorld handle bits, 0 = background)

// PROP-COVERAGE-1: partial alpha (tree cards, fences, alpha-mixed buildings) is resolved by a two-pass
// draw, not by the Bayer 4x4 screen-door dither this island used to share with GL static_prop.frag. The
// stipple read as a fixed checkerboard over every partly covered texel (leaf edges, and at distance
// whole canopies and lattices, whose box-filtered mips average to mid alpha), and nothing in the VK
// chain (no MSAA, no TAA) resolves it.
//   core pass   (coverageFringePass 0): hard cutout at kCoverCut, opaque, writes depth and GBuffer1.
//   fringe pass (coverageFringePass 1): drawn after every core, alpha-tested packets only. The fragments
//     the core cut are alpha-blended over the scene with alpha a/kCoverCut (continuous at the cut);
//     the pipeline depth-tests without writing depth and masks GBuffer1, so a fringe never punches
//     a hole in what is behind it and never rewrites the normal or shadow class.
// The cut rises with the mip level: at mip 0 alpha is the authored cutout (crisp 0.5 edge); once
// the texture is minified, alpha is box-filtered COVERAGE (a chain-link fence at distance averages
// to ~0.5-0.7), and a 0.5 cut would turn it into a solid slab -- so minified partial coverage goes
// to the blended fringe at close to its true alpha instead.
const float kCoverMin = 0.02;   // below this a fringe is invisible; skip the blend
float propCoverage(float a, float lod) {
    // Mode 2 uses this same LOD-aware coverage; only its fringe scheduling
    // differs (after vegetation). A fixed cutoff loses minified wire entirely.
    // Canopies retain depth-writing leaf cores against bright sky at every mip.
    float cut = vTree.y > 0.5 ? 0.5 : mix(0.5, 0.9, clamp(lod * 0.5, 0.0, 1.0));
    if (uPacket.coverageFringePass == 0) {
        if (a < cut) discard;
        return 1.0;
    }
    if (a >= cut || a < kCoverMin) discard;
    return a / cut;
}
#include "../include/axis.hglsl"

// Night light pools: CPU copy of the GL binding-28 pool array (GpuTerrainLight, 48B), the same set GL static_prop.frag reads.
#define PREC highp
#define TERRAIN_RESOURCE_SET 1
#define TERRAIN_OBJECT_RECEIVER 1
#include "include/terrain_frame_vk.glsl"
#include "include/ground_lighting.glsl"
layout(set = 0, binding = 14) uniform sampler2DShadow uTreeObjectMap;
#define MC2_OBJECT_TERRAIN_GRID 1
#define MC2_OBJECT_SHORT_POOL (kBaseQuality == 2)
#define MC2_OBJECT_POOL_BUDGET (kBaseQuality == 0 ? 4 : kBaseQuality == 1 ? 8 : 0)
#include "../include/object_light_pool.hglsl"

// GL static_prop.frag's pool term: albedo-modulated additive pool light at the MC2-frame world position.
vec3 propPoolLight(vec3 albedo) {
    if ((kPlanarReflection != 0 && kReflectCutPools != 0 && uFrame.nightDarkness <= 0.0) || uFrame.poolI.x <= 0) return vec3(0.0);
    return albedo * olp_objectPoolLight(vWorldPos, uFrame.poolI.x, uFrame.poolI.y,
                                        uFrame.poolF.x, uFrame.poolF.y,
                                        uFrame.poolI.z, uFrame.poolI.w,
                                        uFrame.poolF.z);
}

#include "include/prop_sh_l2.glsl"
#include "include/prop_material_vk.glsl"
#include "include/prop_lighting_compat_vk.glsl"

float treeVisibility(vec3 N) {
    float visibility = kPlanarReflection != 0 && kReflectCutMovers != 0 ? 1.0 : moverVisibility(vWorldPos, N);
    if (uFrame.poolF.w > 1.0 && uT.enableShadows != 0) {
        vec4 p = uT.staticLightSpaceMatrix * vec4(vWorldPos + N * 0.25, 1.0);
        vec3 q = p.xyz / p.w; q.xy = q.xy * 0.5 + 0.5;
        if (q == clamp(q,0.0,1.0)) {
            vec4 taps = textureGather(uTreeObjectMap, q.xy, q.z - 3.0/65535.0);
            vec2 f = fract(q.xy * vec2(textureSize(uTreeObjectMap,0)) - 0.5);
            visibility = min(visibility, mix(mix(taps.w,taps.z,f.x),mix(taps.x,taps.y,f.x),f.y));
        }
    }
    return visibility;
}
void main() {
    if (kPlanarReflection != 0 && vWorldPos.z < uT.shorelineWaterElev) discard;
    vec3 rgb;
    vec3 materialAlbedo = vec3(0);
    vec3 poolLight = vec3(0);
    bool haveAlbedo = false;
    float materialAO = 1.0;
    vec3 outputNormalStuff = vNormalStuff;
    // VK-GBUFFER1-CENSUS-1: which GBuffer1 shadow CLASS this fragment belongs to.
    // Set true by the alpha-ON branch below -- see the oGBuffer1 write at the end
    // of main() for why the distinction exists and what its granularity is.
    bool alphaTestClass = false;
    float cover = 1.0;   // PROP-COVERAGE-1 blend alpha (1 in the core pass)
    if (kOpaqueOnly == 0 && uFrame.materialReadyOn != 0 && vLayerOn >= 0) {
        alphaTestClass = true;
        // ALPHA-ON group (alpha-MIXED buildings): sample the alpha-ON array and apply the
        // SAME 0.5 alpha-test discard the GL producer uses (static_prop.frag:236:
        // (materialFlags & ALPHA_TEST_BIT) && tex_color.a < 0.5 -> discard). A discarded
        // fragment leaves the LOADed terrain GBuffer pixel untouched (in-place co-write).
        // Explicit LOD from the sub-region-scaled UV gradients, not clamped above: GL samples the deep box-filtered mips too, whose averaged alpha
        // the Bayer dither below turns into the see-through stipple of thin lattices (hex props, fences) at distance; a level-1 clamp
        // kept the lattice bars opaque and dark where GL reads see-through.
        // FENCE-UVSCALE-1: the crop to this packet's sub-region of the group's max-dims layer mirrors GL's
        // `fract(v_uv) * vec2(uvScaleX, uvScaleY)`.
        vec2  uvContinuousOn = vUv * vUvScale;
        vec2  uvSampledOn = fract(vUv) * vUvScale;
        const int bucketOn = propBucketOf(vLayerOn);
        vec2  tsOn = vec2(textureSize(uAlbedoOn[nonuniformEXT(bucketOn)], 0).xy);
        vec2  duv1 = dFdx(uvContinuousOn) * tsOn;
        vec2  duv2 = dFdy(uvContinuousOn) * tsOn;
        float lodOn = 0.5 * log2(max(max(dot(duv1, duv1), dot(duv2, duv2)), 1e-12));
        lodOn = max(lodOn, 0.0);
        vec4 texc = textureLod(uAlbedoOn[nonuniformEXT(bucketOn)], vec3(uvSampledOn, float(propBucketLayer(vLayerOn))), lodOn);
        cover = propCoverage(texc.a, lodOn);   // PROP-COVERAGE-1 (replaces the Bayer screen-door dither)
        // ★VK-FLIP-FIELD-REPORT-1 BLACK-FENCE FIX: the alpha-ON group is the ALPHA_TEST
        // class (tree cards / fences / foliage). GL static_prop.frag:339 floors their lit
        // color at max(litRgb, vec3(0.5)) so the dark, light-facing-falloff side doesn't
        // read pure black. VK omitted this -> alpha-test fences/cards rendered BLACK. Apply
        // the SAME floor verbatim before multiplying the albedo.
        materialAlbedo = texc.rgb; haveAlbedo = true;
        rgb = texc.rgb * max(vArgb, vec3(0.5));
        rgb += vHighlight.rgb * vHighlight.a;
    } else if (uFrame.materialReady != 0 && (vLayer >= 0 || propHasObjectAlbedo(uPacket.materialId))) {
        // ALPHA-OFF group. Real albedo texture (GL_REPEAT restored via fract, then
        // cropped to this packet's sub-region -- FENCE-UVSCALE-1, mirrors GL's
        // static_prop.frag:206 `fract(v_uv) * vec2(uvScaleX, uvScaleY)` verbatim;
        // uvScale defaults to 1.0 for the common single-texture types so this is a
        // no-op there).
        vec2 uvContinuous = vUv * vUvScale;
        const int bucketOff = vLayer >= 0 ? propBucketOf(vLayer) : 0;
        vec4 texOff = vLayer >= 0 ? textureGrad(uAlbedo[nonuniformEXT(bucketOff)], vec3(fract(vUv) * vUvScale, float(propBucketLayer(vLayer))),
                                               dFdx(uvContinuous), dFdy(uvContinuous)) : vec4(1.0);
        vec3 albedo = texOff.rgb;
        vec3 objectAlbedo;
        if (propAlbedoOverride(uPacket.materialId, objectAlbedo)) albedo = objectAlbedo;
        albedo = propTeamTint(uPacket.materialId, albedo, vTeam);
        materialAlbedo = albedo; haveAlbedo = true;

        // GL DEFAULT PROP MODEL (static_prop.frag:346): tex_color * v_argb, where v_argb
        // is the VERTEX-BAKED lit color (base_light seed + ambient + sun*NdotL). NOT PBR.
        rgb = albedo * vArgb;
        // A packet GL alpha-tests although its type sits in the alpha-OFF group (its texture's alpha flag is set after the arrays
        // were built, e.g. the hex lattice props): GL's dither discard, its alpha-test light floor and its alpha-test shadow class.
        if (kOpaqueOnly == 0 && uPacket.packetAlphaTest != 0) {
            cover = propCoverage(texOff.a, vLayer >= 0 ? textureQueryLod(uAlbedo[nonuniformEXT(bucketOff)], uvContinuous).y : 0.0);   // PROP-COVERAGE-1
            rgb = albedo * max(vArgb, vec3(0.5));
            alphaTestClass = true;
        }
        rgb += vHighlight.rgb * vHighlight.a;
    } else {
        // Fail-soft flat teal placeholder. IN-PLACE opaque write.
        rgb = vec3(0.35, 0.62, 0.68) * (0.6 + 0.4 * vUv.x);
    }
    // PROP-COVERAGE-1: an opaque (non-alpha-tested) fragment belongs to the core pass only.
    if (kOpaqueOnly == 0 && uPacket.coverageFringePass != 0 && !alphaTestClass) discard;
    // One pool walk for both albedo groups (was inlined once per branch), after the fringe discard:
    // halves the shader's code and skips the walk for fragments the fringe pass drops.
    if (haveAlbedo) {
        poolLight = propPoolLight(vec3(1));
        rgb += materialAlbedo * poolLight;
    }
    bool treeMaterial = haveAlbedo && vTree.y > 0.5;
    // Low/Potato reuse the VS's existing SH + sun + authored-light result.
    // Re-evaluating diffuse material lighting per fragment wastes the 24-EU budget.
    if (haveAlbedo && uPacket.materialId >= 0 && uPacket.materialId < uPropMaterials.materials.length()) {
        if (kBaseQuality == 2) {
            float materialCoverage;
            SurfaceMaterial surface = propSurface(uPacket.materialId, materialAlbedo, materialCoverage);
            if (materialCoverage > 0.0) {
                materialAO = mix(1.0, surface.ao, materialCoverage);
                if (!treeMaterial) rgb = mix(rgb, propAuthoredColor(surface, alphaTestClass,
                              propHasAuthoredOrm(uPacket.materialId), poolLight), materialCoverage);
                outputNormalStuff = mix(vNormalStuff, renderToMc2(surface.normal), materialCoverage);
            }
        }
        // Detail-map coverage cannot turn off an authored lamp surface.
        if (uPropMaterials.materials[uPacket.materialId].metadata.x == SURF_CLASS_EMISSIVE && (vMaterialFlags & 3u) == 0u)
            rgb = propAddWindowEmission(rgb, propDecodeColor(materialAlbedo) * uFrame.nightDarkness);
    }
    if (kBaseQuality == 2 && treeMaterial) {
        vec3 N = normalize(mc2ToRender(outputNormalStuff));
        vec3 L = uT.terrainLightDir.xyz / max(length(uT.terrainLightDir.xyz),1e-6);
        vec2 uv = vec2((vWorldPos.x-uT.atlasTopLeftX)*uT.atlasOneOverWorldUnits,
                       (uT.atlasTopLeftY-vWorldPos.y)*uT.atlasOneOverWorldUnits);
        vec3 macro = textureLod(u_colormap,uv,0.0).rgb;
        vec3 tint = macro / max(dot(macro,vec3(0.299,0.587,0.114)),0.01);
        vec3 albedo = propDecodeColor(materialAlbedo * mix(vec3(1),tint,0.25));
        vec3 sh[9]; for (int k=0;k<9;++k) sh[k]=uEnv.shDiffuse[k].xyz;
        // The environment carries the unrotated probe. Evaluate it in the sun's
        // frame so its strong directional sky fill agrees with the shadow map.
        vec3 skyNormal = normalize(outputNormalStuff);
        vec2 sunAxis = vec2(-L.x, L.y), probeAxis = vec2(sh[3].r, sh[2].r);
        if (dot(sunAxis,sunAxis) > 1e-8 && dot(probeAxis,probeAxis) > 1e-8) {
            float angle = atan(sunAxis.y,sunAxis.x) - atan(probeAxis.y,probeAxis.x);
            float c = cos(angle), s = sin(angle);
            skyNormal.xz = mat2(c,-s,s,c) * skyNormal.xz;
        }
        vec3 ambient = uFrame.iblShStrength > 0.0 ? evalShL2(skyNormal,sh)*uFrame.iblShStrength : vec3(0);
        // Crown normals describe the canopy volume: keep the terrain sun's side.
        // Backlit leaves receive transmission and sky fill below.
        float wrap = max((dot(N,L)+0.5)/1.5,0.0);
        float transmission = alphaTestClass ? 0.08*max(dot(-N,L),0.0) : 0.0;
        vec3 direct = propDecodeColor(vMaterialSun) * (propNightSunWeight(wrap, N) + transmission * propNightDirectShare());
        // SH already contains linear irradiance. Preserve the stock albedo exposure ceiling.
        float forestSky = vTree.z;
        vec3 irradiance = max(ambient,vec3(0))*vTree.x*materialAO*forestSky + direct*(vTree.w*mix(treeVisibility(N), 1.0, vTree.w)); // above the baked shadow line the crown is lit
        // Thin leaves retain sky fill through canopy self-occlusion. Fade it
        // with the night profile rather than making dark forests emissive. Full night keeps 0.35*0.35 (was 0.35*0.2: stock-dark
        // leaf albedo read near-black on night maps such as mc2_10).
        if (alphaTestClass) irradiance = max(irradiance, vec3(0.35 * (1.0 - 0.65*uFrame.nightDarkness) * forestSky));
        // Night moonlight/sky fill for foliage (zero by day): shape-giving hemisphere + leaf wrap, crown AO for form.
        // A light, not brighter albedo; it sits outside the albedo exposure ceiling, which kept stock-dark leaves black.
        vec3 nightFill = propFoliageNightAmbient(normalize(outputNormalStuff), alphaTestClass) * (vTree.x * materialAO * forestSky);
        rgb = propEncodeColor(min(albedo, albedo*irradiance) + albedo*nightFill);
        rgb += propEncodeColor(albedo) * poolLight + vHighlight.rgb*vHighlight.a;
    }
    if (vWindow.x != 0u && uFrame.nightDarkness > 0.0 && (vMaterialFlags & 1u) == 0u) {
        uint h = vWindow.x ^ vWindow.y;
        h ^= h >> 16; h *= 0x7feb352du;
        h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
        if (float(h & 0xffffu) < 0.35 * 65536.0) {
            float warmth = float(h >> 16) / 65535.0;
            vec3 emission = uFrame.nightDarkness * propDecodeColor(
                mix(vec3(0.72, 0.36, 0.12), vec3(1.0, 0.70, 0.34), warmth));
            // Compose radiance before the scene fog/tonemap passes. Keep coverage in oColor.a.
            rgb = propAddWindowEmission(rgb, emission);
        }
    }

    if ((vMaterialFlags & 128u) != 0u) rgb *= uintBitsToFloat(vTeam);
    // Material inputs only: bypass prop lighting, windows and ORM coverage for inspection.
    // Missing maps use the same class scalars and geometric normal as normal rendering.
    int debugView = int(uFrame.cameraWorldPos.w);
    if (debugView != 0 && haveAlbedo) {
        vec3 debugNormal = normalize(mc2ToRender(vNormalStuff));
        vec3 rawNormal = vec3(0.5, 0.5, 1.0);
        float roughness = 1.0, metallic = 0.0, ao = 1.0;
        bool validMaterial = uPacket.materialId >= 0 && uPacket.materialId < uPropMaterials.materials.length();
        if (validMaterial) {
            SurfaceMaterial surface = propSurface(uPacket.materialId, materialAlbedo);
            debugNormal = surface.normal;
            roughness = surface.roughness; metallic = surface.metallic; ao = surface.ao;
            PropMaterialGpu row = uPropMaterials.materials[uPacket.materialId];
            if (row.material.normalTex != 0xffffffffu) {
                vec2 uv = vUv * row.uvScale;
                rawNormal = kPerTexMaps && (row.material.flags & kMatFlagPerTexture) != 0u
                    ? texture(uPropTex[nonuniformEXT(row.material.normalTex)], uv).rgb
                    : texture(uPropNormal, vec3(uv, float(row.material.normalTex))).rgb;
            }
        }
        if (debugView == 1) rgb = materialAlbedo;
        else if (debugView == 2) rgb = debugNormal * 0.5 + 0.5;
        else if (debugView == 3) rgb = rawNormal;
        else if (debugView == 4) rgb = vec3(roughness);
        else if (debugView == 5) rgb = vec3(metallic);
        else if (debugView == 6) rgb = vec3(ao);
        else if (debugView == 7) {
            uint h = uint(uPacket.materialId) + 1u;
            h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
            rgb = validMaterial ? vec3(h & 255u, (h >> 8) & 255u, (h >> 16) & 255u) / 255.0 : vec3(0);
        }
    }
    oColor = vec4(rgb, cover);
    // location 1: GBuffer1 — bit-adjacent to static_prop.frag:657-661, which selects
    // between TWO shadow classes off the packet's alpha-test bit:
    //   solid prop      -> rc_gbuffer1_screenShadowEligible_noSelfShadow()          a=0.25
    //   alpha-test prop -> rc_gbuffer1_screenShadowEligible_noSelfShadow_alphaTest() a=0.1875
    // (values inlined; VK has no include of shaders/include/render_contract.hglsl --
    // same convention as vulkan/water_geom.frag:13. The inlined constants are pinned
    // by scripts/check-render-contract-gbuffer1.allowlist, which fails if either
    // number is edited here without re-adjudicating the class.)
    //
    // VK-GBUFFER1-CENSUS-1 CONFORMANCE FIX: this used to emit a FLAT 0.25 for every
    // prop, including the alpha-ON group -- which IS the GL ALPHA_TEST class (tree
    // cards, foliage, chain-link fences; see the alpha-ON branch above, which already
    // applies both the GL 0.5 alpha-test discard and the GL alpha-test light floor).
    // A flat 0.25 therefore misclassified every VK-drawn tree and fence as a solid
    // prop. GL is canonical; VK conforms.
    //
    // GRANULARITY CAVEAT (stated, not hidden). GL selects PER PACKET
    // (PerDrawEntry.materialFlags, gos_static_prop_batcher.cpp:5234-5242). The VK
    // island's material feed carries only a PER TYPE alpha class
    // (Mc2ColorPropMaterialFeed::typeAlphaClass; packetLayerOn[] is filled for every
    // packet of a class-1 type -- vulkan_color_prop_island.cpp:1426-1432), so an
    // ALPHA-MIXED type's opaque packets take this branch too. That makes the VK
    // alpha-test set a strict SUPERSET of GL's at packet granularity. It is inert for
    // every consumer that exists today: 0.1875 and 0.25 are both inside
    // rc_isStaticPropNoSelfShadow's (0.1, 0.5] and both inside the shoreline water
    // band (0.15, 0.35) that shoreline.frag:22 / vulkan/shoreline.frag:102 select on.
    // The ONLY reader that separates them is rc_isAlphaTestProp, used solely by
    // shadow_screen.frag under MC2_PROP_SHADOW_RECEIVE (default OFF) -- and the VK
    // screen-shadow ports (vulkan/screen_shadow_csm.frag, screen_shadow_static.frag)
    // inline only rc_pixelHandlesOwnShadow / rc_isStaticPropNoSelfShadow and have no
    // RECEIVE opt-in at all. Closing the granularity gap needs a per-packet alpha bit
    // on the feed; it is not required for class correctness and is NOT done here.
    // Trees handle their own shadow, but must occlude ground HUD separately from terrain (1.0).
    oGBuffer1 = vec4(normalize(treeMaterial ? mc2ToRender(outputNormalStuff) : outputNormalStuff) * 0.5 + 0.5, treeMaterial ? 0.96875 : alphaTestClass ? 0.1875 : 0.25);
    oObjectId = uint(uPacket.objectId);
}
