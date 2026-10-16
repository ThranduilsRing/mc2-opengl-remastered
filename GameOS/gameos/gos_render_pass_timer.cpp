//============================================================================
// gos_render_pass_timer — see gos_render_pass_timer.h.
//============================================================================
#include <GL/glew.h>
#include <GL/gl.h>
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

const int kRing = 4;   // frames in flight; oldest is polled, never waited on

struct Slot {
    GLuint   q[Pass_Count];     // GL_TIME_ELAPSED query per pass (lazy-gen)
    GLuint   tsBegin;           // GL_TIMESTAMP at first pass begin of the frame
    GLuint   tsEnd;             // GL_TIMESTAMP at FrameEnd
    unsigned ranMask;           // bit i set = pass i has a committed scope
    bool     frameOpen;         // tsBegin issued this frame
    bool     pending;           // submitted, awaiting harvest
};

Slot     s_ring[kRing];
int      s_cur = 0;
bool     s_inited = false;
bool     s_envOn = false;       // MC2_RENDER_PASS_TIME=1 (drives the emit line)
bool     s_collect = false;     // runtime collect flag (editor); no emit

// Published last-window mean ms per pass (snapshotted at each emit, survives the
// accumulator reset). Read by the editor Frame Inspector via LastMs/HasSample.
double   s_publishedMs[Pass_Count] = {0};
bool     s_publishedHas[Pass_Count] = {false};

int      s_openPass = -1;       // pass whose scope is currently open (-1 none)
bool     s_openReal = false;    // glBeginQuery actually issued for it

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
        const int n = mc2::gate::get<"MC2_RENDER_PASS_TIME_EVERY">();
        s_every = (n > 0) ? n : 60;
    }
    return s_every;
}

void harvest(Slot& s)
{
    for (int p = 0; p < Pass_Count; ++p) {
        if (!(s.ranMask & (1u << p))) continue;
        GLuint64 ns = 0;
        glGetQueryObjectui64v(s.q[p], GL_QUERY_RESULT, &ns);
        s_sumMs[p] += (double)ns / 1.0e6;
        ++s_samples[p];
    }
    if (s.frameOpen && s.tsBegin && s.tsEnd) {
        GLuint64 t0 = 0, t1 = 0;
        glGetQueryObjectui64v(s.tsBegin, GL_QUERY_RESULT, &t0);
        glGetQueryObjectui64v(s.tsEnd, GL_QUERY_RESULT, &t1);
        if (t1 > t0) s_sumTotalMs += (double)(t1 - t0) / 1.0e6;
    }
    ++s_frames;
    s.pending = false;
    s.ranMask = 0;
    s.frameOpen = false;
}

