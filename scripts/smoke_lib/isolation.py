# scripts/smoke_lib/isolation.py
"""DESKTOP-ISOLATION-ENFORCE-1 -- move harness launches off the user's desktop.

    ####################################################################
    ##  STATUS: PARKED. NOT WIRED. DO NOT WIRE WITHOUT READING §STATUS ##
    ####################################################################

STATUS (META4-CLOSE-1, 2026-07-31)
----------------------------------
This module is TRACKED but NOT CONNECTED TO ANYTHING. `smoke_lib.nofocus` does
not import it and no launch path calls it. Importing it has no effect; only
`establish()` / `ensure_thread()` do anything, and nothing calls them.

It was briefly wired into `nofocus.apply_env()` / `popen_kwargs()` / `spawn()`
/ `run()` -- i.e. every launch path, fail-closed, with no fallback -- as an
UNCOMMITTED working-tree edit. That edit was reverted and preserved verbatim at
`.claude/DESKTOP-ISOLATION-WIRING-1.patch`. Three reasons, and all three must
be answered before it is re-applied:

  1. THE HEADLINE CLAIM WAS REFUTED, by this project, the same night. See
     "WHAT IS AND IS NOT GUARANTEED" below. The original text of this docstring
     called a private window station "a difference in KIND" and said
     "SwitchDesktop is impossible". Both are false as stated.
  2. IT HAS NEVER EXECUTED IN ANGER. Engine launches are mechanically blocked,
     so ~740 lines of Win32 (window-station creation, DACLs, a PEB write, a
     canary subprocess) have never run on a real harness launch. Wiring it
     fail-closed into every launch path means the first time it runs is the
     moment the block lifts, and any defect in it hard-fails every harness in
     the tree at once.
  3. IT CITED A RULE THAT DOES NOT EXIST (fixed below):
     `check-background-launch.py:54` says "RULE 4 and RULE 5 are RESERVED, not
     missing" -- reserved *because this file names them*. Neither is
     implemented, so neither enforces anything today.

WHY IT IS KEPT RATHER THAN DELETED: the measurements in it are real and were
expensive. In particular the DesktopInfo/PEB inheritance finding under
"INHERITANCE IS THE POINT" is non-obvious, was measured rather than assumed,
and contradicts the common folklore about SetThreadDesktop. Deleting the file
throws that away; leaving it untracked (its state until now) meant a `git
clean` would throw it away by accident, which is worse.

WHAT IS AND IS NOT GUARANTEED
-----------------------------
NOT a security boundary. `c09234f6f` (LAUNCH-ISOLATION-VALIDATION, 02:12 the
same night this module was written) MEASURED a process confined to a private
window station walking back to `WinSta0\\Default` in four unprivileged calls:

    OpenWindowStationW -> SetProcessWindowStation -> OpenDesktopW
                       -> SetThreadDesktop

Reproduced. Being moved off WinSta0 does not remove the ACCESS RIGHTS to
re-open it, so nothing stops code that wants to leave from leaving.

Note that this does NOT contradict the per-API error codes recorded below --
those were measured too and they stand. The two results are compatible because
they test different things: `OpenDesktop("Default")` resolves relative to the
CURRENT window station, so of course it fails from a private one. What was
never tested was re-opening WinSta0 by name first. The bad inference was
"one call fails, therefore the boundary holds", from a single call.

So what it IS worth: the engine is not an adversary. It never calls
OpenWindowStationW; it calls ShowWindow, ClipCursor and SetCursorPos, and
against exactly those a private window station returns kernel errors. That is
a real and useful property -- a strong mitigation against an engine that
ignores env vars -- but it is a mitigation, not a proof, and the launch block
was correctly kept on the strength of that distinction.

BEFORE RE-WIRING, prove: (a) `establish()` succeeds on this machine from a
real harness invocation; (b) the canary confirms a genuine child lands in the
private desktop; (c) a failure path degrades in a way that does not take every
harness down with it, or the fail-closed behaviour is explicitly accepted with
that consequence understood; (d) RULE 4/RULE 5 are actually implemented, or
the references to them are dropped.

WHY THIS EXISTS
---------------
The user has reported four times that agent-initiated engine runs steal his
mouse, keyboard and focus. It has been "fixed" twice with behavioural
mitigations -- STARTUPINFO.wShowWindow, then MC2_WINDOWED / MC2_WIDTH /
MC2_HEIGHT / MC2_NO_CURSOR_GRAB / SDL_HINT_WINDOW_NO_ACTIVATION_WHEN_SHOWN
plus an offscreen park -- and it came back BOTH times, because every one of
those mitigations is a request the engine may ignore or a variable a launch
path may drop:

  * SDL calls ShowWindow(hwnd, SW_SHOW) EXPLICITLY, which ignores
    STARTUPINFO.wShowWindow entirely;
  * the window is CREATED at desktop size and only afterwards moved, so an
    external parker polling at 250ms covers the screen for seconds;
  * scripts/mc2_env_pin.py is deny-by-default and scrubbed all four windowing
    vars; smoke_lib/runner.py's FORCED_ENV was missing MC2_WINDOWED outright.

The whole "create the window, then hide / move / shrink it" family is
structurally incapable of satisfying the requirement, because it is a race
that the engine can always win. So this module stops asking the engine to
behave and instead makes the calls it would use FAIL.

(This sentence used to read "changes the KIND of guarantee". It does not --
see §"WHAT IS AND IS NOT GUARANTEED" at the top. It is a strong mitigation
against an engine that ignores env vars, which is the actual adversary, and
not a boundary against code that wants out.)

MECHANISM
---------
Windows scopes USER-object visibility and input by two kernel objects:

    window station  (WinSta0)  -- owns the clipboard, atom table, the CURSOR
                                  CLIP RECT, and a set of desktops
      desktop       (Default)  -- owns windows, focus, the input queue

Only ONE desktop in the interactive window station is the "input desktop" at
a time; only that desktop is composited to the monitor and fed by the physical
keyboard and mouse. A desktop that is never passed to SwitchDesktop() cannot
be seen and cannot receive input, FOR AS LONG AS ITS PROCESSES DO NOT GO AND
GET WinSta0\\Default THEMSELVES -- which c09234f6f showed they may. The
original text here said such a station "can NEVER become the input desktop's
owner"; that overstated it. Read this section as a description of the default
state a well-behaved process stays in, not as an enforced invariant.

So this module puts the launching process -- and therefore, by inheritance,
the engine and everything it spawns -- onto:

    TIER "winsta"   a private window station AND a desktop inside it.
                    Kernel-enforced blocks, measured on this machine:
                      ClipCursor          -> ERROR_ACCESS_DENIED (5)
                      SetCursorPos        -> ERROR_REQUIRES_INTERACTIVE_
                                             WINDOWSTATION (1459)
                      SendInput           -> error 87
                      GetCursorPos        -> fails
                      OpenInputDesktop    -> fails
                      OpenDesktop("Default") -> ERROR_FILE_NOT_FOUND (2),
                                             i.e. not nameable RELATIVE TO THE
                                             PRIVATE WINDOW STATION. This line
                                             used to conclude "so SwitchDesktop
                                             is impossible". It does not
                                             follow, and c09234f6f refuted it:
                                             re-open WinSta0 BY NAME first and
                                             the walk-back succeeds. See
                                             §"WHAT IS AND IS NOT GUARANTEED".
                      ShowWindow(SW_SHOW) -> IsWindowVisible stays FALSE
                    Hardware OpenGL is unaffected: same session, same WDDM
                    adapter, same AMD ICD. Verified GL_RENDERER="AMD Radeon
                    RX 7900 XTX", GL 4.6 compat / 4.5 core, and SDL2 (the
                    engine's own windowing layer) initialises video there.

    TIER "desktop"  fallback: a private desktop inside WinSta0. Blocks focus,
                    window visibility, SetCursorPos, GetCursorPos and
                    synthetic input just as well -- but NOT ClipCursor, which
                    is a WINDOW-STATION resource and therefore shared. The
                    engine's single ClipCursor call site
                    (GameOS/gameos/gos_render.cpp:107) is gated on
                    gos_CursorControlSuppressed(), so this tier additionally
                    FORCES MC2_NO_CURSOR_GRAB=1 non-overridably, and creates
                    the desktop with a DACL that DENIES DESKTOP_SWITCHDESKTOP
                    so nothing can make it the visible desktop.

INHERITANCE IS THE POINT
------------------------
Python's subprocess.STARTUPINFO does not expose lpDesktop at all, so rather
than reimplementing Popen (and thereby covering only the call sites that
remember to use the wrapper) this module moves the HARNESS PROCESS itself and
lets every descendant inherit.

The inheritance source is NOT what folklore says. `SetThreadDesktop` does not
affect children: when CreateProcess is given lpDesktop == NULL it copies the
string from the CURRENT PROCESS's `RTL_USER_PROCESS_PARAMETERS.DesktopInfo`
(the value this process itself was started with, e.g. "Winsta0\\Default").
This was not assumed -- it was MEASURED here: with the process moved to a
private window station and the thread bound to a private desktop, a freshly
spawned child still reported `DESKTOP=Default`, and the canary below refused
the launch. So `establish()` also rewrites this process's DesktopInfo, and
the canary re-runs to prove a real child now lands where we think.

Consequences, all of them wanted:

  * every subprocess.Popen in the harness inherits isolation, including the
    ~22 engine launch sites, including the ones that roll their own Popen;
  * grandchildren inherit too, so a harness that spawns a harness is covered;
  * a launch path that FORGETS MC2_WINDOWED, or an env policy that SCRUBS it,
    is now harmless -- the guarantee does not travel in an env var;
  * stdio, pipes, .pid, .kill() and every other Popen semantic are untouched,
    because this is still plain subprocess.Popen.

FAIL CLOSED
-----------
If isolation cannot be established, `establish()` raises IsolationError and
the launch ABORTS. There is deliberately NO fallback to the user's desktop:
a silent fall-through to a visible launch is precisely how this bug survived
two previous fixes (cf. c5d934d5, which made a guard fatal instead of
warn-and-proceed for the same reason).

THREADS
-------
The window station is a per-PROCESS attribute but the desktop is per-THREAD,
and CreateProcess reads the CALLING thread's desktop. A harness that spawns
from a worker thread would otherwise leak back onto the user's desktop, so
every launch entry point calls `ensure_thread()` first, which re-asserts the
desktop on whatever thread is about to spawn and raises if it cannot.

ESCAPE HATCH
------------
`MC2_HARNESS_ALLOW_FOREGROUND=1` (already the project's user-present opt-in)
disables isolation for a session a human is watching. It is OFF by default and
nothing in the tree sets it.

META4-CLOSE-1 CORRECTION: this paragraph used to add "and
`scripts/check-background-launch.py` RULE 5 fails the build if anything ever
does", and the section below claimed enforcement by RULE 4. NEITHER RULE
EXISTS. `check-background-launch.py:54` states that RULE 4 and RULE 5 are
"RESERVED, not missing" -- reserved precisely because THIS file cited them by
number while uncommitted. Nothing enforces either property today. Implementing
them is item (d) of the re-wiring checklist at the top of this file.
"""
from __future__ import annotations

