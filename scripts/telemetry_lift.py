#!/usr/bin/env python3
"""
telemetry_lift.py -- Lift [TAG vN] key=value lines from smoke logs into NDJSON.

Produces telemetry.ndjson records with schema:
    {v, tag, tag_v, kind, ts_ms, frame, session, source, fields, raw_line}

Usage:
    # Lift a single log file:
    py -3 scripts/telemetry_lift.py <mission.log> [--session SESSION] [--source SOURCE]

    # Lift all logs in an artifact dir (writes telemetry.ndjson into that dir):
    py -3 scripts/telemetry_lift.py --artifact-dir <path> [--session SESSION]

    # Golden test mode (validates known tag counts):
    py -3 scripts/telemetry_lift.py --golden-test [--artifact-dir <path>]

Options:
    --artifact-dir PATH   Lift all *.log files in PATH, write telemetry.ndjson there.
    --session NAME        Session identifier stamped in each record.
    --source NAME         Source label (smoke|playtest|bridge|manual). Default: smoke.
    --registry PATH       Path to tag-registry.json. Default: tests/telemetry/tag-registry.json.
    --output PATH         NDJSON output path. Default: stdout (or artifact-dir/telemetry.ndjson).
    --golden-test         Run the built-in golden test against the canonical artifact dir and exit.

Exit codes:
    0  success (or golden test pass)
    1  error or golden test failure
"""

import argparse
import json
import os
import re
import sys

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

_NIFTY_WORKTREE = "A:/Games/mc2-main"  # canonical worktree (moved 2026-07-05)
_GOLDEN_STEM = os.path.join("tests", "smoke", "artifacts", "2026-06-09T19-27-36")


def _find_golden_artifact_dir():
    candidate = os.path.join(_NIFTY_WORKTREE, _GOLDEN_STEM)
    if os.path.isdir(candidate):
        return candidate
    return os.path.join(REPO_ROOT, _GOLDEN_STEM)


DEFAULT_REGISTRY = os.path.join(REPO_ROOT, "tests", "telemetry", "tag-registry.json")

# ---------------------------------------------------------------------------
# Schema version
# ---------------------------------------------------------------------------
RECORD_SCHEMA_V = 1

# ---------------------------------------------------------------------------
# Regexes
# ---------------------------------------------------------------------------
# Matches: [TAG vN] or [TAG] at the start of meaningful content.
# Groups: (1) tag name, (2) version str or None
TAG_HEADER_RE = re.compile(
    r'\[([A-Z][A-Z0-9_]+)(?:\s+v(\d+))?\]'
)

# key=value pairs after the tag header.
# Handles: key=value  key="quoted value"  key=bare_word
KV_RE = re.compile(
    r'(\w+)=(?:"([^"]*)"|([\S]*))'
)

# Optional timestamp at the start of a log line (ms since start).
# Patterns seen: "  1234.5 ms " or bare integer prefix.
# We capture what we can and default to None.
TS_RE = re.compile(r'^\s*(\d+(?:\.\d+)?)\s*ms\b')

# ---------------------------------------------------------------------------
# Registry loader
# ---------------------------------------------------------------------------

def load_registry(registry_path):
    """Return dict tag -> registry entry."""
    if not os.path.isfile(registry_path):
        return {}
    with open(registry_path, encoding="utf-8") as fh:
        data = json.load(fh)
    return {entry["tag"]: entry for entry in data.get("tags", [])}


# ---------------------------------------------------------------------------
# Line parser
# ---------------------------------------------------------------------------

