# SMOKE-NO-CURSOR-STEAL-1 — status

**State: SHIPPED (agent-verified).** Branch `claude/nifty-mendeleev`, commit `f0c07b87`.
Worktree `A:/Games/mc2-main`. Deploy/verify lane: `mc2-win64-v0.4c`. Never pushed.

## Problem

Every automated smoke / visual_verify / capture launch yanked the user's mouse
pointer to the centre of the screen, interrupting work in other windows.

## Root cause — THREE independent contributors, not one

1. **Mouse grab + Win32 clip (one-shot at bring-up, re-asserted repeatedly).**
   `graphics::create_window()` (`GameOS/gameos/gos_render.cpp:385`) ends with
   `set_mouse_grab(true)` → `SDL_SetWindowMouseGrab` **plus** `ClipCursor(client rect)`.
   `gameosmain.cpp` re-asserts it on every `SDL_WINDOWEVENT_FOCUS_GAINED` (`:554`),
   `RESIZED` (`:540`) and `MOVED` (`:545`). ClipCursor is a *global* Win32 clamp:
   a pointer parked outside the game's client rect is snapped inside instantly.

2. **Outright pointer warp — including a PER-FRAME recenter.**
   `gos_SetMousePosition()` (`GameOS/gameos/gameos_input.cpp:151`) →
   `SDL_WarpMouseInWindow`. Reached via `UserInput::setMousePos`
   (`mclib/userinput.h:438`). `code/missiongui.cpp:1348` (the tutorial `guiFrozen`
   branch) calls it with `screenWidth*0.5, screenHeight*0.5` **every frame** —
   this is the literal "yanked to the centre of the screen" the user saw.
   Other warp sites: `missiongui.cpp:1248/1254/1309/1315`, `missionbegin.cpp:912/918/973/979`.

3. **Focus steal** — the window activates on show. (Already partly mitigated for
   `run_smoke.py`, which launches minimized `SW_SHOWMINNOACTIVE`; NOT mitigated
   for the non-minimized capture harnesses.) Suppressed under the same flag.

## Fix

New chokepoint `gos_CursorControlSuppressed()` — defined in `gameos_input.cpp`
(deliberately, so the MFC editor shell, which replaces `gos_render.cpp` with
`EditorGameOS.cpp`, still links), declared in `gos_render.h`. Evaluated once, cached.

| env | result |
|---|---|
| `MC2_NO_CURSOR_GRAB=1` | suppressed (works for an interactive launch too) |
| `MC2_NO_CURSOR_GRAB=0` | NOT suppressed — escape hatch, even under smoke |
| unset | suppressed **iff `MC2_SMOKE_MODE` is set** |

Consumed at three sites:
- `apply_mouse_grab()` — forces `grabbed=false`, so `ClipCursor(NULL)` releases and
  no SDL grab is applied (covers create_window + all three re-assert paths).
- `gos_SetMousePosition()` — early return; no warp ever reaches SDL.
- `create_window()` — sets `SDL_HINT_WINDOW_NO_ACTIVATION_WHEN_SHOWN=1` (focus steal).

**Interactive launches (neither var set) are byte-identical to before.**

## Harness wiring — ZERO harness edits needed

Every automation entry point already exports `MC2_SMOKE_MODE=1`, so they all
inherit the hands-off behavior for free:
`scripts/smoke_lib/runner.py:83` (the launcher behind `scripts/run_smoke.py`),
`run_visual_capture.py:212`, `capture_baseline.py:230`, `camera_sweep_smoke.py:88`,
`quick_shot.py`, `quick_sweep.py`, `mech_shot.py`, `smoke_with_screenshots.py:149`,
`renderdoc_capture.py:155`, `check-order-liveness.py:91`, `editor/EditorPlaytest.cpp:947`.
This also avoided a claimed-file collision — `scripts/run_smoke.py` and
`scripts/check-env-registry.sh` are held by lane LIGHT-REALISM-1.

Registry: `MC2_NO_CURSOR_GRAB` added to `kAuxEnvVars`
(`RenderCore/RendererFeatureRegistry.h`) — `sh scripts/check-env-registry.sh` PASSES
(970 vars accounted). Documented in `docs/tier1_env_vars.md` (new section).

## Verification — before/after cursor position

Probe: park the OS cursor at a deliberately off-centre `(137, 823)`, verify the
park sticks, launch `mc2.exe --profile stock --mission mc2_01 --duration 15` with
capture-harness env, sample `GetCursorPos` every 0.2–0.25 s across launch +
mission load + exit. Same exe, same lane, only the env differs.

| run | env | start | end | samples moved |
|---|---|---|---|---|
| A  | `MC2_SMOKE_MODE=1` (automation default) | (137,823) | (137,823) | **0 / 67** |
| A4 | `MC2_SMOKE_MODE=1` + fg-pid attribution | (137,823) | (137,823) | **0 — "NO CURSOR MOVEMENT AT ALL"** |
| B  | `MC2_SMOKE_MODE=1` `MC2_NO_CURSOR_GRAB=0` (forced legacy) | (137,823) | **(960,540)** | **67 / 68** — pinned to screen centre |

B reproduces the reported bug exactly (snap to screen centre, held there);
A/A4 under identical conditions never touch the pointer. Interactive behavior is
therefore demonstrably preserved by the `=0` path.

**Measurement caveat (recorded so a future run isn't confused):** this worktree
had 3–6 parallel lanes running their own *unfixed* `mc2.exe` builds. A control
run with **no launch of my own** still saw the cursor stolen to (960,540) — a
foreign lane's process. Runs are therefore reported against the *attributable*
signal (my child's own launch window / fg-pid), and the A-vs-B contrast is the
decisive evidence, not any single absolute reading.

**tier1: PASS 5/5** (`mc2_01/03/10/17/24`, 30 s, lane 0.4c, exe sha matches HEAD).
No mover-coherence violations, diag-state PASS.

## Notes / caveats

- The deployed exe is flagged BUILT-FROM-DIRTY-TREE: the shared worktree carries
  other live lanes' uncommitted edits (a mid-edit `mclib/cone_glow.cpp` broke the
  first build attempt; it was fixed by its owning lane and the rebuild is green).
  My own five files are committed at `f0c07b87`.
- Not changed: `MC2_OS_CURSOR` (visibility only), and the legacy warp *call sites*
  themselves — the suppression is deliberately at the single chokepoint so no
  future warp site can bypass it.
