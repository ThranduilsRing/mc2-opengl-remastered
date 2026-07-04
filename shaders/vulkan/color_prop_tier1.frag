// VK-P11 STATIC-PROP COLOR — TIER-1 (parity) 2-output GBuffer FS.
// =============================================================================
// Co-writes props IN-PLACE into the SHARED scene GBuffer attachments (no side-image,
// no composite): the VK renderpass LOADs the GL-authored color(MRT0) + normal(MRT1)
// + shared depth, depth-tests GEQUAL reverse-Z, and only overwrites where the prop
// wins the depth test. No blend (opaque, colorWriteMask 0xF, a=1) — a winning prop
// fragment fully replaces the terrain GBuffer pixel exactly as the GL prop pass does.
//
//   location=0 lit color  -> sceneColorTex_  (MRT0, RGBA16F). Matches Tier-0's flat
//                            debug albedo (real texture/MaterialGpu/lighting parity
//                            is a later tier); a=1 (opaque, in-place — NOT premult).
//   location=1 GBuffer1    -> sceneNormalTex_ (MRT1, RGBA16F). BIT-ADJACENT to the GL
//                            producer static_prop.frag:
//                              rc_gbuffer1_screenShadowEligible_noSelfShadow(N) =
//                              vec4(normalize(N)*0.5+0.5, 0.25)
//                            ★N is the STUFF-SPACE (un-swapped) normal from the VS.
//                            a=0.25 = static-prop shadow class (static-map shadow OK,
//                            no dynamic-cascade self-shadow); shadow_screen.frag gates
//                            rc_isStaticPropNoSelfShadow (0.1 < a <= 0.5).
#version 450

layout(location = 0) in vec2 vUv;
layout(location = 1) in vec3 vNormalStuff;

layout(location = 0) out vec4 oColor;     // MRT0 lit color
layout(location = 1) out vec4 oGBuffer1;  // MRT1 normal + shadow class

void main() {
    // location 0: flat debug albedo (matches color_prop.frag Tier-0). IN-PLACE
    // opaque write — a=1, NOT premultiplied (no composite/blend on the Tier-1 path).
    vec3 rgb = vec3(0.35, 0.62, 0.68) * (0.6 + 0.4 * vUv.x);
    oColor = vec4(rgb, 1.0);

    // location 1: GBuffer1 = vec4(normalize(N_stuff)*0.5+0.5, 0.25) — bit-adjacent to
    // static_prop.frag's rc_gbuffer1_screenShadowEligible_noSelfShadow(normalize(v_normal)).
    oGBuffer1 = vec4(normalize(vNormalStuff) * 0.5 + 0.5, 0.25);
}