// Snapshot the window means into the published arrays the editor reads. Called
// from emitWindow before the accumulators are reset, in both paths.
void publishWindow()
{
    const double inv = s_frames ? (1.0 / (double)s_frames) : 0.0;
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
        off += mc2::fmt::to_buf_n(line + off, sizeof(line) - off, "[RENDER_PASS_TIME v1] frame={:d} n={:d} gpu_total={:.2f}", mc2::fmt::as_u32(s_frameNo), mc2::fmt::as_u32(s_frames), s_sumTotalMs / (double)s_frames);
        for (int p = 0; p < Pass_Count; ++p) {
            if (!s_samples[p]) continue;   // pass never ran in window: key absent
            off += mc2::fmt::to_buf_n(line + off, sizeof(line) - off, " {}={:.2f}", mc2::fmt::safe(kPassKey[p]), s_sumMs[p] / (double)s_frames);
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
    s_envOn = mc2::gate::get<"MC2_RENDER_PASS_TIME">();
    s_inited = true;
    if (s_envOn) {
        mc2::fmt::print_out("[RENDER_PASS_TIME v1] armed every={:d} ring={:d}\n", mc2::fmt::as_i32(emitEvery()), mc2::fmt::as_i32(kRing));
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
        s_sumMs[p] += ms[p];
        ++s_samples[p];
        total += ms[p];
    }
    // The VK frame has no single begin/end pair across its islands' submits: gpu_total is the sum of the timed passes.
    s_sumTotalMs += total;
    ++s_frames;
}

void SubmitBackendDropped() { ++s_dropped; }

bool QueryActive()
{
    return Enabled() && s_openPass >= 0 && s_openReal;
}

void Begin(Pass p)
{
    // FramePassStats v1 shares the pass boundaries but is independently gated:
    // capture per-pass GL state even when the GPU timer is OFF (own env check,
    // OFF=zero cost). Done first so the state reflects exactly what the pass
    // is about to render with.
    gos_frame_pass_stats::RecordPassBegin(p);

    if (!Enabled()) return;
    if (vk_render_pass_timer::Active()) return;   // the VK islands time their own passes
    if (p < 0 || p >= Pass_Count) return;
    if (s_openPass >= 0) {
        // Defensive: GL_TIME_ELAPSED cannot nest. Record the open pass so the
        // matching End() is ignored; do not disturb the outer scope.
        return;
    }
    Slot& s = s_ring[s_cur];
    if (s.pending) {
        // Ring lap caught an unharvested slot (GPU far behind): drop its
        // measurements and reuse it rather than stall.
        ++s_dropped;
        s.pending = false;
        s.ranMask = 0;
        s.frameOpen = false;
    }
    if (!s.frameOpen) {
        if (!s.tsBegin) glGenQueries(1, &s.tsBegin);
        if (!s.tsEnd)   glGenQueries(1, &s.tsEnd);
        glQueryCounter(s.tsBegin, GL_TIMESTAMP);
        s.frameOpen = true;
    }
    if (s.ranMask & (1u << p)) {
        // Pass already timed this frame (should not happen with the disjoint
        // placement); keep the first measurement, skip re-begin.
        s_openPass = p;
        s_openReal = false;
        return;
    }
    if (!s.q[p]) glGenQueries(1, &s.q[p]);
    glBeginQuery(GL_TIME_ELAPSED, s.q[p]);
    s_openPass = p;
    s_openReal = true;
}

void End(Pass p)
{
    if (!Enabled()) return;
    if (s_openPass != (int)p) return;   // unmatched / nested-skipped: ignore
    if (s_openReal) {
        glEndQuery(GL_TIME_ELAPSED);
        s_ring[s_cur].ranMask |= (1u << (int)p);
    }
    s_openPass = -1;
    s_openReal = false;
}

void FrameEnd()
{
    if (!Enabled()) return;
    if (vk_render_pass_timer::Active()) {
        ++s_frameNo;
        vk_render_pass_timer::FrameEnd();
        if (s_frameNo % (unsigned long)emitEvery() == 0)
            emitWindow();
        return;
    }
    if (s_openPass >= 0) {
        // Scope leaked across the frame boundary (should not happen): close it
        // so the next frame starts clean.
        if (s_openReal) {
            glEndQuery(GL_TIME_ELAPSED);
            s_ring[s_cur].ranMask |= (1u << s_openPass);
        }
        s_openPass = -1;
        s_openReal = false;
    }
    ++s_frameNo;

    Slot& cur = s_ring[s_cur];
    if (cur.frameOpen) {
        glQueryCounter(cur.tsEnd, GL_TIMESTAMP);
        cur.pending = true;
        s_cur = (s_cur + 1) % kRing;
    }
    // else: no pass ran this frame (menus etc.) — keep the slot, nothing pending.

    // Poll pending slots oldest-first; harvest every slot whose LAST query
    // (tsEnd, issued after all pass queries) is available. Never block.
    for (int i = 1; i <= kRing; ++i) {
        Slot& s = s_ring[(s_cur + i) % kRing];
        if (!s.pending) continue;
        GLint avail = 0;
        glGetQueryObjectiv(s.tsEnd, GL_QUERY_RESULT_AVAILABLE, &avail);
        if (!avail) break;   // younger slots cannot be ready either
        harvest(s);
    }

    if (s_frameNo % (unsigned long)emitEvery() == 0)
        emitWindow();
}

} // namespace gos_render_pass_timer
