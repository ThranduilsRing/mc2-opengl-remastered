# Smoke test matrix — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship a passive-mode automated smoke matrix that boots `mc2.exe` into a named mission, runs it for N seconds, and verifies silence on the codebase's known-silent failure modes (GL errors, TGL pool exhaustion, asset-scale OOB, shader errors, crashes, freezes).

**Architecture:** One-way stdout/stderr contract between an in-engine `MC2_SMOKE_MODE=1` path (new argv `--profile/--mission/--duration`, auto-load, auto-quit, structured `[SMOKE v1]/[PERF v1]/[TIMING v1]` emission) and a Python runner (`scripts/run_smoke.py`) that reads a tiered manifest, spawns serially, parses signals, and writes artifacts on fail. Menu canary and `--smoke-active` autopilot are out of scope for this plan (separate follow-up specs).

**Tech Stack:** C++17 (engine hooks in `gameosmain.cpp`, new `gos_smoke.{h,cpp}`), Python 3.10+ + pytest (runner + tests), SDL2 timing, existing `[INSTR v1]` banner pattern.

**Design spec:** `docs/superpowers/specs/2026-04-23-smoke-test-matrix-design.md`.

---

## Preamble for the implementer

- Work in the `nifty-mendeleev` worktree (`A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`). Deploy target is `A:/Games/mc2-opengl/mc2-win64-v0.1.1/`.
- Build config is always `--config RelWithDebInfo` (per `CLAUDE.md §Critical Rules`). Release crashes with `GL_INVALID_ENUM`.
- Close any running `mc2.exe` before rebuilding, or the linker fails with `LNK1201: error writing to program database`.
- Every engine-side task ends with a manual smoke invocation to prove the new stdout line appears. The Python runner doesn't exist until Milestone B, so Milestone A tasks use `grep`/`findstr` on captured stdout.
- The existing instrumentation banner lives at `GameOS/gameos/gameosmain.cpp:591-596`. Read that block before starting Task 1 so you understand the format used throughout.

---

## File structure

**Created:**
- `GameOS/gameos/gos_smoke.h` — smoke-mode singleton header, argv parse API, phase getters, perf ring-buffer API.
- `GameOS/gameos/gos_smoke.cpp` — singleton state, argv parser, `[SMOKE v1]`/`[PERF v1]`/`[TIMING v1]` emitters, atexit summary.
- `scripts/run_smoke.py` — Python runner CLI.
- `scripts/smoke_lib/__init__.py` — package marker.
- `scripts/smoke_lib/manifest.py` — manifest file parser.
- `scripts/smoke_lib/logparse.py` — stdout/stderr signal parser (line → event).
- `scripts/smoke_lib/gates.py` — fault gate evaluator, bucket assignment.
- `scripts/smoke_lib/runner.py` — per-mission spawn/parse/timeout loop.
- `scripts/smoke_lib/report.py` — markdown + JSON report writer.
- `scripts/smoke_lib/baselines.py` — baseline load/store.
- `tests/smoke/smoke_missions.txt` — tiered mission manifest.
- `tests/smoke/baselines.json` — empty stub, populated by `--baseline-update`.
- `tests/smoke/test_manifest.py` — pytest for manifest parser.
- `tests/smoke/test_logparse.py` — pytest for line parser.
- `tests/smoke/test_gates.py` — pytest for gate evaluator.
- `tests/smoke/test_report.py` — pytest for report writer.
- `tests/smoke/fixtures/fake_engine_pass.log` — canned-input for parser tests.
- `tests/smoke/fixtures/fake_engine_gl_error.log` — canned-input for parser tests.
- `tests/smoke/fixtures/fake_engine_freeze_load.log` — canned-input for parser tests.

**Modified:**
- `GameOS/gameos/gameosmain.cpp` — extend `[INSTR v1]` banner, call `SmokeMode::parseArgs` before `GetGameOSEnvironment`, hook auto-launch after engine init, call perf sampler each frame, auto-quit on duration expiry.
- `CMakeLists.txt` or the GameOS CMake file — add `gos_smoke.cpp` to the `mc2` executable target.
- `code/logistics.cpp` — add smoke-mode branch in `Logistics::beginMission` to short-circuit UI state when invoked programmatically (or more surgically, expose a helper the smoke entry calls).

**Untouched:**
- `scripts/game_auto.py` (reused by future menu canary, not by this plan).
- Shader files.
- Anything under `shaders/`, `mclib/`, `gui/`.

---

## Milestone A — Engine-side passive smoke mode

Goal at end of milestone: a single handwritten command produces a completed smoke run with all structured lines in stdout.

```
MC2_SMOKE_MODE=1 MC2_HEARTBEAT=1 ./mc2.exe --profile stock --mission mc2_01 --duration 30 > run.log 2>&1
```

And `run.log` contains `[INSTR v1] enabled: ... smoke=1 ...`, `[TIMING v1] event=mission_ready ...`, `[PERF v1] avg_fps=...`, `[SMOKE v1] event=summary result=pass ...` as its tail.

---

### Task 1: Create `gos_smoke.{h,cpp}` skeleton + build wire-up

**Files:**
- Create: `GameOS/gameos/gos_smoke.h`
- Create: `GameOS/gameos/gos_smoke.cpp`
- Modify: `GameOS/gameos/CMakeLists.txt` (add `gos_smoke.cpp` to the `gameos` target sources list; locate the existing `gos_crashbundle.cpp` entry and add alongside it)

- [ ] **Step 1: Write the header**

```cpp
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

// Lifecycle milestones — emit [TIMING v1] event=<name> elapsed_ms=<n>.
// elapsed measured from SmokeMode state().startupT0 (set in parseArgs).
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

// True once --duration has elapsed past mission_ready.
bool shouldQuit();

// Called once when the mission is ready to play. Starts the duration timer.
void markMissionReady();

} // namespace SmokeMode
```

- [ ] **Step 2: Write the skeleton .cpp (no logic yet, just stubs that compile)**

```cpp
// GameOS/gameos/gos_smoke.cpp
#include "gos_smoke.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include <SDL.h>

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
    // Filled in Task 6.
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
    // Filled in Task 6.
}

bool shouldQuit() {
    return false; // Filled in Task 8.
}

void markMissionReady() {
    // Filled in Task 5.
}

} // namespace SmokeMode
```

- [ ] **Step 3: Add to CMake**

Open `GameOS/gameos/CMakeLists.txt`. Locate the line referencing `gos_crashbundle.cpp` (it's part of the `gameos` target's source list) and add `gos_smoke.cpp` immediately below it. If the project uses a glob, verify the new file is picked up.

- [ ] **Step 4: Build**

Run `/mc2-build` (or the underlying cmake invocation from CLAUDE.md). Expected: clean build, no new warnings, `mc2.exe` produced under `build64/RelWithDebInfo/`.

- [ ] **Step 5: Commit**

```bash
git add GameOS/gameos/gos_smoke.h GameOS/gameos/gos_smoke.cpp GameOS/gameos/CMakeLists.txt
git commit -m "feat(smoke): scaffold gos_smoke skeleton + CMake wiring"
```

---

### Task 2: Implement `parseArgs` with argv + env handling

**Files:**
- Modify: `GameOS/gameos/gos_smoke.cpp`

- [ ] **Step 1: Implement the parser**

Replace the stub body of `SmokeMode::parseArgs` with:

```cpp
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
        // Smoke args without env — refuse, per §5.1 of the spec.
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
```

- [ ] **Step 2: Build**

Run `/mc2-build`. Expected: clean compile.

- [ ] **Step 3: Smoke-test the parser manually**

Don't wire the call site yet — the parser isn't invoked. This step is only a compile check. Proceed to Task 3.

- [ ] **Step 4: Commit**

```bash
git add GameOS/gameos/gos_smoke.cpp
git commit -m "feat(smoke): implement parseArgs with env + flag handling"
```

---

### Task 3: Wire `parseArgs` into `main` and extend `[INSTR v1]` banner

**Files:**
- Modify: `GameOS/gameos/gameosmain.cpp:591-596` (extend banner)
- Modify: `GameOS/gameos/gameosmain.cpp:629` (insert `SmokeMode::parseArgs` call)

- [ ] **Step 1: Extend the `[INSTR v1]` banner**

Find the block at `gameosmain.cpp:580-597`. Change the banner snprintf to:

```cpp
    {
        const bool tgl     = (getenv("MC2_TGL_POOL_TRACE")       != nullptr);
        const bool destr   = (getenv("MC2_DESTROY_TRACE")        != nullptr);
        const bool glprint = (getenv("MC2_GL_ERROR_DRAIN_SILENT") == nullptr);
        const bool smoke   = (getenv("MC2_SMOKE_MODE")           != nullptr);
        const char* build  =
#ifdef MC2_BUILD_HASH
            MC2_BUILD_HASH
#else
            "UNKNOWN"
#endif
            ;
        char _cbbuf[320];
        snprintf(_cbbuf, sizeof(_cbbuf),
            "[INSTR v1] enabled: tgl_pool=%d destroy=%d gl_error_print=%d smoke=%d build=%s",
            tgl ? 1 : 0, destr ? 1 : 0, glprint ? 1 : 0, smoke ? 1 : 0, build);
        puts(_cbbuf);
        crashbundle_append(_cbbuf);
    }
```

- [ ] **Step 2: Insert the `SmokeMode::parseArgs` call**

At `gameosmain.cpp:629`, the existing line is:

```cpp
    // Parse validation args before GameOS consumes the command line
    validateParseArgs(argc, argv);
```

Add, immediately after it:

```cpp
    // Smoke mode args must be parsed before GetGameOSEnvironment so any exit
    // on bad argv happens with no GL context held. The parser emits the banner
    // line when MC2_SMOKE_MODE=1.
    SmokeMode::parseArgs(argc, argv);
    SmokeMode::installAtexitSummary();
```

Add the include near the other GameOS includes at the top of the file:

```cpp
#include "gos_smoke.h"
```

- [ ] **Step 3: Build**

Run `/mc2-build`. Expected: clean compile.

- [ ] **Step 4: Deploy and verify banner**

Run `/mc2-deploy`. Then from a terminal:

```bash
cd /a/Games/mc2-opengl/mc2-win64-v0.1.1
MC2_SMOKE_MODE=1 ./mc2.exe --profile stock --mission mc2_01 --duration 5 > /tmp/banner.log 2>&1 &
sleep 2
taskkill //F //IM mc2.exe
grep "INSTR v1" /tmp/banner.log
grep "SMOKE v1 .* event=banner" /tmp/banner.log
```

