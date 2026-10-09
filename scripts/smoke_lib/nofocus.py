# scripts/smoke_lib/nofocus.py
"""Single chokepoint for launching the game WITHOUT stealing focus or cursor.

WHY THIS EXISTS (lane BACKGROUND-SMOKE-1)
-----------------------------------------
The user plays other games while agent lanes run smokes. Every agent-initiated
run must be invisible: it may not take focus, raise a window over what the user
is looking at, or move/confine the mouse pointer.

The engine already does the right thing -- but ONLY when it is told to.
`gos_CursorControlSuppressed()` (GameOS/gameos/gameos_input.cpp) is true iff
`MC2_NO_CURSOR_GRAB=1`, or `MC2_SMOKE_MODE` is set. When true the engine:

  * skips `ClipCursor()` (gos_render.cpp apply_mouse_grab),
  * makes `gos_SetMousePosition()` a no-op (no SDL_WarpMouseInWindow),
  * sets `SDL_HINT_WINDOW_NO_ACTIVATION_WHEN_SHOWN=1` before SDL_CreateWindow,
    so the Windows backend uses SW_SHOWNOACTIVATE and the window never
    activates.

That last one matters more than it looks. A parent's `STARTUPINFO.wShowWindow`
only governs the FIRST `ShowWindow(SW_SHOWDEFAULT)`; SDL calls
`ShowWindow(hwnd, SW_SHOW)` explicitly, which IGNORES STARTUPINFO. So
STARTUPINFO alone does NOT prevent focus theft from an SDL app -- the env var
is what actually closes it. Harnesses that set STARTUPINFO but not the env var
were still stealing focus, which is exactly what the user kept reporting.

Hence the contract enforced here: BOTH the env var AND STARTUPINFO, always,
by default, on every path. STARTUPINFO covers the console/early-boot window;
the env var covers the SDL window and the pointer.

USAGE
-----
    from smoke_lib import nofocus

    proc = nofocus.spawn([exe, "--mission", stem], env=env)     # Popen
    cp   = nofocus.run([exe, "--foo"], capture_output=True)     # run

Or, if you must keep your own Popen call:

    subprocess.Popen(argv, env=nofocus.apply_env(env),
                     **nofocus.popen_kwargs())

ESCAPE HATCH
------------
`MC2_HARNESS_ALLOW_FOREGROUND=1` is the user-present opt-in. It permits ONE
thing: a window that is VISIBLE on the desktop (offscreen=False, for the
desktop-composite capture harnesses) and a non-activating topmost raise
(raise_noactivate). It does NOT permit focus, activation, or pointer capture --
OWNER RULE: an engine launch is ALWAYS background, so even then STARTUPINFO is
SW_SHOWNOACTIVATE and MC2_NO_CURSOR_GRAB / MC2_WINDOWED are still applied; the
mouse must stay untouched. It is OFF by default and nothing in the tree sets it.

`show_window=True` asks for a visible (still non-activating)
window -- used by perf runs, because SW_SHOWMINNOACTIVE makes SDL throttle a
minimized window to ~100fps and Avg FPS then is not GPU throughput. It does
NOT ask for the visible DESKTOP: `show_window` and `offscreen` are orthogonal
(see spawn()), and an offscreen window is neither MINIMIZED nor HIDDEN, so it
is not throttled. Only `offscreen=False` asks for desktop area, only a
desktop-composite capture harness has a reason to, and only a user-present
session is granted it -- otherwise `apply_env` raises DesktopOccupancyRefused.

Enforced by `scripts/check-background-launch.py` (wired into check-contracts).
"""
from __future__ import annotations

import os
import subprocess
import sys
import time
from typing import List, Optional, Sequence

__all__ = [
    "DesktopOccupancyRefused",
    "NOFOCUS_ENV",
    "OFFSCREEN_ENV",
    "SMALL_WINDOW_ENV",
    "allow_foreground",
    "apply_env",
    "startupinfo",
    "popen_kwargs",
    "hidden_kwargs",
    "CREATE_NO_WINDOW",
    "DETACHED_PROCESS",
    "spawn",
    "park_window_offscreen",
    "run",
    "set_foreground_window",
    "raise_noactivate",
    "set_cursor_pos",
]

