#!/usr/bin/env python3
"""check-deploy-hygiene.py — deploy-tree hygiene gate (P1 provenance arc).

Root cause (2026-07-02 Truth-First arc, P0): a GENERATED DIAGNOSTIC sidecar
leaked into the shipped viewing release under data/missions/<m>.beauty/ via a
MANUAL copy (deploy_payload.py never walks .beauty/, so its completeness gate
could not catch it). Example: a 4-quadrant test control_map.png + *_preview.png
/ *_delta_heatmap.png / *_report.json diagnostics shipped in v0.5.0.

This gate walks a deployed tree and FAILS on files matching known
generated-diagnostic patterns — the offline-cook byproducts that are authoring
aids, never runtime assets. It catches manual copies that the payload manifest
cannot.

DELIBERATELY NOT flagged: control_map.png / overlay_v2.png / visual_height_4x.r32
/ shoreline_mask.png — these are LEGIT authored/baked sidecar FORMATS the engine
loads. A wrong-CONTENT file of a legit format (e.g. a test-pattern control_map)
is a provenance problem, not a pattern-matchable one; that is handled by
quarantine + canonical-content review, not here.

Usage:
    py -3 scripts/check-deploy-hygiene.py <target-dir> [--strict] [--json]

Exit codes:
    0  clean (or offenders present but --strict not set -> WARN only)
    2  offenders present AND --strict
    1  usage / IO error

An allowlist file <target>/.deploy-hygiene-allow (one repo-relative glob per
line, '#' comments) can waive specific known-good exceptions.
"""
import argparse
import fnmatch
import json
import os
import sys

# Generated-diagnostic filename patterns (case-insensitive fnmatch on basename).
# Every entry here is a byproduct of the offline terrain/beautify cooks that is
# an authoring aid only — it has no engine loader and no runtime effect.
DIAGNOSTIC_PATTERNS = [
    "*_preview.png",         # visual_height_preview / visual_damp_preview
    "*_delta_heatmap.png",   # visual_delta_heatmap
    "*_heatmap.png",         # any heatmap viz
    "*_report.json",         # visual_height_report and friends
    "*.preview.png",
]

ALLOW_FILE = ".deploy-hygiene-allow"


def load_allow(target_dir):
    path = os.path.join(target_dir, ALLOW_FILE)
    globs = []
    if os.path.isfile(path):
        with open(path, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith("#"):
                    globs.append(line.replace(os.sep, "/"))
    return globs


def is_diagnostic(basename):
    low = basename.lower()
    return any(fnmatch.fnmatch(low, pat) for pat in DIAGNOSTIC_PATTERNS)


def scan(target_dir, allow_globs):
    offenders = []
    for dirpath, dirs, files in os.walk(target_dir):
        # Skip the reversible-quarantine holding pen: it lives inside the target
        # by design (so a restore is a local move) but its contents are already
        # withdrawn from the shipping tree.
        dirs[:] = [d for d in dirs if not d.lower().startswith("_quarantine")]
        for fn in files:
            if not is_diagnostic(fn):
                continue
            abs_p = os.path.join(dirpath, fn)
            rel = os.path.relpath(abs_p, target_dir).replace(os.sep, "/")
            if any(fnmatch.fnmatch(rel, g) for g in allow_globs):
                continue
            offenders.append(rel)
    offenders.sort()
    return offenders


def main(argv):
    ap = argparse.ArgumentParser(description="Deploy-tree dev-artifact hygiene gate.")
    ap.add_argument("target", help="deployed install directory to scan")
    ap.add_argument("--strict", action="store_true",
                    help="exit 2 if any offender found (default: warn only)")
    ap.add_argument("--json", action="store_true", help="emit JSON result")
    args = ap.parse_args(argv)

    if not os.path.isdir(args.target):
        print(f"[check-deploy-hygiene] FAIL: not a directory: {args.target}",
              file=sys.stderr)
        return 1

    allow = load_allow(args.target)
    offenders = scan(args.target, allow)

    if args.json:
        print(json.dumps({
            "target": args.target,
            "offenders": offenders,
            "count": len(offenders),
            "strict": args.strict,
            "allow_globs": allow,
        }, indent=2))
    else:
        if offenders:
            tag = "FAIL" if args.strict else "WARN"
            print(f"[check-deploy-hygiene] {tag}: {len(offenders)} generated-"
                  f"diagnostic artifact(s) in deploy tree {args.target}:")
            for rel in offenders:
                print(f"    {rel}")
            print("  These are offline-cook byproducts, not runtime assets. "
                  "Remove them from the install (or waive via "
                  f"{ALLOW_FILE}).")
        else:
            print(f"[check-deploy-hygiene] OK: no generated-diagnostic "
                  f"artifacts under {args.target}")

    if offenders and args.strict:
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
