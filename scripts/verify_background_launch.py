#!/usr/bin/env python3
"""VERIFY-BACKGROUND-LAUNCH-1 -- prove a smoke launch never shows or activates a window.

WHY THIS EXISTS
---------------
check-background-launch.py is static: it proves the CODE routes launches through
nofocus. This is the dynamic half. It launches one real smoke through the fixed
path, polls the desktop twice a second for the first N seconds, and fails if any
window belonging to the launched PROCESS TREE (the harness, mc2.exe, every
console helper they spawn) is

  * the foreground window (focus theft), or
  * visible AND on a monitor and not minimized (a window the user can see) --
    this is the "console window popped up" case, which GetForegroundWindow alone
    cannot see because a console can appear without activating.

A window that is "visible" to Win32 but parked at (-32000,-32000) / iconic is
the intended steady state for an engine window and is reported separately as
`parked`, not as a violation.

Pure ctypes (user32 / kernel32); Windows only. Never touches the mouse or
keyboard: it only reads window state.

Usage:
  py -3 scripts/verify_background_launch.py [--seconds 20] [-- <run_smoke args>]
  default smoke args: --mission mc2_01 --duration 10 --keep-logs
"""
from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from smoke_lib import nofocus  # noqa: E402

user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32

user32.GetForegroundWindow.restype = wt.HWND
user32.GetWindowTextW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
user32.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
user32.IsWindowVisible.argtypes = [wt.HWND]
user32.IsIconic.argtypes = [wt.HWND]
user32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
user32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]

TH32CS_SNAPPROCESS = 0x2


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD),
                ("th32ProcessID", wt.DWORD), ("th32DefaultHeapID", ctypes.c_void_p),
                ("th32ModuleID", wt.DWORD), ("cntThreads", wt.DWORD),
                ("th32ParentProcessID", wt.DWORD), ("pcPriClassBase", ctypes.c_long),
                ("dwFlags", wt.DWORD), ("szExeFile", ctypes.c_wchar * 260)]


def process_table() -> dict:
    """pid -> (ppid, exe name) for every live process."""
    kernel32.CreateToolhelp32Snapshot.restype = wt.HANDLE
    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    out = {}
    pe = PROCESSENTRY32W()
    pe.dwSize = ctypes.sizeof(pe)
    ok = kernel32.Process32FirstW(snap, ctypes.byref(pe))
    while ok:
        out[pe.th32ProcessID] = (pe.th32ParentProcessID, pe.szExeFile)
        ok = kernel32.Process32NextW(snap, ctypes.byref(pe))
    kernel32.CloseHandle(snap)
    return out


def descendants(root_pid: int, table: dict) -> set:
    tree = {root_pid}
    changed = True
    while changed:
        changed = False
        for pid, (ppid, _n) in table.items():
            if ppid in tree and pid not in tree:
                tree.add(pid)
                changed = True
    return tree


def _text(fn, hwnd) -> str:
    buf = ctypes.create_unicode_buffer(256)
    fn(hwnd, buf, 256)
    return buf.value


def virtual_screen():
    return (user32.GetSystemMetrics(76), user32.GetSystemMetrics(77),
            user32.GetSystemMetrics(76) + user32.GetSystemMetrics(78),
            user32.GetSystemMetrics(77) + user32.GetSystemMetrics(79))


def on_screen(rect, vs) -> bool:
    return rect.right > vs[0] and rect.left < vs[2] and rect.bottom > vs[1] and rect.top < vs[3] \
        and (rect.right - rect.left) > 0 and (rect.bottom - rect.top) > 0


