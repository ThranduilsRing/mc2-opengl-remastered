#include "gos_static_prop_registry_internal.h"
#include "../../mclib/texture_assets.h"

namespace GpuStaticPropRegistry {
const bool s_trace = mc2::gate::get<"MC2_STATIC_PROP_TRACE">();
// Task 9 (Track B): mission-load + late-spawn registration ON by default.
// Soak started 2026-05-06; all Task 8 parity gates passed.
// Set MC2_STATIC_PROP_MISSION_LOAD_REG=0 to opt out.
static const bool s_missionLoadRegEnabled =
    mc2::gate::get<"MC2_STATIC_PROP_MISSION_LOAD_REG">();
// Set MC2_STATIC_PROP_LATE_SPAWN_REG=1 to opt in (default OFF as of 2026-05-20).
//
// Previously default-on (Task 9 Track B, 2026-05-06). The sole caller is
// MechWarrior::getWayPointMarker (code/warrior.cpp:7593), which registers
// tactical-order waypoint markers ("WalkWayPoint"/"RunWayPoint"/"JumpWayPoint")
// into the static-prop batcher so they GPU-batch alongside building props.
//
// 2026-05-20: User-driven savegame-restore canary on mc2_10 (post-commit
// 4008185) surfaced that these late-registered markers render as a
// persistent black octagon at the LZ. Two contributing factors:
//   1. MechWarrior::copyFromData (warrior.cpp:8345) iterates the restored
//      tacOrderQueue and re-creates a marker for every saved slot — including
//      slots whose orders had completed pre-save but whose .point was never
//      cleared, planting a marker at stale coordinates.
//   2. Late-registered instances never get their per-instance lightDataIndex
//      patched by flush(); calc_light() returns (0,0,0); v_argb is black.
//
// Project does not use waypoint markers as a gameplay feature, so disabling
// late-spawn registration unconditionally fixes the visible artifact with
// no behavior loss. Operator can opt back in (MC2_STATIC_PROP_LATE_SPAWN_REG=1)
// if revisiting Task 6/9 Track B's lighting-patch integration.
static const bool s_lateSpawnRegEnabled =
    mc2::gate::get<"MC2_STATIC_PROP_LATE_SPAWN_REG">();

// 2026-05-11 per-instance light-idx capture. flush() consumes the value
// stored in RecipeRange.lightDataIndex by markVisible() when this is on;
// otherwise flush() reads the multi's shared cachedGpuLightIndex_ (the
// historical last-writer-wins behavior — buggy under MC2_STATIC_UPDATE_SKIP=1
// because multiple actors of the same multi-type write to the same per-multi
// scratch slot).
//
// 2026-05-11 default-on after user soak: interactive mc2_01 run with
// MC2_STATIC_UPDATE_SKIP=1 confirmed widespread wrong-RGB symptom retired,
// 1 residual instance correlated with substrate-cap edge case (separate
// follow-up). Set MC2_STATIC_PER_INSTANCE_LIGHT=0 to opt back into the
// historical multi-cache path.
const bool s_perInstanceLight =
    mc2::gate::get<"MC2_STATIC_PER_INSTANCE_LIGHT">();

// STATICPROP-PERSISTENT-STATIC-BUCKETS-2B-STAGE2: the static per-type instance block lives in a persistent batcher
// store, rebuilt only when the registry generation changes, and the per-frame static instance re-push is skipped
// (StaticPropRegistryFlush 312us -> 68us at wolfman). Recipes whose light row matches their stamped value take the
// cached-record branch below.

// 2b Stage 2 dirty signal: monotonic generation bumped on every structural change
// to the static-prop registry (spawn/despawn/immutable-field write). A clean
// generation across frames means the persistent static store is reusable.
static uint64_t s_registryGeneration = 0;

// STATICPROP-SNAPSHOT-BRIDGE-COMPARE-1: monotonic version of the submitted-prop set.
// Bumped at end of flush() when s_recipeHasSubstrateRecord differs from the previous
// flush. In a stable scene (same props submitted each frame), this stays stable.
// Never reset — stays monotonic across missions so callers can use it as a change
// signal even across level boundaries.
uint64_t              s_cullRecordVersion = 0;
std::vector<uint8_t>  s_prevCullRecord;  // copy of s_recipeHasSubstrateRecord from last flush


// MC2_TEX_LIFECYCLE_TRACE=1 — same env flag used by mclib/txmmgr.cpp,
// mclib/msl.cpp, GameOS/gameos/gos_static_prop_batcher.cpp so all
// [TEX_LIFECYCLE v1] events stream together under one invocation.
// See docs/superpowers/specs/2026-05-06-static-prop-texture-pin-fix.md
static const bool s_texPinTrace =
    mc2::gate::get<"MC2_TEX_LIFECYCLE_TRACE">();
#define TEX_LC_PIN(fmt, ...)                                            \
    do { if (s_texPinTrace) {                                           \
        printf("[TEX_LIFECYCLE v1] " fmt "\n", ##__VA_ARGS__);          \
        fflush(stdout);                                                 \
    } } while (0)

}  // namespace GpuStaticPropRegistry

namespace GpuStaticPropRegistry {


std::vector<GpuStaticPropInstance> s_recipes;
std::vector<RecipeRange>           s_recipeRanges;

// COUNTER-HONESTY-1: bit p set once setRecipePopulation() has ever been called
// with population p in this process. This is what lets a population census
// distinguish "zero recipes of this kind exist" from "nothing ever tags this
// kind, so the zero is meaningless".
//
// It is load-bearing right now: GenericAppearance has no registerStatic() and
// never calls syncStaticProp(), so no Generic recipe is ever minted and
// reg_recipes_generics is structurally 0 -- exactly the blind-counter shape this
// lane exists to kill. Rather than let that zero look like a measurement, the
// summary prints which populations are instrumented at all, and the mask
// maintains itself when a Generic mint path is added later (no doc to go stale).
//
// Process-lifetime, deliberately NOT cleared in init(): the recipe COUNTS reset
// per mission, but "is this population instrumented in this build" does not.
static uint8_t s_popsEverTagged = 0u;

// RECIPE-SLOT-RECYCLE (MC2_STATIC_RECIPE_RECYCLE, default ON -- kill-switch =0;
// reader at :318 is `!(v && v[0]=='0')`. Said "default OFF" until
// ENV-DROP-CLOSE-1, 2026-07-30). The registry
// historically only APPENDED: registerRecipe() pushed a new s_recipeRanges slot
// and invalidate() merely tombstoned (count=0), never reclaiming the index. Under
// per-frame invalidate+re-register churn (LOD swap, damage, and force-dynamic props
// which default ON for NVIDIA) regIdx therefore climbs without bound. Because the
// baked static light table is indexed BY regIdx (txmmgr.cpp bakeStaticLightSlot ->
// s_staticLightHighWater = max(regIdx)+1), an unbounded regIdx makes the per-frame
// light SSBO upload [0..S) and the addLightDataStructure base scan grow without
// bound (observed 80ms -> 500ms+ on NVIDIA). Recycling tombstoned regIdx slots caps
// max(regIdx) at the live-recipe count, which caps S, which caps both costs.
// DEFAULT ON (confirmed on the 1050 Ti to flatten the LightDataUpload climb and the
// addLightDataStructure scan growth); set MC2_STATIC_RECIPE_RECYCLE=0 to disable.
// Leaf storage (s_recipes) still appends -- that is a slower RAM-only growth, not the
// per-frame cost this targets.
static std::vector<uint32_t>              s_recipeRangeFreeList;
static const bool s_recipeRecycle = mc2::gate::get<"MC2_STATIC_RECIPE_RECYCLE">();

// 2A: per-recipe cached immutable cull record (one per s_recipes[] entry).
// Built lazily on first flush of a recipe; invalidated on any immutable-field write.
// Sized in lockstep with s_recipes (resize in registerRecipe, new entries valid=0).
std::vector<gpu_cull::GpuActorRecord> s_cachedActorRecord;
std::vector<uint8_t>                  s_cachedActorRecordValid; // 0/1 per recipe

// M1 FROZEN-STATIC-CULL-RECORDS (gate MC2_GPU_CULL_STATIC_FROZEN_RECORDS).
// On a dirty store rebuild, capture each recipe's type-local STORE RANK (its slot
// within s_persistentStaticStore[typeID], == its instance-pool slot within the
// type). gpu_cull_buildStaticPrefixGolden() (called after
// batcher_prepareBaseInstanceTable) then places each cull record at
// baseInstanceForType[typeID] + storeRank == the exact binding-0 slot the draw
// indexes, so record-index == instance-pool-slot by construction. All gated;
// gate-off leaves the per-frame substrate_appendStaticPropRecord path untouched.
const bool s_staticFrozenReg = mc2::gate::get<"MC2_GPU_CULL_STATIC_FROZEN_RECORDS">();
std::vector<uint32_t> s_recipeToStoreSlot;   // [ri] -> type-local store rank (dirty-built)
std::vector<uint32_t> s_goldenRecipeList;    // recipes placed this dirty, in store order
std::vector<uint32_t> s_pbStoreCursor;       // per-type store cursor (reset/grown at dirty)
bool                  s_goldenDirty = false; // a dirty rebuild happened; golden needs rebuild

// v1.1: per-typeID primary material cache. Populated by finalizeGeometry().
// Indexed by typeID (dense); resized as needed by staticPropCacheTypePrimaryMaterial.
static std::vector<GpuStaticPropRegistry::StaticPropTypeMaterialCache> s_typeMatCache;

// v2: tracks whether flush() called substrate_appendStaticPropRecord for each recipe.
// Indexed by recipeIndex (parallel to s_recipeRanges). Reset to 0 at flush() start;
// set to 1 after substrate_appendStaticPropRecord in the flush loop.
// Cleared entirely in destroy() (see the ClearCullSubmissionState helper below).
// Do NOT clear in staticPropRegistryClearMaterialCache() — different lifecycle.
std::vector<uint8_t> s_recipeHasSubstrateRecord;

// M1.5 C1 fix: typeID -> recipeIndex side-map. Populated by
// registerRecipe(); set to -1 on invalidate(). Lookup returns -1
// if typeID is unknown. Last-write-wins: if two recipes register
// with the same typeID (unusual but legal), the second overrides;
// the first becomes unreachable via this map but remains addressable
// via its returned recipeIndex.
static std::unordered_map<uint32_t, int32_t> s_typeIDToRecipeIndex;

// Per-frame list of regIdx values (one per visible tree).
// markVisible() appends one regIdx per tree; flush() expands to leaves
// and patches lightDataIndex from the live TG_MultiShape.
std::vector<uint32_t>              s_liveRangeIndices;

// V1A: per-frame visible range count latched at flush() entry (before
// expansion and before frameBegin() clears the vector next frame).
// queryVisibility() reads this for static_props_visible.
static uint64_t                           s_lastFlushLiveCount = 0;

// Pin-call accounting for the [TEX_LIFECYCLE v1] event=pin_summary line
// emitted in destroy(). leakedPins = totalPinCalls - totalUnpinCalls;
// non-zero is a refcount imbalance bug. Reset to 0 in destroy() after
// summary emit so per-mission accounting is clean across load/unload.
static uint64_t s_totalPinCalls      = 0;
static uint64_t s_totalUnpinCalls    = 0;
// [STATIC_FIRST_FRAME v1]: counts entries whose very first flush() attempt was
// rejected by the staleness gate. Must read zero after Task 3's cachedFrame_
// pre-population; non-zero means the pre-population didn't reach flush in time.
uint64_t s_firstFrameSkipCount = 0;
// [STATIC_PROP_REG v1] HC-3 gate signal: counts late-spawn registration
// attempts where isStaticRegistered() returned false (type unknown or
// ineligible). Emitted in destroy() for per-mission accounting.
static uint64_t s_lateSpawnTypeUnknownCount = 0;

// patch 3/4: any write to a recipe's immutable fields (modelMatrix, typeID,
// lightDataIndex, bounds/extent, category/block inputs) MUST call this so the
// next flush rebuilds the cached record from the new values.
static void invalidateCachedFlushRecord(uint32_t recipeIndex) {
    if (recipeIndex < s_cachedActorRecordValid.size())
        s_cachedActorRecordValid[recipeIndex] = 0u;
}

// Builds the immutable cull record for ONE recipe leaf (recipeIdx into s_recipes[]).
// worldCenter uses the range's actor-root translation (actor-center fix: the root
// leaf's modelMatrix translation is shared across all leaves of the range).
// category uses THIS recipe's typeID (per-leaf — a multi-leaf range has different
// typeIDs per leaf, so per-range sharing is incorrect for category scatter).
// Pure function of immutable recipe + range data; produces byte-identical output
// to the inline per-leaf build in flush() for the same recipe+range pair.
// Shared with the flush phase helpers (gos_static_prop_registry_internal.h).
void buildCachedActorRecord(const RecipeRange& rng, uint32_t recipeIdx,
                            gpu_cull::GpuActorRecord& out) {
    out = gpu_cull::GpuActorRecord{};
    // World position: axis-swap from Stuff/MLR frame to raw MC2 world coords.
    // Mirrors the inline actor-center fix at flush():~712-717.
    const float* rootMtx = s_recipes[rng.first].modelMatrix;
    const float actorWorldCenter[3] = {
        -rootMtx[3],   // raw.x = -stuff.x
         rootMtx[11],  // raw.y =  stuff.z
         rootMtx[7],   // raw.z =  stuff.y (elev)
    };
    out.worldCenter[0] = actorWorldCenter[0];
    out.worldCenter[1] = actorWorldCenter[1];
    out.worldCenter[2] = actorWorldCenter[2];
    // Bounding radius from per-prop extent radius; fallback to 200.0f for
    // unpatched callers or missing shape pointer (preserves pre-fix behavior).
    out.boundingRadius = (rng.extentRadius > 0.0f) ? rng.extentRadius : 200.0f;
    out.worldAabbMin[0] = out.worldCenter[0] - out.boundingRadius;
    out.worldAabbMin[1] = out.worldCenter[1] - out.boundingRadius;
    out.worldAabbMin[2] = out.worldCenter[2] - out.boundingRadius;
    out.worldAabbMax[0] = out.worldCenter[0] + out.boundingRadius;
    out.worldAabbMax[1] = out.worldCenter[1] + out.boundingRadius;
    out.worldAabbMax[2] = out.worldCenter[2] + out.boundingRadius;
    // Category: this recipe's typeID (per-leaf) in upper 28 bits + Cat_StaticProp in lower 4.
    out.category = (static_cast<uint32_t>(s_recipes[recipeIdx].typeID) << 4)
                 | static_cast<uint32_t>(gpu_cull::Cat_StaticProp);
    // Diagnostic force-admit flag (mirrors the flush() inline s_diag_forceAdmit check).
    static const bool s_diag_forceAdmit = mc2::gate::get<"MC2_STATIC_FORCE_ADMIT">();
    out.flags          = s_diag_forceAdmit
                           ? static_cast<uint32_t>(gpu_cull::Flag_AlwaysVisible)
                           : gpu_cull::Flag_None;
    out.actorId        = 0u;   // static props have no actor handle
    out.prevVisibilityBit = 1u; // CPU admitted this prop this frame
    out.consumerFlags  = 0u;
    // Terrain block index: feed raw-MC2 east [0] and north [1] only (never elevation [2]).
    out.blockIdx = static_cast<uint32_t>(
        Terrain::worldToBlockIdx(out.worldCenter[0], out.worldCenter[1]));
}

// Release every pin held by a single RecipeRange. Idempotent via
// rng.pinsReleased — invalidate() may run before destroy() does its
// safety-net sweep, and we don't want to unpinNode the same node twice.
// No shrink_to_fit() — parent s_recipeRanges is also cleared on destroy(),
// and the per-range vector destructor handles deallocation.
static void releasePinsForRange(RecipeRange& rng) {
    if (rng.pinsReleased) return;
    if (mc2::textures::ready()) {
        for (DWORD nodeIdx : rng.pinnedTextureNodes) {
            mc2::textures::unpin(nodeIdx);
            ++s_totalUnpinCalls;
            TEX_LC_PIN("event=unpin nodeIdx=%lu refcount=%lu",
                       static_cast<unsigned long>(nodeIdx),
                       static_cast<unsigned long>(mc2::textures::pinCount(nodeIdx)));
        }
    }
    rng.pinnedTextureNodes.clear();
    rng.pinsReleased = true;
}

} // namespace GpuStaticPropRegistry

// [LIGHTBAKE v1] free fns defined in mclib/txmmgr.cpp. Declared at FILE
// scope (NOT inside namespace GpuStaticPropRegistry, else the linker
// looks for GpuStaticPropRegistry::mc2... -> LNK2019).
extern void mc2EraseBakedStaticLight(int32_t);
extern void mc2ClearAllBakedStaticLight();


namespace GpuStaticPropRegistry {

bool isMissionLoadRegEnabled() { return s_missionLoadRegEnabled; }
bool isLateSpawnRegEnabled()   { return s_lateSpawnRegEnabled; }

uint64_t getStaticFirstFrameSkipCount()    { return s_firstFrameSkipCount; }
uint64_t getLateSpawnTypeUnknownCount()    { return s_lateSpawnTypeUnknownCount; }

bool registerStaticProp(Appearance* app) {
    if (!isLateSpawnRegEnabled()) return false;
    if (!app) return false;
    app->registerStatic();
    const bool ok = app->isStaticRegistered();
    if (!ok) {
        ++s_lateSpawnTypeUnknownCount;
    }
    return ok;
}

int32_t registerStaticPropAndReturnRecipe(Appearance* app) {
    if (!isLateSpawnRegEnabled()) return -1;
    if (!app) return -1;
    app->registerStatic();
    if (!app->isStaticRegistered()) {
        ++s_lateSpawnTypeUnknownCount;
        return -1;
    }
    const int32_t recipeIdx = app->getStaticRecipeIndex();
    // Capture appearance name for the inspector (AppearanceType::name always
    // present, not debug-gated). Bulk registerRecipe callers have no name.
    if (recipeIdx >= 0 && static_cast<size_t>(recipeIdx) < s_recipeRanges.size()) {
        AppearanceTypePtr at = app->getAppearanceType();
        if (at && at->name) {
            std::strncpy(s_recipeRanges[static_cast<size_t>(recipeIdx)].shapeName,
                         at->name, 127);
            s_recipeRanges[static_cast<size_t>(recipeIdx)].shapeName[127] = '\0';
        }
    }
    return recipeIdx;
}

uint32_t getActiveCount() {
    // Live recipe count = ranges with count > 0 (non-tombstoned).
    uint32_t n = 0;
    for (const auto& rng : s_recipeRanges) {
        if (rng.count > 0) ++n;
    }
    return n;
}

uint32_t getRecipeRangeSlotCount() {
    return static_cast<uint32_t>(s_recipeRanges.size());
}

uint32_t getRecipeRangeSlotCapacity() {
    return static_cast<uint32_t>(s_recipeRanges.capacity());
}

uint32_t getRecipeLeafCount() {
    return static_cast<uint32_t>(s_recipes.size());
}

uint32_t getRecipeLeafCapacity() {
    return static_cast<uint32_t>(s_recipes.capacity());
}

uint64_t getLastFlushLiveCount() {
    // V1A: per-frame visible range count latched at flush() entry.
    // Returns 0 before the first flush (mission not yet loaded).
    return s_lastFlushLiveCount;
}

// TERRAIN-DECAL-SLICE-0C: clear any captured live cliff-decal context at mission
// (re)init so a recycled recipe slot from a prior mission can't be live-written by
// the ImGui panel. Defined in mclib/cliff_decal_tuning.cpp; resolved at mc2 link.
extern "C" void CliffDecalTuning_clearOnMissionReset();

void init() {
    // TERRAIN-DECAL-SLICE-0C: new mission -> drop stale decal capture (re-captured
    // by BldgAppearance::registerStatic if this mission places a MarbleCliff).
    CliffDecalTuning_clearOnMissionReset();
    // init() reserves memory.
    s_recipes.reserve(20000);
    s_recipeRanges.reserve(15000);
    s_liveRangeIndices.reserve(15000);
    printf("[STATIC_PROP] registry init: memory reserved\n");
    fflush(stdout);
}

void destroy() {
    // [STATIC_FIRST_FRAME v1] summary — emit BEFORE pin-release loop.
    // Non-zero skip_count means Task 3's cachedFrame_ pre-population didn't
    // reach flush in time for at least one registration; escalate if nonzero.
    MC2_DIAGFE(stderr,
        "[STATIC_FIRST_FRAME v1] event=summary skip_count=%llu\n",
        static_cast<unsigned long long>(s_firstFrameSkipCount));
    fflush(stderr);
    s_firstFrameSkipCount = 0;

    // [STATIC_PROP_REG v1] HC-3 gate signal: late-spawn registration failures.
    // count=0 means every late-spawn actor was eligible and registered.
    // count>0 identifies types that fell through to the first-render path.
    MC2_DIAGFE(stderr,
        "[STATIC_PROP_REG v1] event=type_unknown_at_late_spawn count=%llu\n",
        static_cast<unsigned long long>(s_lateSpawnTypeUnknownCount));
    fflush(stderr);
    s_lateSpawnTypeUnknownCount = 0;

    // Texture-pin spec: release any unreleased pins (mission-teardown
    // safety net — covers ranges that were never explicitly invalidated).
    for (auto& rng : s_recipeRanges) {
        releasePinsForRange(rng);
    }

    // Texture-pin spec: pin-call accounting summary.  leakedPins != 0 is a
    // bug signal (refcount imbalance between registerRecipe and invalidate).
    if (s_texPinTrace) {
        printf("[TEX_LIFECYCLE v1] event=pin_summary mission_end "
               "totalPinCalls=%llu totalUnpinCalls=%llu leakedPins=%lld\n",
               static_cast<unsigned long long>(s_totalPinCalls),
               static_cast<unsigned long long>(s_totalUnpinCalls),
               static_cast<long long>((static_cast<int64_t>(s_totalPinCalls) - static_cast<int64_t>(s_totalUnpinCalls))));
        fflush(stdout);
    }
    s_totalPinCalls   = 0;
    s_totalUnpinCalls = 0;

    s_recipes.clear();          s_recipes.shrink_to_fit();
    s_recipeRanges.clear();     s_recipeRanges.shrink_to_fit();
    // RECIPE-SLOT-RECYCLE: the free-list holds indices into s_recipeRanges; it MUST
    // be emptied in lockstep or a stale index would alias / go out of bounds next mission.
    s_recipeRangeFreeList.clear(); s_recipeRangeFreeList.shrink_to_fit();
    // 2A: clear the per-recipe cached actor-record arrays (parallel to s_recipes).
    s_cachedActorRecord.clear();      s_cachedActorRecord.shrink_to_fit();
    s_cachedActorRecordValid.clear(); s_cachedActorRecordValid.shrink_to_fit();
    s_liveRangeIndices.clear(); s_liveRangeIndices.shrink_to_fit();
    s_typeMatCache.clear();          s_typeMatCache.shrink_to_fit();
    staticPropRegistryClearCullSubmissionState();
    // unordered_map has no shrink_to_fit(); swap with empty to release bucket
    // allocation back to the heap, matching the intent of the surrounding
    // vector .clear()+.shrink_to_fit() pattern. Mission 2 may register a
    // completely different set of type IDs, so retaining mission 1's bucket
    // layout would just waste memory.
    s_typeIDToRecipeIndex.clear();
    std::unordered_map<uint32_t, int32_t>().swap(s_typeIDToRecipeIndex);
    // [LIGHTBAKE v1] recipeIndex restarts next mission -> stale baked
    // entries would alias a different actor. Drop the mission-scoped map.
    ::mc2ClearAllBakedStaticLight();
}

void frameBegin() {
    s_liveRangeIndices.clear();
}

int32_t registerRecipe(TG_MultiShape* multi,
                       const std::vector<GpuStaticPropInstance>& batch) {
    if (batch.empty() || !multi || !multi->usesTypeMaterials()) return -1;
    RecipeRange rng;
    rng.first = static_cast<uint32_t>(s_recipes.size());
    rng.count = static_cast<uint32_t>(batch.size());
    rng.multi = multi;
    rng.pinsReleased      = false;
    rng.registeredOnFrame = g_mc2FrameCounter;
    rng.firstFlushSeen    = false;
    rng.lightDataIndex    = 0xFFFFFFFFu;  // 2026-05-11 per-instance capture sentinel
    rng.extentRadius      = 0.0f;
    rng.shapeName[0]      = '\0';         // populated by late-spawn path if Appearance* available
    rng.population        = 0xFFu;        // SHADOW-STATIC-BUILDINGS-2: unset until setRecipePopulation()
    rng.noShadow          = false;        // SHADOW-FOLIAGE: casts unless setRecipeNoShadow(true)
    s_recipes.insert(s_recipes.end(), batch.begin(), batch.end());
    // 2A: keep cached-actor-record arrays in lockstep with s_recipes.
    // New entries are valid=0 (uncached); they will be lazily built on first flush (Task 4).
    s_cachedActorRecord.resize(s_recipes.size());
    s_cachedActorRecordValid.resize(s_recipes.size(), 0u);
    s_recipeToStoreSlot.resize(s_recipes.size(), 0u);  // M1: lockstep with s_recipes
    ++s_registryGeneration;   // 2b Stage 2: spawn = structural change
    // RECIPE-SLOT-RECYCLE: reuse a tombstoned regIdx when available (caps max regIdx
    // -> caps the baked-light high-water S -> caps the per-frame light upload/scan).
    // The reused slot's parallel arrays are reset; leaf storage still appends (rng.first
    // was set from s_recipes.size() above, and the leaves were inserted at the end).
    int32_t regIdx;
    if (s_recipeRecycle && !s_recipeRangeFreeList.empty()) {
        regIdx = static_cast<int32_t>(s_recipeRangeFreeList.back());
        s_recipeRangeFreeList.pop_back();
        s_recipeRanges[static_cast<size_t>(regIdx)] = rng;          // overwrite tombstone
        s_recipeHasSubstrateRecord[static_cast<size_t>(regIdx)] = 0u;
    } else {
        regIdx = static_cast<int32_t>(s_recipeRanges.size());
        s_recipeRanges.push_back(rng);
        s_recipeHasSubstrateRecord.push_back(0u); // v2: one slot per recipe, parallel to s_recipeRanges
    }
    // M1.5 C1: maintain typeID -> recipeIndex side-map for the
    // batcher's objectIdRaw producer. All instances in `batch` share
    // the same typeID in practice (one recipe = one multi-shape =
    // one type); take the first.
    if (!batch.empty()) {
        s_typeIDToRecipeIndex[batch[0].typeID] = regIdx;
    }
    SP_TRACE("register regIdx=%d first=%u count=%u", regIdx, rng.first, rng.count);

    // Pin every mcTextureNodeIndex referenced by this recipe's multi-shape.
    // mc2::textures::ready() evicts textures during gameplay (txmmgr.cpp:update);
    // for actors using touch() instead of update() under MC2_STATIC_UPDATE_SKIP=1
    // there's no re-cache pathway, so the leaf TG_TypeShape::listOfTextures
    // gosTextureHandle goes stale and the batcher renders black quads.
    // Pinning the master node prevents eviction while this range is registered.
    // Use the public TG_MultiShape::GetNumTextures()/GetTextureHandle(j) API
    // (msl.h:454-470) — GetTextureHandle(j) returns mcTextureNodeIndex directly.
    // Do not access TG_TypeMultiShape::numTextures/listOfTextures — protected.
    if (mc2::textures::ready()) {
        // RECIPE-SLOT-RECYCLE: index by regIdx, NOT .back() -- a recycled slot is
        // not at the end of s_recipeRanges.
        RecipeRange& storedRng = s_recipeRanges[static_cast<size_t>(regIdx)];
        const long numTex = multi->GetNumTextures();
        for (long j = 0; j < numTex; ++j) {
            const DWORD nodeIdx = multi->GetTextureHandle(j);
            if (nodeIdx != 0xffffffff) {
                mc2::textures::pin(nodeIdx);
                storedRng.pinnedTextureNodes.push_back(nodeIdx);
                ++s_totalPinCalls;
                TEX_LC_PIN("event=pin nodeIdx=%lu refcount=%lu regIdx=%d multi=%p",
                           static_cast<unsigned long>(nodeIdx),
                           static_cast<unsigned long>(mc2::textures::pinCount(nodeIdx)),
                           regIdx, static_cast<void*>(multi));
            }
        }
    }
    // Track B: structural first-frame fix. Pre-populate cachedFrame_ so the
    // first flush() after registration passes the staleness gate at flush()
    // without requiring a prior CacheGpuLightData() call.
    multi->setCachedFrame(g_mc2FrameCounter);
    return regIdx;
}

void markVisible(int32_t regIdx, uint32_t lightDataIndex, float extentRadius) {
    if (regIdx < 0 || static_cast<uint32_t>(regIdx) >= s_recipeRanges.size()) return;
    RecipeRange& rng = s_recipeRanges[static_cast<uint32_t>(regIdx)];
    if (rng.count == 0) return; // tombstone
    // 2026-05-11: capture per-actor lightDataIndex (the multi's cachedGpuLightIndex_
    // at the moment THIS actor's update/touch wrote it, before sibling actors of
    // the same multi-type overwrote it). flush() consumes this when
    // MC2_STATIC_PER_INSTANCE_LIGHT=1 is set; otherwise flush ignores it.
    // 2A FIX (compare-oracle caught divergence): the permanent per-leaf light is
    // the RANGE's lightDataIndex for ALL leaves (matching the legacy flush, which
    // gives every leaf of a range the single freshLightIdx == rng.lightDataIndex)
    // — NOT each leaf's own recipeIndex. A multi-leaf range (count>1) must stamp
    // the same range light onto s_recipes[first..first+count). Also DIRTY-GATE the
    // propagation + cached-record invalidation on actual change: markVisible runs
    // per-frame per visible actor, but lightDataIndex/extentRadius are stable, so
    // re-stamping every frame would defeat the cache (records would rebuild every
    // frame). Only touch on change.
    const bool lightOrExtentChanged =
        (rng.lightDataIndex != lightDataIndex) || (rng.extentRadius != extentRadius);
    rng.lightDataIndex = lightDataIndex;
    // 2026-05-22 F4 T3: store per-prop extent radius for GPU cull record.
    rng.extentRadius = extentRadius;
    if (lightOrExtentChanged) {
        for (uint32_t k = rng.first; k < rng.first + rng.count; ++k) {
            s_recipes[k].lightDataIndex = lightDataIndex;  // range light → every leaf
            invalidateCachedFlushRecord(k);                // rebuild cached record next flush
        }
        ++s_registryGeneration;   // 2b Stage 2: immutable-field (light/extent) write
    }
    s_liveRangeIndices.push_back(static_cast<uint32_t>(regIdx));
}

// STATIC-PROP REGISTRATION CONTRACT (v1). Validate the registration BEFORE accepting
// visibility, and report the outcome so the caller can fall back instead of suppressing
// the legacy draw on a dead handle. Checks mirror the flush tombstone guard
// (gos_static_prop_registry.cpp: "if (rng.count == 0 || !rng.multi) ... continue") so a
// "Submitted" result GUARANTEES flush will not skip the range for those reasons.
StaticSubmitResult markVisibleChecked(int32_t regIdx, uint32_t lightDataIndex,
                                      float extentRadius) {
    if (regIdx < 0 || static_cast<uint32_t>(regIdx) >= s_recipeRanges.size())
        return StaticSubmitResult::MissingRange;
    const RecipeRange& rng = s_recipeRanges[static_cast<uint32_t>(regIdx)];
    if (rng.count == 0)  return StaticSubmitResult::Tombstoned;   // dead handle
    if (!rng.multi)      return StaticSubmitResult::InvalidRecipe; // flush would skip it
    if (!rng.multi->usesTypeMaterials()) return StaticSubmitResult::InvalidRecipe;
    markVisible(regIdx, lightDataIndex, extentRadius);            // accept (queues for flush)
    return StaticSubmitResult::Submitted;
}

void invalidate(int32_t regIdx) {
    if (regIdx < 0 || static_cast<uint32_t>(regIdx) >= s_recipeRanges.size()) return;
    RecipeRange& rng = s_recipeRanges[static_cast<uint32_t>(regIdx)];
    // RECIPE-SLOT-RECYCLE: only reclaim a slot that was actually LIVE this call.
    // invalidate() is idempotent (may be called on an already-tombstoned slot);
    // pushing an already-freed regIdx would double-list it -> double reuse -> two
    // recipes aliasing one slot. Capture liveness BEFORE the tombstone below.
    const bool wasLive = (rng.count > 0);
    releasePinsForRange(rng);
    // M1.5 C1: tombstone the side-map entry BEFORE zeroing s_recipes
    // (typeID would otherwise be zeroed out by the loop below). Only
    // tombstone if our regIdx still owns the mapping; a newer recipe
    // with the same typeID may have taken over.
    if (rng.count > 0 && rng.first < s_recipes.size()) {
        const uint32_t typeID = s_recipes[rng.first].typeID;
        auto it = s_typeIDToRecipeIndex.find(typeID);
        if (it != s_typeIDToRecipeIndex.end() && it->second == regIdx) {
            it->second = -1;
        }
    }
    for (uint32_t i = 0; i < rng.count; ++i) {
        s_recipes[rng.first + i] = GpuStaticPropInstance{};
        // 2A patch 3: this recipe leaf's immutable fields (modelMatrix, typeID) are
        // being zeroed — invalidate the cached cull record so Task 4 rebuilds it if
        // this slot is ever reused after a re-registration.
        invalidateCachedFlushRecord(rng.first + i);
    }
    SP_TRACE("invalidate regIdx=%d (was count=%u)", regIdx, rng.count);
    rng.count = 0;
    // v2: zero-out substrate tracking (don't erase — keeps index stable).
    if (static_cast<size_t>(regIdx) < s_recipeHasSubstrateRecord.size())
        s_recipeHasSubstrateRecord[static_cast<size_t>(regIdx)] = 0u;
    rng.multi  = nullptr;
    rng.lightDataIndex = 0xFFFFFFFFu;  // 2026-05-11 reset capture on invalidate
    rng.extentRadius   = 0.0f;         // 2026-05-22 F4 T3 reset extent radius
    // [LIGHTBAKE v1] drop the baked static-light entry so destruction/LOD
    // multi-swap lazily re-bakes the same position-derived constant.
    ::mc2EraseBakedStaticLight(regIdx);
    ++s_registryGeneration;   // 2b Stage 2: despawn = structural change
    // RECIPE-SLOT-RECYCLE: return the now-tombstoned slot to the free-list so the
    // next registerRecipe() reuses this regIdx instead of growing the array.
    if (s_recipeRecycle && wasLive)
        s_recipeRangeFreeList.push_back(static_cast<uint32_t>(regIdx));
}

// 2b Stage 2: monotonic dirty signal. A clean generation across frames means the
// persistent static instance store (batcher) is reusable; the per-frame static
// re-push can be skipped. Bumped on spawn (registerRecipe), despawn (invalidate),
// and immutable-field write (markVisible light/extent change).
uint64_t getRegistryGeneration() { return s_registryGeneration; }

// STATICPROP-SNAPSHOT-BRIDGE-COMPARE-1: submitted-prop set version.
uint64_t getCullRecordVersion() { return s_cullRecordVersion; }

bool isReady(int32_t regIdx) {
    if (regIdx < 0 || static_cast<uint32_t>(regIdx) >= s_recipeRanges.size()) return false;
    return s_recipeRanges[static_cast<uint32_t>(regIdx)].count > 0;
}

// M1.5 C1 fix: typeID -> recipeIndex side-map accessor. Returns -1
// for unknown typeID (or after invalidate()).
int32_t getRecipeIndexForType(uint32_t typeID) {
    auto it = s_typeIDToRecipeIndex.find(typeID);
    if (it == s_typeIDToRecipeIndex.end()) return -1;
    return it->second;
}

const char* getRecipeShapeName(int32_t recipeIndex) {
    if (recipeIndex < 0 || static_cast<size_t>(recipeIndex) >= s_recipeRanges.size())
        return nullptr;
    const RecipeRange& rng = s_recipeRanges[static_cast<size_t>(recipeIndex)];
    if (rng.count == 0) return nullptr;   // tombstoned
    return rng.shapeName[0] ? rng.shapeName : nullptr;
}

// M1 FROZEN-STATIC-CULL-RECORDS: build the frozen static cull-record prefix and
// install it via substrate_rebuildStaticPrefix. Call once per frame AFTER
// batcher_prepareBaseInstanceTable() (so baseInstanceForType is valid) and BEFORE
// compute_dispatch(). Rebuilds only when a store-dirty flush happened
// (s_goldenDirty); a clean frame is a no-op (the substrate keeps the frozen
// prefix). No effect unless MC2_GPU_CULL_STATIC_FROZEN_RECORDS is set.
// M2a POPULATION-SPLIT: expose the G2 frozen-records gate to the batcher.
// s_staticFrozenReg lives in the file-local anonymous namespace above, so this
// definition (in the GpuStaticPropRegistry namespace) can read it directly.
bool frozenRecordsArmed() { return s_staticFrozenReg; }

void buildStaticPrefixGolden() {
    if (!s_staticFrozenReg) return;
    if (!s_goldenDirty) return;                          // nothing changed since last build
    if (!batcher_isBaseInstanceTableReady()) return;     // base table not valid this frame yet

    const uint32_t S = static_cast<uint32_t>(s_goldenRecipeList.size());
    static std::vector<gpu_cull::GpuActorRecord> s_goldenStaging;
    s_goldenStaging.assign(S, gpu_cull::GpuActorRecord{});

    // Scatter each recipe's cached cull record to its exact binding-0 slot:
    //   global_slot = baseInstanceForType[typeID] + storeRank
    // Upload (batcher:4995) and baseInstanceForType (batcher:7764) share the
    // alpha-group ordering, so global_slot is the instance the draw renders for
    // this record => record-index == instance-pool-slot by construction.
    uint32_t placed = 0u, outOfRange = 0u;
    for (uint32_t ri : s_goldenRecipeList) {
        if (ri >= s_recipes.size()) { ++outOfRange; continue; }
        const uint32_t tid  = s_recipes[ri].typeID;
        const uint32_t base = batcher_getBaseInstanceForType(tid);
        const uint32_t rank = (ri < s_recipeToStoreSlot.size()) ? s_recipeToStoreSlot[ri] : 0u;
        const uint32_t slot = base + rank;
        if (slot >= S) { ++outOfRange; continue; }       // base+rank must land in [0,S)
        s_goldenStaging[slot] = s_cachedActorRecord[ri];
        ++placed;
    }

    gpu_cull::substrate_rebuildStaticPrefix(s_goldenStaging.data(), S);
    s_goldenDirty = false;

    // Order-validation oracle (gated): for each placed record, verify its actor
    // center is near the recipe's own leaf world position (within the bounding
    // sphere). A base+rank mis-scatter puts a DIFFERENT, far-away prop at the slot
    // => large position delta. This catches an ordering bug that per-type
    // counts-parity cannot see. MC2_GPU_CULL_STATIC_FROZEN_ORDER_ORACLE=1.
    static const bool s_orderOracle = mc2::gate::get<"MC2_GPU_CULL_STATIC_FROZEN_ORDER_ORACLE">();
    if (s_orderOracle) {
        uint32_t checked = 0u, mismatch = 0u;
        for (uint32_t ri : s_goldenRecipeList) {
            if (ri >= s_recipes.size()) continue;
            const uint32_t tid  = s_recipes[ri].typeID;
            const uint32_t slot = batcher_getBaseInstanceForType(tid)
                                + ((ri < s_recipeToStoreSlot.size()) ? s_recipeToStoreSlot[ri] : 0u);
            if (slot >= S) continue;
            const gpu_cull::GpuActorRecord& rec = s_goldenStaging[slot];
            // Leaf world pos from its model matrix (Stuff col-major; unswap to raw
            // MC2 east/north, matching the cull-record build at flush():~923-925).
            const float* m  = s_recipes[ri].modelMatrix;
            const float lx  = -m[3];      // raw east  = -stuff.x
            const float ly  =  m[11];     // raw north =  stuff.z
            const float dx  = rec.worldCenter[0] - lx;
            const float dy  = rec.worldCenter[1] - ly;
            const float tol = rec.boundingRadius * rec.boundingRadius + 1.0f;
            ++checked;
            if (dx*dx + dy*dy > tol) ++mismatch;   // far apart => wrong prop at slot
        }
        fprintf(stderr, "[GPU_CULL_FROZEN_ORDER_ORACLE] S=%u placed=%u outOfRange=%u "
                "checked=%u posMismatch=%u\n", S, placed, outOfRange, checked, mismatch);
        fflush(stderr);
    }
}


// Builds this frame's static-prop instance and cull-record submissions from the visible ranges. The latch, the
// diagnostic counters and clock, the range loop with the per-range light row, actor centre and cached-blob choice,
// and the cached / legacy branch stay here; each phase body is a helper declared in
// gos_static_prop_registry_internal.h.
void flush() {
    // V1A: latch BEFORE any early return so queryVisibility() always sees
    // a current-frame value (0 when disabled or nothing visible this frame).
    s_lastFlushLiveCount = static_cast<uint64_t>(s_liveRangeIndices.size());
    if (s_liveRangeIndices.empty()) return;
    const uint32_t currentFrame = g_mc2FrameCounter;
    GpuStaticPropBatcher& batcher = GpuStaticPropBatcher::instance();
    // 2026-05-10 diag: per-frame outcome counters across all ranges.
    static uint64_t s_diag_flush_calls = 0;
    static uint64_t s_diag_ranges_total = 0;
    static uint64_t s_diag_ranges_tombstone = 0;
    static uint64_t s_diag_ranges_stale_frame = 0;
    static uint64_t s_diag_ranges_stale_after_drawn = 0;  // R2B guard: stale drop of an already-drawn range
    // STATIC-REGISTRY-CURRENTNESS-GUARD-2: generalize the R2b stale_after_drawn counter
    // beyond the single tree case. The R2b counter fires for BOTH benign transient
    // off-screen ranges (cull-gated update, resolves in 1-2 frames) AND the real bug
    // (a live, already-drawn prop whose producer stamp froze → it vanishes on screen
    // every frame). Gate MC2_REGFLUSH_GUARD2 adds the discriminator: per-typeID buckets
    // + a consecutive-stale streak per regIdx. A streak crossing kVanishFrames = a
    // prop that is being dropped persistently = the impossible state (claimed live but
    // never drawn). Default-OFF (per-range map ops have cost); zero overhead when unset.
    static const bool s_guard2 = mc2::gate::get<"MC2_REGFLUSH_GUARD2">();
    static const uint32_t kVanishFrames = 16;  // off-screen returns in 1-2 frames; 16 (~0.3s) = not transient
    static std::unordered_map<int, uint64_t> s_staleByType;       // typeID -> total stale_after_drawn
    static std::unordered_map<uint32_t, uint32_t> s_staleStreak;  // regIdx -> consecutive stale-after-drawn frames
    static uint64_t s_diag_persistent_vanish = 0;                 // streaks that crossed kVanishFrames (the bug signal)
    static uint64_t s_diag_ranges_drawn = 0;
    static uint64_t s_diag_leaves_appended = 0;
    static uint64_t s_diag_total_ns = 0;
    const auto _flush_t0 = std::chrono::steady_clock::now();
    ++s_diag_flush_calls;
    // v2: reset per-recipe cull-submission tracking for this frame.
    // Timing: flush(N) clears → sets bits; extraction(N+1) reads before flush(N+1).
    // So extraction always sees frame N state, NOT the current frame being built.
    std::fill(s_recipeHasSubstrateRecord.begin(), s_recipeHasSubstrateRecord.end(), 0u);

    // 2b Stage 2 (Mechanism B-reinject): the persistent static store is rebuilt only
    // when the registry generation changes (spawn/despawn/light-change). On a clean
    // generation the per-range loop skips the instance append entirely; the static
    // instances are bulk-reinjected into s_bucketsByType after the loop. The records
    // (substrate) still run per-frame (that's Stage 3 work).
    GpuStaticPropBatcher& batcher_pb = GpuStaticPropBatcher::instance();
    const uint64_t s_currentGen   = getRegistryGeneration();
    const bool     s_storeDirty   = batcher_pb.persistentStaticGen() != s_currentGen;
    if (s_storeDirty) batcher_pb.clearPersistentStatic();
    flushFrozenStoreRebuild(batcher_pb, s_storeDirty);

    for (uint32_t regIdx : s_liveRangeIndices) {
        RecipeRange& rng = s_recipeRanges[regIdx];
        if (flushRangeTombstoneGuard(s_diag_ranges_total, s_diag_ranges_tombstone, regIdx, rng)) continue;

        if (flushRangeCurrentness(currentFrame, s_diag_ranges_stale_frame, s_diag_ranges_stale_after_drawn, s_diag_ranges_drawn, s_diag_persistent_vanish, s_guard2, kVanishFrames, s_staleByType, s_staleStreak, regIdx, rng)) continue;

        // Patch lightDataIndex from CacheGpuLightData() result gathered in
        // TreeAppearance::render() immediately before markVisible(). The UBO
        // is reset every frame by resetLightData(), so the baked recipe value
        // is stale; we read the freshly-gathered slot here.
        // UINT32_MAX must not reach flush(): render() guards against emitting
        // a static instance when getCachedGpuLightIndex() == UINT32_MAX by
        // calling invalidateStaticRegistration() and falling through to the
        // dynamic submit path instead.
        //
        // 2026-05-11 per-instance light source-of-truth: when
        // MC2_STATIC_PER_INSTANCE_LIGHT=1 is set AND markVisible() captured a
        // non-sentinel value into rng.lightDataIndex, prefer it over the
        // multi's per-type cache. This retires the last-writer-wins aliasing
        // that produced the MC2_STATIC_UPDATE_SKIP=1 wrong-RGB residual:
        // multiple actor instances sharing one multi-shape were all reading
        // the same multi->cachedGpuLightIndex_ at flush time, getting whichever
        // sibling's update/touch ran last. Per-actor capture decouples them.
        // Without the env flag, behavior is byte-identical to the historical
        // path.
        // ZERO-LIGHT-ROW-1: resolution now lives in mclib/static_light_row_guard.h
        // so the contract harness compiles the SAME function this flush runs.
        // Semantics are unchanged from the inline ternary this replaces.
        const uint32_t freshLightIdx =
            static_light_row::resolve(s_perInstanceLight,
                                      rng.lightDataIndex,
                                      rng.multi->getCachedGpuLightIndex());
        flushLightRowDiag(s_diag_flush_calls, regIdx, rng, freshLightIdx);

        // 2026-05-10 actor-center fix: every leaf of one multishape must share
        // the SAME substrate worldCenter (= the parent actor's position), so
        // the GPU frustum cull accepts/rejects all leaves of an actor as a
        // group. Without this, high-elevation leaves (LitWin_LookoutTower
        // ~60u up, S_admin roof tiles, watchtower platform tops) get
        // independently rejected by the sphere-vs-frustum test while the
        // base leaf passes — visually: base renders, all detail vanishes.
        // The first recipe in the range is the first SHAPE_NODE leaf
        // captured by registerStatic (typically the root/base mesh whose
        // local-to-actor transform is identity), so its modelMatrix
        // translation IS the actor's xlatPosition.
        const float* rootMtx = s_recipes[rng.first].modelMatrix;
        const float actorWorldCenter[3] = {
            -rootMtx[3],   // raw.x = -stuff.x
             rootMtx[11],  // raw.y =  stuff.z
             rootMtx[7],   // raw.z =  stuff.y (elev)
        };
        flushSeamProbe(regIdx, rng, actorWorldCenter);

        // Task 4 (STATICPROP-REGISTRY-FLUSH-CACHED-BLOB-2A): branch on gate.
        // Gate OFF (default): existing per-leaf rebuild runs verbatim (zero behavior change).
        // Gate ON: bulk submit + cached actor records — no per-frame light patch, no rebuild.
        //
        // 2A review fix (adversarial + renderspine M1): take the cached path for
        // THIS range ONLY when it is provably equivalent to legacy — i.e. the
        // legacy freshLightIdx selector resolves to the exact value the cached
        // path would submit (s_recipes[rng.first].lightDataIndex, stamped to the
        // range light by markVisible). This self-heals every light divergence the
        // cached path could otherwise introduce, falling back to the legacy
        // per-leaf path (which uses freshLightIdx directly) for that range:
        //   - MC2_STATIC_PER_INSTANCE_LIGHT=0 → freshLightIdx = multi cache != stamp → legacy
        //   - sentinel range light          → freshLightIdx falls back to multi cache,
        //                                      and the != sentinel guard forces legacy
        // Default config (per-instance light ON, non-sentinel) → freshLightIdx ==
        // the stamped index for all leaves → cached path, as proven by the oracle.
        // 2b Stage 2: persistent-buckets also uses the cached-record branch (same
        // equivalence guard) — its instance submit routes to the persistent store
        // instead of the per-frame bucket.
        const bool useCachedBlob =
            freshLightIdx != 0xFFFFFFFFu
            && freshLightIdx == s_recipes[rng.first].lightDataIndex;
        flushLeafDiag(regIdx, rng, freshLightIdx, useCachedBlob);
        if (!useCachedBlob) {
            flushLegacyRange(batcher, s_diag_flush_calls, s_diag_leaves_appended, regIdx, rng, freshLightIdx, actorWorldCenter);
        } else {
            flushCachedRange(batcher, s_storeDirty, s_diag_leaves_appended, regIdx, rng);
        }
    }

    flushReinjectPersistentStore(batcher_pb, s_currentGen, s_storeDirty);

    s_diag_total_ns += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - _flush_t0).count());

