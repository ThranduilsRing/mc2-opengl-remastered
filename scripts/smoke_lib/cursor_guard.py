# scripts/smoke_lib/cursor_guard.py
"""Harness-side guard against the game stealing the user's mouse cursor.

WHY THIS EXISTS (lane CURSOR-STEAL-HARNESS-GUARD-1)
---------------------------------------------------
The engine-side fix (`gos_CursorControlSuppressed()`, commit f0c07b87, env
`MC2_NO_CURSOR_GRAB`, auto-on under `MC2_SMOKE_MODE`) only helps processes
built AFTER that commit. Several concurrent agent lanes launch their own
older `mc2.exe` deploys that predate it, and those binaries still call
`ClipCursor()` / warp the pointer to the window centre every frame. An
engine-side fix cannot retroactively help a binary built before it.

This module makes cursor theft impossible from OUTSIDE the game process, so
ANY exe -- old or new -- is covered:

  * `ClipCursor(NULL)` is re-issued on a short timer, releasing any clamp the
    game installs (this is what pins the pointer inside the window for the
    whole 15s capture).
  * A watchdog samples `GetCursorPos()` and snaps the pointer back to the
    user's anchor whenever the game warps it, so the cursor is not merely
    restored at the end -- it is never parked away from the user mid-run.
  * The anchor is restored unconditionally on exit via `finally` + `atexit`,
    so a crashed or killed game still leaves the pointer where it started.

HUMAN-vs-MACHINE HEURISTIC
--------------------------
The guard must not fight the user if they move the mouse during a run. The
discriminator is motion continuity:

  * A machine warp parks the pointer at the SAME pixel on consecutive samples
    (a per-frame recenter re-warps to an identical coordinate) -- restore.
  * Human motion produces a DIFFERENT pixel on consecutive samples -- adopt
    it as the new anchor, so we never drag the user's pointer back.

Positions the game has been caught warping to are remembered in
`warp_targets` and snapped away from immediately thereafter; the set is
pre-seeded with the primary-display and virtual-desktop centres, which is
where MC2 recenters to.

SAFETY / PORTABILITY
--------------------
Pure `ctypes` against user32 -- no new dependencies. Every entry point is a
no-op (never raises) when:
  * not running on win32,
  * `user32` / `GetCursorPos` is unavailable or fails (headless, CI, session-0
    service context with no interactive desktop),
  * `MC2_NO_CURSOR_GUARD=1` is set (explicit opt-out).
"""
from __future__ import annotations

import atexit
import ctypes
import os
import sys
import threading
import time
from typing import Iterable, List, Optional, Set, Tuple

Point = Tuple[int, int]

# Poll interval. Short enough that a per-frame warp is corrected before the
# user can perceive the pointer as "stuck", long enough to be free.
_POLL_S = 0.02
# Pixel radius within which a sample counts as "at" a known warp target.
_NEAR_PX = 3
# The game must be quiet (no blocked warp) for this long before a moved cursor
# is believed to be the user's hand and adopted as the new anchor.
_ADOPT_QUIET_S = 0.5
# Consecutive restores whose IMMEDIATE read-back was not the anchor before we
# conclude another process is physically holding the pointer. The read-back test
# (see _restore_to_anchor) already excludes the per-frame-warper case, so this
# only needs to ride out a transient race -- 25 samples is 0.5s at the default
# 20ms poll.
_HELD_STREAK = 25


class _POINT(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]


class _RECT(ctypes.Structure):
    _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long),
                ("right", ctypes.c_long), ("bottom", ctypes.c_long)]


