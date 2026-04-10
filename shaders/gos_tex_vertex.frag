//#version 300 es

#define PREC highp

#include <include/noise.hglsl>

in PREC vec4 Color;
in PREC vec2 Texcoord;
in PREC float FogValue;

layout (location=0) out PREC vec4 FragColor;

uniform sampler2D tex1;
uniform PREC vec4 fog_color;
uniform int isWater;
uniform PREC float time;

void main(void)
{
    PREC vec4 c = Color.bgra;
    PREC vec4 tex_color = texture(tex1, Texcoord);
    c *= tex_color;

#ifdef ALPHA_TEST
    if(tex_color.a < 0.5)
        discard;
#endif

    // Animated water: isWater=1 base layer (slow), isWater=2 detail/spray layer (faster)
    if (isWater > 0) {
        PREC vec2 wuv = Texcoord * 6.2831853;  // 2*PI so sin tiles at integer UVs

        if (isWater == 1) {
            // Base water — moderate speed, slightly more visible
            PREC float wave1 = sin(wuv.x * 3.0 + wuv.y * 2.0 + time * 0.4)
                             + sin(wuv.x * 2.0 - wuv.y * 3.0 + time * 0.3);
            PREC float wave2 = sin(wuv.x * 5.0 + wuv.y * 4.0 - time * 0.5)
                             + sin(wuv.x * 4.0 - wuv.y * 5.0 - time * 0.2);
            wave1 *= 0.5;
            wave2 *= 0.5;

            PREC float waveBrightness = 1.0 + wave1 * 0.025 + wave2 * 0.015;
            c.rgb *= waveBrightness;
        } else {
            // Detail/spray layer — very slow, gentle undulation
            PREC float wave1 = sin(wuv.x * 3.0 + wuv.y * 2.0 + time * 0.12)
                             + sin(wuv.x * 2.0 - wuv.y * 3.0 + time * 0.08);
            PREC float wave2 = sin(wuv.x * 5.0 + wuv.y * 4.0 - time * 0.15)
                             + sin(wuv.x * 4.0 - wuv.y * 5.0 - time * 0.06);
            wave1 *= 0.5;
            wave2 *= 0.5;

            PREC float waveBrightness = 1.0 + wave1 * 0.015 + wave2 * 0.01;
            c.rgb *= waveBrightness;

            // Faint specular glint on detail layer
            PREC vec3 waveNormal = normalize(vec3(wave1 * 0.06, wave2 * 0.06, 1.0));
            PREC vec3 lightDir = normalize(vec3(0.3, 0.2, 1.0));
            PREC float spec = pow(max(dot(reflect(-lightDir, waveNormal), vec3(0.0, 0.0, 1.0)), 0.0), 96.0);
            c.rgb += vec3(spec * 0.08);
        }
    }

	if(fog_color.x>0.0 || fog_color.y>0.0 || fog_color.z>0.0 || fog_color.w>0.0)
    	c.rgb = mix(fog_color.rgb, c.rgb, FogValue);
	FragColor = c;
}