import os
import re
import subprocess
import sys
import threading
from typing import Optional

#: "<window station>\<desktop>" -- the shape RTL_USER_PROCESS_PARAMETERS
#: .DesktopInfo must already have before we are willing to overwrite it.
_re_desktop_info = re.compile(r"^[A-Za-z0-9 _.\-]+\\[A-Za-z0-9 _.\-]+$")

__all__ = [
    "IsolationError",
    "IsolationState",
    "TIER_WINSTA",
    "TIER_DESKTOP",
    "disabled",
    "establish",
    "ensure_thread",
    "state",
    "describe",
    "harden_env",
    "assert_composite_capture_supported",
]

TIER_WINSTA = "winsta"
TIER_DESKTOP = "desktop"

#: Names we will never create or attach to. Attaching to any of these would
#: put the engine right back on the user's input space, which is the bug.
_FORBIDDEN_NAMES = {
    "", "default", "winlogon", "disconnect", "screen-saver", "winsta0",
}

_DEFAULT_WINSTA = "mc2iso_ws"
_DEFAULT_DESKTOP = "mc2iso_dt"

# Win32 constants -----------------------------------------------------------
_UOI_NAME = 2
_WINSTA_ALL_ACCESS = 0x0000037F
#: Every DESKTOP_* right EXCEPT DESKTOP_SWITCHDESKTOP (0x0100). Asking for
#: less than "all" is deliberate: a handle without SWITCHDESKTOP cannot make
#: this desktop visible even by accident.
_DESKTOP_RIGHTS_NO_SWITCH = 0x00FF
_DESKTOP_SWITCHDESKTOP = 0x0100
#: Deny SWITCHDESKTOP to everyone, allow the rest. Applied to the TIER_DESKTOP
#: fallback only: a desktop on a private window station can never become the
#: input desktop anyway, so tier "winsta" keeps the default DACL and avoids
#: the risk of an over-tight ACL breaking child access.
_SDDL_NO_SWITCH = "D:(D;;0x00000100;;;WD)(A;;GA;;;WD)"

