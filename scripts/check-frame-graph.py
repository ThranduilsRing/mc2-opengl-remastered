#!/usr/bin/env python3
"""check-frame-graph.py — gate for the render-frame contract's offline suites.

Builds + runs the WHOLE pure-logic doctest tier (no GL/engine/deploy/smoke).
The suites this gate is named for are still the reason it exists:
  - FrameGraph:   resource-DAG validation -- reorder / missing-producer regression guard
                  (FRAME-GRAPH-SKELETON-1)
  - ViewCurrency: object MVP view-epoch currency + same-camera dedupe, incl. the
                  counter-proof of the old stale-every-frame bug (VIEW-EPOCH-DEDUPE-1)
...but it no longer FILTERS to them (DARK-TESTS-RUN-1; see the comment at the
call site). Running only those two made this script the sole doctest driver in
the repo while still skipping TEST_SUITE("RenderCore"), where a count lock had
drifted 46-vs-62 unnoticed.

Delegates to scripts/run-unit-tests.sh so there is ONE builder/runner. Exit 0
iff every doctest case passes.
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

# ---------------------------------------------------------------------------
# DARK-TESTS-RUN-1 (2026-07-31): the --ts filter is gone, deliberately.
# ---------------------------------------------------------------------------
# This gate used to pass `--ts=FrameGraph,ViewCurrency`, which reads as a
# focus optimisation and behaved as a blind spot. Once check-frame-graph.py was
# repaired it became the ONLY script in the repo that drove the doctest binary
# at all -- and it excluded TEST_SUITE("RenderCore") even on a fully successful
# run. That is precisely where test_rendercore.cpp pinned
# RendererFeature::COUNT == 46 against a real value of 62 and went unnoticed
# through fifteen appends.
#
# The filter bought nothing it was worth: the whole doctest suite is 631 cases
# in ~0.22s, and the build in front of it dominates either way (~0.7s warm,
# minutes cold). So the gate now runs the FULL doctest tier. --doctest-only
# keeps it off the engine build64/ cache, which this gate has no business
# configuring.
cmd = [bash, runner.as_posix(), "--doctest-only"]
sys.exit(subprocess.call(cmd))
