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
    int tickBudget = 0;             // --ticks <N>: with the fixed clock, quit after N sim ticks past mission_ready (0 = use --duration)
    uint32_t seed = 0xC0FFEE;       // MC2_SMOKE_SEED env override
    bool fixedTimestep = false;     // MC2_SMOKE_FIXED_TIMESTEP=1 env override.
                                    // Opt-in, SEPARATE from MC2_SMOKE_MODE: when set,
                                    // the sim clock advances by a fixed 1000/30 ms
                                    // step per frame instead of real wall time, so
                                    // frame N reaches an identical sim state every
                                    // run (S9D deterministic smoke clock).
};

// Fixed-timestep accessors (S9D). All cheap; safe to call before parseArgs.

// True when MC2_SMOKE_FIXED_TIMESTEP=1 was present. Single cached bool read;
// when false the caller MUST behave byte-identically to retail.
bool fixedTimestepEnabled();

// Fixed step in milliseconds (1000/30). Constant; only meaningful when enabled.
float fixedTimestepMs();

// Length in seconds of one virtual render frame under the harness clock: 1/30 unless MC2_FPS_CAP names another rate.
float fixedFrameSeconds();

// S9E deterministic render-shader clock. Returns the fixed-step elapsed time
// in SECONDS = (fixed sim-frame count) * (1/30). Shares the exact S9D sim-frame
// counter (advanced by fixedTimestepOnSimFrame) so the sim AND every
// render-shader `time` uniform are driven off one pinned 30Hz clock: frame N
// renders with identical shader time every run. Only meaningful when
// fixedTimestepEnabled(); callers gate on that and keep their original
// wall-clock expression on the OFF path (byte-identical retail).
double fixedClockSeconds();

// Frame-counter hook for the deterministic clock probe. Call exactly once per
// rendered sim frame (the same place scenarioTime advances). When the flag is
// on, emits "[SMOKE_CLOCK_PROBE v1] frame=<n> scenarioTime=<t>" at frames
// 60/120/180 so two runs can be diffed without the S9 visual-capture path.
// No-op when the flag is off.
void fixedTimestepOnSimFrame(double scenarioTimeSeconds);

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
