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
`MC2_HARNESS_ALLOW_FOREGROUND=1` re-enables foreground/cursor manipulation for
an interactive, user-present session. It is OFF by default and nothing in the
tree sets it. `show_window=True` asks for a visible (still non-activating)
window -- used by perf runs, because SW_SHOWMINNOACTIVE makes SDL throttle a
minimized window to ~100fps and Avg FPS then is not GPU throughput.

Enforced by `scripts/check-background-launch.py` (wired into check-contracts).
"""
from __future__ import annotations

import os
import subprocess
import sys
import time
from typing import List, Optional, Sequence

__all__ = [
    "NOFOCUS_ENV",
    "allow_foreground",
    "apply_env",
    "startupinfo",
    "popen_kwargs",
    "spawn",
    "park_window_offscreen",
    "run",
    "set_foreground_window",
    "set_cursor_pos",
]

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


def allow_foreground() -> bool:
    """True only when a user-present session explicitly opted in."""
    v = os.environ.get("MC2_HARNESS_ALLOW_FOREGROUND", "")
    return bool(v) and v != "0"


def apply_env(env: "Optional[dict]" = None, *, small_window: bool = True) -> dict:
    """Return `env` (or a copy of os.environ) with the no-focus vars applied.

    `setdefault` semantics: an explicit caller value always wins, so a test
    that specifically wants cursor grabbing can pass MC2_NO_CURSOR_GRAB=0, and
    a capture harness that grades at 1600x900 can pass MC2_WIDTH/MC2_HEIGHT.
    """
    out = dict(os.environ if env is None else env)
    if allow_foreground():
        return out
    for k, v in NOFOCUS_ENV.items():
        out.setdefault(k, v)
    if small_window:
        for k, v in SMALL_WINDOW_ENV.items():
            out.setdefault(k, v)
    return out


def startupinfo(show_window: bool = False):
    """STARTUPINFO that never activates. None on non-win32."""
    if sys.platform != "win32":
        return None
    if allow_foreground():
        return None
    si = subprocess.STARTUPINFO()
    si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    # SW_SHOWMINNOACTIVE(7): minimized, never activated -- the default, because
    # a merely-non-activating visible window (4) still paints over the game the
    # user is playing. show_window=True picks 4 for perf runs, which must not
    # be throttled by SDL's minimized-window sleep, but still never activates.
    si.wShowWindow = SW_SHOWNOACTIVATE if show_window else SW_SHOWMINNOACTIVE
    return si


def popen_kwargs(show_window: bool = False, **extra) -> dict:
    """Kwargs to splat into a subprocess call so it launches unactivated."""
    kw = dict(extra)
    si = startupinfo(show_window=show_window)
    if si is not None:
        kw["startupinfo"] = si
    return kw


def park_window_offscreen(pid: int, timeout_s: float = 3600.0,
                          proc: "object" = None) -> "object":
    """Move the child's top-level window off the visible desktop, unactivated.

    WHY: STARTUPINFO's SW_SHOWMINNOACTIVE governs only the first
    ShowWindow(SW_SHOWDEFAULT). SDL calls ShowWindow(hwnd, SW_SHOW) explicitly,
    so the window IS shown regardless. The engine's NO_ACTIVATION hint
    downgrades that to SW_SHOWNOACTIVATE -- never focused, but still VISIBLE,
    painting over whatever the user is looking at.

    SetWindowPos with SWP_NOACTIVATE moves it to negative coordinates well
    outside the virtual desktop. The GL drawable keeps its full size, so
    glReadPixels screenshots (the visual-verify path) are unaffected -- unlike
    minimizing, which makes SDL throttle and can zero the drawable.

    Runs on a daemon thread because the window does not exist at spawn time.

    OPT-IN (MC2_HARNESS_PARK=1), not default. The default already achieves the
    user-visible goal: windowed + small + never activated + no mode change
    (verified live). Parking additionally removes the window from the desktop
    composite, which BREAKS every harness that screenshots the composite via
    pyautogui/mss (quick_shot, quick_sweep, smoke_with_screenshots,
    water_visual_diff, camera_sweep_smoke, capture_baseline) rather than
    glReadPixels. Promote to default only after those are converted.
    """
    if sys.platform != "win32" or allow_foreground():
        return None
    if os.environ.get("MC2_HARNESS_PARK", "") in ("", "0"):
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
          env: "Optional[dict]" = None, **kw) -> "subprocess.Popen":
    """subprocess.Popen that cannot steal focus, the cursor, or the display."""
    proc = subprocess.Popen(list(argv), env=apply_env(env),
                            **popen_kwargs(show_window=show_window, **kw))
    if not show_window:
        park_window_offscreen(proc.pid, proc=proc)
    return proc


def run(argv: Sequence[str], *, show_window: bool = False,
        env: "Optional[dict]" = None, **kw):
    """subprocess.run that cannot steal focus or the cursor."""
    return subprocess.run(list(argv), env=apply_env(env),
                          **popen_kwargs(show_window=show_window, **kw))


# --------------------------------------------------------------------------
# Guarded shims for the two APIs that caused the user-visible damage.
# Callers keep their call sites; the calls simply do nothing by default.
# --------------------------------------------------------------------------

def set_foreground_window(hwnd) -> bool:
    """No-op unless MC2_HARNESS_ALLOW_FOREGROUND=1. Returns True if applied.

    Screen capture does NOT require foreground: mss/pyautogui grab the desktop
    composite, and a non-activated window still renders. Raising the window was
    always a superstition, and it is what yanked the user out of his game.
    """
    if not allow_foreground() or sys.platform != "win32" or not hwnd:
        return False
    import ctypes
    return bool(ctypes.windll.user32.SetForegroundWindow(hwnd))


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
