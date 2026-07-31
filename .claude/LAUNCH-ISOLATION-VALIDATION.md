# LAUNCH-ISOLATION-VALIDATION

**Date:** 2026-07-31
**Question asked:** can a Windows process be run in a way that cannot take focus,
cannot capture the cursor, and cannot receive or steal keyboard input — with that
property *proven* rather than asserted?
**Mechanisms assessed:** private window station, job objects + creation flags,
offscreen/headless window, session boundary (and VM).
**The engine was never launched.** Not once, by anyone, in any lane. Every
measurement below came from throwaway probe processes plus the engine's own
`SDL2.dll` loaded as a library. The block held; nothing tried to route around it.

---

## 1. THE RECOMMENDATION

**None of the four mechanisms is proven safe enough to put the engine behind, so
keep the block exactly as it is.** The session-0 boundary is the only candidate
still standing and it is genuinely promising, but the specific escape that killed
the private-window-station mechanism — a confined process simply *asking* to
rejoin `WinSta0\Default`, which I reproduced myself today — has never been
measured against session 0, and until it is, "session 0 confines the engine" is
an assumption of exactly the kind that has already failed five times.

Second, unrelated to any of this and true in the tree today: there is a **live
armed cursor-capture defect** in the committed offscreen gate (§3.1). It does not
need anyone to adopt anything. It is one env var away from happening.

---

## 2. WHAT WAS DEMONSTRATED vs WHAT WAS REASONED

The split is drawn hard. "Demonstrated" means a real process was confined and the
Win32 return value and `GetLastError` were recorded. Everything else — however
good the argument, however consistent with documentation — is in the reasoned
column.

### 2.1 DEMONSTRATED (real confined process, measured return codes)

| # | Claim | How it was shown |
|---|---|---|
| D1 | A private window station **does** block input from inside | Child in `mc2probe_ws\mc2probe_dt`: `SETFOREGROUNDWINDOW_OK=0`, `SETCAPTURE_RET_NONNULL=0`, `CLIPCURSOR_OK=0 err=5`, `SETCURSORPOS_OK=0 err=1459`, `SENDINPUT_ACCEPTED=0 err=5`, `ISWINDOWVISIBLE=0` after an explicit `SW_SHOW` |
| **D2** | **…and it does not HOLD. The confined process can walk back out.** | **Reproduced by me this session — see the paste below** |
| D3 | Job objects cannot restrict focus/cursor/keyboard *at all* | `SetInformationJobObject(JobObjectBasicUIRestrictions)` accepts `0x01..0x80` and `0xFF`, rejects `0x100`/`0x1FF`/`0xFFFFFFFF` with `ERROR_INVALID_PARAMETER(87)` — the class is exactly 8 bits and none is foreground/capture/cursor/raw-input |
| D4 | Under a *maximally* restricted job (mask `0xFF`) a process still steals input | `ClipCursor` → TRUE (readback `[0,0,1707,960]`); `RegisterHotKey(NULL, …)` → TRUE; `SetWindowsHookExW(WH_KEYBOARD_LL, dwThreadId=0)` → **valid handle** (system-wide keyboard hook); `CreateWindowExW`+`ShowWindow` → `IsWindowVisible=TRUE`. Only three deltas vs mask `0x00`, all irrelevant: foreign-`HWND` `GetWindowRect`/`GetWindowTextLengthW` (err 1400) and `CreateDesktopW` (err 5) |
| D5 | Hiding/moving a window confines nothing — **desktop membership** is the gate | Same thread, three desktops, read-only `GetCursorPos`: Default → DENIED err 5; private desktop → DENIED err 5; **input desktop → PERMITTED (505,730)**. A process with *no window at all* still held `ClipCursor` and a global `GetAsyncKeyState` scan |
| D6 | Hardware GL survives isolation — the thing most likely to kill all of this | Private window station: `GL_RENDERER='AMD Radeon RX 7900 XTX'`, `GL_VERSION='4.3.0 Core Profile Context 26.6.4.260624'`, `MAX_SHADER_STORAGE_BLOCK_SIZE=2147483647`. Private desktop: same. Session 0 with `EnumDisplayDevices` returning **an empty list**: `4.6.0 Compatibility Profile Context`, same renderer |
| D7 | It rasterises, not merely initialises | `glClear`+`glReadPixels` returned the exact clear colour in every configuration (`64,128,191,255` on the private station; `[0,255,64,255]` in session 0 — see `scratchpad/sessvm/r_session0_child.json`); FBO render-to-texture `GL_FRAMEBUFFER_COMPLETE` with correct readback; `glGetError=0` throughout; 1920×1080 whole-frame readback of 6,220,800 bytes clean; 0.119 ms/swap isolated vs 0.092 ms on `WinSta0` — no throttling |
| D8 | Session 0 denies every input primitive tested | `GetCursorPos` err 1459, `SetCursorPos` err 1459, `ClipCursor` err 5, `SendInput` sent=0 err 5, `OpenInputDesktop` err 1, `OpenDesktop("Default")` err 5, `GetForegroundWindow` NULL, `display_devices: []` — verified in `scratchpad/sessvm/r_session0_user.json` |
| D9 | Session inheritance is free — an ordinary child lands inside too | Plain `subprocess.run` child from the session-0 probe: session 0, same `Service-0x0-bd61d8c2$` station, identical blocked table, rc 0 (`r_session0_child.json`). No `lpDesktop` plumbing, no PEB rewrite |
| D10 | Audio — the engine's other hard abort — clears session 0 | `SDL_InitSubSystem(SDL_INIT_AUDIO)` rc=0, 5 devices. The abort it would have hit is `GameOS/gameos/gameos_sound.cpp:118` `STOP(("Failed to initialize SoundEngine…"))` |
| D11 | `MC2_WINDOW_OFFSCREEN`'s cursor defect is real in the committed tree | Source, not inference — `gos_render.cpp:52`, `:107`, `:457`, `:485`; see §3.1 |
| D12 | `isolation.py` rests on a false premise about its own guard | `scripts/check-background-launch.py:54-58` says RULES 4 and 5 are RESERVED; `scripts/smoke_lib/isolation.py:121,124` and `nofocus.py:74` cite them as enforcing |

