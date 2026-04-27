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

void main()
{
    PREC vec4 tex_color = texture(tex1, Texcoord);

    // Straight texture × vertex colour — no tone correction.
    // Color is RGBA [0,1] unpacked from BGRA uint.
    PREC vec4 c = Color * tex_color;

    // Cloud shadows — narrow range for already-dark decals.
    {
        PREC vec2  cloudUV    = WorldPos.xy * 0.0006 + vec2(time * 0.012, time * 0.005);
        PREC float cloudNoise = fbm(cloudUV, 4) * 0.5 + 0.5;
        c.rgb *= mix(0.88, 1.0, smoothstep(0.3, 0.7, cloudNoise));
    }

    // Static + dynamic shadow maps.
    {
        vec3  lightDir3 = normalize(terrainLightDir.xyz);
        float shadow    = calcShadow(WorldPos, vec3(0.0, 0.0, 1.0), lightDir3, 8);
        shadow = min(shadow, calcDynamicShadow(WorldPos, vec3(0.0, 0.0, 1.0), lightDir3, 4));
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
