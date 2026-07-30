// GameAdapters/StaticPropRenderAdapter.h
//
// Slice M1: the ONLY module that may bridge gameData and engine sides.
// Per spec section 12 carve-out, an adapter header may forward-declare
// game-side types; the .cpp may include real game-side headers.
//
// Spec: docs/superpowers/specs/2026-05-22-renderworld-boundary-spec.md
//       section 10 (legacy adapter migration), section 13 (M1 scope).
//
// Sentinel translation rule (load-bearing, spec section 10 amendment
// 2026-05-22): -1 (recipeIndex) <-> RenderCore::RenderObjectHandle::invalid()
// at each game/engine seam. M1 has two seams (GameAdapters seam +
// RenderWorld legacy-backend seam); this header lives at the
// GameAdapters seam. The int32_t MUST NOT leak upward past this header;
// the Handle MUST NOT leak downward into the registry.

#pragma once

#include <cstddef>
#include <cstdint>

#include "../RenderCore/Handle.h"
#include "../RenderCore/MaterialGpu.h"

// Forward-decl game-side base. Spec section 12 carve-out: adapter
// HEADERS may forward-decl; adapter .cpp may include real header.
class Appearance;
class TG_MultiShape;
struct GpuStaticPropInstance;  // adapter .cpp translates via the POD mirror