    flushZeroLightProbe(s_diag_flush_calls);
    flushDiagSummary(s_diag_flush_calls, s_diag_ranges_total, s_diag_ranges_tombstone, s_diag_ranges_stale_frame, s_diag_ranges_stale_after_drawn, s_diag_ranges_drawn, s_diag_leaves_appended, s_diag_persistent_vanish, s_diag_total_ns, s_guard2, s_staleByType, s_staleStreak);
    flushCullRecordVersion();
    // compute_dispatch() runs after this (moved to txmmgr.cpp between registry flush
    // and batcher flush) so it sees the appended static prop records.
    // batcher.flush() is called by txmmgr.cpp immediately after compute_dispatch().
}

// --- Extraction v1: per-recipe read-only accessors ---

static bool recipeValid(int32_t recipeIndex) {
    if (recipeIndex < 0 || recipeIndex >= static_cast<int32_t>(s_recipeRanges.size()))
        return false;
    return s_recipeRanges[static_cast<size_t>(recipeIndex)].count > 0; // count==0 = tombstone
}

bool staticPropGetModelMatrix(int32_t recipeIndex, float out[16]) {
    if (!recipeValid(recipeIndex)) return false;
    const RecipeRange& rng = s_recipeRanges[static_cast<size_t>(recipeIndex)];
    memcpy(out, s_recipes[rng.first].modelMatrix, sizeof(float) * 16);
    return true;
}

