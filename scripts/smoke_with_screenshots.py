#!/usr/bin/env python3
"""Smoke + screenshot wrapper for autonomous Wave 2 gate runs.

Runs mc2.exe per mission, parks OS cursor at screen center (defeats edge-scroll
that would otherwise scroll camera to random positions), waits N seconds for
mission load + stabilize, brings the mc2 window to foreground, screenshots,
then lets the game exit on its own --duration flag (or kills at duration+10).

Pass/fail signal: clean-exit marker "[exit] gos_terminateapplication called"
in stdout AND no crash signatures (unhandled exception, access violation,
fatal error, abort, assert).

Output:
  tests/smoke/artifacts/screenshots-<timestamp>/
    <mission>.log       full stdout+stderr
    <mission>.png       screenshot at <screenshot_at_sec>
    <mission>.result    PASS or FAIL with diagnostic lines
    summary.md          aggregate verdict per mission

Examples:
  py -3 scripts/smoke_with_screenshots.py --mission mc2_01 --mission mc2_24
  py -3 scripts/smoke_with_screenshots.py --mission mc2_01 --duration 30 --screenshot-at 28
"""
from __future__ import annotations

import argparse
import datetime as dt
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))
# BACKGROUND-SMOKE-1: single chokepoint for non-activating launch + guarded focus.
from smoke_lib import nofocus  # noqa: E402

DEFAULT_EXE = Path(r"A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe")
ARTIFACT_ROOT = ROOT / "tests" / "smoke" / "artifacts"


def park_mouse_center() -> tuple[int, int] | None:
    """No-op. Retained as a call site only.

    BACKGROUND-SMOKE-1: this used to slam the pointer to screen centre --
    unconditionally, several times per mission, with no restore -- which yanked
    the mouse out from under the user while an agent lane ran. Edge-scroll is
    now suppressed engine-side instead: nofocus.spawn() sets
    MC2_NO_CURSOR_GRAB=1 (and MC2_SMOKE_MODE is set below), which makes
    gos_CursorControlSuppressed() true, so the engine skips ClipCursor() and
    ignores the OS pointer. Moving the user's cursor is never required.
    """
    return None


def bring_mc2_foreground() -> bool:
    """Locate the mc2 window; raise it only in a user-present session.

    BACKGROUND-SMOKE-1: take_screenshot() below uses pyautogui, which grabs the
    desktop composite; a non-activated window still renders into it, so the
    foreground raise was never needed for capture. nofocus.set_foreground_window
    is a no-op unless MC2_HARNESS_ALLOW_FOREGROUND=1.
    """
    try:
        import ctypes
        user32 = ctypes.windll.user32
        for title in ("mc2", "MechCommander 2", "mc2 - Remastered"):
            hwnd = user32.FindWindowW(None, title)
            if hwnd:
                if nofocus.set_foreground_window(hwnd):
                    time.sleep(0.3)
                return True
    except Exception as e:
        print(f"[smoke] WARN: foreground bring failed: {e}", file=sys.stderr)
    return False


def take_screenshot(path: Path) -> bool:
    try:
        import pyautogui
        pyautogui.FAILSAFE = False
        img = pyautogui.screenshot()
        img.save(str(path))
        return True
    except Exception as e:
        print(f"[smoke] WARN: screenshot failed: {e}", file=sys.stderr)
        return False


CRASH_TOKENS = (
    "unhandled exception",
    "fatal error",
    "access violation",
    "stack overflow",
    " abort",
    "assert",
)


def grade_log(log_text: str) -> tuple[bool, list[str]]:
    """Use the project's canonical smoke-result marker.

    The game emits `[SMOKE v1] event=summary result=<pass|fail> ...` near the
    end of a passive smoke run. This is the authoritative pass/fail signal,
    in lockstep with the exit code returned to run_smoke.py's matrix. A
    crash-signature scan is the secondary belt-and-suspenders check.
    """
    lowered = log_text.lower()
    smoke_pass = "[smoke v1] event=summary result=pass" in lowered
    smoke_fail = "[smoke v1] event=summary result=fail" in lowered
    crashy = [t.strip() for t in CRASH_TOKENS if t in lowered]
    diags: list[str] = []
    if smoke_pass:
        diags.append("smoke_summary: pass")
    elif smoke_fail:
        diags.append("smoke_summary: FAIL")
    else:
        diags.append("smoke_summary: MISSING")
    if crashy:
        diags.append(f"crash_signatures: {crashy}")
    else:
        diags.append("crash_signatures: none")
    # Try to surface fps/frames if PERF v1 line is present.
    import re
    m = re.search(r"\[PERF v1\] avg_fps=([0-9.]+) p50_ms=([0-9.]+) p99_ms=([0-9.]+)", log_text)
    if m:
        diags.append(f"perf: avg_fps={m.group(1)} p50_ms={m.group(2)} p99_ms={m.group(3)}")
    return smoke_pass and not crashy, diags


