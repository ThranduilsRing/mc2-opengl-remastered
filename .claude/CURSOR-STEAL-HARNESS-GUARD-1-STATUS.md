# CURSOR-STEAL-HARNESS-GUARD-1 — STATUS

**Lane:** CURSOR-STEAL-HARNESS-GUARD-1
**Worktree:** `A:/Games/mc2-main` — branch `claude/nifty-mendeleev`
**Date:** 2026-07-25
**Status:** SHIPPED (pure Python; no build, no deploy)

---

## TL;DR — the engine was not the thief

The user reported cursor theft *after* the engine-side fix `f0c07b87`
(`gos_CursorControlSuppressed()`), and the working hypothesis was that six
parallel lanes running older exes were the cause.

**That hypothesis is wrong.** A full re-audit (Part 2) found the engine side is
**completely guarded**, and a measured baseline proved an *old* exe launched
through the smoke harness with the guard disabled **never touches the cursor**
— because `runner.py` launches minimized, and the engine's grab is focus-gated
(`apply_mouse_grab` → `grabbed=false` with no focus).

The actual thief is **the Python harness itself**:
`run_visual_capture.py` and `camera_sweep_smoke.py` called
`park_cursor_center()`, which slammed the pointer to screen centre
**unconditionally** — before launch *and on every poll iteration* — and
`camera_sweep_smoke.py` never restored it at all.

---

## Part 2 — engine audit result: NO unguarded path

`gos_CursorControlSuppressed()` (`GameOS/gameos/gameos_input.cpp:30-42`):
`MC2_NO_CURSOR_GRAB=1` → suppress; `=0` → never; unset → suppress iff
`MC2_SMOKE_MODE` set. Cached in a function-local static.

| Site | Call | Guarded? |
|---|---|---|
| `GameOS/gameos/gos_render.cpp:68` | `SDL_SetWindowMouseGrab` | YES (`:52-54` forces `grabbed=false`) |
| `GameOS/gameos/gos_render.cpp:107` | `ClipCursor(&clip_rect)` | YES (`:82 if (!grabbed) return;`) |
| `GameOS/gameos/gameos_input.cpp:203/208/212` | `SDL_WarpMouseInWindow` | YES (`:181-182` early return) |
| **`code/missiongui.cpp:1348`** (the flagged per-frame recenter) | `userInput->setMousePos` | **YES, transitively** — `mclib/userinput.h:438-450` → `gos_SetMousePosition` → early-returns under the guard. No OS call happens. |
| `code/missiongui.cpp:1248/1254/1309/1315`, `missionbegin.cpp:912/918/973/979` | `setMousePos` | YES (same chokepoint) |

Both engine chokepoints are genuinely closed, including the `FOCUS_GAINED`
re-assert (`gameosmain.cpp:554`), resize/move refreshes, and `set_fullscreen`.
`gos_render.cpp:350` is the only `SDL_CreateWindow` in the game exe and the
window carries no `SDL_WINDOW_*_GRABBED` flag.

Residual unguarded, **not** cursor-position theft (left as-is, noted only):
- `gos_render.cpp:389` `SDL_ShowCursor(SDL_DISABLE)` at window creation — hides
  the pointer, never moves it. `:762` re-enables at shutdown.
- `3rdparty/imgui/backends/imgui_impl_sdl2.cpp:673` `SDL_CaptureMouse` — only
  while a mouse button is held; cannot fire before the user clicks.
  `:688/691` warps are dormant (`WantSetMousePos` never asserted; nav mouse
  positioning not enabled at `GuiRuntime.cpp:408-414`).

**No engine fix was required or made.**

---

## Part 1 — what the harness now does

### New: `scripts/smoke_lib/cursor_guard.py`

Single shared owner of cursor policy. Pure `ctypes`/user32, no new deps.
Every entry point is a silent no-op when not on win32, when there is no
interactive desktop (headless/CI/session-0), or on explicit opt-out.