**D2 — the measurement that decides this report.** The private-window-station
lane asserted the boundary was "enforced by win32k, not by cooperation from the
process." Its skeptic disputed that; I re-ran the probe myself rather than pick a
side. Output of `scratchpad/verify_escape.py`, which spawns
`scratchpad/escape_probe.py` inside a freshly created private station (the child
creates **no** window and calls **no** cursor/focus/input API — it only opens
handles):

```
[verify] created winsta mc2verify_ws (handle ok)
[verify] created desktop mc2verify_dt inside it
[verify] lpDesktop=mc2verify_ws\mc2verify_dt
[verify] child rc=0
---- CHILD OUTPUT ----
START_WINSTA=mc2verify_ws
START_DESKTOP=mc2verify_dt
BEFORE_OPENINPUTDESKTOP_OK=0
BEFORE_OPENDESKTOP_DEFAULT_OK=0
BEFORE_OPENDESKTOP_DEFAULT_ERR=2
OPENWINSTA0_ALLACCESS_OK=1          <-- unprivileged, WINSTA_ALL_ACCESS granted
SETPROCESSWINDOWSTATION_WINSTA0_OK=1
NOW_WINSTA=WinSta0
AFTER_OPENDESKTOP_DEFAULT_GENERIC_ALL_OK=1
SETTHREADDESKTOP_DEFAULT_OK=1
NOW_DESKTOP=Default
AFTER_OPENINPUTDESKTOP_OK=1         <-- the lane's own discriminator, 0 -> 1
NOTE=no window created, no cursor/input API called
```

The process started isolated and ended on `WinSta0\Default`. Same user, same
token; `WinSta0`'s DACL grants the interactive logon session and nothing revokes
it. Four ordinary calls, no admin. The lane's cited proof of the opposite —
`OpenDesktopW("Default")` returning `ERROR_FILE_NOT_FOUND` from inside the
private station — is a tautology: desktop names resolve relative to the caller's
*current* window station, so that call was guaranteed to fail and tested nothing.
The call that decides it, `OpenWindowStationW("WinSta0")`, was never made until
now.

So: a private window station confines a process that does not ask to leave. That
is a cooperative guarantee. It is the same class of guarantee that has already
failed five times, and it is not what was asked for.

### 2.2 REASONED ONLY — argued, never demonstrated

Anything here could be true. None of it was measured with a confined process.

