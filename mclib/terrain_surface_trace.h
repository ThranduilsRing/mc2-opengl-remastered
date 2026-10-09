#pragma once
// [TERRAIN_SURFACE v1] -- env-gated lifecycle instrumentation channel for the
// terrain continuous-surface producer (plan
// docs/superpowers/plans/2026-05-18-terrain-continuous-surface-producer-plan.md,
// PR-0 / Wave 0).
//
// This header is the DORMANT channel. PR-0 adds ONLY the channel + its env
// gate + the [INSTR v1] banner field. It changes NO terrain behavior and
// emits NOTHING by itself (no TS_TRACE call sites land in PR-0). Later
// producer PRs (PR-1 mission-load generation, PR-2/PR-3 surface draw, PR-4
// fallback/teardown) include this header and land their lifecycle prints in
// the SAME commit as their behavior change (debug_instrumentation_rule.md:
// instrumentation ships with the rework, not as a follow-up).
//
// Idiom: matches the canonical [REVERSE_Z v1]
// pattern -- a file-scope-cached getenv() bool + a do/while(0) macro guard.
// Gate MC2_TERRAIN_SURFACE_TRACE is DEFAULT-OFF (silent unless the env var is
// set). It is SEPARATE from the MC2_TERRAIN_SURFACE path-select kill-switch
// (plan Section F): this is trace-only, never selects a code path.
//
// Discipline (cost_split_instrumentation_is_observer_effect_dominated.md):
// LIFECYCLE BOUNDARIES ONLY (init / register / first-use / teardown /
// fallback). 100 ns floor honored -- coarse once-per-phase / once-per-frame
// zones ONLY. NEVER per-quad / per-vertex (per-quad std::chrono fabricated
// ~5 ms of fake setup_total in the cost-split campaign). Demote-not-delete:
// once a producer PR is validated, leave its TS_TRACE calls in-tree, gated
// off -- do not strip them.
//
// Schema regex (grep-friendly, schema-versioned): \[TERRAIN_SURFACE v[0-9]+\]
// Line format: [TERRAIN_SURFACE v1] event=<name> <key=val> ... -- structured
// one-liner, not paragraphs.

#include <cstdlib>
#include <cstdio>

namespace mc2_terrain_surface_trace {

// Cached once on first query (the env var is read once; flipping it
// mid-process has no effect, mirroring the existing trace gates).
inline bool enabled()
{
    static const bool s_enabled = (getenv("MC2_TERRAIN_SURFACE_TRACE") != nullptr);
    return s_enabled;
}

} // namespace mc2_terrain_surface_trace

// TS_TRACE(fmt, ...) -- env-gated, silent by default, grep-friendly.
// Emits to stderr (unbuffered relative to stdout interleave; matches the
// [REVERSE_Z v1] lifecycle-print precedent) only when the gate is set.
// fflush guarantees the line survives a GPU hang for postmortem.
#define TS_TRACE(fmt, ...)                                                     \
    do {                                                                       \
        if (::mc2_terrain_surface_trace::enabled()) {                          \
            fprintf(stderr, "[TERRAIN_SURFACE v1] " fmt "\n", ##__VA_ARGS__);   \
            fflush(stderr);                                                    \
        }                                                                      \
    } while (0)
