"""Launch mc2.exe and capture screenshots at multiple times during the intro.

Usage:
  py -3 scripts/quick_sweep.py <mission> <label> <t1,t2,t3,...>
  e.g. py -3 scripts/quick_sweep.py mc2_10 coordfix 8,14,20,26
"""
import os, sys, subprocess, time
from pathlib import Path

MISSION = sys.argv[1] if len(sys.argv) > 1 else "mc2_10"
LABEL   = sys.argv[2] if len(sys.argv) > 2 else "sweep"
TIMES   = [float(x) for x in (sys.argv[3] if len(sys.argv) > 3 else "8,14,20,26").split(",")]
EXE = Path(r"A:/Games/mc2-opengl/mc2-win64-v0.3/mc2.exe")
OUT_DIR = Path(r"A:/Games/mc2-main/tests/smoke/artifacts/diag-shots")
OUT_DIR.mkdir(parents=True, exist_ok=True)

DURATION = int(max(TIMES) + 10)

def kill():
    try:
        subprocess.run(["taskkill", "/F", "/IM", "mc2.exe"], capture_output=True, text=True)
    except Exception:
        pass
    time.sleep(1)

kill()

env = os.environ.copy()
env.update({
    "MC2_GPU_CULL": "1",
    "MC2_HEARTBEAT": "1",
    "MC2_SMOKE_MODE": "1",
    "MC2_SMOKE_SEED": "0",
})
if os.environ.get("QUICK_SHOT_SUBSTRATE") == "1":
    env["MC2_GPU_CULL_SUBSTRATE"] = "1"
if os.environ.get("QUICK_SHOT_KILLSWITCH") == "1":
    env["MC2_SUBSTRATE_COALESCE_LEGACY"] = "1"
if os.environ.get("QUICK_SHOT_NODILATION") == "1":
    env["MC2_GPU_CULL_FRUSTUM_DILATION"] = "0"
if os.environ.get("QUICK_SHOT_CULLTRACE") == "1":
    env["MC2_GPU_CULL_COMPUTE_TRACE"] = "1"
if os.environ.get("QUICK_SHOT_SUBSTRATETRACE") == "1":
    env["MC2_GPU_CULL_SUBSTRATE_TRACE"] = "1"

log_path = OUT_DIR / f"{MISSION}-{LABEL}.log"
log_fp = open(log_path, "w", encoding="utf-8", errors="replace")

print(f"[sweep] launching mc2.exe mission={MISSION} duration={DURATION}s label={LABEL} times={TIMES}")
proc = subprocess.Popen(
    [str(EXE), "--profile", "stock", "--mission", MISSION, "--duration", str(DURATION)],
    stdout=log_fp, stderr=subprocess.STDOUT, env=env,
    cwd=str(EXE.parent),
)

# Foregrounder helper
import ctypes
user32 = ctypes.windll.user32
def foreground():
    hwnd = user32.FindWindowW(None, "mc2") or \
           user32.FindWindowW(None, "MechCommander 2") or \
           user32.FindWindowW(None, "mc2 - Remastered")
    if hwnd:
        user32.SetForegroundWindow(hwnd)
        return True
    return False

import pyautogui
pyautogui.FAILSAFE = False

start = time.time()
for t in TIMES:
    target = start + t
    while time.time() < target:
        time.sleep(0.05)
    foreground()
    time.sleep(0.3)
    shot_path = OUT_DIR / f"{MISSION}-{LABEL}-t{int(t):02d}s.png"
    try:
        img = pyautogui.screenshot()
        img.save(str(shot_path))
        print(f"[sweep] t={t:.1f}s -> {shot_path.name}")
    except Exception as e:
        print(f"[sweep] ERROR pyautogui at t={t}: {e}")

# wait remainder then kill
remain = (start + DURATION) - time.time()
if remain > 0:
    time.sleep(min(remain, 5))
kill()
print(f"[sweep] log saved {log_path}")
print(f"[sweep] DONE")
