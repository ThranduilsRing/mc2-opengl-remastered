//#version 300 es
#define PREC highp

#include <include/render_contract.hglsl>

in PREC vec4 Color;
in PREC float FogValue;
in PREC vec2 Texcoord;

layout (location=0) out PREC vec4 FragColor;
// F3 Option A: post-shadow-eligible mask + flat-up normal (no Normal varying
// in this shader). Listed in flat-up roster of F3 closing report.
layout (location=1) out PREC vec4 GBuffer1;

#ifdef ENABLE_TEXTURE1
uniform sampler2D tex1;
#endif
uniform sampler2D tex2;
uniform sampler2D tex3;

uniform PREC vec4 fog_color;

void main(void)
{
    PREC vec4 c = Color;
#ifdef ENABLE_TEXTURE1
    c *= texture(tex1, Texcoord);
#endif
	if(fog_color.x>0.0 || fog_color.y>0.0 || fog_color.z>0.0 || fog_color.w>0.0)
    	c.rgb = mix(fog_color.rgb, c.rgb, FogValue);
    FragColor = c;

    // F3 Option A: flat-up fallback (compatibility — no surface normal available).
    GBuffer1 = rc_gbuffer1_screenShadowEligible(vec3(0.0, 0.0, 1.0));
}

