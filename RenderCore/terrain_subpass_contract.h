#pragma once
// TERRAIN-SUBPASS-MODEL-1
// Authoritative recon: docs/render-backend-seams/terrain-subpass-recon-1.md
//
// Descriptive/modeling-only. No runtime behavior. No GL includes. No execute() callbacks.
// The parent `Terrain` row in RenderPassContract.h kFramePassOrder remains the scheduled
// slot — this table refines it with per-branch detail (reads, barrier, mvp-source,
// draw-site, latch state) without touching the scheduler.

#include <cstdint>
#include "RenderCore/RenderPassContract.h"   // BarrierKind; transitively RenderResourceRegistry.h
#include "RenderCore/terrain_path_telemetry.h" // TerrainPath enum (reuse — do NOT redefine)

namespace RenderCore { namespace framegraph {

// Which MVP snapshot each branch consumes.
enum class TerrainMvpSource : uint8_t {
    Live          = 0,  // fresh camera transform every frame
    SnapshotEpoch = 1,  // dispatch-MVP ring-slot + g_viewContentEpoch
};

// Where in the frame loop the branch emits its draw.
enum class TerrainDrawSite : uint8_t {
    Gamecam     = 0,  // LOD-chunk draws in gamecam.cpp:508 BEFORE renderLists
    RenderLists = 1,  // the other 3 branches draw inside renderLists
};

struct TerrainSubPass {
    TerrainPath      path;
    const char*      name;
    RenderResourceId reads[6];            // Unknown(=0)-terminated
    RenderResourceId writes[2];           // all four share {MainColor, MainDepth}
    BarrierKind      barrierAfter;
    TerrainMvpSource mvpSource;
    TerrainDrawSite  drawSite;
    bool             producesTerrainLatch;     // INTENT: every branch is supposed to set markTerrainDrawn
    bool             latchActuallyImplemented; // REALITY (recon §4): Indirect bridge does NOT call it
    const char*      note;
};

static constexpr TerrainSubPass kTerrainSubPasses[] = {
    {
        TerrainPath::LODChunk,
        "LODChunk",
        {   // reads (Unknown-terminated)
            RenderResourceId::TerrainHeightSsbo,
            RenderResourceId::Unknown,
            RenderResourceId::Unknown,
            RenderResourceId::Unknown,
            RenderResourceId::Unknown,
        },
        { RenderResourceId::MainColor, RenderResourceId::MainDepth },
        BarrierKind::None,
        TerrainMvpSource::Live,       // note: once armed -> SnapshotEpoch
        TerrainDrawSite::Gamecam,
        /* producesTerrainLatch     */ true,
        /* latchActuallyImplemented */ true,
        "DEFAULT; draws gamecam.cpp:508; latch gos_terrain_lod_chunk.cpp:1160",
    },
    {
        TerrainPath::IndirectBridge,
        "IndirectBridge",
        {   // reads (Unknown-terminated)
            RenderResourceId::TerrainRecipeBuffer,
            RenderResourceId::TerrainThinBuffer,
            RenderResourceId::CementAtlas,
            RenderResourceId::TransitionMaskArray,
            RenderResourceId::Unknown,
        },
        { RenderResourceId::MainColor, RenderResourceId::MainDepth },
        BarrierKind::Command,
        TerrainMvpSource::SnapshotEpoch,
        TerrainDrawSite::RenderLists,
        /* producesTerrainLatch     */ true,
        /* latchActuallyImplemented */ true,   // markTerrainDrawn now called gos_terrain_indirect.cpp:3750
                                               // (TERRAIN-INDIRECT-LATCH-FIX-1 / 26ee9bdd);
                                               // conditional on s_frameSolidCmdCount>0, like the legacy path
        "markTerrainDrawn called gos_terrain_indirect.cpp:3750 (TERRAIN-INDIRECT-LATCH-FIX-1/26ee9bdd); "
        "conditional on s_frameSolidCmdCount>0 -- all four branches now set the latch (regression guard)",
    },
    {
        TerrainPath::PatchStreamThin,
        "PatchStreamThin",
        {   // reads (Unknown-terminated)
            RenderResourceId::TerrainThinBuffer,
            RenderResourceId::Unknown,
            RenderResourceId::Unknown,
            RenderResourceId::Unknown,
            RenderResourceId::Unknown,
        },
        { RenderResourceId::MainColor, RenderResourceId::MainDepth },
        BarrierKind::None,
        TerrainMvpSource::Live,
        TerrainDrawSite::RenderLists,
        /* producesTerrainLatch     */ true,
        /* latchActuallyImplemented */ true,
        "latch gos_terrain_patch_stream.cpp:1500",
    },
    {
        TerrainPath::LegacyMLR,
        "LegacyMLR",
        {   // reads (Unknown-terminated)
            RenderResourceId::Unknown,
            RenderResourceId::Unknown,
            RenderResourceId::Unknown,
            RenderResourceId::Unknown,
            RenderResourceId::Unknown,
        },
        { RenderResourceId::MainColor, RenderResourceId::MainDepth },
        BarrierKind::None,
        TerrainMvpSource::Live,
        TerrainDrawSite::RenderLists,
        /* producesTerrainLatch     */ true,
        /* latchActuallyImplemented */ true,
        "latch gameos_graphics.cpp:7292 CONDITIONAL on extras-count>0 — secondary latch-skip risk",
    },
};

static constexpr int kTerrainSubPassCount =
    static_cast<int>(sizeof(kTerrainSubPasses) / sizeof(kTerrainSubPasses[0]));

static_assert(kTerrainSubPassCount == static_cast<int>(TerrainPath::Count),
    "one sub-pass row per TerrainPath");

inline const TerrainSubPass* findTerrainSubPass(TerrainPath p) {
    for (int i = 0; i < kTerrainSubPassCount; ++i)
        if (kTerrainSubPasses[i].path == p)
            return &kTerrainSubPasses[i];
    return nullptr;
}

// ---------------------------------------------------------------------------
// LATCH-AUDIT kernel
// Returns false if any row has producesTerrainLatch && !latchActuallyImplemented.
// All four branches now return true (TERRAIN-INDIRECT-LATCH-FIX-1 / 26ee9bdd closed the
// IndirectBridge gap). This is now a REGRESSION GUARD: if any branch's markTerrainDrawn
// is removed and latchActuallyImplemented is not flipped back to false, tests will catch it.
// ---------------------------------------------------------------------------
inline bool allDeclaredLatchProducersImplemented() {
    for (int i = 0; i < kTerrainSubPassCount; ++i)
        if (kTerrainSubPasses[i].producesTerrainLatch && !kTerrainSubPasses[i].latchActuallyImplemented)
            return false;
    return true;
}

// Returns the first path that declares the latch but hasn't implemented it,
// or TerrainPath::Count if all are implemented.
inline TerrainPath firstUnimplementedLatchProducer() {
    for (int i = 0; i < kTerrainSubPassCount; ++i)
        if (kTerrainSubPasses[i].producesTerrainLatch && !kTerrainSubPasses[i].latchActuallyImplemented)
            return kTerrainSubPasses[i].path;
    return TerrainPath::Count;
}

// ---------------------------------------------------------------------------
// MUTUAL-EXCLUSION + ACTIVE-BRANCH PROBE
// Pure: takes counter values as args — does NOT read globals. Deterministic in tests.
// ---------------------------------------------------------------------------
inline int terrainPathsThatDrew(const unsigned long counts[static_cast<int>(TerrainPath::Count)]) {
    int n = 0;
    for (int i = 0; i < static_cast<int>(TerrainPath::Count); ++i)
        if (counts[i] > 0) ++n;
    return n;
}

inline TerrainPath dominantTerrainPath(const unsigned long counts[static_cast<int>(TerrainPath::Count)]) {
    int best = -1;
    unsigned long bestCount = 0;
    for (int i = 0; i < static_cast<int>(TerrainPath::Count); ++i) {
        if (counts[i] > bestCount) {
            bestCount = counts[i];
            best = i;
        }
    }
    if (best < 0) return TerrainPath::Count;
    return static_cast<TerrainPath>(best);
}

// Runtime convenience overload: snapshots the live telemetry counters into a local
// array and calls the pure version above. Reuses existing terrain_path_telemetry.h counters.
inline TerrainPath dominantTerrainPathLive() {
    unsigned long counts[static_cast<int>(TerrainPath::Count)];
    for (int i = 0; i < static_cast<int>(TerrainPath::Count); ++i)
        counts[i] = terrainPathCount(static_cast<TerrainPath>(i));
    return dominantTerrainPath(counts);
}

// ---------------------------------------------------------------------------
// SHARED-INVARIANT helper
// ---------------------------------------------------------------------------
inline bool allTerrainSubPassesWriteMainColorDepth() {
    for (int i = 0; i < kTerrainSubPassCount; ++i) {
        if (kTerrainSubPasses[i].writes[0] != RenderResourceId::MainColor) return false;
        if (kTerrainSubPasses[i].writes[1] != RenderResourceId::MainDepth) return false;
    }
    return true;
}

}} // namespace RenderCore::framegraph
