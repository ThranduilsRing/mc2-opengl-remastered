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

import atexit
import os
import re
import subprocess
import sys
import time
from typing import List, Optional, Sequence

try:    # imported as scripts.smoke_lib (package context)
    from ..console_child import has_console, _adopt_shared_console
except ImportError:    # imported as top-level smoke_lib with scripts/ on sys.path
    from console_child import has_console, _adopt_shared_console

__all__ = [
    "DesktopOccupancyRefused",
    "ExeRefused",
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
    #   * re-issue    -- gameos_graphics.cpp:9086 forces
    #                    GotoWindowMode on every later prefs.applyPrefs()
    #                    SetScreenMode; without this the window bounces
    #                    back to fullscreen mid-run.
    # It is a long-shipped gate, so this works on already-deployed exes with
    # no rebuild.
    "MC2_WINDOWED": "1",
    # No launcher window. gameosmain.cpp main() hands a directly-started mc2.exe
    # off to mc2-launcher.exe (a visible GUI) unless it sees one of a handful of
    # "automation" env vars -- and MC2_NO_CURSOR_GRAB / MC2_WINDOW_OFFSCREEN are
    # NOT among them. A harness that deliberately runs WITHOUT MC2_SMOKE_MODE (the
    # menu canary pops it) therefore spawned the launcher window. MC2_NO_LAUNCHER
    # is the long-shipped escape, so it works on already-deployed exes.
    "MC2_NO_LAUNCHER": "1",
    # MC2_NO_LAUNCHER makes the engine read launcher_env.json next to the exe and
    # apply its MC2_* keys (shadow size, FPS cap, PBR, cheats...), so a harness run
    # would stop measuring the default configuration. MC2_LAUNCHER_ENV_JSON=0 is the
    # engine's kill-switch for that read; normal front-end captures explicitly opt in.
    "MC2_LAUNCHER_ENV_JSON": "0",
}

#: Window SIZE. MC2_WINDOWED alone still opens a window the size of
#: options.cfg's ResolutionX/Y -- observed live at 1719x995 on a 1707x960
#: desktop, i.e. covering the whole screen. Technically windowed (WS_CAPTION,
#: no display-mode change) but indistinguishable from fullscreen to the user,
#: which is the actual complaint. So default small too.
#:
#: setdefault, so any harness that grades pixels at a specific resolution
#: (golden captures, scripts/check-capture-resolution.py, the resolution
#: matrix) keeps full control by setting MC2_WIDTH/MC2_HEIGHT explicitly.
SMALL_WINDOW_ENV = {
    "MC2_WIDTH": "854",
    "MC2_HEIGHT": "480",
}

#: The actual invariant: an agent-launched engine window
#: must never occupy visible desktop area at ANY point in its lifetime.
#:
#: Small + windowed + unactivated is NOT that. Two traps remain:
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
#: mattering and golden capture resolution is preserved. An exe without this
#: support is refused by exe_guard instead of being chased by a poller.
OFFSCREEN_ENV = {
    "MC2_WINDOW_OFFSCREEN": "1",
    # Agent launches are SILENT: SDL dummy audio driver (user is at the desk).
    "SDL_AUDIODRIVER": "dummy",
}


def allow_foreground() -> bool:
    """True only when a user-present session explicitly opted in."""
    v = os.environ.get("MC2_HARNESS_ALLOW_FOREGROUND", "")
    return bool(v) and v != "0"


def gpu_hint(out: dict) -> "Optional[str]":
    """The MC2_VK_DEVICE substring this run asks for (None = MC2_HARNESS_GPU=any: no routing, nothing asserted). See host_lease.device_hint."""
    try:
        from . import host_lease
    except ImportError:
        from smoke_lib import host_lease
    # runners scrub the child env; read the shell too
    return host_lease.pin_device(out.get("MC2_VK_DEVICE") or host_lease.device_hint(
        {k: os.environ.get(k, "") for k in ("MC2_HARNESS_GPU", "MC2_IN_GATE")} | out))


