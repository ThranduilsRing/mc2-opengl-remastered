// GameOS/gameos/gos_smoke.cpp
#include "gos_smoke.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <sys/stat.h>

#include <SDL2/SDL.h>

namespace {
SmokeMode::State g_state;
uint64_t g_startupT0 = 0;   // SDL_GetPerformanceCounter at parseArgs entry
uint64_t g_freq = 0;
uint64_t g_missionReadyT = 0;
std::atomic<bool> g_atexitInstalled{false};

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
    // Filled in Task 6b.
}

void emitTiming(const char*) {
    // Filled in Task 5.
}

void samplePerf(double) {
    // Filled in Task 7.
}

void emitCleanSummary() {
    // Filled in Task 7.
}

void emitFailSummary(const char*, const char*) {
    // Filled in Task 6b.
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
    if (stat(loose.c_str(), &st) == 0 && (st.st_mode & _S_IFREG)) {
        source = "loose";
    }

    std::fprintf(stdout,
        "[SMOKE v1] event=mission_resolve stem=%s source=%s\n",
        g_state.mission.c_str(), source);
    std::fflush(stdout);
    return true;
}

bool shouldQuit() {
    return false; // Filled in Task 8.
}

void markMissionReady() {
    // Filled in Task 5.
}

} // namespace SmokeMode