bool staticPropGetTypeId(int32_t recipeIndex, uint32_t* out) {
    if (!recipeValid(recipeIndex)) return false;
    const RecipeRange& rng = s_recipeRanges[static_cast<size_t>(recipeIndex)];
    *out = s_recipes[rng.first].typeID;
    return true;
}

// TERRAIN-DECAL-SLICE-0C: live per-frame modelMatrix overwrite for interactive
// placement tuning. Overwrites EVERY leaf's modelMatrix (all leaves of a mesh-decal
// recipe share the same face-frame transform), invalidates the cached cull records
// so the GPU cull position is rebuilt, and bumps the registry generation so the
// persistent-static store re-uploads next flush (same dirty mechanism markVisible
// uses for light/extent writes). Cheap: one memcpy per leaf + a generation bump.
// Returns false for an invalid/tombstoned recipe. Intended for a handful of decals
// under human ImGui tuning — do NOT call per-frame for thousands of props.
bool staticPropSetAllLeafMatrices(int32_t recipeIndex, const float in[16]) {
    if (!recipeValid(recipeIndex)) return false;
    const RecipeRange& rng = s_recipeRanges[static_cast<size_t>(recipeIndex)];
    for (uint32_t i = 0; i < rng.count; ++i) {
        memcpy(s_recipes[rng.first + i].modelMatrix, in, sizeof(float) * 16);
        invalidateCachedFlushRecord(rng.first + i);
    }
    ++s_registryGeneration;   // force persistent-static store rebuild next flush
    return true;
}