| # | Reasoned claim | Why it is only reasoned |
|---|---|---|
| **R1** | **Session 0 cannot be escaped the way a private window station can** | **Nobody made the measurement.** The argument is structural: `CreateProcess` has no parameter that crosses a session, and a process cannot move its own token's session without `SeTcbPrivilege`. That is a claim about *spawning*; the escape in D2 was a process *already inside* re-attaching itself. I wrote the probe (`scratchpad/s0_escape_probe.py`) and tried to run it in session 0 today via `Win32_Process.Create` — it landed in session **1** (`SESSION_ID=1`, `CONSOLE_SESSION_ID=1`), so that route does not reach session 0. Getting a session-0 process needs an S4U scheduled task, which is persistent machine configuration and therefore your call, not mine. **This is the single gap between "keep the block" and "here is a mechanism."** |
| R2 | Keyboard delivery is impossible under any of these | No lane ever pressed a key and showed the confined process could not see it. All four inferred it from `OpenInputDesktop` failing. Deliberate and correct: the honest test would have injected keystrokes into your live desktop |
| R3 | `SwitchDesktop` cannot be used to make an isolated desktop visible | Never called, in any lane, on purpose — a success would have blanked your screen. Substituted with read-only `EnumDesktopsW`: `WinSta0` holds `['Screen-saver','Default','Winlogon']`, the private station holds only its own desktop |
| R4 | `ClipCursor` from a non-interactive desktop cannot reach your pointer | It returns **TRUE** from a private desktop. Windows semantics say the clip rect is per-desktop, so it should be inert — unproven. `scratchpad/probe5_hostile_contained.py` is written to settle it and was deliberately not run unattended |
| R5 | The engine survives any of this | **The engine was never run.** Every GL/audio result is Python + the engine's `SDL2.dll` replaying `gos_render.cpp`'s exact call sequence. Untested: GLEW init, the ffmpeg/movie path, ImGui, asset load, the launcher, `mclib`/`code` paths that read `GetSystemMetrics` |
| R6 | The Vulkan backend works isolated | Never probed. `vulkan_present_object.cpp:220` creates an `SDL_WINDOW_VULKAN` window and `vulkan_swapchain_probe.cpp:95` a WSI surface; a WSI surface relates to the presentation engine differently from WGL, so the GL result does not transfer |
| R7 | The scheduled-task registration survives reboot | Reasoned from how Task Scheduler stores tasks. Nobody rebooted |
| R8 | Long runs behave | Longest confined process anywhere in this work lived ~6 seconds. Multi-minute GL load, GPU memory pressure, driver TDR on a display-less desktop, lock/unlock mid-run: all unmeasured |
| R9 | Capture resolution is unaffected | Inside a private station `GetSystemMetrics(SM_CXSCREEN)` reports 3840×2160 (raw) while `WinSta0` reports 1707×960 (DPI-virtualised 2.25×), yet SDL reported the same desktop mode in both. Anything deriving geometry from `GetSystemMetrics` could drift — that shows up as silently wrong goldens, not a crash |

### 2.3 A confound that touches nearly every measurement

**Every run in this entire body of work happened with the workstation locked.**
I confirmed it again today: `BEFORE_OPENINPUTDESKTOP_NAME=Screen-saver` and
`BEFORE_GETCURSORPOS_OK=0 err=5` *from an ordinary unconfined session-1 process*.

Two consequences, both bad for confidence:

- Every "the supervisor saw no change to foreground or cursor" result is a **null
  instrument**. The supervisor logged `fg_hwnd=0x0` and `cursor=None` at every
  sample, before, during and after. It was comparing `None` to `None`. That
  evidence carries no information and should not be read as reassurance.
- Some "blocked" rows are ambiguous: a locked session denies the cursor APIs on
  its own. The rows that survive the confound are the ones with lock-independent
  error codes (`1459 ERROR_REQUIRES_INTERACTIVE_WINDOWSTATION`, and the
  `OpenInputDesktop`/`OpenDesktop` 1→0 differential), plus the three-desktop
  comparison in D5, which is internally controlled.

Nothing here was ever validated against an unlocked session where a failure would
have actually been visible.

---

## 3. THE RESIDUAL RISK — how this still ends with your cursor taken

Every skeptic was asked this directly. Their answers, unsoftened, plus mine.

### 3.1 The one that needs nobody to adopt anything (highest probability)

