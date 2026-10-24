#!/usr/bin/env python3
"""Orchestrator merge-window gate; --dry-run prints the plan without any launches.

Run after lane_merge verification. Deploy consumes that build64-ninja tree;
this gate never builds the engine a second time. Real runs require mc2-main
on claude/nifty-mendeleev. Lanes may only inspect the plan with --dry-run.
"""
from __future__ import annotations

import argparse
from contextlib import redirect_stdout, redirect_stderr
import os
from pathlib import Path
import subprocess
import sys
import time

import console_child

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from lane_merge import git_bash, run_logged  # noqa: E402


def plan(jobs, proof_dir=None):
    py = sys.executable
    install = "A:/Games/mc2-opengl/mc2-win64-v0.4"
    proof = Path(proof_dir or "E:/mc2-archive-2026-10/proofs/merge-gate")
    capture = [py, "tools/visual_verify/capture.py", "--install", install,
               "--bookmarks", "mc2_01_saved_205644,mc2_10_saved_204523", "--clean"]
    steps = [
        ("capture-before", capture + ["--out", str(proof / "before")], {}),
        ("deploy", [py, "scripts/deploy_payload.py", "--target", "game", "--ninja", "--require-clean"], {}),
        ("capture-after", capture + ["--out", str(proof / "after")], {}),
        ("compare-captures", [py, "tools/visual_verify/ab.py", "--compare-only", str(proof / "before"),
                              str(proof / "after"), "--out-root", str(proof / "ab")], {}),
        ("contracts", [git_bash(), "scripts/check-contracts.sh", "-j" + str(jobs)], {}),
        ("harnesses", [py, "tools/build_contract_harnesses.py", "--run", "--unit", "-j", str(jobs)], {}),
        ("pytest", [py, "-B", "-m", "pytest", "scripts/tests", "-q", "-p", "no:cacheprovider"], {}),
        ("tier1", [py, "scripts/run_smoke.py", "--tier", "tier1", "--keep-logs", "--deploy", "0.4"],
         {"MC2_DEBUG_STATE_DUMP": "1", "MC2_DIAG_TAGS": "CONFIG,BUILD,DEVICE"}),
        ("mods", [py, "tools/mod_smoke.py", "--battery", "--exe", install + "/mc2.exe", "--build-dir", "build64-ninja"], {}),
    ]
    for mission in ("mc2_03", "mc2_10", "mc2_24"):
        steps.append(("determinism-" + mission,
                      [py, "tools/sim_determinism.py", "check", mission, "--install", install], {}))
    for mission in ("mc2_01", "mc2_16"):
        steps.append(("saveload-" + mission,
                      [py, "tools/saveload_roundtrip.py", "check", "--mission", mission,
                       "--install", install, "--mask", "tools/save_noise_mask.json", "--first-save-only"], {}))
    steps.append(("validation",
                  [py, "scripts/run_smoke.py", "--lite", "--keep-logs", "--deploy", "0.4"],
                  {"MC2_VULKAN_VALIDATION": "1"}))
    return steps


def git(*args):
    return subprocess.check_output(["git", *args], cwd=ROOT, text=True,
                                   **console_child.child_kwargs(redirected=True)).strip()


def run_child(command, cwd, env):
    return run_logged(cwd, command, env=env)


def execute(steps, head, dry_run=False, runner=run_child):
    """Fail fast, show all remaining steps as skipped, and return nonzero on red."""
    results = []
    failed = False
    for name, command, overrides in steps:
        status, elapsed = "SKIPPED", 0.0
        if dry_run:
            print("PLAN " + " ".join(f"{k}={v}" for k, v in overrides.items()) + " " + subprocess.list2cmdline(command))
            status = "DRY-RUN"
        elif not failed:
            started = time.monotonic()
            try:
                if git("rev-parse", "HEAD") != head:
                    raise RuntimeError("HEAD changed during gate; rerun verification and gate")
                print(f"[merge_gate] {name}", flush=True)
                result = runner(command, cwd=ROOT, env=dict(os.environ, **overrides, MC2_IN_GATE="1"))
                status = "PASS" if result.returncode == 0 else f"FAIL ({result.returncode})"
            except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
                print(f"[merge_gate] {name}: {exc}", file=sys.stderr)
                status = "FAIL"
            elapsed = time.monotonic() - started
            failed = status != "PASS"
        results.append((name, status, elapsed))
    print(f"Merge gate HEAD {head} (deploy tree: build64-ninja)")
    print("| Step | Result | Seconds |\n|---|---|---:|")
    for name, status, elapsed in results:
        print(f"| {name} | {status} | {elapsed:.1f} |")
    return int(failed)


def select_stages(steps, stages=None, skip=None):
    available = {step[0]: step for step in steps}
    requested = [name for group in stages or [] for name in group.split(',')]
    omitted = {name for group in skip or [] for name in group.split(',')}
    unknown = (set(requested) | omitted) - available.keys()
    if unknown:
        raise ValueError("unknown stages: " + ", ".join(sorted(unknown)) + "; available: " + ", ".join(available))
    if len(requested) != len(set(requested)):
        raise ValueError("duplicate stages")
    return [available[name] for name in requested or available if name not in omitted]


class Tee:
    def __init__(self, console, log):
        self.console, self.log = console, log

    def write(self, text):
        self.console.write(text)
        self.log.write(text)
        self.flush()
        return len(text)

    def flush(self):
        self.console.flush()
        self.log.flush()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    if "MC2_IN_GATE" in os.environ:
        ap.error("MC2_IN_GATE is set: nested merge gates are forbidden")
    ap.add_argument("--dry-run", action="store_true", help="print commands only; no builds, deploys, or game launches")
    ap.add_argument("--expect-head", help="ref that HEAD must resolve to, e.g. the reviewed merge SHA")
    ap.add_argument("--stages", nargs="+", help="stage names in execution order (space or comma separated)")
    ap.add_argument("--skip", nargs="+", help="stage names to omit (space or comma separated)")
    ap.add_argument("--log", type=Path, help="gate log path (default: timestamped merge-gate proof directory)")
    try:
        jobs = Path("A:/Games/mc2-ops/JOBS").read_text().strip()
    except OSError:
        jobs = "2"
    ap.add_argument("-j", type=int, default=int(jobs))
    args = ap.parse_args(argv)
    if args.j < 1:
        ap.error("-j must be positive")
    proof = Path("E:/mc2-archive-2026-10/proofs/merge-gate") / time.strftime("%Y%m%d-%H%M%S")
    try:
        steps = select_stages(plan(args.j, proof), args.stages, args.skip)
    except ValueError as exc:
        ap.error(str(exc))
    head = git("rev-parse", "HEAD")
    if args.expect_head and git("rev-parse", args.expect_head + "^{commit}") != head:
        ap.error("HEAD does not match --expect-head")
    if not args.dry_run:
        if (ROOT.resolve() != Path("A:/Games/mc2-main").resolve()
                or git("branch", "--show-current") != "claude/nifty-mendeleev"):
            ap.error("real merge gates are orchestrator-only in mc2-main on claude/nifty-mendeleev; use --dry-run in lanes")
        if git("status", "--porcelain", "--untracked-files=no"):
            ap.error("commit tracked changes before running the merge gate")
    log_path = args.log or proof / "gate.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open("w", encoding="utf-8") as log:
        with redirect_stdout(Tee(sys.stdout, log)), redirect_stderr(Tee(sys.stderr, log)):
            print(f"[merge_gate] log: {log_path}", flush=True)
            return execute(steps, head, args.dry_run)


if __name__ == "__main__":
    sys.exit(main())
