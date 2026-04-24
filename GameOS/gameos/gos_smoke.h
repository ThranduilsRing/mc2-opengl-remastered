// GameOS/gameos/gos_smoke.h
#pragma once

#include <cstdint>
#include <string>

namespace SmokeMode {

// Singleton state. All fields read-only after parseArgs() returns.
// profile/mission are std::string so later code can stash them past argv
// lifetime without reasoning about pointer validity.
struct State {
    bool enabled = false;           // MC2_SMOKE_MODE=1 present
    bool active = false;            // --smoke-active (Phase 2, accepted but inert)
    std::string profile = "stock";  // --profile <name>
    std::string mission;            // --mission <stem>, required when enabled
    int durationSec = 120;          // --duration <seconds>
    uint32_t seed = 0xC0FFEE;       // MC2_SMOKE_SEED env override
};

// Parse argv and env. Call exactly once, before GetGameOSEnvironment.
// On any fatal parse error (e.g. --mission absent when MC2_SMOKE_MODE=1),
// prints "[SMOKE v1] event=summary result=fail reason=argv_error ..." and exits 2.
void parseArgs(int argc, char** argv);

// Accessors. Safe to call before parseArgs (returns defaults / enabled=false).
const State& state();

// Register atexit handler for best-effort summary emission. Idempotent.
void installAtexitSummary();

// Lifecycle milestones -- emit [TIMING v1] event=<name> elapsed_ms=<n>.
// elapsed measured from the performance counter captured in parseArgs.
void emitTiming(const char* eventName);

// Per-frame sampler. Call once per rendered frame with frame duration in ms.
// After mission_ready has been emitted, samples land in the perf ring buffer.
void samplePerf(double frameMs);

// Emit the [PERF v1] line followed by [SMOKE v1] event=summary result=pass.
// Called by the auto-quit path.
void emitCleanSummary();

// Emit [SMOKE v1] event=summary result=fail reason=<r> stage=<s>.
// Safe to call from any point after parseArgs.
void emitFailSummary(const char* reason, const char* stage);

// Resolve --mission stem to concrete paths (.fit/.abl/.pak).
// Emits [SMOKE v1] event=mission_resolve. Returns true when at least the
// loose file exists OR we fall back to FST-assumed. (Implementation in Task 4.)
bool resolveMissionPaths();

// True once --duration has elapsed past mission_ready.
bool shouldQuit();

// True once markMissionReady() has been called (mission is active / was active).
// Used by the frame-cap logic to apply a lower cap during menus vs gameplay.
bool missionHasStarted();

// Called once when the mission is ready to play. Starts the duration timer.
void markMissionReady();

} // namespace SmokeMode
