#pragma once
#include "QualityPresets.h"

struct EnvVarDef {
    const char* key;         // env var name (e.g. mc2::gate::name<"MC2_CLOUD_SHADOW">()), or the options.cfg key when `cfg` is set (e.g. "ShadowCSM")
    const char* label;       // display label
    bool        isBool;      // true = checkbox; false = static+edit
    const char* defaultVal;  // shown in edit / initial check state; "" = unset by default; ignored for `cfg` rows (the engine's default is used)
    const char* section;     // section header (grouped in dialog)
    // KILLSWITCH-CHECKBOX-1: what an UNCHECKED bool emits.
    //   nullptr (the default for every row that omits it) -> the variable is UNSET,
    //   which is the historical behaviour and is correct ONLY for default-OFF
    //   opt-ins, where unset == off.
    //   "0"        -> the variable is set to "0".
    // This field exists because unsetting a DEFAULT-ON killswitch does not turn
    // it off -- the engine just falls back to its ON default, so the checkbox
    // was a no-op in the off direction. Any row whose gate is default-ON must
    // set offVal="0" or its checkbox silently cannot disable anything.
    const char* offVal;
    // true = an options.cfg setting, not an environment variable: `key` is then the options.cfg key (Gates/render_options.h), launcher_env.json
    // keeps the checkbox state under that same key, and Launch writes the value into options.cfg instead of the child's environment.
    bool        cfg;
};

