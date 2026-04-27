//#version 300 es
// using this because it is required if we want to use "binding" qualifier in layout (can be set in cpp code but it is easier to do in shader, so procedd like this and maybe change later)
//#version 420

#define PREC highp

#include <include/lighting.hglsl>
#include <include/shadow.hglsl>
// F3 canary (claude/f3-amd-canary-temp): explicit GBuffer1 write to test
// AMD location=1 corruption claim per render-contract f3 design §4.
#include <include/render_contract.hglsl>

uniform vec4 light_offset_;
uniform int gpuProjection;
uniform vec4 terrainLightDir;

in PREC vec3 Normal;
//in PREC float FogValue;
in PREC vec2 Texcoord;
in PREC vec4 VertexColor;
in PREC vec3 VertexLight;
in PREC vec3 WorldPos;
in PREC vec3 CameraPos;
in PREC vec3 MC2WorldPos;

layout (location=0) out PREC vec4 FragColor;
// F3 canary: explicit post-shadow-eligible write with real per-vertex normal.
// If AMD RX 7900 corrupts color output as the gos_postprocess.cpp:519-520
// comment claims, this declaration will surface the failure mode.
layout (location=1) out PREC vec4 GBuffer1;

uniform sampler2D tex1;
uniform PREC vec4 fog_color;

void main(void)
{
    PREC vec4 c = vec4(1,1,1,1);//Color.bgra;
    PREC vec4 tex_color = texture(tex1, Texcoord);
    c *= tex_color;

#ifdef ALPHA_TEST
    if(tex_color.a == 0.5)
        discard;
#endif

#if ENABLE_VERTEX_LIGHTING
	PREC vec3 lighting = VertexLight;
#else
    const int lights_index = int(light_offset_.x);
    PREC vec3 lighting = calc_light(lights_index, Normal, VertexLight);
#endif

    // GPU projection path: projected ground overlays (mission markers, nav beacons) are
    // 2D UI elements and should not receive world shadows — skip shadow sampling entirely.

    c.xyz = c.xyz * lighting;

    c.xyz = apply_fog(c.xyz, WorldPos.xyz, CameraPos);

	FragColor = vec4(c.xyz, c.a);

	// F3 canary: post-shadow-eligible mask + real per-vertex normal.
	// Replaces today's reliance on AMD's vec4(0,0,0,0) undeclared-output behavior.
	GBuffer1 = rc_gbuffer1_screenShadowEligible(normalize(Normal));
}
