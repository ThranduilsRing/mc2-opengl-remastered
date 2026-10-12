# scripts/smoke_lib/win_input.py
"""The few Windows desktop calls the smoke and capture harness still makes: where the pointer is, whether a person is using the machine, a
guarded pointer write, and a list of running game processes.

WHY THIS REPLACES cursor_guard.py: that module defended the user's pointer from OUTSIDE the game process (a ClipCursor(NULL) timer, a warp
watchdog, edge parking) because older exes grabbed the cursor. exe_guard.check_exe, called inside nofocus.spawn, refuses any exe built before the
fix (MIN_COMMIT), and the engine skips ClipCursor and warps under MC2_SMOKE_MODE, so the watchdog had nothing left to catch and was itself the
cause of the second cursor incident (it took the pointer from a person working at the machine). What stays here only READS, except
set_cursor_pos, which refuses while a person is active.

MC2_CURSOR_GUARD_OFF=1 makes every function inert (no user32 or kernel32 call); the conftest.py files and pytest.ini set it, and pytest_tripwire
makes a test run that reaches the real desktop without it fail loudly.
"""
from __future__ import annotations

import ctypes
import os
import sys
from typing import List, Optional, Set, Tuple

Point = Tuple[int, int]

class _POINT(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]




#: MC2_CURSOR_GUARD_OFF=1 makes every function here inert: no user32 or kernel32 call of any kind


GUARD_OFF_ENV = "MC2_CURSOR_GUARD_OFF"

_guard_off_logged = False


def guard_off() -> bool:
    return os.environ.get(GUARD_OFF_ENV) == "1"

def pytest_tripwire(what: str) -> None:
    """Raise when a test run reaches the real desktop without MC2_CURSOR_GUARD_OFF=1.

    A suite that drove the real guard took the owner's mouse in 5-10 s bursts (2026-10-10). The
    conftest.py files and the root pytest.ini set the switch; this makes a run from anywhere else
    fail loudly instead of moving the pointer.
    """
    if "pytest" in sys.modules and not guard_off():
        raise RuntimeError(
            f"{what} reached the real desktop under pytest without {GUARD_OFF_ENV}=1. "
            f"Set it (the conftest.py files do) or mock the call.")


def _user32():
    """Return the user32 handle, or None when there is no usable desktop."""
    if guard_off():
        return None
    pytest_tripwire("a user32 call")
    if sys.platform != "win32":
        return None
    try:
        return ctypes.windll.user32  # type: ignore[attr-defined]
    except Exception:
        return None


def get_cursor_pos() -> Optional[Point]:
    """Current cursor position, or None if unavailable (headless/CI/non-win32)."""
    u = _user32()
    if u is None:
        return None
    pt = _POINT()
    try:
        if not u.GetCursorPos(ctypes.byref(pt)):
            return None
    except Exception:
        return None
    return (int(pt.x), int(pt.y))


#: A pointer write is skipped when the user's last real input (keyboard or mouse,
#: as the OS counts it) is younger than this. SetCursorPos itself is not counted


USER_ACTIVE_WINDOW_S = 5.0



def user_idle_seconds() -> Optional[float]:
    """Seconds since the user's last keyboard/mouse input, or None if unknown."""
    u = _user32()
    if u is None:
        return None

    class _LASTINPUTINFO(ctypes.Structure):
        _fields_ = [("cbSize", ctypes.c_uint), ("dwTime", ctypes.c_uint)]

    try:
        info = _LASTINPUTINFO()
        info.cbSize = ctypes.sizeof(_LASTINPUTINFO)
        if not u.GetLastInputInfo(ctypes.byref(info)):
            return None
        now = int(ctypes.windll.kernel32.GetTickCount()) & 0xFFFFFFFF
        return ((now - int(info.dwTime)) & 0xFFFFFFFF) / 1000.0
    except Exception:
        return None


def user_is_active() -> bool:
    """True when the user touched the keyboard or mouse within USER_ACTIVE_WINDOW_S.

    Unknown idle time counts as inactive: a headless runner has no user to
    protect, and a failed query must not turn every restore into a no-op.
    """
    idle = user_idle_seconds()
    return idle is not None and idle < USER_ACTIVE_WINDOW_S


_GAME_IMAGES = ("mc2.exe", "editrel.exe")


def find_foreign_game_pids(exclude_pids: Optional[Set[int]] = None) -> List[Tuple[int, str]]:
    """List running game processes as (pid, image). NEVER kills anything.

    Uses CreateToolhelp32Snapshot directly -- no `tasklist` subprocess, so this
    is cheap enough to run on every guard start.

    Any game process alive when the guard starts is by definition NOT our child
    (runner.py starts the guard before Popen), i.e. it belongs to another lane.
    """
    if sys.platform != "win32" or guard_off():
        return []
    pytest_tripwire("find_foreign_game_pids")
    try:
        from . import child_registry
    except ImportError:
        import child_registry  # type: ignore
    return child_registry.engine_processes(_GAME_IMAGES, exclude_pids or set())



def set_cursor_pos(pos: Point) -> bool:
    """The only place in the tree that writes the pointer position.

    WHY: a pointer write would yank the cursor out from under a person using the machine (owner incident 2026-10-10), so it is skipped
    while one is present. -> True when the pointer was moved.
    """
    if user_is_active():
        return False
    u = _user32()
    if u is None:
        return False
    try:
        return bool(u.SetCursorPos(int(pos[0]), int(pos[1])))
    except Exception:
        return False
