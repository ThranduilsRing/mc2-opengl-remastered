#!/usr/bin/env python3
"""check-asset-manifests.py -- CI gate for asset-manifest fixtures.

Runs tools/asset_cook/validate_asset_manifest.py (canonical, single validator
per ruling C1 / asset-cook-pipeline-architecture.md §12.1) over the committed
fixtures:

  tests/fixtures/assets/*.json          -> must VALIDATE   (exit 0)
  tests/fixtures/assets/invalid/*.json  -> must be REJECTED (validator exit 1)

Deterministic, offline, no asset binaries. Exit 0 = all fixtures behaved as
expected; exit 1 = a positive fixture failed or a negative fixture validated.

Usage:
  py -3 scripts/check-asset-manifests.py
"""

import subprocess
import sys
from pathlib import Path
import console_child  # noqa: E402  (stdio-inheriting children keep their output; scripts/console_child.py)

REPO = Path(__file__).resolve().parent.parent
VALIDATOR = REPO / "tools" / "asset_cook" / "validate_asset_manifest.py"
ASSETS = REPO / "tests" / "fixtures" / "assets"
PREFIX = "[check-asset-manifests]"


def _run(manifest: Path, expect_fail: bool) -> bool:
    cmd = [sys.executable, str(VALIDATOR), str(manifest), "--quiet"]
    if expect_fail:
        cmd.append("--expect-fail")
    return subprocess.run(cmd, **console_child.child_kwargs()).returncode == 0


def main() -> int:
    if not VALIDATOR.exists():
        print(f"{PREFIX} ERROR validator not found: {VALIDATOR}", flush=True)
        return 1

    positives = sorted(ASSETS.glob("*.json"))
    negatives = sorted((ASSETS / "invalid").glob("*.json"))
    if not positives:
        print(f"{PREFIX} ERROR no positive fixtures under {ASSETS}", flush=True)
        return 1

    failures = 0
    for m in positives:
        ok = _run(m, expect_fail=False)
        print(f"{PREFIX} {'OK  ' if ok else 'FAIL'} valid   {m.name}", flush=True)
        failures += 0 if ok else 1
    for m in negatives:
        ok = _run(m, expect_fail=True)  # ok == 'rejected as expected'
        print(f"{PREFIX} {'OK  ' if ok else 'FAIL'} invalid {m.name}", flush=True)
        failures += 0 if ok else 1

    total = len(positives) + len(negatives)
    if failures:
        print(f"{PREFIX} {failures}/{total} fixture(s) misbehaved.", flush=True)
        return 1
    print(f"{PREFIX} OK: {total} fixtures behaved as expected "
          f"({len(positives)} valid, {len(negatives)} invalid).", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
