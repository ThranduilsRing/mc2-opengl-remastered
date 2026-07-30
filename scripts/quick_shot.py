"""Launch mc2.exe on a specific mission, wait, screenshot, kill."""
import os, sys, subprocess, time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
# BACKGROUND-SMOKE-1: single chokepoint for non-activating launch + guarded focus.
from smoke_lib import nofocus  # noqa: E402

MISSION = sys.argv[1] if len(sys.argv) > 1 else "mc2_10"
WAIT_SEC = int(sys.argv[2]) if len(sys.argv) > 2 else 60
LABEL = sys.argv[3] if len(sys.argv) > 3 else "current"
EXE = Path(r"A:/Games/mc2-opengl/mc2-win64-v0.3/mc2.exe")
OUT_DIR = Path(r"A:/Games/mc2-main/tests/smoke/artifacts/diag-shots")
OUT_DIR.mkdir(parents=True, exist_ok=True)

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
    "MC2_SMOKE_MODE": "1",       # required for mc2.exe to accept --mission
    "MC2_SMOKE_SEED": "0",
})
# Optional: set MC2_GPU_CULL_SUBSTRATE=1 via QUICK_SHOT_SUBSTRATE=1 in caller env.
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

print(f"[shot] launching mc2.exe mission={MISSION} duration={WAIT_SEC + 10}s label={LABEL}")
proc = nofocus.spawn(
    [str(EXE), "--profile", "stock", "--mission", MISSION, "--duration", str(WAIT_SEC + 10)],
    stdout=log_fp, stderr=subprocess.STDOUT, env=env,
    cwd=str(EXE.parent),
)

print(f"[shot] sleeping {WAIT_SEC}s for mission load + camera settle...")
time.sleep(WAIT_SEC)

# Locate the mc2 window. BACKGROUND-SMOKE-1: we do NOT raise it -- pyautogui
# grabs the desktop composite, so a non-activated window still captures fine.
try:
    import ctypes
    user32 = ctypes.windll.user32
    hwnd = user32.FindWindowW(None, "mc2") or \
           user32.FindWindowW(None, "MechCommander 2") or \
           user32.FindWindowW(None, "mc2 - Remastered")
    if hwnd:
        if nofocus.set_foreground_window(hwnd):
            time.sleep(0.5)
            print(f"[shot] mc2 window foregrounded hwnd={hwnd}")
        else:
            print(f"[shot] mc2 window found hwnd={hwnd} (not raised; background mode)")
    else:
        print("[shot] WARN: mc2 window not found by title")
except Exception as e:
    print(f"[shot] WARN foreground: {e}")

shot_path = OUT_DIR / f"{MISSION}-{LABEL}.png"
try:
    import pyautogui
    pyautogui.FAILSAFE = False
    img = pyautogui.screenshot()
    img.save(str(shot_path))
    print(f"[shot] saved {shot_path}")
except Exception as e:
    print(f"[shot] ERROR pyautogui: {e}")

# Tear down
time.sleep(1)
kill()
print(f"[shot] log saved {log_path}")
print(f"[shot] DONE")
