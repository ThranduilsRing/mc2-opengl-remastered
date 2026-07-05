// decal.frag
// Fragment shader for bomb craters and mech footprints.
// No tone correction — decal textures are authored dark and the vertex colour carries
// the intended alpha for blending.  Cloud shadow range is narrower than cement
// (0.88-1.0) because crater/footprint base luminance is already low.
//
// Render state set by gosRenderer::drawDecals():
//   alpha blend (SRC_ALPHA, ONE_MINUS_SRC_ALPHA), depth-write OFF, depth-test LEQUAL,
//   polygon offset (-1,-1).
//
// MRT:
//   location=0  FragColor       — blended scene colour
//   location=1  GBuffer1.alpha  = 1.0  → shadow_screen.frag skips this pixel
//                                  (decal handles its own shadow inline; opt out
//                                  of post-shadow to avoid double-shadowing).
//
// GROUND-CONTACT-BLOB-1: Texcoord.y < -0.5 marks a quad that provides its OWN
// darkening (a contact-shadow disc under a mover) and must SKIP the inline sun
// shadow multiply below — the disc already IS the darkening; applying calcShadow
// on top would double-darken it where a cast shadow overlaps the blob.

#define PREC highp

#include <include/noise.hglsl>
#include <include/shadow.hglsl>
#include <include/render_contract.hglsl>

// [RENDER_CONTRACT]
//   Pass:           TerrainDecal
//   Color0:         RGBA, alpha-blended (SRC_ALPHA / ONE_MINUS_SRC_ALPHA)
//   GBuffer1:       rc_gbuffer1_shadowHandled_flatUp
//   ShadowContract: castsStatic=false, castsDynamic=false,
//                   skipsPostScreenShadow=true
//   StateContract:  depthTest=true (LEQUAL), depthWrite=false,
//                   blend=AlphaBlend, requiresMRT=true,
//                   polygon offset (-1,-1)

in PREC vec3  WorldPos;
in PREC vec2  Texcoord;
in PREC float FogValue;
in PREC vec4  Color;

layout(location=0) out PREC vec4 FragColor;
#ifdef MRT_ENABLED
layout(location=1) out PREC vec4 GBuffer1;
#endif

uniform sampler2D tex1;
uniform PREC vec4 fog_color;
uniform PREC float time;
uniform vec4 terrainLightDir;
uniform int u_pathTint;  // MC2_SHADER_PATH_TINT: 1 = solid signature colour (debug); 0 = normal

void main()
{
    // MC2_SHADER_PATH_TINT: solid CYAN so this shader's surfaces are unmistakable.
    if (u_pathTint != 0) {
        FragColor = vec4(0.0, 1.0, 1.0, 1.0);
#ifdef MRT_ENABLED
        GBuffer1 = rc_gbuffer1_shadowHandled_flatUp();
#endif
        return;
    }
    PREC vec4 tex_color = (Texcoord.x < -0.5) ? vec4(1.0) : texture(tex1, Texcoord);

    // Straight texture × vertex colour — no tone correction.
    // Color is RGBA [0,1] unpacked from BGRA uint.
    PREC vec4 c = Color * tex_color;

    // Cloud shadows moved to the fullscreen cloud pass (cloud.frag).

    // GROUND-CONTACT-BLOB-1: Texcoord.y < -0.5 = self-darkening quad, skip the
    // inline sun-shadow multiply entirely (see file header comment).
    bool skipInlineShadow = (Texcoord.y < -0.5);

    // SHADOW-DECAL-SINGLE: apply sun shadow ONCE here. Opaque decals fully cover
    // the shadowed terrain underneath (alpha-blend, no depth write) and are skipped
    // by shadow_screen.frag (GBuffer1.a=1, shadowHandled). The earlier "double"
    // claim was a distribution math error: base*sh*(1-a)+decal*sh*a = sh once.
    if (!skipInlineShadow) {
        const PREC vec3 shadowN = vec3(0.0, 0.0, 1.0);   // flat-up, matches terrain
        float staticShadow = calcShadow(WorldPos, shadowN, terrainLightDir.xyz, 8);
        float dynShadow    = calcDynamicShadow(WorldPos, shadowN, terrainLightDir.xyz, 4);
        float shadow       = min(staticShadow, dynShadow);
        c.rgb *= shadow;
    }

    // Fog.
    if (fog_color.x > 0.0 || fog_color.y > 0.0 || fog_color.z > 0.0 || fog_color.w > 0.0)
        c.rgb = mix(fog_color.rgb, c.rgb, FogValue);

    FragColor = c;

#ifdef MRT_ENABLED
    // Decal handles its own shadow inline (cloud + static + dynamic above);
    // opt out of post-process shadow multiply to avoid double-shadowing.
    GBuffer1 = rc_gbuffer1_shadowHandled_flatUp();
#endif
}