Expected output contains both:
```
[INSTR v1] enabled: tgl_pool=0 destroy=0 gl_error_print=1 smoke=1 build=...
[SMOKE v1] event=banner mode=passive mission=mc2_01 profile=stock duration=5 seed=0xc0ffee
```

Also verify bad-argv rejection:

```bash
MC2_SMOKE_MODE=1 ./mc2.exe --profile stock > /tmp/badargv.log 2>&1; echo "exit=$?"
grep "argv_error" /tmp/badargv.log
```

Expected: `exit=2` and a `result=fail reason=argv_error` line.

- [ ] **Step 5: Commit**

```bash
git add GameOS/gameos/gameosmain.cpp
git commit -m "feat(smoke): wire parseArgs + extend [INSTR v1] banner with smoke flag"
```

---

### Task 4: Mission stem resolver

**Files:**
- Modify: `GameOS/gameos/gos_smoke.cpp`
- Modify: `GameOS/gameos/gos_smoke.h` (add `resolveMissionPaths`)

- [ ] **Step 1: Add the API declaration**

In `gos_smoke.h`, after `emitTiming`:

```cpp
// Resolve --mission stem to concrete paths (.fit/.abl/.pak).
// Searches data/missions/ first (loose override), then "missions/" prefix via the
// FastFile / FST resolver (File::open does this itself when we call it with a
// "missions/<stem>.fit" path). Emits [SMOKE v1] event=mission_resolve.
// Returns true if at least the .fit file is reachable.
bool resolveMissionPaths();
```

- [ ] **Step 2: Implement**

In `gos_smoke.cpp`, add near the top:

```cpp
#include <sys/stat.h>
#include <string>
```

And implement:

```cpp
bool SmokeMode::resolveMissionPaths() {
    if (!g_state.enabled || g_state.mission.empty()) return false;

    // Loose override path: data/missions/<stem>.fit
    // We can only *confirm* the loose path by statting it. We cannot confirm
    // FST membership without opening the FST index, so if the loose file is
    // absent we emit source=fst_assumed. If the mission is truly missing,
    // the downstream File::open path will fail and the runner will bucket it
    // under missing_file / engine_reported_fail — not our problem to pre-detect.
    std::string loose = "data/missions/" + g_state.mission + ".fit";
    struct stat st{};
    const char* source = "fst_assumed";
    if (stat(loose.c_str(), &st) == 0 && (st.st_mode & S_IFREG)) {
        source = "loose";
    }

    std::fprintf(stdout,
        "[SMOKE v1] event=mission_resolve stem=%s source=%s\n",
        g_state.mission.c_str(), source);
    std::fflush(stdout);
    return true;
}
```

- [ ] **Step 3: Build**

Run `/mc2-build`.

- [ ] **Step 4: Call from main**

This function has no call site yet — the auto-launch branch in Task 6 will invoke it. Proceed.

- [ ] **Step 5: Commit**

```bash
git add GameOS/gameos/gos_smoke.h GameOS/gameos/gos_smoke.cpp
git commit -m "feat(smoke): mission stem resolver with loose/fst source emission"
```

---

### Task 5: `[TIMING v1]` milestone emitter + existing `startup_phase` bridge

**Files:**
- Modify: `GameOS/gameos/gos_smoke.cpp`
- Modify: `GameOS/gameos/gameosmain.cpp:523-527` (the existing `startup_phase` function)

- [ ] **Step 1: Implement `emitTiming` and `markMissionReady`**

In `gos_smoke.cpp`:

```cpp
void SmokeMode::emitTiming(const char* eventName) {
    if (!g_state.enabled) return;
    double ms = elapsedMsSince(g_startupT0);
    std::fprintf(stdout,
        "[TIMING v1] event=%s elapsed_ms=%.1f\n", eventName, ms);
    std::fflush(stdout);
}

void SmokeMode::markMissionReady() {
    if (!g_state.enabled) return;
    if (!g_missionReadyT) {
        g_missionReadyT = SDL_GetPerformanceCounter();
        emitTiming("mission_ready");
    }
}
```

- [ ] **Step 2: Bridge from existing `startup_phase`**

`gameosmain.cpp:523-527` currently looks like:

```cpp
    static void startup_phase(const char* name) {
        double s = startup_elapsed_s();
        printf("[STARTUP] phase=%s elapsed_s=%.3f\n", name, s);
        fflush(stdout);
    }
```

Change it to also emit a `[TIMING v1]` line when smoke mode is enabled. This keeps the existing `[STARTUP]` output intact for non-smoke runs and adds a parallel smoke line:

```cpp
    static void startup_phase(const char* name) {
        double s = startup_elapsed_s();
        printf("[STARTUP] phase=%s elapsed_s=%.3f\n", name, s);
        fflush(stdout);
        if (SmokeMode::state().enabled) {
            // Emit canonical smoke-line for the same milestone. Name maps 1:1.
            SmokeMode::emitTiming(name);
        }
    }
```

The include `#include "gos_smoke.h"` was added in Task 3.

- [ ] **Step 3: Build**

Run `/mc2-build`.

- [ ] **Step 4: Verify** (deferred to Task 8 end-to-end verify — no call path reaches `markMissionReady` yet).

- [ ] **Step 5: Commit**

```bash
git add GameOS/gameos/gos_smoke.cpp GameOS/gameos/gameosmain.cpp
git commit -m "feat(smoke): [TIMING v1] milestone emitter + startup_phase bridge"
```

---

### Task 6a: Identify the exact Launch entrypoint and data fields (exploration)

This is the plan's highest-risk unknown and a source of plan speculation. Resolve it before writing any code.

**Files:**
- Create: `docs/superpowers/plans/progress/2026-04-23-smoke-launch-entry-notes.md` (short findings doc, committed)

**Research questions to answer:**

1. What exact function does the Logistics "Launch" button's callback invoke? (We know `Logistics::beginMission(void*, int, void*[])` at `code/logistics.cpp:458` is the target; confirm by tracing which aObject callback in the Logistics GUI wires to it.)
2. What fields does the `MISSION_LOAD_SP_QUICKSTART` code path at `logistics.cpp:465` read to determine *which* mission to load? (Search `LogisticsData` for "current mission" fields, not just methods.)
3. Does `LogisticsData::skipLogistics()` return a bool backed by a simple field, or is it computed? If backed by a field, what's the setter (if any)? If computed, what drives it?
4. Where and how does the engine go from "`beginMission` returned" to "first playable frame renders" — i.e. what is the narrowest observable predicate for `mission_ready`?

**Steps:**

- [ ] **Step 1: Trace callback wiring**

```bash
grep -rn "beginMission" code/ gui/
grep -rn "onLaunch\|launchBtn\|LaunchAction" gui/ code/
# Find the aObject callback table entry that references beginMission.
```

Record the call site and surrounding context in the notes doc.

- [ ] **Step 2: Inspect `LogisticsData` fields**

```bash
grep -n "currentMission\|missionName\|curMission" code/logisticsdata.*
grep -n "skipLogistics\|bool.*skip" code/logisticsdata.*
```

Record the exact field name used for "active mission file" and the backing field for `skipLogistics()`.

- [ ] **Step 3: Find mission_ready predicate**

```bash
grep -rn "MissionStart\|missionIsLive\|numPlayerMechs\|mission->get\|MissionData::" code/ | head -30
```

Identify the cheapest predicate true iff the mission is in live-play state (not just loaded). Candidate: `mission && mission->numPlayers() > 0 && !mission->isLoading()`. Record the exact predicate shape with file:line.

- [ ] **Step 4: Commit findings**

Write `docs/superpowers/plans/progress/2026-04-23-smoke-launch-entry-notes.md`:

```markdown
# Smoke launch entry notes

## Launch callback wiring
- File: `<path>:<line>`
- Callback: `<function signature>`
- How the Launch button invokes it: <summary>

## LogisticsData active-mission field
- File: `<path>:<line>`
- Field: `<type> LogisticsData::<name>` (private/public)
- Setter (if any): `<signature>` or "direct field write required"

## skipLogistics backing
- File: `<path>:<line>`
- Backed by: `<field>` or "computed from <source>"

## mission_ready predicate
- File: `<path>:<line>`
- Expression: `<c++ predicate>`
- Notes: <edge cases, timing>
```

```bash
git add docs/superpowers/plans/progress/2026-04-23-smoke-launch-entry-notes.md
git commit -m "chore(smoke): document Launch-entry findings for Task 6b"
```

---

### Task 6b: Auto-launch hook into `Logistics::beginMission` + atexit summary

**⚠ Do NOT begin this task until Task 6a's notes doc (`docs/superpowers/plans/progress/2026-04-23-smoke-launch-entry-notes.md`) has been committed with real identifiers for the active-mission field, skip-logistics backing, and mission_ready predicate. The placeholder `<ACTIVE_MISSION_FIELD>`/`<SKIP_LOGISTICS_FIELD>` tokens below must be substituted before this task compiles.**

Implements the hook using the concrete identifiers from Task 6a's notes doc.

**Files:**
- Modify: `code/logistics.cpp` (add smoke-mode branch after init, before normal UI flow)
- Modify: `GameOS/gameos/gos_smoke.cpp` (implement `installAtexitSummary`, `emitFailSummary`)

This task has two halves that together bypass the Logistics UI when smoke mode is enabled.

- [ ] **Step 1: Implement the atexit summary**

In `gos_smoke.cpp`:

```cpp
namespace {
std::atomic<bool> g_summaryEmitted{false};
} // namespace

void SmokeMode::emitFailSummary(const char* reason, const char* stage) {
    if (!g_state.enabled) return;
    if (g_summaryEmitted.exchange(true)) return;
    std::fprintf(stdout,
        "[SMOKE v1] event=summary result=fail reason=%s stage=%s\n",
        reason ? reason : "unknown",
        stage  ? stage  : "unknown");
    std::fflush(stdout);
}

void SmokeMode::installAtexitSummary() {
    if (g_atexitInstalled.exchange(true)) return;
    std::atexit([]{
        if (!g_state.enabled) return;
        if (g_summaryEmitted.load()) return;
        // Best-effort — if we reach atexit without an explicit summary, we
        // assume something failed after parseArgs but before emitCleanSummary.
        std::fprintf(stdout,
            "[SMOKE v1] event=summary result=fail reason=early_exit stage=atexit\n");
        std::fflush(stdout);
    });
}
```

- [ ] **Step 2: Re-read the Task 6a notes doc**

