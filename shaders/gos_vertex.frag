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
// FAR-BACKDROP-NIGHT-LIGHTING-1: per-channel black floor for the untextured
// world-geometry path (far horizon terrain + far tree impostors). The driver
// uploads a small dim value ONLY on untextured, depth-tested world draws and
// (0,0,0) otherwise, so day / HUD / overlays stay identity (max() no-op). See
// gosRenderer::applyBackdropNightFloor. Unset uniform reads (0,0,0) => identity.
uniform PREC vec3 u_worldAmbientFloor;

void main(void)
{
    PREC vec4 c = Color.bgra;
#ifdef ENABLE_TEXTURE1
    c *= texture(tex1, Texcoord);
#endif
	if(fog_color.x>0.0 || fog_color.y>0.0 || fog_color.z>0.0 || fog_color.w>0.0)
		c.rgb = mix(fog_color.rgb, c.rgb, FogValue);
    // FAR-BACKDROP-NIGHT-LIGHTING-1: lift the night horizon off pitch-black.
    c.rgb = max(c.rgb, u_worldAmbientFloor);
    FragColor = c;

    // F3 Option A: flat-up, shadow handled — HUD/overlay draws skip post-shadow.
    GBuffer1 = rc_gbuffer1_shadowHandled_flatUp();
}