// SHADOW-STATIC-BUILDINGS-2: tag a recipe's population at registration time.
void setRecipePopulation(int32_t recipeIndex, GpuStaticPropPopulation pop) {
    const uint8_t p = static_cast<uint8_t>(pop);
    // COUNTER-HONESTY-1: record instrumentation coverage BEFORE the range check,
    // so "this population has a tagging path" stays true even if one particular
    // call arrives with a stale index.
    if (p < 8u) s_popsEverTagged |= static_cast<uint8_t>(1u << p);
    if (recipeIndex < 0 ||
        recipeIndex >= static_cast<int32_t>(s_recipeRanges.size())) return;
    s_recipeRanges[static_cast<size_t>(recipeIndex)].population = p;
}

void setRecipeNoShadow(int32_t recipeIndex, bool noShadow) {
    if (recipeIndex < 0 ||
        recipeIndex >= static_cast<int32_t>(s_recipeRanges.size())) return;
    s_recipeRanges[static_cast<size_t>(recipeIndex)].noShadow = noShadow;
}

// COUNTER-HONESTY-1: see header. out[4] counts recipes whose population was
// never tagged by any caller — the "never instrumented" bucket that keeps the
// breakdown from masquerading as a complete census.
void getPopulationCensus(uint32_t out[5]) {
    if (!out) return;
    for (int i = 0; i < 5; ++i) out[i] = 0u;
    for (const RecipeRange& rng : s_recipeRanges) {
        if (rng.count == 0) continue;             // tombstone
        if (rng.population < 4u) out[rng.population]++;
        else                     out[4]++;        // 0xFF = never tagged
    }
}

