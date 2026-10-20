"""Sync-validation regression: resize the menu before lazy mission post-effects exist.

Run explicitly: py -3 tests/smoke/scene_resize.py --install <game dir> --out <log>
Uses only its own offscreen window; never activates a window or injects input.
"""
import argparse
import ctypes
from ctypes import wintypes
import os
from pathlib import Path
import re
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from smoke_lib import nofocus


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--install", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    user = ctypes.WinDLL("user32", use_last_error=True)
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user.EnumWindows.argtypes = [callback_type, wintypes.LPARAM]
    user.IsWindowVisible.argtypes = [wintypes.HWND]
    user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user.SetWindowPos.argtypes = [wintypes.HWND, wintypes.HWND, ctypes.c_int, ctypes.c_int,
                                  ctypes.c_int, ctypes.c_int, wintypes.UINT]
    user.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    env = {k: v for k, v in os.environ.items() if not k.startswith("MC2_")}
    env.update(MC2_VULKAN_VALIDATION="sync", MC2_LOG="1")
    with args.out.open("wb") as log:
        proc = nofocus.spawn([str(args.install / "mc2.exe")], cwd=str(args.install), env=env,
                             stdout=log, stderr=log, show_window=True)
        hwnd = None
        try:
            deadline = time.monotonic() + 60
            while not hwnd and time.monotonic() < deadline and proc.poll() is None:
                windows = []
                @callback_type
                def visit(window, _):
                    pid = wintypes.DWORD()
                    user.GetWindowThreadProcessId(window, ctypes.byref(pid))
                    if pid.value == proc.pid and user.IsWindowVisible(window):
                        windows.append(window)
                    return True
                user.EnumWindows(visit, 0)
                if windows and "[SCENE_TARGETS] backend=Vulkan" in args.out.read_text(errors="replace"):
                    hwnd = windows[0]
                else:
                    time.sleep(0.1)
            if not hwnd:
                raise RuntimeError("no initialized offscreen menu window")
            time.sleep(2)  # submit menu composites before the first resize
            for i in range(40):
                if proc.poll() is not None:
                    raise RuntimeError(f"engine exited during resize: {proc.returncode}")
                w, h = ((854, 480), (960, 540))[i % 2]
                if not user.SetWindowPos(hwnd, None, 0, 0, w, h, 0x0016):
                    raise ctypes.WinError(ctypes.get_last_error())
                time.sleep(0.1)
            time.sleep(1)
            user.PostMessageW(hwnd, 0x0010, 0, 0)  # close only this process's window
            proc.wait(timeout=30)
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait(timeout=10)
    text = args.out.read_text(errors="replace")
    resizes = text.count("[SCENE_TARGETS] backend=Vulkan")
    failures = re.findall(r"^.*(?:severity=error|Validation Error|SYNC-HAZARD|VUID-).*$", text, re.MULTILINE)
    sync = "validation messenger engaged (preset=sync)" in text
    if proc.returncode or resizes < 3 or failures or not sync:
        raise RuntimeError(f"exit={proc.returncode} scene allocations={resizes} validation={failures}")
    print(f"PASS: menu scene allocations={resizes}; zero Vulkan validation errors")


if __name__ == "__main__":
    main()
