#!/usr/bin/env python3
"""
check-oracle-registry.py -- Drift check for tests/telemetry/tag-registry.json.

Fails (exit 1) if any [TAG ...] emitted in the golden artifact dir logs,
or any tag named in docs/oracle-dynamic-pipeline-gate.md, is absent from
the registry.

Usage:
    py -3 scripts/check-oracle-registry.py [--artifact-dir <path>]

Defaults:
    --artifact-dir  tests/smoke/artifacts/2026-06-09T19-27-36
    --registry      tests/telemetry/tag-registry.json
    --gate-doc      docs/oracle-dynamic-pipeline-gate.md

Exit codes:
    0  all tags in registry
    1  one or more missing tags; details printed to stdout
"""

import argparse
import json
import os
import re
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# The golden artifact dir may live in the nifty-mendeleev worktree (the
# canonical smoke-artifact store) rather than this worktree's tests/ tree.
# We probe both locations and fall back gracefully.
_NIFTY_WORKTREE = "A:/Games/mc2-main"  # canonical worktree (moved 2026-07-05)
_GOLDEN_STEM = os.path.join("tests", "smoke", "artifacts", "2026-06-09T19-27-36")

def _find_default_artifact_dir():
    # Prefer the nifty-mendeleev worktree (where smoke runs write artifacts).
    candidate_nifty = os.path.join(_NIFTY_WORKTREE, _GOLDEN_STEM)
    if os.path.isdir(candidate_nifty):
        return candidate_nifty
    return os.path.join(REPO_ROOT, _GOLDEN_STEM)

DEFAULT_ARTIFACT_DIR = _find_default_artifact_dir()
DEFAULT_REGISTRY = os.path.join(REPO_ROOT, "tests", "telemetry", "tag-registry.json")
DEFAULT_GATE_DOC = os.path.join(REPO_ROOT, "docs", "oracle-dynamic-pipeline-gate.md")

# Pattern: [TAG] or [TAG vN] -- tag is upper-snake-case starting with a letter
TAG_BRACKET_RE = re.compile(r'\[([A-Z][A-Z0-9_]+)(?:\s+v\d+)?\]')


def load_registry(registry_path):
    """Return set of known tag names from the registry."""
    with open(registry_path, encoding="utf-8") as fh:
        data = json.load(fh)
    return {entry["tag"] for entry in data.get("tags", [])}


def scan_logs(artifact_dir):
    """Return set of tag names found in *.log files under artifact_dir."""
    found = set()
    if not os.path.isdir(artifact_dir):
        print(f"WARNING: artifact dir not found: {artifact_dir}", file=sys.stderr)
        return found
    for fname in os.listdir(artifact_dir):
        if not fname.endswith(".log"):
            continue
        fpath = os.path.join(artifact_dir, fname)
        try:
            with open(fpath, encoding="utf-8", errors="replace") as fh:
                for line in fh:
                    for m in TAG_BRACKET_RE.finditer(line):
                        found.add(m.group(1))
        except OSError as exc:
            print(f"WARNING: cannot read {fpath}: {exc}", file=sys.stderr)
    return found


