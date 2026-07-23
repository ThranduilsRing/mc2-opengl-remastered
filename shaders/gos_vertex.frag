//#version 300 es

#define PREC highp

#include <include/render_contract.hglsl>

in PREC vec4 Color;
in PREC vec2 Texcoord;
in PREC float FogValue;

layout (location=0) out PREC vec4 FragColor;
// F3 Option A: shadow-handled (HUD/overlay/debug draws skip post-shadow).
// Lines, selection brackets, health bars: never shadow-darken.
layout (location=1) out PREC vec4 GBuffer1;

#ifdef ENABLE_TEXTURE1
uniform sampler2D tex1;
#endif
uniform sampler2D tex2;
uniform sampler2D tex3;

uniform PREC vec4 fog_color;

void main(void)
{
    PREC vec4 c = Color.bgra;
#ifdef ENABLE_TEXTURE1
    c *= texture(tex1, Texcoord);
#endif
	if(fog_color.x>0.0 || fog_color.y>0.0 || fog_color.z>0.0 || fog_color.w>0.0)
		c.rgb = mix(fog_color.rgb, c.rgb, FogValue);
    FragColor = c;

    // F3 Option A: flat-up, shadow handled — HUD/overlay draws skip post-shadow.
    GBuffer1 = rc_gbuffer1_shadowHandled_flatUp();
}