class _PROCESSENTRY32W(ctypes.Structure):
    """Subset of PROCESSENTRY32W sufficient to read pid + image name."""
    _fields_ = [("dwSize", ctypes.c_ulong),
                ("cntUsage", ctypes.c_ulong),
                ("th32ProcessID", ctypes.c_ulong),
                ("th32DefaultHeapID", ctypes.POINTER(ctypes.c_ulong)),
                ("th32ModuleID", ctypes.c_ulong),
                ("cntThreads", ctypes.c_ulong),
                ("th32ParentProcessID", ctypes.c_ulong),
                ("pcPriClassBase", ctypes.c_long),
                ("dwFlags", ctypes.c_ulong),
                ("szExeFile", ctypes.c_wchar * 260)]


def _user32():
    """Return the user32 handle, or None when there is no usable desktop."""
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


def set_cursor_pos(pos: Point) -> bool:
    u = _user32()
    if u is None:
        return False
    try:
        return bool(u.SetCursorPos(int(pos[0]), int(pos[1])))
    except Exception:
        return False


def window_rects_for_pids(pids: Iterable[int]) -> List[Tuple[int, int, int, int]]:
    """Screen rectangles (l,t,r,b) of the visible-or-parked top-level windows owned
    by `pids`. Empty when there is no desktop or no such window."""
    u = _user32()
    wanted = {int(p) for p in pids}
    if u is None or not wanted:
        return []
    rects: List[Tuple[int, int, int, int]] = []
    enum_proc_t = ctypes.WINFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p)

    def _each(hwnd, _lparam):
        pid = ctypes.c_ulong(0)
        u.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if pid.value in wanted:
            r = _RECT()
            if u.GetWindowRect(hwnd, ctypes.byref(r)):
                rects.append((int(r.left), int(r.top), int(r.right), int(r.bottom)))
        return 1

    try:
        u.EnumWindows(enum_proc_t(_each), 0)
    except Exception:
        return []
    return rects


def _rects_overlap(a: Tuple[int, int, int, int], b: Tuple[int, int, int, int]) -> bool:
    return a[0] < b[2] and b[0] < a[2] and a[1] < b[3] and b[1] < a[3]


def unclip_cursor(owner_pids: Optional[Iterable[int]] = None, *, force: bool = False) -> bool:
    """ClipCursor(NULL), but only for a clamp that lies on a window of `owner_pids`.

    A bare ClipCursor(NULL) also drops a clamp another application legitimately
    holds (a game the user is playing), so the default is to release nothing:
    the active clip must be smaller than the virtual desktop AND overlap a window
    owned by one of `owner_pids` (our game child). `force=True` is the old
    unconditional release, for a caller that owns the whole session.
    """
    u = _user32()
    if u is None:
        return False
    try:
        if not force:
            clip = detect_foreign_clip()
            if clip is None or not owner_pids:
                return False
            if not any(_rects_overlap(clip, w) for w in window_rects_for_pids(owner_pids)):
                return False
        return bool(u.ClipCursor(None))
    except Exception:
        return False


def _warn(msg: str) -> None:
    """Single-channel warning so a misled next agent has something to grep for."""
    try:
        print(f"[cursor-guard] {msg}", file=sys.stderr, flush=True)
    except Exception:
        pass


def get_clip_rect() -> Optional[Tuple[int, int, int, int]]:
    """Current ClipCursor rect as (l,t,r,b), or None if unavailable."""
    u = _user32()
    if u is None:
        return None
    r = _RECT()
    try:
        if not u.GetClipCursor(ctypes.byref(r)):
            return None
    except Exception:
        return None
    return (int(r.left), int(r.top), int(r.right), int(r.bottom))