_ERROR_ALREADY_EXISTS = 183

#: PEB -> RTL_USER_PROCESS_PARAMETERS offset, and DesktopInfo's offset inside
#: it, for x64. Both are checked at runtime against the value they are
#: supposed to yield ("<winsta>\<desktop>"), so a layout change on some future
#: Windows produces a clean IsolationError instead of a wild pointer write.
_PEB_PROCESS_PARAMETERS_OFFSET = 0x20
_RUPP_DESKTOP_INFO_OFFSET = 0xC0
_PROCESS_BASIC_INFORMATION = 0

_lock = threading.Lock()
_state: "Optional[IsolationState]" = None
_failure: "Optional[str]" = None
#: Keeps the rewritten DesktopInfo buffer alive: the PEB now points AT it, so
#: letting Python free it would leave CreateProcess reading freed memory.
_desktop_info_buf = None


class IsolationError(RuntimeError):
    """Isolation could not be established. The caller MUST NOT launch."""


class IsolationState:
    """The established isolation. Handles are held for the process lifetime."""

    def __init__(self, tier: str, winsta: str, desktop: str,
                 hwinsta, hdesk, evidence: dict):
        self.tier = tier
        self.winsta = winsta
        self.desktop = desktop
        self._hwinsta = hwinsta      # kept alive: closing destroys the object
        self._hdesk = hdesk
        self.evidence = evidence

    def __repr__(self) -> str:  # pragma: no cover - debug aid
        return (f"IsolationState(tier={self.tier!r}, winsta={self.winsta!r}, "
                f"desktop={self.desktop!r})")