def enum_windows() -> list:
    """[(hwnd, pid, visible, iconic, rect, title, cls)] for every top-level window."""
    found = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)
    def cb(hwnd, _lp):
        pid = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        rect = wt.RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(rect))
        found.append((hwnd, pid.value, bool(user32.IsWindowVisible(hwnd)),
                      bool(user32.IsIconic(hwnd)), rect,
                      _text(user32.GetWindowTextW, hwnd), _text(user32.GetClassNameW, hwnd)))
        return True

    user32.EnumWindows(cb, 0)
    return found


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seconds", type=float, default=20.0,
                    help="minimum poll window (default 20)")
    ap.add_argument("--max-seconds", type=float, default=150.0,
                    help="keep polling until run_smoke exits, up to this long: the "
                         "engine is only spawned AFTER the harness prelude (lease, "
                         "preflight, git), so a fixed 20s can end before mc2.exe exists")
    ap.add_argument("--interval", type=float, default=0.5)
    ap.add_argument("smoke_args", nargs="*",
                    help="run_smoke.py args (default: --mission mc2_01 "
                         "--duration 10 --keep-logs)")
    a = ap.parse_args()
    smoke_args = a.smoke_args or ["--mission", "mc2_01", "--duration", "10",
                                  "--keep-logs"]

    before = {w[0] for w in enum_windows()}
    env = dict(os.environ)
    env["MC2_CURSOR_NO_PARK"] = "1"
    log_path = Path(tempfile.gettempdir()) / "verify_background_launch.last.log"
    log = open(log_path, "w", encoding="utf-8")
    # The verifier's own launch is windowless too (hidden console + redirected output).
    proc = subprocess.Popen([sys.executable, str(ROOT / "scripts" / "run_smoke.py"), *smoke_args],
                            cwd=str(ROOT), env=env, stdout=log, stderr=subprocess.STDOUT,
                            **nofocus.hidden_kwargs())
    print(f"[verify-bg] run_smoke output -> {log_path}")
    print(f"[verify-bg] launched run_smoke pid={proc.pid} args={' '.join(smoke_args)}")
    vs = virtual_screen()
    t0 = time.monotonic()
    fg_titles: list = []
    violations: list = []
    parked: dict = {}
    seen_tree_exes: set = set()
    ticks = 0
    while (time.monotonic() - t0 < a.seconds or proc.poll() is None)             and time.monotonic() - t0 < a.max_seconds:
        ticks += 1
        table = process_table()
        tree = descendants(proc.pid, table) if proc.poll() is None or proc.pid in table else {proc.pid}
        for pid in tree:
            if pid in table:
                seen_tree_exes.add(table[pid][1])
        fg = user32.GetForegroundWindow()
        fpid = wt.DWORD()
        if fg:
            user32.GetWindowThreadProcessId(fg, ctypes.byref(fpid))
        title = _text(user32.GetWindowTextW, fg) if fg else ""
        stamp = f"{time.monotonic() - t0:5.1f}s"
        fg_titles.append((stamp, fpid.value, title))
        if fpid.value in tree:
            violations.append((stamp, "FOREGROUND", fpid.value, title, ""))
        for hwnd, pid, vis, iconic, rect, wtitle, cls in enum_windows():
            if pid not in tree or hwnd in before or not vis:
                continue
            key = (pid, cls, wtitle)
            if iconic or not on_screen(rect, vs):
                parked.setdefault(key, (stamp, (rect.left, rect.top)))
            else:
                violations.append((stamp, "VISIBLE-ONSCREEN", pid, wtitle or cls,
                                   f"{table.get(pid, ('', '?'))[1]} class={cls} "
                                   f"rect=({rect.left},{rect.top},{rect.right},{rect.bottom})"))
        time.sleep(a.interval)

    rc = proc.poll()
    print(f"[verify-bg] polled {ticks} ticks over {time.monotonic() - t0:.0f}s; run_smoke "
          f"{'still running' if rc is None else 'exited rc=%s' % rc}")
    print("[verify-bg] foreground window by tick (never the launched tree):")
    last = None
    for stamp, pid, title in fg_titles:
        row = (pid, title)
        if row != last:
            print(f"    {stamp}  pid={pid:<6} {title!r}")
            last = row
    print(f"[verify-bg] process tree exes seen: {sorted(seen_tree_exes)}")
    if "mc2.exe" not in seen_tree_exes and "Mission Editor.exe" not in seen_tree_exes:
        print("[verify-bg] WARNING: the engine never appeared in the process tree during "
              "the poll -- this run proves nothing about the game window")
        violations.append(("-", "ENGINE-NOT-OBSERVED", 0, "", ""))
    if parked:
        print("[verify-bg] parked windows (visible to Win32 but offscreen/iconic -- OK):")
        for (pid, cls, wtitle), (stamp, pos) in sorted(parked.items()):
            print(f"    {stamp} pid={pid} class={cls} title={wtitle!r} at {pos}")
    if violations:
        print(f"[verify-bg] FAIL: {len(violations)} violation(s):")
        for v in violations[:20]:
            print("    ", v)
    else:
        print("[verify-bg] PASS: zero foreground steals, zero visible on-screen windows "
              "from the launched process tree")
    try:
        proc.wait(timeout=240)
    except subprocess.TimeoutExpired:
        proc.kill()
    log.close()
    return 1 if violations else 0


if __name__ == "__main__":
    sys.exit(main())