class DesktopOccupancyRefused(RuntimeError):
    """A launch asked for a window ON the user's visible desktop. Refused.

    NOFOCUS-FINISH-1. `apply_env(offscreen=False)` is the one sanctioned way to
    ask for that, and it exists only for the DESKTOP-COMPOSITE capture
    harnesses (pyautogui / mss / PIL.ImageGrab), which read the input desktop's
    pixels and therefore get nothing but wallpaper from an offscreen window.

    The request is granted ONLY in a user-present session
    (`MC2_HARNESS_ALLOW_FOREGROUND=1`), because the invariant an agent lane
    must not be able to waive is exactly this one: an agent-launched engine
    window may not occupy visible desktop area at any point in its lifetime.
    So a composite harness in an agent run does not silently bless the user's
    wallpaper as a golden -- it aborts here, before the engine starts, with a
    message that names the two real options.
    """


# ShowWindow constants (winuser.h).
SW_HIDE = 0
SW_SHOWNORMAL = 1
SW_SHOWNOACTIVATE = 4
SW_SHOWMINNOACTIVE = 7

#: Engine-side suppression. `MC2_NO_CURSOR_GRAB` is used rather than
#: `MC2_SMOKE_MODE` because several harnesses (mechbay_preview_harness,
#: sweep_campaign, the menu canary) deliberately run WITHOUT smoke mode to
#: exercise real menu/interactive code paths -- they still must not steal
#: focus. `MC2_SMOKE_MODE` implies this var, so setting it here is additive
#: and never contradicts a smoke run.
NOFOCUS_ENV = {
    "MC2_NO_CURSOR_GRAB": "1",
    # NO-FULLSCREEN (the loudest symptom). An agent run must NEVER take the
    # display fullscreen: that is a mode change, it can alter desktop
    # resolution, and it hard-yanks the user out of whatever app is in front.
    #
    # MC2_WINDOWED=1 is a hard override at BOTH decision points, so a stale
    # user options.cfg with FullScreen=TRUE cannot beat it:
    #   * boot        -- gameosmain.cpp:1534 zeroes Environment.fullScreen
    #                    AFTER options.cfg has been parsed;
    #   * re-issue    -- gameos_graphics.cpp:9086 (WINDOWED-8006-1) forces
    #                    GotoWindowMode on every later prefs.applyPrefs()
    #                    SetScreenMode, which is what used to bounce the
    #                    window back to fullscreen mid-run.
    # It is a long-shipped gate, so this works on already-deployed exes with
    # no rebuild.
    "MC2_WINDOWED": "1",
    # NO-LAUNCHER-WINDOW. gameosmain.cpp main() hands a directly-started mc2.exe
    # off to mc2-launcher.exe (a visible GUI) unless it sees one of a handful of
    # "automation" env vars -- and MC2_NO_CURSOR_GRAB / MC2_WINDOW_OFFSCREEN are
    # NOT among them. A harness that deliberately runs WITHOUT MC2_SMOKE_MODE (the
    # menu canary pops it) therefore spawned the launcher window. MC2_NO_LAUNCHER
    # is the long-shipped escape, so it works on already-deployed exes.
    "MC2_NO_LAUNCHER": "1",
    "MC2_LAUNCHER_ENV_JSON": "0",
}

#: Window SIZE. MC2_WINDOWED alone still opens a window the size of
#: options.cfg's ResolutionX/Y -- observed live at 1719x995 on a 1707x960
#: desktop, i.e. covering the whole screen. Technically windowed (WS_CAPTION,
#: no display-mode change) but indistinguishable from fullscreen to the user,
#: which is the actual complaint. So default small too.
#:
#: setdefault, so any harness that grades pixels at a specific resolution
#: (golden captures, scripts/check-capture-resolution.py, the #49 resolution
#: matrix) keeps full control by setting MC2_WIDTH/MC2_HEIGHT explicitly.
SMALL_WINDOW_ENV = {
    "MC2_WIDTH": "640",
    "MC2_HEIGHT": "480",
}

