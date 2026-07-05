#pragma once

// TERRAIN-MATERIAL-LIB-1: optional data-defined terrain material tuning.
//
// Generalizes the visual_tuning_profile.cpp TinyJson reader pattern into a
// dedicated terrain_materials.json schema covering ALL per-layer chunk-path
// tunables (tiling, tint, normalBoost, class thresholds, roughness, AO,
// blend params) -- not just the ~6 keys the visual tuning profile touches.
//
// Precedence (per USER RULING): env var wins over JSON for any overlapping
// knob (e.g. snowBrightnessDampen already has MC2_TERRAIN_SNOW_BRIGHTNESS_DAMPEN).
// JSON is applied AFTER visual_tuning.json at mission load, so it wins for
// terrain material keys if both set the same key.
//
// Gate: MC2_TERRAIN_MATERIAL_LIB, default OFF. OFF -> terrainMaterials_apply()
// returns immediately (no file read, no member writes) -> gosRenderer members
// keep their C++ initializer values verbatim (byte-identical to pre-slice).
//
// File path override: MC2_TERRAIN_MATERIAL_LIB_FILE (default data/terrain_materials.json).
// Missing file = silent no-op (same convention as visual_tuning.json).
// This is a SEPARATE file from data/visual_tuning.json (kept separate per
// user ruling: material lib = per-layer authoring, visual_tuning = global
// post/lighting). Do not merge them.
//
// TERRAIN-CONTROLMAP-ALBEDO-1: the "controlAlbedoStrength" key (shipped
// default 0.7) is read from the SAME terrain_materials.json but is gated
// INDEPENDENTLY by MC2_TERRAIN_CONTROLMAP_ALBEDO (default OFF), not by
// MC2_TERRAIN_MATERIAL_LIB -- it applies even when the rest of the material
// lib is off. Precedence: env MC2_TERRAIN_CONTROLMAP_ALBEDO_STRENGTH wins
// over the JSON key, which wins over the 0.7 shipped default. Lifts
// terrain_lod_chunk.frag's tintStrength cap so authored control-map weights
// (TERRAIN-CONTROLMAP-SAMPLE-1) can fully repaint the albedo instead of a
// faint tint pull. 0.0 (gate OFF) is algebraically identical to pre-slice.

void terrainMaterials_apply(const char* missionName);