def disabled() -> bool:
    """True only when a user-present session explicitly opted out.

    Mirrors nofocus.allow_foreground(); kept separate so this module has no
    import dependency on nofocus (nofocus imports THIS).
    """
    v = os.environ.get("MC2_HARNESS_ALLOW_FOREGROUND", "")
    return bool(v) and v != "0"


def _min_tier() -> str:
    """Lowest acceptable tier. Default allows the desktop-only fallback."""
    v = (os.environ.get("MC2_ISOLATION_MIN", "") or TIER_DESKTOP).strip().lower()
    if v not in (TIER_WINSTA, TIER_DESKTOP):
        raise IsolationError(
            f"MC2_ISOLATION_MIN={v!r} is not one of "
            f"{TIER_WINSTA!r}/{TIER_DESKTOP!r}")
    return v


def _object_name(candidate: str, fallback: str, what: str) -> str:
    name = (candidate or fallback).strip()
    if name.lower() in _FORBIDDEN_NAMES or "\\" in name:
        raise IsolationError(
            f"refusing to use {what} name {name!r}: it is the user's own "
            f"input space (or contains a path separator). Isolation would be "
            f"a no-op and the engine would appear on his screen.")
    return name


# ---------------------------------------------------------------------------
# Win32 plumbing
# ---------------------------------------------------------------------------

def _win32():
    """Bind the USER32/ADVAPI32 entry points with correct 64-bit argtypes.

    argtypes are not optional here: without them ctypes marshals HANDLEs as
    c_int and TRUNCATES them on 64-bit Python, so the call silently targets a
    bogus object -- the same footgun already documented for SetWindowPos in
    nofocus.park_window_offscreen.
    """
    import ctypes
    import ctypes.wintypes as wt

    u32 = ctypes.WinDLL("user32", use_last_error=True)
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    adv = ctypes.WinDLL("advapi32", use_last_error=True)

    class SECURITY_ATTRIBUTES(ctypes.Structure):
        _fields_ = [("nLength", wt.DWORD),
                    ("lpSecurityDescriptor", ctypes.c_void_p),
                    ("bInheritHandle", wt.BOOL)]

    u32.CreateWindowStationW.argtypes = [wt.LPCWSTR, wt.DWORD, wt.DWORD,
                                         ctypes.c_void_p]
    u32.CreateWindowStationW.restype = ctypes.c_void_p
    u32.SetProcessWindowStation.argtypes = [ctypes.c_void_p]
    u32.SetProcessWindowStation.restype = wt.BOOL
    u32.GetProcessWindowStation.argtypes = []
    u32.GetProcessWindowStation.restype = ctypes.c_void_p
    u32.CreateDesktopW.argtypes = [wt.LPCWSTR, wt.LPCWSTR, ctypes.c_void_p,
                                   wt.DWORD, wt.DWORD, ctypes.c_void_p]
    u32.CreateDesktopW.restype = ctypes.c_void_p
    u32.SetThreadDesktop.argtypes = [ctypes.c_void_p]
    u32.SetThreadDesktop.restype = wt.BOOL
    u32.GetThreadDesktop.argtypes = [wt.DWORD]
    u32.GetThreadDesktop.restype = ctypes.c_void_p
    u32.GetUserObjectInformationW.argtypes = [ctypes.c_void_p, ctypes.c_int,
                                              ctypes.c_void_p, wt.DWORD,
                                              ctypes.POINTER(wt.DWORD)]
    u32.GetUserObjectInformationW.restype = wt.BOOL
    k32.GetCurrentThreadId.restype = wt.DWORD
    adv.ConvertStringSecurityDescriptorToSecurityDescriptorW.argtypes = [
        wt.LPCWSTR, wt.DWORD, ctypes.POINTER(ctypes.c_void_p),
        ctypes.POINTER(wt.DWORD)]
    adv.ConvertStringSecurityDescriptorToSecurityDescriptorW.restype = wt.BOOL
    return ctypes, wt, u32, k32, adv, SECURITY_ATTRIBUTES


def _user_object_name(handle) -> str:
    ctypes, wt, u32, _k32, _adv, _sa = _win32()
    buf = ctypes.create_unicode_buffer(256)
    need = wt.DWORD()
    ok = u32.GetUserObjectInformationW(handle, _UOI_NAME, buf,
                                       ctypes.sizeof(buf), ctypes.byref(need))
    return buf.value if ok else ""


