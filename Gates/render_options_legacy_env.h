// Gates/render_options_legacy_env.h -- the environment-variable names the render options had before they moved into options.cfg.
//
// WHAT: one table (old MC2_* name -> options.cfg key) and the translation of an old value to the text the options grammar accepts.
// WHY: the launcher used to keep these as launcher_env.json entries ("MC2_SHADOW_CSM": "0"). The engine no longer reads those names, so a
// player's old choice would silently reset to the default. The launcher migrates each key it finds once, through this table, and then drops it.
// Pure functions over text; no file IO and no logging (the launcher owns both).
#pragma once

#include "render_options.h"

#include <optional>
#include <string>
#include <string_view>

namespace mc2::options::legacy {

struct LegacyKey {
    const char* envName;   // the retired environment variable, as launcher_env.json spells it
    const char* option;    // the options.cfg key (render_options.h) that replaced it
};

// Every option that was an environment gate. PostCAS, PostCASSharpness and TonemapMode were options.cfg keys from the start and have no entry.
inline constexpr LegacyKey kLegacyKeys[] = {
    {"MC2_SHORELINE_LEGACY_SUPPRESS",        "ShorelineLegacySuppress"},
    {"MC2_CPU_PHONG_RETIRE",                 "CpuPhongRetire"},
    {"MC2_STATIC_PROP_AMBIENT_V1",           "StaticPropAmbient"},
    {"MC2_STATIC_PROP_IBL_SH",               "StaticPropIblSh"},
    {"MC2_STATIC_PROP_IBL_SUN_ROTATE",       "StaticPropIblSunRotate"},
    {"MC2_STATIC_PROP_IBL_SUN_ROTATE_FLOOR", "StaticPropIblSunRotateFloor"},
    {"MC2_STATIC_PROP_PBR_V1",               "StaticPropPbr"},
    {"MC2_SUN_AXIS_FIX",                     "SunAxisFix"},
    {"MC2_SUN_ELEVATION_FIX",                "SunElevationFix"},
    {"MC2_VFX_SOFT_PARTICLES",               "VfxSoftParticles"},
    {"MC2_VFX_LIT_PARTICLES",                "VfxLitParticles"},
};

// The options.cfg key that replaced `envName`, or nullptr when `envName` is not a retired render option.
inline const char* optionFor(std::string_view envName) {
    for (const LegacyKey& k : kLegacyKeys)
        if (envName == k.envName) return k.option;
    return nullptr;
}

// An old value as the text the options grammar takes for `opt` ("TRUE"/"FALSE" for a boolean, the number for the others), or nothing when the
// old value means nothing under the new rules (a blank, a word, a number out of range): that option keeps its default instead of a guess.
inline std::optional<std::string> translate(const Option& opt, std::string_view oldValue) {
    oldValue = detail::trim(oldValue);
    RenderOptions scratch;
    if (setFromText(scratch, opt, oldValue) != Verdict::Ok) return std::nullopt;
    if (opt.kind == Kind::Bool) return std::string(scratch.*(opt.b) ? "TRUE" : "FALSE");
    return std::string(oldValue);
}

}  // namespace mc2::options::legacy