// Curated interactive list.  Excludes:
//   - MC2_DIRECT_FIRE_STRAIGHT / MC2_PROJECTILE_SPEED_MULT (covered by Faster Weapons)
//   - Pure debug/diagnostic vars (MC2_LOG, cost-split, trace, smoke clock, etc.)
//   - Default-ON vars where toggling is rarely useful interactively
static const EnvVarDef k_envVars[] = {
    // --- Rendering ---
    { "RemasteredBuildings",         "Building art: Remastered (off: Classic)", true, nullptr, "Rendering", "0", true },
    { "MechLabLocations",            "Mech lab: Locations view (off: Classic grid)", true, nullptr, "Rendering", "0", true },
    { "StockOverlayConcrete",        "Concrete: Stock overlay (off: new detail)", true, nullptr, "Rendering", nullptr, true },
    // Rows marked `true` in the last column are options.cfg settings (Gates/render_options.h), not environment variables: the key is the
    // options.cfg key and the engine reads it once at startup through CPrefs. The dialog starts such a row from the engine's own default for
    // that key (RowDefault), so a fresh dialog shows what the game does with no key in options.cfg and Launch never writes a value the engine
    // did not choose; its defaultVal column is ignored. offVal "0" on a default-ON row records that unchecking means FALSE.
    // The engine defaults off and the launcher starts on. Persist an explicit zero so an unchecked row survives the next launcher load.
    { k_presetKeys[3],               "Cloud shadows (engine OFF; launcher ON)", true, "1",  "Rendering", "0" },
    { "StaticPropPbr",               "Static prop PBR specular (OFF)",        true,  nullptr,   "Rendering", nullptr, true },
    { "StaticPropAmbient",           "Static prop hemisphere ambient (ON)",   true,  nullptr,   "Rendering", "0", true },
    // The TREE/PROP A/B knob (see READ-ME-FIRST-AB-TESTS.md); default ON.
    { "StaticPropIblSh",             "Static prop SH-L2 IBL ambient (ON)",    true,  nullptr,   "Rendering", "0", true },
    { k_presetKeys[1],               "Object shadows (ON)",                  true,  "1",    "Rendering", "0" },
    { k_presetKeys[2],               "Object shadow resolve (ON)",           true,  "1",    "Rendering", "0" },
    // --- Display ---
    { mc2::gate::name<"MC2_FORCE_43">(),                "Force 4:3 aspect (pillarbox widescreen)", true, "",    "Display"   },
    { mc2::gate::name<"MC2_WINDOWED">(),                "Windowed mode",                          true,  "",    "Display"   },
    { mc2::gate::name<"MC2_WIDTH">(),                   "Window width (blank = options.cfg)",     false, "",    "Display"   },
    { mc2::gate::name<"MC2_HEIGHT">(),                  "Window height (blank = options.cfg)",    false, "",    "Display"   },
    // --- Terrain ---
    // Reader mclib/terrain.cpp:147
    // `off = (v && v[0] == '0' && v[1] == '\0'); return !off;` -- DEFAULT ON, and
    // exactly "0" is the kill token. Sole reader: terrain.h:79 records that the old
    // scattered getenv(mc2::gate::name<"MC2_TERRAIN_LOD_CHUNK">()) presence checks were consolidated
    // behind mc2TerrainLodChunkEnabled() precisely so they could not drift, and a
    // whole-tree census confirms no presence-test copy survives. offVal="0" safe.
    { mc2::gate::name<"MC2_TERRAIN_LOD_CHUNK">(),       "Chunk terrain LOD renderer (ON)",       true,  "1",    "Terrain", "0" },
    // Reader mclib/terrtxm2.cpp:203 `!v || v[0] != '0'` -- DEFAULT ON, "0" kills
    // (leading-'0' test, so the exact token "0" this row emits disables it). Sole
    // reader. offVal="0" mandatory: unsetting alone left the atlas on its ON default.
    { mc2::gate::name<"MC2_COLORMAP_KTX2">(),           "BC7 KTX2 colormap atlas (ON)",          true,  "1",    "Terrain", "0" },
    { k_presetKeys[0],               "Vegetation population cap",            false, "5000000", "Terrain" },
    { k_presetKeys[4],               "Mover shadow filter samples (4-32)",   false, "32", "Terrain" },
    { k_presetKeys[5],               "Scene resolution scale (0.5-1)",       false, "1", "Terrain" },
    // --- Water / VFX ---
    // NO offVal, and adding one would be ACTIVELY WRONG. Primary reader
    // gameos_graphics.cpp:2867 `(v && v[0] && v[0] != '0') ? 0.15f : 0.0f` --
    // DEFAULT OFF, so unchecked == unset == off and an offVal is unnecessary.
    // But a SECOND reader, visual_tuning_profile.cpp:182, is a pure PRESENCE test:
    //     if (!envIsSet(mc2::gate::name<"MC2_WATER_SKYTINT">())) { gos_SetWaterSkyTintStrength(val); }
    // with envIsSet(name) == `v && v[0] != '\0'` (visual_tuning_profile.cpp:151).
    // It suppresses the per-mission waterSkyTintStrength from data/visual_tuning.json
    // whenever the var is set to ANYTHING -- "0" included. So here "0" is NOT
    // equivalent to unset: it would silently switch off per-mission water art
    // direction that unsetting leaves intact. Same hazard class as
    // MC2_TREE_DIAG_TRACE, where "=0" enables. Do not add offVal to this row.
    // 2026-08-02 USER DIRECTIVE ("just turn it off"): the box starts unchecked so a
    // launcher launch no longer trips that presence test and the per-mission
    // waterSkyTintStrength from data/visual_tuning.json applies again.
    // OFF here MUST mean UNSET (never "0") -- see presence-test note above.
    { mc2::gate::name<"MC2_WATER_SKYTINT">(),           "Water sky tint override (suppresses per-mission art; leave OFF)", true, "",  "Water/VFX" },
    { mc2::gate::name<"MC2_WATER_REFLECTION">(),        "Water SH-L2 sky reflection",            true,  "",     "Water/VFX" },
    { "VfxSoftParticles",            "Soft particles (depth-fade)",           true,  nullptr,   "Water/VFX", nullptr, true },
    { "VfxLitParticles",             "Lit particles",                         true,  nullptr,   "Water/VFX", nullptr, true },
    // --- Debug ---
    { mc2::gate::name<"MC2_IMGUI">(),                   "ImGui overlay",                         true,  "",     "Debug"     },
    { mc2::gate::name<"MC2_IMGUI_INSPECTOR">(),         "ImGui inspector panel",                 true,  "",     "Debug"     },
    // --- Lighting / Grounding A-B (2026-07-30 / 07-31) ---
    // The gates the README in the install root asks for a human verdict on.
    // Read that file before flipping these.
    //
    // Default-OFF options: unchecked writes FALSE, which is what the engine does with
    // no key, so offVal stays nullptr.
    { "SunAxisFix",                  "Sun axis fix (mech/vehicle diffuse)",   true,  nullptr,   "Lighting A/B", nullptr, true },
    { "SunElevationFix",             "Sun elevation fix (whole world)",       true,  nullptr,   "Lighting A/B", nullptr, true },
    // Aims the SH-IBL prop ambient probe at the mission sun. Default OFF.
    { "StaticPropIblSunRotate",      "Aim prop IBL probe at the sun",         true,  nullptr,   "Lighting A/B", nullptr, true },
    // Companion value row (not a checkbox): the probe's irradiance floor. "0" gives
    // the bare rotation with no floor.
    { "StaticPropIblSunRotateFloor", "  ^ probe floor (def 0.45, 0=bare)",    false, nullptr,   "Lighting A/B", nullptr, true },
    // Default-ON options that are already active on a normal launch; unchecking
    // writes FALSE (offVal "0"). Suppressing the legacy shoreline post pass has been
    // the default since LEGACY-SHORELINE-MISAIM-1.
    { "ShorelineLegacySuppress",     "Suppress legacy shoreline post pass (ON)", true, nullptr, "Lighting A/B", "0", true },
    // Gameplay elevation follows the drawn terrain surface; FALSE reverts to the legacy CPU Phong.
    { "CpuPhongRetire",              "CPU Phong retire (grounding; ON)",      true,  nullptr,   "Lighting A/B", "0", true },
    // --- Cheats (interactive play) ---
    { mc2::gate::name<"MC2_CHEAT_INFINITE_MONEY">(),    "Infinite Money",                        true,  "",     "Cheats" },
    { mc2::gate::name<"MC2_CHEAT_SALVAGE_ALL">(),       "Salvage Everything",                    true,  "",     "Cheats" },
    { mc2::gate::name<"MC2_PURCHASE_ALL">(),            "Purchase All (every mech/weapon/pilot)", true, "",     "Cheats" },
    { mc2::gate::name<"MC2_JUMPJETS_ALL">(),            "Jump jets on any mech",                 true,  "",     "Cheats" },
};