def _security_attributes(sddl: "Optional[str]"):
    """SECURITY_ATTRIBUTES from an SDDL string, or None on any failure."""
    if not sddl:
        return None
    ctypes, wt, _u32, _k32, adv, SECURITY_ATTRIBUTES = _win32()
    sd = ctypes.c_void_p()
    size = wt.DWORD()
    if not adv.ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl, 1, ctypes.byref(sd), ctypes.byref(size)):
        return None
    sa = SECURITY_ATTRIBUTES()
    sa.nLength = ctypes.sizeof(SECURITY_ATTRIBUTES)
    sa.lpSecurityDescriptor = sd
    sa.bInheritHandle = False
    return sa


# ---------------------------------------------------------------------------
# The canary: PROVE isolation rather than assume it
# ---------------------------------------------------------------------------

#: Runs as a child process, i.e. it reports what a launched ENGINE would get,
#: not what this process merely believes. Read-only probes only: it never
#: calls ClipCursor or SetCursorPos, because in the desktop-only tier those
#: succeed and would disturb the pointer of the user we are protecting.
_CANARY = r"""
import ctypes, ctypes.wintypes as wt
u = ctypes.WinDLL('user32', use_last_error=True)
k = ctypes.WinDLL('kernel32', use_last_error=True)
u.GetProcessWindowStation.restype = ctypes.c_void_p
u.GetThreadDesktop.argtypes = [wt.DWORD]
u.GetThreadDesktop.restype = ctypes.c_void_p
u.GetUserObjectInformationW.argtypes = [ctypes.c_void_p, ctypes.c_int,
                                        ctypes.c_void_p, wt.DWORD,
                                        ctypes.POINTER(wt.DWORD)]
u.GetUserObjectInformationW.restype = wt.BOOL
u.OpenInputDesktop.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
u.OpenInputDesktop.restype = ctypes.c_void_p
u.GetCursorPos.argtypes = [ctypes.POINTER(wt.POINT)]
u.GetCursorPos.restype = wt.BOOL
u.GetForegroundWindow.restype = ctypes.c_void_p

def nm(h):
    b = ctypes.create_unicode_buffer(256); n = wt.DWORD()
    if u.GetUserObjectInformationW(h, 2, b, ctypes.sizeof(b), ctypes.byref(n)):
        return b.value
    return '?'

ws = nm(ctypes.c_void_p(u.GetProcessWindowStation()))
dk = nm(ctypes.c_void_p(u.GetThreadDesktop(k.GetCurrentThreadId())))
pt = wt.POINT()
getcur = 1 if u.GetCursorPos(ctypes.byref(pt)) else 0
h = u.OpenInputDesktop(0, False, 0x0100)
oid = 1 if h else 0
fg = 1 if u.GetForegroundWindow() else 0
print('WINSTA=%s' % ws)
print('DESKTOP=%s' % dk)
print('GETCURSORPOS=%d' % getcur)
print('OPENINPUTDESKTOP=%d' % oid)
print('FOREGROUND=%d' % fg)
"""

_CREATE_NO_WINDOW = 0x08000000


def _run_canary(timeout: float = 30.0) -> dict:
    """Spawn a child and ask it, from the inside, where it actually landed."""
    si = subprocess.STARTUPINFO()
    si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    si.wShowWindow = 0  # SW_HIDE; the canary creates no window regardless
    cp = subprocess.run([sys.executable, "-c", _CANARY],
                        capture_output=True, text=True, timeout=timeout,
                        startupinfo=si, creationflags=_CREATE_NO_WINDOW)
    if cp.returncode != 0:
        raise IsolationError(
            f"isolation canary failed (rc={cp.returncode}): "
            f"{(cp.stderr or '').strip()[:400]}")
    out = {}
    for line in (cp.stdout or "").splitlines():
        if "=" in line:
            k, _, v = line.partition("=")
            out[k.strip()] = v.strip()
    return out


def _verify(tier: str, winsta: str, desktop: str) -> dict:
    ev = _run_canary()
    got_desk = ev.get("DESKTOP", "")
    if got_desk.lower() != desktop.lower():
        raise IsolationError(
            f"a child process landed on desktop {got_desk!r}, not the "
            f"isolated {desktop!r}. Launching would put the engine on the "
            f"user's screen. Refusing.")
    if tier == TIER_WINSTA:
        got_ws = ev.get("WINSTA", "")
        if got_ws.lower() != winsta.lower():
            raise IsolationError(
                f"a child process landed on window station {got_ws!r}, not "
                f"the isolated {winsta!r}. Refusing to launch.")
        if ev.get("OPENINPUTDESKTOP") != "0":
            raise IsolationError(
                "a child process could open the INPUT desktop from the "
                "isolated window station; the boundary is not holding. "
                "Refusing to launch.")
    if ev.get("GETCURSORPOS") != "0":
        raise IsolationError(
            "a child process could read the cursor position, which means it "
            "is on an interactive desktop. Refusing to launch.")
    return ev


