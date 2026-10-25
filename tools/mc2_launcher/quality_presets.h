// Existing renderer controls grouped for the launcher's explicit quality buttons.
#pragma once

inline constexpr const char* k_presetKeys[] = {
    "MC2_VEGETATION_MAX", "MC2_SHADOW_ENABLE", "MC2_VK_POST_SCREENFX_SHADOW", "MC2_CLOUD_SHADOW",
};
struct GfxPreset {
    const char* label;
    int shadQual, aa;
    const char* values[4];
};
inline constexpr GfxPreset k_gfxPresets[] = {
    {"Low",    2, 0, {"1000000", "0", "0", "0"}},
    {"Medium", 2, 0, {"1000000", "1", "0", "0"}},
    {"High",   2, 1, {"5000000", "1", "1", "1"}},
};
