//#version 420 (version provided by prefix)

in vec2 TexCoord;
layout(location = 0) out vec4 FragColor;

uniform vec3 sunDir;
uniform vec3 zenithColor;
uniform vec3 horizonColor;
uniform vec3 sunColor;

void main()
{
    float t = TexCoord.y;

    // Two-stage gradient: horizon haze band + upper sky
    // Rapid transition near horizon gives atmospheric density feel
    float hazeT = smoothstep(0.0, 0.25, t);   // haze fades quickly
    float skyT  = smoothstep(0.15, 0.85, t);  // sky gradient is broader

    vec3 hazeColor = mix(horizonColor, zenithColor, 0.3);  // warm mid-tone
    vec3 sky = mix(horizonColor, hazeColor, hazeT);
    sky = mix(sky, zenithColor, skyT);

    // Subtle warm glow near horizon (atmospheric scattering look)
    float horizonGlow = exp(-t * 5.0) * 0.06;
    sky += sunColor * horizonGlow;

    FragColor = vec4(sky, 1.0);
}