# ---------------------------------------------------------------------------
# Establishment
# ---------------------------------------------------------------------------

def _attach_winsta(name: str):
    """Create-or-open the private window station and move this PROCESS to it."""
    ctypes, _wt, u32, _k32, _adv, _sa = _win32()
    h = u32.CreateWindowStationW(name, 0, _WINSTA_ALL_ACCESS, None)
    if not h:
        raise OSError(ctypes.get_last_error(),
                      f"CreateWindowStationW({name!r}) failed")
    if not u32.SetProcessWindowStation(ctypes.c_void_p(h)):
        raise OSError(ctypes.get_last_error(),
                      f"SetProcessWindowStation({name!r}) failed")
    return h


def _create_desktop(name: str, sddl: "Optional[str]"):
    """Create-or-open a desktop ON THE PROCESS'S CURRENT window station."""
    ctypes, _wt, u32, _k32, _adv, _sa = _win32()
    sa = _security_attributes(sddl)
    h = u32.CreateDesktopW(name, None, None, 0, _DESKTOP_RIGHTS_NO_SWITCH,
                           ctypes.byref(sa) if sa is not None else None)
    if not h and sa is not None:
        # An over-tight DACL is not worth failing over: retry with the
        # default one. Tier "winsta" does not need the ACL at all, and tier
        # "desktop" still blocks focus/visibility/cursor-position without it.
        h = u32.CreateDesktopW(name, None, None, 0,
                               _DESKTOP_RIGHTS_NO_SWITCH, None)
    if not h:
        raise OSError(ctypes.get_last_error(),
                      f"CreateDesktopW({name!r}) failed")
    return h


