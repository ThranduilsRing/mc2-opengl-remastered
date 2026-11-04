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


EARLY_EXIT = re.compile(r"\[SMOKE v1\] event=summary result=fail reason=early_exit stage=atexit")
RESULTS_DONE =re.compile(r"\[RESULTS v1\] resultsDone complete ")


GRAVEYARD = re.compile(r"\[GRAVEYARD v1\] drains=(\d+) commands=(\d+)")
MIN_DRAINS = 20


def judge(text: str, require_results: bool = False, min_drains: int = 0) -> tuple[bool, str]:
    """(passed, reason) for one mission's concatenated runner output + kept game log."""
    m = VIOLATION.search(text)
    if m:
        return False, "sim-overlap violation: " + text[m.start():m.start() + 160].splitlines()[0]
    if not OPENED.search(text):
        return False, "no '[VIDEO] init: opened' line: the comm video never decoded"
    if require_results and not RESULTS_DONE.search(text):
        return False, "no '[RESULTS v1] resultsDone complete' line: the results path was never reached"
    drains = max((int(g.group(1)) for g in GRAVEYARD.finditer(text)), default=0)
    if min_drains and drains < min_drains:
        return False, f"only {drains} graveyard drain(s) with work under overlap (need {min_drains}): the R1 drain-after-join proof did not run"
    return True, ("video opened, no sim-overlap violation" + (", results path reached" if require_results else "")
                  + (f", {drains} graveyard drains with work" if min_drains else ""))


def run_mission(exe: str, mission: str, duration: int, countdown: bool = False, log_to: Path | None = None, support: bool = False) -> tuple[bool, str]:
    env = dict(os.environ, MC2_BOOT_VIDEO="1", MC2_SMOKE_REQUIRE_FINGERPRINT=os.environ.get("MC2_SMOKE_REQUIRE_FINGERPRINT", "1"))
    env.pop("MC2_SIM_OVERLAP", None)   # default ON is the point
    if countdown:   # mission-end row: normal results countdown, so resultsDone runs on the sim worker with a live pilot video
        env.update(MC2_SOAK_AUTOWIN="1", MC2_SOAK_AUTOWIN_COUNTDOWN="1", MC2_SOAK_WIN_AFTER_SEC="12", MC2_DIAG_TAGS="RESULTS", MC2_SIM_OVERLAP_AUDIT="2")
    if support:   # graveyard stress row (R1): periodic airstrikes fill the graveyard while the worker overlaps; AUDIT=2 flags a drain before the join
        env.update(MC2_SOAK_SUPPORT_EVERY="0.1", MC2_DIAG_TAGS="GRAVEYARD", MC2_SIM_OVERLAP_AUDIT="2")
    cmd = [sys.executable, str(ROOT / "scripts" / "run_smoke.py"), "--mission", mission, "--duration", str(duration),
           "--keep-logs", "--exe", exe]
    if support:   # the 30 s cap leaves ~16 s in the mission; strikes live ~15 s and the pool is 130, so run the sim 4x to recycle slots
        cmd += ["--sim-speed", "4"]
    proc = subprocess.run(cmd, cwd=ROOT, env=env, capture_output=True, text=True, errors="replace",
                          **console_child.child_kwargs(redirected=True))
    text = proc.stdout + "\n" + proc.stderr
    m = re.search(r"\[runner\] artifacts: (.+)", text)
    if m:
        for log in Path(m.group(1).strip()).glob("*.log"):
            text += "\n" + log.read_text(encoding="utf-8", errors="replace")
    if log_to:
        log_to.parent.mkdir(parents=True, exist_ok=True)
        log_to.write_text(text, encoding="utf-8")
    ok, why = judge(text, require_results=countdown, min_drains=MIN_DRAINS if support else 0)
    # The end row ends the mission on purpose: run_smoke reports that as early_exit (rc=1), which is the pass shape.
    if proc.returncode != 0 and countdown and ok and EARLY_EXIT.search(text):
        return True, why + " (mission ended by the countdown: early_exit)"
    if proc.returncode != 0:
        ok, why = False, f"run_smoke rc={proc.returncode}; {why}"
    return ok, why


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--mission", action="append", default=[])
    ap.add_argument("--duration", type=int, default=30)
    ap.add_argument("--end-duration", type=int, default=30)
    ap.add_argument("--out", help="directory for the per-mission full logs (merge-window proofs)")
    args = ap.parse_args()
    bad = 0
    for mission in args.mission or MISSIONS:
        ok, why = run_mission(args.exe, mission, args.duration, log_to=Path(args.out) / (mission + ".log") if args.out else None)
        print(f"[video_decode_smoke] {mission}: {'PASS' if ok else 'FAIL'} - {why}")
        bad += not ok
    # mission-end row (P0-1): results countdown with pilot videos live
    ok, why = run_mission(args.exe, "mc2_01", args.end_duration, countdown=True,
                    log_to=Path(args.out) / "mc2_01_end.log" if args.out else None)
    print(f"[video_decode_smoke] mc2_01 end: {'PASS' if ok else 'FAIL'} - {why}")
    bad += not ok
    # graveyard stress row (R1): at least MIN_DRAINS non-empty drains during overlap
    ok, why = run_mission(args.exe, "mc2_01", args.duration, support=True,
                    log_to=Path(args.out) / "mc2_01_graveyard.log" if args.out else None)
    print(f"[video_decode_smoke] mc2_01 graveyard: {'PASS' if ok else 'FAIL'} - {why}")
    bad += not ok
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
