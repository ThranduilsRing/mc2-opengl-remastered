// T1.16 — (E)-owned spotlight slot tagging registry.
//
// Single-source tag/untag set for the per-slot per-frame diagnostic that
// converts T1.15's population-level "86-88% rejection" stat into per-slot
// ground truth. Tagged at the three lazy-init sites (bldg/mech/gv) and
// untagged by the camera when a slot is removed or reused. Camera::updateLights consults
// is_e_slot() inside the POINT/SPOT branch to emit only on (E)-owned slots.
//
// Tags also classify shipped mover pools; MC2_SPOT_DIAG gates diagnostic output.

#pragma once
#include <cstdint>

namespace mc2_spotlight_diag {

enum class SourceClass : uint8_t { Bldg = 0, Mech = 1, Gv = 2 };
using enum SourceClass;

// Tag a slot when (E) lazy-init registers a light. Idempotent; safe
// across duplicate calls (per-mission lifecycle should not re-register,
// but the set is hash-checked to be safe).
// GHOST-LIGHT-DUMP-1 (batch-7 item 5): `owner` (the registering appearance,
// may be null) is recorded for the MC2_LIGHT_POOL_DUMP owner-attribution dump.
// A slot number is meaningful only within its registering camera.
void tag_slot(const void* camera, long slot, SourceClass src, const void* owner = nullptr);

// GHOST-LIGHT-DUMP-1: owner appearance recorded at tag time (null if none).
const void* slot_owner(const void* camera, long slot);

// Camera slot ownership clears this on removal and before reuse.
void untag_slot(const void* camera, long slot);

// Membership probe — used by Camera::updateLights.
// outSrc may be nullptr.
bool is_e_slot(const void* camera, long slot, SourceClass* outSrc);

// Clear every tag for tests, or only the destroyed camera's tags in production.
void reset();
void reset(const void* camera);

// Whether MC2_SPOT_DIAG env is set. Cached at startup.
bool is_enabled();

// MOVER-LIGHT-HALOGEN-1: aRGB color for MOVER-attached SpotLight_ registrations
// (mech3d.cpp / gvactor.cpp). Legacy hardcoded 0xffffff00 (pure yellow = "egg
// yolk" pools). Default is a desaturated warm halogen 0xFFEED1; override with
// MC2_LIGHT_POOL_MOVER_COLOR=RRGGBB hex (e.g. FFFF00 restores legacy yellow).
// Buildings (bdactor.cpp, authored warm 0xffe8c870) are NOT routed through this.
unsigned long mover_light_argb();

// MOVER-LIGHT-FORWARD-1: forward offset (world units) applied to mover
// SpotLight_/searchlight pool centers along the mover's facing — headlights
// illuminate AHEAD of the vehicle instead of an omni puddle centered under
// it. MC2_LIGHT_POOL_MOVER_FWD override (default 24, clamp [0,120]; 0 = off,
// legacy centered pools).
float mover_light_fwd_offset();

} // namespace mc2_spotlight_diag