def run_mission(exe: Path, mission: str, duration: int, screenshot_at: int,
                profile: str, artifact_dir: Path) -> dict:
    log_path = artifact_dir / f"{mission}.log"
    shot_path = artifact_dir / f"{mission}.png"
    result_path = artifact_dir / f"{mission}.result"

    park_mouse_center()

    log_fp = open(log_path, "w", encoding="utf-8", errors="replace")
    # game's own --duration gives it a clean self-exit window.
    # MC2_SMOKE_MODE=1 is REQUIRED by mc2.exe to honor --mission/--duration args
    # (the argv parser rejects --mission without it). Add MC2_SMOKE_SEED for
    # determinism + MC2_HEARTBEAT for stderr liveness during long runs.
    env = os.environ.copy()
    env["MC2_SMOKE_MODE"] = "1"
    env["MC2_SMOKE_SEED"] = "0xC0FFEE"
    env["MC2_HEARTBEAT"] = "1"
    proc_args = [str(exe), "--profile", profile, "--mission", mission,
                 "--duration", str(duration)]
    print(f"[smoke] launching {mission}: {' '.join(proc_args)}")
    t0 = time.time()
    # NOFOCUS-FINISH-1: take_screenshot() above is pyautogui (DESKTOP
    # COMPOSITE), which sees nothing of a window parked at (-32000,-32000) --
    # so declare offscreen=False. nofocus then REFUSES the launch unless
    # MC2_HARNESS_ALLOW_FOREGROUND=1 instead of attaching wallpaper frames to a
    # smoke report. Listed in check-background-launch.COMPOSITE_CAPTURE_FILES.
    proc = nofocus.spawn(proc_args, stdout=log_fp, stderr=subprocess.STDOUT,
                         cwd=str(exe.parent), env=env, offscreen=False)

    # Wait until screenshot point, with a cursor re-park midway to be safe.
    time.sleep(max(0, screenshot_at - 5))
    park_mouse_center()
    time.sleep(min(5, screenshot_at))

    fg = bring_mc2_foreground()
    park_mouse_center()  # foreground-bring might have moved the cursor
    time.sleep(0.3)
    shot_ok = take_screenshot(shot_path)

    # Give the game until duration+10 to exit cleanly.
    remaining = max(1, (duration + 10) - (time.time() - t0))
    try:
        proc.wait(timeout=remaining)
    except subprocess.TimeoutExpired:
        print(f"[smoke] {mission} did not exit in {duration}+10s; killing", file=sys.stderr)
        proc.kill()
        try:
            proc.wait(timeout=10)
        except Exception:
            pass
    log_fp.close()

    log_text = log_path.read_text(encoding="utf-8", errors="replace")
    passed, diags = grade_log(log_text)
    result = {
        "mission": mission,
        "exit_code": proc.returncode,
        "passed": passed,
        "diags": diags,
        "screenshot_ok": shot_ok,
        "foreground": fg,
        "log_path": str(log_path),
        "shot_path": str(shot_path) if shot_ok else "",
        "duration_actual": time.time() - t0,
    }
    body = (
        f"mission: {mission}\n"
        f"result: {'PASS' if passed else 'FAIL'}\n"
        f"exit_code: {proc.returncode}\n"
        f"screenshot: {'ok' if shot_ok else 'FAILED'}\n"
        f"foreground_bring: {'ok' if fg else 'FAILED'}\n"
        f"duration_actual_s: {result['duration_actual']:.1f}\n"
        f"diagnostics:\n"
        + "\n".join(f"  - {d}" for d in diags) + "\n"
    )
    result_path.write_text(body, encoding="utf-8")
    print(f"[smoke] {mission}: {'PASS' if passed else 'FAIL'}")
    return result


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--mission", action="append", default=[], required=True,
                    help="Mission stem (e.g. mc2_01). Repeat for multiple.")
    ap.add_argument("--duration", type=int, default=30,
                    help="Per-mission --duration arg to mc2.exe (default 30)")
    ap.add_argument("--screenshot-at", type=int, default=28,
                    help="Seconds after launch to take screenshot (default 28)")
    ap.add_argument("--profile", default="stock")
    ap.add_argument("--exe", default=str(DEFAULT_EXE))
    ap.add_argument("--label", default="smoke",
                    help="Artifact dir label (default 'smoke')")
    args = ap.parse_args()

    exe = Path(args.exe)
    if not exe.exists():
        print(f"[smoke] ERROR: exe not found at {exe}", file=sys.stderr)
        return 4

    timestamp = dt.datetime.now().strftime("%Y-%m-%dT%H-%M-%S")
    artifact_dir = ARTIFACT_ROOT / f"{args.label}-{timestamp}"
    artifact_dir.mkdir(parents=True, exist_ok=True)
    print(f"[smoke] artifacts -> {artifact_dir}")

    results: list[dict] = []
    for mission in args.mission:
        r = run_mission(exe, mission, args.duration, args.screenshot_at,
                        args.profile, artifact_dir)
        results.append(r)
        # serial-only smoke (per memory rule feedback_smoke_serial_only.md):
        # we never overlap runs; wait a beat before the next launch.
        time.sleep(2)

    summary_lines = ["# Smoke + screenshot summary", "",
                     f"- artifact dir: `{artifact_dir}`",
                     f"- exe: `{exe}`",
                     f"- duration: {args.duration}s, screenshot at: {args.screenshot_at}s",
                     f"- profile: {args.profile}",
                     "",
                     "| mission | result | exit | shot | diags |",
                     "|---|---|---|---|---|"]
    for r in results:
        diags = "; ".join(r["diags"])
        summary_lines.append(
            f"| {r['mission']} | {'PASS' if r['passed'] else 'FAIL'} | "
            f"{r['exit_code']} | {'ok' if r['screenshot_ok'] else 'FAIL'} | {diags} |"
        )
    summary_text = "\n".join(summary_lines) + "\n"
    (artifact_dir / "summary.md").write_text(summary_text, encoding="utf-8")
    sys.stdout.write("\n" + summary_text)
    sys.stdout.flush()

    any_fail = any(not r["passed"] for r in results)
    return 1 if any_fail else 0


if __name__ == "__main__":
    sys.exit(main())
