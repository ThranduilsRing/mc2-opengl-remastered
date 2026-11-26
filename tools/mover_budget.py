#!/usr/bin/env python3
"""mover_budget - M6 synthetic 200-mover sim tick budget (design target: tick <= 0.6 ms).

Boots a stock mission headless with MC2_MOVER_FIXTURE=N (code/mover_budget_fixture.h tops the mission up to N movers on two teams and
keeps move/attack orders flowing), runs --ticks sim ticks and reads the engine's [SIM_TICK_COST v1] rows (MC2_MISSION_SPLIT=1).
Prints one verdict line: mean of the per-run avg and p95 sim tick ms over --runs runs.

  py -3 tools/mover_budget.py --install G:/mc2-installs/claude-m5-m6-movers [--mission mc2_03] [--movers 200] [--ticks 900] [--runs 3]
"""
from __future__ import annotations

import argparse
import os
import re
import statistics
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
sys.path.insert(0, HERE)
import console_child  # noqa: E402
import sim_determinism as sd  # noqa: E402

COST = re.compile(r"\[SIM_TICK_COST v1\] ticks=(\d+) avg_ms=([\d.]+) p95_ms=([\d.]+) p99_ms=([\d.]+) max_ms=([\d.]+)")
FIX = re.compile(r"\[MOVER_FIXTURE v1\] result=(\w+)(?: requested=(\d+) base=(\d+) added=(\d+) movers=(\d+))?")


def one_run(args, run: int):
    env = dict(os.environ, MC2_SMOKE_FIXED_TIMESTEP="1", MC2_MISSION_SPLIT="1", MC2_MOVER_FIXTURE=str(args.movers))
    cmd = [sys.executable, os.path.join(ROOT, "scripts", "run_smoke.py"), "--mission", args.mission, "--duration", "60",
           *sd.smoke_target(args.install), "--keep-logs", "--sim-determinism", "--headless", "--ticks", str(args.ticks)]
    r = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace",
                       **console_child.child_kwargs(redirected=True))
    arts = [l.removeprefix("[runner] artifacts: ").strip() for l in r.stdout.splitlines() if l.startswith("[runner] artifacts: ")]
    text = r.stdout
    if arts:
        log = os.path.join(arts[-1], args.mission + ".log")
        if os.path.exists(log):
            with open(log, encoding="utf-8", errors="replace") as fh:
                text += "\n" + fh.read()
    if not sd.headless_exit_ok(r.returncode, text):
        sys.exit(f"mover_budget: run {run} did not reach its tick budget (rc={r.returncode}):\n" + "\n".join(r.stdout.splitlines()[-8:]))
    fix = FIX.findall(text)
    cost = COST.findall(text)
    if not fix or fix[-1][0] != "spawned" or not cost:
        sys.exit(f"mover_budget: run {run} fixture={fix[-1] if fix else None} cost_rows={len(cost)}")
    # Rows are per 180-tick window. Skip window 1 (load/spawn warm-up) when more than one exists; weight windows equally.
    rows = cost[1:] if len(cost) > 1 else cost
    movers = int(fix[-1][4])
    return statistics.mean(float(c[1]) for c in rows), max(float(c[2]) for c in rows), movers, len(rows)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--install", default=sd.INSTALL_DIR)
    ap.add_argument("--mission", default="mc2_03")
    ap.add_argument("--movers", type=int, default=200)
    ap.add_argument("--ticks", type=int, default=900)
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--target-ms", type=float, default=0.6)
    args = ap.parse_args()
    res = [one_run(args, i + 1) for i in range(args.runs)]
    avg = statistics.mean(r[0] for r in res)
    p95 = statistics.mean(r[1] for r in res)
    verdict = "PASS" if avg <= args.target_ms else "MISS"
    print(f"[MOVER_BUDGET v1] {verdict} mission={args.mission} movers={res[0][2]} runs={args.runs} windows={sum(r[3] for r in res)} "
          f"avg_tick_ms={avg:.3f} p95_tick_ms={p95:.3f} per_run_avg={','.join(f'{r[0]:.3f}' for r in res)} target_ms={args.target_ms}")
    return 0 if verdict == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
