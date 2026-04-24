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
    (void)argc; (void)argv;
    // Filled in Task 2.
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
