// Renderer controls grouped for the launcher's explicit quality buttons.
#pragma once
#include "../../Gates/gate_name.h"

inline constexpr const char* k_presetKeys[] = {
    mc2::gate::name<"MC2_VEGETATION_MAX">(), mc2::gate::name<"MC2_SHADOW_ENABLE">(),
    mc2::gate::name<"MC2_VK_POST_SCREENFX_SHADOW">(), mc2::gate::name<"MC2_CLOUD_SHADOW">(),
    mc2::gate::name<"MC2_SHADOW_MOVER_SAMPLES">(), mc2::gate::name<"MC2_RENDER_SCALE">(),
};
struct GfxPreset {
    const char* label;
    int shadQual, aa;
    const char* values[6];
};
inline constexpr GfxPreset k_gfxPresets[] = {
    {"Low",    2, 0, {"5000000", "0", "0", "0", "4",  "0.5"}},
    {"Medium", 2, 0, {"5000000", "1", "0", "0", "8",  "0.5"}},
    {"High",   2, 1, {"5000000", "1", "1", "1", "32", "1"}},
};
