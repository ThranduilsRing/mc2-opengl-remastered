#!/usr/bin/env python3
"""check-frame-graph.py — CI gate for the render-frame contract's offline suites.

Builds + runs the pure-logic doctest suites that guard the contract (no GL/engine/
deploy/smoke):
  - FrameGraph:   resource-DAG validation -- reorder / missing-producer regression guard
                  (FRAME-GRAPH-SKELETON-1)
  - ViewCurrency: object MVP view-epoch currency + same-camera dedupe, incl. the
                  counter-proof of the old stale-every-frame bug (VIEW-EPOCH-DEDUPE-1)

Delegates to scripts/run-unit-tests.sh so there is ONE builder/runner. Exit 0 iff both
suites pass.
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
runner = ROOT / "scripts" / "run-unit-tests.sh"
if not runner.exists():
    print(f"check-frame-graph: missing {runner}", file=sys.stderr)
    sys.exit(2)


# ---------------------------------------------------------------------------
# UNWIRED-CHECK-CLOSE-1 (2026-07-30): this gate could not execute, at all.
# ---------------------------------------------------------------------------
# It exited 127 on every invocation since it was added, and nobody saw it
# because the script is not wired into any runner. TWO independent defects,
# both of which left the gate looking healthy:
#
#   1. str(Path) on Windows yields backslashes
#      ("A:\\Games\\mc2-main\\scripts\\run-unit-tests.sh"). bash consumes each
#      backslash as an escape, so the argv reaching it was the nonsense path
#      "A:Gamesmc2-mainscriptsrun-unit-tests.sh". Fixed with .as_posix().
#
#   2. Bare "bash" resolves via PATH to WSL's bash (C:/WINDOWS/system32/bash),
#      which is a Linux binary: it cannot see the A: drive at all, so even the
#      CORRECT posix path "A:/Games/.../run-unit-tests.sh" is "No such file or
#      directory" to it. The runner-exists() guard above passes either way,
#      because Python resolves the path natively -- so the gate's own
#      preflight told it everything was fine while it was incapable of
#      running. That is the failure class this lane exists to close: an
#      instrument alive by assertion only.
#
# Resolve Git Bash EXPLICITLY (never the PATH "bash"), and fail LOUD with a
# distinct exit code if no Windows-native bash exists -- a gate that cannot
# execute must say so, not return a number that reads like a verdict.
def find_git_bash():
    override = os.environ.get("MC2_BASH")
    if override and Path(override).exists():
        return override
    for cand in (
        r"C:/Program Files/Git/bin/bash.exe",
        r"C:/Program Files (x86)/Git/bin/bash.exe",
        r"C:/Program Files/Git/usr/bin/bash.exe",
    ):
        if Path(cand).exists():
            return cand
    # git --exec-path points into the Git install; bash sits two levels up.
    git = shutil.which("git")
    if git:
        cand = Path(git).resolve().parent.parent / "bin" / "bash.exe"
        if cand.exists():
            return cand.as_posix()
    return None


bash = find_git_bash()
if bash is None:
    print(
        "check-frame-graph: no Windows-native bash found (WSL bash cannot read "
        "A:/ paths). Set MC2_BASH=/path/to/Git/bin/bash.exe.",
        file=sys.stderr,
    )
    sys.exit(2)

# --ts filters to the two contract suites so the gate is fast and focused.
cmd = [bash, runner.as_posix(), "--ts=FrameGraph,ViewCurrency"]
sys.exit(subprocess.call(cmd))