`MC2_WINDOW_OFFSCREEN=1` **without** `MC2_NO_CURSOR_GRAB` or `MC2_SMOKE_MODE`
clips your cursor to a rectangle that is entirely off your screen. This is in the
tree right now. The chain, verified line by line:

- `GameOS/gameos/gos_render.cpp:414-417` — offscreen mode creates the window at
  `kOffscreenWindowX/Y = (-32000,-32000)` (`:155-156`) with `SDL_WINDOW_HIDDEN`.
- `gos_render.cpp:457` — `SDL_ShowWindow(window)` is called **unconditionally**.
  The window is not hidden at runtime; it is a normal shown top-level window
  parked where you cannot see it. Grepping all of `GameOS/` for
  `SDL_WINDOW_SKIP_TASKBAR` / `SDL_WINDOW_UTILITY` / `WS_EX_TOOLWINDOW` returns
  zero hits, so it carries a taskbar button and an Alt-Tab entry.
- `gos_render.cpp:485` — `set_mouse_grab(true)` is called **unconditionally** at
  the end of window creation.
- `gos_render.cpp:52` — `apply_mouse_grab` suppresses the grab **only** on
  `gos_CursorControlSuppressed()`. It does **not** consult
  `window_offscreen_requested()`, which is used freely elsewhere in the same file
  (`:404`, `:414`, `:459`, `:738`).
- `gos_render.cpp:86-108` — the grab path takes `GetClientRect` +
  `ClientToScreen` on that `HWND` and calls `ClipCursor(&clip_rect)`.

So the clip rect is the client rect of a window at (-32000,-32000). Your pointer
gets pinned somewhere it cannot be seen, by a window you cannot see, with no
obvious way to undo it.

Worse, it is not a one-shot at launch. `GameOS/gameos/gameosmain.cpp:548-555`
re-asserts `graphics::set_mouse_grab(true)` on every `SDL_WINDOWEVENT_FOCUS_GAINED`,
and `:541`/`:546` call `refresh_mouse_grab()` on resize and move. Alt-Tab to the
invisible window, or merely close whatever window currently has focus, and it
re-arms.

What stands between you and this is env-var discipline:

- `scripts/smoke_lib/nofocus.py` does couple them — `NOFOCUS_ENV` sets
  `MC2_NO_CURSOR_GRAB=1` (`:143-144`) and `check-background-launch.py:172`
  `FORCED_MINIMUM = ("MC2_WINDOWED","MC2_NO_CURSOR_GRAB","MC2_WINDOW_OFFSCREEN")`.
  Any launch through `nofocus` is fine.
- Nothing else is. `run-with-log.bat:6` sets `MC2_LOG=1` and **no windowing var at
  all**. `run-mc2.bat`, `run-editor.bat` and `tools/mc2_launcher` are the same. A
  hand launch, or an agent typing `$env:MC2_WINDOW_OFFSCREEN=1` in PowerShell
  because it read "offscreen is the safe mode", gets the full chain.
- The `FORCED_MINIMUM` guard is `names_in_code` — a syntactic AST presence check
  with no value check, applied to two policy files. It cannot see a `.bat`.
- `MC2_WINDOW_OFFSCREEN` is advertised as a user-facing gate in
  `RenderCore/RendererFeatureRegistry.h:947`.

**This is a bug worth fixing on its own merits, independent of every isolation
question in this document, and it can be fixed without running anything.** Two
lines: gate `apply_mouse_grab` on `window_offscreen_requested()` at
`gos_render.cpp:52`, and gate the `SDL_ShowWindow` at `:457`.

### 3.2 Session 0, if it gets adopted (the skeptic's answer, verbatim in substance)

> An agent re-registers the task per run — which the proposal explicitly endorses
> (`schtasks /Create /F` each run, to vary mission/env) — and one registration
> silently loses the isolating property: `/NP` dropped, `/IT` added, or the
> principal resolving to Interactive rather than S4U. Alternatively `schtasks`
> errors and the agent "helpfully" falls back to executing the wrapper `.cmd`
> directly. In every one of these cases the process lands in session 1 on
> `WinSta0\Default`, where `gos_render.cpp:485`'s unconditional
> `set_mouse_grab(true)` reaches `ClipCursor` at line 107 and **this time
> succeeds**.

