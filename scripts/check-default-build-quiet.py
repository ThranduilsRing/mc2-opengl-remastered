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
  py -3 scripts/check-default-build-quiet.py --smoke-artifacts   # newest default-env smoke run on disk
  py -3 scripts/check-default-build-quiet.py --fresh-smoke --exe <mc2.exe>   # merge-gate stage: launches the game
  py -3 scripts/check-default-build-quiet.py --update-baseline   # re-seed (review the diff!)

Exit 0 = no new tags, 1 = new unconditional diagnostic output found.
OFFLINE BY DEFAULT: with no flags this grades current-HEAD clean default-env smoke logs, or
SKIPs ("no fresh smoke log; graded at merge gate"). It never launches the game, so
the ordinary check-contracts row stays offline. Only --fresh-smoke launches one, and
it requires the explicit --exe (scripts/merge_gate.py passes the gate's install).
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import tempfile

import console_child
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
BENIGN_ENV = ("MC2_SMOKE_REQUIRE_FINGERPRINT", "MC2_SMOKE_ARTIFACT_ROOT", "MC2_HOST_LEASE_BATCH", "MC2_SMOKE_QUIET_DEFAULT", "MC2_IN_GATE")


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
    head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True,
                                   **console_child.child_kwargs(redirected=True)).strip()
    runs = sorted((d for d in ARTIFACT_ROOT.iterdir() if d.is_dir()),
                  key=lambda d: d.stat().st_mtime, reverse=True)
    for run in runs:
        # Only a run taken with no output-affecting
        # MC2_* env can be graded against the default-build baseline.
        if not is_default_env_run(run):
            continue
        # Grade the binary that ran, rather than the source checkout recorded by its runner.
        try:
            manifest = json.loads((run / "manifest.json").read_text(encoding="utf-8"))
            provenance = manifest.get("identity", {}).get("binary_provenance", {})
            commit = provenance.get("commit") or ""
            if len(commit) < 7 or not head.startswith(commit) or provenance.get("dirty") is not False:
                continue
        except (OSError, ValueError):
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


from smoke_lib.paths import PROOF_ROOT

def fresh_smoke_logs(exe):
    """Grade only the smoke launched by this invocation, never historical artifacts.

    The only game-launch path in this tool; reached only through --fresh-smoke with an explicit exe."""
    if not exe:
        raise RuntimeError("--fresh-smoke requires an explicit --exe")
    # A gate row names its artefact root (its --resume evidence); standalone runs keep the archive folder.
    proof = Path(os.environ.get("MC2_SMOKE_ARTIFACT_ROOT") or (PROOF_ROOT + '/default-build-quiet'))
    proof.mkdir(parents=True, exist_ok=True)
    run_root = Path(tempfile.mkdtemp(prefix="run-", dir=proof))
    env = {k: v for k, v in os.environ.items() if not k.startswith("MC2_")}
    for key in ("MC2_HOST_LEASE_BATCH", "MC2_IN_GATE"):   # lease plumbing: a gate child must stay a gate child (GATE QUIET)
        if os.environ.get(key):
            env[key] = os.environ[key]
    env["MC2_SMOKE_ARTIFACT_ROOT"] = str(run_root)
    env["MC2_SMOKE_QUIET_DEFAULT"] = "1"   # run_smoke must not add the PERF_COUNTERS / CULL_PARITY diag tags to this sample
    command = [sys.executable, str(ROOT / "scripts/run_smoke.py"), "--mission", "mc2_10",
               "--duration", "15", "--keep-logs"]
    command += ["--exe", str(exe)]
    result = subprocess.run(command, cwd=ROOT, env=env, **console_child.child_kwargs())
    if result.returncode:
        raise RuntimeError(f"fresh default smoke failed ({result.returncode}); logs: {run_root}")
    runs = [p for p in run_root.iterdir() if p.is_dir() and is_default_env_run(p)]
    logs = [log for run in runs for log in console_logs(run)]
    if not logs or any(not p.stat().st_size for p in logs):
        raise RuntimeError(f"fresh default smoke produced no default console logs: {run_root}")
    return logs


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", action="append", default=[],
                    help="log file to grade. Repeat for several.")
    ap.add_argument("--smoke-artifacts", action="store_true",
                    help="grade the newest tier1 smoke run's logs")
    ap.add_argument("--update-baseline", action="store_true",
                    help="re-seed the baseline from the graded logs")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--fresh-smoke", action="store_true",
                    help="launch a fresh default smoke of --exe and grade it (merge-gate stage only)")
    ap.add_argument("--exe", type=Path, help="install executable for --fresh-smoke")
    ap.add_argument("--run-root", type=Path, help="grade a named current gate's smoke evidence; never search historical runs")
    ap.add_argument("--mission", default="mc2_17")
    args = ap.parse_args()

    logs = [Path(p) for p in args.log]
    if args.run_root:
        logs = list(args.run_root.glob(f"*/{args.mission}.log"))
        if len(logs) != 1 or not logs[0].stat().st_size:
            print("[quiet-ratchet] FAIL: missing or ambiguous current tier1 quiet sample")
            return 1
        try:
            sample = json.loads((logs[0].parent / "quiet_sample.json").read_text(encoding="utf-8"))
            env = {**sample["ambient"], **sample["env_extra"]}
            dirty = [k for k in env if (any(hint in k for hint in DIAG_ENV_HINTS)
                     or k in ("MC2_MOVER_COHERENCE_AUDIT", "MC2_VULKAN_VALIDATION")
                     or k.startswith("MC2_VISUAL_")) and env[k] not in ("0", "", "FALSE")]
            if sample["mission"] != args.mission or not sample["passed"] or dirty:
                raise ValueError(f"non-default or failed quiet sample: {dirty}")
        except (OSError, ValueError, KeyError) as exc:
            print(f"[quiet-ratchet] FAIL: {exc}")
            return 1
    if args.smoke_artifacts:
        logs.extend(newest_smoke_logs())
    if args.fresh_smoke:
        try:
            logs.extend(fresh_smoke_logs(args.exe))
        except (OSError, RuntimeError) as exc:
            print(f"[quiet-ratchet] FAIL: {exc}")
            return 1
    elif not logs and not args.smoke_artifacts:
        logs.extend(newest_smoke_logs())
    logs = [p for p in logs if p.is_file()]

    if not logs:
        print("[quiet-ratchet] SKIP: no fresh smoke log; graded at merge gate "
              "(merge_gate stage 'quiet' runs --fresh-smoke --exe <install>/mc2.exe)")
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
