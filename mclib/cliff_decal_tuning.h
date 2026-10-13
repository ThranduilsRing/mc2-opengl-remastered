#pragma once
// TERRAIN-DECAL-SLICE-0C — live cliff mesh-decal placement tuning.
//
// Holds the placement knobs (scale/offset/lateral/lift/yaw/pitch) as MUTABLE global
// state, starting from the frozen defaults below so launch behavior is
// byte-identical when the panel is untouched. bdactor.cpp's BldgAppearance::
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

// The frozen placement defaults (the former MC2_TERRAIN_DECAL_* gate defaults). A tuned
// placement found with the panel is frozen by changing these.
inline constexpr float kDefaultScale    = 1.0f;
inline constexpr float kDefaultOffset   = 8.0f;    // outward distance along the wall facing axis, world units
inline constexpr float kDefaultLateral  = 0.0f;    // side-to-side along the wall contour axis, world units
inline constexpr float kDefaultLift     = 0.0f;    // along world-up, world units
inline constexpr float kDefaultYawDeg   = 0.0f;    // rotation of the facing/tangent about world-up
inline constexpr float kDefaultPitchDeg = 0.0f;    // lean of the wall top backward toward the hill

// The live placement knobs: the defaults above, then mutated by the panel.
struct Knobs {
    float scale    = kDefaultScale;
    float offset   = kDefaultOffset;
    float lateral  = kDefaultLateral;
    float lift     = kDefaultLift;
    float yawDeg   = kDefaultYawDeg;
    float pitchDeg = kDefaultPitchDeg;
};

// Mutable knob accessor. GUI edits this.
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

// True iff a CLIFF_WALL decal was captured this mission (a MarbleCliff was
// placed). The ImGui panel renders nothing when false.
bool cliffDecal_hasDecal();

// Copy current knob values out (any pointer may be null).
void cliffDecal_getKnobs(float* scale, float* offset, float* lateral,
                         float* lift, float* yawDeg, float* pitchDeg);

// Set knob values (call from the ImGui sliders) then re-solve + re-upload the
// captured decal's transform. No-op if no decal is captured. Cheap enough to call
// every frame the panel is open.
void cliffDecal_setKnobsAndApply(float scale, float offset, float lateral,
                                 float lift, float yawDeg, float pitchDeg);

// Print the current knob values to stderr so a tuned placement can be frozen into
// the kDefault* constants above.
void cliffDecal_logValues();

} // namespace CliffDecalTuning
