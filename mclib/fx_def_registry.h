// fx_def_registry.h — FX-DEFS-SIDECAR-1.
// Engine-independent parse of data/effects/defs/<EffectName>.fxdef.json — the
// replaceability keystone from .claude/VFX-MODERNIZATION-PROPOSAL-1.md §3.1.
// Sibling of anim_override_registry.h / model_override_registry.h (same house
// pattern: parse -> validate -> log-and-drop-never-fatal -> apply at engine
// merge point). No TG_*/GameOS deps in the parse layer.
//
// Schema v1 (overlay tier only; §3.1): one JSON file per effect, keyed by the
// SAME string EffectLibrary::Find()/effects.csv use (case-insensitive engine
// lookup; this registry normalizes to lowercase for its own map key).
//   {
//     "effect": "Fireball",
//     "disabled": false,
//     "texture": "vfxTxrPrtl_fireball.tga",
//     "blend": "additive" | "alpha",
//     "curves": {
//       "alpha": <const-number>, "red": <const-number>, "green": <const-number>,
//       "blue": <const-number>, "scale": <const-number>, "lifeSpan": <const-number>
//     }
//   }
// v1 only supports CONSTANT curve overrides (collapses the age curve to a
// flat value), matching tools/mc2fx's `patch.json` "constant" tier. Unknown
// top-level keys are tolerated (forward-compat with schema v2 §3.1) but
// unknown "curves" sub-keys are logged and dropped. "flipbook"/"erosion"/
// "distortion"/"light" keys are schema-reserved for later slices (#4-#6) —
// parsed-and-ignored here, never an error.
//
// Merge point: engine applies overlays AFTER SpecLibrary::Load() completes
// (code/mechcmd2.cpp, right after gosFX::EffectLibrary::Instance->Load()).
// Gate: MC2_FX_DEFS (default OFF). When OFF or no defs present, ApplyAll()
// is a no-op and the loaded blob is byte-identical to stock.
#pragma once
#include <string>
#include <unordered_map>
#include <vector>

namespace gosFX { class Effect__Specification; }

namespace mc2 { namespace particles { class SpecLibrary; } }

namespace mc2fxdefs {

// One field edit under "curves". field is normalized lowercase
// (alpha/red/green/blue/scale/lifespan); value is the constant to apply.
struct CurveOverride {
    std::string field;
    double value = 0.0;
};

// One parsed def. effectKey is the lowercased match key; effectNameRaw is the
// original "effect" string as authored (used for log lines / fxlint).
struct EffectDef {
    std::string effectNameRaw;
    std::string effectKey;       // lowercased effectNameRaw — the lookup key
    std::string sourcePath;      // file this def was parsed from (diagnostics)
    bool        disabled = false;
    bool        hasTexture = false;
    std::string texture;         // relative texture name (as authored; resolved
                                  // through the same MLRTexturePool lookup the
                                  // stock loader uses — no path validation here)
    bool        hasBlend = false;
    bool        blendAdditive = false;   // true="additive", false="alpha"
    std::vector<CurveOverride> curves;
    // Reserved v1-forward keys (flipbook/erosion/distortion/light): NOT parsed
    // into structured fields yet (slices #4-#6). Presence is tolerated.
};

class EffectDefRegistry {
public:
    // Clears registry then loads every *.fxdef.json in dir (non-recursive).
    // Returns count of valid defs loaded. Malformed files are logged+skipped.
    int loadFromDir(const std::string& dir);

    // Additive: loads dir without clearing; entries WIN on dup effectKey
    // (mod overlay semantics — same as AnimOverrideRegistry::mergeFromFile).
    // Returns count merged in (not total).
    int mergeFromDir(const std::string& dir);

    // Returns the def for effectName (case-insensitive), or nullptr.
    const EffectDef* resolve(const char* effectName) const;

    int count() const { return (int)defs_.size(); }

    // Apply every loaded def to the matching spec in lib (by name, via
    // SpecLibrary::Find semantics — case-insensitive). Effects with no def
    // are untouched. Returns count of specs actually modified. Never throws;
    // per-spec failures are logged and skipped (fail loud, never fatal, per
    // proposal §3.1 "friendly warning, not failure").
    int applyAll(mc2::particles::SpecLibrary* lib) const;

    // Lazy singleton: loads base data/effects/defs/ then merges the active
    // mod's data/effects/defs/ (MC2_ACTIVE_MOD) on first use. Cheap no-op
    // when neither directory exists. Gate MC2_FX_DEFS is checked by the
    // CALLER (code/mechcmd2.cpp) before invoking applyAll() — instance()
    // itself always parses (parsing is free; only the apply is gated) so
    // fxlint-style diagnostics stay available under MC2_LOG regardless of
    // the feature gate.
    static EffectDefRegistry& instance();

private:
    std::vector<EffectDef> defs_;
    std::unordered_map<std::string, size_t> byKey_;  // effectKey -> defs_ index
};

}  // namespace mc2fxdefs