namespace GameAdapters {
namespace StaticProp {

// M1 fix (adversarial review pass 2 + order-correct split): spec D3 entry-points.
//
// Per-mission lifecycle — split so GpuMechBatcher::onMapLoad/Unload can be
// interleaved at the call-site (call order is fragile; this must be pure
// indirection, zero behavior change vs the original Mission::init/destroy).
//
// init call order (byte-identical to original):
//   1. beginMissionEarly()            → GpuStaticPropBatcher::onMapLoad()
//   2. GpuMechBatcher::onMapLoad()    → (caller, NOT this adapter)
//   3. beginMissionLate(missionName)  → setMissionForIbl() + Registry::init()
//                                        + RenderWorld::init()
//   4. Mech::beginMission()           → (caller)
//
// destroy call order (byte-identical to original):
//   1. endMissionEarly()              → GpuStaticPropBatcher::onMapUnload()
//   2. GpuMechBatcher::onMapUnload()  → (caller, NOT this adapter)
//   3. endMissionLate()               → GpuStaticPropRegistry::destroy()
//                                        + RenderWorld::destroy()
//   4. Mech::endMission()             → (caller)
//
// missionName may be nullptr/empty — resolves to "default" IBL set.
void beginMissionEarly();
void beginMissionLate(const char* missionName);
void endMissionEarly();
void endMissionLate();

// Thin RenderWorld init/destroy seam. PRE-EXISTING public API retained for
// the EDITOR (editor/EditorData.cpp): the editor drives onMapLoad /
// GpuStaticPropRegistry::init / onMapUnload / destroy ITSELF and only needs
// the RenderWorld seam (it has no mission name, so it skips setMissionForIbl).
// The GAME lifecycle uses beginMissionEarly()/beginMissionLate() +
// endMissionEarly()/endMissionLate() instead — do NOT call these two from the
// game path (they would double-init RenderWorld).
void beginMission();   // == RenderWorld::init()
void endMission();     // == RenderWorld::destroy()

// Post-spawn geometry finalisation. Absorbs the direct
// GpuStaticPropBatcher::instance().finalizeGeometry() call previously
// made in code/mission.cpp and code/saveload.cpp.
void finalizeGeometry();

// Per-frame reset of the live-instance list. Absorbs the direct
// GpuStaticPropRegistry::frameBegin() call previously made in
// code/gamecam.cpp.
void frameBegin();

// Number of registered static-prop types post-finalize (== batcher_getTypeCount()).
// Narrow read-only accessor so code/mission.cpp and code/saveload.cpp can size
// gpu_cull::compute_buildIndirectBuffer() without including the batcher header.
uint32_t typeCount();

// Save-game restore path: mirrors the pre-spawn batcher reset from
// Mission::init without the IBL selection or registry init (those are
// already live — destroy() ran endMission() first, which called
// GpuStaticPropRegistry::destroy(); the registry is re-init'd on the
// next real beginMission call). Used by code/saveload.cpp only.
void resetForRestore();

// M1 surface: building / tree first-render and bulk-register sites.
// Returns invalid() on failure (registry disabled, OOM, or batch empty).
//
// Caller MUST persist the returned handle alongside its existing
// staticReg.recipeIndex (M1 keeps the int32_t storage; see Plan
// Decision D4). The legacyRecipeIndexOut OUT parameter exists for the
// transitional period: bdactor.cpp's staticReg.recipeIndex field stays
// int32_t in M1 (slot-side storage; not a public boundary). The adapter
// writes the legacy sentinel value there; the Handle is returned
// for any future upward consumer.
RenderCore::RenderObjectHandle syncStaticProp(
    TG_MultiShape* shape,
    const GpuStaticPropInstance* batchData,
    size_t batchCount,
    int32_t* legacyRecipeIndexOut);  // may be nullptr

// m5 fix: late-spawn entry point (warrior.cpp:7593 path). Uses the
// new GpuStaticPropRegistry::registerStaticPropAndReturnRecipe() to
// obtain the recipe index, then calls
// RenderWorld::adoptStaticPropRecipe() to wrap it in a real Handle
// (so the [RENDER_WORLD v1] counter stays honest).
//
// Returns invalid() on failure. legacyRecipeIndexOut receives the
// raw recipe index (or -1) for any caller that still tracks the
// legacy storage type.
RenderCore::RenderObjectHandle syncStaticPropLateSpawn(
    Appearance* app,
    int32_t* legacyRecipeIndexOut);  // may be nullptr

// Destroy / invalidate. Mirrors GpuStaticPropRegistry::invalidate but
// goes through RenderWorld so counters and sentinel translation stay
// centralized. Currently unused in M1 (the bdactor sites that
// invalidate do so via separate paths; promotion to this API is a
// future-slice cleanup). Declared here so the surface is shape-complete.
void destroyStaticProp(RenderCore::RenderObjectHandle h);

// Destroy by legacy recipe index. Used by BldgAppearance / TreeAppearance
// invalidateStaticRegistration() to tombstone the registry recipe AND
// retire the matching RenderWorld slot in one atomic call.
//
// Without this, invalidateStaticRegistration only tombstoned the recipe
// (count=0) but left the s_objectRecords slot alive=true. On the next
// syncStaticProp the prop re-registered at a new index, leaving a zombie
// slot that caused sp_fail=1 permanently (ok=0) in snapshot extraction.
//
// Constructs a transient handle with generation=1 (sufficient: retireRecord
// and legacy::invalidateStaticProp use h.index() only; generation is not
// inspected by either callee). No-op for recipeIndex < 0.
void destroyStaticPropByIndex(int32_t recipeIndex);

// Inspector bridge: copy shape filename for recipeIndex into out[0..outLen-1].
// out is null-terminated on return. Writes an empty string when recipeIndex
// is invalid, tombstoned, or the registry is disabled.
// recipeIndex comes from LookupResult::gameObjectId (M1.6 sentinel: -1 == invalid).
void getRecipeShapeName(int32_t recipeIndex, char* out, size_t outLen);

// RENDERER-INPUT-READBACK-1: five narrow read-only recipe accessors for the
// `draw_path` / `shadow_feed` / `ground` / `light_row` command-channel verbs.
//
// They exist HERE rather than as a direct
// `#include "gos_static_prop_registry.h"` in code/missiongui.cpp because that
// header drags <GL/glew.h> and Stuff/Stuff.hpp behind it, and because this
// adapter is the sanctioned seam for exactly this traffic (the neighbouring
// getRecipeShapeName is already used by the inspector for the same reason).
// Each is a straight pass-through to the identically-named
// GpuStaticPropRegistry accessor -- no derivation, no caching, no
// reinterpretation. All return false and leave *out untouched when the
// registry is off, recipeIndex < 0, or the recipe is tombstoned; false is
// therefore "cannot answer", never "answer is zero".
//
// WHY THESE FIELDS: they are what the batcher actually replays. The baked
// modelMatrix -- NOT the object's live transform -- is what places a
// registered prop's pixels and what both shadow feeds submit, so a drift
// between it and the live pose is a renderer-input defect that no
// game-state readback could ever see.
bool recipeModelMatrix(int32_t recipeIndex, float out[16]);
bool recipeTypeId(int32_t recipeIndex, uint32_t* out);
bool recipeLightDataIndex(int32_t recipeIndex, uint32_t* out);
bool recipeInstanceFlags(int32_t recipeIndex, uint32_t* out);
bool recipeHadCullRecord(int32_t recipeIndex, bool* out);

// M3-inspector: look up the MaterialGpu sidecar entry for a material handle.
// materialHandleBits is LookupResult::materialHandleBits (0 = sentinel / unknown).
// Returns true and fills *out when MC2_MATERIAL_GPU=1, the handle is valid, and
// the index is in-range. Returns false (out unchanged) otherwise.
bool getMaterialGpuData(uint32_t materialHandleBits, RenderCore::MaterialGpu* out);

// Returns true when the MaterialGpu sidecar table is active (MC2_MATERIAL_GPU=1
// and at least one entry has been uploaded this mission).
bool isMaterialGpuActive();

} // namespace StaticProp
} // namespace GameAdapters