#: NOFOCUS-HARDEN-1 -- the actual invariant: an agent-launched engine window
#: must never occupy visible desktop area at ANY point in its lifetime.
#:
#: Small + windowed + unactivated is NOT that. Two holes stayed open:
#:   (a) SMALL_WINDOW_ENV is setdefault, and the busiest launch paths
#:       deliberately do NOT set the size (run_visual_capture grades goldens at
#:       options.cfg resolution; smoke_lib.runner never set it at all), so those
#:       windows come up desktop-sized -- measured live at 1719x995 on a
#:       1707x960 desktop;
#:   (b) even a small window is still ON the user's screen.
#:
#: MC2_WINDOW_OFFSCREEN=1 (gos_render.cpp) creates the SDL window HIDDEN at
#: (-32000,-32000) and re-asserts that origin synchronously after every later
#: mode change. Nothing is ever mapped to a visible pixel, so the SIZE stops
#: mattering and golden capture resolution is preserved. This replaces
#: park_window_offscreen() as the mechanism -- the poller remains only as a
#: backstop for exes built before this change.
OFFSCREEN_ENV = {
    "MC2_WINDOW_OFFSCREEN": "1",
    # Agent launches are SILENT: SDL dummy audio driver (user is at the desk).
    "SDL_AUDIODRIVER": "dummy",
}


def allow_foreground() -> bool:
    """True only when a user-present session explicitly opted in."""
    v = os.environ.get("MC2_HARNESS_ALLOW_FOREGROUND", "")
    return bool(v) and v != "0"


def apply_env(env: "Optional[dict]" = None, *, small_window: bool = True,
              offscreen: bool = True) -> dict:
    """Return `env` (or a copy of os.environ) with the no-focus vars applied.

    `setdefault` semantics: an explicit caller value always wins, so a test
    that specifically wants cursor grabbing can pass MC2_NO_CURSOR_GRAB=0, and
    a capture harness that grades at 1600x900 can pass MC2_WIDTH/MC2_HEIGHT.

    `offscreen=False` DECLARES that this harness needs the window on the
    visible desktop. It exists for the harnesses that screenshot the DESKTOP
    COMPOSITE (pyautogui / mss / PIL.ImageGrab) instead of reading the engine's
    glReadPixels FBO -- an offscreen window contributes no pixels to the
    composite, so those tools would otherwise save the user's WALLPAPER as a
    golden and grade it. Every such call site is listed (with its reason) in
    `check-background-launch.COMPOSITE_CAPTURE_FILES`, and RULE 6/RULE 7 there
    keep that list honest in both directions: a composite harness that does not
    declare `offscreen=False` fails the build, and a non-composite file that
    does fails it too. Converting a harness to the engine capture path
    (MC2_VISUAL_CAPTURE / MC2_SCREENSHOT_SOURCE=backbuffer) retires its entry.

    Declaring it is not the same as being granted it. In an agent session the
    request RAISES `DesktopOccupancyRefused`: a wallpaper golden and a stolen
    desktop are both unacceptable, so the run stops instead of picking one.
    `MC2_HARNESS_ALLOW_FOREGROUND=1` -- the existing, OFF-by-default,
    user-present opt-in that nothing in the tree sets -- is what grants it.
    """
    out = dict(os.environ if env is None else env)
    if allow_foreground():
        # User-present session: the human asked for the window to be VISIBLE
        # (so offscreen / small-window do not apply). They did NOT ask for the
        # game to take focus or the pointer -- OWNER RULE: an engine launch is
        # ALWAYS background. So the focus/cursor/fullscreen vars still apply;
        # they are what makes SDL create the window with
        # SDL_HINT_WINDOW_NO_ACTIVATION_WHEN_SHOWN and skip ClipCursor/warp.
        for k, v in NOFOCUS_ENV.items():
            out.setdefault(k, v)
        return out
    if not offscreen:
        # Refuse BEFORE anything is spawned or established: there is no partial
        # state to unwind, and the message is the whole point.
        raise DesktopOccupancyRefused(
            "this launch asked for offscreen=False, i.e. an engine window ON "
            "the user's visible desktop. That is refused in an agent session. "
            "It is only reachable with MC2_HARNESS_ALLOW_FOREGROUND=1 (a "
            "user-present session). If you are here because a pyautogui/mss/"
            "ImageGrab screenshot came back as wallpaper: that is the same "
            "defect seen from the other side -- port the harness to the engine "
            "readback path (MC2_VISUAL_CAPTURE=1 or "
            "MC2_SCREENSHOT_SOURCE=backbuffer, see scripts/run_visual_capture.py "
            "and scripts/mech_shot.py), which reads the GL drawable and is "
            "unaffected by where the window sits.")
    for k, v in NOFOCUS_ENV.items():
        out.setdefault(k, v)
    if small_window:
        for k, v in SMALL_WINDOW_ENV.items():
            out.setdefault(k, v)
    if offscreen:
        for k, v in OFFSCREEN_ENV.items():
            out.setdefault(k, v)
    return out