uint8_t getPopulationsEverTagged() { return s_popsEverTagged; }

// SHADOW-STATIC-BUILDINGS-2: append every non-tombstoned BUILDING recipe's leaf
// instances (baked modelMatrix + typeID) to `out`. Visibility-independent — reads
// the full registry (s_recipes/s_recipeRanges), NOT the per-frame visible buckets.
// Trees and unset-population recipes are excluded. Used once at the static
// shadow-map build to replay all rigid buildings into the world-fixed depth map.
void getBuildingShadowInstances(std::vector<GpuStaticPropInstance>& out) {
    out.clear();
    const uint8_t bldg = static_cast<uint8_t>(GpuStaticPropPopulation::Building);
    for (const RecipeRange& rng : s_recipeRanges) {
        if (rng.count == 0) continue;            // tombstone (invalidated/destroyed)
        if (rng.population != bldg) continue;     // buildings only; trees/unset excluded
        const size_t end = static_cast<size_t>(rng.first) + rng.count;
        if (end > s_recipes.size()) continue;     // defense: stale range
        out.insert(out.end(),
                   s_recipes.begin() + rng.first,
                   s_recipes.begin() + end);
    }
}

// SHADOW-DYNAMIC-PROP-CASTERS-1: append every non-tombstoned NON-BUILDING recipe's
// leaf instances (trees/fences/generic props) to `out`. Visibility-independent —
// reads the full registry (s_recipes/s_recipeRanges), NOT the per-frame visible
// buckets. Buildings are EXCLUDED (population==Building) because they cast via the
// world-fixed static map; everything else (Tree/Generic/Legacy and unset recipes,
// which are non-buildings in practice — buildings are tagged Building at register)
// is included so the dynamic shadow pass admits ALL props regardless of camera
// visibility. Mirror of getBuildingShadowInstances with the filter inverted.
void getDynamicPropShadowInstances(std::vector<GpuStaticPropInstance>& out) {
    out.clear();
    const uint8_t bldg = static_cast<uint8_t>(GpuStaticPropPopulation::Building);
    for (const RecipeRange& rng : s_recipeRanges) {
        if (rng.count == 0) continue;            // tombstone (invalidated/destroyed)
        if (rng.noShadow) continue;              // SHADOW-FOLIAGE: impostor/far-LOD casts no shadow
        if (rng.population == bldg) continue;    // buildings cast via the static map
        const size_t end = static_cast<size_t>(rng.first) + rng.count;
        if (end > s_recipes.size()) continue;     // defense: stale range
        out.insert(out.end(),
                   s_recipes.begin() + rng.first,
                   s_recipes.begin() + end);
    }
}

