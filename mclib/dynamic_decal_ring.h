// dynamic_decal_ring.h
// Fixed-capacity ring buffer for runtime terrain impact decals
// (scorch/crater rings at weapon impact sites with lifetime alpha fade).
//
// Gate: MC2_DYNAMIC_DECALS (default OFF).
// Off behavior: spawn() is a no-op; gatherToDecalBatch() pushes nothing.
//
// Design constraints:
//   - No heap allocation; fixed 64-slot array in static storage.
//   - Feeds the EXISTING gos_PushDecal / drawDecals path; no second renderer.
//   - Each slot submits 2 triangles (one quad) per frame via gos_PushDecal.
//   - Alpha fade: linear 1.0 -> 0.0 over the final 20% of lifetime.
//   - Spawn API is intentionally not public GameOS API; callers are in mclib.
//
// [RENDER_CONTRACT:Pass=TerrainDecal id=dynamic_decal_ring]
//   Feeds gos_PushDecal, which is flushed by gos_DrawDecals() in
//   gosRenderer::drawDecals(). No direct GL calls here.

#pragma once

#include <cstdint>

// Forward declaration using 'class' (not 'struct') to match Stuff::Vector3D's
// actual definition — MSVC name mangling differs between class (V) and struct (U).
#include "math/glm_config.h"

namespace DynDecal {

// Per-slot descriptor (stored in ring buffer).
// 28 bytes; texHandle stored as resolved gos handle at spawn time.
struct Slot {
    float        wx, wy, wz;   // world centre (MC2 space: x=east, y=north, z=elev)
    float        radius;       // half-size of the quad (world units)
    float        rotation;     // rotation angle in radians
    unsigned int texHandle;    // gos texture handle (resolved by caller at spawn)
    uint16_t     lifetimeMs;   // total life in milliseconds (0 = slot empty)
    uint16_t     ageMs;        // elapsed ms so far (updated each frame)
};

static constexpr int kCapacity = 64;

// ─── Spawn ──────────────────────────────────────────────────────────────────
// Add a decal at the given world position.  Gate-off: returns immediately.
// texHandle: gos texture handle resolved by the caller (crater.cpp has access
//            to the protected craterTextureIndices via the class method).
// rotationRad: rotation angle in RADIANS (caller converts from game degrees).
// lifetimeMs: total fade duration (0 = use default 6000 ms).
void spawn(const glm::vec3& worldPos, float radius,
           float rotationRad, unsigned int texHandle, uint16_t lifetimeMs = 0);

// ─── Update + Gather ────────────────────────────────────────────────────────
// Advance age of all live slots by frameDeltaSec, then push triangles via
// gos_PushDecal() for all slots that are still alive.
// Call from txmmgr.cpp renderLists() immediately before gos_DrawDecals().
// Gate-off: pushes nothing, touches no GL state.
void gatherToDecalBatch(float frameDeltaSec);

// ─── ImGui debug info ───────────────────────────────────────────────────────
// Returns the count of currently live (age < lifetime) slots.
int liveCount();

// ─── Read-only snapshot (DECAL-INTEGRATE-1) ──────────────────────────────────
// Copy currently-live slots into outSlots (caller-supplied, capacity >= kCapacity).
// Returns the number written. Pure read; does NOT advance ages or expire slots
// (that remains gatherToDecalBatch()'s job). Lets a SECOND, parallel consumer
// (the box-decal screen-space projector) read impact sites without disturbing the
// baked gos_PushDecal path. Each returned slot carries a precomputed alpha [0,1]
// in the unused high bits is NOT done — caller recomputes fade from age/lifetime if
// needed; here we expose the raw Slot plus a parallel alpha array for convenience.
int snapshotLiveSlots(Slot* outSlots, float* outAlpha, int maxOut);

} // namespace DynDecal
