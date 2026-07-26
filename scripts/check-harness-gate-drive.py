#!/usr/bin/env python3
"""check-harness-gate-drive.py -- PERF-HARNESS-TRUTH-1 ratchet.

FAILS when a harness script tries to DRIVE an MC2_* gate that
`scripts/run_smoke.py` would silently strip from the child environment.

Why this exists
---------------
`run_smoke.py` builds the child env from an explicit passthrough allowlist.
Any MC2_* var not in that tuple never reaches mc2.exe. When a harness sets a
gate that is not allowlisted, the run does not fail -- it quietly executes the
gate-OFF path while the operator believes the gate is ON. Every conclusion
drawn from that run is then wrong in a way nothing surfaces.

This has now bitten repeatedly and expensively:

  * `MC2_CAMERA_PARK` was missing until `cadf9a92`, so EVERY pose-pinned smoke
    through this runner had silently measured the DEFAULT camera.
  * `MC2_LEGACY_LIT_MC2_FRAME` was missing, which voided its "verified via
    smoke" claim -- the A/B was never shown to have driven the arms apart.
  * `MC2_TERRAIN_LIGHT_POOLS` (the MASTER gate of the night light-pool
    feature) was missing while its own dependants were allowlisted.

The runner already warns at runtime ([ENV-DROP]), but a warning printed into a
long log during an unattended run is not a gate. This check fails the build.

Exit codes: 0 = clean, 1 = at least one undrivable gate found.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RUNNER = ROOT / "scripts" / "run_smoke.py"

# Harness scripts that launch the game THROUGH run_smoke.py. A script that
# spawns mc2.exe directly (tracy_capture_compare.py, run_visual_capture.py)
# builds its own env and is not subject to the runner's allowlist.
HARNESS_GLOBS = ["scripts/smoke_*.py", "scripts/run_*.py", "tools/*.py"]

# Vars that are harness-internal or intentionally not forwarded.
EXEMPT = {
    "MC2_SMOKE_MODE",       # set by the runner itself
    "MC2_LOG",              # ditto
    "MC2_DEBUG_STATE_DUMP",
    "MC2_DIAGNOSTIC_TRACE_FILE",
    "MC2_DIAG_TAGS",
}

SET_PAT = re.compile(
    r"""(?x)
    (?: env\s*\[\s*["'](MC2_[A-Z0-9_]+)["']\s*\]\s*=      # env["MC2_X"] =
      | ["'](MC2_[A-Z0-9_]+)["']\s*:\s*["']               # "MC2_X": "1"  (dict)
      | --env\s+(MC2_[A-Z0-9_]+)=                         # --env MC2_X=
      | f?["'](MC2_[A-Z0-9_]+)=\{?                        # "MC2_X=..."
    )"""
)


def allowlist() -> set:
    """Every MC2_* string literal appearing in run_smoke.py.

    Deliberately permissive: the allowlist tuple is a long, comment-interleaved
    literal, so any MC2_* mentioned in the file is treated as reachable. That
    biases toward false NEGATIVES (missing a real bug) rather than false
    positives, which keeps the check credible enough to stay enabled.
    """
    return set(re.findall(r'"(MC2_[A-Z0-9_]+)"', RUNNER.read_text(encoding="utf-8")))


def engine_reads() -> set:
    """MC2_* vars actually read by engine source -- ignore harness-only names."""
    found = set()
    for sub in ("GameOS", "code", "mclib", "RenderCore"):
        d = ROOT / sub
        if not d.is_dir():
            continue
        for p in d.rglob("*"):
            if p.suffix.lower() not in (".cpp", ".h", ".hpp", ".c"):
                continue
            try:
                txt = p.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            found.update(re.findall(r'getenv\s*\(\s*"(MC2_[A-Z0-9_]+)"', txt))
    return found


def main() -> int:
    if not RUNNER.is_file():
        print(f"SKIP: {RUNNER} not found", file=sys.stderr)
        return 0
    allowed, readable = allowlist(), engine_reads()

    problems = []
    for glob in HARNESS_GLOBS:
        for script in sorted(ROOT.glob(glob)):
            if script.resolve() == RUNNER.resolve():
                continue
            try:
                txt = script.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            # Only scripts that actually delegate to run_smoke are in scope.
            if "run_smoke" not in txt:
                continue
            for m in SET_PAT.finditer(txt):
                var = next(g for g in m.groups() if g)
                if var in EXEMPT or var in allowed or var not in readable:
                    continue
                line = txt[:m.start()].count("\n") + 1
                problems.append((script.relative_to(ROOT).as_posix(), line, var))

    if not problems:
        print("check-harness-gate-drive: OK -- every gate a harness drives "
              "through run_smoke.py is in the passthrough allowlist.")
        return 0

    print("check-harness-gate-drive: FAIL -- harness sets gate(s) the runner "
          "will STRIP (the gate would run inert and the run would lie):",
          file=sys.stderr)
    for path, line, var in sorted(set(problems)):
        print(f"  {path}:{line}: {var}", file=sys.stderr)
    print("\nFix: add the var to the passthrough allowlist tuple in "
          "scripts/run_smoke.py (with a comment saying why), or stop setting "
          "it in the harness.", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
