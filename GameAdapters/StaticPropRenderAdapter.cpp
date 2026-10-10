// GameAdapters/StaticPropRenderAdapter.cpp
//
// Slice M1: the bridge .cpp. Includes BOTH sides.

#include "StaticPropRenderAdapter.h"
#include "mc2_diag_verbose.h"  // QUIET-DEFAULT-BUILD-1
#include "Gates/gate.h"

// Engine side.
#include "../RenderWorld/RenderWorld.h"
#include "../RenderCore/RenderObjectDesc.h"
#include "../RenderCore/StaticPropInstanceDesc.h"
// m5 fix: late-spawn path uses the new return-recipe accessor.
#include "gos_static_prop_registry.h"
// C1 fix: adapter is one of the two seams for POD-mirror translation;
// it MUST include the batcher to translate GpuStaticPropInstance ->
// StaticPropInstanceDesc.
#include "gos_static_prop_batcher.h"

// Game side. Per spec section 12 + adversarial M2 carve-out, this is
// the ONLY .cpp where game-side headers may be reached.
#include "appear.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>

namespace {

// C1 fix: game-side seam for POD-mirror translation. Engine-side seam
// lives in RenderWorld/legacy/static_prop_backend.cpp. Both perform
// the same translation; both have static_asserts guarding size match.
static_assert(sizeof(RenderCore::StaticPropInstanceDesc) ==
              sizeof(GpuStaticPropInstance),
              "StaticPropInstanceDesc / GpuStaticPropInstance size "
              "mismatch; update RenderCore/StaticPropInstanceDesc.h.");

static_assert(std::is_trivially_copyable<GpuStaticPropInstance>::value,
              "GpuStaticPropInstance must be trivially copyable.");

inline RenderCore::StaticPropInstanceDesc toMirror(
    const GpuStaticPropInstance& src) {
    RenderCore::StaticPropInstanceDesc dst;
    std::memcpy(&dst, &src, sizeof(dst));
    return dst;
}

} // namespace

