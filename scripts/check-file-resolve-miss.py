#!/usr/bin/env python3
"""check-file-resolve-miss.py — META-FIX design E nightly MISS audit.

Runs one smoke mission with the level-2 file-resolve JSONL trace
(MC2_LOG_FILE_RESOLVE=2 + MC2_RESOLVE_TRACE_FILE) and fails when any MISS
key is absent from the versioned benign-probe baseline
(scripts/file-resolve-miss-baseline.txt). This is the ONLY channel that
catches NEW content data with a baked backslash/uppercase path that slipped
past the canonical-key chokepoint — the static tiers cannot see data files.

Clone of scripts/check-combat-soak.py per INFRA-5 (oracle table, JSONL
counting, stale-proof unlink, env assembly). Do NOT force-share a module —
the oracle logic diverges.

fileExists() is a probe API (the engine legitimately probes absent files —
e.g. the CD-presence check, optional sidecars), so a baseline of known-benign
MISS keys is expected to be non-empty. Seed/refresh it deliberately with
--update-baseline after eyeballing the keys.

Convention: exit 0 = no unknown MISS keys; 1 = unknown MISS key(s) or trace
missing; 2 = harness error.

Usage:
  py -3 scripts/check-file-resolve-miss.py --exe <deployed mc2.exe>
        [--mission mc2_01] [--duration 30] [--update-baseline] [--quiet]
        [--json out.json]
"""

import argparse
import json
import os
import subprocess
import sys
import os as _os, sys as _sys  # noqa: E401
_sys.path.append(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "..", "scripts"))
import console_child  # noqa: E402  (windowless console children without a conhost each; scripts/console_child.py)

BASELINE_REL = os.path.join("scripts", "file-resolve-miss-baseline.txt")


def load_baseline(path):
    keys = set()
    try:
        with open(path, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                keys.add(line)
    except OSError:
        return None
    return keys


def collect_miss_keys(jsonl_path):
    """Return the set of MISS keys from a file-resolve JSONL trace, or None
    if the trace cannot be read."""
    misses = set()
    try:
        with open(jsonl_path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                if '"layer":"MISS"' not in line:
                    continue
                try:
                    rec = json.loads(line)
                except ValueError:
                    continue
                key = rec.get("key", "")
                if key:
                    misses.add(key)
    except OSError:
        return None
    return misses


def main():
    ap = argparse.ArgumentParser(
        description="Nightly file-resolve MISS audit (META-FIX design E)")
    ap.add_argument("--exe", required=True, help="Deployed mc2.exe to run")
    ap.add_argument("--root", default=None,
                    help="Worktree root (default: parent of scripts/)")
    ap.add_argument("--mission", default="mc2_01",
                    help="Smoke mission to trace (default mc2_01)")
    ap.add_argument("--duration", type=int, default=30,
                    help="Seconds (default 30 — run_smoke hard-caps at 30)")
    ap.add_argument("--update-baseline", action="store_true",
                    help="Write the observed MISS keys as the new baseline "
                         "(deliberate refresh only — eyeball the keys first)")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--json", default=None,
                    help="Write structured results to this path")
    args = ap.parse_args()

    root = args.root or os.path.dirname(
        os.path.dirname(os.path.abspath(__file__)))
    baseline_path = os.path.join(root, BASELINE_REL)
    if not os.path.isfile(args.exe):
        print("FAIL  file_resolve_miss  exe not found: %s" % args.exe)
        return 2

    trace_rel = os.path.join("debug_state",
                             "file_resolve_%s.jsonl" % args.mission)
    exe_dir = os.path.dirname(os.path.abspath(args.exe))
    jsonl_path = os.path.join(exe_dir, trace_rel)
    # Stale-proof: remove any prior trace so a failed launch can't read an
    # old pass (check-combat-soak.py precedent).
    try:
        os.remove(jsonl_path)
    except OSError:
        pass
    os.makedirs(os.path.dirname(jsonl_path), exist_ok=True)

    env = dict(os.environ)
    env["MC2_LOG_FILE_RESOLVE"] = "2"
    env["MC2_RESOLVE_TRACE_FILE"] = trace_rel

    cmd = [sys.executable, os.path.join(root, "scripts", "run_smoke.py"),
           "--exe", args.exe, "--mission", args.mission,
           "--duration", str(args.duration), "--keep-logs"]
    proc = subprocess.run(cmd, env=env, capture_output=True, text=True, **console_child.child_kwargs(redirected=True))
    smoke_ok = (proc.returncode == 0)

    misses = collect_miss_keys(jsonl_path)
    if misses is None:
        print("FAIL  file_resolve_miss  trace file not found/readable: %s "
              "(smoke_ok=%s)" % (jsonl_path, smoke_ok))
        return 1

    if args.update_baseline:
        with open(baseline_path, "w", encoding="utf-8", newline="\n") as f:
            f.write("# scripts/file-resolve-miss-baseline.txt — known-benign "
                    "file-resolve MISS keys\n")
            f.write("# (fileExists probes of legitimately-absent files, "
                    "optional sidecars, ...).\n")
            f.write("# Seeded from mission %s, duration %ds. Refresh "
                    "deliberately:\n" % (args.mission, args.duration))
            f.write("#   py -3 scripts/check-file-resolve-miss.py --exe ... "
                    "--update-baseline\n")
            for k in sorted(misses):
                f.write(k + "\n")
        print("baseline written: %s (%d benign MISS key(s))"
              % (baseline_path, len(misses)))
        return 0

    baseline = load_baseline(baseline_path)
    if baseline is None:
        print("FAIL  file_resolve_miss  baseline missing: %s "
              "(seed with --update-baseline)" % baseline_path)
        return 2

    unknown = sorted(misses - baseline)
    resolved_from_baseline = sorted(baseline - misses)

    if not args.quiet:
        verdict = "FAIL" if unknown else "PASS"
        print("%s  file_resolve_miss  %-8s  misses=%d known=%d unknown=%d "
              "smoke_ok=%s" % (verdict, args.mission, len(misses),
                               len(misses) - len(unknown), len(unknown),
                               smoke_ok))
        for k in unknown:
            print("  UNKNOWN MISS: %s" % k)
        if resolved_from_baseline and len(resolved_from_baseline) <= 10:
            for k in resolved_from_baseline:
                print("  (baseline key not seen this run: %s)" % k)

    if args.json:
        with open(args.json, "w") as jf:
            json.dump({"result": "FAIL" if unknown else "PASS",
                       "mission": args.mission, "smoke_ok": smoke_ok,
                       "miss_total": len(misses),
                       "unknown": unknown}, jf, indent=2)

    return 1 if unknown else 0


if __name__ == "__main__":
    sys.exit(main())
