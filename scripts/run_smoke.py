#!/usr/bin/env python3
# scripts/run_smoke.py
"""MC2 smoke matrix runner.

Examples:
  python scripts/run_smoke.py --tier tier1 --fail-fast
  python scripts/run_smoke.py --tier tier2
  python scripts/run_smoke.py --tier tier3 --kill-existing
  python scripts/run_smoke.py --mission mc2_01 --mission mc2_03
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from scripts.smoke_lib import baselines, manifest, report
from scripts.smoke_lib.runner import RunConfig, run_one

DEFAULT_EXE = Path(r"A:/Games/mc2-opengl/mc2-win64-v0.1.1/mc2.exe")
ARTIFACT_ROOT = ROOT / "tests" / "smoke" / "artifacts"
MANIFEST_PATH = ROOT / "tests" / "smoke" / "smoke_missions.txt"
BASELINE_PATH = ROOT / "tests" / "smoke" / "baselines.json"


def _running_mc2() -> list[int]:
    try:
        out = subprocess.check_output(
            ["tasklist", "/FI", "IMAGENAME eq mc2.exe", "/NH", "/FO", "CSV"],
            text=True, stderr=subprocess.DEVNULL)
    except Exception:
        return []
    pids = []
    for line in out.splitlines():
        if "mc2.exe" in line:
            parts = [p.strip('"') for p in line.split(",")]
            if len(parts) > 1 and parts[1].isdigit():
                pids.append(int(parts[1]))
    return pids


def _taskkill_mc2():
    subprocess.run(["taskkill", "/F", "/IM", "mc2.exe"],
                   stderr=subprocess.DEVNULL, stdout=subprocess.DEVNULL)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tier", choices=["tier1", "tier2", "tier3"])
    ap.add_argument("--mission", action="append", default=[])
    ap.add_argument("--fail-fast", action="store_true")
    ap.add_argument("--continue", dest="cont", action="store_true", default=True)
    ap.add_argument("--keep-logs", action="store_true")
    ap.add_argument("--baseline-update", action="store_true")
    ap.add_argument("--kill-existing", action="store_true")
    ap.add_argument("--duration", type=int)
    ap.add_argument("--profile", default="stock")
    ap.add_argument("--exe", default=str(DEFAULT_EXE))
    args = ap.parse_args()

    # Existing-process safety.
    pids = _running_mc2()
    if pids:
        if args.kill_existing:
            print(f"[runner] killing existing mc2.exe PIDs {pids}", file=sys.stderr)
            _taskkill_mc2()
        else:
            print(f"[runner] ERROR: mc2.exe already running (PIDs {pids}); "
                  f"pass --kill-existing to override.", file=sys.stderr)
            sys.exit(4)

    entries = manifest.parse_manifest(MANIFEST_PATH)
    if args.mission:
        selected = [e for e in entries if e.stem in args.mission and e.tier != "skip"]
    elif args.tier:
        selected = [e for e in entries if e.tier == args.tier]
    else:
        ap.error("--tier or --mission required")

    if not selected:
        print("[runner] no missions selected", file=sys.stderr)
        sys.exit(0)

    baseline_data = baselines.load(BASELINE_PATH)
    timestamp = dt.datetime.now().strftime("%Y-%m-%dT%H-%M-%S")
    artifact_dir = ARTIFACT_ROOT / timestamp
    artifact_dir.mkdir(parents=True, exist_ok=True)

    rows: list[report.Row] = []
    for e in selected:
        duration = args.duration or e.duration or 120
        tier = args.tier or "adhoc"
        cfg = RunConfig(
            exe=[args.exe],
            profile=e.profile or args.profile,
            stem=e.stem,
            duration=duration,
            heartbeat_timeout_load_s=e.heartbeat_timeout_load or 60,
            heartbeat_timeout_play_s=e.heartbeat_timeout_play or 3,
            grace_s=60,
            env_extra={"MC2_SMOKE_SEED": "0xC0FFEE"},
        )
        print(f"[runner] running {e.stem} (tier={tier} duration={duration})",
              file=sys.stderr)
        result = run_one(cfg)

        key = baselines.key(cfg.profile, e.stem, tier, duration)
        delta = baselines.destroys_delta(baseline_data, key, result.summary.destroys)

        rows.append(report.Row(stem=e.stem, verdict=result.verdict,
                               summary=result.summary, destroys_delta=delta or 0))

        if not result.verdict.passed or args.keep_logs:
            (artifact_dir / f"{e.stem}.log").write_text(result.stdout_text)
        if args.baseline_update and result.verdict.passed:
            baseline_data.setdefault(key, {})["destroys"] = {
                "mean": result.summary.destroys, "stddev": 0, "samples": 1,
                "updated": timestamp,
            }
            baseline_data[key]["perf"] = {
                "avg_fps": result.summary.perf.avg_fps,
                "p1low_fps": result.summary.perf.p1low_fps,
                "peak_ms": result.summary.perf.peak_ms,
            }

        if args.fail_fast and not result.verdict.passed:
            print(f"[runner] --fail-fast: stopping at {e.stem}", file=sys.stderr)
            break

        # 2s grace for PDB lock release before next spawn.
        import time as _t; _t.sleep(2)

    md = report.render_markdown(rows, tier=args.tier or "adhoc",
                                 profile=args.profile, timestamp=timestamp)
    (artifact_dir / "report.md").write_text(md)
    (artifact_dir / "report.json").write_text(
        json.dumps(report.render_json(rows, tier=args.tier or "adhoc",
                                      profile=args.profile, timestamp=timestamp),
                   indent=2))

    if args.baseline_update:
        baselines.save(BASELINE_PATH, baseline_data)

    print(md)
    passed = all(r.verdict.passed for r in rows)
    sys.exit(0 if passed else 1)


if __name__ == "__main__":
    main()
