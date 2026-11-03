// GameOS/gameos/gos_smoke.cpp
#include "gos_smoke.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/stat.h>
#ifndef S_ISREG
#define S_ISREG(x) ((_S_IFREG & (x)) != 0)
#endif

#include <SDL2/SDL.h>
#include "Gates/gate.h"
#include "Gates/gate_cold.h"   // str<>()
#include "mc2fmt.h"

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
        [[noreturn]] static void missingValue(const char* flag) {
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

void emitTiming(const char* eventName) {
    if (!g_state.enabled) return;
    double ms = elapsedMsSince(g_startupT0);
    mc2::fmt::print_out("[TIMING v1] event={} elapsed_ms={:.1f}\n", mc2::fmt::safe(eventName), ms);
    std::fflush(stdout);
}

void samplePerf(double frameMs) {
    if (!g_state.enabled) return;
    if (!g_firstFrameSeen) {
        g_firstFrameSeen = true;
        // NOTE: first_frame fires BEFORE mission_ready. Runner must not treat
        // this as a gameplay-ready signal — mission_ready is authoritative.
        emitTiming("first_frame");
        {
            const int v = mc2::gate::get<"MC2_SMOKE_PERF_SAMPLES">();   // row default 0 = keep the built-in cap
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

void emitFailSummary(const char* reason, const char* stage) {
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
