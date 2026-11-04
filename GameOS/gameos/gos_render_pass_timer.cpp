//============================================================================
// gos_render_pass_timer — see gos_render_pass_timer.h.
//============================================================================
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gos_render_pass_timer.h"
#include "gos_frame_pass_stats.h"
#include "vk_render_pass_timer.h"
#include "Gates/gate.h"
#include "mc2fmt.h"

namespace gos_render_pass_timer {

namespace {

// Stable emit keys, index == Pass enum. Never reorder (smoke scripts regex them).
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

bool     s_inited = false;
bool     s_envOn = false;       // MC2_RENDER_PASS_TIME=1 (drives the emit line)
bool     s_collect = false;     // runtime collect flag (editor); no emit

// Published last-window mean ms per pass (snapshotted at each emit, survives the
// accumulator reset). Read by the editor Frame Inspector via LastMs/HasSample.
double   s_publishedMs[Pass_Count] = {0};
bool     s_publishedHas[Pass_Count] = {false};

// Window accumulators (reset at each emit).
double   s_sumMs[Pass_Count] = {0};
unsigned s_samples[Pass_Count] = {0};
double   s_sumTotalMs = 0.0;
unsigned s_frames = 0;          // harvested frames in window
unsigned s_dropped = 0;
unsigned s_lastDropped = 0;     // s_dropped of the last emitted window (LastDropped)
unsigned long s_frameNo = 0;    // monotonic FrameEnd count

int emitEvery()
{
    static int s_every = 0;
    if (s_every == 0) {
        const int n = 0;
        s_every = (n > 0) ? n : 60;
    }
    return s_every;
}

// Snapshot the window means into the published arrays the editor reads. Called
// from emitWindow before the accumulators are reset, in both paths.
void publishWindow()
{
    const double inv = s_frames ? (1.0 / static_cast<double>(s_frames)) : 0.0;
    for (int p = 0; p < Pass_Count; ++p) {
        if (s_samples[p]) {
            s_publishedMs[p]  = s_sumMs[p] * inv;
            s_publishedHas[p] = true;
        } else {
            s_publishedMs[p]  = 0.0;
            s_publishedHas[p] = false;
        }
    }
}

void emitWindow()
{
    publishWindow();
    s_lastDropped = s_dropped;
    if (s_frames == 0) {
        // No slot harvested this window (GPU results never became available).
        // Heartbeat so silence is diagnosable (ENV-gated only), then reset.
        if (s_envOn) {
            mc2::fmt::print_out("[RENDER_PASS_TIME v1] frame={:d} n=0 dropped={:d}\n", mc2::fmt::as_u32(s_frameNo), mc2::fmt::as_u32(s_dropped));
            std::fflush(stdout);
        }
        s_dropped = 0;
        return;
    }
    // The telemetry line is gated on the ENV alone -- the runtime collect flag
    // drives harvesting (so the editor can read ms) but never emits stdout.
    if (s_envOn) {
        char line[1024];
        int off = 0;
        off += mc2::fmt::to_buf_n(line + off, sizeof(line) - off, "[RENDER_PASS_TIME v1] frame={:d} n={:d} gpu_total={:.2f}", mc2::fmt::as_u32(s_frameNo), mc2::fmt::as_u32(s_frames), s_sumTotalMs / static_cast<double>(s_frames));
        for (int p = 0; p < Pass_Count; ++p) {
            if (!s_samples[p]) continue;   // pass never ran in window: key absent
            off += mc2::fmt::to_buf_n(line + off, sizeof(line) - off, " {}={:.2f}", mc2::fmt::safe(kPassKey[p]), s_sumMs[p] / static_cast<double>(s_frames));
        }
        if (s_dropped)
            off += mc2::fmt::to_buf_n(line + off, sizeof(line) - off, " dropped={:d}", mc2::fmt::as_u32(s_dropped));
        mc2::fmt::print_out("{}\n", mc2::fmt::safe(line));
        std::fflush(stdout);
    }

    std::memset(s_sumMs, 0, sizeof(s_sumMs));
    std::memset(s_samples, 0, sizeof(s_samples));
    s_sumTotalMs = 0.0;
    s_frames = 0;
    s_dropped = 0;
}

} // namespace

static void initEnv()
{
    if (s_inited) return;
    s_envOn = false;
    s_inited = true;
    if (s_envOn) {
        mc2::fmt::print_out("[RENDER_PASS_TIME v1] armed every={:d}\n", mc2::fmt::as_i32(emitEvery()));
        std::fflush(stdout);
    }
}

bool Enabled()
{
    initEnv();
    // Collection runs when the env arms it OR the editor flips the runtime flag.
    // Game build with env unset + window never opened keeps both false.
    return s_envOn || s_collect;
}

void SetCollect(bool on) { initEnv(); s_collect = on; }

double LastMs(Pass p)
{
    if (p < 0 || p >= Pass_Count) return 0.0;
    return s_publishedMs[p];
}

bool HasSample(Pass p)
{
    if (p < 0 || p >= Pass_Count) return false;
    return s_publishedHas[p];
}

unsigned LastDropped() { return s_lastDropped; }

void SubmitBackendFrame(const double ms[Pass_Count], const bool ran[Pass_Count])
{
    double total = 0.0;
    for (int p = 0; p < Pass_Count; ++p) {
        if (!ran[p]) continue;
        gos_frame_pass_stats::MarkPassRan(static_cast<Pass>(p));
        s_sumMs[p] += ms[p];
        ++s_samples[p];
        total += ms[p];
    }
    // The VK frame has no single begin/end pair across its islands' submits: gpu_total is the sum of the timed passes.
    s_sumTotalMs += total;
    ++s_frames;
}

void SubmitBackendDropped() { ++s_dropped; }

void FrameEnd()
{
    if (!Enabled()) return;
    if (!vk_render_pass_timer::Active()) return;   // the Vulkan islands are the only time source
    ++s_frameNo;
    vk_render_pass_timer::FrameEnd();
    if (s_frameNo % static_cast<unsigned long>(emitEvery()) == 0)
        emitWindow();
}

} // namespace gos_render_pass_timer
