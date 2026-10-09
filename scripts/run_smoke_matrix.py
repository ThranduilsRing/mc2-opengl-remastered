#!/usr/bin/env python3
"""run_smoke_matrix.py -- run named smoke environment matrices.

Reads a JSON matrix file (see tests/smoke/matrices/), builds a
run_smoke.py invocation per entry, and executes them in sequence.
Exits 0 only if every entry passes.

Schema (tests/smoke/matrices/*.json):
  {
    "matrix_id": "staticprop",
    "description": "...",
    "default_missions": ["mc2_01", "mc2_10"],   # used when entry has no missions
    "default_duration": 15,                      # seconds; overridable per entry or CLI
    "entries": [
      {
        "id": "some_variant",
        "description": "...",
        "env": {"MC2_FOO": "1", "MC2_BAR": "0"},  # env overrides for this entry
        "missions": ["mc2_01"],   # optional; falls back to matrix default_missions
        "duration": 20            # optional; falls back to matrix default_duration
      },
      ...
    ]
  }

Usage:
  # dry-run: print commands without executing
  py -3 scripts/run_smoke_matrix.py tests/smoke/matrices/staticprop.json --dry-run

  # run all entries
  py -3 scripts/run_smoke_matrix.py tests/smoke/matrices/staticprop.json

  # run by matrix name (looked up in tests/smoke/matrices/)
  py -3 scripts/run_smoke_matrix.py terrain --dry-run

  # run a single entry
  py -3 scripts/run_smoke_matrix.py staticprop --entry staticprop_ibl_off

  # override duration for all entries
  py -3 scripts/run_smoke_matrix.py terrain --duration 10
"""

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
# OWNER RULE: no window for any child process (hidden_kwargs: CREATE_NO_WINDOW + SW_HIDE).
from smoke_lib import nofocus  # noqa: E402

SCRIPT_DIR = Path(__file__).resolve().parent
WORKTREE_ROOT = SCRIPT_DIR.parent
RUN_SMOKE = SCRIPT_DIR / "run_smoke.py"
MATRICES_DIR = WORKTREE_ROOT / "tests" / "smoke" / "matrices"


def load_matrix(path: Path) -> dict:
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def resolve_matrix_path(name_or_path: str) -> Path:
    p = Path(name_or_path)
    if p.exists():
        return p
    # name lookup: "staticprop" -> tests/smoke/matrices/staticprop.json
    candidate = MATRICES_DIR / f"{name_or_path}.json"
    if candidate.exists():
        return candidate
    print(f"[MATRIX] ERROR: matrix not found: {name_or_path!r}", file=sys.stderr)
    print(f"[MATRIX]   tried: {p}", file=sys.stderr)
    print(f"[MATRIX]   tried: {candidate}", file=sys.stderr)
    sys.exit(1)


def build_smoke_args(entry: dict, matrix: dict, duration_override: int | None) -> list[str]:
    missions: list[str] = entry.get("missions") or matrix.get("default_missions") or []
    if duration_override is not None:
        dur = duration_override
    elif entry.get("duration") is not None:
        dur = entry["duration"]
    elif matrix.get("default_duration") is not None:
        dur = matrix["default_duration"]
    else:
        dur = 15

    # NEVER --kill-existing: it taskkills any concurrent mc2.exe (producing a
    # false crash_silent). run_smoke.py already holds a concurrency-safe lock
    # and refuses to run if another instance is live. Enforced by
    # scripts/check-smoke-matrices.py.
    args = [
        sys.executable, str(RUN_SMOKE),
        "--keep-logs",
        "--duration", str(dur),
    ]

    if missions:
        for m in missions:
            args += ["--mission", m]
    else:
        args += ["--tier", "tier1"]

    return args


def run_entry(
    entry: dict,
    matrix: dict,
    duration_override: int | None,
    dry_run: bool,
) -> int:
    env_overrides: dict[str, str] = {k: str(v) for k, v in entry.get("env", {}).items()}
    smoke_args = build_smoke_args(entry, matrix, duration_override)

    env_desc = "  ".join(f"{k}={v}" for k, v in env_overrides.items()) or "(default env)"
    print(f"\n[MATRIX] ---- entry: {entry['id']} ----")
    print(f"[MATRIX] desc:  {entry.get('description', '')}")
    print(f"[MATRIX] env:   {env_desc}")
    print(f"[MATRIX] cmd:   {' '.join(smoke_args)}")

    if dry_run:
        print("[MATRIX] (dry-run: skipping execution)")
        return 0

    env = os.environ.copy()
    env.update(env_overrides)
    # background-launch: allow streams run_smoke.py output to the caller's console; inherits it (no new window when the parent has one)
    result = subprocess.run(smoke_args, env=env)
    rc = result.returncode
    print(f"[MATRIX] exit:  {rc}  ({'PASS' if rc == 0 else 'FAIL'})")
    return rc


def main() -> None:
    ap = argparse.ArgumentParser(
        description="Run named smoke environment matrices.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    ap.add_argument(
        "matrix",
        help="Matrix JSON file path, or matrix name (looks up tests/smoke/matrices/<name>.json).",
    )
    ap.add_argument(
        "--dry-run", action="store_true",
        help="Print commands without executing.",
    )
    ap.add_argument(
        "--duration", type=int, default=None,
        help="Override per-mission duration in seconds for all entries.",
    )
    ap.add_argument(
        "--entry", default=None,
        help="Run only the entry with this id.",
    )
    ap.add_argument(
        "--list", action="store_true",
        help="List matrix entries without running anything.",
    )
    args = ap.parse_args()

    matrix_path = resolve_matrix_path(args.matrix)
    matrix = load_matrix(matrix_path)
    entries: list[dict] = matrix.get("entries", [])

    if args.list:
        print(f"Matrix: {matrix['matrix_id']}  ({matrix_path})")
        print(f"  {matrix.get('description', '')}")
        print(f"  default_missions: {matrix.get('default_missions', [])}")
        print(f"  default_duration: {matrix.get('default_duration', 15)}s")
        print(f"  entries ({len(entries)}):")
        for e in entries:
            print(f"    {e['id']:40s}  {e.get('description', '')}")
        return

    if args.entry:
        filtered = [e for e in entries if e["id"] == args.entry]
        if not filtered:
            ids = [e["id"] for e in entries]
            print(f"[MATRIX] ERROR: entry {args.entry!r} not found.", file=sys.stderr)
            print(f"[MATRIX]   available: {ids}", file=sys.stderr)
            sys.exit(1)
        entries = filtered

    print(f"[MATRIX] matrix={matrix['matrix_id']}  entries={len(entries)}")
    if args.dry_run:
        print("[MATRIX] DRY-RUN: commands printed, not executed")

    failures: list[str] = []
    for entry in entries:
        rc = run_entry(entry, matrix, args.duration, args.dry_run)
        if rc != 0:
            failures.append(entry["id"])

    print(f"\n[MATRIX] ===== RESULTS =====")
    print(f"[MATRIX] {len(entries) - len(failures)}/{len(entries)} passed")
    if failures:
        print(f"[MATRIX] FAILED: {', '.join(failures)}")
        sys.exit(1)


if __name__ == "__main__":
    main()
