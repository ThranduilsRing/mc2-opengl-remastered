# Smoke launch entry — research notes for Task 6b

Task 6a output. Read this before implementing Task 6b.

## 1. Launch callback wiring

- File: `code/missionbegin.cpp:739`
- Function signature: `int _stdcall Logistics::beginMission(void*, int, void*[])` (code/logistics.cpp:458)
- How the Launch button invokes it:
  The "Launch" button is on the `LoadScreen` (at slot `singlePlayerScreens[4][1]` in `MissionBegin`). When the load-door animation finishes, `LoadScreen::update()` sets `status = READYTOLOAD` (code/loadscreen.cpp:469). `MissionBegin::update()` polls `pCurScreen->getStatus() == LogisticsScreen::READYTOLOAD` and sets `bReadyToLoad = true` when `curScreenX == 4` (code/missionbegin.cpp:739–741). `Logistics::update()` polls `missionBegin->readyToLoad()` and calls `beginMission(0, 0, 0)` directly (code/logistics.cpp:415–421). There is no aObject callback table entry; the function pointer form of the signature exists only for historical reasons. In the single-player path it is always called as a plain member call `beginMission(0, 0, 0)`.

## 2. LogisticsData active-mission field

- File: `code/logisticsmissioninfo.h:174`
- Declaration: `EString currentMissionName;` (private field inside `LogisticsMissionInfo`)
- Visibility: private (inside `LogisticsMissionInfo`, which is private to `LogisticsData` via `missionInfo` pointer)
- Setter (if any): `LogisticsData::setCurrentMission(const char* missionName)` at code/logisticsdata.cpp:762. This calls `missionInfo->setNextMission(missionName)`, which opens the `.fit` file and fills all `MissionInfo` fields including `playLogistics`.
- Notes: The campaign path (normal flow) navigates through `MissionGroup::infos[currentMission]` (an index, not a string). The smoke path should call `LogisticsData::instance->setSingleMission(stem)` — this sets `currentMissionName = stem`, `currentMission = 0`, `currentStage = 0`, and reads the `.fit` file. After `setSingleMission`, `getCurrentMission()` returns the stem as an `EString`. `beginMission()` uses `LogisticsData::instance->getCurrentMission()` directly at logistics.cpp:593.

## 3. skipLogistics() backing

- File: `code/logisticsmissioninfo.cpp:1096`
- Return type: `bool`
- Backed by: `bool playLogistics` — a private field inside the private `MissionInfo` struct at `logisticsmissioninfo.h:135`. Accessed as `groups[currentStage].infos[currentMission]->playLogistics`.
- How normal UI flow sets it: read from the `.fit` file key `PlayLogistics` (boolean) during `readMissionInfo()`. Defaults to `true` if the key is absent (logisticsmissioninfo.cpp:232–233).
- How smoke mode should set it: **There is no public setter.** Task 6b must add one. The cleanest approach is to add a public method to `LogisticsMissionInfo`:

  ```cpp
  // In logisticsmissioninfo.h (public section):
  void setSkipLogistics(bool skip);

  // In logisticsmissioninfo.cpp:
  void LogisticsMissionInfo::setSkipLogistics(bool skip) {
      if (currentStage < groupCount && !groups[currentStage].infos.IsEmpty())
          groups[currentStage].infos[currentMission]->playLogistics = !skip;
  }
  ```

  And a passthrough on `LogisticsData`:

  ```cpp
  // In logisticsdata.h (public section):
  void setSkipLogistics(bool skip);

  // In logisticsdata.cpp:
  void LogisticsData::setSkipLogistics(bool skip) {
      missionInfo->setSkipLogistics(skip);
  }
  ```

  Call as: `LogisticsData::instance->setSkipLogistics(true);`

  Alternative (no new setter, slightly more fragile): call `LogisticsData::instance->setSingleMission(stem)` — `setSingleMission` calls `readMissionInfo`, which will read `PlayLogistics=FALSE` from the `.fit` if the author placed it there. For the standard campaign missions, `PlayLogistics` is absent so it defaults to `true`, meaning the logistics screen will still be shown. A setter is required.

## 4. mission_ready predicate

- File: `code/mission.h:376` (where `isActive()` is declared)
- Expression to use in the frame loop: `mission && mission->isActive()`
- Rationale: `Mission::active` is set to `true` exclusively in `Mission::start()` (code/mission.cpp:3054), which is called only after the full mission `.fit` load and `beginMission()` completes. It is `false` during all loading screens and `false` during the logistics UI. The field is `bool`; `isActive()` returns it directly with no side effects.
- Cost: O(1) — pointer null-check plus a single bool field read.
- Edge cases: `mission` is a global pointer (`code/mission.h:402`, set to `NULL` before first init in `code/mission.cpp:138`). The null-guard is required. After a mission ends, `Mission::active` is reset to `false` by `Mission::destroy()`, so the one-shot static guard `s_missionReadyMarked` in the frame loop is load-bearing — without it, `markMissionReady()` would fire again on the next mission.

  Note: `Mission::start()` already calls `mission_phase_mark("mission_ready")` (code/mission.cpp:3055), which goes to `gameosmain.cpp:546`'s implementation and prints `[MISSION] t=... phase=mission_ready`. `SmokeMode::markMissionReady()` is separate and emits `[TIMING v1] event=mission_ready elapsed_ms=...` from `gos_smoke.cpp:159–164`. Both should fire; the `mission_phase_mark` path is pre-existing and not under our control.

