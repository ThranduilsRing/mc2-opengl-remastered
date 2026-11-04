//============================================================================
// gos_frame_pass_stats — see gos_frame_pass_stats.h.
//============================================================================
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gos_frame_pass_stats.h"
#include "gos_render_pass_timer.h"
#include "Gates/gate.h"
#include "mc2fmt.h"

namespace gos_frame_pass_stats {

using gos_render_pass_timer::Pass_Count;

namespace {

// Stable emit keys, index == gos_render_pass_timer::Pass. Kept identical to the
// timer's kPassKey so both telemetry streams line up. Never reorder.
const char* const kPassKey[Pass_Count] = {
    "shadowStatic",
    "shadowDyn",
    "obj3d",
    "terrainChunk",
    "terrainSolid",
    "spColor",
    "mechs",
    "overlays",
    "water",
    "blobs",
    "alphaVfx",
    "hud",
    "post",
};

PassRow         s_rows[Pass_Count];   // current frame (filling)
PassRow         s_last[Pass_Count];   // last completed frame (for consumers)
FrameAggregates s_agg;                // current frame (filling)
FrameAggregates s_lastAgg;            // last completed frame

bool s_inited  = false;
bool s_envOn   = false;   // MC2_FRAME_PASS_STATS=1 (drives the emit line)
bool s_collect = false;   // runtime collect flag (editor tab); no emit

int emitEvery()
{
    static int s_every = 0;
    if (s_every == 0) {
        const int n = 0;
        s_every = (n > 0) ? n : 60;
    }
    return s_every;
}

void emitLine(unsigned long frameNo)
{
    char line[2048];
    int off = 0;
    off += mc2::fmt::to_buf_n(line + off, sizeof(line) - off, "[FRAME_PASS_STATS v1] frame={:d} chunks={:d} spBatches={:d} mechInst={:d} vfx={:d}", mc2::fmt::as_u32(frameNo), mc2::fmt::as_u32(s_lastAgg.visibleTerrainChunks), mc2::fmt::as_u32(s_lastAgg.staticPropBatches), mc2::fmt::as_u32(s_lastAgg.mechBatchInstances), mc2::fmt::as_u32(s_lastAgg.vfxCount));
    for (int p = 0; p < Pass_Count; ++p) {
        const PassRow& r = s_last[p];
        if (!r.ran) continue;   // pass never ran this frame: key absent
        off += mc2::fmt::to_buf_n(line + off, sizeof(line) - off, " {}{{draws={:d} inst={:d}}}", mc2::fmt::safe(kPassKey[p]), mc2::fmt::as_u32(r.drawCount), mc2::fmt::as_u32(r.instanceCount));
        if (off >= static_cast<int>(sizeof(line)) - 96) break;   // guard truncation
    }
    mc2::fmt::print_out("{}\n", mc2::fmt::safe(line));
    std::fflush(stdout);
}

} // namespace

// Init the cached env gate once. Separate from Enabled() so the emit path can
// query env-state independently of the runtime collect flag.
static void initEnv()
{
    if (s_inited) return;
    s_envOn = false;
    s_inited = true;
    if (s_envOn) {
        mc2::fmt::print_out("[FRAME_PASS_STATS v1] armed every={:d}\n", mc2::fmt::as_i32(emitEvery()));
        std::fflush(stdout);
    }
}

bool Enabled()
{
    initEnv();
    // Collection runs when the env arms it OR the editor tab flips the runtime
    // flag. Game build with env unset + tab never opened keeps both false.
    return s_envOn || s_collect;
}

void SetCollect(bool on) { initEnv(); s_collect = on; }
bool CollectFlag()       { return s_collect; }

void MarkPassRan(gos_render_pass_timer::Pass p)
{
    if (!Enabled()) return;
    const int pi = static_cast<int>(p);
    if (pi < 0 || pi >= Pass_Count) return;
    s_rows[pi].ran = true;
}

void SetFrameAggregates(const FrameAggregates& a)
{
    if (!Enabled()) return;
    // Preserve the chunk count set by the terrain flush producer (the seam
    // does not know it); the seam fills the other three from accessors.
    const uint32_t chunks = s_agg.visibleTerrainChunks;
    s_agg = a;
    if (a.visibleTerrainChunks == 0u)
        s_agg.visibleTerrainChunks = chunks;
}

void SetVisibleTerrainChunks(uint32_t chunks)
{
    if (!Enabled()) return;
    s_agg.visibleTerrainChunks = chunks;
}

void SetPassCounts(gos_render_pass_timer::Pass p,
                   uint32_t drawCount, uint32_t instanceCount)
{
    if (!Enabled()) return;
    const int pi = static_cast<int>(p);
    if (pi < 0 || pi >= Pass_Count) return;
    s_rows[pi].drawCount     = drawCount;
    s_rows[pi].instanceCount = instanceCount;
}

void FrameEnd(unsigned long frameNo)
{
    if (!Enabled()) return;

    // Promote this frame's rows to the "last completed" snapshot consumers read.
    std::memcpy(s_last, s_rows, sizeof(s_last));
    s_lastAgg = s_agg;

    // Emit cadence keys off the ENV gate only. When collection is active purely
    // because the editor tab set the runtime flag (env unset), no [FRAME_PASS_
    // STATS v1] lines are produced -- the tab reads s_last directly. Keeps the
    // emit format and game-OFF "zero lines" guarantee intact.
    if (s_envOn && emitEvery() > 0 && frameNo % static_cast<unsigned long>(emitEvery()) == 0)
        emitLine(frameNo);

    // Reset for the next frame.
    for (int p = 0; p < Pass_Count; ++p)
        s_rows[p] = PassRow{};
    s_agg = FrameAggregates{};
}

const PassRow& GetPassRow(int passIndex)
{
    static const PassRow s_empty{};
    if (passIndex < 0 || passIndex >= Pass_Count) return s_empty;
    return s_last[passIndex];
}

const FrameAggregates& GetFrameAggregates() { return s_lastAgg; }

int PassCount() { return Pass_Count; }

const char* PassKey(int passIndex)
{
    if (passIndex < 0 || passIndex >= Pass_Count) return "";
    return kPassKey[passIndex];
}

} // namespace gos_frame_pass_stats