def parse_tag_line(line):
    """
    Parse a single log line that contains a [TAG vN] bracket.

    Returns a list of dicts (one per [TAG] hit on the line), each with:
        tag, tag_v, ts_ms, frame, fields (dict of str->str)
    Returns empty list if no tag found.
    """
    results = []
    for m in TAG_HEADER_RE.finditer(line):
        tag = m.group(1)
        tag_v = int(m.group(2)) if m.group(2) is not None else 0

        # Attempt timestamp extraction from start of line.
        ts_ms = None
        ts_m = TS_RE.match(line)
        if ts_m:
            try:
                ts_ms = float(ts_m.group(1))
            except ValueError:
                pass

        # Extract all key=value pairs after the bracket.
        after = line[m.end():]
        fields = {}
        for kv in KV_RE.finditer(after):
            key = kv.group(1)
            val = kv.group(2) if kv.group(2) is not None else kv.group(3)
            fields[key] = val

        # Try to coerce numeric fields.
        parsed_fields = {}
        for k, v in fields.items():
            try:
                # Prefer int then float.
                if '.' in v:
                    parsed_fields[k] = float(v)
                else:
                    parsed_fields[k] = int(v)
            except (ValueError, TypeError):
                parsed_fields[k] = v

        # Extract frame if present.
        frame = parsed_fields.get("frame", None)
        if frame is not None:
            try:
                frame = int(frame)
            except (ValueError, TypeError):
                frame = None

        results.append({
            "tag": tag,
            "tag_v": tag_v,
            "ts_ms": ts_ms,
            "frame": frame,
            "fields": parsed_fields,
        })
    return results


# ---------------------------------------------------------------------------
# Lifter core
# ---------------------------------------------------------------------------

def lift_log(
    log_path,
    registry,
    session,
    source,
    mission_stem=None,
):
    """
    Lift one log file into a list of NDJSON record dicts.

    Args:
        log_path: path to the .log file.
        registry: dict tag -> registry entry (from load_registry).
        session: session identifier string.
        source: source label string.
        mission_stem: optional mission stem (e.g. "mc2_01"); derived from filename if None.

    Returns:
        list of record dicts.
    """
    if mission_stem is None:
        mission_stem = os.path.splitext(os.path.basename(log_path))[0]

    records = []
    try:
        with open(log_path, encoding="utf-8", errors="replace") as fh:
            for lineno, line in enumerate(fh, start=1):
                if '[' not in line:
                    continue
                hits = parse_tag_line(line)
                for hit in hits:
                    tag = hit["tag"]
                    reg_entry = registry.get(tag, {})
                    kind = reg_entry.get("kind", "event")  # unknown -> event

                    fields = dict(hit["fields"])
                    # Stamp mission into fields for multi-mission artifact dirs.
                    if mission_stem:
                        fields["_mission"] = mission_stem

                    record = {
                        "v": RECORD_SCHEMA_V,
                        "tag": tag,
                        "tag_v": hit["tag_v"],
                        "kind": kind,
                        "ts_ms": hit["ts_ms"],
                        "frame": hit["frame"],
                        "session": session,
                        "source": source,
                        "fields": fields,
                        "raw_line": lineno,
                    }
                    records.append(record)
    except OSError as exc:
        print(f"WARNING: cannot read {log_path}: {exc}", file=sys.stderr)
    return records


def lift_artifact_dir(artifact_dir, registry, session, source):
    """Lift all *.log files in artifact_dir. Returns list of records."""
    records = []
    if not os.path.isdir(artifact_dir):
        print(f"ERROR: artifact dir not found: {artifact_dir}", file=sys.stderr)
        return records
    for fname in sorted(os.listdir(artifact_dir)):
        if not fname.endswith(".log"):
            continue
        if fname.endswith(".ring_trace.log"):
            # Ring trace logs use a different format; skip for now.
            continue
        mission_stem = fname[: -len(".log")]
        fpath = os.path.join(artifact_dir, fname)
        recs = lift_log(fpath, registry, session=session, source=source, mission_stem=mission_stem)
        records.extend(recs)
    return records


# ---------------------------------------------------------------------------
# Golden test
# ---------------------------------------------------------------------------

