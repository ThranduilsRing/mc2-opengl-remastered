#!/usr/bin/env python3
"""Camera-sweep smoke for cull-cascade motion validation.

Static-camera smoke under-samples the 768u/384u-dilated off-rect band where
a slim-vs-legacy cull divergence would live (Step-8 review CRIT-2). This
launches a mission and drives a continuous arrow-key box-pan so a wide,
continuously-refreshed terrain vertex population sweeps that band, making the
MC2_VPL_CULL superset assertion a broad-population tripwire rather than a
narrow fixed-view one.

Env (set by caller, e.g. MC2_VPL_CULL=1) is inherited. Adds the required
MC2_SMOKE_MODE/SEED/HEARTBEAT. Parks the cursor center first (kills random
edge-scroll), then drives DELIBERATE arrow-key pans (the controlled motion).

Pass: smoke_summary=pass, no crash, AND every [VPL_CULL v1] event=summary
line has superset_violations=0 (grep + assert by the caller).

Usage:
  py -3 scripts/camera_sweep_smoke.py --mission mc2_01 --duration 40 --screenshot-at 34
"""
from __future__ import annotations
import argparse, atexit, ctypes, datetime as dt, os, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
# CURSOR-STEAL-HARNESS-GUARD-1: shared cursor policy (conditional park + restore).
from smoke_lib.cursor_guard import park_cursor_if_edge, restore_parked_cursor  # noqa: E402
# BACKGROUND-SMOKE-1: single chokepoint for non-activating launch + guarded focus.
from smoke_lib import nofocus  # noqa: E402
DEFAULT_EXE = Path(r"A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe")
ARTIFACT_ROOT = ROOT / "tests" / "smoke" / "artifacts"

# Win32 virtual key codes (match scripts/game_auto.py KEY_NAME_TO_VK).
VK = {"left": 0x25, "up": 0x26, "right": 0x27, "down": 0x28}
KEYEVENTF_KEYUP = 0x0002
_user32 = ctypes.windll.user32


