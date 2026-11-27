#ifndef MC2_LIGHTING_VK_GLSL
#define MC2_LIGHTING_VK_GLSL
// Incident irradiance, before albedo/BRDF, emission and local lights. Callers
// supply material-specific normal/SH response and baked-art compensation.
// Live sun/SH already carry the mission scale; only prelit art needs nightScale.
vec3 incidentLight(vec3 ambient, vec3 sun, vec3 nightScale, float visibility)
{
    return (ambient + sun * visibility) * nightScale;
}

float computeNdotLBand(vec3 N, vec3 lightDir)
{
    float NdotL   = dot(N, lightDir);
    float diffuse = clamp(NdotL, 0.02, 1.0);
    return mix(0.35, 1.20, diffuse);
}

#endif