And the part that matters most: **`schtasks` is invisible to the block.**
`.claude/hooks/block_game_launch.py` matches command *text* against 14 patterns
(`mc2.exe`, `run_smoke.py`, `run-mc2.bat`, …). `schtasks /Run /TN MC2_ISOLATED_RUN`
matches none of them. Adopting session 0 as-designed installs a permanent
hook-invisible door beside the locked one.

Second, unguarded by any of these mechanisms: **the session boundary is a
windowing boundary, not a device boundary.** A session-0 process shares the
physical 7900 XTX driving your desktop. An unverified GL/compute path — which is
precisely the backlog this work exists to unblock — that hangs the GPU triggers a
WDDM TDR, which blanks and reshuffles your displays and changes focus. Isolation
does nothing about that.

### 3.3 Window station, if anyone revives it

> A launch site that simply never gets wrapped. The mechanism is per-spawn opt-in
> and `lpDesktop` cannot be set through Python's `subprocess.STARTUPINFO`, so
> isolation only exists where someone remembered to route through the ~170-line
> ctypes wrapper. An agent writes the natural thing — `subprocess.Popen([mc2_exe,
> …])` — and gets a full-size window on `WinSta0\Default`. Nothing catches it:
> RULE 8 only enforces delivery of an env var, and RULES 4 and 5 are reserved
> stubs. The failure is silent, is the default, and is indistinguishable from
> success in the logs until the cursor is clipped.

Plus D2: even a *correctly* wrapped launch is escapable. Any in-process DLL that
attaches to games — an overlay, a driver shim — can call
`OpenWindowStationW("WinSta0", WINSTA_ALL_ACCESS)` on the engine's behalf and walk
out, while the isolation reports success the entire time.

### 3.4 Job objects

> Partial adoption creating false confidence. The one legitimate salvage —
> `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` for orphan-reaping — is ~60 lines of ctypes
> that *reads* exactly like isolation code: `CreateProcessW` suspended,
> `AssignProcessToJobObject`, a UI restriction mask, `ResumeThread`. The next agent
> opens a launch site, sees `JOB_OBJECT_UILIMIT_ALL` armed, reasonably concludes
> the launch is confined, and skips the real mechanism as redundant.

If that salvage is ever taken, set the UI mask to **zero** so the code cannot be
misread as an input boundary.

### 3.5 An escape route nobody's mechanism covers

The engine creates **other top-level windows** that no offscreen/window-station
plumbing touches. Unguarded modal `MessageBox(NULL, …)` calls, owner-NULL,
confirmed present:

```
mclib/terrtxm2.cpp:1229   MessageBox( NULL, msg, NULL, MBOK );
mclib/terrtxm2.cpp:1260   MessageBox( NULL, msg, NULL, MBOK );
mclib/ffile.cpp:178       MessageBox(NULL,data,MissingTitleString,MB_OKCANCEL|MB_ICONWARNING);
mclib/file.cpp:1233       MessageBox(NULL,data,MissingTitleString,MB_OKCANCEL|MB_ICONWARNING);
code/mechcmd2.cpp:951, :984, :1008, :1927, :2658
code/forcegroupbar.cpp:932
```

`terrtxm2.cpp`'s two fire on a terrain texture in an unaccepted TGA format —
literally the domain of the unverified default-ON terrain work this effort exists
to unblock. A modal dialog is a separate `HWND`, blocks the engine thread
indefinitely, and flashes or takes foreground by normal rules. (Under session 0 it
would instead hang forever with nobody able to click it — a different failure, not
a worse one.)

### 3.6 One genuinely reassuring finding, stated so it is not overweighted

The engine contains **no** `SetForegroundWindow`, `BringWindowToTop`,
`SetWindowsHookEx`, `RegisterRawInputDevices` or `SwitchDesktop` anywhere in
`GameOS/`, `code/` or `mclib/` — I grepped, zero hits. It is not an *active*
focus thief. What it does is show a window (`gos_render.cpp:457`) and clip the
cursor (`:107`). That narrows the problem considerably. It does not make any of
the mechanisms above proven.

---

## 4. WHAT YOU WOULD HAVE TO DO

### Option A — do nothing. Keep the block.
**Cost: the visual-verification backlog stays blocked.** Landed default-ON
lighting, terrain and gameplay-elevation changes remain unseen. That is the
status quo and it is a legitimate choice; the recommendation in §1 is that it is
the *correct* choice today.

