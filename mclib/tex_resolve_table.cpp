//===========================================================================
// tex_resolve_table.cpp — sidecar implementation. See tex_resolve_table.h.
//===========================================================================
#include "tex_resolve_table.h"
#include "../GameOS/gameos/gos_profiler.h"   // ZoneScopedN
#include <stdio.h>
#include <stdlib.h>          // getenv
#include <string.h>          // memset

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
}

void texResolveLogMismatch(DWORD nodeId, DWORD table_h, DWORD legacy_h)
{
    if (s_mismatchCount++ < kMaxMismatchPrints) {
        printf("[TEX_RESOLVE v1] event=mismatch frame=%llu nodeId=%u table=0x%08x legacy=0x%08x\n",
               (unsigned long long)g_texResolveTable.buildGeneration,
               (unsigned)nodeId, (unsigned)table_h, (unsigned)legacy_h);
        fflush(stdout);
    }
}

void texResolveLogOOB(DWORD nodeId)
{
    if (s_oobCount++ < kMaxOobPrints) {
        printf("[TEX_RESOLVE v1] event=oob_node frame=%llu nodeId=%u max=%d\n",
               (unsigned long long)g_texResolveTable.buildGeneration,
               (unsigned)nodeId, (int)MC_MAXTEXTURES);
        fflush(stdout);
    }
}

void initTexResolveTable(void)
{
    // Validate implies enabled — otherwise setting only MC2_MODERN_TEX_RESOLVE_VALIDATE
    // would print "validate" at startup but tex_resolve() would short-circuit on !enabled.
    g_texResolveTable.validate = (getenv("MC2_MODERN_TEX_RESOLVE_VALIDATE") != nullptr);
    g_texResolveTable.enabled  = g_texResolveTable.validate
                              || (getenv("MC2_MODERN_TEX_RESOLVE") != nullptr);
    g_texResolveTable.trace    = (getenv("MC2_MODERN_TEX_RESOLVE_TRACE") != nullptr);

    memset(g_texResolveTable.handles, 0xFF, sizeof(g_texResolveTable.handles));
    g_texResolveTable.buildGeneration   = 0;
    g_texResolveTable.resolvedThisFrame = 0;
    g_texResolveTable.frameActive       = false;  // armed by first beginFrameTexResolve

    const char* mode = "off";
    if (g_texResolveTable.validate)      mode = "validate";
    else if (g_texResolveTable.enabled)  mode = "on";

    printf("[TEX_RESOLVE v1] event=startup mode=%s max_textures=%d\n",
           mode, (int)MC_MAXTEXTURES);
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

    memset(g_texResolveTable.handles, 0xFF, sizeof(g_texResolveTable.handles));
    g_texResolveTable.frameActive = true;

    if (g_texResolveTable.trace) {
        printf("[TEX_RESOLVE v1] event=begin_frame frame=%llu\n",
               (unsigned long long)frameId);
        fflush(stdout);
    }
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
        const double avg_resolved = (double)s_totalResolves / (double)s_totalFrames;
        printf("[TEX_RESOLVE v1] event=summary frames=%llu resolved_per_frame_avg=%.1f mismatches=%llu oob=%llu\n",
               (unsigned long long)s_totalFrames,
               avg_resolved,
               (unsigned long long)s_mismatchCount,
               (unsigned long long)s_oobCount);
        fflush(stdout);
    }
}

void shutdownTexResolveTable(void)
{
    printf("[TEX_RESOLVE v1] event=shutdown total_frames=%llu total_resolves=%llu mismatches=%llu oob=%llu\n",
           (unsigned long long)s_totalFrames,
           (unsigned long long)s_totalResolves,
           (unsigned long long)s_mismatchCount,
           (unsigned long long)s_oobCount);
    fflush(stdout);
}
