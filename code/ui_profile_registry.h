#ifndef UI_PROFILE_REGISTRY_H
#define UI_PROFILE_REGISTRY_H
// UI-PROFILE-1 (Phase-2 slice 6): optional additive-merge UIProfile data source.
//
// Mirrors the model_override_registry / anim_override_registry shape the recon
// sanctioned (PRESENTATION-LOADOUT-STRANGLER-RECON-1.md "a proven strangler
// pattern already exists"): a gated, additive, CSV/legacy-fallback registry that
// maps a unit's chassis filename -> an authored UIProfile. STOCK declares NONE,
// so the registry is empty and every lookup returns null -> the accessor seam
// keeps its legacy iconFileNames[]/IDS_ read -> byte-identical stock UI.
//
// Engine-only glue (the .cpp opens an optional data .fit via FitIniFile, which
// drags file I/O); the pure facet + override decision live in ui_profile.h and
// are offline-tested. This header exposes only a thin, header-safe lookup.
struct UIProfile;  // fwd (ui_profile.h defines it as a POD struct)

// Resolve the authored UIProfile for a chassis filename (case-insensitive), or
// null if none is declared / the gate is OFF / the optional data source is
// absent. Lazy: the optional registry file is read once on first call. The
// returned pointer is owned by the registry and stays valid for the process.
const UIProfile* uiProfileForChassis(const char* chassisFileName);

#endif // UI_PROFILE_REGISTRY_H
