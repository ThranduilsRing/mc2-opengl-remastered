#!/usr/bin/env python3
"""video_decode_smoke: real comm-video decode with the sim overlap worker ON (regression for the lane9 hotfix).

Smoke mode skips the movie decode (code/mc2movie.cpp), so a movie opened from the ABL VM on the sim worker
(gos_NewEmptyTexture is Render-owner only -> std::abort) was never exercised. This runs mc2_01 (V1A) and mc2_02 (V3G)
with MC2_BOOT_VIDEO=1 (decode on, passthrough in Gates/gate_rows_0.inc) and MC2_SIM_OVERLAP at its default (ON), and
fails on a [SIM_OVERLAP v1] VIOLATION, or when no "[VIDEO] init: opened" line shows the movie really played.

  py -3 scripts/video_decode_smoke.py --exe E:/mc2-installs/<lane>/mc2.exe [--mission mc2_01 ...]
"""
import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import console_child  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
MISSIONS = ("mc2_01", "mc2_02")
VIOLATION = re.compile(r"\[SIM_OVERLAP v1\] VIOLATION")
OPENED = re.compile(r"\[VIDEO\] init: opened ")


def judge(text: str) -> tuple[bool, str]:
    """(passed, reason) for one mission's concatenated runner output + kept game log."""
    m = VIOLATION.search(text)
    if m:
        return False, "sim-overlap violation: " + text[m.start():m.start() + 160].splitlines()[0]
    if not OPENED.search(text):
        return False, "no '[VIDEO] init: opened' line: the comm video never decoded"
    return True, "video opened, no sim-overlap violation"


def run_mission(exe: str, mission: str, duration: int, countdown: bool = False) -> tuple[bool, str]:
    env = dict(os.environ, MC2_BOOT_VIDEO="1", MC2_SMOKE_REQUIRE_FINGERPRINT=os.environ.get("MC2_SMOKE_REQUIRE_FINGERPRINT", "1"))
    env.pop("MC2_SIM_OVERLAP", None)   # default ON is the point
    if countdown:   # mission-end row: normal results countdown, so resultsDone runs on the sim worker with a live pilot video
        env.update(MC2_SOAK_AUTOWIN="1", MC2_SOAK_AUTOWIN_COUNTDOWN="1", MC2_SOAK_WIN_AFTER_SEC="6")
    cmd = [sys.executable, str(ROOT / "scripts" / "run_smoke.py"), "--mission", mission, "--duration", str(duration),
           "--keep-logs", "--exe", exe]
    proc = subprocess.run(cmd, cwd=ROOT, env=env, capture_output=True, text=True, errors="replace",
                          **console_child.child_kwargs(redirected=True))
    text = proc.stdout + "\n" + proc.stderr
    m = re.search(r"\[runner\] artifacts: (.+)", text)
    if m:
        for log in Path(m.group(1).strip()).glob("*.log"):
            text += "\n" + log.read_text(encoding="utf-8", errors="replace")
    ok, why = judge(text)
    if proc.returncode != 0:
        ok, why = False, f"run_smoke rc={proc.returncode}; {why}"
    return ok, why


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--mission", action="append", default=[])
    ap.add_argument("--duration", type=int, default=30)
    ap.add_argument("--end-duration", type=int, default=90)
    args = ap.parse_args()
    bad = 0
    for mission in args.mission or MISSIONS:
        ok, why = run_mission(args.exe, mission, args.duration)
        print(f"[video_decode_smoke] {mission}: {'PASS' if ok else 'FAIL'} - {why}")
        bad += not ok
    # mission-end row (P0-1): results countdown with pilot videos live
    ok, why = run_mission(args.exe, "mc2_01", args.end_duration, countdown=True)
    print(f"[video_decode_smoke] mc2_01 end: {'PASS' if ok else 'FAIL'} - {why}")
    bad += not ok
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