### Option B — fix the offscreen cursor defect (§3.1). Independent of everything else.
Two small edits in `gos_render.cpp` (`:52`, `:457`), no run required to write
them, ordinary review to land them. **Cost: near zero. Recommended regardless of
what you decide about isolation.** It removes a live trap, and it does not lift
or weaken the block in any way.

### Option C — authorise the one missing measurement (§5, Stage 0). No engine.
Closes R1, the gap between "keep the block" and "here is a mechanism."
**Cost: about a minute, and your agreement to create one temporary Windows
scheduled task that is deleted seconds later.** I did not do this unilaterally —
a scheduled task is persistent machine configuration and that is your decision,
not an agent's. Nothing is launched but a Python probe that creates no window and
calls no input API.

### Option D — if C passes, build the session-0 launcher properly.
Not a small job, and only worth starting after C. It needs, minimally:

1. **Amend `.claude/hooks/block_game_launch.py` in the same change** to match
   `schtasks` / `Register-ScheduledTask` / `Start-ScheduledTask`, allowing exactly
   one audited wrapper path — so the isolated launcher is the only door, not a
   hole beside the locked one.
2. **A fail-closed preflight inside the wrapper**: read `ProcessIdToSessionId`,
   and if it equals `WTSGetActiveConsoleSessionId()`, abort *before* exec'ing the
   engine. ~10 lines. This is what turns an honour system into an interlock.
3. **One fixed task, never re-registered per run.** Args via a file. Every
   re-registration is a fresh chance to emit an interactive principal.
4. **A GPU-hang watchdog** with a hard timeout kill (§3.2, the TDR path).

**The real cost, and it is not small:** eight harnesses in
`check-background-launch.py:112-135` `COMPOSITE_CAPTURE_FILES` read the *desktop
composite* or drive the engine with synthetic input. Under any isolation they do
not degrade, they stop working:

```
scripts/capture_baseline.py    <- "V-BASELINE-0 golden capture via pyautogui.screenshot().
                                   The highest-risk entry: a wallpaper frame here
                                   becomes a checked-in visual BASELINE."
scripts/camera_sweep_smoke.py      scripts/game_auto.py
scripts/quick_shot.py              scripts/quick_sweep.py
scripts/smoke_with_screenshots.py  scripts/water_visual_diff.py
tools/menu_oracle.py
```

The migration target exists and is proven to work isolated (D7): the engine's own
in-process capture is `glReadPixels` off an FBO —
`GameOS/gameos/gos_visual_capture.cpp:221`, `gos_screenshot.cpp:21`. But
`capture_baseline.py` must be rewritten and **its goldens re-baselined against the
new capture route**, with R9 (the `GetSystemMetrics` 3840×2160 vs 1707×960
divergence) as a live risk of silently non-comparable captures. External synthetic
input has no equivalent at all — that is the point of the mechanism.

---

## 5. THE PROPOSED TRIAL

Two stages. **Stage 0 does not involve the engine and does not lift the block.**
Stage 1 is described only so the shape is on record; it is not proposed for now
and nothing below should be read as authorising it.

### Stage 0 — the missing measurement (recommended; no engine)

- **What runs:** `scratchpad/s0_escape_probe.py`, a Python probe I wrote and you
  can read. It creates no window; it calls no `SetCursorPos`, `ClipCursor`,
  `SendInput`, `SetForegroundWindow` or `SwitchDesktop`. It opens handles,
  rebinds its own station/desktop, and re-reads read-only discriminators.
- **Where:** session 0, obtained by a temporary S4U scheduled task
  (`schtasks /Create /TN MC2_ESCAPE_PROBE /RU <you> /NP /SC ONCE`, run, then
  `/Delete /F` with a `schtasks /Query` post-check).
- **Why the task is unavoidable:** measured today — `Win32_Process.Create` lands
  in session **1** on this machine (`SESSION_ID=1`), so there is no
  zero-configuration route to session 0.
- **What is watching:** nothing needs to. Even a successful escape produces no
  on-screen artifact, because the probe never creates a window or touches input.
- **How it aborts:** it exits on its own in under a second. If it hangs,
  `taskkill /F /PID` works across the session boundary — that was demonstrated
  (a session-0 sleeper was enumerated from session 1, killed, and confirmed gone).
