#!/usr/bin/env python3
"""check-asset-manifest-fixtures.py -- run the canonical asset-manifest
validator over the positive + negative fixture corpus and assert each one's
expected verdict.

Uses tools/asset_cook/validate_asset_manifest.py (canonical, single validator
per ruling C1 / asset-cook-pipeline-architecture.md §12.1). The scaffold copy
(tools/validate_asset_manifest.py) has been deleted.

  * the canonical valid fixture must PASS (exit 0)
  * every file under tests/fixtures/assets/invalid/ must FAIL (exit nonzero)

A negative fixture that starts PASSING is a schema regression -> this script
fails. Pure static check: it shells out to the validator only, launches no
engine, mutates nothing.

Usage:
  py -3 scripts/check-asset-manifest-fixtures.py
  py -3 scripts/check-asset-manifest-fixtures.py --verbose

Exit 0 = every fixture matched its expected verdict. Exit 1 = a mismatch.
"""

import subprocess
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
ROOT = SCRIPT_DIR.parent
VALIDATOR = ROOT / "tools" / "asset_cook" / "validate_asset_manifest.py"
VALID_FIXTURE = ROOT / "tests" / "fixtures" / "assets" / "minimal_asset_manifest.json"
INVALID_DIR = ROOT / "tests" / "fixtures" / "assets" / "invalid"


def run_validator(path: Path) -> int:
    """Return the validator's exit code for one manifest."""
    proc = subprocess.run(
        [sys.executable, str(VALIDATOR), str(path), "--quiet"],
        capture_output=True, text=True,
        creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
    )
    return proc.returncode, proc.stdout + proc.stderr


def main() -> int:
    verbose = "--verbose" in sys.argv[1:]

    if not VALIDATOR.exists():
        print(f"[manifest-fixtures] FAIL: validator not found: {VALIDATOR}", file=sys.stderr)
        return 1

    failures = []

    # 1. Positive fixture must PASS (exit 0).
    cases = [(VALID_FIXTURE, 0, "valid")]

    # 2. Every invalid fixture must FAIL (exit nonzero).
    if not INVALID_DIR.is_dir():
        print(f"[manifest-fixtures] FAIL: invalid-fixture dir not found: {INVALID_DIR}", file=sys.stderr)
        return 1
    invalid_files = sorted(INVALID_DIR.glob("*.json"))
    if not invalid_files:
        print(f"[manifest-fixtures] FAIL: no invalid fixtures under {INVALID_DIR}", file=sys.stderr)
        return 1
    for f in invalid_files:
        cases.append((f, "nonzero", "invalid"))

    for path, expect, kind in cases:
        rc, out = run_validator(path)
        if expect == 0:
            ok = (rc == 0)
        else:  # expect nonzero
            ok = (rc != 0)
        mark = "PASS" if ok else "FAIL"
        print(f"[manifest-fixtures] {mark}: {kind:<7} {path.name} (exit={rc}, expected={expect})")
        if not ok:
            failures.append(path.name)
            if verbose:
                print(out)

    print()
    if failures:
        print(f"[manifest-fixtures] FAIL: {len(failures)} fixture(s) gave the wrong verdict: "
              f"{', '.join(failures)}", file=sys.stderr)
        return 1
    print(f"[manifest-fixtures] OK: 1 valid + {len(invalid_files)} invalid fixtures "
          f"all matched expected verdicts.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