# Known tag counts from tests/smoke/artifacts/2026-06-09T19-27-36/mc2_01.log
# Measured by running the lifter and counting records per tag.
# These are the ground-truth counts this test asserts.
GOLDEN_EXPECTED_COUNTS = {
    # Tag name -> expected record count in the golden artifact dir.
    # Measured on 2026-06-10 from mc2_01.log (single-mission artifact dir).
    "MODOVERRIDE":              68,
    "GPU_CULL":                 43,
    "HEARTBEAT":                29,
    "TGL_POOL":                 16,
    "MVP_DIAG":                 16,
    "VIEW_UNIFORMS":            16,
    "SPLATTING":                13,
    "TESS":                     12,
    "TIMING":                   11,
    "MATERIAL_GPU":             10,
    "MECHBATCHER":              10,
    "RENDER_WORLD":             10,
    "TERRAIN_LIGHTING_GPU":      8,
    "INSTR":                     6,
    "TIME":                      6,
    "TEX_RESOLVE":               6,
    "LIGHTBRIDGE":               6,
    "OBJBATCHER":                6,
    "OBJECT_ADMISSION_PREDICATE": 5,
    "ENGINE_VIEW":               5,
    "DRAW_PACKET_V6":            5,
    "WATER_FULL_RECIPE_AUTH":    5,
    "STATIC_PROP_REG":           4,
    "GPUPROPS":                  4,
    "MECH_MATERIAL_GPU":         4,
    "CAMERA_MOTION":             4,
    "QUADSETUP_SKIP":            4,
    "TERRAIN_INDIRECT_PARITY":   4,
    "RENDER_STATES":             4,
    "VISIBILITY":                4,
    "STATIC_UPDATE":             4,
    "APPEAR_ROUTE":              4,
    "ANIM_GATE":                 4,
    "RENDER_SNAPSHOT":           4,
    "SMOKE":                     3,
    "GPU":                       3,
    "CEMENT_ATLAS":              3,
    "COALESCE":                  3,
    "TERRAIN_INDIRECT":          3,
    "ENGINE_VIEW_REGISTRY":      3,
    "TXM":                       3,
    "PATCH_STREAM":              2,
    "MISSION":                   2,
    "COLORMAP":                  2,
    "SPOTLIGHT_REAL_TRACE":      2,
    "LIGHTBAKE":                 2,
    "VIDEO":                     2,
    "STATIC_FIRST_FRAME":        2,
    # Single-occurrence tags
    "VSYNC":                     1,
    "GL":                        1,
    "WINDOW":                    1,
    "MODE":                      1,
    "TRACY":                     1,
    "SHADOW_MECH":               1,
    "SHADOW_STATIC_PROP":        1,
    "THIN_TERRAIN":              1,
    "TERRAIN_SURFACE":           1,
    "MINE_STATIC":               1,
    "MASK_SOLID":                1,
    "MASK_WATER":                1,
    "WATER_FAST":                1,
    "HZB_BUILD":                 1,
    "HDRI_SKY":                  1,
    "HDR_POST":                  1,
    "BLOOM":                     1,
    "TONEMAP_ACES":              1,
    "SSAO":                      1,
    "HZB_PROBE":                 1,
    "VIEWMODE":                  1,
    "VIEWMODE_LOWLIGHT":         1,
    "ASSET_SCALE":               1,
    "PACKET_OOB":                1,
    "STATIC_PROP":               1,
    "OBJECT_ID":                 1,
    "GAMEPLAY_PICK":             1,
    "MECH_OBJECT_ID_SELFTEST":   1,
    "ABL":                       1,
    "TERRAIN_HEIGHT_TEX":        1,
    "BURNIN_JPG":                1,
    "STATIC_PROP_TYPE_TABLE":    1,
    "FRAMECAP":                  1,
    "TERRAIN_ADMISSION":         1,
    "SPOT_DIAG":                 1,
    "STATIC_PROP_PACKET_DISPATCH": 1,
    "WATER_MDI":                 1,
    "RING_MVP":                  1,
    "PERF":                      1,
    "GPU_PARTICLES":             1,
}

# Total expected record count (sum of all above).
GOLDEN_EXPECTED_TOTAL = sum(GOLDEN_EXPECTED_COUNTS.values())