- **What the result means:**
  - `AFTER_OPENINPUTDESKTOP_OK=0` and `AFTER_GETCURSORPOS_ERR=1459` still, after
    the escape attempt → the session boundary held where the window station did
    not. R1 becomes demonstrated, and Option D becomes worth the cost.
  - `AFTER_OPENINPUTDESKTOP_OK=1` → session 0 is escapable too, **no mechanism in
    this document is a real boundary**, and the block stays indefinitely.
  - Run it **unlocked**, at the keyboard. Everything in §2.3 was measured locked;
    this one datapoint should not inherit that confound.

### Stage 1 — the smallest conceivable first engine run (NOT proposed now)

On record only, so it is not designed under pressure later. It is contingent on
Stage 0 passing *and* on Option D items 1–4 being built *and* on your explicit
decision.

- **Not a mission.** The shortest existing boot-and-exit path, `MC2_SMOKE_MODE=1`,
  with `MC2_WINDOWED=1` / `MC2_NO_CURSOR_GRAB=1` / `MC2_WINDOW_OFFSCREEN=1` all
  set as belt-and-braces even though session 0 should make them moot.
- **You present and unlocked**, hand on the keyboard, not away from the machine.
- **Watching:** a supervisor in session 1 sampling `GetClipCursor`,
  `GetForegroundWindow` and cursor position every 250 ms to a log, which kills the
  job on *any* change rather than reporting it. That supervisor is the instrument
  that was dead (§2.3) in all previous work — it must be validated unlocked
  *before* the engine run, by confirming it reports real non-null values.
- **Aborts:** the wrapper's session preflight (fails closed before exec); a hard
  120-second timeout; `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` so nothing outlives the
  supervisor; and a single `taskkill /F /PID <n>` line printed on screen *before*
  the run starts so you can stop it without looking anything up.
- **Success criterion is negative:** no supervisor sample changed, and the run
  produced a `glReadPixels` capture. Anything else is a failure and the block
  stays.

---

## 6. REFUTED MECHANISMS

| Mechanism | Verdict | Why it fell |
|---|---|---|
| **Job objects + `CREATE_NO_WINDOW`/`DETACHED_PROCESS`** | **REFUTED — structurally, cannot be fixed** | The UI-restriction class is exactly 8 bits (`0x01..0x80`; `0x100` → `ERROR_INVALID_PARAMETER`) and not one is foreground, activation, capture, cursor-position or raw-input. Under mask `0xFF` a process still succeeded at `ClipCursor`, `RegisterHotKey`, a **system-wide `WH_KEYBOARD_LL` hook**, and showing a top-level window. A control run with mask `0x00` on a private station was byte-identical — every block came from the window station, none from the job. No tuning of the mask can ever buy the property. `CREATE_NO_WINDOW`/`DETACHED_PROCESS` govern console allocation only (`GetConsoleWindow()==0` in every run, while a GUI window appeared anyway). **Salvage:** `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` for orphan-reaping across the ~22 hand-rolled `Popen` sites — a reliability win with **zero** isolation value; if taken, set the UI mask to zero so it cannot be misread. |
| **Offscreen / hidden window (`MC2_WINDOW_OFFSCREEN`)** | **REFUTED — was never a boundary, and is currently armed** | Windows gates focus/cursor/input on *desktop membership*, never on window existence, visibility or position: same thread, three desktops, `GetCursorPos` DENIED / DENIED / **PERMITTED**; and a process with no window at all still held `ClipCursor` and a global key scan. So hiding a window is a courtesy the program can revoke at any instant. Worse, the shipped form is not hidden — `gos_render.cpp:457` shows it unconditionally, with a taskbar button and Alt-Tab entry, and `:485`→`:52`→`:107` clips the cursor to an off-screen rect (§3.1). **Salvage — and it is the valuable half:** *rendering* with no interactive window is genuinely proven (D6/D7). Offscreen belongs **inside** a real boundary as the pixel source, never instead of one. |
| **Private window station + desktop** | **REFUTED — cooperative, not enforced** | It really does block input from inside (D1), and that work was careful and well-evidenced. But the confined process can leave: `OpenWindowStationW("WinSta0", WINSTA_ALL_ACCESS)` → `SetProcessWindowStation` → `OpenDesktopW("Default")` → `SetThreadDesktop`, unprivileged, four calls, reproduced independently today (D2). Its cited counter-proof was a tautology. Secondary faults: per-spawn opt-in with a silent unsafe default across ~22 launch sites, guarded by RULES 4/5 which `check-background-launch.py:54-58` confirms do not exist; and the headline supervisor evidence was a null instrument (§2.3). |
| **VM / second session / RDP-to-self** | **REFUTED — measured absent or actively disruptive** | `Test-Path C:\Windows\System32\vmms.exe` → False (Hyper-V not installed); `WindowsSandbox.exe` → False. Both need admin + reboot; a single GPU means passthrough would blank your display; Sandbox opens a visible RDP-style window that *is* a focus stealer, is ephemeral (multi-GB deploy re-copied each launch), and targets D3D not GL 4.6 compat. WSL2/Wine is far short of GL 4.6 + the SSBO/compute set for an MSVC/SDL2 Win32 binary. Fast user switching and RDP-to-localhost both disconnect your console session — maximum disruption, the opposite of the goal. |
| **Session 0 (S4U scheduled task)** | **NOT REFUTED — and NOT PROVEN** | The strongest candidate. Input primitives all denied with lock-independent codes, hardware GL 4.6 with zero display devices, pixel readback correct, audio init clean, and inheritance by an ordinary `subprocess.run` child *measured* rather than argued (D8–D10). What stops it being a recommendation: R1 — the D2 escape was never tested against it; it is invisible to the block hook; there is no interlock verifying a run actually landed in session 0; the engine has never run in it; and it is not a device boundary, so a GPU hang still reaches your displays. |

