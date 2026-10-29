// State and helpers shared by the static-prop registry pieces: gos_static_prop_registry.cpp (registration, visibility,
// extraction) and the flush phase helpers in gos_static_prop_registry_flush*.cpp.
#ifndef GOS_STATIC_PROP_REGISTRY_INTERNAL_H
#define GOS_STATIC_PROP_REGISTRY_INTERNAL_H

#include <atomic>
#include "gos_static_prop_registry.h"
#include "mc2_diag_verbose.h"  // QUIET-DEFAULT-BUILD-1
#include "../../mclib/render_submissions.h"  // 2026-05-05: peekLightSlotNumLights/getLightStructCount for flush trace
#include "../../mclib/static_light_row_guard.h"  // ZERO-LIGHT-ROW-1: shared light-index validity contract
#include "../../mclib/appear.h"   // Task 6: Appearance* for registerStaticProp()
#include "../../mclib/apprtype.h"  // AppearanceType::name (inspector shapeName capture)
#include "gpu_cull_substrate.h"  // C1b GPU authority flip: substrate_appendStaticPropRecord
#include "gpu_cull_record.h"     // C1b: GpuActorRecord, Cat_StaticProp, CategoryMask
#include "../../mclib/terrain.h" // C1b temporal-superset: Terrain::worldToBlockIdx()
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <climits>
#include <algorithm>
#include <array>
#include <chrono>
#include <set>
#include <unordered_map>
#include "diagnostic_trace.h"
#include "Gates/gate.h"

// legacy texture manager singleton, defined in mclib/txmmgr.cpp.

// 2026-05-05: frame counter for cull-aware static replay. When an actor goes
// offscreen, MC2's cull gate skips its update() (`cull_gates_are_load_bearing.md`),
// which means CacheGpuLightData()/ResubmitCachedGpuLightData() also doesn't run,
// so its cachedGpuLightIndex_ points at a UBO slot whose content was filled by
// a different actor THIS frame (or is beyond the upload count). flush() must
// detect this and skip emitting a draw for that recipe.
extern std::atomic<uint32_t> g_mc2FrameCounter;

// [G1-STATIC-EAGER-LIGHT v1] Zero-slot probe helper (defined in mclib/txmmgr.cpp).
// Returns true when lightData_[recipeIndex] has numLights_ > 0 (baked at least once).
// File-scope so the declaration lands outside the GpuStaticPropRegistry namespace.
extern bool mc2IsStaticLightSlotBaked(int32_t recipeIndex);

// PROP-RECIPE-BAKE-1 per-recipe row summary (defined in mclib/txmmgr.cpp).
// File-scope for the SAME reason as the declaration above — declaring it inside
// flush() namespaces it into GpuStaticPropRegistry:: and fails to link (LNK2019).
extern bool mc2PeekStaticLightSlotSummary(int32_t recipeIndex, int* outNumLights,
                                          float amb[3], float sun[3]);

// PROP-RECIPE-BAKE-1 typeID -> shape node name + alpha class (defined in
// gos_static_prop_batcher.cpp). File-scope for the same LNK2019 reason.
extern bool mc2PeekStaticPropTypeInfo(uint32_t typeID, const char** outName,
                                      int* outAlphaClass);

