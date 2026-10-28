// Native terrain readiness and the shared CPU MVP snapshot; no GL indirect work.
#include <atomic>
#include "gos_terrain_indirect_internal.h"

namespace {
bool frameArmed = false;
bool everArmed = false;
uint32_t dispatchFp = 0;
uint64_t dispatchFrame = 0;
long dispatchEpoch = -1;
float dispatchMvp[16] = {};
float dispatchFloats[4] = {};
}

namespace gos_terrain_indirect {
bool IsFrameSolidArmed() { return frameArmed; }
bool WasEverFrameSolidArmed() { return everArmed; }
void BeginFrame() { frameArmed = false; }

void BeginFrameSolidWindow() {
    if (!SolidWindowEnabled()) return;
    // Reserve last-frame max + 10% slack + 64, mirroring water :149-158.
    // vector::clear on a trivially-destructible uint32_t is a size reset; no
    // allocation occurs in the hot loop once the high-water mark is reached.
    const size_t reserve =
        static_cast<size_t>((g_solidWindowMaxSeen + (g_solidWindowMaxSeen / 10) + 64));
    if (g_solidWindowStaging.capacity() < reserve)
        g_solidWindowStaging.reserve(reserve);
    g_solidWindowStaging.clear();
}

void AppendSolidWindowCandidate(int32_t vn0) {
    // Caller (terrain.cpp) asserts the quad already passed a predicate that is
    // STRICTLY LOOSER than the GPU keep-set: corners non-null, vertexNum >= 0,
    // and RecipeForVertexNum(vn0) != nullptr.  It applies NO pz, NO
    // terrainHandle, NO _wp2 filter — the GPU shader still does all of those
    // per-thread.  Looseness == guaranteed superset == no terrain-vanish.
    if (vn0 < 0) return;
    g_solidWindowStaging.push_back(static_cast<uint32_t>(vn0));
    if (g_solidWindowStaging.size() > g_solidWindowMaxSeen)
        g_solidWindowMaxSeen = static_cast<uint32_t>(g_solidWindowStaging.size());
}

// Phase 8b A/B: read-only view of the legacy staging window. No mutation.
const uint32_t* SolidWindowStagingData(uint32_t* outCount) {
    if (outCount) *outCount = static_cast<uint32_t>(g_solidWindowStaging.size());
    return g_solidWindowStaging.empty() ? nullptr : g_solidWindowStaging.data();
}

bool ComputePreflight() {
    frameArmed = false;
    if (!IsEnabled() || !IsDenseRecipeReady() || InMissionTransition()) return false;
    if (s_loadRaceDelayAtlas && g_atlasSize == 0) BuildColormapAtlas();
    frameArmed = gos_terrain_arm::ShouldArmGpuTerrain(
        gpu_driven::IsTerrainSolidEnabled(), !g_uniqueTerrainNodeIds.empty(), g_atlasSize > 0);
    everArmed = everArmed || frameArmed;
    if (frameArmed && g_loadRaceTrace) {
        g_loadRaceLastTerrainPath = 1;
        if (g_loadRaceArmFirstFrame < 0) g_loadRaceArmFirstFrame = static_cast<long>(g_mc2FrameCounter.load(std::memory_order_relaxed));
    }
    return frameArmed;
}

// Terrain, water and object draws share this snapshot and its view-content epoch.
void CaptureFrameInputs() {
    if (!frameArmed) return;
    const float* mvp = gos_GetTerrainMVPMat4();
    if (!mvp) return;
    uint32_t fp = 2166136261u;
    for (int k = 0; k < 12; ++k) {
        uint32_t bits = 0;
        memcpy(&bits, &mvp[k], sizeof(bits));
        fp ^= bits; fp *= 16777619u;
    }
    dispatchFp = fp;
    ++dispatchFrame;
    memcpy(dispatchMvp, mvp, sizeof(dispatchMvp));
    dispatchEpoch = ::g_viewContentEpoch;
    memcpy(dispatchFloats, mvp, sizeof(dispatchFloats));
}
} // namespace gos_terrain_indirect

extern "C" {
uint32_t gos_terrain_indirect_getDispatchMvpFp() {
    return dispatchFp;
}

// Water-consistency fix (2026-05-17): the full MVP terrain-solid baked its
// Fix-B clipPos with this frame. Callers MUST gate on IsFrameSolidArmed()
// (only then did CaptureFrameInputs refresh this); otherwise it is stale.
const float* gos_terrain_indirect_getDispatchMvp16() {
    return dispatchMvp;
}

uint64_t gos_terrain_indirect_getDispatchMvpFrameIdx() {
    return dispatchFrame;
}

// RENDER-VIEW-CURRENCY-1: view epoch (g_mvpDiagFrame) the live dispatch snapshot
// was sourced under. Consumers compare against the current g_mvpDiagFrame.
long gos_terrain_indirect_getDispatchMvpViewEpoch() {
    return dispatchEpoch;
}

void gos_terrain_indirect_getDispatchMvpFloats4(float out[4]) {
    out[0] = dispatchFloats[0];
    out[1] = dispatchFloats[1];
    out[2] = dispatchFloats[2];
    out[3] = dispatchFloats[3];
}

} // extern "C"