---

## 7. ONE HONESTY NOTE ON THE SOURCE MATERIAL

The session-0 report cites window station `Service-0x0-bd32ba4d$`; the on-disk
artifact says `Service-0x0-bd61d8c2$`, and a cited artifact `r_session0.json` is
not in the directory (its content survives only in `s0_stdout.txt`). Almost
certainly a re-run, and immaterial to the physics — but for a body of work whose
entire value is that it measured rather than asserted, the quoted evidence should
match the artifacts exactly. Treat quoted strings as approximate; treat the JSON
files in `scratchpad/sessvm/` as the record.

---

## APPENDIX — where the evidence lives

Scratchpad root:
`C:\Users\Joe\AppData\Local\Temp\claude\A--Games-mc2-opengl-src\f4bbf866-7ac2-41e3-b73b-ee6e568b2228\scratchpad\`

| File | What it is |
|---|---|
| `verify_escape.py`, `verify_escape_out.txt` | **D2** — my independent reproduction of the window-station escape |
| `escape_probe.py` | the escape probe itself (read it: no window, no input API) |
| `s0_escape_probe.py`, `s0esc_baseline.txt` | **Stage 0** — written and baselined in session 1; never run in session 0 |
| `sessvm/r_baseline.json`, `r_session0_user.json`, `r_session0_child.json`, `sessprobe.py` | session-0 evidence (D8–D10) |
| `focus_thief.py`, `winsta_lib.py`, `gl_probe.py`, `probe2..probe6*.py` | window-station lane (D1, D6, D7) |
| `jobprobe_parent.py`, `jobprobe_child.py`, `mask_surface.py` | job-object lane (D3, D4) |
| `probe1_desktop_caps.py`, `probe6_discriminator.py`, `probe7_child_on_private_desktop.py` | offscreen lane (D5) |
| `probe5_hostile_contained.py` | written, deliberately **not** run — would settle R4, needs you present |

In-repo, verified this session:
`GameOS/gameos/gos_render.cpp:52,107,155-156,404,414-417,457,485` ·
`GameOS/gameos/gameosmain.cpp:541,546,548-560` ·
`GameOS/gameos/gameos_input.cpp:30-40` ·
`GameOS/gameos/gameos_sound.cpp:118` ·
`GameOS/gameos/gos_visual_capture.cpp:221` · `gos_screenshot.cpp:21` ·
`scripts/check-background-launch.py:54-58,112-135,172` ·
`scripts/smoke_lib/nofocus.py:46-53,74,143-144,193` ·
`RenderCore/RendererFeatureRegistry.h:947` ·
`scripts/smoke_lib/isolation.py:121,124` (untracked) ·
`.claude/hooks/block_game_launch.py:20-34`
