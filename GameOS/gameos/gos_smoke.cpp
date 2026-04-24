// GameOS/gameos/gos_smoke.cpp
#include "gos_smoke.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

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

    auto failMissingValue = [](const char* flag) {
        std::fprintf(stdout,
            "[SMOKE v1] event=summary result=fail reason=argv_error "
            "stage=parseArgs detail=\"%s requires value\"\n", flag);
        std::fflush(stdout);
        std::exit(2);
    };

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
            g_state.durationSec = std::atoi(argv[++i]);
        } else if (std::strcmp(a, "--smoke-active") == 0) {
            g_state.active = true;
        }
        // Unknown flags pass through to other parsers (GameOS, validate, etc.).
    }

    if (g_state.enabled && g_state.mission.empty()) {
        std::fprintf(stdout,
            "[SMOKE v1] event=summary result=fail reason=argv_error "
            "stage=parseArgs detail=\"--mission required when MC2_SMOKE_MODE=1\"\n");
        std::fflush(stdout);
        std::exit(2);
    }
    if (!g_state.enabled && !g_state.mission.empty()) {
        // Smoke args without env -- refuse.
        std::fprintf(stdout,
            "[SMOKE v1] event=summary result=fail reason=argv_error "
            "stage=parseArgs detail=\"--mission requires MC2_SMOKE_MODE=1\"\n");
        std::fflush(stdout);
        std::exit(2);
    }
    if (g_state.durationSec <= 0) g_state.durationSec = 120;

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
    // Filled in Task 4.
    return false;
}

bool shouldQuit() {
    return false; // Filled in Task 8.
}

void markMissionReady() {
    // Filled in Task 5.
}

} // namespace SmokeMode