def detect_foreign_clip() -> Optional[Tuple[int, int, int, int]]:
    """Return a pre-existing clamp rect if the cursor is confined, else None.

    A rect smaller than the virtual desktop means SOMEONE ELSE has already
    called ClipCursor -- typically a foreign mc2.exe from another agent lane.
    """
    u = _user32()
    if u is None:
        return None
    clip = get_clip_rect()
    if clip is None:
        return None
    try:
        SM_XVIRTUALSCREEN, SM_YVIRTUALSCREEN = 76, 77
        SM_CXVIRTUALSCREEN, SM_CYVIRTUALSCREEN = 78, 79
        vx, vy = u.GetSystemMetrics(SM_XVIRTUALSCREEN), u.GetSystemMetrics(SM_YVIRTUALSCREEN)
        vw, vh = u.GetSystemMetrics(SM_CXVIRTUALSCREEN), u.GetSystemMetrics(SM_CYVIRTUALSCREEN)
    except Exception:
        return None
    if not vw or not vh:
        return None
    full = (vx, vy, vx + vw, vy + vh)
    # Tolerate a 1px rounding difference on the full-desktop rect.
    if all(abs(a - b) <= 1 for a, b in zip(clip, full)):
        return None
    return clip


# Executables that grab the cursor and are launched by sibling agent lanes.
_GAME_IMAGES = ("mc2.exe", "editrel.exe")


def find_foreign_game_pids(exclude_pids: Optional[Set[int]] = None) -> List[Tuple[int, str]]:
    """List running game processes as (pid, image). NEVER kills anything.

    Uses CreateToolhelp32Snapshot directly -- no `tasklist` subprocess, so this
    is cheap enough to run on every guard start.

    Any game process alive when the guard starts is by definition NOT our child
    (runner.py starts the guard before Popen), i.e. it belongs to another lane.
    """
    if sys.platform != "win32":
        return []
    exclude = exclude_pids or set()
    out: List[Tuple[int, str]] = []
    try:
        k32 = ctypes.windll.kernel32  # type: ignore[attr-defined]
        TH32CS_SNAPPROCESS = 0x00000002
        INVALID = ctypes.c_void_p(-1).value
        snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
        if snap == INVALID or not snap:
            return []
        try:
            entry = _PROCESSENTRY32W()
            entry.dwSize = ctypes.sizeof(_PROCESSENTRY32W)
            ok = k32.Process32FirstW(snap, ctypes.byref(entry))
            while ok:
                name = entry.szExeFile
                if name.lower() in _GAME_IMAGES and entry.th32ProcessID not in exclude:
                    out.append((int(entry.th32ProcessID), name))
                ok = k32.Process32NextW(snap, ctypes.byref(entry))
        finally:
            k32.CloseHandle(snap)
    except Exception:
        return out
    return out


