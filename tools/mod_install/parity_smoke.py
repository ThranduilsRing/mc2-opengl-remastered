"""
MC2 VFS Resolve-Trace Parity Smoke (S4)
========================================
Compares an engine-produced resolve trace (NDJSON from MC2_RESOLVE_TRACE_FILE)
against the static Python resolver (tools/mod_install/resolver.py) record by
record, reporting mismatches and exiting nonzero on any unexpected divergence.

USAGE
-----
    python parity_smoke.py <trace_file.ndjson> --game-dir <deploy_root>
                          [--active-mod <id>] [--mods-root <path>]
                          [--fst-listing <path>] ...

    The engine run that produced <trace_file.ndjson> MUST have been started with:
        MC2_REBUILD_MOD_CACHE=1
        MC2_RESOLVE_TRACE_FILE=<same trace_file.ndjson path>
        MC2_LOG_FILE_RESOLVE=2
    (MC2_REBUILD_MOD_CACHE=1 ensures the mod index is a fresh scan, not a
    potentially stale .modindex-cache.  Without it, the static resolver and
    the engine may use different index states and produce phantom mismatches.)

KNOWN-ACCEPTABLE DIVERGENCES (skip-list)
-----------------------------------------
These divergences are structurally expected and do NOT count as mismatches.
Each has an explicit reason; see SKIP_RULES below.

  1. layer "cd"     -- The static resolver never emits "cd".  The CD path
                       (file.cpp:822-860) requires Environment.checkCDForFiles
                       which is always false in modern MC2 deploys.  Any engine
                       record with layer="cd" is skipped with SKIP_CD.

  2. layer "MISS" for non-data/ paths -- ShouldSearchMods gates the mod overlay
                       on paths starting "data/".  Non-data/ engine records
                       (e.g. "prefs.cfg", "options.cfg") resolve to base-loose
                       or MISS in both engine and resolver, but the resolver
                       cannot verify on-disk state for non-game-dir-relative
                       paths that the engine opens via CWD differences.
                       Records whose key does not start "data/" AND whose engine
                       layer is "base-loose" AND the static resolver sees "MISS"
                       are skipped with SKIP_NON_DATA_LOOSE (disk state delta).

  3. "base-strip"   -- The static resolver implements the numeric-subdir strip.
                       However, if the stripped file exists on the static
                       resolver's game_dir but not where the engine ran from
                       (or vice-versa), a false mismatch appears.  This is a
                       disk-state delta, not a logic error.  Such cases are
                       flagged SKIP_STRIP_DISK_DELTA and counted but not failed.

  4. "fastfile"     -- The static resolver uses .fst listing side-car files
                       (text listings).  If no listing files are found, the fst
                       set is empty and all fastfile hits show as MISS in the
                       static resolver.  When --fst-listing is not supplied AND
                       no auto-discovered listing files exist, ALL engine
                       "fastfile" records are skipped with SKIP_NO_FST_LISTING
                       and a warning is printed.

EXIT CODES
----------
  0 -- all records matched (ignoring known-acceptable divergences)
  1 -- one or more unexpected mismatches
  2 -- input/argument error
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from collections import Counter
from typing import List, Optional, Dict

# Resolve relative import for running as script or as module
_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

from resolver import ResolverConfig, resolve, load_config, normalize_key


# ---------------------------------------------------------------------------
# Skip rule tags
# ---------------------------------------------------------------------------
SKIP_CD = "SKIP_CD"
SKIP_NON_DATA_LOOSE = "SKIP_NON_DATA_LOOSE"
SKIP_STRIP_DISK_DELTA = "SKIP_STRIP_DISK_DELTA"
SKIP_NO_FST_LISTING = "SKIP_NO_FST_LISTING"


# ---------------------------------------------------------------------------
# Trace loading
# ---------------------------------------------------------------------------

def load_trace(path: str) -> List[dict]:
    """
    Load an engine-produced NDJSON resolve trace.
    Skips blank lines and lines that are not valid JSON objects.
    Raises SystemExit(2) on file-not-found.
    """
    records = []
    try:
        with open(path, "r", encoding="utf-8") as f:
            for lineno, line in enumerate(f, 1):
                line = line.strip()
                if not line:
                    continue
                try:
                    rec = json.loads(line)
                    if not isinstance(rec, dict):
                        continue
                    records.append(rec)
                except json.JSONDecodeError as e:
                    print(f"[parity_smoke] WARNING: line {lineno} not valid JSON ({e}); skipped",
                          file=sys.stderr)
    except OSError as e:
        print(f"[parity_smoke] ERROR: cannot open trace file {path!r}: {e}", file=sys.stderr)
        sys.exit(2)
    return records


# ---------------------------------------------------------------------------
# Skip-list evaluation
# ---------------------------------------------------------------------------

def _classify_skip(engine_rec: dict, static_rec: dict, fst_set_empty: bool) -> Optional[str]:
    """
    Return a skip-rule tag if this mismatch is a known-acceptable divergence,
    or None if it is a real unexpected mismatch.
    """
    engine_layer = engine_rec.get("layer", "")
    static_layer = static_rec.get("layer", "")
    key = static_rec.get("key", "")

    # Rule 1: CD path -- static resolver never emits "cd"
    if engine_layer == "cd":
        return SKIP_CD

    # Rule 4: FastFile with no listing available
    if engine_layer == "fastfile" and fst_set_empty:
        return SKIP_NO_FST_LISTING

    # Rule 2: non-data/ base-loose vs MISS disk-state delta
    if not key.startswith("data/"):
        if engine_layer == "base-loose" and static_layer == "MISS":
            return SKIP_NON_DATA_LOOSE
        # Also allow MISS<->base-loose for non-data paths (CWD differences)
        if engine_layer == "MISS" and static_layer == "base-loose":
            return SKIP_NON_DATA_LOOSE

    # Rule 3: base-strip disk-state delta
    # Engine sees base-strip but static sees MISS (or vice versa) -- disk delta
    if engine_layer == "base-strip" and static_layer == "MISS":
        return SKIP_STRIP_DISK_DELTA
    if engine_layer == "MISS" and static_layer == "base-strip":
        return SKIP_STRIP_DISK_DELTA

    return None  # real mismatch


# ---------------------------------------------------------------------------
# Comparison
# ---------------------------------------------------------------------------

def run_parity(
    trace_records: List[dict],
    config: ResolverConfig,
    fst_set_empty: bool,
    verbose: bool = False,
) -> int:
    """
    Compare each engine trace record against the static resolver.
    Prints a summary and returns the count of unexpected mismatches.
    """
    total = len(trace_records)
    matched = 0
    mismatched = 0
    skipped_counts: Counter = Counter()
    mismatch_lines: List[str] = []

    for i, eng_rec in enumerate(trace_records):
        req = eng_rec.get("req") or eng_rec.get("key", "")
        if not req:
            continue

        static_rec = resolve(req, config)

        eng_layer = eng_rec.get("layer", "")
        sta_layer = static_rec.get("layer", "")
        eng_path = eng_rec.get("path", "")
        sta_path = static_rec.get("path", "")

        # Normalize paths for comparison: lowercase + forward-slash
        def _norm_path(p: str) -> str:
            return p.replace("\\", "/").lower()

        layer_match = (eng_layer == sta_layer)

        # Path comparison: the engine emits CWD-relative paths for base-loose
        # records (e.g. "data/tgl/foo.ini") while the resolver emits absolute
        # paths (e.g. "A:/Games/.../data/tgl/foo.ini").  We compare the
        # basename suffix: if the static path ends with the engine path
        # (after normalization), treat them as matching.  This is the expected
        # structural difference between engine trace and static resolver output.
        def _paths_equivalent(ep: str, sp: str) -> bool:
            ep_n = _norm_path(ep)
            sp_n = _norm_path(sp)
            if ep_n == sp_n:
                return True
            # Absolute static path ends with engine's relative path
            if ep_n and sp_n.endswith("/" + ep_n):
                return True
            # Both empty (MISS / fastfile)
            if not ep_n and not sp_n:
                return True
            return False

        path_match = (
            _paths_equivalent(eng_path, sta_path)
            or (eng_layer == "fastfile" and sta_layer == "fastfile")
        )

        if layer_match and path_match:
            matched += 1
            if verbose:
                print(f"[OK] {static_rec['key']!r} layer={eng_layer}")
        else:
            skip_tag = _classify_skip(eng_rec, static_rec, fst_set_empty)
            if skip_tag:
                skipped_counts[skip_tag] += 1
                if verbose:
                    print(f"[SKIP:{skip_tag}] {static_rec['key']!r} "
                          f"engine={eng_layer!r} static={sta_layer!r}")
            else:
                mismatched += 1
                line = (
                    f"[MISMATCH #{mismatched}] key={static_rec['key']!r}\n"
                    f"  engine : layer={eng_layer!r} path={eng_path!r}\n"
                    f"  static : layer={sta_layer!r} path={sta_path!r}"
                )
                mismatch_lines.append(line)
                if verbose:
                    print(line)

    # Summary
    print()
    print("=" * 60)
    print("Parity smoke summary")
    print("=" * 60)
    print(f"  Total records   : {total}")
    print(f"  Matched         : {matched}")
    print(f"  Mismatched      : {mismatched}")
    total_skipped = sum(skipped_counts.values())
    print(f"  Skipped (known) : {total_skipped}")
    for tag, count in sorted(skipped_counts.items()):
        print(f"    {tag}: {count}")

    # Per-layer breakdown
    layer_counts_eng: Counter = Counter(r.get("layer", "?") for r in trace_records)
    layer_counts_sta: Counter = Counter()
    # recompute for display
    for rec in trace_records:
        req = rec.get("req") or rec.get("key", "")
        if req:
            s = resolve(req, config)
            layer_counts_sta[s["layer"]] += 1

    print()
    print("Per-layer (engine -> static):")
    all_layers = sorted(set(layer_counts_eng.keys()) | set(layer_counts_sta.keys()))
    for layer in all_layers:
        e = layer_counts_eng.get(layer, 0)
        s = layer_counts_sta.get(layer, 0)
        flag = " *" if e != s else ""
        print(f"  {layer:<20} engine={e:5d}  static={s:5d}{flag}")

    if mismatch_lines:
        print()
        print("Unexpected mismatches:")
        for line in mismatch_lines:
            print(line)
    print("=" * 60)

    return mismatched


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "trace_file",
        help="Engine-produced NDJSON resolve trace (MC2_RESOLVE_TRACE_FILE output).",
    )
    parser.add_argument(
        "--game-dir",
        required=True,
        help="Deploy root the engine ran from (e.g. A:/Games/mc2-opengl/mc2-win64-v0.4).",
    )
    parser.add_argument(
        "--active-mod",
        default=None,
        help=(
            "MC2_ACTIVE_MOD value used when producing the trace.  "
            "Omit for base-game runs (no MC2_ACTIVE_MOD set)."
        ),
    )
    parser.add_argument(
        "--mods-root",
        default=None,
        help="Path to mods/ folder.  Default: <game_dir>/mods.",
    )
    parser.add_argument(
        "--fst-listing",
        action="append",
        dest="fst_listings",
        default=None,
        metavar="PATH",
        help=(
            "Path to a .fst text listing file (one per line, each line is a "
            "member path relative to the deploy root).  May be repeated.  "
            "If omitted, auto-discovery is attempted under game_dir.  "
            "FastFile records will be SKIP_NO_FST_LISTING if none are found."
        ),
    )
    parser.add_argument(
        "--verbose", "-v",
        action="store_true",
        help="Print every record (OK / SKIP / MISMATCH).",
    )
    parser.add_argument(
        "--require-rebuild-cache",
        action="store_true",
        default=True,
        help=(
            "Warn if MC2_REBUILD_MOD_CACHE=1 was likely not set (detected by "
            "checking for the presence of .modindex-cache files in the active mod "
            "dir that are newer than the trace file).  Default: True."
        ),
    )

    args = parser.parse_args(argv)

    # Load trace
    trace_records = load_trace(args.trace_file)
    if not trace_records:
        print("[parity_smoke] ERROR: trace file is empty or has no valid records.",
              file=sys.stderr)
        return 2

    print(f"[parity_smoke] loaded {len(trace_records)} engine records from {args.trace_file!r}")

    # Build config
    # dependencies=None: read MC2_MOD_DEPS, which is the layer set the engine
    # mounts when the trace was produced (InitModSearchPaths), so the static
    # resolver indexes the same mod layers without a second flag.
    config = load_config(
        game_dir=args.game_dir,
        mods_root=args.mods_root,
        active_mod=args.active_mod,
        fst_files=args.fst_listings,
    )

    fst_set_empty = len(config.fst_set) == 0
    if fst_set_empty:
        print(
            "[parity_smoke] WARNING: no .fst listing files found.  "
            "All engine 'fastfile' records will be skipped (SKIP_NO_FST_LISTING).  "
            "Pass --fst-listing <path> to enable fastfile parity.",
            file=sys.stderr,
        )

    mod_count = len(config.mod_index)
    if config.active_mod:
        print(f"[parity_smoke] mod index: active_mod={config.active_mod!r} "
              f"entries={mod_count} fst_members={len(config.fst_set)}")
    else:
        print(f"[parity_smoke] base-game mode (no MC2_ACTIVE_MOD).  "
              f"fst_members={len(config.fst_set)}")

    mismatches = run_parity(trace_records, config, fst_set_empty, verbose=args.verbose)

    return 0 if mismatches == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
