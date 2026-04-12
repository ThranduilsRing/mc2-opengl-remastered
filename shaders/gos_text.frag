//#version 300 es

#define PREC highp

in PREC vec4 Color;
in PREC vec2 Texcoord;
in PREC float FogValue;

layout (location=0) out PREC vec4 FragColor;
layout (location=1) out PREC vec4 GBuffer1;

uniform sampler2D tex1;
uniform PREC vec4 Foreground;

uniform PREC vec4 fog_color;

void main(void)
{
    PREC vec4 c = Foreground;//Color;
    PREC vec4 mask = texture(tex1, Texcoord);
    c *= mask.xxxx;
	if(fog_color.x>0.0 || fog_color.y>0.0 || fog_color.z>0.0 || fog_color.w>0.0)
		c.rgb = mix(fog_color.rgb, c.rgb, FogValue);
    FragColor = c;
    GBuffer1 = vec4(0.5, 0.5, 1.0, 0.0);
}

