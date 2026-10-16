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

// Per-frame UBO: generated from RenderCore/gpu_abi/GpuColorFrameUBO.h.
#include "include/color_frame_ubo_vk.glsl"

// VK-P11 STATIC-PROP MATERIAL: alpha-OFF albedo GL_TEXTURE_2D_ARRAY (copy-once import).
layout(set = 0, binding = 3) uniform sampler2DArray uAlbedo;
// VK-P11 STATIC-PROP MATERIAL alpha-ON: alpha-MIXED prop array (copy-once import of
// s_texArrayOn). Sampled with a 0.5 alpha-test discard, matching GL static_prop.frag.
layout(set = 0, binding = 6) uniform sampler2DArray uAlbedoOn;

layout(location = 0) in vec2 vUv;
layout(location = 1) in vec3 vNormalStuff;
flat layout(location = 2) in int  vLayer;
layout(location = 3) in vec3 vArgb;   // VERTEX-BAKED lit color (GL default prop model)
flat layout(location = 4) in int  vLayerOn;  // alpha-ON albedo array layer (-1 = none)
flat layout(location = 5) in vec2 vUvScale;  // FENCE-UVSCALE-1: this packet's uvScaleX/Y
layout(location = 6) in vec3 vWorldPos;
flat layout(location = 7) in vec3 vSunDirStuff;
flat layout(location = 8) in vec3 vSunColor;

// The vertex stage's push block; the FS reads only packetAlphaTest.
layout(push_constant) uniform PacketLayerPC {
    int   packetLayerOff;
    int   packetLayerOn;
    float packetUvScaleX;
    float packetUvScaleY;
    int   packetAlphaTest;
} uPacket;

layout(location = 0) out vec4 oColor;     // MRT0 lit color
layout(location = 1) out vec4 oGBuffer1;  // MRT1 normal + shadow class

#include "../include/prop_dither.hglsl"
#include "../include/axis.hglsl"

// StaticPropPbr option: Schlick-Fresnel sun specular added to the lit prop colour. The albedo-tinted F0 and the power lobe are the GL terms; metallic 0 and roughness 1
// are the GL literal defaults, because the island carries no per-material scalars; the GL roughness override (on at 0.95 by default) replaces the roughness as in GL.
vec3 propSunSpecular(vec3 albedo) {
    const float metallic  = 0.0;
    const float roughness = uFrame.pbrV1RoughnessOverride >= 0.0 ? uFrame.pbrV1RoughnessOverride : 1.0;
    vec3 N = mc2ToRender(normalize(vNormalStuff));
    vec3 V = normalize(uFrame.cameraWorldPos.xyz - vWorldPos);
    vec3 L = mc2ToRender(normalize(-vSunDirStuff));
    vec3 H = normalize(L + V);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 F  = F0 + (vec3(1.0) - F0) * pow(max(0.0, 1.0 - dot(N, V)), 5.0);
    float smoothness = 1.0 - roughness;
    float specPower  = mix(1.0, 512.0, smoothness * smoothness);
    return vSunColor * F * pow(max(0.0, dot(N, H)), specPower) * max(0.0, dot(N, L));
}

void main() {
    vec3 rgb;
    // VK-GBUFFER1-CENSUS-1: which GBuffer1 shadow CLASS this fragment belongs to.
    // Set true by the alpha-ON branch below -- see the oGBuffer1 write at the end
    // of main() for why the distinction exists and what its granularity is.
    bool alphaTestClass = false;
    if (uFrame.materialReadyOn != 0 && vLayerOn >= 0) {
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
        vec2  uvSampledOn = fract(vUv) * vUvScale;
        vec2  tsOn = vec2(textureSize(uAlbedoOn, 0).xy);
        vec2  duv1 = dFdx(uvSampledOn) * tsOn;
        vec2  duv2 = dFdy(uvSampledOn) * tsOn;
        float lodOn = 0.5 * log2(max(max(dot(duv1, duv1), dot(duv2, duv2)), 1e-12));
        lodOn = max(lodOn, 0.0);
        vec4 texc = textureLod(uAlbedoOn, vec3(uvSampledOn, float(vLayerOn)), lodOn);
        // BUILDING-GLASS-1 parity with GL static_prop.frag: Bayer 4x4 ordered dither
        // (thresholds (index+0.5)/16, gl_FragCoord-based) so partial-alpha leaf/edge
        // texels stipple instead of hard-cutting at 0.5.
        if (propBayerDitherDiscard(texc.a, gl_FragCoord.xy)) discard;
        // ★VK-FLIP-FIELD-REPORT-1 BLACK-FENCE FIX: the alpha-ON group is the ALPHA_TEST
        // class (tree cards / fences / foliage). GL static_prop.frag:339 floors their lit
        // color at max(litRgb, vec3(0.5)) so the dark, light-facing-falloff side doesn't
        // read pure black. VK omitted this -> alpha-test fences/cards rendered BLACK. Apply
        // the SAME floor verbatim before multiplying the albedo.
        rgb = texc.rgb * max(vArgb, vec3(0.5));
        if (uFrame.pbrV1Strength > 0.0) rgb += propSunSpecular(texc.rgb) * uFrame.pbrV1Strength;
    } else if (uFrame.materialReady != 0 && vLayer >= 0) {
        // ALPHA-OFF group. Real albedo texture (GL_REPEAT restored via fract, then
        // cropped to this packet's sub-region -- FENCE-UVSCALE-1, mirrors GL's
        // static_prop.frag:206 `fract(v_uv) * vec2(uvScaleX, uvScaleY)` verbatim;
        // uvScale defaults to 1.0 for the common single-texture types so this is a
        // no-op there).
        vec4 texOff = texture(uAlbedo, vec3(fract(vUv) * vUvScale, float(vLayer)));
        vec3 albedo = texOff.rgb;

        // GL DEFAULT PROP MODEL (static_prop.frag:346): tex_color * v_argb, where v_argb
        // is the VERTEX-BAKED lit color (base_light seed + ambient + sun*NdotL). NOT PBR.
        rgb = albedo * vArgb;
        // A packet GL alpha-tests although its type sits in the alpha-OFF group (its texture's alpha flag is set after the arrays
        // were built, e.g. the hex lattice props): GL's dither discard, its alpha-test light floor and its alpha-test shadow class.
        if (uPacket.packetAlphaTest != 0) {
            if (propBayerDitherDiscard(texOff.a, gl_FragCoord.xy)) discard;
            rgb = albedo * max(vArgb, vec3(0.5));
            alphaTestClass = true;
        }
        if (uFrame.pbrV1Strength > 0.0) rgb += propSunSpecular(albedo) * uFrame.pbrV1Strength;
    } else {
        // Fail-soft flat teal placeholder. IN-PLACE opaque write.
        rgb = vec3(0.35, 0.62, 0.68) * (0.6 + 0.4 * vUv.x);
    }
    oColor = vec4(rgb, 1.0);

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
    oGBuffer1 = vec4(normalize(vNormalStuff) * 0.5 + 0.5, alphaTestClass ? 0.1875 : 0.25);
}