def route_gpu(out: dict, perf: bool = False) -> None:
    """On the dual-XTX PC defer to the lease, then pin its runtime name@uuid."""
    try:
        from . import host_lease
    except ImportError:
        from smoke_lib import host_lease
    if host_lease.runtime_lane_adapter():
        out.pop("MC2_VK_DEVICE", None)
    hint = gpu_hint(out)
    if hint:
        out["MC2_VK_DEVICE"] = hint


def gpu_mismatch(out: dict, perf: bool, devices: "Sequence[str]", log_text: str = "") -> "Optional[str]":
    """A message when the device the engine logged is not the one the run asked for, else None. `devices` = the logged [VK_PROFILE] device names.

    The implicit lane default is only asserted when the hint matched a device: a host with no such card
    (the farm laptop) makes the engine log "matches no accepted device", and that run is not red. An explicit
    MC2_HARNESS_GPU=2070|xtx, an explicit MC2_VK_DEVICE, or a gate run always asserts."""
    want = gpu_hint(out)
    if not want:
        return None
    want, _, want_uuid = want.partition("@")   # name@uuid: also assert the engine's [VK_DEVICE] uuid
    if want_uuid and "matches no accepted device" not in log_text:
        got = re.findall(r"\[VK_DEVICE\] index=\d+ of \d+ uuid=([0-9a-f-]+)", log_text)
        if not got or any(want_uuid.lower() not in g for g in got):
            return f"GPU routing: the run asked for device uuid {want_uuid!r} but the engine logged {got or 'no [VK_DEVICE] line'}"
    # Explicit = the user asked: MC2_HARNESS_GPU=2070|xtx, or MC2_VK_DEVICE in the shell. A harness-derived hint stays skippable.
    explicit = ((out.get("MC2_HARNESS_GPU") or os.environ.get("MC2_HARNESS_GPU") or "auto").lower() in ("2070", "xtx")
                or bool(os.environ.get("MC2_VK_DEVICE")) or os.environ.get("MC2_IN_GATE") == "1"
                or want != gpu_hint({k: v for k, v in out.items() if k != "MC2_VK_DEVICE"}))   # a hint other than the implicit one was typed by someone: it must match
    if not explicit and "matches no accepted device" in log_text:
        return None
    if not devices:
        return f"GPU routing: the run asked for a device matching {want!r} but its log has no [VK_PROFILE] device line"
    bad = [d for d in devices if want.lower() not in d.lower()]
    return f"GPU routing: the run asked for a device matching {want!r} but the engine used {bad[0]!r}" if bad else None


