#!/usr/bin/env python3
"""video_decode_smoke: real comm-video decode with the sim overlap worker ON (regression for the lane9 hotfix).

Smoke mode skips the movie decode (code/mc2movie.cpp), so a movie opened from the ABL VM on the sim worker
(gos_NewEmptyTexture is Render-owner only -> std::abort) was never exercised. This runs mc2_01 (V1A) and mc2_02 (V3G)
with MC2_BOOT_VIDEO=1 (decode on, passthrough in Gates/gate_rows_0.inc) and MC2_SIM_OVERLAP at its default (ON), and
fails on a [SIM_OVERLAP v1] VIOLATION, or when no "[VIDEO] init: opened" line shows the movie really played.

  py -3 scripts/video_decode_smoke.py --exe G:/mc2-installs/<lane>/mc2.exe [--mission mc2_01 ...]
"""
import argparse
import json
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


SUMMARY_FAIL = re.compile(r"\[SMOKE v1\] event=summary result=fail reason=(\S+)")
FAIL_ROW = re.compile(r"^\|\s*(\S+)\s*\|\s*FAIL\s*\|\s*([^|]*)\|", re.M)
HARD_MARKERS = ("HARD FAIL", "RUN INVALID", "[DEPLOY_FINGERPRINT] MISMATCH", "DEPLOY_FINGERPRINT:** FAIL", "Crash Report")


def early_exit_only(text: str) -> bool:
    """True only when the run's single failure is the countdown's deliberate early_exit. A nonzero exit that also carries a
    fingerprint mismatch, shader error, crash, invalid run, another summary reason or another verdict bucket is a real failure."""
    reasons = {m.group(1) for m in SUMMARY_FAIL.finditer(text)}
    if reasons != {"early_exit"} or any(h in text for h in HARD_MARKERS) or re.search(r"shader_error|SHADER_ERROR|shader compile", text):
        return False
    return all({b.strip().split(":")[0] for b in buckets.split(",") if b.strip()} <= {"engine_reported_fail"}
               for _stem, buckets in FAIL_ROW.findall(text))


def countdown_exit_ok(rc: int, text: str, report) -> bool:
    """The mission-end row's accepted failure, judged on run_smoke's structured verdict, not only on log text (p31: all() over
    zero parsed FAIL rows was true, any nonzero rc qualified, and truncated output kept the markers). Exactly rc 1, the text
    carries only the early_exit summary, and a complete report.json has no run failure, no fingerprint failure, and at least
    one row, every row FAIL with the single bucket engine_reported_fail."""
    if rc != 1 or not EARLY_EXIT.search(text) or not early_exit_only(text) or not isinstance(report, dict):
        return False
    if report.get("run_failures") or (report.get("fingerprint") or {}).get("hard_fail"):
        return False
    rows = report.get("rows")
    return bool(rows) and isinstance(rows, list) and all(
        isinstance(r, dict) and r.get("result") == "FAIL" and r.get("buckets") == ["engine_reported_fail"] for r in rows)


GRAVEYARD = re.compile(r"\[GRAVEYARD v1\] retire_calls=(\d+) nonempty=(\d+)")
MIN_RETIRES = 200


def judge(text: str, require_results: bool = False, min_retires: int = 0) -> tuple[bool, str]:
    """(passed, reason) for one mission's concatenated runner output + kept game log."""
    m = VIOLATION.search(text)
    if m:
        return False, "sim-overlap violation: " + text[m.start():m.start() + 160].splitlines()[0]
    if not OPENED.search(text):
        return False, "no '[VIDEO] init: opened' line: the comm video never decoded"
    if require_results and not RESULTS_DONE.search(text):
        return False, "no '[RESULTS v1] resultsDone complete' line: the results path was never reached"
    retires = max((int(g.group(1)) for g in GRAVEYARD.finditer(text)), default=0)
    if min_retires and retires < min_retires:
        return False, f"only {retires} graveyard drain call(s) audited (need {min_retires}): the R1 ordering proof did not run"
    return True, ("video opened, no sim-overlap violation" + (", results path reached" if require_results else "")
                  + (f", {retires} graveyard drain calls audited" if min_retires else ""))


def run_mission(exe: str, mission: str, duration: int, countdown: bool = False, log_to: Path | None = None, ordering: bool = False) -> tuple[bool, str]:
    env = dict(os.environ, MC2_BOOT_VIDEO="1", MC2_SMOKE_REQUIRE_FINGERPRINT=os.environ.get("MC2_SMOKE_REQUIRE_FINGERPRINT", "1"))
    env.pop("MC2_SIM_OVERLAP", None)   # default ON is the point
    if countdown:   # mission-end row: normal results countdown, so resultsDone runs on the sim worker with a live pilot video
        env.update(MC2_SOAK_AUTOWIN="1", MC2_SOAK_AUTOWIN_COUNTDOWN="1", MC2_SOAK_WIN_AFTER_SEC="12", MC2_DIAG_TAGS="RESULTS", MC2_SIM_OVERLAP_AUDIT="2")
    if ordering:   # graveyard ordering row (R1): AUDIT=2 flags a drain before the worker join; the counter shows the drain ran
        env.update(MC2_DIAG_TAGS="GRAVEYARD", MC2_SIM_OVERLAP_AUDIT="2")
    cmd = [sys.executable, str(ROOT / "scripts" / "run_smoke.py"), "--mission", mission, "--duration", str(duration),
           "--keep-logs", "--exe", exe]
    proc = subprocess.run(cmd, cwd=ROOT, env=env, capture_output=True, text=True, errors="replace",
                          **console_child.child_kwargs(redirected=True))
    text = proc.stdout + "\n" + proc.stderr
    m = re.search(r"\[runner\] artifacts: (.+)", text)
    report = None
    if m:
        for log in Path(m.group(1).strip()).glob("*.log"):
            text += "\n" + log.read_text(encoding="utf-8", errors="replace")
        try:
            report = json.loads((Path(m.group(1).strip()) / "report.json").read_text(encoding="utf-8"))
        except (OSError, ValueError):
            report = None   # no complete structured verdict: the countdown exception cannot apply
    if log_to:
        log_to.parent.mkdir(parents=True, exist_ok=True)
        log_to.write_text(text, encoding="utf-8")
    ok, why = judge(text, require_results=countdown, min_retires=MIN_RETIRES if ordering else 0)
    # The end row ends the mission on purpose: run_smoke reports that as early_exit (rc=1), which is the pass shape.
    if countdown and ok and countdown_exit_ok(proc.returncode, text, report):
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
    # graveyard stress row (R1): at least MIN_RETIRES audited drain calls, zero VIOLATION
    ok, why = run_mission(args.exe, "mc2_01", args.duration, ordering=True,
                    log_to=Path(args.out) / "mc2_01_graveyard_ordering.log" if args.out else None)
    print(f"[video_decode_smoke] mc2_01 graveyard ordering: {'PASS' if ok else 'FAIL'} - {why}")
    bad += not ok
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
