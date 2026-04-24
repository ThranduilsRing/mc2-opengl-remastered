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

double elapsedMsSince(uint64_t t0) {
    if (!t0 || !g_freq) return 0.0;
    uint64_t now = SDL_GetPerformanceCounter();
    return 1000.0 * (double)(now - t0) / (double)g_freq;
}
} // namespace

namespace SmokeMode {

const State& state() { return g_state; }

void parseArgs(int argc, char** argv) {
    g_freq = SDL_GetPerformanceFrequency();
    g_startupT0 = SDL_GetPerformanceCounter();

    g_state.enabled = (std::getenv("MC2_SMOKE_MODE") != nullptr);

    if (const char* seedEnv = std::getenv("MC2_SMOKE_SEED")) {
        char* end = nullptr;
        unsigned long v = std::strtoul(seedEnv, &end, 0);
        if (end != seedEnv) g_state.seed = static_cast<uint32_t>(v);
    }

    // [[noreturn]] on lambdas requires C++23; use a local static function instead.
    struct ParseArgsFail {
        [[noreturn]] static void missingValue(const char* flag) {
            std::fprintf(stdout,
                "[SMOKE v1] event=summary result=fail reason=argv_error "
                "stage=parseArgs detail=\"%s requires value\"\n", flag);
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
        } else if (std::strcmp(a, "--smoke-active") == 0) {
            g_state.active = true;
        }
        // Unknown flags pass through to other parsers (GameOS, validate, etc.).
    }

    if (g_state.enabled && g_state.mission.empty()) {
        std::fprintf(stdout,
            "[SMOKE v1] event=summary result=fail reason=argv_error "
            "stage=parseArgs detail=\"--mission required when MC2_SMOKE_MODE is set\"\n");
        std::fflush(stdout);
        std::exit(2);
    }
    if (!g_state.enabled && !g_state.mission.empty()) {
        // Smoke args without env -- refuse.
        std::fprintf(stdout,
            "[SMOKE v1] event=summary result=fail reason=argv_error "
            "stage=parseArgs detail=\"--mission requires MC2_SMOKE_MODE to be set\"\n");
        std::fflush(stdout);
        std::exit(2);
    }
    if (g_state.enabled) {
        std::fprintf(stdout,
            "[SMOKE v1] event=banner mode=%s mission=%s profile=%s duration=%d seed=0x%x\n",
            g_state.active ? "active" : "passive",
            g_state.mission.c_str(), g_state.profile.c_str(),
            g_state.durationSec, g_state.seed);
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
        std::fprintf(stdout,
            "[SMOKE v1] event=summary result=fail reason=early_exit stage=atexit\n");
        std::fflush(stdout);
    });
}

void emitTiming(const char* eventName) {
    if (!g_state.enabled) return;
    double ms = elapsedMsSince(g_startupT0);
    std::fprintf(stdout,
        "[TIMING v1] event=%s elapsed_ms=%.1f\n", eventName, ms);
    std::fflush(stdout);
}

void samplePerf(double frameMs) {
    if (!g_state.enabled) return;
    if (!g_firstFrameSeen) {
        g_firstFrameSeen = true;
        // NOTE: first_frame fires BEFORE mission_ready. Runner must not treat
        // this as a gameplay-ready signal — mission_ready is authoritative.
        emitTiming("first_frame");
        if (const char* capEnv = std::getenv("MC2_SMOKE_PERF_SAMPLES")) {
            int v = std::atoi(capEnv);
            if (v > 0 && v < 1000000) g_frameMsCap = (size_t)v;
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
        size_t idx = (size_t)((p / 100.0) * (samples.size() - 1));
        return samples[idx];
    };
    double p50 = pct(50.0);
    double p99 = pct(99.0);
    double peak = samples.empty() ? 0.0 : samples.back();
    double sum = 0.0;
    for (double v : samples) sum += v;
    double avgMs = samples.empty() ? 0.0 : sum / (double)samples.size();
    double avgFps = (avgMs > 0.0) ? 1000.0 / avgMs : 0.0;
    double p1LowFps = (pct(99.0) > 0.0) ? 1000.0 / pct(99.0) : 0.0;

    std::fprintf(stdout,
        "[PERF v1] avg_fps=%.1f p50_ms=%.2f p99_ms=%.2f p1low_fps=%.1f peak_ms=%.2f samples=%zu\n",
        avgFps, p50, p99, p1LowFps, peak, samples.size());

    double actualMs = elapsedMsSince(g_missionReadyT ? g_missionReadyT : g_startupT0);
    // Fault counters OMITTED intentionally — the runner is the authoritative
    // source; it counts [GL_ERROR v1]/[TGL_POOL v1]/[ASSET_SCALE v1]/[DESTROY v1]
    // lines from the stream. Embedding engine-side counts would create a
    // second source of truth with drift potential.
    std::fprintf(stdout,
        "[SMOKE v1] event=summary result=pass duration_actual_ms=%.1f frames=%zu\n",
        actualMs, g_frameCount);
    std::fflush(stdout);
}

void emitFailSummary(const char* reason, const char* stage) {
    if (!g_state.enabled) return;
    if (g_summaryEmitted.exchange(true)) return;
    std::fprintf(stdout,
        "[SMOKE v1] event=summary result=fail reason=%s stage=%s\n",
        reason ? reason : "unknown",
        stage  ? stage  : "unknown");
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

    std::fprintf(stdout,
        "[SMOKE v1] event=mission_resolve stem=%s source=%s\n",
        g_state.mission.c_str(), source);
    std::fflush(stdout);
    return true;
}

bool shouldQuit() {
    if (!g_state.enabled) return false;
    if (!g_missionReadyT) return false;
    double ms = elapsedMsSince(g_missionReadyT);
    return ms >= (double)g_state.durationSec * 1000.0;
}

void markMissionReady() {
    if (!g_state.enabled) return;
    if (!g_missionReadyT) {
        g_missionReadyT = SDL_GetPerformanceCounter();
        emitTiming("mission_ready");
    }
}

bool missionHasStarted() {
    return g_missionReadyT != 0;
}

} // namespace SmokeMode
