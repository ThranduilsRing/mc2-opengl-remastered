// VK-P14 MECH TIER-1 — 2-output GBuffer FS.
// =============================================================================
// Co-writes skinned mechs IN-PLACE into the SHARED scene GBuffer (LOAD-preserve,
// GEQUAL reverse-Z, no blend). A winning mech fragment fully overwrites the pixel.
//
// VK-MECH-MATERIAL-LIGHTING-1: ports the GL DEFAULT mech shading (mech.frag with
// MC2_USE_VIEW_UNIFORMS + StandardLit OFF, both default-OFF):
//     c = textureLod(u_tex, uv, 0) * v_litColor;
//     c.rgb += v_highlightColor.rgb * v_highlightColor.a;   // additive, AGAIN
//     c.rgb = mix(c.rgb, fogColor, hazeFactor);
// v_litColor is the per-vertex calc_light Lambert from the VS. NOT the PBR/GGX path
// (that needs the viewuniforms variant). Highlight is folded into v_litColor (multiply)
// AND re-added here (additive) — exactly as GL does (mech.vert:223 + mech.frag:190).
//
//   location=0 lit color  -> sceneColorTex_  (MRT0, RGBA16F).
//     * material+lighting ON: albedo * litColor + highlight, fogged.
//     * material OFF: flat silhouette fill (the skinning/axis-swap proof).
//   location=1 GBuffer1   -> sceneNormalTex_ (MRT1, RGBA16F). Matches mech.frag:
//       rc_gbuffer1_screenShadowEligible(normalize(N)) = vec4(N*0.5+0.5, 0.0).
//     N is the MC2/GL-WORLD (swapped) normal. a=0.0 = screen-shadow-eligible; the GL
//     screen pass does mech shadow.
#version 450

layout(set = 0, binding = 0, std140) uniform MechFrameUBO {
    mat4  worldToClipGL;
    float objectDepthBias;
    int   materialReady;   // 1 = sample the copy-once albedo
    int   lightingReady;   // 1 = calc_light valid
    int   _pad1;
    vec4  fogColorRGB;
} uFrame;

layout(set = 0, binding = 3) uniform sampler2D uAlbedo;   // copy-once slot-0 paint scheme

layout(location = 0) in vec2 vUv;
layout(location = 1) in vec3 vNormalGL;
layout(location = 2) in vec3 vLitColor;
layout(location = 3) in vec4 vHighlight;
layout(location = 4) in vec4 vFog;        // .rgb fog color, .a haze factor

layout(location = 0) out vec4 oColor;     // MRT0
layout(location = 1) out vec4 oGBuffer1;  // MRT1

void main() {
    vec3 rgb;
    if (uFrame.materialReady != 0) {
        // GL default mech: textureLod(...,0) (AMD mip strict-fail avoidance, mech.frag:171).
        vec3 tex = textureLod(uAlbedo, vUv, 0.0).rgb;
        // c = tex * v_litColor (mech.frag:189); highlight ADDED again (mech.frag:190).
        rgb = tex * vLitColor;
        rgb += vHighlight.rgb * vHighlight.a;
        // Per-actor haze fog (mech.frag:393).
        rgb = mix(rgb, vFog.rgb, vFog.a);
    } else {
        // FLAT silhouette fill — the axis-swap / skinning proof (distinct hue vs props).
        rgb = vec3(0.75, 0.45, 0.25) * (0.6 + 0.4 * vUv.y);
    }
    oColor = vec4(rgb, 1.0);

    // GBuffer1: MC2/GL-world normal, a=0.0 (screen-shadow-eligible, no in-frag shadow).
    oGBuffer1 = vec4(normalize(vNormalGL) * 0.5 + 0.5, 0.0);
}
