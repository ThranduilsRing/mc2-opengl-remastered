// GameOS/gameos/ibl_sh_runtime.h
//
// V-IBL-STATIC-1 runtime cross-TU bridge.
//
// Defines (in gos_static_prop_batcher.cpp):
//   float g_iblShStrength;     // ImGui slider value, default 0.5f.
//
// The options.cfg key StaticPropIblSh (read once as s_iblShEnabled inside the
// batcher TU) is the authoritative switch -- the slider only modulates strength
// when the option is on. Per-frame upload computes
// `s_iblShEnabled ? g_iblShStrength : 0.0f` so StaticPropIblSh = FALSE uploads
// 0.0 (byte-identical off, regardless of slider value). Default on.
//
// Consumers: GuiRuntime/EditorInspector.cpp (ImGui slider).
//
// Firewall: no GL, no game-side includes. Header-only extern decl.

#pragma once

extern float g_iblShStrength;

// V-MATERIAL-PBR-3 runtime cross-TU bridge. Same idiom as g_iblShStrength:
// the options.cfg key StaticPropPbr (read once as s_pbrV1Enabled inside the
// batcher TU) is the authoritative switch; the slider only modulates strength
// when the option is on. Per-frame upload
// computes `(s_pbrV1Enabled && !s_viewUniformsDisabled) ? g_pbrV1Strength
// : 0.0f`. Default 0.5f at process start (slider range 0..3).
extern float g_pbrV1Strength;

// V-MATERIAL-PBR-3-TUNE-UI: ImGui-driven runtime roughness override.
// When _Enabled is false (default), CPU uploads -1.0 sentinel and the
// shader falls through to its literal 0.6 fallback (byte-identical to
// V-MATERIAL-PBR-2-TUNE baseline). When true, _Value (clamped 0.05..1.0)
// is uploaded and replaces the literal. Debug/tuning surface only; MaterialGpu
// roughness/metallic consumption lives in the fragment PBR path.
extern bool  g_pbrV1RoughnessOverrideEnabled;
extern float g_pbrV1RoughnessOverrideValue;

// V-IBL-STATIC-2: inspector accessor for the active per-mission SH set.
// Forwarded here (instead of including the batcher header) so GuiRuntime
// stays independent of Stuff/. Implementation lives in
// gos_static_prop_batcher.cpp. Never returns nullptr.
const char* ibl_sh_runtime_currentSetName();
