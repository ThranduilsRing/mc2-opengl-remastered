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
//       (matches Tier-0's debug albedo) — fail-soft, never crashes/holes.
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
//                            producer static_prop.frag:
//                              rc_gbuffer1_screenShadowEligible_noSelfShadow(N) =
//                              vec4(normalize(N)*0.5+0.5, 0.25)
//                            ★N is the STUFF-SPACE (un-swapped) normal from the VS.
//                            a=0.25 = static-prop shadow class (static-map shadow OK,
//                            no dynamic-cascade self-shadow); shadow_screen.frag gates
//                            rc_isStaticPropNoSelfShadow (0.1 < a <= 0.5).
#version 450

layout(set = 0, binding = 0, std140) uniform ColorFrameUBO {
    mat4  worldToClipGL;
    float objectDepthBias;
    int   instBase;
    int   materialReady;   // 1 = sample alpha-OFF albedo array + apply vertex-baked vArgb
    int   materialReadyOn; // 1 = sample alpha-ON albedo array (alpha-test discard)
    vec4  terrainSunMC2;    // surface->sun (MC2 world); .w pad
} uFrame;

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

layout(location = 0) out vec4 oColor;     // MRT0 lit color
layout(location = 1) out vec4 oGBuffer1;  // MRT1 normal + shadow class

void main() {
    vec3 rgb;
    if (uFrame.materialReadyOn != 0 && vLayerOn >= 0) {
        // ALPHA-ON group (alpha-MIXED buildings): sample the alpha-ON array and apply the
        // SAME 0.5 alpha-test discard the GL producer uses (static_prop.frag:236:
        // (materialFlags & ALPHA_TEST_BIT) && tex_color.a < 0.5 -> discard). A discarded
        // fragment leaves the LOADed terrain GBuffer pixel untouched (in-place co-write).
        // ADJUDICATION-1 FENCE FIX: fract() BEFORE sampling destroys screen-space
        // derivatives at the UV wrap seam (0.99->0.01 jump -> fwidth explodes ->
        // implicit LOD slams toward the tiniest mip -> minified alpha aliases <0.5
        // -> panel discards). At fence minification each panel spans only a few
        // pixels, so one seam-crossing quad can kill a panel's entire footprint;
        // whether a panel's pixels straddle a seam depends on UV phase -> the
        // ALTERNATING panel dropout. GL shows the same fract() in text but its
        // implicit-derivative behavior (coarse/fine choice, quad packing) is
        // implementation-defined and its compiler path happens to survive; the VK
        // path does not. Fix: gradients from the UNWRAPPED vUv (smooth everywhere),
        // wrapped coord for addressing, via textureGrad — not a control-flow
        // construct, so uniform control flow is preserved.
        // Bisect proof (this session): forcing LOD 0 restored every dropped panel,
        // while textureGrad with smooth gradients did NOT -> the kill mechanism is
        // the minified mip chain's alpha decaying below the 0.5 test (naive
        // glGenerateMipmap box filter halves thin-wire coverage per level), with
        // VK's LOD selection landing deeper than GL's for the same footprint.
        // Fix: deterministic LOD from the UNWRAPPED vUv gradients (no fract-seam
        // spikes, no driver-implicit divergence), CLAMPED to <=1.0 for this
        // alpha-test class so chain-link/foliage alpha coverage survives the test
        // (LOD<=1 proven panel-preserving by the bisect). Uniform control flow
        // preserved (explicit-LOD sample, no control-flow construct).
        vec2  tsOn = vec2(textureSize(uAlbedoOn, 0).xy);
        vec2  duv1 = dFdx(vUv) * tsOn;
        vec2  duv2 = dFdy(vUv) * tsOn;
        float lodOn = 0.5 * log2(max(max(dot(duv1, duv1), dot(duv2, duv2)), 1e-12));
        lodOn = clamp(lodOn, 0.0, 1.0);
        vec4 texc = textureLod(uAlbedoOn, vec3(fract(vUv), float(vLayerOn)), lodOn);
        if (texc.a < 0.5) discard;
        // ★VK-FLIP-FIELD-REPORT-1 BLACK-FENCE FIX: the alpha-ON group is the ALPHA_TEST
        // class (tree cards / fences / foliage). GL static_prop.frag:339 floors their lit
        // color at max(litRgb, vec3(0.5)) so the dark, light-facing-falloff side doesn't
        // read pure black. VK omitted this -> alpha-test fences/cards rendered BLACK. Apply
        // the SAME floor verbatim before multiplying the albedo.
        rgb = texc.rgb * max(vArgb, vec3(0.5));
    } else if (uFrame.materialReady != 0 && vLayer >= 0) {
        // ALPHA-OFF group. Real albedo texture (GL_REPEAT restored via fract; the GL frag
        // uses fract(v_uv) with per-draw uvScale — uvScale is 1.0 for the common
        // single-texture types, so fract() alone reproduces the dominant case; sub-region
        // uvScale is DEFERRED).
        vec3 albedo = texture(uAlbedo, vec3(fract(vUv), float(vLayer))).rgb;

        // GL DEFAULT PROP MODEL (static_prop.frag:346): tex_color * v_argb, where v_argb
        // is the VERTEX-BAKED lit color (base_light seed + ambient + sun*NdotL). NOT PBR.
        rgb = albedo * vArgb;
    } else {
        // Fail-soft flat teal (matches color_prop.frag Tier-0). IN-PLACE opaque write.
        rgb = vec3(0.35, 0.62, 0.68) * (0.6 + 0.4 * vUv.x);
    }
    oColor = vec4(rgb, 1.0);

    // location 1: GBuffer1 = vec4(normalize(N_stuff)*0.5+0.5, 0.25) — bit-adjacent to
    // static_prop.frag's rc_gbuffer1_screenShadowEligible_noSelfShadow(normalize(v_normal)).
    oGBuffer1 = vec4(normalize(vNormalStuff) * 0.5 + 0.5, 0.25);
}
