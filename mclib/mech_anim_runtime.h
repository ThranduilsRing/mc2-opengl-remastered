// mech_anim_runtime.h — BT2018-SKEL-ENGINE-1B-RUNTIME
//
// Per-frame CPU re-bake of IMPORTED skinned mechs (Assimp BT2018 GLB). The
// importer (assimp_importer.cpp) bakes ONE static pose into a merged TG type
// shape; this drives that same merged geometry per frame from a looping clip so
// the mech MOVES. Engine-only — the implementation lives in assimp_importer.cpp
// (it needs Assimp + the shared mc2skel FK), so NO Assimp/TG types leak through
// this header. Callers (mech3d.cpp) see only plain scalars.
//
// Scope (1B): single forced clip, CPU mech path (MC2_GPU_MECHS=0). Shared-type
// re-bake → all actors of the chassis animate in lockstep (one demo mech). Gate
// MC2_MECH_IMPORT_ANIMATE (default OFF, opt-in) + MC2_MECH_IMPORT_FORCE_CLIP
// names the clip. Per-actor animation and the GPU mech path are deferred.
#pragma once
namespace mc2::sim { struct ImportedAnimState; }

namespace mc2mechanim {

// Per-frame movement signals from the calling Mech3DAppearance, as plain scalars
// (no engine types leak into this header). 1C uses these to pick the clip:
//   gestureId     : Mech3DAppearance::currentGestureId (stock gesture, already
//                   debounced by the transition state machine — free hysteresis)
//   px,py,pz      : world position (diffed frame-to-frame for speed)
//   legHeadingDeg : leg heading in degrees (diffed for turn-in-place direction)
struct MechMotion {
    int   gestureId = 0;
    float px = 0.0f, py = 0.0f, pz = 0.0f;
    float legHeadingDeg = 0.0f;
    bool  destroyed = false;   // P9: death clips instead of knockdown/downed clips
};

// Advance + re-bake every registered imported animated mech, once per frameStamp.
// Idempotent on frameStamp (g_mc2FrameCounter), so it is safe to call from every
// mech's updateGeometry — only the first call for a given frame does work. A
// no-op (single branch) when no imported animated mech is registered.
//
// 1C: unless a clip is PINNED (MC2_MECH_IMPORT_FORCE_CLIP), the active clip is
// selected each frame from `motion` (idle / walk / run / turn). frameLengthSec is
// the per-frame dt (timing.h `frameLength`).
//
// PER-ACTOR (GPU path): call once per actor per frame with its own actorKey (the
// actor's instance TG_MultiShape*) and typeKey (its TG_TypeMultiShape*). Each actor
// advances its own clip/palette, so two mechs of one chassis no longer lockstep.
// ASSIMP-MECH-PAUSE-GATE-1: advanceClock=false (game paused) freezes the clip clock
// and clip selection; the pose is still re-baked at the frozen time so geometry/skinning
// keeps running. Defaults true so non-pause-aware callers are unchanged.
void TickImportedMechs(float frameLengthSec, unsigned frameStamp, const MechMotion& motion,
                       const void* actorKey, const void* typeKey, bool advanceClock = true,
                       mc2::sim::ImportedAnimState* authoritative = nullptr);

// True once at least one imported mech anim is registered (gate on + bones).
bool AnyImportedAnim();

// --- BT2018-SKEL-GPU-PALETTE-PLACEMENT-1 (default OFF, MC2_MECH_IMPORT_GPU=1) ---
// Imported mech rides the existing GPU skinned-mech path. The VBO keeps the proven
// assembled-REST geometry (Y-up, mechToMC2Pos). Each frame we supply a per-bone
// MODEL DELTA D_i = (A1·S·C_i)·(A2·S·R_i)^-1 that maps the Y-up rest vertex to the
// Z-up animated MODEL pose (A2=mechToMC2Pos matches the VBO; A1=Z-up so the engine's
// shapeToWorld + the shader's (-x,z,y) swap place it like a stock mech). Placement
// is composed by the batcher from the actor's live node shapeToWorld:
//   boneT_i = shapeToWorld_root · D_i. All args plain / void* (TG_TypeMultiShape*).
bool ImportedGpuEnabled();

// If `typeMulti` is a registered imported-GPU mech: returns joint count (>0), sets
// *perVertexBone to a *numVerts-long uint8 per-type-vertex bone index. Else 0.
int ImportedGpuTypeInfo(const void* typeMulti, const unsigned char** perVertexBone, int* numVerts);

// Current per-frame MODEL-DELTA matrices for THIS ACTOR (keyed by actorKey = the
// instance TG_MultiShape*): *mats16 -> count*16 floats, each a row-major 4x4 (NOT
// placement-composed, NOT GpuMechBone-packed — the batcher composes shapeToWorld and
// packs). Returns count (== joint count) or 0.
int ImportedGpuModelDelta(const void* actorKey, const float** mats16);

// Per-actor foot-ground lift to add to the placed palette translation (imported GLBs
// are pelvis-origin). 0 if none.
float ImportedGpuLift(const void* actorKey);
// Which world translation component the lift targets (0/1/2 = Stuff.x/y/z).
int ImportedGpuLiftAxis();

// IMPORTED-ACTOR-STABLE-KEY-1: drop this actor's per-frame anim state (keyed by the
// instance TG_MultiShape*). Call when that instance is destroyed/swapped (LOD change,
// death-shape swap, final teardown) so a later allocation reusing the same address
// cannot inherit a stale pose for a few frames. Safe/no-op if the key isn't present.
void UnregisterImportedActor(const void* actorKey);
// Drop the imported-anim entry (and its scene) of a type being freed.
void UnregisterImportedType(const void* multi);
void CopyImportedActorPose(const void* source, const void* destination);

// AO-1: imported mech's AO texture name (materials.ao.tga from the package), or nullptr
// if not imported / no AO declared. Caller loads it + binds on texture unit 6.
const char* ImportedMechAoTexName(const void* typeKey);

// NORMALS-1: imported mech's normal-map texture name (materials.normal.tga from the
// package), or nullptr if not imported / no normal declared. Caller loads it + binds on
// texture unit 7.
const char* ImportedMechNormalTexName(const void* typeKey);

// BT2018-MECH-NODE-MANIFEST-1A. Resolve an MC2 semantic node name (e.g.
// "weapon_rightarm", "hit_left") to the imported mech's animated joint WORLD
// position, via the per-mech package manifest (mc2Name -> source joint) + the live
// clip bone globals. rootToWorld12 = the actor's listOfShapes[0].shapeToWorld
// .entries (row-major 3x4, 12 floats). Read-only: no FK mutation, no clip change.
// Returns false (caller falls back to the legacy TG node lookup) when not an
// imported mech, no manifest, the name is unmapped, or the joint/globals are absent.
bool GetImportedNodeWorld(const void* actorKey, const void* typeKey,
                          const char* mc2Name, const float* rootToWorld12,
                          float outXYZ[3]);

}  // namespace mc2mechanim
