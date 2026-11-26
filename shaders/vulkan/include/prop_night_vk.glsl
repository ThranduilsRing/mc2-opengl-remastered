#ifndef MC2_PROP_NIGHT_VK_GLSL
#define MC2_PROP_NIGHT_VK_GLSL
// Night light on static props (uFrame.nightDarkness: 0 day and evening, 1 full night; the same
// mc2NightLightFactor() that fades the sun shadows and dims the water's sky).
// At night there is no dominant light direction: night missions author a low or horizontal
// sun/moon, which lit one face of every building like daylight and left the rest black. Most of
// that light becomes a soft overhead fill, matching the faded shadows. Authored non-tag vertex
// light (near-white on hangars and terminals) is environment light and dims with the night.
const float kNightDiffuseShare  = 0.8;    // share of the sun/moon term that turns into the fill
const float kNightAuthoredLight = 0.2;    // encoded scale of authored vertex light at full night

// 1 by day; the share of the light that still has a direction at night.
float propNightDirectShare() { return 1.0 - kNightDiffuseShare * uFrame.nightDarkness; }

// Weight of the sun/moon colour for a surface: its directional N.L share plus the overhead fill
// (full on roofs, half on walls, none underneath). By day this is exactly ndl.
float propNightSunWeight(float ndl, vec3 nMC2) {
    float fill = kNightDiffuseShare * uFrame.nightDarkness * (0.5 + 0.5 * nMC2.z);
    return ndl * propNightDirectShare() + fill;
}

#endif