Open `docs/superpowers/plans/progress/2026-04-23-smoke-launch-entry-notes.md`. The rest of this task substitutes the concrete field names recorded there into the skeleton below.

- [ ] **Step 3: Emit `profile_ready` from the stock profile load path**

The spec contract (§5.3) requires a `[TIMING v1] event=profile_ready` milestone after profile load succeeds. Find the code path that returns from the stock profile load:

```bash
grep -rn "LoadProfile\|loadProfile\|profile.*loaded\|ReadProfile" code/
```

At the return-success site, add:

```cpp
#include "GameOS/gameos/gos_smoke.h"
// ...
if (SmokeMode::state().enabled) {
    SmokeMode::emitTiming("profile_ready");
}
```

- [ ] **Step 4: Add the smoke-mode branch in Logistics**

At the Logistics entry point identified in 6a's notes (likely early in `Logistics::update()` guarded by a run-once flag), add:

```cpp
#include "GameOS/gameos/gos_smoke.h"  // near existing includes

// ... at the chosen entry point:
static bool s_smokeEntered = false;
if (SmokeMode::state().enabled && !s_smokeEntered) {
    s_smokeEntered = true;
    SmokeMode::emitTiming("logistics_ready");
    SmokeMode::resolveMissionPaths();

    // REPLACE FIELD NAMES BELOW with the ones from 6a notes doc.
    // The direct-field variant is shown; use setters if 6a found them.
    const std::string& stem = SmokeMode::state().mission;
    LogisticsData::instance-><ACTIVE_MISSION_FIELD> = stem.c_str();
    LogisticsData::instance-><SKIP_LOGISTICS_FIELD> = true;

    SmokeMode::emitTiming("mission_load_start");
    int rc = Logistics::beginMission(nullptr, 0, nullptr);
    if (rc != 0) {
        SmokeMode::emitFailSummary("mission_begin_failed", "logistics");
        std::exit(3);
    }
    // mission_ready is emitted from Task 7's frame-loop hook when the first
    // playable frame renders — not here, because beginMission returning
    // doesn't mean the frame loop has started.
}
```

Substitute `<ACTIVE_MISSION_FIELD>` and `<SKIP_LOGISTICS_FIELD>` with the real names from the 6a notes doc. If 6a identified setters instead of direct fields, use those.

- [ ] **Step 5: Build**

Run `/mc2-build`.

- [ ] **Step 6: Verify smoke boots into mission**

```bash
/mc2-deploy
cd /a/Games/mc2-opengl/mc2-win64-v0.1.1
MC2_SMOKE_MODE=1 ./mc2.exe --profile stock --mission mc2_01 --duration 15 > /tmp/run.log 2>&1 &
sleep 20
grep -E "(TIMING|SMOKE) v1" /tmp/run.log
```

Expected lines include `[TIMING v1] event=profile_ready ...`, `[TIMING v1] event=logistics_ready ...`, `[SMOKE v1] event=mission_resolve ...`, `[TIMING v1] event=mission_load_start ...`. The process may not auto-quit yet (Task 8 lands that) — taskkill it.

- [ ] **Step 7: Commit**

```bash
git add code/logistics.cpp GameOS/gameos/gos_smoke.cpp code/<profile_loader>.cpp
git commit -m "feat(smoke): auto-launch via Logistics::beginMission + atexit summary"
```

---

### Task 7: Perf ring buffer + `[PERF v1]` emission + `mission_ready` hook

**Files:**
- Modify: `GameOS/gameos/gos_smoke.cpp`
- Modify: `GameOS/gameos/gameosmain.cpp` — the frame-loop block (find via `grep -n "first_frame_presented" gameosmain.cpp` then locate the per-frame present/swap site nearby)

- [ ] **Step 1: Add ring buffer state in `gos_smoke.cpp`**

Near the top:

```cpp
#include <algorithm>
#include <vector>

namespace {
std::vector<double> g_frameMs;          // ring buffer, size = cap
size_t              g_frameIdx = 0;
size_t              g_frameCount = 0;   // monotonic
size_t              g_frameMsCap = 8192;
bool                g_firstFrameSeen = false;
} // namespace
```

- [ ] **Step 2: Implement `samplePerf` and `emitCleanSummary`**

```cpp
void SmokeMode::samplePerf(double frameMs) {
    if (!g_state.enabled) return;
    if (!g_firstFrameSeen) {
        g_firstFrameSeen = true;
        // NOTE: first_frame can fire BEFORE mission_ready (e.g. the logistics
        // screen's first rendered frame). Do not treat first_frame as a
        // gameplay-ready signal in the runner report — it's a pure
        // "something is rendering" milestone. mission_ready is the authoritative
        // gameplay-live boundary.
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

void SmokeMode::emitCleanSummary() {
    if (!g_state.enabled) return;
    if (g_summaryEmitted.exchange(true)) return;

    // Ring-buffer-aware copy. Before wrap, the valid samples are [0..count).
    // After wrap, the valid samples are the whole ring; the write cursor
    // g_frameIdx points at the oldest slot, but for percentile math the
    // ordering within the snapshot doesn't matter since we sort. What matters
    // is that we don't include unwritten slots (pre-wrap) or count samples
    // twice (post-wrap). Keep it simple: slot count = min(count, cap).
    size_t used = std::min(g_frameCount, g_frameMsCap);
    std::vector<double> samples;
    samples.reserve(used);
    if (g_frameCount < g_frameMsCap) {
        // Pre-wrap: valid slots are [0..count).
        samples.assign(g_frameMs.begin(), g_frameMs.begin() + used);
    } else {
        // Post-wrap: whole ring is valid. Order within snapshot is irrelevant
        // for sort-based percentiles, but copy in chronological order for
        // debugging clarity: [idx..cap) then [0..idx).
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
    // Fault counters (gl_errors, pool_nulls, asset_oob, destroys) are OMITTED
    // here intentionally. The runner is the authoritative source — it counts
    // [GL_ERROR v1]/[TGL_POOL v1]/[ASSET_SCALE v1 event=oob_blit]/[DESTROY v1]
    // lines in the stream and publishes them in the report. Adding a
    // sometimes-wrong engine-side count would create a second source of truth.
    std::fprintf(stdout,
        "[SMOKE v1] event=summary result=pass duration_actual_ms=%.1f frames=%zu\n",
        actualMs, g_frameCount);
    std::fflush(stdout);
}
```

- [ ] **Step 3: Hook the frame loop**

Find the per-frame present/swap site. Grep for `SDL_GL_SwapWindow` or look just after the `first_frame_presented` startup_phase call in `gameosmain.cpp`:

```bash
grep -n "first_frame_presented\|SDL_GL_SwapWindow" GameOS/gameos/gameosmain.cpp
```

Immediately after the swap/present call each frame, add:

```cpp
{
    static uint64_t s_lastFrameT = SDL_GetPerformanceCounter();
    uint64_t now = SDL_GetPerformanceCounter();
    double ms = 1000.0 * (double)(now - s_lastFrameT) /
                (double)SDL_GetPerformanceFrequency();
    s_lastFrameT = now;
    SmokeMode::samplePerf(ms);
}
```

Also: when the mission transitions to playable (first frame after `beginMission` returns with a live map), call `SmokeMode::markMissionReady()`. The cleanest signal is the first frame for which the mission-update path runs successfully. If a clean boundary isn't obvious, the **first frame after `first_frame_presented` AND while `mission_load_start` has been emitted but `mission_ready` has not** is close enough — add a one-shot:

```cpp
// Somewhere in the main frame loop, after rendering a frame that shows gameplay:
if (SmokeMode::state().enabled && !s_missionReadyMarked && missionIsPlayable()) {
    SmokeMode::markMissionReady();
    s_missionReadyMarked = true;
}
```

Where `missionIsPlayable()` is a 1-liner: `return mission && mission->numPlayerMechs() > 0;` or whatever the existing predicate is for "mission is live." If no such predicate is cheap, use `SDL_GetPerformanceCounter` delta: mark ready at T = `mission_load_start + 2s` as a pragmatic fallback, and file a follow-up to tighten it.

- [ ] **Step 4: Build and verify**

```bash
/mc2-build-deploy
cd /a/Games/mc2-opengl/mc2-win64-v0.1.1
MC2_SMOKE_MODE=1 ./mc2.exe --profile stock --mission mc2_01 --duration 15 > /tmp/run.log 2>&1
grep -E "(TIMING|SMOKE|PERF) v1" /tmp/run.log
```

Expected: `[TIMING v1] event=mission_ready ...` appears. If run is manually killed, `[PERF v1]` and `[SMOKE v1] event=summary result=pass` do not appear yet (Task 8 lands auto-quit).

- [ ] **Step 5: Commit**

```bash
git add GameOS/gameos/gos_smoke.cpp GameOS/gameos/gameosmain.cpp
git commit -m "feat(smoke): perf ring buffer, [PERF v1] emission, mission_ready hook"
```

---

### Task 8: Auto-quit after duration, emit clean summary

**Files:**
- Modify: `GameOS/gameos/gos_smoke.cpp`
- Modify: `GameOS/gameos/gameosmain.cpp` (frame loop quit check)

- [ ] **Step 1: Implement `shouldQuit`**

In `gos_smoke.cpp`:

```cpp
bool SmokeMode::shouldQuit() {
    if (!g_state.enabled) return false;
    if (!g_missionReadyT) return false;
    double ms = elapsedMsSince(g_missionReadyT);
    return ms >= (double)g_state.durationSec * 1000.0;
}
```

- [ ] **Step 2: Wire the quit check into the frame loop**

In `gameosmain.cpp`, inside the main frame loop, immediately after the existing per-frame event-pump / update block, add:

```cpp
if (SmokeMode::shouldQuit()) {
    SmokeMode::emitTiming("mission_quit");
    SmokeMode::emitCleanSummary();
    // Set the same exit flag the normal Escape-quit path uses. Example:
    gos_AbortMainLoop(); // or whatever the existing clean-exit call is
    break;               // if the loop is while(!quit) { ... }
}
```

Find the clean-exit call by grepping:

```bash
grep -n "Abort\|g_quit\|mainLoopRunning" GameOS/gameos/gameosmain.cpp | head
```

- [ ] **Step 3: Build and end-to-end verify**

```bash
/mc2-build-deploy
cd /a/Games/mc2-opengl/mc2-win64-v0.1.1
MC2_SMOKE_MODE=1 MC2_HEARTBEAT=1 ./mc2.exe --profile stock --mission mc2_01 --duration 30 > /tmp/run.log 2>&1
echo "exit=$?"
tail -5 /tmp/run.log
```

