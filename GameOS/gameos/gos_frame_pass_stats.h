#pragma once
//============================================================================
// gos_frame_pass_stats — FramePassStats v1 ([FRAME_PASS_STATS v1]).
//
// ONE per-frame, per-pass advisory stats collector. Pairs with the GPU pass
// timers (gos_render_pass_timer, MC2_RENDER_PASS_TIME) — same Pass enum, same
// pass boundaries — but instead of GPU wall-time it records per-pass GL state
// (FBO id, viewport, depth test/mask, blend, cull) plus per-pass draw/instance
// counts and a handful of frame-level aggregates (visible terrain chunks,
// static-prop batches, mech batch instances, VFX count).
//
// Substrate ONLY: feeds (a) a [FRAME_PASS_STATS v1] summary line every
// MC2_FRAME_PASS_STATS_EVERY frames (default 60) and (b) an additive
// framePassStats object in the MC2_DEBUG_STATE_DUMP JSON. No render behavior
// change. No per-element hot-loop counting — frame-level fields are pulled
// from already-computed aggregates at the gameosmain snapshot seam. The
// ImGui "Frame Inspector" tab is the next consumer (deferred).
//
// Enable: MC2_FRAME_PASS_STATS=1. Default OFF = zero cost: no log lines, no JSON addition (lazy init on first enabled frame; every
// entry point early-returns on the cached env check).
//
// Pattern = gos_render_pass_timer + TERRAIN-PASS-PACKET-0.
//============================================================================

#include <cstdint>

#include "gos_render_pass_timer.h"   // gos_render_pass_timer::Pass enum

namespace gos_frame_pass_stats {

// Per-pass row. Mirrors the Pass enum index from gos_render_pass_timer.
struct PassRow {
    bool     ran = false;        // the GPU pass timer measured this pass
    uint32_t drawCount = 0u;     // pass draw calls (from aggregates; 0 if unknown)
    uint32_t instanceCount = 0u; // pass instances (from aggregates; 0 if unknown)
};

// Collection is active when (MC2_FRAME_PASS_STATS=1) OR the in-process runtime
// collect flag is set (see SetCollect). All other entry points early-return
// when this is false. Default OFF in the game (env unset + flag false) = zero
// cost. The editor Frame Inspector tab flips the runtime flag on while visible.
bool Enabled();

// In-process runtime collect flag. Lets the editor Frame Inspector tab turn
// collection on while open (env unset) and off when hidden, WITHOUT enabling
// the [FRAME_PASS_STATS v1] emit cadence path in the game. Game build with the
// env unset never calls this, so the flag stays false = unchanged. Does NOT
// alter the emit-line behavior (that still keys off the env, see FrameEnd).
void SetCollect(bool on);
bool CollectFlag();

// The GPU pass timer reports pass p ran in a harvested frame. Ignored when OFF.
void MarkPassRan(gos_render_pass_timer::Pass p);

// Frame-level aggregates, filled once per frame at the gameosmain snapshot
// seam from already-computed counters (no hot-loop counting).
struct FrameAggregates {
    uint32_t visibleTerrainChunks = 0u;
    uint32_t staticPropBatches    = 0u;
    uint32_t mechBatchInstances   = 0u;
    uint32_t vfxCount             = 0u;
};
void SetFrameAggregates(const FrameAggregates& a);

// Producer-side setter for the visible terrain chunk count (set where the
// chunk draw commands are built — terrain.cpp flush — the authoritative
// source). Merged with the seam-filled aggregates. Ignored when OFF.
void SetVisibleTerrainChunks(uint32_t chunks);

// Optional per-pass draw/instance counts from aggregates (e.g. terrain chunk
// cmds, mech submit count, static-prop slots). Ignored when OFF.
void SetPassCounts(gos_render_pass_timer::Pass p,
                   uint32_t drawCount, uint32_t instanceCount);

// Once per frame at FrameEnd (after gos_render_pass_timer::FrameEnd): emits
// the summary line at cadence, then resets the per-frame rows.
void FrameEnd(unsigned long frameNo);

// Read-only accessors for downstream consumers (JSON dump, future ImGui tab).
// Reflect the LAST completed frame. Safe to call when OFF (rows are zeroed).
const PassRow&         GetPassRow(int passIndex);
const FrameAggregates& GetFrameAggregates();
int                    PassCount();
const char*            PassKey(int passIndex);

} // namespace gos_frame_pass_stats