def _set_inherited_desktop(full_name: str) -> str:
    """Rewrite this process's RTL_USER_PROCESS_PARAMETERS.DesktopInfo.

    This is the value CreateProcess copies into a child when
    STARTUPINFO.lpDesktop is NULL, i.e. it is what actually decides where a
    subprocess.Popen child lands. Returns the previous value.

    Every structural assumption is validated before anything is written, and
    the write is a POINTER swap into memory this process owns (not a
    cross-process write): if the layout is not what we expect we raise, and
    establish() turns that into a refusal to launch.
    """
    import ctypes
    import ctypes.wintypes as wt

    if ctypes.sizeof(ctypes.c_void_p) != 8:
        raise IsolationError(
            "desktop isolation needs 64-bit Python (the process-parameters "
            "layout below is x64-only).")

    class UNICODE_STRING(ctypes.Structure):
        _fields_ = [("Length", ctypes.c_ushort),
                    ("MaximumLength", ctypes.c_ushort),
                    ("Buffer", ctypes.c_void_p)]

    class PROCESS_BASIC_INFORMATION(ctypes.Structure):
        _fields_ = [("Reserved1", ctypes.c_void_p),
                    ("PebBaseAddress", ctypes.c_void_p),
                    ("Reserved2", ctypes.c_void_p * 2),
                    ("UniqueProcessId", ctypes.c_void_p),
                    ("Reserved3", ctypes.c_void_p)]

    ntdll = ctypes.WinDLL("ntdll", use_last_error=True)
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    ntdll.NtQueryInformationProcess.argtypes = [
        wt.HANDLE, ctypes.c_int, ctypes.c_void_p, ctypes.c_ulong,
        ctypes.POINTER(ctypes.c_ulong)]

    pbi = PROCESS_BASIC_INFORMATION()
    used = ctypes.c_ulong()
    status = ntdll.NtQueryInformationProcess(
        k32.GetCurrentProcess(), _PROCESS_BASIC_INFORMATION,
        ctypes.byref(pbi), ctypes.sizeof(pbi), ctypes.byref(used))
    if status != 0 or not pbi.PebBaseAddress:
        raise IsolationError(
            f"NtQueryInformationProcess failed (status=0x{status & 0xffffffff:08x}); "
            f"cannot retarget child-process desktop inheritance.")

    params = ctypes.c_void_p.from_address(
        pbi.PebBaseAddress + _PEB_PROCESS_PARAMETERS_OFFSET).value
    if not params:
        raise IsolationError("PEB.ProcessParameters is NULL; refusing to launch.")

    info = UNICODE_STRING.from_address(params + _RUPP_DESKTOP_INFO_OFFSET)
    previous = ""
    if info.Buffer and info.Length:
        try:
            previous = ctypes.wstring_at(info.Buffer, info.Length // 2)
        except (OSError, ValueError):
            previous = ""
    # The one load-bearing sanity check: whatever is at this offset must
    # already look like "<window station>\<desktop>". If it does not, the
    # struct layout is not what this code believes and we must not write.
    if not _re_desktop_info.match(previous):
        raise IsolationError(
            f"process-parameters DesktopInfo did not look like "
            f"'<winsta>\\<desktop>' (read {previous!r}); the layout this build "
            f"of Windows uses is not the one this code knows. Refusing to "
            f"launch rather than write to an unknown offset.")

    global _desktop_info_buf
    _desktop_info_buf = ctypes.create_unicode_buffer(full_name)
    info.Buffer = ctypes.cast(_desktop_info_buf, ctypes.c_void_p).value
    info.Length = len(full_name) * 2
    info.MaximumLength = (len(full_name) + 1) * 2
    return previous


def _bind_thread(hdesk) -> None:
    ctypes, _wt, u32, _k32, _adv, _sa = _win32()
    if not u32.SetThreadDesktop(ctypes.c_void_p(hdesk)):
        raise OSError(ctypes.get_last_error(), "SetThreadDesktop failed")


def _already_isolated(winsta: str, desktop: str):
    """Detect isolation inherited from an isolated parent harness."""
    ctypes, _wt, u32, k32, _adv, _sa = _win32()
    cur_ws = _user_object_name(ctypes.c_void_p(u32.GetProcessWindowStation()))
    cur_dk = _user_object_name(
        ctypes.c_void_p(u32.GetThreadDesktop(k32.GetCurrentThreadId())))
    if cur_dk.lower() != desktop.lower():
        return None
    if cur_ws.lower() == winsta.lower():
        return TIER_WINSTA
    if cur_ws.lower() != "winsta0":
        return TIER_WINSTA
    return TIER_DESKTOP


def establish(*, quiet: bool = False) -> "Optional[IsolationState]":
    """Put this process (and every descendant) out of the user's input space.

    Idempotent and cached, including the failure: once isolation has failed in
    a process it stays failed, so a caller cannot retry its way onto the
    user's desktop.

    Returns None when isolation is deliberately disabled
    (MC2_HARNESS_ALLOW_FOREGROUND=1). Raises IsolationError otherwise.
    """
    global _state, _failure

    if disabled():
        return None
    if sys.platform != "win32":
        # No window stations to isolate with. Every launch site in this repo
        # is Windows-only today; refuse rather than pretend.
        raise IsolationError(
            "desktop isolation is implemented for Windows only; refusing to "
            "launch the engine without it.")

    with _lock:
        if _state is not None:
            return _state
        if _failure is not None:
            raise IsolationError(_failure)
        try:
            _state = _establish_locked(quiet=quiet)
        except IsolationError as exc:
            _failure = str(exc)
            raise
        except Exception as exc:  # noqa: BLE001 - convert everything
            _failure = f"{type(exc).__name__}: {exc}"
            raise IsolationError(_failure) from exc
        return _state


def _establish_locked(*, quiet: bool) -> IsolationState:
    winsta = _object_name(os.environ.get("MC2_ISOLATION_WINSTA", ""),
                          _DEFAULT_WINSTA, "window station")
    desktop = _object_name(os.environ.get("MC2_ISOLATION_DESKTOP", ""),
                           _DEFAULT_DESKTOP, "desktop")
    minimum = _min_tier()

    inherited = _already_isolated(winsta, desktop)
    if inherited is not None:
        if inherited == TIER_DESKTOP and minimum == TIER_WINSTA:
            raise IsolationError(
                "inherited desktop-only isolation but MC2_ISOLATION_MIN=winsta")
        ev = _verify(inherited, winsta, desktop)
        st = IsolationState(inherited, winsta, desktop, None, None, ev)
        if not quiet:
            _announce(st, inherited_from_parent=True)
        return st

    tier = TIER_WINSTA
    hwinsta = None
    winsta_error = ""
    try:
        hwinsta = _attach_winsta(winsta)
    except OSError as exc:
        winsta_error = str(exc)
        tier = TIER_DESKTOP

    if tier == TIER_DESKTOP and minimum == TIER_WINSTA:
        raise IsolationError(
            f"MC2_ISOLATION_MIN=winsta but a private window station could not "
            f"be created ({winsta_error}). Refusing to launch.")

    # From here on a partial state is possible (process moved to a private
    # window station but no desktop bound). Any failure below is terminal --
    # we never unwind back onto WinSta0/Default and continue, because that is
    # exactly the silent fall-through that let this bug survive twice.
    hdesk = _create_desktop(desktop,
                            _SDDL_NO_SWITCH if tier == TIER_DESKTOP else None)
    _bind_thread(hdesk)

    # The step that actually moves the CHILDREN. Binding this thread is not
    # enough (measured), so retarget the inheritance source too.
    host_winsta = winsta if tier == TIER_WINSTA else "Winsta0"
    _set_inherited_desktop(f"{host_winsta}\\{desktop}")

    ev = _verify(tier, winsta, desktop)
    st = IsolationState(tier, winsta, desktop, hwinsta, hdesk, ev)
    if not quiet:
        _announce(st, inherited_from_parent=False, winsta_error=winsta_error)
    return st


def _announce(st: IsolationState, *, inherited_from_parent: bool,
              winsta_error: str = "") -> None:
    where = "inherited" if inherited_from_parent else "established"
    if st.tier == TIER_WINSTA:
        detail = f"window station {st.winsta}\\{st.desktop}"
    else:
        detail = f"desktop {st.desktop} (WinSta0)"
    print(f"[isolation] {where} tier={st.tier}: {detail} -- the engine cannot "
          f"reach the user's screen, focus, keyboard or pointer.",
          file=sys.stderr)
    if st.tier == TIER_DESKTOP and winsta_error:
        print(f"[isolation] NOTE: private window station unavailable "
              f"({winsta_error}); ClipCursor is window-station scoped and "
              f"therefore still reachable, so MC2_NO_CURSOR_GRAB=1 is being "
              f"FORCED (not defaulted) for every child.", file=sys.stderr)


def ensure_thread() -> "Optional[IsolationState]":
    """Establish isolation AND bind the CALLING thread's desktop.

    CreateProcess with lpDesktop==NULL uses the calling THREAD's desktop, so a
    harness that spawns from a worker thread would otherwise inherit the
    user's desktop. Every launch entry point calls this first.
    """
    st = establish()
    if st is None:
        return None
    ctypes, _wt, u32, k32, _adv, _sa = _win32()
    cur = _user_object_name(
        ctypes.c_void_p(u32.GetThreadDesktop(k32.GetCurrentThreadId())))
    if cur.lower() == st.desktop.lower():
        return st
    if st._hdesk is None:
        # Isolation was inherited from a parent process; we hold no handle to
        # re-bind with. A thread that is somehow off-desktop cannot be fixed.
        raise IsolationError(
            f"thread {threading.current_thread().name!r} is on desktop "
            f"{cur!r}, not the inherited {st.desktop!r}, and no desktop "
            f"handle is available to re-bind it. Refusing to launch.")
    try:
        _bind_thread(st._hdesk)
    except OSError as exc:
        raise IsolationError(
            f"could not bind thread {threading.current_thread().name!r} to "
            f"the isolated desktop ({exc}). Refusing to launch.") from exc
    return st


def state() -> "Optional[IsolationState]":
    """The established isolation, or None if establish() has not run/applies."""
    return _state


def describe() -> str:
    if disabled():
        return "disabled (MC2_HARNESS_ALLOW_FOREGROUND)"
    if _state is None:
        return "not established"
    return f"tier={_state.tier} {_state.winsta}\\{_state.desktop}"


def harden_env(env: dict) -> dict:
    """Apply the env hardening that the ACTIVE tier makes non-negotiable.

    Tier "winsta" needs none of this -- ClipCursor is denied by the kernel.
    Tier "desktop" shares the window station's cursor clip rect with the user,
    so MC2_NO_CURSOR_GRAB is FORCED here (assignment, not setdefault) and a
    caller cannot opt back into cursor grabbing in the leaky tier.
    """
    if _state is not None and _state.tier == TIER_DESKTOP:
        env["MC2_NO_CURSOR_GRAB"] = "1"
    return env


def assert_composite_capture_supported(who: str) -> None:
    """Fail LOUDLY in a harness that screenshots the DESKTOP COMPOSITE.

    pyautogui / mss / PIL.ImageGrab grab the input desktop. From an isolated
    desktop they raise "screen grab failed", or -- worse in the desktop-only
    tier -- could capture the USER'S screen and silently bless his wallpaper
    as a golden image. Neither is acceptable, so say so up front instead of
    after a 60-second run.

    The supported path is the engine's own readback: MC2_VISUAL_CAPTURE /
    MC2_SCREENSHOT_SOURCE=backbuffer (glReadPixels from the scene FBO or from
    FBO 0), which is unaffected by isolation and is what
    scripts/run_visual_capture.py and tools/visual_verify/ already use.
    """
    if disabled():
        return
    if _state is None and sys.platform == "win32":
        # Not yet established: this harness has not launched anything, so the
        # capture would target the user's desktop. Still wrong for a golden.
        pass
    raise IsolationError(
        f"{who} captures the DESKTOP COMPOSITE (pyautogui/mss/ImageGrab), "
        f"which cannot see an isolated engine window and may instead capture "
        f"the user's own screen. Port it to the engine readback path "
        f"(MC2_VISUAL_CAPTURE=1 or MC2_SCREENSHOT_SOURCE=backbuffer, see "
        f"scripts/run_visual_capture.py), or run it in a user-present session "
        f"with MC2_HARNESS_ALLOW_FOREGROUND=1.")
