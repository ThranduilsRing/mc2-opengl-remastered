#!/usr/bin/env python3
"""Spawn two smoke runs (legacy + fast-path), screenshot each at ~30s, save side-by-side."""
import os
import sys
import time
import subprocess
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

EXE = Path(r"A:/Games/mc2-opengl/mc2-win64-v0.2/mc2.exe")
ARTIFACT_DIR = ROOT / "tests" / "smoke" / "artifacts" / f"water-diff-{int(time.time())}"
ARTIFACT_DIR.mkdir(parents=True, exist_ok=True)

COMMON_ENV = {
    "MC2_SMOKE_MODE": "1",
    "MC2_SMOKE_SEED": "0xC0FFEE",
    "MC2_HEARTBEAT": "1",
    "MC2_PATCHSTREAM_THIN_RECORDS": "1",
    "MC2_PATCHSTREAM_THIN_RECORDS_DRAW": "1",
    "MC2_PATCHSTREAM_THIN_RECORD_FASTPATH": "1",
    "MC2_MODERN_TERRAIN_PATCHES": "1",
    "MC2_WATER_STREAM_DEBUG": "1",
    "MC2_WATER_DEBUG": "1",
}

def kill_existing():
    try:
        subprocess.run(["taskkill", "/F", "/IM", "mc2.exe"],
                       capture_output=True, text=True)
    except Exception:
        pass
    time.sleep(1)

def run_and_screenshot(mode_label, fast_path: bool, mission="mc2_01", wait_sec=35,
                       fastpath_debug=None):
    kill_existing()
    env = os.environ.copy()
    env.update(COMMON_ENV)
    if fast_path:
        env["MC2_RENDER_WATER_FASTPATH"] = "1"
        if fastpath_debug is not None:
            env["MC2_RENDER_WATER_FASTPATH_DEBUG"] = str(fastpath_debug)
        else:
            env.pop("MC2_RENDER_WATER_FASTPATH_DEBUG", None)
    else:
        env.pop("MC2_RENDER_WATER_FASTPATH", None)
        env.pop("MC2_RENDER_WATER_FASTPATH_DEBUG", None)

    log_path = ARTIFACT_DIR / f"{mode_label}.log"
    log_fp = open(log_path, "w", encoding="utf-8", errors="replace")

    print(f"[{mode_label}] launching mc2.exe (fastpath={fast_path}) for {mission}")
    proc = subprocess.Popen(
        [str(EXE), "--profile", "stock", "--mission", mission, "--duration", str(wait_sec + 10)],
        stdout=log_fp, stderr=subprocess.STDOUT, env=env,
        cwd=str(EXE.parent),
    )

    print(f"[{mode_label}] sleeping {wait_sec}s to let mission load + stabilize...")
    time.sleep(wait_sec)

    # Bring mc2 window to foreground before screenshot
    try:
        import ctypes
        user32 = ctypes.windll.user32
        # Find mc2 window by class/title - try a few names
        hwnd = user32.FindWindowW(None, "mc2")
        if not hwnd:
            hwnd = user32.FindWindowW(None, "MechCommander 2")
        if not hwnd:
            hwnd = user32.FindWindowW(None, "mc2 - Remastered")
        if hwnd:
            user32.SetForegroundWindow(hwnd)
            time.sleep(0.5)
            print(f"[{mode_label}] brought mc2 window to foreground (hwnd={hwnd})")
        else:
            print(f"[{mode_label}] WARN: could not find mc2 window by title")
    except Exception as e:
        print(f"[{mode_label}] WARN: foreground bring failed: {e}")

    shot = ARTIFACT_DIR / f"{mode_label}.png"
    print(f"[{mode_label}] screenshot -> {shot}")
    try:
        import pyautogui
        pyautogui.FAILSAFE = False
        img = pyautogui.screenshot()
        img.save(str(shot))
    except Exception as e:
        print(f"[{mode_label}] screenshot failed: {e}")

    print(f"[{mode_label}] killing process and waiting...")
    proc.kill()
    try:
        proc.wait(timeout=10)
    except Exception:
        pass
    log_fp.close()

if __name__ == "__main__":
    mission = sys.argv[1] if len(sys.argv) > 1 else "mc2_01"
    print(f"=== water visual diff for {mission} -> {ARTIFACT_DIR} ===")
    run_and_screenshot("debug-alpha",     fast_path=True,  mission=mission, fastpath_debug=4)
    time.sleep(3)
    run_and_screenshot("debug-elev",      fast_path=True,  mission=mission, fastpath_debug=6)
    print(f"\nartifacts in: {ARTIFACT_DIR}")
    for p in sorted(ARTIFACT_DIR.iterdir()):
        print(f"  {p.name}: {p.stat().st_size} bytes")