Expected:
- `exit=0`
- Last three lines include `[PERF v1] avg_fps=...`, `[SMOKE v1] event=summary result=pass duration_actual_ms=3000x.x frames=...`
- Intermediate log contains `[INSTR v1] enabled: ... smoke=1 ...`, `[SMOKE v1] event=banner ...`, `[SMOKE v1] event=mission_resolve ...`, `[TIMING v1] event=logistics_ready ...`, `[TIMING v1] event=mission_load_start ...`, `[TIMING v1] event=mission_ready ...`, `[HEARTBEAT]` lines at ~1Hz.

- [ ] **Step 4: Commit and milestone-tag**

```bash
git add GameOS/gameos/gos_smoke.cpp GameOS/gameos/gameosmain.cpp
git commit -m "feat(smoke): auto-quit after duration, emit [PERF v1] + clean summary"
git tag smoke-milestone-a
```

**End of Milestone A.** Engine emits a complete line record for a passing run.

---

## Milestone B — Python runner, manifest, tests

All tasks here run under Python 3.10+. Use pytest for unit tests. The runner spawns `mc2.exe` but Milestone B's tests use canned log fixtures, so you can develop without rebuilding the engine.

---

### Task 9: Manifest parser + tests

**Files:**
- Create: `scripts/smoke_lib/__init__.py` (empty file)
- Create: `scripts/smoke_lib/manifest.py`
- Create: `tests/smoke/smoke_missions.txt`
- Create: `tests/smoke/test_manifest.py`

- [ ] **Step 1: Seed the manifest with tier1 entries**

```
# tests/smoke/smoke_missions.txt
# Format: <tier> <stem> [key=value]... [reason="..."]
# Keys: duration, heartbeat_timeout_load, heartbeat_timeout_play, profile, active
# Tiers: tier1, tier2, tier3, skip

tier1 mc2_01 reason="baseline grass/desert combat"
tier1 mc2_03 reason="salvage + heavier combat"
tier1 mc2_10 reason="urban/complex objects"

skip ai_glenn reason="dev leftover"
skip gamesys reason="systems stub, not a real mission"
```

- [ ] **Step 2: Write the failing tests**

```python
# tests/smoke/test_manifest.py
from pathlib import Path
from scripts.smoke_lib.manifest import parse_manifest, Entry

SAMPLE = """
# comment
tier1 mc2_01 reason="baseline grass/desert combat"
tier1 mc2_17 duration=180 heartbeat_timeout_load=120 reason="large map"
tier2 mc2_02
skip ai_glenn reason="dev leftover"
"""

def test_parse_collects_tiered_entries(tmp_path):
    p = tmp_path / "m.txt"
    p.write_text(SAMPLE)
    entries = parse_manifest(p)
    assert [e.stem for e in entries if e.tier == "tier1"] == ["mc2_01", "mc2_17"]
    assert [e.stem for e in entries if e.tier == "tier2"] == ["mc2_02"]
    assert [e.stem for e in entries if e.tier == "skip"] == ["ai_glenn"]

def test_parse_handles_overrides(tmp_path):
    p = tmp_path / "m.txt"
    p.write_text(SAMPLE)
    entries = parse_manifest(p)
    e17 = next(e for e in entries if e.stem == "mc2_17")
    assert e17.duration == 180
    assert e17.heartbeat_timeout_load == 120
    assert e17.reason == "large map"

def test_parse_ignores_comments_and_blanks(tmp_path):
    p = tmp_path / "m.txt"
    p.write_text("# a\n\n  # b\n\ntier1 mc2_01\n")
    entries = parse_manifest(p)
    assert len(entries) == 1

def test_parse_skip_section_excluded_from_other_tiers(tmp_path):
    p = tmp_path / "m.txt"
    p.write_text("tier1 mc2_01\nskip mc2_01 reason=\"broken\"\n")
    entries = parse_manifest(p)
    # skip wins: one tier1 entry, one skip entry, runner filters skip out of tier runs.
    tiers = {e.tier for e in entries if e.stem == "mc2_01"}
    assert tiers == {"tier1", "skip"}
```

- [ ] **Step 3: Run tests to confirm they fail**

```bash
pytest tests/smoke/test_manifest.py -v
```

Expected: all four FAIL with "No module named scripts.smoke_lib.manifest" or similar.

- [ ] **Step 4: Implement `manifest.py`**

```python
# scripts/smoke_lib/manifest.py
"""MC2 smoke mission manifest parser.

Line grammar (whitespace-split with shell-style quoting):
    <tier> <stem> [key=value]... [reason="..."]

Tiers: tier1 | tier2 | tier3 | skip
Unknown keys emit a warning (stderr) but are not errors.
"""
from __future__ import annotations

import shlex
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

VALID_TIERS = {"tier1", "tier2", "tier3", "skip"}
VALID_KEYS = {"duration", "heartbeat_timeout_load", "heartbeat_timeout_play",
              "profile", "active", "reason"}


@dataclass
class Entry:
    tier: str
    stem: str
    duration: Optional[int] = None
    heartbeat_timeout_load: Optional[int] = None
    heartbeat_timeout_play: Optional[int] = None
    profile: Optional[str] = None
    active: bool = False
    reason: str = ""
    source_line: int = 0


def parse_manifest(path: Path) -> list[Entry]:
    text = Path(path).read_text()
    out: list[Entry] = []
    for lineno, raw in enumerate(text.splitlines(), start=1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        tokens = shlex.split(line, posix=True)
        if len(tokens) < 2:
            print(f"[manifest] warn: line {lineno} too short: {raw!r}", file=sys.stderr)
            continue
        tier, stem = tokens[0], tokens[1]
        if tier not in VALID_TIERS:
            print(f"[manifest] warn: line {lineno} unknown tier {tier!r}",
                  file=sys.stderr)
            continue
        e = Entry(tier=tier, stem=stem, source_line=lineno)
        for kv in tokens[2:]:
            if "=" not in kv:
                print(f"[manifest] warn: line {lineno} bad token {kv!r}",
                      file=sys.stderr)
                continue
            k, v = kv.split("=", 1)
            if k not in VALID_KEYS:
                print(f"[manifest] warn: line {lineno} unknown key {k!r}",
                      file=sys.stderr)
                continue
            if k == "duration":
                e.duration = int(v)
            elif k == "heartbeat_timeout_load":
                e.heartbeat_timeout_load = int(v)
            elif k == "heartbeat_timeout_play":
                e.heartbeat_timeout_play = int(v)
            elif k == "profile":
                e.profile = v
            elif k == "active":
                e.active = v.lower() in ("1", "true", "yes")
            elif k == "reason":
                e.reason = v
        out.append(e)
    return out
```

- [ ] **Step 5: Run tests to confirm they pass**

```bash
pytest tests/smoke/test_manifest.py -v
```

Expected: all four PASS.

- [ ] **Step 6: Commit**

```bash
git add scripts/smoke_lib/__init__.py scripts/smoke_lib/manifest.py \
        tests/smoke/smoke_missions.txt tests/smoke/test_manifest.py
git commit -m "feat(smoke): mission manifest parser with tests"
```

---

### Task 10: Log-line parser + tests

**Files:**
- Create: `scripts/smoke_lib/logparse.py`
- Create: `tests/smoke/fixtures/fake_engine_pass.log`
- Create: `tests/smoke/fixtures/fake_engine_gl_error.log`
- Create: `tests/smoke/fixtures/fake_engine_freeze_load.log`
- Create: `tests/smoke/test_logparse.py`

- [ ] **Step 1: Write fixture logs**

`tests/smoke/fixtures/fake_engine_pass.log`:
```
[INSTR v1] enabled: tgl_pool=1 destroy=0 gl_error_print=1 smoke=1 build=abc123
[SMOKE v1] event=banner mode=passive mission=mc2_01 profile=stock duration=30 seed=0xc0ffee
[SMOKE v1] event=mission_resolve stem=mc2_01 source=fst_assumed
[TIMING v1] event=profile_ready elapsed_ms=1800.0
[TIMING v1] event=logistics_ready elapsed_ms=2134.5
[TIMING v1] event=mission_load_start elapsed_ms=2135.0
[TIMING v1] event=first_frame elapsed_ms=2200.3
[TIMING v1] event=mission_ready elapsed_ms=6800.1
[HEARTBEAT] frames=60 elapsed_ms=1000 fps=60.0
[HEARTBEAT] frames=120 elapsed_ms=2000 fps=60.0
[HEARTBEAT] frames=180 elapsed_ms=3000 fps=60.0
[TIMING v1] event=mission_quit elapsed_ms=36800.0
[PERF v1] avg_fps=59.8 p50_ms=16.70 p99_ms=19.10 p1low_fps=52.4 peak_ms=24.10 samples=1794
[SMOKE v1] event=summary result=pass duration_actual_ms=30000.1 frames=1800
```

The `[PERF v1] samples` value (1794) is the perf ring-buffer count; it is NOT the same as the summary `frames=1800` (total rendered frames including pre-ready). Tests on `perf.samples` should use the former, tests on total frame count the latter.

`tests/smoke/fixtures/fake_engine_gl_error.log`:
```
[INSTR v1] enabled: tgl_pool=1 destroy=0 gl_error_print=1 smoke=1 build=abc123
[SMOKE v1] event=banner mode=passive mission=mc2_10 profile=stock duration=30 seed=0xc0ffee
[TIMING v1] event=mission_ready elapsed_ms=5000
[GL_ERROR v1] first_error=GL_INVALID_ENUM call=glDrawElements frame=143
[HEARTBEAT] frames=180 elapsed_ms=3000 fps=60.0
[SMOKE v1] event=summary result=pass duration_actual_ms=30000 frames=1800
```

`tests/smoke/fixtures/fake_engine_freeze_load.log`:
```
[INSTR v1] enabled: tgl_pool=1 destroy=0 gl_error_print=1 smoke=1 build=abc123
[SMOKE v1] event=banner mode=passive mission=mc2_17 profile=stock duration=180 seed=0xc0ffee
[TIMING v1] event=logistics_ready elapsed_ms=2134
[TIMING v1] event=mission_load_start elapsed_ms=2150
[HEARTBEAT] frames=1 elapsed_ms=100 fps=10.0
```
(Deliberately truncated — no further heartbeats, no `mission_ready`.)

- [ ] **Step 2: Write the failing tests**