## 5. Proposed Task 6b code

Based on findings 1–4, the Task 6b smoke-mode branch inside `Logistics::update()` (top of the function, before the movie/state-machine block) should look like:

```cpp
// Smoke-mode auto-launch hook (Task 6b)
#include "gos_smoke.h"
static bool s_smokeEntered = false;
if (SmokeMode::state().enabled && !s_smokeEntered) {
    s_smokeEntered = true;
    SmokeMode::emitTiming("logistics_ready");
    SmokeMode::resolveMissionPaths();

    // Set active mission and skip logistics UI:
    LogisticsData::instance->setSingleMission(
        SmokeMode::state().mission.c_str());
    LogisticsData::instance->setSkipLogistics(true); // NEW setter, see section 3

    SmokeMode::emitTiming("mission_load_start");
    int rc = beginMission(nullptr, 0, nullptr);
    if (rc != 0) {
        SmokeMode::emitFailSummary("mission_begin_failed", "logistics");
        std::exit(3);
    }
}
```

Note: `beginMission` is a static member, so it can be called as `Logistics::beginMission(nullptr, 0, nullptr)` or via `beginMission(nullptr, 0, nullptr)` from within the class. `rc != 0` is the failure condition — `beginMission` returns 0 on success based on code/logistics.cpp:421 where `!beginMission(0,0,0)` is the failure branch.

Also for profile_ready and mission_ready (section 4):

- Profile loader emit site: there is no single "profile loaded" event in the existing code; `LogisticsData::instance->init()` is called in several places (logistics.cpp:864, logistics.cpp:134, mech.cpp:1963). The cleanest hook is at the top of `Logistics::start()` after `bMissionLoaded = 0` (code/logistics.cpp:108) — add `SmokeMode::emitTiming("profile_ready")` there as a one-shot. Alternatively, since the smoke path calls `setSingleMission` in `Logistics::update()`, `profile_ready` can be emitted immediately before `logistics_ready` in the same hook block (they collapse to the same instant in smoke mode; the distinction matters only for campaign profile load timing which smoke mode bypasses).

- mission_ready frame-loop hook: in `GameOS/gameos/gameosmain.cpp` after the per-frame rendering, OR in `code/mechcmd2.cpp` near line 2215 (after `mission->update()`), add:

```cpp
static bool s_missionReadyMarked = false;
if (SmokeMode::state().enabled && !s_missionReadyMarked && mission && mission->isActive()) {
    SmokeMode::markMissionReady();
    s_missionReadyMarked = true;
}
```

Placement in `mechcmd2.cpp` near line 2215 (after the `mission->update()` call at line 2217) is recommended because it is in the same translation unit that owns the mission update loop and has `mission` in scope. `gameosmain.cpp` would require an extern declaration.

## 6. Uncertainties / concerns

1. **`setSkipLogistics` setter does not exist.** `playLogistics` is private inside a private struct inside `LogisticsMissionInfo`. Task 6b cannot bypass the logistics screen without adding two new public methods (one on `LogisticsMissionInfo`, one passthrough on `LogisticsData`). This is a small mechanical addition but requires touching two header/source pairs.

2. **`setSingleMission` sets `bMultiplayer = true`.** This is confusing naming — it is used to mean "not using a campaign save file," not actual multiplayer. The `skipLogistics()` method returns `false` early if `MPlayer` is non-null (logisticsmissioninfo.cpp:1098), but `bMultiplayer` is a separate internal flag and does not shadow `MPlayer`. The smoke path does not touch `MPlayer` so this should not interfere. Confirmed: `bMultiplayer` is only checked inside `LogisticsMissionInfo::setNextMission()` to take the "single/multiplayer variant creation" path; it does not affect `skipLogistics()`.

3. **`beginMission` return value semantics.** The call at logistics.cpp:421 is `if (!beginMission(0, 0, 0))` — so return value `0` means success (no error). The signature returns `int`. If `rc` is non-zero that indicates failure. This convention should be verified at the top of the function body — it calls `mission->init(...)` which returns `void`; the actual success/failure is not explicitly returned in the visible code at lines 458–810. The function falls off the end returning `0` implicitly. Task 6b should treat any non-zero return as failure but also watch for assertions/early returns within `beginMission` that call `STOP()`/`Assert()` — these terminate the process before returning.

4. **`logisticsState` must be `log_SPLASH` for the update hook to reach the `missionBegin->readyToLoad()` branch.** Since smoke mode calls `beginMission` directly, it bypasses `missionBegin` entirely. That is fine — but the one-shot guard `s_smokeEntered` must fire before the normal state-machine body. Placing the block at the top of `Logistics::update()` before any `logisticsState` checks guarantees this.

5. **`resolveMissionPaths()` currently only stats the loose `.fit` file.** It does not confirm `.abl` availability. For smoke mode, if the `.fit` is found but the ABL brain is missing, `beginMission` will call `STOP()` / Assert internally. This is acceptable for a smoke harness — the runner buckets it as a mission-load failure.