def _screen_centres() -> List[Point]:
    """Centres MC2 is known to recenter to: primary display and virtual desktop."""
    u = _user32()
    if u is None:
        return []
    out: List[Point] = []
    try:
        SM_CXSCREEN, SM_CYSCREEN = 0, 1
        SM_XVIRTUALSCREEN, SM_YVIRTUALSCREEN = 76, 77
        SM_CXVIRTUALSCREEN, SM_CYVIRTUALSCREEN = 78, 79
        cx, cy = u.GetSystemMetrics(SM_CXSCREEN), u.GetSystemMetrics(SM_CYSCREEN)
        if cx and cy:
            out.append((cx // 2, cy // 2))
        vx, vy = u.GetSystemMetrics(SM_XVIRTUALSCREEN), u.GetSystemMetrics(SM_YVIRTUALSCREEN)
        vw, vh = u.GetSystemMetrics(SM_CXVIRTUALSCREEN), u.GetSystemMetrics(SM_CYVIRTUALSCREEN)
        if vw and vh:
            out.append((vx + vw // 2, vy + vh // 2))
    except Exception:
        return out
    return out


def _near(a: Point, b: Point, tol: int = _NEAR_PX) -> bool:
    return abs(a[0] - b[0]) <= tol and abs(a[1] - b[1]) <= tol


class CursorGuard:
    """Context manager that prevents a child process from stealing the cursor.

    Usage::

        with CursorGuard():
            subprocess.Popen(...)  # game runs; cursor stays with the user

    Inert (and silent) when there is no interactive Windows desktop, or when
    ``MC2_NO_CURSOR_GUARD=1``. Never raises out of enter/exit -- a guard
    failure must never fail a smoke run.
    """

    def __init__(self, poll_s: float = _POLL_S, sample_log: Optional[list] = None):
        self.poll_s = poll_s
        self.active = False
        self.anchor: Optional[Point] = None
        self.origin: Optional[Point] = None
        self.warps_blocked = 0
        self.unclips = 0
        self.samples = 0
        # Optional caller-supplied list; receives (elapsed_s, pos, action) for
        # verification runs. Kept opt-in so normal smokes stay allocation-free.
        self.sample_log = sample_log
        self.warp_targets: Set[Point] = set()
        # Startup-defence findings (populated by start()).
        self.preexisting_clip: Optional[Tuple[int, int, int, int]] = None
        self.foreign_games: List[Tuple[int, str]] = []
        # Set when our restores are observed to have no effect -- i.e. some other
        # process is physically holding the pointer and we cannot win.
        self.held_by_other = False
        # Pids of the game child(ren) this guard defends; only a clamp on one of
        # their windows is released (see unclip_cursor).
        self.owner_pids: Set[int] = set()
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None
        self._atexit_registered = False
        self._t0 = 0.0

    def set_owner_pid(self, pid: int) -> None:
        """Register the game child's pid so its clamp can be released."""
        self.owner_pids.add(int(pid))

    # -- lifecycle ---------------------------------------------------------
    def start(self) -> "CursorGuard":
        if os.environ.get("MC2_NO_CURSOR_GUARD") == "1":
            return self
        # STARTUP DEFENCE, part 1: release any clamp BEFORE sampling the anchor.
        # If a foreign process has the cursor clipped, the position we are about
        # to adopt as "the user's" may be a position they cannot currently leave.
        self.preexisting_clip = detect_foreign_clip()

        pos = get_cursor_pos()
        if pos is None:
            # No desktop (headless/CI) -- stay inert.
            return self
        self.origin = pos
        self.anchor = pos
        self.active = True
        self._t0 = time.monotonic()
        self.warp_targets.update(_screen_centres())

        # STARTUP DEFENCE, part 2: name the situation instead of silently
        # anchoring to a stolen position.
        #
        # A per-run guard defends the pointer around ITS OWN child process. It
        # cannot defend against another lane's already-running game, and if that
        # game is holding the cursor when we start, `self.origin` is the STOLEN
        # position, not the user's. Verified during CURSOR-STEAL-HARNESS-GUARD-1:
        # a foreign mc2.exe pinned the cursor at (3,3) and the guard dutifully
        # anchored there. Warn loudly so the next agent is not misled into
        # trusting `origin`, and so a stuck cursor is attributed to the right
        # process.
        if self.preexisting_clip is not None:
            _warn(f"WARNING: cursor was ALREADY CLIPPED to {self.preexisting_clip} "
                  f"by another process at guard start; left in place (it is not "
                  f"this run's window).")
        # Anything running now cannot be our child -- runner.py starts the guard
        # before Popen. We only report; killing a foreign lane's game is never
        # this guard's call.
        self.foreign_games = find_foreign_game_pids()
        if self.foreign_games:
            listed = ", ".join(f"{n} pid={p}" for p, n in self.foreign_games)
            _warn(f"WARNING: {len(self.foreign_games)} foreign game process(es) "
                  f"already running ({listed}). These belong to OTHER agent lanes "
                  f"and are NOT touched. If the cursor still gets stolen during "
                  f"this run, they are the likely cause -- this guard can only "
                  f"protect against the child it launches.")
            _warn(f"NOTE: anchor={self.origin} was sampled while those processes "
                  f"were live, so it may already be a stolen position rather "
                  f"than where the user left the pointer.")

        self._stop.clear()
        self._thread = threading.Thread(target=self._watch, name="cursor-guard",
                                        daemon=True)
        self._thread.start()
        if not self._atexit_registered:
            atexit.register(self._restore)
            self._atexit_registered = True
        return self

    def stop(self) -> None:
        if not self.active:
            return
        self._stop.set()
        t = self._thread
        if t is not None:
            t.join(timeout=2)
        self._restore()
        self.active = False

    def _restore(self) -> None:
        """Release any clamp and put the pointer back on the anchor. Idempotent."""
        try:
            unclip_cursor(self.owner_pids)
            if self.anchor is not None:
                set_cursor_pos(self.anchor)
        except Exception:
            pass

    def _restore_to_anchor(self) -> Optional[bool]:
        """SetCursorPos(anchor) and report whether the write actually landed.

        Returns True if the immediate read-back is the anchor, False if some
        other process discarded our write, None if undeterminable.
        """
        if self.anchor is None:
            return None
        set_cursor_pos(self.anchor)
        back = get_cursor_pos()
        if back is None:
            return None
        return _near(back, self.anchor)

    # -- watchdog ----------------------------------------------------------
    def _watch(self) -> None:
        prev = self.anchor
        # Timestamp of the last blocked warp. While the game is ACTIVELY
        # fighting us, anchor adoption is suspended entirely -- see the
        # _ADOPT_QUIET_S rationale below.
        #
        # Seeded to "now" rather than 0.0: launch is exactly when the game
        # grabs, so the first samples must never be eligible for adoption. With
        # a 0.0 seed the very first off-anchor sample looked infinitely quiet
        # and got adopted, silently disarming the guard for the whole run
        # (observed once at a 50ms poll: warps_blocked=0).
        last_block = time.monotonic()
        # Consecutive samples where a restore was issued but the pointer did not
        # come back -- the signature of a foreign holder rather than a warper.
        failed_restores = 0
        while not self._stop.is_set():
            try:
                # Defeat the clamp every tick: the game may (re)install a
                # ClipCursor rect at any point during load or play.
                if unclip_cursor(self.owner_pids):
                    self.unclips += 1
                pos = get_cursor_pos()
                if pos is not None and self.anchor is not None:
                    self.samples += 1
                    action = "ok"
                    now = time.monotonic()
                    took = None
                    if pos == self.anchor:
                        pass
                    elif any(_near(pos, t) for t in self.warp_targets):
                        # Known machine warp target -- snap back.
                        took = self._restore_to_anchor()
                        self.warps_blocked += 1
                        last_block = now
                        action = "restored"
                    elif prev is not None and pos == prev:
                        # Parked on the same pixel across samples while off the
                        # anchor: machine, not a human hand. Learn it and undo.
                        self.warp_targets.add(pos)
                        took = self._restore_to_anchor()
                        self.warps_blocked += 1
                        last_block = now
                        action = "restored-learned"
                    elif now - last_block < _ADOPT_QUIET_S:
                        # A warp was blocked moments ago, so we are mid-fight
                        # with a per-frame warper. Samples then ALTERNATE
                        # anchor/warp-target and each one looks like "motion",
                        # which would let the anchor drift onto the very
                        # position we are trying to escape (observed: anchor
                        # walked to the warp target at a 10ms poll). Hold the
                        # anchor absolutely until the game goes quiet.
                        took = self._restore_to_anchor()
                        self.warps_blocked += 1
                        action = "held"
                    else:
                        # Quiet for a while and continuously moving -> the user.
                        # Follow them so the guard never drags a real hand back.
                        self.anchor = pos
                        action = "adopted"
                    # Did our restore actually take? Read back IMMEDIATELY after
                    # the write -- that is what separates the two failure modes:
                    #   * a per-frame WARPER lets our write land, then moves the
                    #     pointer again ~a frame later, so the read-back == anchor
                    #     even though later samples are off-anchor. We are merely
                    #     losing a ping-pong, which is expected and survivable.
                    #   * a HOLDER discards the write outright, so the read-back
                    #     is still the foreign position (observed with a foreign
                    #     mc2.exe: SetCursorPos returned TRUE with no effect).
                    # A consecutive-restore streak alone cannot tell these apart
                    # and false-positived on the warper, so key off `took`.
                    if took is False:
                        failed_restores += 1
                        if failed_restores >= _HELD_STREAK and not self.held_by_other:
                            self.held_by_other = True
                            _warn(f"WARNING: cursor appears to be HELD BY ANOTHER "
                                  f"PROCESS at {pos} -- {failed_restores} consecutive "
                                  f"restores to {self.anchor} had no effect. This "
                                  f"guard cannot win against a process it did not "
                                  f"launch; suspect another lane's game.")
                            if self.foreign_games:
                                _warn("  candidates: " + ", ".join(
                                    f"{n} pid={p}" for p, n in self.foreign_games))
                    else:
                        failed_restores = 0
                    if self.sample_log is not None:
                        self.sample_log.append(
                            (round(time.monotonic() - self._t0, 3), pos, action))
                    prev = pos
            except Exception:
                # A guard must never take down the run it is protecting.
                pass
            self._stop.wait(self.poll_s)

    # -- context manager ---------------------------------------------------
    def __enter__(self) -> "CursorGuard":
        try:
            return self.start()
        except Exception:
            return self

    def __exit__(self, exc_type, exc, tb) -> bool:
        try:
            self.stop()
        except Exception:
            pass
        return False  # never swallow the caller's exception

    # -- reporting ---------------------------------------------------------
    def report(self) -> dict:
        return {
            "active": self.active,
            "origin": self.origin,
            "final_anchor": self.anchor,
            "warps_blocked": self.warps_blocked,
            "samples": self.samples,
            "learned_targets": sorted(self.warp_targets),
            # Startup-defence findings. `origin` is UNTRUSTWORTHY when either of
            # the first two is non-empty -- it may be a stolen position.
            "preexisting_clip": self.preexisting_clip,
            "foreign_games": self.foreign_games,
            "held_by_other": self.held_by_other,
        }


# ---------------------------------------------------------------------------
# Conditional cursor parking (shared by run_visual_capture / camera_sweep_smoke)
# ---------------------------------------------------------------------------
#
# WHY CAPTURE SCRIPTS PARK AT ALL
# -------------------------------
# MC2's RTS camera edge-scrolls from the *desktop* cursor position regardless of
# window focus. The trigger is `code/missiongui.cpp:5206-5216`:
#
#     if (mouseX <= screenScrollLeft)                        scrollLeft();
#     if (mouseX >= Environment.screenWidth - screenScrollRight)  scrollRight();
#     if (mouseY <= screenScrollUp && mouseY >= -screenScrollUp)   scrollUp();
#     if (mouseY >= Environment.screenHeight - screenScrollDown)   scrollDown();
#
# so a cursor resting in a screen edge band adds a per-frame camera delta and
# makes captures nondeterministic. The historical fix was to slam the pointer to
# screen centre before launch and on every poll -- which is exactly the "the
# harness stole my cursor" symptom, because it fires unconditionally even when
# the pointer was never anywhere near an edge.
#
# The band values (`screenScrollLeft` etc.) are data-driven floats read from the
# mission-GUI .fit (`missiongui.cpp:4631-4640`) and are small in stock MC2. We
# therefore do not need their exact values -- only a safe UPPER BOUND. Any
# cursor further than `band` px from every screen edge provably cannot satisfy
# any of the four comparisons above, so parking it buys no determinism at all.
#
# Result: the pointer is left alone in the overwhelmingly common case (cursor
# resting mid-screen) and is only moved when it genuinely sits in a scroll band.
_DEFAULT_EDGE_BAND = 100
_PARK_ORIGIN: Optional[Point] = None
_PARK_ORIGIN_SAVED = False


def _edge_band() -> int:
    try:
        return max(0, int(os.environ.get("MC2_CURSOR_EDGE_BAND", _DEFAULT_EDGE_BAND)))
    except Exception:
        return _DEFAULT_EDGE_BAND


def cursor_is_edge_safe(pos: Optional[Point] = None) -> Optional[bool]:
    """True if `pos` cannot trigger MC2 edge-scroll. None if undeterminable.

    Safe == strictly inside the primary screen rect, inset by the edge band.
    A position OUTSIDE the primary screen (second monitor) is reported unsafe:
    the engine clamps out-of-window coordinates toward the window border, so it
    can still read as an edge. That is the conservative direction -- correctness
    of the capture wins over leaving a second-monitor pointer untouched.
    """
    u = _user32()
    if u is None:
        return None
    if pos is None:
        pos = get_cursor_pos()
    if pos is None:
        return None
    try:
        w, h = u.GetSystemMetrics(0), u.GetSystemMetrics(1)  # SM_CXSCREEN/CYSCREEN
    except Exception:
        return None
    if not w or not h:
        return None
    b = _edge_band()
    x, y = pos
    return (b < x < w - b) and (b < y < h - b)


def park_cursor_if_edge(reason: str = "", engine_suppressed: bool = False) -> str:
    """Park the pointer at screen centre ONLY if it could drive edge-scroll.

    Returns one of: "inert" (no desktop / non-Windows), "disabled"
    (MC2_CURSOR_NO_PARK=1, or MC2_CURSOR_PARK not set to 1 -- the default), "not-needed" (already edge-safe, pointer untouched),
    "engine-suppressed" (MC2_VISUAL_DIFF_CAPTURE=1 -- the engine already skips
    edge-scroll at missiongui.cpp:5204, so parking is pure theft), or "parked".

    The pre-park position is snapshotted once so `restore_parked_cursor()` can
    put it back.
    """
    global _PARK_ORIGIN, _PARK_ORIGIN_SAVED
    if os.environ.get("MC2_CURSOR_NO_PARK") == "1":
        return "disabled"
    # OWNER RULE: an agent run never moves the user's pointer. Parking is a
    # SetCursorPos on the user's own cursor, so it is OFF unless a user-present
    # session explicitly asks (MC2_CURSOR_PARK=1). The only cost is that a
    # pointer resting in a screen-edge band can edge-scroll a composite capture;
    # that is reported (below) instead of "fixed" by warping the pointer.
    if os.environ.get("MC2_CURSOR_PARK") != "1":
        return "disabled"
    # Engine skips edge-scroll under MC2_SMOKE_MODE=1 too; callers that set it only in
    # the child env (run_visual_capture) pass engine_suppressed=True.
    if engine_suppressed or os.environ.get("MC2_SMOKE_MODE") == "1":
        return "engine-suppressed"
    u = _user32()
    if u is None:
        return "inert"
    pos = get_cursor_pos()
    if pos is None:
        return "inert"
    if not _PARK_ORIGIN_SAVED:
        _PARK_ORIGIN = pos
        _PARK_ORIGIN_SAVED = True
    # The engine already disables edge-scroll wholesale under the visual-diff
    # capture gate, so there is nothing left for a park to protect.
    if os.environ.get("MC2_VISUAL_DIFF_CAPTURE") == "1":
        return "engine-suppressed"
    safe = cursor_is_edge_safe(pos)
    if safe:
        return "not-needed"
    if safe is None:
        return "inert"
    try:
        w, h = u.GetSystemMetrics(0), u.GetSystemMetrics(1)
        set_cursor_pos((w // 2, h // 2))
    except Exception:
        return "inert"
    return "parked"


def restore_parked_cursor() -> bool:
    """Put the pointer back where it was before the first park. Idempotent."""
    global _PARK_ORIGIN_SAVED
    if not _PARK_ORIGIN_SAVED or _PARK_ORIGIN is None:
        return False
    return set_cursor_pos(_PARK_ORIGIN)