```python
# tests/smoke/test_logparse.py
from pathlib import Path
from scripts.smoke_lib.logparse import parse_log, LogSummary

FIX = Path(__file__).parent / "fixtures"

def test_pass_log_counts_zero_faults():
    text = (FIX / "fake_engine_pass.log").read_text()
    # Synthesize per-line wallclocks at 0.1s intervals for this test.
    wall = [0.1 * i for i in range(len(text.splitlines()))]
    s = parse_log(text, line_wallclocks=wall)
    assert s.instr_banner_seen
    assert s.smoke_summary_result == "pass"
    assert s.gl_errors == 0
    assert s.pool_nulls == 0
    assert s.asset_oob == 0
    assert s.heartbeats_play > 0
    assert s.mission_ready_ms == 6800.1
    assert s.last_heartbeat_wall_s_play is not None
    assert s.perf.avg_fps == 59.8

def test_gl_error_detected():
    s = parse_log((FIX / "fake_engine_gl_error.log").read_text())
    assert s.gl_errors == 1

def test_freeze_load_no_mission_ready():
    text = (FIX / "fake_engine_freeze_load.log").read_text()
    wall = [0.1 * i for i in range(len(text.splitlines()))]
    s = parse_log(text, line_wallclocks=wall)
    assert s.mission_ready_ms is None
    assert s.heartbeats_load >= 1
    assert s.heartbeats_play == 0
    assert s.smoke_summary_result is None
    assert s.last_heartbeat_wall_s_load is not None

def test_crash_no_summary_detected():
    log = (
        "[INSTR v1] enabled: smoke=1\n"
        "[TIMING v1] event=mission_ready elapsed_ms=5000\n"
        "CRASH: unhandled exception at 0xdeadbeef\n"
    )
    s = parse_log(log)
    assert s.crash_handler_hit is True
    assert s.smoke_summary_result is None
```

- [ ] **Step 3: Run tests to confirm they fail**

```bash
pytest tests/smoke/test_logparse.py -v
```

Expected: four FAIL with import error.

- [ ] **Step 4: Implement `logparse.py`**

```python
# scripts/smoke_lib/logparse.py
"""Parse mc2.exe smoke-mode stdout+stderr into a structured summary."""
from __future__ import annotations

import re
from dataclasses import dataclass, field
from typing import Optional

PERF_RE = re.compile(
    r"\[PERF v1\] avg_fps=(?P<avg>[\d.]+) p50_ms=(?P<p50>[\d.]+) "
    r"p99_ms=(?P<p99>[\d.]+) p1low_fps=(?P<p1l>[\d.]+) peak_ms=(?P<peak>[\d.]+) "
    r"samples=(?P<samples>\d+)"
)
TIMING_RE = re.compile(r"\[TIMING v1\] event=(?P<ev>\w+) elapsed_ms=(?P<ms>[\d.]+)")
SUMMARY_RE = re.compile(
    r"\[SMOKE v1\] event=summary result=(?P<r>\w+)(?: reason=(?P<reason>\S+))?"
    r"(?: stage=(?P<stage>\S+))?"
)
HEARTBEAT_RE = re.compile(r"\[HEARTBEAT\] frames=(?P<f>\d+) elapsed_ms=(?P<ms>\d+)")

# Known crash / fail signatures.
CRASH_PATTERNS = (
    re.compile(r"^CRASH:", re.M),
    re.compile(r"^\[CRASHBUNDLE\]", re.M),
    re.compile(r"unhandled exception", re.I),
)
SHADER_ERROR_PATTERNS = (
    # Matches shader_builder.cpp:358 and friends.
    re.compile(r"Shader filename:.*failed to load shader", re.I),
    re.compile(r"shader.*compile.*fail", re.I),
    re.compile(r"GLSL link.*failed", re.I),
)
MISSING_FILE_PATTERNS = (
    re.compile(r"cannot open.*required", re.I),
    re.compile(r"Missing file:", re.I),
)


@dataclass
class PerfRow:
    avg_fps: float = 0.0
    p50_ms: float = 0.0
    p99_ms: float = 0.0
    p1low_fps: float = 0.0
    peak_ms: float = 0.0
    samples: int = 0


@dataclass
class LogSummary:
    instr_banner_seen: bool = False
    smoke_banner_seen: bool = False
    mission_resolve_seen: bool = False
    profile_ready_ms: Optional[float] = None
    logistics_ready_ms: Optional[float] = None
    mission_load_start_ms: Optional[float] = None
    mission_ready_ms: Optional[float] = None
    first_frame_ms: Optional[float] = None
    mission_quit_ms: Optional[float] = None
    heartbeats_load: int = 0
    heartbeats_play: int = 0
    # Runner-side wallclock (monotonic seconds since spawn) at which the most
    # recent heartbeat line was *received*. None if no heartbeat yet. These
    # fields are authoritative for freeze detection — do NOT use the engine's
    # [HEARTBEAT] elapsed_ms, which is mission-relative and drifts from
    # runner walltime during load pauses.
    last_heartbeat_wall_s_load: Optional[float] = None
    last_heartbeat_wall_s_play: Optional[float] = None
    gl_errors: int = 0
    pool_nulls: int = 0
    asset_oob: int = 0
    shader_errors: int = 0
    missing_files: int = 0
    crash_handler_hit: bool = False
    smoke_summary_result: Optional[str] = None
    smoke_summary_reason: Optional[str] = None
    smoke_summary_stage: Optional[str] = None
    destroys: int = 0
    perf: PerfRow = field(default_factory=PerfRow)


def parse_log(text: str,
              line_wallclocks: Optional[list[float]] = None) -> LogSummary:
    """Parse engine stdout.

    If `line_wallclocks` is provided, it must be the same length as text's
    line count and contain monotonic-clock seconds (since spawn) for each line.
    Used for freeze-detection gates. When None (e.g. fixture-based unit tests),
    wallclock fields remain unset and the gate evaluator can fall back to the
    summary-level walltime parameter.
    """
    s = LogSummary()
    in_play_phase = False

    lines = text.splitlines()
    for idx, line in enumerate(lines):
        line_wall = line_wallclocks[idx] if line_wallclocks and idx < len(line_wallclocks) else None
        if "[INSTR v1] enabled:" in line:
            s.instr_banner_seen = True
        if "[SMOKE v1] event=banner" in line:
            s.smoke_banner_seen = True
        if "[SMOKE v1] event=mission_resolve" in line:
            s.mission_resolve_seen = True

        m = TIMING_RE.search(line)
        if m:
            ev = m.group("ev"); ms = float(m.group("ms"))
            if ev == "profile_ready":      s.profile_ready_ms = ms
            elif ev == "logistics_ready":    s.logistics_ready_ms = ms
            elif ev == "mission_load_start": s.mission_load_start_ms = ms
            elif ev == "first_frame":       s.first_frame_ms = ms
            elif ev == "mission_ready":
                s.mission_ready_ms = ms
                in_play_phase = True
            elif ev == "mission_quit":     s.mission_quit_ms = ms

        m = HEARTBEAT_RE.search(line)
        if m:
            # Runner-side wallclock is authoritative for freeze detection.
            if in_play_phase:
                s.heartbeats_play += 1
                if line_wall is not None:
                    s.last_heartbeat_wall_s_play = line_wall
            else:
                s.heartbeats_load += 1
                if line_wall is not None:
                    s.last_heartbeat_wall_s_load = line_wall

        if "[GL_ERROR v1]" in line:        s.gl_errors += 1
        if "[TGL_POOL v1]" in line and "summary" not in line: s.pool_nulls += 1
        if "[ASSET_SCALE v1] event=oob_blit" in line: s.asset_oob += 1
        if "[DESTROY v1]" in line and "event=destroy" in line: s.destroys += 1

        for p in CRASH_PATTERNS:
            if p.search(line): s.crash_handler_hit = True; break
        for p in SHADER_ERROR_PATTERNS:
            if p.search(line): s.shader_errors += 1; break
        for p in MISSING_FILE_PATTERNS:
            if p.search(line): s.missing_files += 1; break

        m = SUMMARY_RE.search(line)
        if m:
            s.smoke_summary_result = m.group("r")
            s.smoke_summary_reason = m.group("reason")
            s.smoke_summary_stage = m.group("stage")

        m = PERF_RE.search(line)
        if m:
            s.perf = PerfRow(
                avg_fps=float(m.group("avg")),
                p50_ms=float(m.group("p50")),
                p99_ms=float(m.group("p99")),
                p1low_fps=float(m.group("p1l")),
                peak_ms=float(m.group("peak")),
                samples=int(m.group("samples")),
            )

    return s
```

- [ ] **Step 5: Run tests to confirm they pass**

```bash
pytest tests/smoke/test_logparse.py -v
```

Expected: all four PASS.

- [ ] **Step 6: Commit**

```bash
git add scripts/smoke_lib/logparse.py tests/smoke/fixtures tests/smoke/test_logparse.py
git commit -m "feat(smoke): log-line parser + fixtures + tests"
```

---

### Task 11: Fault-gate evaluator + tests

**Files:**
- Create: `scripts/smoke_lib/gates.py`
- Create: `tests/smoke/test_gates.py`

- [ ] **Step 1: Write the failing tests**

```python
# tests/smoke/test_gates.py
from scripts.smoke_lib.logparse import LogSummary, PerfRow
from scripts.smoke_lib.gates import evaluate, GateConfig, Verdict

CFG = GateConfig(heartbeat_timeout_load_s=60, heartbeat_timeout_play_s=3,
                 duration_s=30)

def _base():
    # walltime_s=31 in the tests below; set last play heartbeat at 30.5s
    # wallclock so the default 3s play gate is satisfied.
    return LogSummary(instr_banner_seen=True, smoke_banner_seen=True,
                      mission_resolve_seen=True, smoke_summary_result="pass",
                      mission_ready_ms=5000.0, heartbeats_play=30,
                      last_heartbeat_wall_s_play=30.5,
                      perf=PerfRow(avg_fps=60))

def test_clean_pass():
    v = evaluate(_base(), CFG, exit_code=0, walltime_s=31)
    assert v.passed and v.buckets == []

def test_gl_error_fails():
    s = _base(); s.gl_errors = 1
    v = evaluate(s, CFG, exit_code=0, walltime_s=31)
    assert not v.passed and "gl_error" in v.buckets

def test_missing_instr_banner_fails():
    s = _base(); s.instr_banner_seen = False
    v = evaluate(s, CFG, exit_code=0, walltime_s=31)
    assert "instrumentation_missing" in v.buckets

def test_shader_error_fails():
    s = _base(); s.shader_errors = 2
    v = evaluate(s, CFG, exit_code=0, walltime_s=31)
    assert "shader_error" in v.buckets

def test_crash_no_summary():
    s = _base(); s.crash_handler_hit = True; s.smoke_summary_result = None
    v = evaluate(s, CFG, exit_code=-1, walltime_s=31)
    assert "crash_no_summary" in v.buckets

def test_heartbeat_freeze_play():
    s = _base()
    # Last play heartbeat at wallclock 20s; walltime 31s ⇒ 11s gap > 3s cfg.
    s.last_heartbeat_wall_s_play = 20.0
    v = evaluate(s, CFG, exit_code=0, walltime_s=31)
    assert "heartbeat_freeze_play" in v.buckets

def test_timeout_bucket_when_walltime_cap_hit():
    s = _base()
    v = evaluate(s, CFG, exit_code=-9, walltime_s=90, killed_by_timeout=True)
    assert v.buckets == ["timeout"]

def test_multiple_buckets_reported():
    s = _base(); s.gl_errors = 1; s.asset_oob = 1
    v = evaluate(s, CFG, exit_code=0, walltime_s=31)
    assert set(v.buckets) >= {"gl_error", "asset_oob"}
```

