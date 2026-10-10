#pragma once
//----------------------------------------------------------------------------
// mc2_diag_verbose.h -- QUIET-DEFAULT-BUILD-1
//
// ONE registered umbrella gate for developer diagnostic chatter that used to
// print UNCONDITIONALLY in a default build.
//
// THE PROBLEM THIS SOLVES
// -----------------------
// VERIFY-INTEGRITY-3 measured 83 distinct `[TAG vN]` diagnostic tags printing
// in a DEFAULT tier1 build -- i.e. output a real user sees on a normal launch
// with no MC2_* env set. Nine of them repeated every 600 frames forever during
// play; three announced features that were switched OFF. The baseline lives in
// tests/contract/default_build_quiet_baseline.json.
//
// WHY ONE GATE AND NOT ONE GATE PER TAG
// -------------------------------------
// 70-odd call sites across ~20 files spanning three link units (GameOS, mclib,
// code). A gate per tag would add ~70 registry entries nobody will ever set,
// and each would need its own plumbing. These lines share ONE purpose -- "show
// me the engine's internal chatter" -- so they share ONE switch. Per-subsystem
// facilities (MC2_TERRAIN_LOD_CHUNK_DIAG bits, MC2_DIAG_TAGS) stay the right
// answer for anything that needs selective, structured, or per-bit control;
// this is the blanket for the undifferentiated remainder.
//
// WHY HEADER-ONLY
// ---------------
// GameOS/include is on the include path of every target that needs this
// (COMMON_INCLUDE_DIRS), but GameOS, mclib and code are separate link units
// with no shared utility library. A header-only inline costs one getenv per
// translation unit at first use and needs no CMake change -- no new source
// file to add to the right target, no link-order risk.
//
// USAGE
//   #include "mc2_diag_verbose.h"
//   MC2_DIAGF("[SOME_TAG v1] event=thing x=%d\n", x);
//
// Default OFF => the printf is not reached. Set MC2_DIAG_VERBOSE=1 to restore
// the old behaviour verbatim.
//
// NOT for: anything a harness parses. Tags consumed by scripts/smoke_lib/*
// (BUILD_FINGERPRINT, INSTR, PERF, TIMING, TGL_POOL, ASSET_SCALE,
// ASSET_LOAD, RENDER_SNAPSHOT, QUADSETUP_SKIP, SMOKE) must keep printing or
// verification goes blind. Those are recorded as attributed debt in the quiet
// baseline with a `keep` reason.
//----------------------------------------------------------------------------

#include <cstdio>
#include "Gates/gate.h"
#include <cstdlib>

// True when MC2_DIAG_VERBOSE is set to something other than "0".
// Read once per translation unit; never re-read (matches the house
// getenv-seeded-static convention, so behaviour cannot drift mid-run).
inline bool mc2DiagVerbose()
{
    static const bool s_on = mc2::gate::get<"MC2_DIAG_VERBOSE">();
    return s_on;
}

// Drop-in replacement for a `printf("[TAG vN] ...")` diagnostic line.
// The argument list is IDENTICAL to the printf it replaces -- the rewrite is a
// one-token change, which keeps the merge surface minimal for the several
// lanes editing these files concurrently.
#define MC2_DIAGF(...)                                                        \
    do {                                                                      \
        if (mc2DiagVerbose()) {                                               \
            std::printf(__VA_ARGS__);                                         \
        }                                                                     \
    } while (0)

// Same, for the `fprintf(stderr, "[TAG vN] ...")` spelling. The stream stays
// the first argument, so this too is a one-token rewrite.
#define MC2_DIAGFE(...)                                                       \
    do {                                                                      \
        if (mc2DiagVerbose()) {                                               \
            std::fprintf(__VA_ARGS__);                                        \
        }                                                                     \
    } while (0)

// A third house idiom formats into a local buffer with snprintf and then
// pushes it through puts/fputs. The snprintf itself is harmless (it writes to
// a stack buffer, not the console), so the SINK is what gets gated -- that
// keeps the rewrite to one token here as well.
#define MC2_DIAG_PUTS(...)                                                    \
    do {                                                                      \
        if (mc2DiagVerbose()) {                                               \
            std::puts(__VA_ARGS__);                                           \
        }                                                                     \
    } while (0)

#define MC2_DIAG_FPUTS(...)                                                   \
    do {                                                                      \
        if (mc2DiagVerbose()) {                                               \
            std::fputs(__VA_ARGS__);                                          \
        }                                                                     \
    } while (0)