def startupinfo(show_window: bool = False):
    """STARTUPINFO that never activates. None on non-win32."""
    if sys.platform != "win32":
        return None
    si = subprocess.STARTUPINFO()
    si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    # SW_SHOWMINNOACTIVE(7): minimized, never activated -- the default, because
    # a merely-non-activating visible window (4) still paints over the game the
    # user is playing. show_window=True picks 4 for perf runs, which must not
    # be throttled by SDL's minimized-window sleep, but still never activates.
    #
    # A user-present session (MC2_HARNESS_ALLOW_FOREGROUND=1) wants the window
    # VISIBLE (composite-capture harnesses read desktop pixels), so it gets 4
    # too -- but it NEVER gets the old `return None` (= SW_SHOWDEFAULT, which
    # activates). Visible is permitted there; activating is not.
    si.wShowWindow = (SW_SHOWNOACTIVATE if (show_window or allow_foreground())
                      else SW_SHOWMINNOACTIVE)
    return si


def popen_kwargs(show_window: bool = False, **extra) -> dict:
    """Kwargs to splat into a subprocess call so it launches unactivated."""
    kw = dict(extra)
    si = startupinfo(show_window=show_window)
    if si is not None:
        kw["startupinfo"] = si
    return kw


#: Win32 process-creation flags (winbase.h). CREATE_NO_WINDOW gives a console
#: child a console with no window; DETACHED_PROCESS gives it none at all.
CREATE_NO_WINDOW = 0x08000000
DETACHED_PROCESS = 0x00000008


def hidden_kwargs(**extra) -> dict:
    """Kwargs that keep a CONSOLE child (python, cmd, git, taskkill...) windowless.

    OWNER RULE covers EVERY window, including the console windows of helper
    processes spawned during a smoke/capture/editor run (a "map generator
    terminal" popped during an editor smoke). A console-subsystem child launched
    from a parent that has no console of its own -- an MCP server, a hook, a GUI
    app, anything started detached -- gets a NEW visible console window.

    CREATE_NO_WINDOW suppresses that window; STARTF_USESHOWWINDOW/SW_HIDE covers
    children that create their own window anyway. Use ONLY for console tools,
    never for the game/editor (use spawn()/run(): they need the SDL window, just
    offscreen and non-activating).

    The child's console is hidden, so inherited stdout/stderr go nowhere:
    redirect them (capture_output=True / stdout=PIPE / a log file) at the call
    site. Merged into a call as `subprocess.run(cmd, **nofocus.hidden_kwargs())`.
    """
    kw = dict(extra)
    if sys.platform != "win32":
        return kw
    kw["creationflags"] = kw.get("creationflags", 0) | CREATE_NO_WINDOW
    si = kw.get("startupinfo") or subprocess.STARTUPINFO()
    si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    si.wShowWindow = SW_HIDE
    kw["startupinfo"] = si
    return kw