def apply_env(env: "Optional[dict]" = None, *, small_window: bool = True,
              offscreen: bool = True, perf: bool = False) -> dict:
    """Return `env` (or a copy of os.environ) with the no-focus vars applied.

    Safety settings are mandatory; an explicit caller value cannot enable
    cursor grabbing, fullscreen or on-screen agent windows. Capture dimensions
    still preserve an explicit MC2_WIDTH/MC2_HEIGHT.

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
    (MC2_VISUAL_CAPTURE / MC2_SCREENSHOT_PATH on the Vulkan backend) retires its entry.

    Declaring it is not the same as being granted it. In an agent session the
    request RAISES `DesktopOccupancyRefused`: a wallpaper golden and a stolen
    desktop are both unacceptable, so the run stops instead of picking one.
    `MC2_HARNESS_ALLOW_FOREGROUND=1` -- the existing, OFF-by-default,
    user-present opt-in that nothing in the tree sets -- is what grants it.
    """
    out = dict(os.environ if env is None else env)
    # Normal front-end captures may explicitly retain the install's saved content choices.
    defaults = dict(NOFOCUS_ENV)
    defaults["MC2_LAUNCHER_ENV_JSON"] = out.get("MC2_LAUNCHER_ENV_JSON", "0")
    route_gpu(out, perf)
    if allow_foreground():
        # User-present session: the human asked for the window to be VISIBLE
        # (so offscreen / small-window do not apply). They did NOT ask for the
        # game to take focus or the pointer -- OWNER RULE: an engine launch is
        # ALWAYS background. So the focus/cursor/fullscreen vars still apply;
        # they are what makes SDL create the window with
        # SDL_HINT_WINDOW_NO_ACTIVATION_WHEN_SHOWN and skip ClipCursor/warp.
        out.update(defaults)
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
            "MC2_SCREENSHOT_PATH, see scripts/run_visual_capture.py "
            "and scripts/mech_shot.py), which reads the Vulkan present image and is "
            "unaffected by where the window sits.")
    out.update(defaults)
    if small_window:
        for k, v in SMALL_WINDOW_ENV.items():
            out.setdefault(k, v)
    if offscreen:
        out.update(OFFSCREEN_ENV)
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

    CREATE_NO_WINDOW suppresses that window, but ONLY when this process has no
    console: a consoled parent's children share its console (no window can pop),
    whereas the flag would allocate a hidden console -- one conhost.exe -- per
    child, and hundreds of those crashed the owner's VPN split-tunnel monitor
    (2026-10-09). So the flag is added only when `has_console()` is false, the
    same rule as scripts/console_child.child_kwargs(). STARTF_USESHOWWINDOW/
    SW_HIDE covers children that create their own window anyway. Use ONLY for
    console tools, never for the game/editor (use spawn()/run(): they need the
    SDL window, just offscreen and non-activating).

    With no console the child's console is hidden, so inherited stdout/stderr go
    nowhere: redirect them (capture_output=True / stdout=PIPE / a log file) at
    the call site. Merged into a call as
    `subprocess.run(cmd, **nofocus.hidden_kwargs())`.
    """
    kw = dict(extra)
    if sys.platform != "win32":
        return kw
    if not has_console() and not _adopt_shared_console():
        kw["creationflags"] = kw.get("creationflags", 0) | CREATE_NO_WINDOW
    si = kw.get("startupinfo") or subprocess.STARTUPINFO()
    si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    si.wShowWindow = SW_HIDE
    kw["startupinfo"] = si
    return kw


def _desktop_blocked(what: str) -> bool:
    """True when MC2_CURSOR_GUARD_OFF=1 (tests: no window call at all); raises under pytest without it."""
    try:
        from . import win_input
    except ImportError:
        from smoke_lib import win_input
    if win_input.guard_off():
        return True
    win_input.pytest_tripwire(what)
    return False


def _record_child(proc, argv: Sequence[str]) -> None:
    """Note an engine child so a later run may end it if its runner died (see child_registry)."""
    try:
        from . import exe_guard, child_registry, host_lease
    except ImportError:
        from smoke_lib import exe_guard, child_registry, host_lease
    if argv and exe_guard.engine_kind(argv[0]) is not None:
        holder = next((r for _, r in host_lease._records() if host_lease._ours(r)), None)
        identity = {key: holder[key] for key in ("pid", "created")} if holder else None
        child_registry.record(proc.pid, argv[0], lease_holder=identity)


class ExeRefused(RuntimeError):
    """The engine exe cannot keep itself off the user's desktop (see smoke_lib/exe_guard.py)."""


def _hold_host_lease(argv: Sequence[str]) -> None:
    """Wait for the host-wide game lease before an engine (game or editor) launch; a launcher that starts the engine counts too.

    A --headless engine opens no window or GPU device and takes no slot (run_smoke skips it too); taking one here after
    the folder lease was the W114 lock-order inversion (host_lease.py LOCK ORDER)."""
    try:
        from . import exe_guard, host_lease
    except ImportError:
        from smoke_lib import exe_guard, host_lease
    if "--headless" in (argv or ()):
        return
    if any(exe_guard.engine_kind(a) is not None for a in argv or ()):
        host_lease.ensure_held("nofocus " + os.path.basename(sys.argv[0]))