**`CursorGuard`** — context manager / `start()`+`stop()`, wired into
`smoke_lib/runner.py` around the game spawn (so it covers `run_smoke.py`,
`capture_baseline.py`, `quick_shot.py`, `mech_shot.py`,
`smoke_with_screenshots.py`, `renderdoc_capture.py`, …):
- re-issues `ClipCursor(NULL)` every tick, defeating the clamp for the whole run
  (not just at the end);
- samples `GetCursorPos()` and snaps the pointer back whenever the game warps it;
- restores the anchor in `finally` **and** via `atexit`, so a crashed or killed
  game still leaves the pointer where it started;
- opt-out `MC2_NO_CURSOR_GUARD=1`.

Human-vs-machine discriminator: a machine warp parks on the *same pixel* across
consecutive samples → restore; genuine hand motion yields a *different* pixel
each sample → adopt as the new anchor, so the guard never drags a real hand back.
Two hardening fixes came out of measurement (both documented in-file):
- **anchor drift** — at fast poll rates the anchor/warp-target ping-pong reads as
  "motion" and the anchor walked onto the warp target. Adoption is now suspended
  for `_ADOPT_QUIET_S` (0.5s) after any blocked warp.
- **startup race** — `last_block` seeded to `now`, not `0.0`; with a `0.0` seed
  the first off-anchor sample looked infinitely quiet and got adopted, silently
  disarming the guard for a whole run (observed once: `warps_blocked=0`).

**`park_cursor_if_edge()` / `restore_parked_cursor()`** — conditional parking,
replacing the unconditional slam in both capture scripts.

The determinism requirement is real: MC2's RTS camera edge-scrolls from the
*desktop* cursor regardless of focus, at `code/missiongui.cpp:5206-5216`:

```cpp
if (mouseX <= screenScrollLeft)                             scrollLeft();
if (mouseX >= Environment.screenWidth  - screenScrollRight) scrollRight();
if (mouseY <= screenScrollUp && mouseY >= -screenScrollUp)  scrollUp();
if (mouseY >= Environment.screenHeight - screenScrollDown)  scrollDown();
```

The band values are data-driven floats from the mission-GUI `.fit`
(`missiongui.cpp:4631-4640`) and are small in stock MC2 — so we need only a safe
**upper bound**, not their exact values. A cursor further than
`MC2_CURSOR_EDGE_BAND` (default **100px**) from every screen edge provably
cannot satisfy any of the four comparisons, so parking it buys **zero**
determinism. Outcomes: `not-needed` (pointer untouched — the common case),
`parked`, `engine-suppressed` (`MC2_VISUAL_DIFF_CAPTURE=1` already skips
edge-scroll wholesale at `missiongui.cpp:5204`, so a park is pure theft),
`disabled` (`MC2_CURSOR_NO_PARK=1`), `inert`.

Positions **outside** the primary screen (second monitor) are treated as unsafe
and parked — the engine clamps out-of-window coords toward the border, so they
can still read as an edge. Conservative direction: capture correctness wins.

### Changed files

| File | Change |
|---|---|
| `scripts/smoke_lib/cursor_guard.py` | NEW — guard + conditional park |
| `scripts/smoke_lib/runner.py` | `CursorGuard` around spawn; `stop()` in `finally`; sets `MC2_NO_CURSOR_GRAB=1` in child env |
| `scripts/run_visual_capture.py` | `park_cursor_center()` → `park_cursor_if_edge()`; logs outcome once per state change |
| `scripts/camera_sweep_smoke.py` | same, **plus** `atexit`-registered restore (previously it never restored at all) |

`scripts/run_smoke.py` was **not touched** — claimed by lane LIGHT-REALISM-1.
All behaviour lands in `runner.py`, which `run_smoke.py` calls.

---

## Verification — sampled cursor positions

Screen 1920x1080, centre `(960,540)`. Exes classified by probing the binary for
the `MC2_NO_CURSOR_GRAB` string: `releases/mc2-win64-v0.4d-rc1` = **NO_FIX**
(predates `f0c07b87`, 18:58), `releases/mc2-win64-issuefix-test` = **HAS_FIX**
(19:15). Both unheld by live lanes.

### Real exes through the harness (`runner.py`), mission mc2_01, 20s

Cursor parked at `(317,244)`; sampled every 250ms during the run and after exit.