#define SP_TRACE(fmt, ...) \
    do { if (s_trace) { printf("[STATIC_PROP] " fmt "\n", ##__VA_ARGS__); \
         fflush(stdout); } } while (0)

namespace GpuStaticPropRegistry {

struct RecipeRange {
    uint32_t       first;   // index into s_recipes
    uint32_t       count;   // 0 = invalidated (tombstone)
    TG_InstanceId  multiId; // generation-checked borrow of the actor-owned instance
    TG_MultiShape* multi() const { return TglInstanceRegistry::get().resolve(multiId); }
    // Texture pin sibling (texture-pin-fix spec):
    std::vector<DWORD> pinnedTextureNodes;  // mcTextureNodeIndex values pinned for this range
    bool               pinsReleased;        // double-release guard for invalidate→destroy ordering
    // [STATIC_FIRST_FRAME v1] proof-of-fix fields (Track B Task 4):
    uint32_t           registeredOnFrame;   // g_mc2FrameCounter at registerRecipe()
    bool               firstFlushSeen;      // cleared at registerRecipe; set on first successful flush
    // 2026-05-11 per-instance light-idx capture (MC2_STATIC_PER_INSTANCE_LIGHT):
    // populated by markVisible() with the slot the actor's update/touch wrote
    // into multi->cachedGpuLightIndex_ before sibling actors overwrote it.
    // UINT32_MAX = uncaptured (flush falls back to multi->getCachedGpuLightIndex()).
    uint32_t           lightDataIndex;
    // 2026-05-22 per-prop extent radius (F4 T3 static-prop pop-in fix):
    // world-unit bounding sphere radius from bldgShape/treeShape->GetExtentRadius().
    // Written to GpuActorRecord.boundingRadius in flush(). 0.0f = uncaptured;
    // flush falls back to 200.0f (legacy placeholder) for unpatched callers.
    float              extentRadius;
    // Inspector: AppearanceType::name captured at registerStaticPropAndReturnRecipe
    // time (late-spawn path only; bulk registerRecipe path has no name available
    // in RelWithDebInfo). Empty string = name not captured.
    char               shapeName[128];
    // SHADOW-STATIC-BUILDINGS-2: population tag set at registration via
    // setRecipePopulation() (caller knows Building vs Tree). 0xFF = unset
    // (excluded from the static building shadow). Visibility-independent — the
    // static building shadow pass replays from here, NOT per-frame buckets.
    uint8_t            population;
    // SHADOW-FOLIAGE-ALPHA-DISCARD: impostor / far-LOD recipes skip dynamic
    // shadow casting (flat alpha cards otherwise cast solid blob shadows; the
    // shadow depth pass has no alpha discard). Set via setRecipeNoShadow().
    bool               noShadow;
};

// Registry gates (read once at program start).
extern const bool s_trace;
extern const bool s_perInstanceLight;
extern const bool s_staticFrozenReg;

// Registry state the flush phases read and write.
extern uint64_t s_cullRecordVersion;
extern std::vector<uint8_t> s_prevCullRecord;
extern std::vector<GpuStaticPropInstance> s_recipes;
extern std::vector<RecipeRange> s_recipeRanges;
extern std::vector<gpu_cull::GpuActorRecord> s_cachedActorRecord;
extern std::vector<uint8_t> s_cachedActorRecordValid;
extern std::vector<uint32_t> s_recipeToStoreSlot;
extern std::vector<uint32_t> s_goldenRecipeList;
extern std::vector<uint32_t> s_pbStoreCursor;
extern bool s_goldenDirty;
extern std::vector<uint8_t> s_recipeHasSubstrateRecord;
extern std::vector<uint32_t> s_liveRangeIndices;
extern uint64_t s_firstFrameSkipCount;

// Builds the immutable cull record for one recipe leaf.
void buildCachedActorRecord(const RecipeRange& rng, uint32_t recipeIdx, gpu_cull::GpuActorRecord& out);

// Phases of flush() (gos_static_prop_registry_flush_range.cpp, gos_static_prop_registry_flush_tail.cpp).
void flushFrozenStoreRebuild(GpuStaticPropBatcher& batcher_pb, bool s_storeDirty);
bool flushRangeTombstoneGuard(uint64_t& s_diag_ranges_total, uint64_t& s_diag_ranges_tombstone, uint32_t regIdx, RecipeRange& rng);
bool flushRangeCurrentness(uint32_t currentFrame, uint64_t& s_diag_ranges_stale_frame, uint64_t& s_diag_ranges_stale_after_drawn, uint64_t& s_diag_ranges_drawn, uint64_t& s_diag_persistent_vanish, bool s_guard2, uint32_t kVanishFrames, std::unordered_map<int, uint64_t>& s_staleByType, std::unordered_map<uint32_t, uint32_t>& s_staleStreak, uint32_t regIdx, RecipeRange& rng);
void flushLightRowDiag(uint64_t s_diag_flush_calls, uint32_t regIdx, RecipeRange& rng, uint32_t freshLightIdx);
void flushSeamProbe(uint32_t regIdx, RecipeRange& rng, const float (&actorWorldCenter)[3]);
void flushLeafDiag(uint32_t regIdx, RecipeRange& rng, uint32_t freshLightIdx, bool useCachedBlob);
void flushLegacyRange(GpuStaticPropBatcher& batcher, RecipeRange& rng, uint32_t freshLightIdx);
void flushCachedRange(GpuStaticPropBatcher& batcher, bool storeDirty, RecipeRange& rng);
void flushZeroLightProbe(uint64_t s_diag_flush_calls);
void flushDiagSummary(uint64_t s_diag_flush_calls, uint64_t& s_diag_ranges_total, uint64_t& s_diag_ranges_tombstone, uint64_t& s_diag_ranges_stale_frame, uint64_t& s_diag_ranges_stale_after_drawn, uint64_t& s_diag_ranges_drawn, uint64_t& s_diag_leaves_appended, uint64_t& s_diag_persistent_vanish, uint64_t s_diag_total_ns, bool s_guard2, std::unordered_map<int, uint64_t>& s_staleByType, std::unordered_map<uint32_t, uint32_t>& s_staleStreak);
void flushCullRecordVersion();

}  // namespace GpuStaticPropRegistry

#endif  // GOS_STATIC_PROP_REGISTRY_INTERNAL_H