def _vet_engine_exe(argv: Sequence[str]) -> None:
    """Refuse to launch a stale or fingerprint-less mc2.exe / editor exe.

    Every harness launcher goes through spawn()/run(), so this one check covers them all:
    an exe that predates the background-launch fix sizes its window to the monitor and
    warps the pointer, and no environment variable can stop that.
    """
    try:
        from . import exe_guard
    except ImportError:
        from smoke_lib import exe_guard
    # Every argument, not only argv[0]: a launcher such as `renderdoccmd capture -w mc2.exe` starts the
    # engine itself, and argv[0] there is the launcher.
    for arg in argv or ():
        if exe_guard.engine_kind(arg) is None:
            continue
        verdict = exe_guard.check_exe(arg, purpose="spawn")
        if not verdict.ok:
            raise ExeRefused(verdict.refusal())


_AUTO_SAVE_DIRS: list = []


def cleanup_auto_save_dirs() -> None:
    """Remove the auto-made save dirs that stayed empty (a dir holding saves is evidence and stays); runs at exit and after each pytest test."""
    while _AUTO_SAVE_DIRS:
        try:
            os.rmdir(_AUTO_SAVE_DIRS.pop())
        except OSError:
            pass


atexit.register(cleanup_auto_save_dirs)


def ensure_save_dir(child_env: dict) -> str:
    """Every engine launch gets a private MC2_SAVE_DIR: a caller's own value wins, else a fresh per-run dir under the lane scratch.
    An empty value, or one naming the player's own savegame folder, is replaced (with a warning) by the private dir.

    Without it the game writes the player's ~/.mechcommander2/savegame (after_mission_*.fit, testgame*.ims) and the merge gate's
    player-path row reports "savegame folder changed during the run" (Fable 1118h P1-2)."""
    def _canon(p):
        return os.path.normcase(os.path.realpath(p))
    home_mc2 = _canon(os.path.join(os.path.expanduser("~"), ".mechcommander2"))
    given = child_env.get("MC2_SAVE_DIR")
    # Refuse the player's folder, anything under it, and its parent (realpath: case, trailing slash, 8.3 aliases, symlinks).
    if given and (lambda g: g == home_mc2 or g.startswith(home_mc2 + os.sep) or home_mc2.startswith(g + os.sep))(_canon(given)):
        print(f"[save-dir] refused MC2_SAVE_DIR={given} (the player's savegame folder); using a private dir", file=sys.stderr, flush=True)
        given = None
    if not given:
        import tempfile
        base = child_env.get("MC2_LANE_SCRATCH") or os.environ.get("MC2_LANE_SCRATCH") or tempfile.gettempdir()
        os.makedirs(base, exist_ok=True)
        child_env["MC2_SAVE_DIR"] = tempfile.mkdtemp(prefix="mc2-save-", dir=base)
        _AUTO_SAVE_DIRS.append(child_env["MC2_SAVE_DIR"])
        print(f"[save-dir] private MC2_SAVE_DIR={child_env['MC2_SAVE_DIR']} (removed at exit when empty)", file=sys.stderr, flush=True)
    return child_env["MC2_SAVE_DIR"]