bool staticPropGetExtentRadius(int32_t recipeIndex, float* out) {
    if (!recipeValid(recipeIndex)) return false;
    *out = s_recipeRanges[static_cast<size_t>(recipeIndex)].extentRadius;
    return true;
}

bool staticPropGetLightDataIndex(int32_t recipeIndex, uint32_t* out) {
    if (!recipeValid(recipeIndex)) return false;
    *out = s_recipeRanges[static_cast<size_t>(recipeIndex)].lightDataIndex;
    return true;
}

// --- Extraction v1.1: per-typeID primary material cache ---

void staticPropCacheTypePrimaryMaterial(uint32_t typeID,
                                        int32_t  texArrayLayer,
                                        uint32_t materialIdx,
                                        bool     hasMaterialIdx,
                                        bool     wasAlphaOn,
                                        bool     multiPacket,
                                        uint8_t  alphaClass,
                                        uint32_t packetCount,
                                        uint32_t firstPacket) {
    if (typeID >= static_cast<uint32_t>(s_typeMatCache.size())) {
        s_typeMatCache.resize(typeID + 1u); // default-init: hasPrimary=false
    }
    StaticPropTypeMaterialCache& c = s_typeMatCache[typeID];
    // SNAPSHOT-DIRTYONLY coherence: this fn mutates five fields the render snapshot
    // captures per row (texArrayLayer/materialIdx/alphaClass/packetCount/firstPacket)
    // but is invoked from the batcher AFTER ExtractRenderSnapshot() in the same frame.
    // The snapshot dirty-only cache (render_snapshot.cpp) is gated on s_registryGeneration;
    // if a material first-cache or alpha-on→alpha-off upgrade here did NOT bump the
    // generation, the next clean frame would serve a stale material row. Snapshot the
    // pre-state and bump on any real change so the next snapshot rebuilds. First-write-
    // wins keeps this stable after warmup, so the dirty-only steady-state win is preserved.
    const StaticPropTypeMaterialCache before = c;
    // Type metadata: always idempotent (same type → same values).
    // Written unconditionally BEFORE the prefer-alpha-off check so
    // alphaClass/packetCount/firstPacket are always set regardless of primary outcome.
    c.alphaClass   = alphaClass;
    c.packetCount  = packetCount;
    c.firstPacket  = firstPacket;
    // Prefer alpha-off primary over alpha-on fallback.
    // Rule: alpha-off overwrites alpha-on; nothing overwrites alpha-off.
    // keepExisting == the original early-return conditions, restructured so the
    // generation-change check below always runs.
    const bool keepExisting = c.hasPrimary && (!c.primaryWasAlphaOn || wasAlphaOn);
    if (!keepExisting) {
        c.hasPrimary        = true;
        c.primaryWasAlphaOn = wasAlphaOn;
        c.multiPacket       = multiPacket;
        c.texArrayLayer     = texArrayLayer;
        c.materialIdx       = materialIdx;
        c.hasMaterialIdx    = hasMaterialIdx;
    }
    if (c.texArrayLayer != before.texArrayLayer ||
        c.materialIdx   != before.materialIdx   ||
        c.alphaClass    != before.alphaClass    ||
        c.packetCount   != before.packetCount   ||
        c.firstPacket   != before.firstPacket) {
        ++s_registryGeneration;   // material-cache change = snapshot row change
    }
}