- [ ] **Step 2: Run tests to confirm they fail**

```bash
pytest tests/smoke/test_gates.py -v
```

Expected: all FAIL with import error.

- [ ] **Step 3: Implement `gates.py`**

```python
# scripts/smoke_lib/gates.py
"""Fault-gate evaluator. Maps LogSummary → pass/fail + bucket list."""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import List, Optional

from .logparse import LogSummary


@dataclass
class GateConfig:
    heartbeat_timeout_load_s: int = 60
    heartbeat_timeout_play_s: int = 3
    duration_s: int = 120


@dataclass
class Verdict:
    passed: bool
    buckets: List[str] = field(default_factory=list)
    details: List[str] = field(default_factory=list)


def evaluate(s: LogSummary, cfg: GateConfig, *,
             exit_code: int, walltime_s: float,
             killed_by_timeout: bool = False,
             final_elapsed_ms: Optional[int] = None) -> Verdict:
    buckets: List[str] = []
    details: List[str] = []

    if killed_by_timeout:
        buckets.append("timeout")
        details.append(f"walltime cap hit at {walltime_s:.1f}s")
        return Verdict(passed=False, buckets=buckets, details=details)

    if not s.instr_banner_seen:
        buckets.append("instrumentation_missing")
        details.append("[INSTR v1] banner absent")

    if s.crash_handler_hit and s.smoke_summary_result is None:
        buckets.append("crash_no_summary")
    elif s.smoke_summary_result == "fail":
        buckets.append("engine_reported_fail")
        details.append(f"reason={s.smoke_summary_reason} stage={s.smoke_summary_stage}")
    elif s.smoke_summary_result is None and exit_code != 0:
        buckets.append("crash_silent")

    if s.gl_errors > 0:
        buckets.append("gl_error"); details.append(f"{s.gl_errors} errors")
    if s.pool_nulls > 0:
        buckets.append("pool_null"); details.append(f"{s.pool_nulls} NULLs")
    if s.asset_oob > 0:
        buckets.append("asset_oob"); details.append(f"{s.asset_oob} oob")
    if s.shader_errors > 0:
        buckets.append("shader_error"); details.append(f"{s.shader_errors}")
    if s.missing_files > 0:
        buckets.append("missing_file"); details.append(f"{s.missing_files}")

    # Heartbeat freeze gates — wallclock-based (runner-side). Engine-side
    # [HEARTBEAT] elapsed_ms is mission-relative and cannot be compared to
    # runner walltime during load pauses; see logparse.py docstring.
    if s.mission_ready_ms is None and s.heartbeats_load > 0:
        last = s.last_heartbeat_wall_s_load or 0.0
        if walltime_s - last > cfg.heartbeat_timeout_load_s:
            buckets.append("heartbeat_freeze_load")
            details.append(f"last load heartbeat at {last:.1f}s wallclock")
    elif s.mission_ready_ms is not None:
        last = s.last_heartbeat_wall_s_play
        if last is None:
            # No play-phase heartbeat at all — definite freeze.
            buckets.append("heartbeat_freeze_play")
            details.append("no play-phase heartbeat observed")
        elif walltime_s - last > cfg.heartbeat_timeout_play_s:
            buckets.append("heartbeat_freeze_play")
            details.append(f"last play heartbeat at {last:.1f}s wallclock")

    passed = not buckets and s.smoke_summary_result == "pass" and exit_code == 0
    return Verdict(passed=passed, buckets=buckets, details=details)
```

- [ ] **Step 4: Run tests to confirm they pass**

```bash
pytest tests/smoke/test_gates.py -v
```

Expected: all PASS.

- [ ] **Step 5: Commit**

```bash
git add scripts/smoke_lib/gates.py tests/smoke/test_gates.py
git commit -m "feat(smoke): fault-gate evaluator with bucket taxonomy + tests"
```

---

### Task 12: Report writer + tests + baseline store

**Files:**
- Create: `scripts/smoke_lib/report.py`
- Create: `scripts/smoke_lib/baselines.py`
- Create: `tests/smoke/test_report.py`

- [ ] **Step 1: Write the failing tests**

```python
# tests/smoke/test_report.py
from scripts.smoke_lib.logparse import LogSummary, PerfRow
from scripts.smoke_lib.gates import Verdict
from scripts.smoke_lib.report import Row, render_markdown, render_json

def test_markdown_contains_header_and_rows():
    rows = [
        Row(stem="mc2_01", verdict=Verdict(passed=True), summary=LogSummary(
            perf=PerfRow(avg_fps=142, p1low_fps=58), mission_ready_ms=4300),
            destroys_delta=+2),
        Row(stem="mc2_10", verdict=Verdict(passed=False, buckets=["gl_error"]),
            summary=LogSummary(perf=PerfRow(avg_fps=119, p1low_fps=32),
                               mission_ready_ms=6800), destroys_delta=0),
    ]
    md = render_markdown(rows, tier="tier1", profile="stock",
                         timestamp="2026-04-23T14-32-07")
    assert "# Smoke run" in md
    assert "| mc2_01  | PASS" in md
    assert "| mc2_10  | FAIL" in md
    assert "gl_error" in md

def test_json_roundtrips_verdict():
    rows = [Row(stem="mc2_01", verdict=Verdict(passed=True),
                summary=LogSummary(perf=PerfRow(avg_fps=60)), destroys_delta=0)]
    d = render_json(rows, tier="tier1", profile="stock", timestamp="t")
    assert d["tier"] == "tier1"
    assert d["rows"][0]["stem"] == "mc2_01"
    assert d["rows"][0]["result"] == "PASS"
```

- [ ] **Step 2: Run tests to confirm they fail.**

```bash
pytest tests/smoke/test_report.py -v
```

- [ ] **Step 3: Implement `report.py` and `baselines.py`**

```python
# scripts/smoke_lib/report.py
"""Markdown + JSON report rendering."""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Optional

from .logparse import LogSummary
from .gates import Verdict


@dataclass
class Row:
    stem: str
    verdict: Verdict
    summary: LogSummary
    destroys_delta: int = 0


def render_markdown(rows: list[Row], *, tier: str, profile: str,
                    timestamp: str) -> str:
    passed = sum(1 for r in rows if r.verdict.passed)
    total = len(rows)
    head = (f"# Smoke run {timestamp}  tier={tier}  profile={profile}  "
            f"result={'PASS' if passed == total else 'FAIL'} ({passed}/{total} passed)\n\n")
    table = [
        "| Mission | Result | Bucket                 | Frames | Avg FPS | p1% | Load ms | Δ destroys |",
        "|---------|--------|------------------------|--------|---------|-----|---------|-----------|"
    ]
    failures = []
    for r in rows:
        status = "PASS" if r.verdict.passed else "FAIL"
        bucket = ",".join(r.verdict.buckets)
        frames = r.summary.perf.samples
        avg = f"{r.summary.perf.avg_fps:.0f}" if r.summary.perf.avg_fps else "-"
        p1 = f"{r.summary.perf.p1low_fps:.0f}" if r.summary.perf.p1low_fps else "-"
        # Load column = ms to gameplay-ready (mission_ready), NOT first_frame,
        # which can fire during the logistics screen before gameplay starts.
        load = f"{r.summary.mission_ready_ms:.0f}" if r.summary.mission_ready_ms else "-"
        d = f"{r.destroys_delta:+d}" if r.destroys_delta is not None else "-"
        table.append(f"| {r.stem:<7} | {status:<6} | {bucket:<22} | "
                     f"{frames:<6} | {avg:<7} | {p1:<3} | {load:<7} | {d:<9} |")
        if not r.verdict.passed:
            failures.append(f"### {r.stem} — {bucket}\n" +
                            "\n".join(r.verdict.details))
    body = "\n".join(table) + "\n"
    if failures:
        body += "\n## Failures\n" + "\n\n".join(failures) + "\n"
    return head + body


def render_json(rows: list[Row], *, tier: str, profile: str,
                timestamp: str) -> dict:
    return {
        "timestamp": timestamp,
        "tier": tier,
        "profile": profile,
        "rows": [
            {
                "stem": r.stem,
                "result": "PASS" if r.verdict.passed else "FAIL",
                "buckets": r.verdict.buckets,
                "details": r.verdict.details,
                "avg_fps": r.summary.perf.avg_fps,
                "p1low_fps": r.summary.perf.p1low_fps,
                "mission_ready_ms": r.summary.mission_ready_ms,
                "destroys_delta": r.destroys_delta,
            }
            for r in rows
        ],
    }
```

```python
# scripts/smoke_lib/baselines.py
"""Baseline load/store keyed by <profile>@<stem>@<tier>@<duration>."""
from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
from typing import Optional


def key(profile: str, stem: str, tier: str, duration: int) -> str:
    return f"{profile}@{stem}@{tier}@{duration}"


def load(path: Path) -> dict:
    if not path.exists(): return {}
    return json.loads(path.read_text())


def save(path: Path, data: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, sort_keys=True))


def destroys_delta(baselines: dict, key_str: str, observed: int) -> Optional[int]:
    b = baselines.get(key_str, {}).get("destroys", {})
    mean = b.get("mean")
    if mean is None: return None
    return observed - int(mean)
```

- [ ] **Step 4: Run tests to confirm they pass.**

```bash
pytest tests/smoke/test_report.py -v
```

