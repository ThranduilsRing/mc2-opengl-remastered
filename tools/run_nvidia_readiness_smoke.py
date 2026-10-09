#!/usr/bin/env python3
"""NVIDIA-FIRST-RUN-TRACE-PRESET-1 — readiness preset (NO renderer code).

Aims the first NVIDIA run per docs/render-backend-seams/nvidia-readiness-runbook.md.
This script sets known-useful trace env vars and drives the EXISTING harness/checkers —
it does not change rendering and adds no NVIDIA-specific code.

Modes:
  --checkers-only   (DEFAULT) run the 8 render-seam checkers only. No game launch, no
                    mouse grab — vendor-independent static gate. Run this on the NVIDIA
                    checkout before anything else.
  --functional EXE  set the first-run functional-sanity gate profile and run an autoplay
                    smoke (mc2_10 + mc2_24) against EXE. Launches the game (minimized
                    autoplay) — only pass this when you intend to run it.

The byte-parity capture and light-stall Tracy passes are deliberately NOT automated here
(run them separately per the runbook §2 so debug-fatal noise doesn't abort a capture).
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "scripts"))
import console_child  # noqa: E402  (stdio-inheriting children keep their output; scripts/console_child.py)
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

SEAM_CHECKERS = [
    "check-colormask-ownership.py",
    "check-pass-attachment-contract.py",
    "check-frame-resource-feedback.py",
    "check-drawbuffer-ownership.py",
    "check-static-prop-family.py",
    "check-mdi-submission-ownership.py",
    "check-buffer-lifetime-ownership.py",
    "check-shader-variant.py",
]

# First-run FUNCTIONAL-SANITY profile (runbook §2). NOT the byte-parity profile.
FUNCTIONAL_ENV = {
    "MC2_GL_DEBUG_FATAL": "1",          # surface AMD-tolerated GL errors as hard stops
    "MC2_RENDER_FRAME_PLAN_TRACE": "1",  # live pass/path identity
    "MC2_MDI_SUBMIT_TRACE": "1",         # indirect submitters
    "MC2_PIPELINE_BIND_TRACE": "1",      # pipeline binds present
    "MC2_LOG": "1",
}
MISSIONS = ["mc2_10", "mc2_24"]


def run_checkers() -> int:
    rc = 0
    for c in SEAM_CHECKERS:
        p = ROOT / "scripts" / c
        r = subprocess.run([sys.executable, str(p), "--quiet"],
                           capture_output=True, text=True, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        ok = (r.returncode == 0)
        print(f"  {'PASS' if ok else 'FAIL'}  {c}")
        if not ok:
            rc = 1
            if r.stderr.strip():
                print(r.stderr.strip())
    print("[nvidia-readiness] checkers " + ("PASS" if rc == 0 else "FAIL"))
    return rc


def run_functional(exe: str) -> int:
    env = os.environ.copy()
    env.update(FUNCTIONAL_ENV)
    cmd = [sys.executable, str(ROOT / "scripts" / "run_smoke.py")]
    for m in MISSIONS:
        cmd += ["--mission", m]
    cmd += ["--duration", "30", "--keep-logs"]
    print("[nvidia-readiness] functional-sanity profile:")
    for k, v in FUNCTIONAL_ENV.items():
        print(f"    {k}={v}")
    print(f"[nvidia-readiness] launching: {' '.join(cmd)} (exe={exe})")
    env["MC2_SMOKE_EXE"] = exe  # informational; run_smoke takes --exe if supported
    r = subprocess.run(cmd + (["--exe", exe] if exe else []), env=env, **console_child.child_kwargs())
    print("[nvidia-readiness] NOTE: triage any new GL_DEBUG_FATAL stops as EXPECTED-first "
          "(runbook §5); diff the [FRAME_PLAN]/[PIPELINE_BIND]/[MDI_SUBMIT] pass SET vs an "
          "AMD log; bless a NVIDIA golden set (do not force the AMD hash).")
    return r.returncode


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--checkers-only", action="store_true", default=True)
    ap.add_argument("--functional", metavar="EXE", default=None,
                    help="run the autoplay functional-sanity smoke against EXE (launches game)")
    args = ap.parse_args()

    print("[nvidia-readiness] see docs/render-backend-seams/nvidia-readiness-runbook.md")
    if args.functional:
        rc = run_checkers()
        if rc != 0:
            print("[nvidia-readiness] checkers FAILED — fix before the functional run.")
            return rc
        return run_functional(args.functional)
    return run_checkers()


if __name__ == "__main__":
    sys.exit(main())
