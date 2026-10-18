#!/usr/bin/env python3
"""check-default-build-quiet.py -- VERIFY-INTEGRITY-3

RATCHET: a DEFAULT build must not grow new unconditional diagnostic output.

WHY A RUNTIME CHECK AND NOT A GREP
----------------------------------
"Is this printf gated?" is not answerable lexically. A call site can sit behind
`if (s_someDiag)`, a static bool seeded from getenv, an `MC2_*_DIAG` bitmask
test, a default-ON feature's `IsEnabled()`, an `#ifdef`, or an already-gated
enclosing function. A grep-based checker either misses real cases or cries wolf
on gated ones -- and a checker that cries wolf gets ignored, which is how this
debt accumulated in the first place.

So this measures the property we actually care about, directly: run the game
with NO diagnostic env vars set, and look at what it printed. A tag that shows
up in that log IS unconditional output in a default build, by definition. No
inference, no false positives.

WHAT IT DOES
------------
Extracts the set of distinct `[TAG vN]` diagnostic tags from a smoke/run log,
and compares it against the tracked baseline in
`tests/contract/default_build_quiet_baseline.json`.

  * A tag in the log but NOT in the baseline  -> FAIL (new unconditional spam).
    Either gate it, or -- if it is genuinely meant to always print, like
    [BUILD_FINGERPRINT v1] which run_smoke.py parses -- add it to the baseline
    with a reason.
  * A tag in the baseline but NOT in the log  -> PASS, and report it as
    RETIRED so the baseline can be tightened. The ratchet only ever gets
    stricter.

The baseline is DEBT, not an approved list. Every entry without an explicit
`keep` reason is something that should eventually move behind a gate.

USAGE
  py -3 scripts/check-default-build-quiet.py --log <path> [--log <path> ...]
  py -3 scripts/check-default-build-quiet.py --smoke-artifacts   # newest tier1 run
  py -3 scripts/check-default-build-quiet.py --update-baseline   # re-seed (review the diff!)

Exit 0 = no new tags, 1 = new unconditional diagnostic output found.
Exits 0 with a SKIP when no log is available (so it is safe in check-contracts.sh
on a machine that has not run a smoke).
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BASELINE = ROOT / "tests" / "contract" / "default_build_quiet_baseline.json"
ARTIFACT_ROOT = ROOT / "tests" / "smoke" / "artifacts"

# `[SOMETHING v1]` / `[SOMETHING_ELSE v12]` -- the house diagnostic convention.
TAG_RE = re.compile(r"\[([A-Z][A-Z0-9_]{2,})\s+v(\d+)\]")

# Env vars whose presence means the log is NOT a clean default-build sample.
# This tuple was declared but never consulted -- the
# selector took the newest run unconditionally, so a tier1 run (which sets
# MC2_DEBUG_STATE_DUMP / MC2_DIAG_TAGS) would be graded as if it were a default
# build and fail on tags the user never sees. Runs are now filtered on their
# manifest's `env_gates`, which is the authoritative record of what the harness
# actually exported -- more reliable than scraping the log text.
DIAG_ENV_HINTS = ("MC2_DIAG_TAGS", "_DIAG", "_TRACE", "_PROBE", "MC2_DEBUG_")

# env_gates keys that do NOT change what the engine prints, so a run carrying
# only these is still a valid default-build sample.
BENIGN_ENV = ("MC2_SMOKE_REQUIRE_FINGERPRINT",)


def is_default_env_run(run_dir: Path) -> bool:
    """True if this smoke run was taken with no output-affecting MC2_* env.

    A tag is only 'unconditional in a default build' if it printed when nothing
    was set. Grading a diagnostic run against that baseline is exactly the kind
    of false alarm that gets a checker ignored.
    """
    mf = run_dir / "manifest.json"
    if not mf.is_file():
        return False          # cannot prove it was clean -> do not grade it
    try:
        gates = json.loads(mf.read_text(encoding="utf-8")).get("env_gates", {})
    except (OSError, ValueError):
        return False
    for k in gates:
        if k in BENIGN_ENV:
            continue
        return False
    return True


def tags_in_log(path: Path) -> set[str]:
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return set()
    return {f"{m.group(1)} v{m.group(2)}" for m in TAG_RE.finditer(text)}


def newest_smoke_logs() -> list[Path]:
    if not ARTIFACT_ROOT.is_dir():
        return []
    runs = sorted((d for d in ARTIFACT_ROOT.iterdir() if d.is_dir()),
                  key=lambda d: d.stat().st_mtime, reverse=True)
    for run in runs:
        # Only a run taken with no output-affecting
        # MC2_* env can be graded against the default-build baseline.
        if not is_default_env_run(run):
            continue
        logs = console_logs(run)
        if logs:
            return logs
    return []


def console_logs(run_dir: Path) -> list[Path]:
    """The per-mission STDOUT logs only -- `<mission>.log`, named by the run
    manifest.

    QUIET-DEFAULT-BUILD-1: a bare `rglob("*.log")` also swept up sidecar trace
    files that the engine writes to disk on purpose, e.g.
    `mc2_10.ring_trace.log` (an fprintf to a FILE handle, never the console).
    Those are not output the user sees, so failing the quiet ratchet on them is
    a false alarm -- and false alarms are how a checker earns the habit of
    being ignored.
    """
    mf = run_dir / "manifest.json"
    try:
        missions = json.loads(mf.read_text(encoding="utf-8")).get("missions", [])
    except (OSError, ValueError):
        return []
    return [p for p in (run_dir / f"{m}.log" for m in missions) if p.is_file()]


def load_baseline() -> dict:
    if not BASELINE.exists():
        return {"_doc": "", "tags": {}}
    return json.loads(BASELINE.read_text(encoding="utf-8"))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", action="append", default=[],
                    help="log file to grade. Repeat for several.")
    ap.add_argument("--smoke-artifacts", action="store_true",
                    help="grade the newest tier1 smoke run's logs")
    ap.add_argument("--update-baseline", action="store_true",
                    help="re-seed the baseline from the graded logs")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    logs = [Path(p) for p in args.log]
    if args.smoke_artifacts or not logs:
        logs.extend(newest_smoke_logs())
    logs = [p for p in logs if p.is_file()]

    if not logs:
        print("[quiet-ratchet] SKIP: no log available "
              "(run scripts/run_smoke.py --tier tier1 --keep-logs first)")
        return 0

    seen: set[str] = set()
    for p in logs:
        seen |= tags_in_log(p)

    baseline = load_baseline()
    known = set(baseline.get("tags", {}))

    if args.update_baseline:
        merged = dict(baseline.get("tags", {}))
        for t in sorted(seen):
            merged.setdefault(t, {"status": "debt",
                                  "reason": "unconditional in a default build; "
                                            "needs gating"})
        baseline["tags"] = merged
        BASELINE.parent.mkdir(parents=True, exist_ok=True)
        BASELINE.write_text(json.dumps(baseline, indent=2) + "\n",
                            encoding="utf-8")
        print(f"[quiet-ratchet] baseline updated: {len(merged)} tag(s) -> "
              f"{BASELINE.relative_to(ROOT).as_posix()}")
        return 0

    new = sorted(seen - known)
    retired = sorted(known - seen)

    if not args.quiet:
        print(f"[quiet-ratchet] graded {len(logs)} log(s); "
              f"{len(seen)} distinct tag(s) observed, "
              f"{len(known)} in baseline")
    for t in retired:
        print(f"[quiet-ratchet] RETIRED (tighten the baseline): {t}")

    if new:
        print(f"\n[quiet-ratchet] FAIL: {len(new)} NEW unconditional "
              f"diagnostic tag(s) in a default build:\n")
        for t in new:
            print(f"  - [{t}]")
        print("\n  A default build is what the USER runs. Put these behind an "
              "MC2_*_DIAG gate\n  (see MC2_TERRAIN_LOD_CHUNK_DIAG bit 524288 "
              "for the pattern), or, if the line\n  is genuinely meant to "
              "always print, add it to\n  "
              f"{BASELINE.relative_to(ROOT).as_posix()} with a `keep` reason.")
        return 1

    print("[quiet-ratchet] OK: no new unconditional diagnostic output")
    return 0


if __name__ == "__main__":
    sys.exit(main())
