// VK-P14 MECH TIER-1 — 2-output GBuffer FS (SUB-SLICE 1: skinning + flat color).
// =============================================================================
// Co-writes skinned mechs IN-PLACE into the SHARED scene GBuffer (LOAD-preserve,
// GEQUAL reverse-Z, no blend). A winning mech fragment fully overwrites the pixel.
//
//   location=0 lit color  -> sceneColorTex_  (MRT0, RGBA16F).
//     * MC2_VK_MECH_MATERIAL OFF (or albedo not ready): FLAT color — a solid teal-ish
//       fill (the P-conjugation/axis-swap SILHOUETTE proof; A/B the shape vs GL BEFORE
//       adding material/lighting).
//     * ON: samples the copy-once slot-0 paint-scheme 2D albedo. Lighting (calc_light
//       + LightsData UBO) is a SEPARATE follow-on (MC2_VK_MECH_LIGHTING) — until then
//       the albedo is shown un-lit (flat). NOT byte-parity with the GL mech (which
//       applies v_litColor); this sub-slice de-risks skinning only.
//   location=1 GBuffer1   -> sceneNormalTex_ (MRT1, RGBA16F). Matches mech.frag:
//       GBuffer1 = rc_gbuffer1_screenShadowEligible(normalize(N)) = vec4(N*0.5+0.5, 0.0)
//     ★N is the MC2/GL-WORLD (swapped) normal from the VS (mech.frag forwards the
//     swapped worldNormal as v_normal). a=0.0 = screen-shadow-eligible, NO in-frag
//     shadow (the GL screen pass does mech shadow).
#version 450

layout(set = 0, binding = 0, std140) uniform MechFrameUBO {
    mat4  worldToClipGL;
    float objectDepthBias;
    int   materialReady;   // 1 = sample the copy-once albedo
    int   _pad0;
    int   _pad1;
} uFrame;

layout(set = 0, binding = 3) uniform sampler2D uAlbedo;   // copy-once slot-0 paint scheme

layout(location = 0) in vec2 vUv;
layout(location = 1) in vec3 vNormalGL;

layout(location = 0) out vec4 oColor;     // MRT0
layout(location = 1) out vec4 oGBuffer1;  // MRT1

void main() {
    vec3 rgb;
    if (uFrame.materialReady != 0) {
        // Single-material probe: un-lit albedo (lighting deferred to MC2_VK_MECH_LIGHTING).
        rgb = texture(uAlbedo, vUv).rgb;
    } else {
        // FLAT silhouette fill — the axis-swap / skinning proof. Distinct hue vs the
        // prop island teal so mech-vs-prop regions are separable in an A/B capture.
        rgb = vec3(0.75, 0.45, 0.25) * (0.6 + 0.4 * vUv.y);
    }
    oColor = vec4(rgb, 1.0);

    // GBuffer1: MC2/GL-world normal, a=0.0 (screen-shadow-eligible, no in-frag shadow) —
    // bit-adjacent to mech.frag's rc_gbuffer1_screenShadowEligible(normalize(v_normal)).
    oGBuffer1 = vec4(normalize(vNormalGL) * 0.5 + 0.5, 0.0);
}