namespace GameAdapters {
namespace StaticProp {

// Pre-mech-batcher static-prop reset. Called BEFORE GpuMechBatcher::onMapLoad().
// Original position: Mission::init line 1752 (GpuStaticPropBatcher::onMapLoad).
void beginMissionEarly() {
    GpuStaticPropBatcher::instance().onMapLoad();
}

// Post-mech-batcher static-prop init. Called AFTER GpuMechBatcher::onMapLoad().
// Absorbs (in order):
//   GpuStaticPropBatcher::setMissionForIbl(missionName)  ← V-IBL-STATIC-2
//   GpuStaticPropRegistry::init()                         ← Stage 3.C
//   RenderWorld::init()                                   ← old thin beginMission()
// missionName may be nullptr/empty — falls back to "default" IBL set.
void beginMissionLate(const char* missionName) {
    GpuStaticPropBatcher::setMissionForIbl(missionName);
    GpuStaticPropRegistry::init();
    RenderWorld::init();
}

// Pre-mech-batcher static-prop teardown. Called BEFORE GpuMechBatcher::onMapUnload().
// Original position: Mission::destroy line 3345 (GpuStaticPropBatcher::onMapUnload).
void endMissionEarly() {
    GpuStaticPropBatcher::instance().onMapUnload();
}

// Post-mech-batcher static-prop teardown. Called AFTER GpuMechBatcher::onMapUnload().
// Absorbs (in order):
//   GpuStaticPropRegistry::destroy()   ← Stage 3.C
//   RenderWorld::destroy()             ← old thin endMission()
void endMissionLate() {
    GpuStaticPropRegistry::destroy();
    RenderWorld::destroy();
}

// Thin RenderWorld seam for the EDITOR (see header). The editor drives the
// batcher/registry lifecycle directly and only needs RenderWorld init/destroy
// here. Equivalent to the pre-refactor thin beginMission()/endMission(); the
// game path now uses the Early/Late split instead.
void beginMission() {
    RenderWorld::init();
}
void endMission() {
    RenderWorld::destroy();
}

// Post-spawn geometry finalisation bridge. Wraps
// GpuStaticPropBatcher::instance().finalizeGeometry().
// Called from code/mission.cpp and code/saveload.cpp.
void finalizeGeometry() {
    GpuStaticPropBatcher::instance().finalizeGeometry();
}

// Per-frame live-instance list reset. Wraps
// GpuStaticPropRegistry::frameBegin().
// Called from code/gamecam.cpp.
void frameBegin() {
    GpuStaticPropRegistry::frameBegin();
}

// Registered static-prop type count post-finalize. Wraps the free
// function batcher_getTypeCount() so game TUs need not include the
// batcher header. Called from code/mission.cpp + code/saveload.cpp to
// size gpu_cull::compute_buildIndirectBuffer().
uint32_t typeCount() {
    return batcher_getTypeCount();
}

// Save-game restore path: batcher-only pre-spawn reset. Does NOT
// call setMissionForIbl (IBL set is already correct from the
// previous beginMission call; this is a within-mission save restore),
// does NOT call GpuStaticPropRegistry::init() (destroy() was called
// via endMission() already and registry is dormant until next
// beginMission), does NOT call RenderWorld::init() (same reason).
// Mirrors code/saveload.cpp's pre-spawn reset at Mission::load.
void resetForRestore() {
    GpuStaticPropBatcher::instance().onMapLoad();
}

RenderCore::RenderObjectHandle syncStaticProp(
    TG_MultiShape* shape,
    const GpuStaticPropInstance* batchData,
    size_t batchCount,
    int32_t* legacyRecipeIndexOut) {

    if (shape == nullptr || batchCount == 0) {
        if (legacyRecipeIndexOut) *legacyRecipeIndexOut = -1;
        return RenderCore::RenderObjectHandle::invalid();
    }

    RenderCore::StaticPropDesc desc;
    desc.shape = shape;
    desc.batch.reserve(batchCount);
    for (size_t i = 0; i < batchCount; ++i) {
        desc.batch.push_back(toMirror(batchData[i]));
    }

    RenderCore::RenderObjectHandle h =
        RenderWorld::upsertStaticProp(std::move(desc));

    if (legacyRecipeIndexOut) {
        *legacyRecipeIndexOut =
            h.isValid() ? static_cast<int32_t>(h.index()) : -1;
    }

    if (mc2::gate::get<"MC2_RENDER_WORLD_TRACE">()) {
        MC2_DIAGFE(stderr,
            "[RENDER_WORLD v1] event=adapter_sync_static "
            "shape=%p batch=%zu handle.valid=%d legacy=%d\n",
            (void*)shape, batchCount, (int)h.isValid(),
            legacyRecipeIndexOut ? *legacyRecipeIndexOut : -2);
    }
    return h;
}

RenderCore::RenderObjectHandle syncStaticPropLateSpawn(
    Appearance* app,
    int32_t* legacyRecipeIndexOut) {

    if (!app) {
        if (legacyRecipeIndexOut) *legacyRecipeIndexOut = -1;
        return RenderCore::RenderObjectHandle::invalid();
    }

    const int32_t recipe =
        GpuStaticPropRegistry::registerStaticPropAndReturnRecipe(app);

    RenderCore::RenderObjectHandle h =
        RenderWorld::adoptStaticPropRecipe(recipe);

    if (legacyRecipeIndexOut) {
        *legacyRecipeIndexOut = recipe;
    }

    if (mc2::gate::get<"MC2_RENDER_WORLD_TRACE">()) {
        MC2_DIAGFE(stderr,
            "[RENDER_WORLD v1] event=adapter_sync_late "
            "app=%p recipe=%d handle.valid=%d\n",
            (void*)app, recipe, (int)h.isValid());
    }
    return h;
}

void destroyStaticProp(RenderCore::RenderObjectHandle h) {
    RenderWorld::destroy(h);
}

void destroyStaticPropByIndex(int32_t recipeIndex) {
    if (recipeIndex < 0) return;
    RenderWorld::destroy(
        RenderCore::RenderObjectHandle::make(
            static_cast<uint32_t>(recipeIndex), 1u));
}

void getRecipeShapeName(int32_t recipeIndex, char* out, size_t outLen) {
    if (!out || outLen == 0) return;
    out[0] = '\0';
    if (recipeIndex < 0) return;
    const char* name = GpuStaticPropRegistry::getRecipeShapeName(recipeIndex);
    if (!name) return;
    std::strncpy(out, name, outLen - 1);
    out[outLen - 1] = '\0';
}

// RENDERER-INPUT-READBACK-1: pass-through recipe readback. See the contract
// block in StaticPropRenderAdapter.h. The `recipeIndex < 0` guard is the
// sentinel-translation rule at this seam (-1 == invalid); the registry
// accessors range-check and tombstone-check on their own side, so a false
// from either layer means the same thing to the caller: cannot answer.
bool recipeModelMatrix(int32_t recipeIndex, float out[16]) {
    if (!out || recipeIndex < 0) return false;
    return GpuStaticPropRegistry::staticPropGetModelMatrix(recipeIndex, out);
}

bool recipeTypeId(int32_t recipeIndex, uint32_t* out) {
    if (!out || recipeIndex < 0) return false;
    return GpuStaticPropRegistry::staticPropGetTypeId(recipeIndex, out);
}

bool recipeLightDataIndex(int32_t recipeIndex, uint32_t* out) {
    if (!out || recipeIndex < 0) return false;
    return GpuStaticPropRegistry::staticPropGetLightDataIndex(recipeIndex, out);
}

bool recipeInstanceFlags(int32_t recipeIndex, uint32_t* out) {
    if (!out || recipeIndex < 0) return false;
    return GpuStaticPropRegistry::staticPropGetInstanceFlags(recipeIndex, out);
}

bool recipeHadCullRecord(int32_t recipeIndex, bool* out) {
    if (!out || recipeIndex < 0) return false;
    return GpuStaticPropRegistry::staticPropGetHasCullRecord(recipeIndex, out);
}

bool getMaterialGpuData(uint32_t materialHandleBits, RenderCore::MaterialGpu* out) {
    if (!out || materialHandleBits == 0u) return false;
    RenderCore::MaterialHandle mh;
    mh.bits = materialHandleBits;
    return batcher_getMaterialGpuEntry(mh.index(), out);
}

bool isMaterialGpuActive() {
    RenderCore::MaterialGpu dummy;
    // Table is active if entry 0 exists (one sentinel entry is always uploaded
    // when MC2_MATERIAL_GPU=1 and at least one prop registered).
    return batcher_getMaterialGpuEntry(0u, &dummy);
}

} // namespace StaticProp
} // namespace GameAdapters
