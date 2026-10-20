//===========================================================================
// tex_resolve_table.cpp — sidecar implementation. See tex_resolve_table.h.
//===========================================================================
#include "tex_resolve_table.h"
#include "mc2_diag_verbose.h"  // QUIET-DEFAULT-BUILD-1
#include "../GameOS/gameos/gos_profiler.h"   // ZoneScopedN
#include <stdio.h>
#include <stdlib.h>
#include <algorithm>
#include "Gates/gate.h"

TexResolveTable g_texResolveTable = {};
uint64_t        g_currentFrameId  = 0;

namespace {
    uint64_t s_totalFrames         = 0;
    uint64_t s_totalResolves       = 0;
    constexpr uint64_t kSummaryEveryNFrames = 600;

    // Cold-path counters; throttled prints.
    uint64_t s_mismatchCount       = 0;
    uint64_t s_oobCount            = 0;
    constexpr uint64_t kMaxMismatchPrints = 32;
    constexpr uint64_t kMaxOobPrints      = 16;
    void advanceMemo() {
        if (++g_texResolveTable.memoGeneration==0) {
            std::fill(g_texResolveTable.stamps.begin(),g_texResolveTable.stamps.end(),0);
            g_texResolveTable.memoGeneration=1;
        }
    }

}

void texResolveLogMismatch(DWORD nodeId, DWORD table_h, DWORD legacy_h)
{
    if (s_mismatchCount++ < kMaxMismatchPrints) {
        MC2_DIAGF("[TEX_RESOLVE v1] event=mismatch frame=%llu nodeId=%u table=0x%08x legacy=0x%08x\n",
               static_cast<unsigned long long>(g_texResolveTable.buildGeneration),
               static_cast<unsigned>(nodeId), static_cast<unsigned>(table_h), static_cast<unsigned>(legacy_h));
        fflush(stdout);
    }
}

void texResolveLogOOB(DWORD nodeId)
{
    if (s_oobCount++ < kMaxOobPrints) {
        MC2_DIAGF("[TEX_RESOLVE v1] event=oob_node frame=%llu nodeId=%u max=%d\n",
               (unsigned long long)g_texResolveTable.buildGeneration,
               (unsigned)nodeId, (int)mc2::textures::assetCapacity());
        fflush(stdout);
    }
}

void initTexResolveTable(void)
{
    // Default ON with explicit opt-out. MC2_MODERN_TEX_RESOLVE=0|false|off disables.
    // MC2_MODERN_TEX_RESOLVE_VALIDATE implies enabled regardless of the opt-out.
    g_texResolveTable.validate = mc2::gate::get<"MC2_MODERN_TEX_RESOLVE_VALIDATE">();
    if (g_texResolveTable.validate) {
        g_texResolveTable.enabled = true;
    } else {
        g_texResolveTable.enabled = mc2::gate::get<"MC2_MODERN_TEX_RESOLVE">();   // promoted default ON
    }
    g_texResolveTable.trace = mc2::gate::get<"MC2_MODERN_TEX_RESOLVE_TRACE">();

    g_texResolveTable.handles.clear();
    g_texResolveTable.stamps.clear();
    g_texResolveTable.memoGeneration=1;
    g_texResolveTable.buildGeneration   = 0;
    g_texResolveTable.resolvedThisFrame = 0;
    g_texResolveTable.frameActive       = false;  // armed by first beginFrameTexResolve

    const char* mode = "off";
    if (g_texResolveTable.validate)      mode = "validate";
    else if (g_texResolveTable.enabled)  mode = "on (default)";

    MC2_DIAGF("[TEX_RESOLVE v1] event=startup mode=%s asset_capacity=%d\n",
           mode, (int)mc2::textures::assetCapacity());
    fflush(stdout);

    atexit(&shutdownTexResolveTable);
}

void beginFrameTexResolve(uint64_t frameId)
{
    ZoneScopedN("Terrain.BeginFrameTexResolve");

    if (!g_texResolveTable.enabled) {
        // Killswitch OFF — table never read; skip the memset and leave
        // frameActive false so tex_resolve falls through bit-exactly to legacy.
        return;
    }

    g_currentFrameId                  = frameId;
    g_texResolveTable.buildGeneration = frameId;

    // Reset the running per-frame counter at frame open. End-of-frame will
    // accumulate it into s_totalResolves before zeroing.
    g_texResolveTable.resolvedThisFrame = 0;

    g_texResolveTable.handles.resize(mc2::textures::assetCapacity(),TexResolveTable::kSentinel);
    g_texResolveTable.stamps.resize(g_texResolveTable.handles.size(),0);
    advanceMemo();
    g_texResolveTable.frameActive = true;

    if (g_texResolveTable.trace) {
        MC2_DIAGF("[TEX_RESOLVE v1] event=begin_frame frame=%llu\n",
               static_cast<unsigned long long>(frameId));
        fflush(stdout);
    }
}

void invalidateTexResolveTable(void)
{
    // Mission::load runs INSIDE a frame (quickload fires from mission update,
    // after Terrain::geometry armed the table). Mission::destroy has just
    // released every terrain node, so the memoized gos handles are stale.
    if (!g_texResolveTable.enabled) return;
    advanceMemo();
}

void endFrameTexResolve(void)
{
    ZoneScopedN("Terrain.EndFrameTexResolve");

    if (!g_texResolveTable.enabled || !g_texResolveTable.frameActive) {
        // Killswitch OFF, or beginFrameTexResolve never ran this frame
        // (e.g. mission-load path that gets to GameCamera::render without
        // having called Terrain::geometry). Nothing to close.
        return;
    }

    // Accumulate this frame's resolution count into the lifetime total
    // and bump the completed-frame counter. Counting at end (not begin)
    // means s_totalFrames reflects fully-rendered frames only.
    s_totalResolves += g_texResolveTable.resolvedThisFrame;
    s_totalFrames++;

    g_texResolveTable.frameActive = false;

    if ((s_totalFrames % kSummaryEveryNFrames) == 0) {
        const double avg_resolved = static_cast<double>(s_totalResolves) / static_cast<double>(s_totalFrames);
        MC2_DIAGF("[TEX_RESOLVE v1] event=summary frames=%llu resolved_per_frame_avg=%.1f mismatches=%llu oob=%llu\n",
               static_cast<unsigned long long>(s_totalFrames),
               avg_resolved,
               static_cast<unsigned long long>(s_mismatchCount),
               static_cast<unsigned long long>(s_oobCount));
        fflush(stdout);
    }
}

void shutdownTexResolveTable(void)
{
    MC2_DIAGF("[TEX_RESOLVE v1] event=shutdown total_frames=%llu total_resolves=%llu mismatches=%llu oob=%llu\n",
           static_cast<unsigned long long>(s_totalFrames),
           static_cast<unsigned long long>(s_totalResolves),
           static_cast<unsigned long long>(s_mismatchCount),
           static_cast<unsigned long long>(s_oobCount));
    fflush(stdout);
}
