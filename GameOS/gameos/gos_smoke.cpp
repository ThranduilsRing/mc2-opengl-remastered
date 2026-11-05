// GameOS/gameos/gos_smoke.cpp
#include "mc2_czstring.h"
#include "gos_smoke.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>
#ifndef S_ISREG
#define S_ISREG(x) ((_S_IFREG & (x)) != 0)
#endif

#include <SDL2/SDL.h>
#include "Gates/gate.h"
#include "Gates/gate_cold.h"   // str<>()
#include "mc2fmt.h"
#include "diagnostic_trace.h"
#include "hitch_stats.h"
#include "vulkan_context.h"
#include "platform_windows.h"
#ifndef LINUX_BUILD
#include <tlhelp32.h>
#endif

namespace {
// Host load stamped into the FIRST60 line so a perf verdict taken on a busy machine can be discounted: CPU busy percent of everything except this
// process between two samples, and how many other mc2.exe processes were running.
struct CpuSample { unsigned long long idle = 0, total = 0, self = 0; };
#ifdef LINUX_BUILD
CpuSample sampleCpu() { return {}; }   // host-load stamp is Windows-only
#else
CpuSample sampleCpu() {
    FILETIME i, k, u, c, e, sk, su;
    CpuSample s;
    auto q = [](const FILETIME& f) { return (static_cast<unsigned long long>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    if (GetSystemTimes(&i, &k, &u)) {
        s.idle = q(i);
        s.total = q(k) + q(u);   // kernel time includes idle
    }
    if (GetProcessTimes(GetCurrentProcess(), &c, &e, &sk, &su)) s.self = q(sk) + q(su);
    return s;
}
#endif
double cpuBusyPct(const CpuSample& a, const CpuSample& b) {
    const unsigned long long dt = b.total - a.total, di = b.idle - a.idle, ds = b.self - a.self;
    if (!dt) return -1.0;
    const double busy = static_cast<double>(dt - di), mine = static_cast<double>(ds);
    return 100.0 * (busy > mine ? busy - mine : 0.0) / static_cast<double>(dt);
}
#ifdef LINUX_BUILD
std::optional<int> otherMc2Count() { return std::nullopt; }
void startOtherMc2Sampler() {}
#else
std::optional<int> otherMc2Count() {
    int n = 0;
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return std::nullopt;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    const DWORD self = GetCurrentProcessId();
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (pe.th32ProcessID != self && _wcsicmp(pe.szExeFile, L"mc2.exe") == 0) ++n;
    CloseHandle(snap);
    return n;
}
// The process snapshot costs ms, so with HITCH requested a detached sampler refreshes the count once per second for the slot-wait split.
void startOtherMc2Sampler() {
    static std::atomic<bool> started{false};
    if (started.exchange(true)) return;
    std::thread([] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
        for (;;) {
            mc2_hitch2::stats().otherMc2.store(otherMc2Count().value_or(-1), std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }).detach();
}
#endif
}  // namespace

namespace {
SmokeMode::State g_state;
uint64_t g_startupT0 = 0;   // SDL_GetPerformanceCounter at parseArgs entry
uint64_t g_freq = 0;
uint64_t g_missionReadyT = 0;
std::atomic<bool> g_atexitInstalled{false};
std::atomic<bool> g_summaryEmitted{false};

std::vector<double> g_frameMs;          // ring buffer, size = cap
size_t              g_frameIdx = 0;
size_t              g_frameCount = 0;   // monotonic
size_t              g_frameMsCap = 8192;
bool                g_firstFrameSeen = false;
double              g_simSeconds = 0.0;
bool                g_simExtendLogged = false;
uint32_t            g_fixedSimFrame = 0;     // S9D: sim-frames where fixed step advanced

double elapsedMsSince(uint64_t t0) {
    if (!t0 || !g_freq) return 0.0;
    uint64_t now = SDL_GetPerformanceCounter();
    return 1000.0 * static_cast<double>((now - t0)) / static_cast<double>(g_freq);
}
} // namespace

namespace SmokeMode {

const State& state() { return g_state; }

void parseArgs(int argc, char** argv) {
    g_freq = SDL_GetPerformanceFrequency();
    g_startupT0 = SDL_GetPerformanceCounter();

    g_state.enabled = mc2::gate::get<"MC2_SMOKE_MODE">();

    // S9D deterministic clock. Opt-in and SEPARATE from MC2_SMOKE_MODE so
    // default smoke timing is unchanged until this is proven. Parsed once;
    // read via fixedTimestepEnabled() (single cached bool) on hot paths.
    g_state.fixedTimestep = mc2::gate::get<"MC2_SMOKE_FIXED_TIMESTEP">();
    if (g_state.fixedTimestep) {
        mc2::fmt::print_err("[SMOKE_FIXED_TIMESTEP v1] dt_ms=33.333 fps=30\n");
        std::fflush(stderr);
    }

    if (mc2::gate::isSet<"MC2_SMOKE_SEED">()) {
        const std::string seedText = mc2::gate::str<"MC2_SMOKE_SEED">();   // "" when set but empty
        char* end = nullptr;
        unsigned long v = std::strtoul(seedText.c_str(), &end, 0);
        if (end != seedText.c_str()) g_state.seed = static_cast<uint32_t>(v);
    }

    // [[noreturn]] on lambdas requires C++23; use a local static function instead.
    struct ParseArgsFail {
        [[noreturn]] static void missingValue(mc2::czstring flag) {
            mc2::fmt::print_out("[SMOKE v1] event=summary result=fail reason=argv_error stage=parseArgs detail=\"{} requires value\"\n", mc2::fmt::safe(flag));
            std::fflush(stdout);
            std::exit(2);
        }
    };
    const auto failMissingValue = ParseArgsFail::missingValue;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--mission") == 0) {
            if (i + 1 >= argc) failMissingValue("--mission");
            g_state.mission = argv[++i];
        } else if (std::strcmp(a, "--profile") == 0) {
            if (i + 1 >= argc) failMissingValue("--profile");
            g_state.profile = argv[++i];
        } else if (std::strcmp(a, "--duration") == 0) {
            if (i + 1 >= argc) failMissingValue("--duration");
            const char* s = argv[++i];
            char* end = nullptr;
            long v = std::strtol(s, &end, 10);
            if (end == s || *end != '\0' || v <= 0 || v > 86400) {
                failMissingValue("--duration (must be integer 1..86400)");
            }
            g_state.durationSec = static_cast<int>(v);
        } else if (std::strcmp(a, "--headless") == 0) {
            g_state.headless = true;
        } else if (std::strcmp(a, "--ticks") == 0) {
            if (i + 1 >= argc) failMissingValue("--ticks");
            const char* s = argv[++i];
            char* end = nullptr;
            long v = std::strtol(s, &end, 10);
            if (end == s || *end != '\0' || v <= 0 || v > 10000000) {
                failMissingValue("--ticks (must be integer 1..10000000)");
            }
            g_state.tickBudget = static_cast<int>(v);
        } else if (std::strcmp(a, "--smoke-active") == 0) {
            g_state.active = true;
        }
        // Unknown flags pass through to other parsers (GameOS, validate, etc.).
    }

    if (g_state.enabled && g_state.mission.empty()) {
        mc2::fmt::print_out("[SMOKE v1] event=summary result=fail reason=argv_error stage=parseArgs detail=\"--mission required when MC2_SMOKE_MODE is set\"\n");
        std::fflush(stdout);
        std::exit(2);
    }
    if (!g_state.enabled && !g_state.mission.empty()) {
        // Smoke args without env -- refuse.
        mc2::fmt::print_out("[SMOKE v1] event=summary result=fail reason=argv_error stage=parseArgs detail=\"--mission requires MC2_SMOKE_MODE to be set\"\n");
        std::fflush(stdout);
        std::exit(2);
    }
    if (g_state.enabled) {
        mc2::fmt::print_out("[SMOKE v1] event=banner mode={} mission={} profile={} duration={:d} seed=0x{:x}\n",
            mc2::fmt::safe(g_state.active ? "active" : "passive"),
            g_state.mission.c_str(), g_state.profile.c_str(),
            mc2::fmt::as_i32(g_state.durationSec), mc2::fmt::as_u32(g_state.seed));
        std::fflush(stdout);
    }
}

void installAtexitSummary() {
    if (g_atexitInstalled.exchange(true)) return;
    std::atexit([]{
        if (!g_state.enabled) return;
        if (g_summaryEmitted.load()) return;
        // Best-effort: if we reach atexit without an explicit summary, we
        // assume something failed after parseArgs but before emitCleanSummary.
        mc2::fmt::print_out("[SMOKE v1] event=summary result=fail reason=early_exit stage=atexit\n");
        std::fflush(stdout);
    });
}

void emitTiming(mc2::czstring eventName) {
    if (!g_state.enabled) return;
    double ms = elapsedMsSince(g_startupT0);
    mc2::fmt::print_out("[TIMING v1] event={} elapsed_ms={:.1f}\n", mc2::fmt::safe(eventName), ms);
    std::fflush(stdout);
}

void samplePerf(double frameMs) {
    // First 60 frames after mission_ready, every run (smoke mode or not): one [FIRST60 v1] line (only under MC2_DIAG_TAGS=FIRST60; via_menu.py sets it), so a via-menu run reports the frames a
    // player sees first. Printed once; stdout is live only under MC2_LOG, as for every other line.
    {
        static constexpr int kN = 60;
        static double s_ms[kN];
        static int s_n = 0;
        static double s_loadFrameMs = 0.0;
        static bool s_loadFrameSeen = false;
        static CpuSample s_cpu0;
        static mc2_hitch2::Snapshot s_base;   // every counter as it stood when the window opened: the window is now minus this
        static int s_otherMc2 = -1;
        if (g_missionReadyT && !s_loadFrameSeen) {
            // The frame that was running when mission_ready fired contains the whole mission load; it is reported apart, not in the window.
            s_loadFrameSeen = true;
            s_loadFrameMs = frameMs;
            s_cpu0 = sampleCpu();
            s_base = mc2_hitch2::takeSnapshot();
            mc2_hitch2::beginWaitWindow();
            s_otherMc2 = otherMc2Count().value_or(-1);   // the process snapshot costs ms: take it here, not in a window frame
        } else if (g_missionReadyT && s_n < kN) {
            s_ms[s_n++] = frameMs;
            if (s_n == kN && mc2_diag::tagRequested("FIRST60")) {
                std::vector<double> sorted(s_ms, s_ms + kN);
                std::sort(sorted.begin(), sorted.end());
                double sum = 0.0;
                for (double v : sorted) sum += v;
                mc2::fmt::print_out("[FIRST60 v1] n={:d} avg_ms={:.2f} p50_ms={:.2f} p99_ms={:.2f} max_ms={:.2f} first_ms={:.2f} load_frame_ms={:.2f}\n",
                    mc2::fmt::as_i32(kN), sum / kN, sorted[kN / 2], sorted[kN - 2], sorted[kN - 1], s_ms[0], s_loadFrameMs);
                // v2 tail: the five worst frames (index:ms), the bake cache state the window ran under, and the GPU, so a cold-cache 2070 run is not mistaken for a regression.
                int worst[5];
                int order[kN];
                for (int i = 0; i < kN; ++i) order[i] = i;
                std::partial_sort(order, order + 5, order + kN, [&](int a, int b) { return s_ms[a] > s_ms[b]; });
                for (int i = 0; i < 5; ++i) worst[i] = order[i];
                std::string w;
                for (int i = 0; i < 5; ++i) w += std::to_string(worst[i]) + ":" + std::to_string(static_cast<int>(s_ms[worst[i]] * 10.0) / 10.0).substr(0, 6) + (i < 4 ? "," : "");
                auto& hs = mc2_hitch2::stats();
                // Window numbers are the 60 frames after the load frame (now minus the snapshot taken at the load frame); load_* is everything before it.
                const mc2_hitch2::Snapshot now = mc2_hitch2::takeSnapshot();
                const auto& one = hs.wait[mc2_hitch2::kOneShot];
                const auto& slot = hs.wait[mc2_hitch2::kSlot];
                mc2::fmt::print_out("[FIRST60 v2] worst5={} bake_cache={} bake_state={} waitidle_n={:d} waitidle_ms={:.2f} fence_n={:d} fence_ms={:.2f} fence_max_ms={:.2f} behind_upload_ms={:.2f} slot_n={:d} slot_ms={:.2f} slot_max_ms={:.2f} staged_kb={:d} load_waitidle_ms={:.2f} load_fence_ms={:.2f} load_slot_ms={:.2f} load_staged_kb={:d} host_cpu_pct={:.0f} other_mc2={:d} device={}\n", mc2::fmt::safe(w.c_str()),
                    mc2::fmt::safe(hs.bakeCache.load()), mc2::fmt::safe(hs.bakeState.load()), mc2::fmt::as_i32(static_cast<int>(now.idleN - s_base.idleN)),
                    (now.idleNs - s_base.idleNs) / 1e6, mc2::fmt::as_i32(static_cast<int>(now.waitN[0] - s_base.waitN[0])), (now.waitNs[0] - s_base.waitNs[0]) / 1e6, one.windowMaxNs.load() / 1e6,
                    (now.behindNs - s_base.behindNs) / 1e6, mc2::fmt::as_i32(static_cast<int>(now.waitN[1] - s_base.waitN[1])), (now.waitNs[1] - s_base.waitNs[1]) / 1e6, slot.windowMaxNs.load() / 1e6,
                    mc2::fmt::as_i32(static_cast<int>((now.staged - s_base.staged) / 1024)), s_base.idleNs / 1e6, s_base.waitNs[0] / 1e6, s_base.waitNs[1] / 1e6,
                    mc2::fmt::as_i32(static_cast<int>(s_base.staged / 1024)), cpuBusyPct(s_cpu0, sampleCpu()), mc2::fmt::as_i32(s_otherMc2), mc2::fmt::safe(mc2vk::getOrCreateEngineVulkanContext().health().deviceName));
                std::fflush(stdout);
            }
        }
    }
    // Per-hitch frame record (diag tag HITCH): a frame over twice the running median (and over 12 ms) names its worst scopes and the counter deltas since the last frame.
    {
        auto& hs = mc2_hitch2::stats();
        static std::vector<float> s_all;
        static uint64_t s_frame = 0, s_wic = 0, s_win = 0, s_pc = 0, s_ub = 0, s_om = 0, s_db = 0, s_sj = 0, s_fc = 0, s_fn = 0, s_sb = 0, s_sl = 0, s_aq = 0, s_cw = 0;
        const uint64_t wic = hs.waitIdleCount.load(), win = hs.waitIdleNs.load(), pc = hs.pipelineCreates.load(), ub = hs.uploadBytes.load(),
                       om = hs.objMapRecords.load(), db = hs.dirtyBatches.load(), sj = hs.simJoinNs.load(),
                       fc = hs.wait[0].count.load() + hs.wait[1].count.load(), fn = hs.wait[0].ns.load() + hs.wait[1].ns.load(), sb = hs.stagedBytes.load(),
                       sl = hs.wait[mc2_hitch2::kSlot].ns.load(), aq = hs.acquireNs.load(), cw = hs.compositeWaitNs.load();
        auto& sc = mc2_hitch2::scopes();
        if (g_missionReadyT) {
            ++s_frame;
            if (mc2_diag::tagRequested("HITCH")) startOtherMc2Sampler();
            // Frames 0-29 have no stable median yet: they are recorded at an absolute 25 ms, so the first post-ready (bake upload) frames get a line.
            if (frameMs >= 12.0 && mc2_diag::tagRequested("HITCH")) {
                std::vector<float> tmp(s_all);
                double med = 0.0;
                if (!tmp.empty()) { std::nth_element(tmp.begin(), tmp.begin() + tmp.size() / 2, tmp.end()); med = tmp[tmp.size() / 2]; }
                if (s_all.size() >= 30 ? frameMs > 2.0 * med : frameMs >= 25.0) {
                    double gspan = -1.0, gbusy = -1.0;
                    mc2_hitch2::gpuMax4(gspan, gbusy);
                    int idx[mc2_hitch2::ScopeTable::kMax];
                    for (int i = 0; i < sc.n; ++i) idx[i] = i;
                    const int top = sc.n < 5 ? sc.n : 5;
                    std::partial_sort(idx, idx + top, idx + sc.n, [&](int a, int b) { return sc.slots[a].ms > sc.slots[b].ms; });
                    std::string scopes;
                    for (int i = 0; i < top; ++i) scopes += std::string(sc.slots[idx[i]].name) + "=" + std::to_string(sc.slots[idx[i]].ms).substr(0, 5) + (i + 1 < top ? "," : "");
                    if (mc2_diag::tagRequested("HITCH"))
                    mc2::fmt::print_err("[HITCH v2] frame={} ms={:.1f} median={:.1f} waitidle_n={} waitidle_ms={:.2f} fence_n={} fence_ms={:.2f} staged_kb={} pipelines={} upload_kb={} objmap_records={} dirty_batches={} bake_state={} bake_cache={} sim_join_ms={:.2f} slot_ms={:.2f} acquire_ms={:.2f} composite_wait_ms={:.2f} gpu_span4_ms={:.2f} gpu_busy4_ms={:.2f} other_mc2={} scopes={}\n",
                        mc2::fmt::as_u64(s_frame), frameMs, med, mc2::fmt::as_u64(wic - s_wic), (win - s_win) / 1e6,
                        mc2::fmt::as_u64(fc - s_fc), (fn - s_fn) / 1e6, mc2::fmt::as_u64((sb - s_sb) / 1024), mc2::fmt::as_u64(pc - s_pc), mc2::fmt::as_u64((ub - s_ub) / 1024),
                        mc2::fmt::as_u64(om - s_om), mc2::fmt::as_u64(db - s_db), mc2::fmt::safe(hs.bakeState.load()), mc2::fmt::safe(hs.bakeCache.load()), (sj - s_sj) / 1e6,
                        (sl - s_sl) / 1e6, (aq - s_aq) / 1e6, (cw - s_cw) / 1e6, gspan, gbusy, mc2::fmt::as_i32(hs.otherMc2.load()),
                        mc2::fmt::safe(scopes.c_str()));
                }
            }
            if (s_all.size() < 20000) s_all.push_back(static_cast<float>(frameMs));
        }
        s_fc = fc; s_fn = fn; s_sb = sb; s_sl = sl; s_aq = aq; s_cw = cw;
        s_wic = wic; s_win = win; s_pc = pc; s_ub = ub; s_om = om; s_db = db; s_sj = sj;
        sc.n = 0;
    }
    if (!g_state.enabled) return;
    if (!g_firstFrameSeen) {
        g_firstFrameSeen = true;
        // NOTE: first_frame fires BEFORE mission_ready. Runner must not treat
        // this as a gameplay-ready signal — mission_ready is authoritative.
        emitTiming("first_frame");
        {
            const int v = 0;   // row default 0 = keep the built-in cap
            if (v > 0 && v < 1000000) g_frameMsCap = static_cast<size_t>(v);
        }
        g_frameMs.assign(g_frameMsCap, 0.0);
    }
    if (!g_missionReadyT) return; // don't count loading-phase frames
    g_frameMs[g_frameIdx] = frameMs;
    g_frameIdx = (g_frameIdx + 1) % g_frameMsCap;
    g_frameCount++;
}

void emitCleanSummary() {
    if (!g_state.enabled) return;
    if (g_summaryEmitted.exchange(true)) return;

    // Ring-buffer-aware snapshot. Pre-wrap: valid slots are [0..count).
    // Post-wrap: whole ring is valid; copy in chronological order
    // [idx..cap) then [0..idx) for debugging clarity (sort ignores order).
    size_t used = std::min(g_frameCount, g_frameMsCap);
    std::vector<double> samples;
    samples.reserve(used);
    if (g_frameCount < g_frameMsCap) {
        samples.assign(g_frameMs.begin(), g_frameMs.begin() + used);
    } else {
        samples.assign(g_frameMs.begin() + g_frameIdx, g_frameMs.end());
        samples.insert(samples.end(), g_frameMs.begin(),
                       g_frameMs.begin() + g_frameIdx);
    }
    std::sort(samples.begin(), samples.end());

    auto pct = [&](double p) -> double {
        if (samples.empty()) return 0.0;
        size_t idx = static_cast<size_t>(((p / 100.0) * (samples.size() - 1)));
        return samples[idx];
    };
    double p50 = pct(50.0);
    double p99 = pct(99.0);
    double peak = samples.empty() ? 0.0 : samples.back();
    double sum = 0.0;
    for (double v : samples) sum += v;
    double avgMs = samples.empty() ? 0.0 : sum / static_cast<double>(samples.size());
    double avgFps = (avgMs > 0.0) ? 1000.0 / avgMs : 0.0;
    double p1LowFps = (pct(99.0) > 0.0) ? 1000.0 / pct(99.0) : 0.0;

    mc2::fmt::print_out("[PERF v1] avg_fps={:.1f} p50_ms={:.2f} p99_ms={:.2f} p1low_fps={:.1f} peak_ms={:.2f} samples={:d}\n",
        avgFps, p50, p99, p1LowFps, peak, mc2::fmt::as_size(samples.size()));

    double actualMs = elapsedMsSince(g_missionReadyT ? g_missionReadyT : g_startupT0);
    // Fault counters OMITTED intentionally — the runner is the authoritative
    // source; it counts [GL_ERROR v1]/[TGL_POOL v1]/[ASSET_SCALE v1]/[DESTROY v1]
    // lines from the stream. Embedding engine-side counts would create a
    // second source of truth with drift potential.
    mc2::fmt::print_out("[SMOKE v1] event=summary result=pass duration_actual_ms={:.1f} frames={:d}\n",
        actualMs, mc2::fmt::as_size(g_frameCount));
    std::fflush(stdout);
}

void emitFailSummary(mc2::czstring reason, mc2::czstring stage) {
    if (!g_state.enabled) return;
    if (g_summaryEmitted.exchange(true)) return;
    mc2::fmt::print_out("[SMOKE v1] event=summary result=fail reason={} stage={}\n",
        mc2::fmt::safe(reason ? reason : "unknown"),
        mc2::fmt::safe(stage  ? stage  : "unknown"));
    std::fflush(stdout);
}

bool resolveMissionPaths() {
    if (!g_state.enabled || g_state.mission.empty()) return false;

    // Loose override path: data/missions/<stem>.fit
    // We can only *confirm* the loose path by statting it. We cannot confirm
    // FST membership without opening the FST index, so if the loose file is
    // absent we emit source=fst_assumed. If the mission is truly missing,
    // the downstream File::open path will fail and the runner will bucket
    // it under missing_file / engine_reported_fail -- not our problem to
    // pre-detect here.
    std::string loose = "data/missions/" + g_state.mission + ".fit";
    struct stat st{};
    const char* source = "fst_assumed";
    if (stat(loose.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
        source = "loose";
    }

    mc2::fmt::print_out("[SMOKE v1] event=mission_resolve stem={} source={}\n",
        g_state.mission.c_str(), mc2::fmt::safe(source));
    std::fflush(stdout);
    return true;
}

bool shouldQuit() {
    if (!g_state.enabled) return false;
    if (!g_missionReadyT) return false;
    if (g_state.tickBudget > 0 && g_state.fixedTimestep) {
        // Tick-budget exit: sim ticks, not wall time. --duration stays the runner's timeout.
        if (g_fixedSimFrame < static_cast<uint32_t>(g_state.tickBudget)) return false;
        mc2::fmt::print_out("[SMOKE v1] event=tick_budget ticks={:d}\n", mc2::fmt::as_i32(static_cast<int>(g_fixedSimFrame)));
        std::fflush(stdout);
        return true;
    }
    double ms = elapsedMsSince(g_missionReadyT);
    const double wallMs = static_cast<double>(g_state.durationSec) * 1000.0;
    if (ms < wallMs) return false;
    // The run is bounded by wall time, but mission scripts key off sim time (mc2_10 plays its
    // intro video at 5 s of sim). A host stall inside the window starves the sim and the
    // script event never fires, which is not an engine fault. Hold the quit until the sim has
    // covered kMinSimSeconds, bounded by a 3x wall cap so a hung sim still ends the run.
    constexpr double kMinSimSeconds = 10.0;
    if (g_simSeconds < kMinSimSeconds && ms < wallMs * 3.0) {
        if (!g_simExtendLogged) {
            g_simExtendLogged = true;
            mc2::fmt::print_out("[SMOKE v1] event=sim_extend sim_s={:.2f} need_s={:.1f}\n", g_simSeconds, kMinSimSeconds);
            std::fflush(stdout);
        }
        return false;
    }
    return true;
}

void onSimTick(double scenarioTimeSeconds) {
    g_simSeconds = scenarioTimeSeconds;
}

void markMissionReady() {
    if (!g_missionReadyT) {
        g_missionReadyT = SDL_GetPerformanceCounter();
        mc2_hitch2::stats().missionReady.store(true);
        if (g_state.enabled) emitTiming("mission_ready");
    }
}

bool missionHasStarted() {
    return g_missionReadyT != 0;
}

// ---------------------------------------------------------------------------
// S9D fixed timestep (deterministic smoke clock).
// ---------------------------------------------------------------------------

bool fixedTimestepEnabled() { return g_state.fixedTimestep; }

float fixedFrameSeconds() {
    // The virtual render frame of the harness clock. Unset (or 0) keeps the legacy 1/30 s, so one frame is exactly one sim
    // tick and the committed SIMID baselines hold. MC2_FPS_CAP=N sets N virtual frames per second instead: the sim then runs
    // 30 ticks per virtual second whatever N is, which is the frame-rate-independence proof (tools/sim_determinism.py --env-a/--env-b).
    if (mc2::gate::isSet<"MC2_FPS_CAP">() && mc2::gate::isNonEmpty<"MC2_FPS_CAP">()) {
        const int cap = mc2::gate::get<"MC2_FPS_CAP">();
        if (cap > 0) return 1.0f / static_cast<float>(cap);
    }
    return 1.0f / 30.0f;
}

double fixedClockSeconds() {
    // S9E: shares the S9D fixed sim-frame counter (g_fixedSimFrame, advanced by
    // fixedTimestepOnSimFrame on each fixed-step sim frame). Frame N maps to a
    // deterministic time across runs. 1/30 s per frame == 30Hz, the same basis
    // scenarioTime advances on, so sim and render-shader clocks stay locked.
    return static_cast<double>(g_fixedSimFrame) / 30.0;
}

void fixedTimestepOnSimFrame(double scenarioTimeSeconds) {
    if (!g_state.fixedTimestep) return;
    // Counts only sim frames where the fixed step actually advances the clock
    // (caller invokes this from the same branch that advances scenarioTime),
    // so frame N maps to a deterministic scenarioTime across runs.
    ++g_fixedSimFrame;
    if (g_fixedSimFrame == 60 || g_fixedSimFrame == 120 ||
        g_fixedSimFrame == 180) {
        mc2::fmt::print_err("[SMOKE_CLOCK_PROBE v1] frame={:d} scenarioTime={:.6f}\n", mc2::fmt::as_u32(static_cast<unsigned>(g_fixedSimFrame)), scenarioTimeSeconds);
        std::fflush(stderr);
    }
}

} // namespace SmokeMode
