// Renderer controls grouped for the player UI's explicit quality buttons.
#pragma once
#include "../Gates/gate_name.h"
#include "../Gates/render_options.h"

inline constexpr const char* k_presetKeys[] = {
    mc2::gate::name<"MC2_VEGETATION_MAX">(), mc2::gate::name<"MC2_SHADOW_ENABLE">(),
    mc2::gate::name<"MC2_VK_POST_SCREENFX_SHADOW">(), mc2::gate::name<"MC2_CLOUD_SHADOW">(),
    mc2::gate::name<"MC2_SHADOW_MOVER_SAMPLES">(), mc2::gate::name<"MC2_RENDER_SCALE">(),
};
struct GfxPreset {
    const char* label;
    int shadQual, aa, baseQuality;
    const char* values[6];
    void apply(mc2::options::RenderOptions& draft) const {
        draft.baseQuality = baseQuality;
        draft.shadowQuality = shadQual;
        draft.spatialAA = aa;
    }
};
inline constexpr GfxPreset k_gfxPresets[] = {
    {"Low",    0, 0, 1, {"5000000", "0", "0", "0", "4",  "0.5"}},
    {"Medium", 1, 0, 2, {"5000000", "1", "0", "0", "24", "0.5"}},
    {"High",   2, 1, 2, {"5000000", "1", "1", "1", "32", "1"}},
    {"Potato", 0, 0, 0, {"5000000", "0", "0", "0", "4",  "0.5"}},
};