def scan_gate_doc(gate_doc_path):
    """
    Return set of tag names mentioned in the oracle gate doc.

    We look for [TAG ...] bracket patterns in the markdown source, plus
    backtick-quoted [TAG vN] strings common in code blocks.
    """
    found = set()
    if not os.path.isfile(gate_doc_path):
        print(f"WARNING: gate doc not found: {gate_doc_path}", file=sys.stderr)
        return found
    with open(gate_doc_path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            for m in TAG_BRACKET_RE.finditer(line):
                found.add(m.group(1))
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--artifact-dir",
        default=DEFAULT_ARTIFACT_DIR,
        help="Path to the golden artifact dir to scan (default: 2026-06-09T19-27-36)",
    )
    parser.add_argument(
        "--registry",
        default=DEFAULT_REGISTRY,
        help="Path to tag-registry.json",
    )
    parser.add_argument(
        "--gate-doc",
        default=DEFAULT_GATE_DOC,
        help="Path to oracle-dynamic-pipeline-gate.md",
    )
    # UNWIRED-CHECK-CLOSE-1 (2026-07-30). This check has TWO sources of
    # required tags: the gate doc, and the versioned tags found in a golden
    # artifact log dir. The artifact dir is a smoke-run output that is often
    # absent -- and when it is, scan_logs() printed a WARNING to stderr,
    # returned an empty set, and the run went on to report
    # "OK: all 8 required tags present" having exercised only HALF the check.
    # That is exactly a green light from a blind instrument: it passed today,
    # unwired, reporting "Tags found in logs: 0" on the line above its own OK.
    #
    # Silently degrading to half a check is the disease. Degrading is still
    # sometimes legitimate -- so it must be DECLARED AT THE CALL SITE with
    # this flag, never assumed inside.
    parser.add_argument(
        "--allow-missing-artifacts",
        action="store_true",
        help="Permit the artifact-log half of the check to be skipped when the "
             "artifact dir is absent (doc half still enforced). Without this, "
             "a missing artifact dir is a FAILURE, because a half-run check "
             "that reports a full PASS is worse than no check.",
    )
    args = parser.parse_args()

    # ------------------------------------------------------------------
    # 1. Load registry
    # ------------------------------------------------------------------
    if not os.path.isfile(args.registry):
        print(f"ERROR: registry not found: {args.registry}")
        return 1
    known = load_registry(args.registry)
    print(f"Registry: {len(known)} tags loaded from {args.registry}")

    # ------------------------------------------------------------------
    # 2. Collect required tags from artifact logs
    # ------------------------------------------------------------------
    artifacts_present = os.path.isdir(args.artifact_dir)
    if not artifacts_present and not args.allow_missing_artifacts:
        print(f"\nFAIL: artifact dir absent: {args.artifact_dir}", file=sys.stderr)
        print("  The artifact-log half of this check cannot run, so a PASS here "
              "would grade only the gate doc while claiming to grade both.",
              file=sys.stderr)
        print("  Point --artifact-dir at a real golden artifact dir, or pass "
              "--allow-missing-artifacts to declare the reduced scope "
              "explicitly.", file=sys.stderr)
        return 1

    from_logs = scan_logs(args.artifact_dir)
    print(f"Artifact dir: {args.artifact_dir}")
    print(f"  Tags found in logs: {len(from_logs)}  ({', '.join(sorted(from_logs))})")

    # ------------------------------------------------------------------
    # 3. Collect required tags from gate doc
    # ------------------------------------------------------------------
    from_doc = scan_gate_doc(args.gate_doc)
    print(f"Gate doc: {args.gate_doc}")
    print(f"  Tags found in doc:  {len(from_doc)}  ({', '.join(sorted(from_doc))})")

    # ------------------------------------------------------------------
    # 4. Tags that must be in the registry
    # ------------------------------------------------------------------
    # We require all oracle-gate rows (from doc) and all versioned tags
    # seen in the golden artifact logs.  Unversioned/infrastructure tags
    # (HEARTBEAT, MODOVERRIDE, etc.) from logs are informational only --
    # we warn but do not fail for tags whose names are purely lowercase
    # or that appear only once (noise avoidance).
    #
    # Hard required: anything in the gate doc + oracle kind entries in logs.
    required = set(from_doc)

    # Also require tags from logs that appear with a version number (v1+)
    # since those are intentionally structured emits.
    versioned_from_logs = set()
    if os.path.isdir(args.artifact_dir):
        versioned_re = re.compile(r'\[([A-Z][A-Z0-9_]+)\s+v([1-9]\d*)\]')
        for fname in os.listdir(args.artifact_dir):
            if not fname.endswith(".log"):
                continue
            fpath = os.path.join(args.artifact_dir, fname)
            try:
                with open(fpath, encoding="utf-8", errors="replace") as fh:
                    for line in fh:
                        for m in versioned_re.finditer(line):
                            versioned_from_logs.add(m.group(1))
            except OSError:
                pass
    required |= versioned_from_logs

    # ------------------------------------------------------------------
    # 5. Diff
    # ------------------------------------------------------------------
    missing = required - known
    unversioned_warn = from_logs - known - required

    if unversioned_warn:
        print(
            f"\nINFO: {len(unversioned_warn)} unversioned/informal tags in logs not in registry "
            f"(warning only, not a failure):"
        )
        for t in sorted(unversioned_warn):
            print(f"  {t}")

    if missing:
        print(f"\nFAIL: {len(missing)} required tag(s) absent from registry:")
        for t in sorted(missing):
            src = []
            if t in from_doc:
                src.append("gate-doc")
            if t in versioned_from_logs:
                src.append("artifact-log (versioned)")
            print(f"  {t}  (source: {', '.join(src)})")
        print(
            "\nAdd an entry for each missing tag to tests/telemetry/tag-registry.json."
        )
        return 1

    if not artifacts_present:
        print(f"\nOK (REDUCED SCOPE -- artifact-log half SKIPPED, dir absent: "
              f"{args.artifact_dir}): all {len(required)} doc-required tags "
              f"present in registry.")
    else:
        print(f"\nOK: all {len(required)} required tags present in registry "
              f"({len(from_logs)} tag(s) seen in artifact logs).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