| Trial | during-run samples | at-centre | after exit | ends where started |
|---|---|---|---|---|
| OLD exe, guard OFF (baseline) | n=113, all `(317,244)` | **0** | `(317,244)` | YES |
| OLD exe, guard ON | n=114, all `(317,244)` | **0** | `(317,244)` | YES |
| NEW exe, guard ON | n=118, all `(317,244)` | **0** | `(317,244)` | YES |

All three engine verdicts `passed=True`. **The OLD/guard-OFF baseline passing is
the key finding** — it proves `runner.py`-launched smokes were never the source
of the theft.

Instrumented re-run from `(410,330)`: `origin == final_anchor == (410,330)`,
647 samples, 0 warps blocked, 0 adoptions, `after == (410,330)`.

### Synthetic worst-case thief (harsher than any real exe)

A stand-in that both `ClipCursor`s a tight rect **and** `SetCursorPos`-recenters
every 16ms — i.e. a per-frame warper that never stops:

| Config | during run | after exit | anchor drift |
|---|---|---|---|
| No guard (baseline) | 13/13 samples pinned to `(960,540)` | `(960,540)` — **left stolen** | — |
| Guard, 50ms poll | 49 warps blocked | `(317,244)` | none |
| Guard, 20ms poll (default) | 26% of samples at anchor, 128 blocked | `(317,244)` | none |
| Guard, 10ms poll | 48% at anchor, 246 blocked | `(317,244)` | none |

Honest limitation: against a *per-frame* warper an outside-the-process poller
cannot win every frame — the pointer ping-pongs. What is guaranteed is (a) the
clamp is released continuously, (b) the anchor never drifts, (c) the pointer is
always restored on exit. Real exes never exhibited this (0/345 samples stolen).

### Conditional park

| Cursor at | outcome | pointer moved |
|---|---|---|
| `(317,244)` mid-screen | `not-needed` | **NO** (was: → `(960,540)`) |
| `(900,600)` mid-screen | `not-needed` | **NO** (was: → `(960,540)`) |
| `(3,3)` corner | `parked` | yes (decision correct) |
| `(50,300)` inside 100px band | `edge_safe=False` → `parked` | yes |

`restore_parked_cursor()` returned the pointer to `(317,244)`. Import health of
both rewired scripts confirmed; all four files `py_compile` clean.

**Gap:** I could not demonstrate a *physical* park move end-to-end — see below.

---

## Environmental finding: a foreign `mc2.exe` was stealing the cursor live

During verification the cursor became pinned at `(3,3)`/`(2,2)`: `SetCursorPos`
returned success but had no effect, with `GetClipCursor` showing a full-screen
rect. `tasklist` showed **three concurrent foreign `mc2.exe` instances** (PIDs
35592, 37348, 27856) from other lanes. They were left untouched per the
never-kill-foreign-instances rule.

This is almost certainly **what the user is actually experiencing right now**,
and it is worth stating plainly:

- A per-run guard protects the pointer around **its own child process**. It
  cannot defend against another lane's already-running game.
- If a lane's game is *already* holding the cursor when a guard starts, the
  guard anchors to the **stolen** position (verified: anchor became `(3,3)`).

So the remaining user-visible theft is other lanes' live game processes, not the
smoke/capture path.

---

## Follow-ups (not done here)

1. **Other lanes must rebuild** to pick up `f0c07b87`: `LIGHT-REALISM-1`,
   `VEHICLE-NIGHT-LIT-1`, `LETTERBOX-TOP-RESTORE-1`, `RUNWAY-MARKER-LIGHTS-1`,
   `MC2-03-CEMENT-PINK-1`, `PROP-SELF-SHADOW-1`.
2. `scripts/game_auto.py:187/195/203` (`do_click`/`do_rclick`/`do_move`) moves
   the real cursor by design — it is a desktop-automation driver simulating a
   user, so it was deliberately left alone. It has no save/restore around a
   session; adding one would be a small, separate courtesy fix.
3. Consider having long-running lanes launch minimized (as `runner.py` does),
   which is what makes the engine's focus-gated grab inert.