- [ ] **Step 5: Commit.**

```bash
git add scripts/smoke_lib/report.py scripts/smoke_lib/baselines.py \
        tests/smoke/test_report.py
git commit -m "feat(smoke): report rendering + baseline store with tests"
```

---

### Task 13: Runner core (spawn + parse + timeout)

**Files:**
- Create: `scripts/smoke_lib/runner.py`
- Create: `tests/smoke/test_runner.py`

- [ ] **Step 1: Write tests with a stub engine script**

`tests/smoke/fixtures/fake_mc2_pass.py` (a Python stand-in for mc2.exe that the runner can spawn):

```python
# tests/smoke/fixtures/fake_mc2_pass.py
import sys, time, argparse

ap = argparse.ArgumentParser()
ap.add_argument("--profile"); ap.add_argument("--mission"); ap.add_argument("--duration", type=int, default=1)
a = ap.parse_args()

print("[INSTR v1] enabled: tgl_pool=0 destroy=0 gl_error_print=1 smoke=1 build=fake", flush=True)
print(f"[SMOKE v1] event=banner mode=passive mission={a.mission} profile={a.profile} duration={a.duration} seed=0xc0ffee", flush=True)
print(f"[TIMING v1] event=mission_ready elapsed_ms=500", flush=True)
for i in range(a.duration):
    print(f"[HEARTBEAT] frames={60*(i+1)} elapsed_ms={1000*(i+1)} fps=60.0", flush=True)
    time.sleep(1)
print("[PERF v1] avg_fps=60.0 p50_ms=16.70 p99_ms=19.10 p1low_fps=52.4 peak_ms=24.10 samples=120", flush=True)
print(f"[SMOKE v1] event=summary result=pass duration_actual_ms={1000*a.duration} frames={60*a.duration}", flush=True)
```

`tests/smoke/fixtures/fake_mc2_hang.py` (for the timeout test — ignores argv and hangs):

```python
# tests/smoke/fixtures/fake_mc2_hang.py
import sys, time
print("[INSTR v1] enabled: smoke=1 build=fake", flush=True)
print("[SMOKE v1] event=banner mode=passive mission=hang profile=stock duration=1 seed=0x0", flush=True)
# Intentionally hang past any runner cap.
time.sleep(600)
```

```python
# tests/smoke/test_runner.py
import sys
from pathlib import Path
from scripts.smoke_lib.runner import run_one, RunConfig

FIX = Path(__file__).parent / "fixtures"

def test_run_fake_pass(tmp_path):
    cfg = RunConfig(
        exe=[sys.executable, str(FIX / "fake_mc2_pass.py")],
        profile="stock", stem="mc2_01", duration=1,
        heartbeat_timeout_load_s=60, heartbeat_timeout_play_s=3,
        grace_s=5, env_extra={},
    )
    result = run_one(cfg)
    assert result.verdict.passed
    assert result.summary.smoke_summary_result == "pass"
    assert not result.killed_by_timeout

def test_run_fake_timeout(tmp_path):
    # Use a second fixture script that hangs regardless of --duration, so the
    # runner's walltime cap is what trips. duration=1 grace=0 ⇒ cap=1s total.
    cfg = RunConfig(
        exe=[sys.executable, str(FIX / "fake_mc2_hang.py")],
        profile="stock", stem="mc2_01", duration=1,
        heartbeat_timeout_load_s=60, heartbeat_timeout_play_s=3,
        grace_s=0, env_extra={},
    )
    result = run_one(cfg)
    assert result.killed_by_timeout
    assert "timeout" in result.verdict.buckets
```

- [ ] **Step 2: Run tests to confirm they fail.**

```bash
pytest tests/smoke/test_runner.py -v
```

- [ ] **Step 3: Implement `runner.py`**

```python
# scripts/smoke_lib/runner.py
"""Per-mission spawn/parse/timeout loop."""
from __future__ import annotations

import os
import subprocess
import time
from dataclasses import dataclass, field
from typing import List, Optional

from .gates import GateConfig, Verdict, evaluate
from .logparse import LogSummary, parse_log


@dataclass
class RunConfig:
    exe: List[str]                      # argv[0..n]; usually [exe_path, --profile, stock, --mission, stem, --duration, d]
    profile: str
    stem: str
    duration: int
    heartbeat_timeout_load_s: int
    heartbeat_timeout_play_s: int
    grace_s: int                        # walltime cap = duration + grace
    env_extra: dict


@dataclass
class RunResult:
    summary: LogSummary
    verdict: Verdict
    stdout_text: str
    exit_code: int
    walltime_s: float
    killed_by_timeout: bool = False


def _build_argv(base_exe: List[str], cfg: RunConfig) -> List[str]:
    return list(base_exe) + [
        "--profile", cfg.profile,
        "--mission", cfg.stem,
        "--duration", str(cfg.duration),
    ]


def run_one(cfg: RunConfig) -> RunResult:
    # If exe already contains `--mission` (fake engines may embed it), don't re-append.
    argv = cfg.exe if any("--mission" in a for a in cfg.exe) else _build_argv(cfg.exe, cfg)

    env = os.environ.copy()
    env["MC2_SMOKE_MODE"] = "1"
    env["MC2_HEARTBEAT"] = "1"
    env["MC2_TGL_POOL_TRACE"] = "1"
    env["MC2_ASSET_SCALE_TRACE"] = "1"
    env.pop("MC2_GL_ERROR_DRAIN_SILENT", None)
    env.update(cfg.env_extra)

    cap = cfg.duration + cfg.grace_s
    t0 = time.monotonic()
    proc = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            env=env, text=True, bufsize=1)

    # Stream lines and record per-line wallclock so freeze detection can use
    # runner walltime rather than engine-emitted elapsed_ms (which is mission-
    # relative and drifts during load pauses).
    lines: list[str] = []
    line_wallclocks: list[float] = []
    killed = False
    try:
        while True:
            if time.monotonic() - t0 > cap:
                proc.kill()
                killed = True
                break
            line = proc.stdout.readline() if proc.stdout else ""
            if not line:
                # process exited
                if proc.poll() is not None:
                    break
                continue
            lines.append(line.rstrip("\n"))
            line_wallclocks.append(time.monotonic() - t0)
    finally:
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=2)
    walltime = time.monotonic() - t0
    stdout = "\n".join(lines)

    summary = parse_log(stdout, line_wallclocks=line_wallclocks)
    gcfg = GateConfig(
        heartbeat_timeout_load_s=cfg.heartbeat_timeout_load_s,
        heartbeat_timeout_play_s=cfg.heartbeat_timeout_play_s,
        duration_s=cfg.duration,
    )
    verdict = evaluate(summary, gcfg, exit_code=proc.returncode,
                       walltime_s=walltime, killed_by_timeout=killed)
    return RunResult(summary=summary, verdict=verdict, stdout_text=stdout or "",
                     exit_code=proc.returncode, walltime_s=walltime,
                     killed_by_timeout=killed)
```

- [ ] **Step 4: Run tests to confirm they pass.**

```bash
pytest tests/smoke/test_runner.py -v
```

- [ ] **Step 5: Commit.**

```bash
git add scripts/smoke_lib/runner.py tests/smoke/test_runner.py \
        tests/smoke/fixtures/fake_mc2_pass.py \
        tests/smoke/fixtures/fake_mc2_hang.py
git commit -m "feat(smoke): runner core with fake-engine integration test"
```

---

### Task 14: CLI wiring `run_smoke.py` (kill-existing, tiers, fail-fast, artifacts)

**Files:**
- Create: `scripts/run_smoke.py`
- Create: `tests/smoke/baselines.json` (empty JSON `{}`)

- [ ] **Step 1: Create the baselines stub**

```bash
mkdir -p tests/smoke
printf '{}\n' > tests/smoke/baselines.json
```

- [ ] **Step 2: Write the CLI**