def _kill_mc2():
    subprocess.run(["taskkill", "/F", "/IM", "mc2.exe"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1)


def _park_cursor_center():
    """Keep the cursor out of MC2's edge-scroll bands.

    CURSOR-STEAL-HARNESS-GUARD-1: this previously slammed the pointer to screen
    centre unconditionally, three times per run, and NEVER restored it -- the
    user's mouse was simply left at centre when the sweep finished. It now moves
    the pointer only when it actually sits in a scroll band (see cursor_guard.py
    for the missiongui.cpp:5206 trigger), and the pre-park position is restored
    at exit via the atexit hook registered below.
    """
    try:
        outcome = park_cursor_if_edge()
        if outcome == "parked":
            print("[camsweep] cursor parked (was in edge-scroll band)", file=sys.stderr)
    except Exception as e:
        print(f"[camsweep] WARN park cursor: {e}", file=sys.stderr)


# Restore the user's pointer even if the sweep crashes or is killed.
atexit.register(restore_parked_cursor)


def _foreground_mc2():
    """Locate the mc2 window; raise it ONLY if a user-present session opted in.

    BACKGROUND-SMOKE-1: screenshots do not need the window foregrounded --
    pyautogui/mss grab the desktop composite and a non-activated window still
    renders. The find-window walk is kept because callers may want the hwnd.
    """
    for title in ("mc2", "MechCommander 2", "mc2 - Remastered"):
        hwnd = _user32.FindWindowW(None, title)
        if hwnd:
            if nofocus.set_foreground_window(hwnd):
                time.sleep(0.3)
            return True
    return False


def _hold_key(vk: int, hold_s: float):
    """Press a vk, hold hold_s, release. keybd_event matches game_auto.py."""
    _user32.keybd_event(vk, 0, 0, 0)
    time.sleep(hold_s)
    _user32.keybd_event(vk, 0, KEYEVENTF_KEYUP, 0)
    time.sleep(0.05)


def _box_sweep(total_s: float):
    """Continuous L/D/R/U arrow pans for ~total_s. Each leg ~2s; the camera
    sweeps a large terrain area so fresh vertices continuously cross the
    screen-edge dilation band the probe must exercise."""
    legs = ["left", "down", "right", "up"]
    t_end = time.time() + total_s
    i = 0
    while time.time() < t_end:
        _hold_key(VK[legs[i % 4]], 2.0)
        i += 1


def run(exe: Path, mission: str, duration: int, screenshot_at: int,
        profile: str, artifact_dir: Path) -> dict:
    log_path = artifact_dir / f"{mission}.log"
    shot_path = artifact_dir / f"{mission}.png"
    _kill_mc2()
    _park_cursor_center()
    env = os.environ.copy()           # inherits MC2_VPL_CULL=1 etc. from caller
    env["MC2_SMOKE_MODE"] = "1"
    env["MC2_SMOKE_SEED"] = "0xC0FFEE"
    env["MC2_HEARTBEAT"] = "1"
    log_fp = open(log_path, "w", encoding="utf-8", errors="replace")
    args = [str(exe), "--profile", profile, "--mission", mission,
            "--duration", str(duration)]
    print(f"[camsweep] launch {mission}: {' '.join(args)}")
    t0 = time.time()
    # NOFOCUS-FINISH-1: this harness grades the DESKTOP COMPOSITE (pyautogui
    # below), which sees nothing of a window parked at (-32000,-32000) -- so it
    # must declare offscreen=False. nofocus then REFUSES the launch unless
    # MC2_HARNESS_ALLOW_FOREGROUND=1, rather than letting the sweep save the
    # user's wallpaper. Listed in check-background-launch.COMPOSITE_CAPTURE_FILES.
    proc = nofocus.spawn(args, stdout=log_fp, stderr=subprocess.STDOUT,
                         cwd=str(exe.parent), env=env, offscreen=False)
    # Mission load + stabilize, then deliberate camera motion.
    time.sleep(9)
    _foreground_mc2()
    _park_cursor_center()
    sweep_window = max(4, screenshot_at - 11)   # sweep until just before the shot
    print(f"[camsweep] box-sweeping camera for ~{sweep_window}s")
    _box_sweep(sweep_window)
    _foreground_mc2()
    _park_cursor_center()
    time.sleep(0.3)
    try:
        import pyautogui
        pyautogui.FAILSAFE = False
        pyautogui.screenshot().save(str(shot_path))
        shot_ok = True
    except Exception as e:
        print(f"[camsweep] WARN screenshot: {e}", file=sys.stderr)
        shot_ok = False
    remaining = max(1, (duration + 10) - (time.time() - t0))
    try:
        proc.wait(timeout=remaining)
    except subprocess.TimeoutExpired:
        proc.kill()
        try:
            proc.wait(timeout=10)
        except Exception:
            pass
    log_fp.close()
    txt = log_path.read_text(encoding="utf-8", errors="replace").lower()
    smoke_pass = "[smoke v1] event=summary result=pass" in txt
    crashy = [t for t in ("unhandled exception", "access violation",
                          "fatal error", " abort", "assert")
              if t in txt]
    # superset_violations: every summary line must be =0.
    import re
    sv = re.findall(r"superset_violations=(\d+)", txt)
    sv_bad = [v for v in sv if v != "0"]
    result = {
        "mission": mission, "exit_code": proc.returncode,
        "smoke_pass": smoke_pass, "crashy": crashy,
        "shot_ok": shot_ok,
        "superset_samples": len(sv),
        "superset_violations_nonzero": sv_bad,
        "log": str(log_path), "shot": str(shot_path),
    }
    ok = (smoke_pass and not crashy and proc.returncode == 0
          and len(sv) > 0 and not sv_bad)
    result["pass"] = ok
    return result


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--mission", required=True)
    ap.add_argument("--duration", type=int, default=40)
    ap.add_argument("--screenshot-at", type=int, default=34)
    ap.add_argument("--profile", default="stock")
    ap.add_argument("--exe", default=str(DEFAULT_EXE))
    ap.add_argument("--label", default="camsweep")
    a = ap.parse_args()
    exe = Path(a.exe)
    if not exe.exists():
        print(f"[camsweep] ERROR exe missing {exe}", file=sys.stderr)
        return 4
    ts = dt.datetime.now().strftime("%Y-%m-%dT%H-%M-%S")
    d = ARTIFACT_ROOT / f"{a.label}-{ts}"
    d.mkdir(parents=True, exist_ok=True)
    print(f"[camsweep] artifacts -> {d}")
    r = run(exe, a.mission, a.duration, a.screenshot_at, a.profile, d)
    verdict = "PASS" if r["pass"] else "FAIL"
    print(f"\n[camsweep] {a.mission}: {verdict} "
          f"smoke_pass={r['smoke_pass']} crash={bool(r['crashy'])} "
          f"superset_samples={r['superset_samples']} "
          f"superset_violations_nonzero={r['superset_violations_nonzero']} "
          f"shot={'ok' if r['shot_ok'] else 'FAIL'}")
    (d / "summary.txt").write_text(str(r), encoding="utf-8")
    return 0 if r["pass"] else 1


if __name__ == "__main__":
    sys.exit(main())
