#pragma once
// TERRAIN-DECAL-SLICE-0C — live cliff mesh-decal placement tuning.
//
// Holds the 5 placement knobs (scale/offset/lateral/lift/yaw) as MUTABLE global
// state, initialized from the MC2_TERRAIN_DECAL_* env defaults so launch behavior
// is byte-identical when the panel is untouched. bdactor.cpp's BldgAppearance::
// registerStatic captures the per-decal build context (recipe index + placement
// origin + sampled terrain normal) here, and both registration and the live ImGui
// panel share buildCliffWallMatrix() so a slider drag recomputes and re-uploads the
// exact same face frame that registration built.
//
// The heavy path (Stuff::Matrix4D, registry writeback) lives in the .cpp. The GUI
// panel (GuiRuntime, which cannot cheaply include Stuff.hpp / the batcher headers)
// consumes ONLY the plain-float / bool API below, forward-declared at its call site.

#include <cstdint>

namespace Stuff { class Matrix4D; class Vector3D; }

namespace CliffDecalTuning {

// The five live placement knobs. Defaults match the Slice-0A/0B hardcoded values
// (scale=1, offset=8, lateral=0, lift=0, yaw=0), overridable at launch via the
// MC2_TERRAIN_DECAL_* env vars. Read once at first access, then mutated live.
struct Knobs {
    float scale   = 1.0f;
    float offset  = 8.0f;
    float lateral = 0.0f;
    float lift    = 0.0f;
    float yawDeg  = 0.0f;
    float pitchDeg = 0.0f; // TERRAIN-DECAL-PITCH: lean the wall top backward toward the hill.
};

// Mutable knob accessor (lazily seeded from env on first call). GUI edits this.
Knobs& knobs();

// Shared face-frame builder used by BOTH registerStatic and the live update.
// xlatPosition: shape-world placement origin (Stuff row-vector translation row).
// nAcc: accumulated (un-normalized) terrain normal in WORLD axes (nx,ny,nz).
// Reads the current knobs(). Writes the CLIFF_WALL mat4 into out.
void buildCliffWallMatrix(const Stuff::Vector3D& xlatPosition,
                          const Stuff::Vector3D& nAcc,
                          const Knobs& k,
                          Stuff::Matrix4D& out);

// Called by registerStatic once the CLIFF_WALL decal recipe is registered.
// Captures the context needed to recompute the transform live. recipeIndex<0
// clears the captured decal (no live target this mission).
void captureDecalContext(int32_t recipeIndex,
                         const Stuff::Vector3D& xlatPosition,
                         const Stuff::Vector3D& nAcc);

// Cleared at mission unload so a stale recipe index can't be written into a new
// mission's registry.
void clearDecalContext();

// ---- Plain GUI-facing API (forward-declarable without Stuff/batcher headers) ----

// True iff a CLIFF_WALL decal was captured this mission (i.e. MC2_TERRAIN_DECAL=1
// and a MarbleCliff was placed). The ImGui panel renders nothing when false.
bool cliffDecal_hasDecal();

// Copy current knob values out (any pointer may be null).
void cliffDecal_getKnobs(float* scale, float* offset, float* lateral,
                         float* lift, float* yawDeg, float* pitchDeg);

// Set knob values (call from the ImGui sliders) then re-solve + re-upload the
// captured decal's transform. No-op if no decal is captured. Cheap enough to call
// every frame the panel is open.
void cliffDecal_setKnobsAndApply(float scale, float offset, float lateral,
                                 float lift, float yawDeg, float pitchDeg);

// Print the current knob values to stderr so a tuned placement can be captured
// into view-terrain.bat (as MC2_TERRAIN_DECAL_* env vars).
void cliffDecal_logValues();

} // namespace CliffDecalTuning