```python
#!/usr/bin/env python3
# scripts/run_smoke.py
"""MC2 smoke matrix runner.

Examples:
  python scripts/run_smoke.py --tier tier1 --fail-fast
  python scripts/run_smoke.py --tier tier2
  python scripts/run_smoke.py --tier tier3 --kill-existing
  python scripts/run_smoke.py --mission mc2_01 --mission mc2_03
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from scripts.smoke_lib import baselines, manifest, report
from scripts.smoke_lib.runner import RunConfig, run_one

DEFAULT_EXE = Path(r"A:/Games/mc2-opengl/mc2-win64-v0.1.1/mc2.exe")
ARTIFACT_ROOT = ROOT / "tests" / "smoke" / "artifacts"
MANIFEST_PATH = ROOT / "tests" / "smoke" / "smoke_missions.txt"
BASELINE_PATH = ROOT / "tests" / "smoke" / "baselines.json"


def _running_mc2() -> list[int]:
    try:
        out = subprocess.check_output(
            ["tasklist", "/FI", "IMAGENAME eq mc2.exe", "/NH", "/FO", "CSV"],
            text=True, stderr=subprocess.DEVNULL)
    except Exception:
        return []
    pids = []
    for line in out.splitlines():
        if "mc2.exe" in line:
            parts = [p.strip('"') for p in line.split(",")]
            if len(parts) > 1 and parts[1].isdigit():
                pids.append(int(parts[1]))
    return pids


def _taskkill_mc2():
    subprocess.run(["taskkill", "/F", "/IM", "mc2.exe"],
                   stderr=subprocess.DEVNULL, stdout=subprocess.DEVNULL)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tier", choices=["tier1", "tier2", "tier3"])
    ap.add_argument("--mission", action="append", default=[])
    ap.add_argument("--fail-fast", action="store_true")
    ap.add_argument("--continue", dest="cont", action="store_true", default=True)
    ap.add_argument("--keep-logs", action="store_true")
    ap.add_argument("--baseline-update", action="store_true")
    ap.add_argument("--kill-existing", action="store_true")
    ap.add_argument("--duration", type=int)
    ap.add_argument("--profile", default="stock")
    ap.add_argument("--exe", default=str(DEFAULT_EXE))
    args = ap.parse_args()

    # Existing-process safety.
    pids = _running_mc2()
    if pids:
        if args.kill_existing:
            print(f"[runner] killing existing mc2.exe PIDs {pids}", file=sys.stderr)
            _taskkill_mc2()
        else:
            print(f"[runner] ERROR: mc2.exe already running (PIDs {pids}); "
                  f"pass --kill-existing to override.", file=sys.stderr)
            sys.exit(4)

    entries = manifest.parse_manifest(MANIFEST_PATH)
    if args.mission:
        selected = [e for e in entries if e.stem in args.mission and e.tier != "skip"]
    elif args.tier:
        selected = [e for e in entries if e.tier == args.tier]
    else:
        ap.error("--tier or --mission required")

    if not selected:
        print("[runner] no missions selected", file=sys.stderr)
        sys.exit(0)

    baseline_data = baselines.load(BASELINE_PATH)
    timestamp = dt.datetime.now().strftime("%Y-%m-%dT%H-%M-%S")
    artifact_dir = ARTIFACT_ROOT / timestamp
    artifact_dir.mkdir(parents=True, exist_ok=True)

    rows: list[report.Row] = []
    for e in selected:
        duration = args.duration or e.duration or 120
        tier = args.tier or "adhoc"
        cfg = RunConfig(
            exe=[args.exe],
            profile=e.profile or args.profile,
            stem=e.stem,
            duration=duration,
            heartbeat_timeout_load_s=e.heartbeat_timeout_load or 60,
            heartbeat_timeout_play_s=e.heartbeat_timeout_play or 3,
            grace_s=60,
            env_extra={"MC2_SMOKE_SEED": "0xC0FFEE"},
        )
        print(f"[runner] running {e.stem} (tier={tier} duration={duration})",
              file=sys.stderr)
        result = run_one(cfg)

        key = baselines.key(cfg.profile, e.stem, tier, duration)
        delta = baselines.destroys_delta(baseline_data, key, result.summary.destroys)

        rows.append(report.Row(stem=e.stem, verdict=result.verdict,
                               summary=result.summary, destroys_delta=delta or 0))

        if not result.verdict.passed or args.keep_logs:
            (artifact_dir / f"{e.stem}.log").write_text(result.stdout_text)
        if args.baseline_update and result.verdict.passed:
            baseline_data.setdefault(key, {})["destroys"] = {
                "mean": result.summary.destroys, "stddev": 0, "samples": 1,
                "updated": timestamp,
            }
            baseline_data[key]["perf"] = {
                "avg_fps": result.summary.perf.avg_fps,
                "p1low_fps": result.summary.perf.p1low_fps,
                "peak_ms": result.summary.perf.peak_ms,
            }

        if args.fail_fast and not result.verdict.passed:
            print(f"[runner] --fail-fast: stopping at {e.stem}", file=sys.stderr)
            break

        # 2s grace for PDB lock release before next spawn.
        import time as _t; _t.sleep(2)

    md = report.render_markdown(rows, tier=args.tier or "adhoc",
                                 profile=args.profile, timestamp=timestamp)
    (artifact_dir / "report.md").write_text(md)
    (artifact_dir / "report.json").write_text(
        json.dumps(report.render_json(rows, tier=args.tier or "adhoc",
                                      profile=args.profile, timestamp=timestamp),
                   indent=2))

    if args.baseline_update:
        baselines.save(BASELINE_PATH, baseline_data)

    print(md)
    passed = all(r.verdict.passed for r in rows)
    sys.exit(0 if passed else 1)


if __name__ == "__main__":
    main()
```

- [ ] **Step 3: Make it executable + smoke-test dry syntax**

```bash
chmod +x scripts/run_smoke.py
python scripts/run_smoke.py --help
```

Expected: usage output, no tracebacks.

- [ ] **Step 4: Unit-run the full pytest suite**

```bash
pytest tests/smoke/ -v
```

Expected: every test passes.

- [ ] **Step 5: Commit + tag milestone**

```bash
git add scripts/run_smoke.py tests/smoke/baselines.json
git commit -m "feat(smoke): run_smoke.py CLI with kill-existing, tiers, artifacts"
git tag smoke-milestone-b
```

---

## Milestone C — Integration

### Task 15: End-to-end tier1 run against real `mc2.exe`

**Files:**
- None modified. This is a live-run verification step.

- [ ] **Step 1: Pre-flight**

```bash
# From the worktree root:
ls A:/Games/mc2-opengl/mc2-win64-v0.1.1/mc2.exe     # deployed build exists
tasklist | grep mc2 || true                          # no running instance
```

- [ ] **Step 2: Run tier1, fail-fast off (expect first-run failures)**

```bash
python scripts/run_smoke.py --tier tier1 --keep-logs
```

Expected: some missions may fail on first run (missing `setCurrentMission` setter, timing hook mismatch, etc.). Capture the report.

- [ ] **Step 3: Iterate on engine-side issues until tier1 is green**

For each failure:
1. Open `tests/smoke/artifacts/<timestamp>/<stem>.log`.
2. Identify the bucket and root cause (grep for the signal line).
3. File an issue note in `docs/superpowers/plans/progress/2026-04-23-smoke-iter.md`.
4. Fix, rebuild, redeploy, rerun.

Common first-run issues to expect:
- `heartbeat_freeze_load` on slower maps → widen manifest override for that stem.
- `crash_silent` at mission_load_start → a `setCurrentMission`/`setSkipLogistics` fieldname doesn't exist; find the real field in `code/logisticsdata.cpp`.
- `instrumentation_missing` → banner emitted too late, move the `snprintf` block earlier or verify stdout isn't being buffered past the crash.

- [ ] **Step 4: Record baselines once tier1 is stable**

```bash
python scripts/run_smoke.py --tier tier1 --baseline-update
cat tests/smoke/baselines.json
```

Expected: 3 entries (one per tier1 mission) under keys `stock@mc2_01@tier1@120` etc.

- [ ] **Step 5: Commit baselines**

```bash
git add tests/smoke/baselines.json
git commit -m "chore(smoke): initial tier1 baselines"
```

---

### Task 16: Populate tier2 (full campaign) in manifest

**Files:**
- Modify: `tests/smoke/smoke_missions.txt`

- [ ] **Step 1: Enumerate campaign missions**

```bash
ls A:/Games/mc2-opengl-src/mc2srcdata/missions/ | grep -E "^mc2_[0-9]+\.fit$" | sed 's/\.fit$//'
```

- [ ] **Step 2: Append tier2 block to manifest**

For each stem from step 1 not already in tier1, add a line:

```
tier2 mc2_02
tier2 mc2_04
tier2 mc2_05
... (one per line)
```

Leave `reason="…"` optional; only add it for missions you know are special (long loads, water biomes, urban maps needing overrides).

- [ ] **Step 3: Dry-run the tier2 list**

```bash
python scripts/run_smoke.py --tier tier2 --mission mc2_02  # sanity single
```

Expected: runs one mission, reports pass/fail.

- [ ] **Step 4: Commit**

```bash
git add tests/smoke/smoke_missions.txt
git commit -m "chore(smoke): populate tier2 (full campaign) in manifest"
```

---

### Task 17: Populate tier3 + record tier2/tier3 baselines

**Files:**
- Modify: `tests/smoke/smoke_missions.txt`
- Modify: `tests/smoke/baselines.json`

- [ ] **Step 1: Enumerate all non-campaign missions**

```bash
ls A:/Games/mc2-opengl-src/mc2srcdata/missions/ | grep -E "\.fit$" | sed 's/\.fit$//' > /tmp/all_missions.txt
```

Review the list. For each stem not already in tier1/tier2 and not obviously broken, add `tier3 <stem>`. For known-broken dev leftovers, add `skip <stem> reason="..."`.

- [ ] **Step 2: Run tier2 end-to-end, record baselines**

```bash
python scripts/run_smoke.py --tier tier2 --baseline-update --kill-existing
```

Runtime ≈ 60 min.

- [ ] **Step 3: Run tier3 end-to-end, record baselines**

```bash
python scripts/run_smoke.py --tier tier3 --baseline-update --kill-existing
```

Runtime ≈ 170 min.

- [ ] **Step 4: Commit**

```bash
git add tests/smoke/smoke_missions.txt tests/smoke/baselines.json
git commit -m "chore(smoke): populate tier3 manifest + baselines for tier2/tier3"
git tag smoke-milestone-c
```

**End of plan.** The matrix is live. Developer usage:

- Local pre-push: `python scripts/run_smoke.py --tier tier1 --fail-fast`
- Pre-PR: `python scripts/run_smoke.py --tier tier2`
- Pre-release / nightly: `python scripts/run_smoke.py --tier tier3 --kill-existing`

---

## Follow-up plans (out of scope for this plan)

- **Menu canary (`scripts/smoke_menu_canary.py`)** — separate plan; touches pyautogui scripting + UI click coordinates. Runs opt-in via `--menu-canary` flag on `run_smoke.py` (add the flag when that plan lands).
- **Phase 2 active smoke (`--smoke-active`)** — separate plan; implements the in-engine autopilot (iterate movers, issue MOVE at +10s, nearest-enemy ATTACK at +30s). The CLI flag is already accepted in this plan but inert.
- **Screenshot diffing (Tier-C)** — separate plan; baseline screenshots per mission, perceptual diff on run.
- **Shader-compile bucket tightening** — extend `SHADER_ERROR_PATTERNS` in `logparse.py` as real failures surface in tier3 runs.

---

## Spec coverage self-check

| Spec section | Implemented in |
|---|---|
| §3 Architecture / components | Tasks 1, 9–14 |
| §4.1 Fail gates (binary) | Task 11 |
| §4.2 Logged-not-fail (DESTROY, perf) | Tasks 7, 12 |
| §4.3 Fail bucket taxonomy (incl. shader_error, instrumentation_missing) | Task 11 |
| §5.1 CLI surface | Tasks 2, 3 |
| §5.2 Stem resolution | Task 4 |
| §5.3 Init chain hook into Logistics (incl. profile_ready) | Tasks 6a (exploration), 6b (implementation) |
| §5.4 Deterministic seed | Task 2 (env parse) |
| §5.5 [SMOKE v1] line taxonomy | Tasks 2, 4, 6b, 7, 8 |
| §5.6 Perf emission | Task 7 |
| §5.7 Phase 2 autopilot flag accepted but inert | Task 2 |
| §6.1 Runner CLI | Task 14 |
| §6.2 Existing-process safety (--kill-existing) | Task 14 |
| §6.3 Per-mission execution loop | Task 13 |
| §6.4 Failure mode (continue-by-default, --fail-fast) | Task 14 |
| §6.5 Artifact layout | Task 14 |
| §6.6 Baselines with <profile>@<stem>@<tier>@<duration> key | Tasks 12, 14 |
| §6.7 Report format | Task 12 |
| §7 Manifest format | Task 9 |
| §8 Menu canary | Out of scope; follow-up plan |
| §9 Interaction with existing instrumentation | Task 13 (env setup) |
