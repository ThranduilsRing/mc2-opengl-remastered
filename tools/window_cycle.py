#!/usr/bin/env python3
"""Minimize / restore / resize the game window during a mission and grade the present path's recovery.

Launches one mission through nofocus.spawn (host game lease, exe vetting, offscreen window, no activation), finds the
engine's top-level window by pid, then for --cycles rounds: ShowWindow(SW_SHOWMINNOACTIVE) -> ShowWindow(SW_SHOWNOACTIVATE)
-> SetWindowPos resize (NOMOVE|NOZORDER|NOACTIVATE, so the window stays offscreen and never takes focus). The engine
exits on its own at --duration.

Verdict: PASS when the engine exits 0 after the cycles, and the log has no [PRESENT_THREAD] STALL line and no fatal backend line.
The tool also counts the recovery lines: `[VK_PRESENT] present failure treated as transient` (the failure handoff) and
`[VK_PRESENT] deferred init recovered` (the re-init). It reports both counts but requires neither, because a minimize that
the swapchain rebuild absorbs is a pass too.

    py -3 tools/window_cycle.py --install G:/mc2-installs/<lane> [--mission mc2_01] [--out G:/mc2-tmp/<lane>/window_cycle]
"""
from __future__ import annotations

import argparse
import ctypes
import os
import re
import subprocess
import sys
import time
from ctypes import wintypes
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from smoke_lib import nofocus, runner  # noqa: E402

SW_SHOWNOACTIVATE, SW_SHOWMINNOACTIVE = 4, 7
SWP_NOMOVE, SWP_NOZORDER, SWP_NOACTIVATE = 0x0002, 0x0004, 0x0010
SIZES = ((1024, 600), (854, 480), (640, 400))

TRANSIENT = "[VK_PRESENT] present failure treated as transient"
RECOVERED = "[VK_PRESENT] deferred init recovered"
STALL = re.compile(r"\[PRESENT_THREAD\] STALL")
FATAL = re.compile(r"fatalBackendFailure|\[RENDER_BACKEND\].*fatal|present worker never started|held >30s", re.I)


def _user32():
    u32 = ctypes.windll.user32
    u32.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
    u32.SetWindowPos.argtypes = [wintypes.HWND, wintypes.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_uint]
    u32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    u32.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
    return u32


def find_window(u32, pid: int):
    """The engine's SDL top-level window, or None."""
    found = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)
    def cb(hwnd, _lp):
        owner = wintypes.DWORD()
        u32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid:
            name = ctypes.create_unicode_buffer(64)
            u32.GetClassNameW(hwnd, name, 64)
            if name.value.startswith("SDL"):
                found.append(hwnd)
        return True

    u32.EnumWindows(cb, 0)
    return found[0] if found else None


def wait_for(pred, timeout_s: float, step_s: float = 0.25) -> bool:
    end = time.monotonic() + timeout_s
    while time.monotonic() < end:
        if pred():
            return True
        time.sleep(step_s)
    return False


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--install", required=True, help="install dir holding mc2.exe (a lane's own G:/mc2-installs/<branch>)")  # drive-path: ok PC path in user-facing help
    ap.add_argument("--mission", default="mc2_01")
    ap.add_argument("--duration", type=int, default=40, help="engine --duration seconds")
    ap.add_argument("--settle", type=float, default=12.0, help="seconds after the window appears before the first cycle")
    ap.add_argument("--cycles", type=int, default=3)
    ap.add_argument("--hold", type=float, default=1.2, help="seconds in each state (minimized, restored, resized)")
    ap.add_argument("--out", default=os.path.join(os.environ.get("TMP", "G:/mc2-tmp"), "window_cycle"))
    args = ap.parse_args(argv)
    if sys.platform != "win32":
        print("window_cycle: Win32 only")
        return 2
    install = Path(args.install)
    exe = install / "mc2.exe"
    if not exe.is_file():
        print(f"window_cycle: no mc2.exe in {install}")
        return 2
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    log_path = out / "stdout.txt"

    env = os.environ.copy()
    for k in runner.unregistered_ambient(env):   # harness-only knobs: fatal under the engine's MC2_CONFIG_STRICT=1
        env.pop(k, None)
    env.update(runner.FORCED_ENV)
    for k, v in runner.SOFT_ENV.items():
        env.setdefault(k, v)
    for k in runner.POPPED_ENV:
        env.pop(k, None)
    u32 = _user32()
    # Each cycle also loses and regains focus the way an alt-tab does: the input script queues the real SDL focus events.
    script = out / "focus_script.txt"
    script.write_text("", encoding="utf-8")
    env["MC2_INPUT_SCRIPT"] = str(script)
    argv_exe = [str(exe), "--profile", "stock", "--mission", args.mission, "--duration", str(args.duration)]
    # show_window: start restored (offscreen), not minimized, so the first minimize is one this tool makes.
    with open(log_path, "w", encoding="utf-8", errors="replace") as fo:
        proc = nofocus.spawn(argv_exe, show_window=True, cwd=str(install), stdout=fo, stderr=subprocess.STDOUT, env=env)
        try:
            hwnd = None
            if wait_for(lambda: proc.poll() is not None or find_window(u32, proc.pid) is not None, 60):
                hwnd = find_window(u32, proc.pid)
            if hwnd is None:
                print(f"window_cycle: FAIL no engine window (exit={proc.poll()}); log {log_path}")
                return 1
            time.sleep(args.settle)
            done = 0
            for i in range(args.cycles):
                if proc.poll() is not None:
                    break
                w, h = SIZES[i % len(SIZES)]
                with open(script, "a", encoding="utf-8") as fs:
                    fs.write("focus lost\n")
                time.sleep(args.hold)
                u32.ShowWindow(hwnd, SW_SHOWMINNOACTIVE)
                time.sleep(args.hold)
                u32.ShowWindow(hwnd, SW_SHOWNOACTIVATE)
                time.sleep(args.hold)
                with open(script, "a", encoding="utf-8") as fs:
                    fs.write("focus gained\n")
                u32.SetWindowPos(hwnd, None, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)
                time.sleep(args.hold)
                done += 1
                print(f"window_cycle: cycle {done} minimize/restore/resize {w}x{h}", flush=True)
            rc = proc.wait(timeout=args.duration + 90)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
            print(f"window_cycle: FAIL engine did not exit by itself; log {log_path}")
            return 1
    text = log_path.read_text(encoding="utf-8", errors="replace")
    stalls = len(STALL.findall(text))
    fatals = len(FATAL.findall(text))
    transient, recovered = text.count(TRANSIENT), text.count(RECOVERED)
    focus = text.count("cmd=focus lost")
    bundles = text.count("[CRASHBUNDLE]")
    ok = rc == 0 and done == args.cycles and stalls == 0 and fatals == 0 and bundles == 0 and focus == done
    print(f"window_cycle: {'PASS' if ok else 'FAIL'} exit={rc} cycles={done}/{args.cycles} focus_lost={focus} stall={stalls} "
          f"fatal={fatals} crashbundle={bundles} transient={transient} recovered={recovered} log={log_path}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