def spawn(argv: Sequence[str], *, show_window: bool = False,
          offscreen: "Optional[bool]" = None, small_window: bool = True,
          env: "Optional[dict]" = None, engine_default_workers: bool = False, **kw) -> "subprocess.Popen":
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
    window back on the user's screen (e.g.
    tools/menu_oracle.py:101). `offscreen` therefore defaults to True
    unconditionally; only a desktop-composite capture harness passes False,
    and only a user-present session is granted it (see apply_env).

    `small_window=False` skips the 854x480 MC2_WIDTH/MC2_HEIGHT default for
    harnesses whose output resolution is graded (golden captures at options.cfg
    size, perf runs); position is still offscreen, so nothing is on the desktop.
    """
    if offscreen is None:
        offscreen = True
    _vet_engine_exe(argv)
    child_env = apply_env(env, small_window=small_window, offscreen=offscreen, perf=show_window)
    try:
        from . import exe_guard, host_lease
    except ImportError:
        from smoke_lib import exe_guard, host_lease
    # The lease (slot, queue, core reservation) is keyed off the device the child actually gets, not this process's env.
    with host_lease.keyed_by(child_env.get("MC2_VK_DEVICE")):
        _hold_host_lease(argv)
        dev = host_lease.held_device()   # the slot this run won decides its GPU unless one was forced
        if dev and not child_env.get("MC2_VK_DEVICE"):
            child_env["MC2_VK_DEVICE"] = dev
        if child_env.get("MC2_VK_DEVICE"):   # two identical XTXs: preserve the acquired slot's UUID
            child_env["MC2_VK_DEVICE"] = host_lease.pin_device(child_env["MC2_VK_DEVICE"])
        engine = any(exe_guard.engine_kind(a) is not None for a in argv)
        if engine:
            ensure_save_dir(child_env)
        if engine and sys.platform == "win32":
            # Games must not inherit the lane harness's Idle priority. Replace
            # any caller priority bits while preserving window/console flags.
            kw["creationflags"] = (kw.get("creationflags", 0) & ~0xC1E0) | 0x4000
        if engine and not engine_default_workers and not child_env.get("MC2_FRAME_JOBS_WORKERS"):
            width = host_lease.frame_jobs_workers()   # the lane host's game cores, not the whole machine
            if width:
                child_env["MC2_FRAME_JOBS_WORKERS"] = str(width)
        proc = subprocess.Popen(list(argv), env=child_env, **popen_kwargs(show_window=show_window, **kw))
        _record_child(proc, argv)
        if engine:
            if sys.platform == "win32" and getattr(proc, "_handle", None) is not None:   # test doubles carry no handle
                import ctypes
                from ctypes import wintypes
                kernel = ctypes.WinDLL("kernel32", use_last_error=True)
                kernel.GetPriorityClass.argtypes = [wintypes.HANDLE]
                kernel.GetPriorityClass.restype = wintypes.DWORD
                actual = kernel.GetPriorityClass(int(proc._handle))
                print(f"[host-priority] pid={proc.pid} class={actual:#x} expected=0x4000 (BelowNormal)",
                      file=sys.stderr, flush=True)
                if actual != 0x4000:
                    proc.kill()
                    proc.wait()
                    raise RuntimeError(f"engine priority mismatch: expected BelowNormal, got {actual:#x}")
            host_lease.apply_affinity(proc)
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
    _vet_engine_exe(argv)
    _hold_host_lease(argv)
    try:
        from . import exe_guard
    except ImportError:
        from smoke_lib import exe_guard
    if any(exe_guard.engine_kind(a) is not None for a in argv):
        timeout = kw.pop("timeout", None)
        check = kw.pop("check", False)
        input_data = kw.pop("input", None)
        if input_data is not None:
            kw["stdin"] = subprocess.PIPE
        if kw.pop("capture_output", False):
            kw["stdout"] = kw["stderr"] = subprocess.PIPE
        with spawn(argv, show_window=show_window, offscreen=offscreen, small_window=small_window, env=env, **kw) as proc:
            try:
                stdout, stderr = proc.communicate(input_data, timeout=timeout)
            except BaseException:
                proc.kill()
                proc.communicate()
                raise
            result = subprocess.CompletedProcess(list(argv), proc.returncode, stdout, stderr)
            if check:
                result.check_returncode()
            return result
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
    if _desktop_blocked("raise_noactivate"):
        return False
    import ctypes
    import ctypes.wintypes as wt
    HWND_TOPMOST = wt.HWND(-1)
    SWP_NOSIZE, SWP_NOMOVE, SWP_NOACTIVATE = 0x0001, 0x0002, 0x0010
    u32 = ctypes.windll.user32
    # argtypes are mandatory: without them the HWND is marshalled as c_int and
    # truncated on 64-bit Python (HWND handles are 64-bit).
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

    `restoring=True` is reserved for smoke_lib.win_input, whose entire job
    is putting the pointer BACK where the user left it. Moving the pointer
    anywhere else during an agent run is forbidden.
    """
    if sys.platform != "win32":
        return False
    if not restoring and not allow_foreground():
        return False
    # The pointer write itself lives in win_input so that one function owns
    # (and idle-guards) every harness cursor move.
    try:
        from . import win_input
    except ImportError:
        from smoke_lib import win_input
    return win_input.set_cursor_pos((int(x), int(y)))