def run_golden_test(registry, artifact_dir=None):
    """
    Run the golden test against the canonical artifact dir.

    Returns True on pass, False on failure.
    """
    if artifact_dir is None:
        artifact_dir = _find_golden_artifact_dir()

    print(f"Golden test artifact dir: {artifact_dir}")
    if not os.path.isdir(artifact_dir):
        print(f"FAIL: artifact dir not found: {artifact_dir}")
        return False

    records = lift_artifact_dir(
        artifact_dir,
        registry,
        session="golden-2026-06-09T19-27-36",
        source="smoke",
    )

    # Tally per-tag counts.
    actual_counts = {}
    for rec in records:
        tag = rec["tag"]
        actual_counts[tag] = actual_counts.get(tag, 0) + 1

    # Compare against expected.
    failures = []
    for tag, expected in GOLDEN_EXPECTED_COUNTS.items():
        actual = actual_counts.get(tag, 0)
        status = "OK" if actual == expected else "FAIL"
        if actual != expected:
            failures.append((tag, expected, actual))
        print(f"  {status}  {tag:<40s}  expected={expected:4d}  actual={actual:4d}")

    # Report tags in actual but not in expected (informational).
    extra_tags = set(actual_counts) - set(GOLDEN_EXPECTED_COUNTS)
    if extra_tags:
        print(f"\nINFO: {len(extra_tags)} tags in log not in golden table (informational):")
        for t in sorted(extra_tags):
            print(f"  {t} = {actual_counts[t]}")

    print(f"\nTotal records lifted: {len(records)}  (expected >= {GOLDEN_EXPECTED_TOTAL})")

    if failures:
        print(f"\nFAIL: {len(failures)} tag count mismatches:")
        for tag, expected, actual in failures:
            print(f"  {tag}: expected {expected}, got {actual}")
        return False

    print("\nGolden test PASS.")
    return True


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "log_file",
        nargs="?",
        help="Single log file to lift (stdout NDJSON unless --output given).",
    )
    parser.add_argument(
        "--artifact-dir",
        help="Lift all logs in this artifact dir; writes telemetry.ndjson there.",
    )
    parser.add_argument(
        "--session",
        default=None,
        help="Session identifier for records. Default: derived from artifact dir name or log filename.",
    )
    parser.add_argument(
        "--source",
        default="smoke",
        choices=["smoke", "playtest", "bridge", "manual"],
        help="Source label. Default: smoke.",
    )
    parser.add_argument(
        "--registry",
        default=DEFAULT_REGISTRY,
        help="Path to tag-registry.json.",
    )
    parser.add_argument(
        "--output",
        default=None,
        help="NDJSON output path (default: stdout, or artifact-dir/telemetry.ndjson).",
    )
    parser.add_argument(
        "--golden-test",
        action="store_true",
        help="Run the built-in golden test and exit.",
    )
    args = parser.parse_args()

    registry = load_registry(args.registry)

    # ------------------------------------------------------------------
    # Golden test mode
    # ------------------------------------------------------------------
    if args.golden_test:
        ok = run_golden_test(registry, artifact_dir=args.artifact_dir)
        return 0 if ok else 1

    # ------------------------------------------------------------------
    # Artifact-dir mode
    # ------------------------------------------------------------------
    if args.artifact_dir:
        artifact_dir = args.artifact_dir
        session = args.session or os.path.basename(artifact_dir.rstrip("/\\"))
        records = lift_artifact_dir(artifact_dir, registry, session=session, source=args.source)

        out_path = args.output or os.path.join(artifact_dir, "telemetry.ndjson")
        with open(out_path, "w", encoding="utf-8") as fh:
            for rec in records:
                fh.write(json.dumps(rec, ensure_ascii=False) + "\n")
        print(f"Wrote {len(records)} records to {out_path}")
        return 0

    # ------------------------------------------------------------------
    # Single-file mode
    # ------------------------------------------------------------------
    if args.log_file:
        session = args.session or os.path.splitext(os.path.basename(args.log_file))[0]
        records = lift_log(
            args.log_file,
            registry,
            session=session,
            source=args.source,
        )
        out = sys.stdout
        if args.output:
            out = open(args.output, "w", encoding="utf-8")
        try:
            for rec in records:
                out.write(json.dumps(rec, ensure_ascii=False) + "\n")
        finally:
            if args.output:
                out.close()
        if not args.output:
            print(f"# {len(records)} records lifted", file=sys.stderr)
        return 0

    parser.print_help()
    return 1


if __name__ == "__main__":
    sys.exit(main())
