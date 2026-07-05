#!/usr/bin/env python3
"""scripts/slice_gate.py — automated slice validation chain.

Chains the manual sequence we run for every gated slice: build -> deploy ->
smoke pair (baseline vs gate-ON) -> verdict. Each step is independently
skippable. stdlib only (subprocess/argparse/re/json), Python 3.

Examples:
  # Full chain, default deploy target, default gate-less single smoke run
  py -3 scripts/slice_gate.py

  # Dry run showing the exact command plan for a gated slice validation
  py -3 scripts/slice_gate.py --dry-run --gate MC2_TERRAIN_CONTROLMAP=1

  # Multiple gates, custom missions, custom deploy target
  py -3 scripts/slice_gate.py --gate MC2_TERRAIN_CONTROLMAP=1 \\
      --gate MC2_TERRAIN_CONTROLMAP_FILE=data/controlmap/sample.ctl \\
      --mission mc2_01 --mission mc2_10 \\
      --deploy-target "A:/Games/mc2-opengl/releases/0.5 testing/mc2-win64-v0.5.0"

  # Skip build+deploy, only run the smoke pair (binaries already deployed)
  py -3 scripts/slice_gate.py --no-build --no-deploy --gate MC2_TERRAIN_CONTROLMAP=1

  # Build only (e.g. CI compile check)
  py -3 scripts/slice_gate.py --no-deploy --no-smoke

Constraints honored: never launches a game process directly (only via
run_smoke.py), never passes --kill-existing, never edits run_smoke.py /
deploy_payload.py.
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

CMAKE_EXE = (
    "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/"
    "Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
)

DEFAULT_DEPLOY_TARGET = "A:/Games/mc2-opengl/releases/0.5 testing/mc2-win64-v0.5.0"
DEFAULT_MISSIONS = ["mc2_01", "mc2_24"]
DEFAULT_DEPLOY_LANE = "0.5-testing"

STANDARD_SMOKE_ENV = {
    "MC2_DEBUG_STATE_DUMP": "1",
    "MC2_DIAGNOSTIC_TRACE_FILE": "debug_state/diagnostic_trace.jsonl",
    "MC2_DIAG_TAGS": "CONFIG,BUILD,DEVICE",
}

# Candidate locations for the launcher exe across build layouts we've seen.
LAUNCHER_GLOBS = [
    "build64/out/mc2_launcher/RelWithDebInfo/mc2-launcher.exe",
    "build64/out/mc2_launcher/RelWithDebInfo/mc2_launcher.exe",
    "build64/mc2_launcher/RelWithDebInfo/mc2_launcher.exe",
    "build64/RelWithDebInfo/mc2_launcher.exe",
    "build64/mc2_launcher.exe",
]

# Candidate build-dir locations for mc2.exe (order = preference).
MC2_EXE_CANDIDATES = [
    "build64/mc2.exe",
    "build64/RelWithDebInfo/mc2.exe",
]

class Step:
    def __init__(self, name):
        self.name = name
        self.result = "SKIPPED"
        self.note = ""

def run(cmd, *, cwd=None, env=None, dry_run=False, capture=False):
    printable = " ".join(f'"{c}"' if " " in c else c for c in cmd)
    print(f"[slice_gate] $ {printable}")
    if dry_run:
        return 0, ""
    if capture:
        proc = subprocess.run(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True)
        print(proc.stdout)
        return proc.returncode, proc.stdout
    proc = subprocess.run(cmd, cwd=cwd, env=env)
    return proc.returncode, ""

def find_launcher_exe() -> Path | None:
    for rel in LAUNCHER_GLOBS:
        p = ROOT / rel
        if p.exists():
            return p
    # Fallback: recursive glob for any *launcher*.exe under build64.
    build64 = ROOT / "build64"
    if build64.exists():
        for cand in build64.rglob("*launcher*.exe"):
            if "CMakeFiles" not in str(cand):
                return cand
    return None

def find_mc2_build_dir() -> str | None:
    """Return the build dir (relative to ROOT) containing mc2.exe, or None."""
    for rel in MC2_EXE_CANDIDATES:
        if (ROOT / rel).exists():
            return str(Path(rel).parent)
    return None

def do_build(args, steps) -> bool:
    step = Step("build")
    steps.append(step)
    targets = ["mc2"]
    launcher_exe = find_launcher_exe()
    if launcher_exe is None:
        targets.append("mc2_launcher")
    cmd = [CMAKE_EXE, "--build", "build64", "--config", "RelWithDebInfo"]
    for t in targets:
        cmd += ["--target", t]
    rc, _ = run(cmd, cwd=str(ROOT), dry_run=args.dry_run)
    if args.dry_run:
        step.result = "DRY-RUN"
        step.note = f"targets={','.join(targets)}"
        return True
    if rc != 0:
        step.result = "FAIL"
        step.note = f"cmake --build exited {rc}"
        return False
    step.result = "PASS"
    step.note = f"targets={','.join(targets)}"
    return True

def do_deploy(args, steps) -> bool:
    step = Step("deploy")
    steps.append(step)
    build_dir = find_mc2_build_dir()
    if build_dir is None:
        if args.dry_run:
            build_dir = "build64"  # best guess for printing the plan
        else:
            step.result = "FAIL"
            step.note = "could not find mc2.exe under build64/ or build64/RelWithDebInfo/"
            return False
    cmd = [
        "py", "-3", "scripts/deploy_payload.py", args.deploy_target,
        "--source-root", ".",
        "--build-dir", build_dir,
        "--exe-name", "mc2.exe",
        "--allow-new-target",
    ]
    rc, _ = run(cmd, cwd=str(ROOT), dry_run=args.dry_run)
    if args.dry_run:
        step.result = "DRY-RUN"
        step.note = f"build_dir={build_dir} target={args.deploy_target}"
        return True
    if rc != 0:
        step.result = "FAIL"
        step.note = f"deploy_payload.py exited {rc}"
        return False
    step.result = "PASS"
    step.note = f"build_dir={build_dir}"
    return True

def build_smoke_cmd(args, gate_env: dict[str, str] | None) -> list[str]:
    cmd = ["py", "-3", "scripts/run_smoke.py", "--deploy", args.deploy_lane]
    for m in args.mission:
        cmd += ["--mission", m]
    cmd += ["--duration", str(args.duration), "--keep-logs"]
    return cmd

def run_smoke_pass(label: str, args, gate_env: dict[str, str] | None, steps) -> tuple[bool, str]:
    """Run one smoke pass; returns (ok, combined_stdout)."""
    step = Step(f"smoke:{label}")
    steps.append(step)

    import os
    env = dict(os.environ)
    env.update(STANDARD_SMOKE_ENV)
    gate_names = []
    if gate_env:
        for k, v in gate_env.items():
            env[k] = v
            gate_names.append(k)

    cmd = build_smoke_cmd(args, gate_env)
    if args.dry_run:
        run(cmd, cwd=str(ROOT), env=env, dry_run=True)
        step.result = "DRY-RUN"
        step.note = f"gates={','.join(gate_names) or 'none'}"
        return True, ""

    rc, out = run(cmd, cwd=str(ROOT), env=env, dry_run=False, capture=True)

    passed = (rc == 0) and bool(re.search(r"\bresult=PASS\b", out))
    failed_explicit = bool(re.search(r"\bresult=FAIL\b", out))
    dropped = [g for g in gate_names if re.search(
        rf"\[ENV-DROP\][^\n]*\b{re.escape(g)}\b", out)]
    fingerprint_lines = re.findall(r"\[DEPLOY_FINGERPRINT\][^\n]*", out)

    note_bits = []
    if gate_names:
        note_bits.append(f"gates={','.join(gate_names)}")
    if fingerprint_lines:
        note_bits.append(fingerprint_lines[-1][:80])
    if dropped:
        note_bits.append(f"ENV-DROP! {','.join(dropped)}")

    if rc != 0 or failed_explicit or dropped:
        step.result = "FAIL"
        if rc != 0 and not failed_explicit:
            note_bits.insert(0, f"exit={rc}")
        step.note = "; ".join(note_bits) or "smoke failed"
        return False, out

    if not passed:
        # rc==0 but couldn't confirm result=PASS in stdout; treat as soft-pass
        # since run_smoke.py's own exit code is the authoritative verdict.
        note_bits.insert(0, "no result=PASS line found (exit 0 trusted)")

    step.result = "PASS"
    step.note = "; ".join(note_bits) or "ok"
    return True, out

def print_summary(steps):
    print("\n[slice_gate] summary")
    print("| step | result | note |")
    print("|------|--------|------|")
    for s in steps:
        print(f"| {s.name} | {s.result} | {s.note} |")

def main() -> int:
    ap = argparse.ArgumentParser(
        description="Automate build+deploy+smoke-pair slice validation chain.")
    ap.add_argument("--no-build", dest="build", action="store_false", default=True)
    ap.add_argument("--no-deploy", dest="deploy", action="store_false", default=True)
    ap.add_argument("--no-smoke", dest="smoke", action="store_false", default=True)
    ap.add_argument("--deploy-target", default=DEFAULT_DEPLOY_TARGET,
                     help="Deploy destination path passed to deploy_payload.py")
    ap.add_argument("--deploy-lane", default=DEFAULT_DEPLOY_LANE,
                     help="--deploy <name> lane passed to run_smoke.py")
    ap.add_argument("--mission", action="append", default=[],
                     help="Mission to smoke (repeatable). Default: mc2_01 mc2_24")
    ap.add_argument("--duration", type=int, default=30)
    ap.add_argument("--gate", action="append", default=[],
                     help="Gate env var to enable in the second smoke pass, "
                          "NAME or NAME=VALUE (repeatable). If omitted, only "
                          "a single baseline smoke pass runs.")
    ap.add_argument("--dry-run", action="store_true",
                     help="Print the command plan without executing anything.")
    args = ap.parse_args()

    if not args.mission:
        args.mission = list(DEFAULT_MISSIONS)

    steps: list[Step] = []
    ok = True

    if args.build:
        ok = do_build(args, steps) and ok
        if not ok and not args.dry_run:
            print_summary(steps)
            return 1
    else:
        steps.append(Step("build"))

    if args.deploy:
        ok = do_deploy(args, steps) and ok
        if not ok and not args.dry_run:
            print_summary(steps)
            return 1
    else:
        steps.append(Step("deploy"))

    if args.smoke:
        baseline_ok, _ = run_smoke_pass("baseline", args, None, steps)
        ok = ok and baseline_ok

        gate_env: dict[str, str] = {}
        for g in args.gate:
            if "=" in g:
                k, v = g.split("=", 1)
            else:
                k, v = g, "1"
            gate_env[k] = v

        if gate_env:
            gate_ok, _ = run_smoke_pass("gate-on", args, gate_env, steps)
            ok = ok and gate_ok
    else:
        steps.append(Step("smoke"))

    print_summary(steps)

    if args.dry_run:
        return 0
    return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main())
