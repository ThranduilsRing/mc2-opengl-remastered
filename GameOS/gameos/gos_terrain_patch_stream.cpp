// GameOS/gameos/gos_terrain_patch_stream.cpp
#include "gos_terrain_patch_stream.h"

#include <cstdio>
#include <cstdlib>

namespace {
    bool s_killswitch = false;   // resolved at init() from MC2_MODERN_TERRAIN_SURFACE
    bool s_initOk     = false;   // true once init() succeeded
    bool s_traceOn    = false;   // MC2_PATCH_STREAM_TRACE=1
}

bool TerrainPatchStream::init()
{
    s_killswitch = (getenv("MC2_MODERN_TERRAIN_SURFACE") != nullptr) &&
                   (getenv("MC2_MODERN_TERRAIN_SURFACE")[0] == '1');
    s_traceOn    = (getenv("MC2_PATCH_STREAM_TRACE") != nullptr);

    if (!s_killswitch) {
        // Default-off path. Do nothing (no GL, no allocations).
        // Lifecycle: do not print 'init' when disabled — matches stock behavior.
        return true;
    }

    // GL allocation arrives in Task 2. For now mark init OK so subsequent
    // tasks can wire isReady() correctly even when the ring isn't real yet.
    s_initOk = true;
    fprintf(stderr,
        "[PATCH_STREAM v1] event=init slots=%u colorBytes=%u extrasBytes=%u trace=%d\n",
        kPatchStreamRingFrames,
        kPatchStreamColorBytesPerSlot,
        kPatchStreamExtrasBytesPerSlot,
        (int)s_traceOn);
    fflush(stderr);
    return true;
}

void TerrainPatchStream::destroy()
{
    if (!s_killswitch || !s_initOk) return;
    fprintf(stderr, "[PATCH_STREAM v1] event=shutdown\n");
    fflush(stderr);
    s_initOk = false;
}

bool TerrainPatchStream::isReady()       { return s_killswitch && s_initOk; }
bool TerrainPatchStream::isOverflowed()  { return false; }
void TerrainPatchStream::beginFrame()    { /* Task 4 */ }
void TerrainPatchStream::appendTriangle(DWORD, const gos_VERTEX*, const gos_TERRAIN_EXTRA*) { /* Task 3 */ }
bool TerrainPatchStream::flush()         { return false; /* Task 6 */ }