def park_window_offscreen(pid: int, timeout_s: float = 3600.0,
                          proc: "object" = None, force: bool = False) -> "object":
    """BACKSTOP ONLY -- move a child's window offscreen if it got created there.

    NOFOCUS-HARDEN-1 DEMOTED THIS. It is no longer the mechanism, because
    polling cannot satisfy the invariant: this thread ticks every 250ms and the
    window does not even exist at spawn time, so a window created at
    options.cfg size in the middle of the desktop is VISIBLE for seconds before
    the first successful reposition. That multi-second desktop-covering flash on
    every launch is the user-visible complaint, and no tick rate fixes it --
    correct-at-creation geometry does.

    The real fix is `MC2_WINDOW_OFFSCREEN=1` (OFFSCREEN_ENV, applied by
    apply_env), which makes gos_render.cpp create the SDL window HIDDEN at
    (-32000,-32000) and re-assert that origin synchronously after every later
    mode change. This poller now exists solely to cover DEPLOYED EXES BUILT
    BEFORE that change, where the env var is inert. Delete it once no lane
    points at a pre-NOFOCUS-HARDEN-1 exe.

    Mechanics: SetWindowPos with SWP_NOACTIVATE, to coordinates outside the
    virtual desktop. The GL drawable keeps its full size, so glReadPixels
    captures are unaffected -- unlike minimizing, which makes SDL throttle and
    can zero the drawable.

    Runs whenever offscreen operation is wanted (force=True, or
    MC2_HARNESS_PARK=1). Harnesses that screenshot the DESKTOP COMPOSITE via
    pyautogui/mss must not park and must not set MC2_WINDOW_OFFSCREEN -- see
    apply_env(offscreen=False).
    """
    if sys.platform != "win32" or allow_foreground():
        return None
    # force=True: the caller KNOWS its capture path reads pixels via the
    # engine's glReadPixels FBO readback, not the desktop composite, so an
    # offscreen window is not merely safe but correct (run_visual_capture).
    if not force and os.environ.get("MC2_HARNESS_PARK", "") in ("", "0"):
        return None

    import ctypes
    import ctypes.wintypes as wt
    import threading

    u32 = ctypes.windll.user32
    SWP_NOSIZE, SWP_NOZORDER, SWP_NOACTIVATE = 0x0001, 0x0004, 0x0010

    # MUST declare argtypes: without them ctypes marshals the HWND as c_int and
    # TRUNCATES it on 64-bit Python, so SetWindowPos silently targets a bogus
    # handle and the park appears to do nothing (observed: window stayed put).
    u32.SetWindowPos.argtypes = [wt.HWND, wt.HWND, ctypes.c_int, ctypes.c_int,
                                 ctypes.c_int, ctypes.c_int, ctypes.c_uint]
    u32.SetWindowPos.restype = ctypes.c_bool
    u32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
    u32.GetWindowRect.restype = ctypes.c_bool
    u32.IsWindowVisible.argtypes = [wt.HWND]
    u32.IsWindowVisible.restype = ctypes.c_bool

    def _worker():
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            if proc is not None and proc.poll() is not None:
                return
            found: List[int] = []

            @ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)
            def _enum(hwnd, _lparam):
                owner = wt.DWORD()
                u32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
                if owner.value == pid and u32.IsWindowVisible(hwnd):
                    found.append(hwnd)
                return True

            try:
                u32.EnumWindows(_enum, 0)
            except Exception:
                return
            for hwnd in found:
                # RE-park every tick, not once: the engine re-issues
                # SetScreenMode via prefs.applyPrefs() during boot and on
                # options changes, and SDL re-centres the window each time --
                # a one-shot park was observed being undone, leaving a
                # desktop-sized window back on screen.
                # -32000,-32000 is off every physical monitor on a normal
                # multi-mon layout and is what Windows itself uses for
                # minimized-window placeholders.
                rect = wt.RECT()
                if u32.GetWindowRect(hwnd, ctypes.byref(rect)) and rect.left <= -30000:
                    continue  # already parked; don't churn SetWindowPos
                u32.SetWindowPos(hwnd, 0, -32000, -32000, 0, 0,
                                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE)
            time.sleep(0.25)

    t = threading.Thread(target=_worker, name="nofocus-park", daemon=True)
    t.start()
    return t


def spawn(argv: Sequence[str], *, show_window: bool = False,
          offscreen: "Optional[bool]" = None, small_window: bool = True,
          env: "Optional[dict]" = None, **kw) -> "subprocess.Popen":
    """subprocess.Popen whose window never occupies visible desktop area.

    `show_window` and `offscreen` are ORTHOGONAL, and conflating them is a
    defect this function used to have (`offscreen = not show_window`).

      * `show_window` picks the STARTUPINFO ShowWindow verb: SW_SHOWNOACTIVATE
        instead of SW_SHOWMINNOACTIVE. Its only job is to keep the window out
        of the MINIMIZED state, because gameosmain.cpp throttles the whole
        frame loop by 10ms when `SDL_GetWindowFlags & (MINIMIZED|HIDDEN)` --
        so a minimized run measures a sleep, not the renderer.
      * `offscreen` decides WHERE the window is: MC2_WINDOW_OFFSCREEN pins it
        at (-32000,-32000). That flag is neither MINIMIZED nor HIDDEN, so an
        offscreen window does NOT throttle.

    A perf run therefore wants BOTH -- visible-but-parked-offscreen is full
    throughput AND zero desktop pixels -- which the old coupling made
    unexpressible: `show_window=True` silently put a full options.cfg-sized
    window back on the user's screen (tools/perf_snapshot.py:106,
    tools/menu_oracle.py:101). `offscreen` therefore defaults to True
    unconditionally; only a desktop-composite capture harness passes False,
    and only a user-present session is granted it (see apply_env).

    `small_window=False` skips the 640x480 MC2_WIDTH/MC2_HEIGHT default for
    harnesses whose output resolution is graded (golden captures at options.cfg
    size, perf runs); position is still offscreen, so nothing is on the desktop.
    """
    if offscreen is None:
        offscreen = True
    proc = subprocess.Popen(
        list(argv),
        env=apply_env(env, small_window=small_window, offscreen=offscreen),
        **popen_kwargs(show_window=show_window, **kw))
    if offscreen:
        # Backstop for exes predating MC2_WINDOW_OFFSCREEN; a current exe is
        # already offscreen at creation and the poller finds nothing to move.
        park_window_offscreen(proc.pid, proc=proc, force=True)
    return proc


