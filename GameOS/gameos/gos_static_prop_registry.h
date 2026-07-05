#pragma once
#include <cstdint>
#include <vector>
#include "gos_static_prop_batcher.h"

// TG_MultiShape forward-declared via gos_static_prop_batcher.h -> msl.h.
// Appearance forward-declared for registerStaticProp().
class Appearance;

namespace GpuStaticPropRegistry {

// One-time init / teardown (called from code/mission.cpp alongside batcher).
void init();
void destroy();

// Returns true iff MC2_STATIC_PROP_REGISTRY=1 was set at startup.
// Valid before init() because flags are parsed at file scope.
bool isEnabled();

// Returns true iff MC2_STATIC_PROP_MISSION_LOAD_REG=1 was set at startup.
// Gates the mission-load bulk registration walk in GameObjectManager.
bool isMissionLoadRegEnabled();

// Returns true iff MC2_STATIC_PROP_LATE_SPAWN_REG=1 was set at startup.
// Gates the late-spawn per-actor registration calls in spawn sites.
bool isLateSpawnRegEnabled();

// Task 6 (Track B): late-spawn registration API.
// Calls app->registerStatic() under isLateSpawnRegEnabled() guard.
// Returns true iff app->isStaticRegistered() after the call.
// Increments HC-3 counter on failure for gate-signal tracking.
bool registerStaticProp(Appearance* app);

// HC-3 gate signal: count of late-spawn registration attempts where
// isStaticRegistered() returned false (type unknown or ineligible).
// Emitted in destroy() as [STATIC_PROP_REG v1] event=type_unknown_at_late_spawn.
uint64_t getLateSpawnTypeUnknownCount();

// Called once per frame from gamecam.cpp BEFORE land->render().
// Clears the per-frame live-range list.
void frameBegin();

// Called from TreeAppearance::render() after a successful first-time
// submitMultiShape(). Stores multi (for per-frame lightDataIndex patch)
// and snapshots the batch returned by GpuStaticPropBatcher::getLastBuiltBatch().
// Returns recipeIndex (>= 0) on success, -1 if disabled or OOM.
int32_t registerRecipe(TG_MultiShape* multi,
                       const std::vector<GpuStaticPropInstance>& batch);

// Called from TreeAppearance::render() when IsStaticNow() is true (and
// getCachedGpuLightIndex() is valid). Appends regIdx to the per-frame live
// list; flush() does leaf expansion + lightDataIndex patch.
//
// 2026-05-11: lightDataIndex is the slot value captured at this actor's
// update()/touch() time (when the multi's cache was THIS actor's slot
// before sibling actors of the same multi-type overwrote it). flush()
// uses this per-instance value when MC2_STATIC_PER_INSTANCE_LIGHT=1;
// without the flag, flush() ignores it and reads the multi's cache (the
// historical last-writer-wins behavior). Default arg keeps source-compat
// for callers that haven't been updated.
// extentRadius: per-prop world-unit bounding sphere radius from
// bldgShape->GetExtentRadius() / treeShape->GetExtentRadius().
// Written into GpuActorRecord.boundingRadius at flush time so the GPU
// clipSpaceFrustumAdmitSphere test uses the actual prop footprint rather
// than the old hardcoded 200.0f placeholder. Pass 0.0f when unknown;
// flush falls back to 200.0f (preserves pre-fix behavior for unpatched
// callers).
void markVisible(int32_t regIdx, uint32_t lightDataIndex = 0xFFFFFFFFu,
                 float extentRadius = 0.0f);

// STATIC-PROP REGISTRATION CONTRACT (v1). The plain markVisible() above is
// fire-and-forget: it silently returns on a tombstoned/dead range, yet callers
// (bdactor Tree/Bldg render) then set submittedToGpu=true and SUPPRESS the legacy
// draw fallback -> the prop renders nothing (the "trees vanish on replay" class bug).
// markVisibleChecked() is the contract entry point: it returns whether the registry
// ACCEPTED a live recipe this frame. A caller must NOT suppress legacy render unless
// the result is Submitted; on any other result it should invalidate + re-bake (or fall
// back to legacy), turning "GPU cache stale -> invisible" into "GPU cache miss -> legacy
// draws" (the correct, safe failure mode). Generation-handle invalidation is v2.
enum class StaticSubmitResult {
    Submitted,      // range live (registered, count>0, multi!=null) and queued for flush
    NotRegistered,  // registry disabled / regIdx<0
    MissingRange,   // regIdx out of range
    Tombstoned,     // range.count == 0 (invalidated/destroyed but handle still held)
    InvalidRecipe,  // range.multi == null (would be skipped at the flush tombstone guard)
};
StaticSubmitResult markVisibleChecked(int32_t regIdx,
                                      uint32_t lightDataIndex = 0xFFFFFFFFu,
                                      float extentRadius = 0.0f);

// Called when static registration must be cleared (fall, late-reg recovery,
// shape-pointer change, UINT32_MAX light index). Sets recipe range to
// count=0 (tombstone) and NULLs multi; caller must also clear staticReg.
void invalidate(int32_t regIdx);

// Returns true iff regIdx is valid and not invalidated (count > 0).
bool isReady(int32_t regIdx);

// M1.5 C1 fix: typeID -> recipeIndex side-map accessor. Returns -1
// for unknown typeID (or after invalidate()).
int32_t getRecipeIndexForType(uint32_t typeID);

// Inspector accessor: shape filename for a registered recipe.
// Returns nullptr for out-of-range, tombstoned, or null-shapeName entries.
// Pointer is valid until the recipe is invalidated or destroy() is called.
const char* getRecipeShapeName(int32_t recipeIndex);

// Called from txmmgr.cpp BEFORE GpuStaticPropBatcher::instance().flush().
// For each live regIdx: reads multi->getCachedGpuLightIndex() (freshened by
// CacheGpuLightData() in render()), patches lightDataIndex in a stack copy
// of each leaf recipe, injects via submitCachedInstance(). Batcher flush()
// then draws everything in one combined GPU pass.
void flush();

// M1 FROZEN-STATIC-CULL-RECORDS: build + install the frozen static cull-record
// prefix (pool-ordered). Call once per frame AFTER batcher_prepareBaseInstanceTable()
// and BEFORE gpu_cull::compute_dispatch(). No-op on clean frames and unless
// MC2_GPU_CULL_STATIC_FROZEN_RECORDS is set. See gos_static_prop_registry.cpp.
void buildStaticPrefixGolden();

// M2a POPULATION-SPLIT: true when MC2_GPU_CULL_STATIC_FROZEN_RECORDS (G2) is
// armed. Cross-module gate read by the batcher's static population-split path.
bool frozenRecordsArmed();

// [STATIC_FIRST_FRAME v1] proof-of-fix accessor (Track B Task 4).
// Returns the count of registrations whose VERY FIRST flush() attempt was
// rejected by the staleness gate. Must read zero after Task 3's cachedFrame_
// pre-population; non-zero means the pre-population didn't protect the entry.
uint64_t getStaticFirstFrameSkipCount();

// m4 fix (RenderWorld Slice M1): live recipe count for
// [RENDER_WORLD v1] objects=N. Read-only; reflects current tombstone-
// adjusted active recipe slot count.
uint32_t getActiveCount();
uint32_t getRecipeRangeSlotCount();
uint32_t getRecipeRangeSlotCapacity();
uint32_t getRecipeLeafCount();
uint32_t getRecipeLeafCapacity();

// V1A (VisibilityRequest v1A): per-frame visible range count latched at
// flush() entry. Returns 0 before the first flush (mission not yet loaded).
// Used by queryVisibility() for static_props_visible in VisibilityResult.
uint64_t getLastFlushLiveCount();

// m5 fix (RenderWorld Slice M1): late-spawn path needs the recipe
// index back so the adapter can produce a RenderObjectHandle. Returns
// the recipe index that registerStaticProp() created via
// app->registerStatic() + app->getStaticRecipeIndex(), or -1 on
// failure / not eligible. registerStaticProp(Appearance*) above is
// preserved for backward compat but no longer called from the
// worktree once Task 12 migrates warrior.cpp.
int32_t registerStaticPropAndReturnRecipe(Appearance* app);

// --- Extraction v1: per-recipe read-only accessors ---
// All return false if recipeIndex is out of range or the recipe is tombstoned (count==0).

// Copies modelMatrix[16] (row-major) for the first instance of the recipe.
// Translation indices: [3]=east, [7]=elevation, [11]=north (Stuff-space; see
// gos_static_prop_registry.cpp:550-555 for the canonical axis-swap pattern).
bool staticPropGetModelMatrix(int32_t recipeIndex, float out[16]);

// Returns the typeID field from the first instance of the recipe.
bool staticPropGetTypeId(int32_t recipeIndex, uint32_t* out);

// TERRAIN-DECAL-SLICE-0C: overwrite every leaf's modelMatrix (row-major mat4)
// and mark the persistent-static store dirty so the new transform re-uploads next
// flush. Used by the live cliff mesh-decal ImGui tuning panel. Returns false if
// recipeIndex is out of range / tombstoned. See gos_static_prop_registry.cpp.
bool staticPropSetAllLeafMatrices(int32_t recipeIndex, const float in[16]);

// SHADOW-STATIC-BUILDINGS-2: tag a recipe's population (Building/Tree) at
// registration. Lets the world-fixed static shadow map replay buildings only,
// visibility-independent (NOT per-frame markVisible buckets).
void setRecipePopulation(int32_t recipeIndex, GpuStaticPropPopulation pop);
// SHADOW-FOLIAGE: mark a recipe (impostor/far-LOD) to skip dynamic shadow casting.
void setRecipeNoShadow(int32_t recipeIndex, bool noShadow);

// SHADOW-STATIC-BUILDINGS-2: append all non-tombstoned BUILDING recipe leaves
// (baked modelMatrix + typeID) to `out` for the static building shadow pass.
// Reads the full registry, not per-frame buckets. Trees/unset excluded.
void getBuildingShadowInstances(std::vector<GpuStaticPropInstance>& out);

// SHADOW-DYNAMIC-PROP-CASTERS-1: append all non-tombstoned NON-BUILDING recipe
// leaves (trees/fences/generic props; baked modelMatrix + typeID) to `out`.
// Visibility-independent (full registry, NOT per-frame markVisible buckets), so
// the dynamic shadow pass admits EVERY prop, not just the camera-visible subset
// that reached s_typeRanges. By default buildings are EXCLUDED (they cast via the
// world-fixed static map); pass includeBuildings=true to also include them (used
// when the static building map is NOT active, so buildings still cast a dynamic
// shadow). includeBuildings=false = inverse filter of getBuildingShadowInstances.
void getDynamicPropShadowInstances(std::vector<GpuStaticPropInstance>& out,
                                   bool includeBuildings = false);

// Returns RecipeRange::extentRadius. Value is from the previous frame's markVisible()
// call; 0.0f if markVisible was never called for this recipe.
bool staticPropGetExtentRadius(int32_t recipeIndex, float* out);

// Returns RecipeRange::lightDataIndex.
bool staticPropGetLightDataIndex(int32_t recipeIndex, uint32_t* out);

// --- Extraction v1.1: per-typeID primary material cache ---
// Representative/inspector data: primary packet only.
// Populated by GpuStaticPropBatcher::finalizeGeometry().
// Indexed by typeID (dense). Cleared on finalizeGeometry start and registry destroy().

struct StaticPropTypeMaterialCache {
    bool     hasPrimary        = false;
    bool     primaryWasAlphaOn = false;  // true = alpha-on fallback (no alpha-off primary)
    bool     multiPacket       = false;  // type has > 1 packet (informational)
    int32_t  texArrayLayer     = -1;     // sentinel if !hasPrimary
    uint32_t materialIdx       = 0xFFFFFFFFu; // sentinel if !hasMaterialIdx
    bool     hasMaterialIdx    = false;  // true only when MC2_MATERIAL_GPU was on at cache time
    // v2 additions — type-level stable properties (same typeID → same values always)
    uint8_t  alphaClass        = 0;           // s_types[typeID].alphaClass (0=alpha-off, 1=alpha-on)
    uint32_t packetCount       = 0;           // s_types[typeID].packetCount
    uint32_t firstPacket       = 0xFFFFFFFFu; // s_types[typeID].firstPacket; sentinel if !hasPrimary
};

struct MaterialCacheStats {
    uint32_t cacheVectorSize;    // s_typeMatCache.size() -- includes default slots from resize; NOT active-type count
    uint32_t texWired;           // hasPrimary == true
    uint32_t matWired;           // hasMaterialIdx == true
    uint32_t multiPacket;        // multiPacket == true
    uint32_t alphaOnFallback;    // primaryWasAlphaOn == true
    uint32_t noPrimary;          // hasPrimary == false -- INFORMATIONAL ONLY; inflated by sparse typeIDs
};

// Called by GpuStaticPropBatcher::finalizeGeometry() only.
// First-time-wins per typeID with prefer-alpha-off upgrade.
// alphaClass/packetCount/firstPacket: written unconditionally before primary-selection
// logic (idempotent — same type always has same values). Required — no defaults.
void staticPropCacheTypePrimaryMaterial(uint32_t typeID,
                                        int32_t  texArrayLayer,
                                        uint32_t materialIdx,
                                        bool     hasMaterialIdx,
                                        bool     wasAlphaOn,
                                        bool     multiPacket,
                                        uint8_t  alphaClass,
                                        uint32_t packetCount,
                                        uint32_t firstPacket);

// Clears the per-typeID cache. Called from:
//   - start of finalizeGeometry() (re-bake)
//   - registry destroy() (mission unload cleanup)
//   - GpuStaticPropBatcher::onMapUnload()
void staticPropRegistryClearMaterialCache();

// Clears s_recipeHasSubstrateRecord entirely (called from destroy() only).
// Per-frame reset (fill to 0) happens inside flush() — NOT via this function.
// NOT called from finalizeGeometry() or staticPropRegistryClearMaterialCache().
void staticPropRegistryClearCullSubmissionState();

// Returns false + *out=false if recipeIndex is invalid, tombstoned, or out is null.
// *out is true iff substrate_appendStaticPropRecord was called for this recipe
// in the most recent PREVIOUS render frame's flush() pass.
// NOTE: ExtractRenderSnapshot() runs BEFORE flush() — this reflects the prior frame.
bool staticPropGetHasCullRecord(int32_t recipeIndex, bool* out);

// 2b Stage 2: monotonic registry generation (bumped on spawn/despawn/immutable-field
// write). A clean generation across frames means the persistent static store is reusable.
uint64_t getRegistryGeneration();

// STATICPROP-SNAPSHOT-BRIDGE-COMPARE-1: monotonic version of the submitted-prop set.
// Bumped at end of flush() only when s_recipeHasSubstrateRecord differs from the
// previous flush. In a stable scene (same props visible every frame), this is stable
// across frames — the caller can skip re-extraction when this is unchanged.
uint64_t getCullRecordVersion();

// Returns GpuStaticPropInstance.flags for the first leaf of this recipe.
// bit 0: lightsOut, bit 1: isWindow, bit 2: isSpotlight.
// Returns false + *out=0 if recipeIndex invalid, tombstoned, or out is null.
bool staticPropGetInstanceFlags(int32_t recipeIndex, uint32_t* out);

// Returns false + sets *out to sentinel if recipeIndex invalid/tombstoned or !hasPrimary.
bool staticPropGetTexArrayLayer(int32_t recipeIndex, int32_t* out);
bool staticPropGetMaterialIdx(int32_t recipeIndex, uint32_t* out);

// Fills *out with full cache entry for inspector/counter use.
// Returns false if recipeIndex invalid, tombstoned, or !hasPrimary.
bool staticPropGetMaterialCacheInfo(int32_t recipeIndex, StaticPropTypeMaterialCache* out);

// Returns cache stats -- call after finalizeGeometry() for the batcher log.
void staticPropGetMaterialCacheStats(MaterialCacheStats* out);

} // namespace GpuStaticPropRegistry