void staticPropRegistryClearMaterialCache() {
    s_typeMatCache.clear();
    s_typeMatCache.shrink_to_fit();
}

void staticPropRegistryClearCullSubmissionState() {
    s_recipeHasSubstrateRecord.clear();
    s_recipeHasSubstrateRecord.shrink_to_fit();
    s_prevCullRecord.clear();
    s_prevCullRecord.shrink_to_fit();
    // Do NOT reset s_cullRecordVersion — stays monotonic across missions.
}

bool staticPropGetHasCullRecord(int32_t recipeIndex, bool* out) {
    if (!out) return false;
    *out = false;
    if (!recipeValid(recipeIndex)) return false;
    if (static_cast<size_t>(recipeIndex) < s_recipeHasSubstrateRecord.size())
        *out = (s_recipeHasSubstrateRecord[static_cast<size_t>(recipeIndex)] != 0u);
    return true;
}

bool staticPropGetTexArrayLayer(int32_t recipeIndex, int32_t* out) {
    if (!out) return false;
    *out = -1;
    if (!recipeValid(recipeIndex)) return false;
    const RecipeRange& rng = s_recipeRanges[static_cast<size_t>(recipeIndex)];
    const uint32_t typeID = s_recipes[rng.first].typeID;
    if (typeID >= static_cast<uint32_t>(s_typeMatCache.size())) return false;
    const StaticPropTypeMaterialCache& c = s_typeMatCache[typeID];
    if (!c.hasPrimary) return false;
    *out = c.texArrayLayer;
    return true;
}

