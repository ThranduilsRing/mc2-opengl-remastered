"""Scheduled nightly/weekly merge gate against the PUBLISHED nifty in mc2-main.

    py -3 scripts/nightly_gate.py [--dry-run] [--weekly]

Runs `gate_wait.py --timeout 7200 -- merge_gate.py --nightly` (Sundays: --weekly), restricted to the tier-1/2 stages plus
deploy, at BELOW_NORMAL, never --kill-existing. Proof dir: E:/mc2-archive-2026-10/proofs/merge-gate/nightly-<date>/
(gate-verdict.json inside). Refuses (rc 2) when mc2-main is not on claude/nifty-mendeleev or is dirty. While another gate holds
A:/Games/mc2-ops/GATE_QUIET.json, gate_wait waits for it (up to --timeout) instead of skipping the night. tools/lane_merge_stage.py --promote refuses unless the newest
verdict here is under 36 h old, failed == [] and not INFRA (FAIL needs a triage* file). The gate must run in mc2-main, so a stage
branch checkout still refuses the night (the gate itself is orchestrator-only in mc2-main).
"""
import argparse
import datetime
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import console_child  # noqa: E402  (BELOW_NORMAL + CREATE_NO_WINDOW for every child)

PROOF_ROOT = "E:/mc2-archive-2026-10/proofs/merge-gate"
NIFTY = "claude/nifty-mendeleev"
TMP_DIR = "G:/mc2-tmp"


def git(*a):
    return subprocess.run(["git", "-C", ROOT, *a], capture_output=True, text=True,
                          **console_child.child_kwargs(redirected=True)).stdout.strip()


def refusal():
    if git("branch", "--show-current") != NIFTY:
        return f"mc2-main is not on {NIFTY}"
    if git("status", "--porcelain", "--untracked-files=no"):
        return "mc2-main is dirty"
    return None


def stage_names(weekly):
    import merge_gate
    keep = [n for n, _c, _e in merge_gate.plan(1)
            if n == "deploy" or merge_gate.stage_tier(n) >= 1]
    return [n for n in keep if weekly or merge_gate.stage_tier(n) < 2]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--weekly", action="store_true", help="default: only on Sundays")
    args = ap.parse_args(argv)
    why = refusal()
    if why:
        print(f"[nightly_gate] REFUSED: {why}")
        return 2
    today = datetime.date.today()
    weekly = args.weekly or today.weekday() == 6
    proof = f"{PROOF_ROOT}/nightly-{today.isoformat()}"
    os.makedirs(proof, exist_ok=True)
    py = [sys.executable] if os.name != "nt" else ["py", "-3"]
    cmd = py + [f"{ROOT}/scripts/gate_wait.py", "--timeout", "7200", "--log", f"{proof}/gate_wait.log", "--"] + py + [
        f"{ROOT}/scripts/merge_gate.py", "--weekly" if weekly else "--nightly", "--proofs-dir", proof,
        "--stages"] + stage_names(weekly)
    print("[nightly_gate] " + " ".join(cmd))
    if args.dry_run:
        return 0
    env = dict(os.environ, TMP=TMP_DIR, TEMP=TMP_DIR, TMPDIR=TMP_DIR)  # never write to C: (lane rules)
    os.makedirs(TMP_DIR, exist_ok=True)
    return subprocess.run(cmd, cwd=ROOT, env=env, **console_child.child_kwargs()).returncode


if __name__ == "__main__":
    sys.exit(main())