def run(argv: Sequence[str], *, show_window: bool = False,
        offscreen: "Optional[bool]" = None, small_window: bool = True,
        env: "Optional[dict]" = None, **kw):
    """subprocess.run that cannot steal focus, the cursor, or the desktop.

    Same orthogonality as spawn(): `offscreen` defaults True regardless of
    `show_window`.
    """
    if offscreen is None:
        offscreen = True
    return subprocess.run(
        list(argv),
        env=apply_env(env, small_window=small_window, offscreen=offscreen),
        **popen_kwargs(show_window=show_window, **kw))


# --------------------------------------------------------------------------
# Guarded shims for the two APIs that caused the user-visible damage.
# Callers keep their call sites; the calls simply do nothing by default.
# --------------------------------------------------------------------------

def raise_noactivate(hwnd) -> bool:
    """Put `hwnd` ABOVE other windows WITHOUT giving it focus. Returns True if applied.

    This is the only z-order operation a desktop-composite capture harness may
    use (pyautogui / mss / ImageGrab read the composite, so the engine window
    must be on top of whatever else is on screen). It is
    SetWindowPos(HWND_TOPMOST, SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE):

      * topmost  -> composited above other windows, so the grab sees the game;
      * NOACTIVATE -> the window never becomes the foreground window, never
        receives keyboard input, and never takes the thread's capture/focus.

    It does NOT touch the mouse. The pointer must stay wherever the user left
    it: composite capture needs pixels, not input, and the engine side runs with
    MC2_NO_CURSOR_GRAB=1 so nothing clips or warps it. A harness that needs to
    CLICK is an interactive driver (game_auto.py) and is gated separately.

    Granted only in a user-present session (MC2_HARNESS_ALLOW_FOREGROUND=1) --
    in an agent session the window is offscreen and there is nothing to raise.
    """
    if not allow_foreground() or sys.platform != "win32" or not hwnd:
        return False
    import ctypes
    import ctypes.wintypes as wt
    HWND_TOPMOST = wt.HWND(-1)
    SWP_NOSIZE, SWP_NOMOVE, SWP_NOACTIVATE = 0x0001, 0x0002, 0x0010
    u32 = ctypes.windll.user32
    # argtypes are mandatory: without them the HWND is marshalled as c_int and
    # truncated on 64-bit Python (same footgun as park_window_offscreen).
    u32.SetWindowPos.argtypes = [wt.HWND, wt.HWND, ctypes.c_int, ctypes.c_int,
                                 ctypes.c_int, ctypes.c_int, ctypes.c_uint]
    u32.SetWindowPos.restype = ctypes.c_bool
    return bool(u32.SetWindowPos(wt.HWND(int(hwnd)), HWND_TOPMOST, 0, 0, 0, 0,
                                 SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE))


def set_foreground_window(hwnd) -> bool:
    """Legacy name kept so composite harnesses need no edits. NEVER activates.

    OWNER RULE: no agent-launched window may take focus. This used to call
    SetForegroundWindow behind the user-present gate; it now delegates to
    raise_noactivate() (topmost, SWP_NOACTIVATE). Screen capture does not need
    foreground -- mss/pyautogui grab the composite, and a non-activated topmost
    window renders and is captured. Raising focus was always a superstition,
    and it is what yanked the user out of his game.
    """
    return raise_noactivate(hwnd)


def set_cursor_pos(x: int, y: int, *, restoring: bool = False) -> bool:
    """No-op unless allowed, or unless RESTORING the user's own anchor.

    `restoring=True` is reserved for smoke_lib.cursor_guard, whose entire job
    is putting the pointer BACK where the user left it. Moving the pointer
    anywhere else during an agent run is forbidden.
    """
    if sys.platform != "win32":
        return False
    if not restoring and not allow_foreground():
        return False
    import ctypes
    return bool(ctypes.windll.user32.SetCursorPos(int(x), int(y)))