bool staticPropGetMaterialIdx(int32_t recipeIndex, uint32_t* out) {
    if (!out) return false;
    *out = 0xFFFFFFFFu;
    if (!recipeValid(recipeIndex)) return false;
    const RecipeRange& rng = s_recipeRanges[static_cast<size_t>(recipeIndex)];
    const uint32_t typeID = s_recipes[rng.first].typeID;
    if (typeID >= static_cast<uint32_t>(s_typeMatCache.size())) return false;
    const StaticPropTypeMaterialCache& c = s_typeMatCache[typeID];
    if (!c.hasPrimary || !c.hasMaterialIdx) return false;
    *out = c.materialIdx;
    return true;
}

bool staticPropGetMaterialCacheInfo(int32_t recipeIndex,
                                    StaticPropTypeMaterialCache* out) {
    if (!out) return false;
    if (!recipeValid(recipeIndex)) return false;
    const RecipeRange& rng = s_recipeRanges[static_cast<size_t>(recipeIndex)];
    const uint32_t typeID = s_recipes[rng.first].typeID;
    if (typeID >= static_cast<uint32_t>(s_typeMatCache.size())) return false;
    const StaticPropTypeMaterialCache& c = s_typeMatCache[typeID];
    if (!c.hasPrimary) return false;
    *out = c;
    return true;
}

void staticPropGetMaterialCacheStats(MaterialCacheStats* out) {
    if (!out) return;
    *out = {};
    out->cacheVectorSize = static_cast<uint32_t>(s_typeMatCache.size());
    for (const auto& c : s_typeMatCache) {
        if (c.hasPrimary) {
            ++out->texWired;
            if (c.hasMaterialIdx)    ++out->matWired;
            if (c.multiPacket)       ++out->multiPacket;
            if (c.primaryWasAlphaOn) ++out->alphaOnFallback;
        } else {
            ++out->noPrimary; // informational only; may include resize-padding slots
        }
    }
}

bool staticPropGetInstanceFlags(int32_t recipeIndex, uint32_t* out) {
    if (!out) return false;
    *out = 0u;
    if (!recipeValid(recipeIndex)) return false;
    const RecipeRange& rng = s_recipeRanges[static_cast<size_t>(recipeIndex)];
    *out = s_recipes[rng.first].flags;
    return true;
}

} // namespace GpuStaticPropRegistry

// ---------------------------------------------------------------------------
