#!/usr/bin/env python3
"""verify_executor_slice.py -- canonical executor-slice verification gauntlet.

Runs OFF/ON/dryrun smoke passes and asserts the gate-counter contracts from
debug_state/latest_render_state.json so every executor slice gets the same
verified proof without reinventing the gauntlet.

Usage (normal):
    py -3 scripts/verify_executor_slice.py

Usage (parse-only, no smoke -- validates JSON-path logic against an existing dump):
    py -3 scripts/verify_executor_slice.py --parse-only <path/to/latest_render_state.json>

Full example:
    py -3 scripts/verify_executor_slice.py \\
        --exe A:/Games/mc2-opengl/mc2-win64-0.4c/mc2.exe \\
        --mission mc2_01 --mission mc2_24 \\
        --expect-validated-top-level-min 100 \\
        --with-dryrun
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path
import os as _os, sys as _sys  # noqa: E401
_sys.path.append(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "..", "scripts"))
import console_child  # noqa: E402  (windowless console children without a conhost each; scripts/console_child.py)

# ---------------------------------------------------------------------------
# Gate-counter map (also used by run_smoke.py --require-gate).
# dotted path is relative to the "frame_graph" sub-object in the JSON dump.
# ---------------------------------------------------------------------------
GATE_COUNTER_MAP: dict[str, str] = {
    "MC2_FRAMEGRAPH_EXECUTOR": "executor_owned_passes",
    "MC2_FRAMEGRAPH_DRYRUN":   "frame_graph_dryrun.frames",
}

# Known ApplyPassId names that appear in frame_graph.executor_apply_state_by_pass
# (added by PER-PASS-APPLY-COUNTERS-1, commit 0e0b582a). Used to validate
# --assert-pass-fired NAME against typos before asserting the counter.
APPLY_PASS_NAMES: tuple[str, ...] = (
    "PostProcessEdgeFog",
    "PostProcessFogOob",
    "PostProcessShoreline",
    "PostProcessCloudShadow",
    "PostProcessScreenShadow",
    "TerrainDecal",
    "TerrainOverlay",
    "StaticPropOpaque",
    "MechOpaque",  # APPLY-STATE-MECHOPAQUE-1
    "Water",  # APPLY-STATE-WATER-1
    "Shadow",  # APPLY-STATE-SHADOW-1 (render-target MODE: pipeline + DepthForwardZ clear)
)

# Default deploy path for the 0.4c release folder.
_DEFAULT_EXE = "A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe"
_DEFAULT_MISSIONS = ["mc2_01", "mc2_24"]

# Executor counter assertions (gate-ON mode).
# Each entry: (json_path_under_frame_graph, assertion_callable, description)
_ExecutorAssertion = tuple[str, object, str]  # (dotted_path, assert_fn, label)


def _resolve_dotted(obj: dict, path: str) -> object:
    """Resolve a dotted path through nested dicts. Returns 0 on miss."""
    cur: object = obj
    for part in path.split("."):
        if isinstance(cur, dict):
            cur = cur.get(part, 0)
        else:
            return 0
    return cur


def parse_dump(dump_path: Path) -> dict:
    """Load and return latest_render_state.json, raise on error."""
    with open(dump_path, encoding="utf-8") as fh:
        return json.load(fh)


def extract_frame_graph(ds: dict) -> dict:
    """Return the frame_graph sub-object, or {} if absent."""
    return ds.get("frame_graph", {})


def read_counter(fg: dict, dotted_path: str) -> int:
    """Read a dotted counter from the frame_graph sub-object."""
    val = _resolve_dotted(fg, dotted_path)
    try:
        return int(val)
    except (TypeError, ValueError):
        return 0


def parse_pass_fired_spec(spec: str) -> tuple[str, int]:
    """Parse a --assert-pass-fired NAME[:MIN] spec into (name, min).

    Raises SystemExit with a clear message on an unknown pass name or a
    non-integer MIN. Default MIN is 1.
    """
    name, _, min_str = spec.partition(":")
    name = name.strip()
    if name not in APPLY_PASS_NAMES:
        valid = ", ".join(APPLY_PASS_NAMES)
        raise SystemExit(
            f"[verify_executor_slice] ERROR: unknown pass name {name!r} for "
            f"--assert-pass-fired.\n  Valid names: {valid}")
    if min_str.strip() == "":
        return name, 1
    try:
        return name, int(min_str.strip())
    except ValueError:
        raise SystemExit(
            f"[verify_executor_slice] ERROR: --assert-pass-fired MIN must be an "
            f"integer, got {min_str!r} (in {spec!r})")


def assert_passes_fired(
    fg: dict, specs: list[tuple[str, int]]
) -> tuple[list[tuple[str, str, str]], bool]:
    """Assert executor_apply_state_by_pass[NAME] >= MIN for each spec.

    Returns (rows, all_ok). Missing key or value < MIN -> FAIL.
    """
    by_pass = fg.get("executor_apply_state_by_pass")
    rows: list[tuple[str, str, str]] = []
    all_ok = True
    if not isinstance(by_pass, dict):
        for name, mn in specs:
            rows.append((f"assert_pass_fired:{name}", "FAIL",
                         "executor_apply_state_by_pass map absent from dump "
                         "(needs PER-PASS-APPLY-COUNTERS-1 build)"))
            all_ok = False
        return rows, all_ok
    for name, mn in specs:
        if name not in by_pass:
            rows.append((f"assert_pass_fired:{name}", "FAIL",
                         f"key missing from executor_apply_state_by_pass "
                         f"(expected >= {mn})"))
            all_ok = False
            continue
        try:
            val = int(by_pass[name])
        except (TypeError, ValueError):
            val = 0
        ok = val >= mn
        rows.append((f"assert_pass_fired:{name}",
                     "PASS" if ok else "FAIL",
                     f"={val} (min={mn})"))
        if not ok:
            all_ok = False
    return rows, all_ok


def print_gate_table(rows: list[tuple[str, str, str]]) -> None:
    """Print a formatted PASS/FAIL/WARN gate table to stdout."""
    w = max((len(r[0]) for r in rows), default=30)
    print()
    print(f"{'Gate/Check':<{w}}  {'Status':<6}  Detail")
    print("-" * (w + 40))
    for name, status, detail in rows:
        print(f"{name:<{w}}  {status:<6}  {detail}")
    print()


def _run_smoke(
    exe: str,
    missions: list[str],
    extra_env: dict[str, str],
    require_gates: list[str],
    smoke_script: Path,
) -> tuple[int, str]:
    """Run run_smoke.py as a subprocess. Returns (returncode, combined stderr)."""
    cmd = [
        sys.executable, str(smoke_script),
        "--exe", exe,
        "--keep-logs",
        "--duration", "30",
    ]
    for m in missions:
        cmd += ["--mission", m]
    for g in require_gates:
        cmd += ["--require-gate", g]

    env = dict(os.environ)
    env.update(extra_env)

    result = subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        env=env,
        **console_child.child_kwargs(redirected=True),
    )
    return result.returncode, result.stderr


def check_off_pass(fg: dict, verbose: bool = True) -> list[tuple[str, str, str]]:
    """Assert all executor_* counters == 0 (gate-OFF, byte-identical)."""
    rows: list[tuple[str, str, str]] = []
    off_counters = [
        "executor_owned_passes",
        "executor_validation_failures",
        "executor_owned_wrappers",
        "executor_validated_top_level_passes",
        "executor_apply_state_passes",
        "executor_scheduled_passes",
    ]
    for ctr in off_counters:
        val = read_counter(fg, ctr)
        ok = val == 0
        rows.append((f"OFF:{ctr}", "PASS" if ok else "FAIL",
                     f"={val}" + ("" if ok else " (expected 0)")))
    dryrun_frames = read_counter(fg, "frame_graph_dryrun.frames")
    rows.append(("OFF:frame_graph_dryrun.frames", "PASS" if dryrun_frames == 0 else "FAIL",
                 f"={dryrun_frames}" + ("" if dryrun_frames == 0 else " (expected 0)")))
    return rows


def check_on_pass(fg: dict, expect_validated_min: int | None) -> list[tuple[str, str, str]]:
    """Assert executor ON counters meet contract."""
    rows: list[tuple[str, str, str]] = []

    failures = read_counter(fg, "executor_validation_failures")
    rows.append(("ON:executor_validation_failures",
                 "PASS" if failures == 0 else "FAIL",
                 f"={failures}" + ("" if failures == 0 else " (expected 0)")))

    wrappers = read_counter(fg, "executor_owned_wrappers")
    rows.append(("ON:executor_owned_wrappers",
                 "PASS" if wrappers > 0 else "FAIL",
                 f"={wrappers}" + ("" if wrappers > 0 else " (expected >0)")))

    apply_state = read_counter(fg, "executor_apply_state_passes")
    rows.append(("ON:executor_apply_state_passes", "INFO", f"={apply_state}"))

    scheduled = read_counter(fg, "executor_scheduled_passes")
    rows.append(("ON:executor_scheduled_passes",
                 "PASS" if scheduled == 0 else "WARN",
                 f"={scheduled}" + ("" if scheduled == 0 else " (expected 0 on default path)")))

    validated = read_counter(fg, "executor_validated_top_level_passes")
    if expect_validated_min is not None:
        ok = validated >= expect_validated_min
        rows.append(("ON:executor_validated_top_level_passes",
                     "PASS" if ok else "FAIL",
                     f"={validated} (min={expect_validated_min})"))
    else:
        ok = validated > 0
        rows.append(("ON:executor_validated_top_level_passes",
                     "PASS" if ok else "FAIL",
                     f"={validated}" + ("" if ok else " (expected >0)")))
    return rows


def check_dryrun_pass(fg: dict) -> list[tuple[str, str, str]]:
    """Assert dryrun ON counters meet contract."""
    rows: list[tuple[str, str, str]] = []
    dr = fg.get("frame_graph_dryrun", {})

    ooo = int(dr.get("out_of_order", 0))
    rows.append(("DRYRUN:out_of_order",
                 "PASS" if ooo == 0 else "FAIL",
                 f"={ooo}" + ("" if ooo == 0 else " (expected 0)")))

    mismatches = read_counter(fg, "ambient_probe_mismatches")
    rows.append(("DRYRUN:ambient_probe_mismatches",
                 "PASS" if mismatches == 0 else "FAIL",
                 f"={mismatches}" + ("" if mismatches == 0 else " (expected 0)")))

    fbo_mm = read_counter(fg, "fbo_mismatches")
    rows.append(("DRYRUN:fbo_mismatches",
                 "PASS" if fbo_mm == 0 else "FAIL",
                 f"={fbo_mm}" + ("" if fbo_mm == 0 else " (expected 0)")))

    frames = int(dr.get("frames", 0))
    rows.append(("DRYRUN:frames", "PASS" if frames > 0 else "FAIL",
                 f"={frames}" + ("" if frames > 0 else " (expected >0)")))
    return rows


def _read_manifest_src_commit(manifest: Path) -> str | None:
    """Read the src_commit from .deployed_manifest.csv (manifest v1).

    deploy_payload.py writes src_commit as a PER-ROW column (index 3) of every
    data row, NOT as a top-level key. Header is:
        relpath,sha256,bytes,src_commit,timestamp
    All data rows carry the same commit; we take the first data row's value and
    assert the remaining rows are uniform. Returns None if not found.
    """
    import csv

    with open(manifest, encoding="utf-8", newline="") as fh:
        reader = csv.reader(fh)
        seen: set[str] = set()
        for row in reader:
            # Skip the manifest_version line and the column-header line.
            if not row or row[0] in ("manifest_version", "relpath"):
                continue
            if len(row) >= 4 and row[3].strip():
                seen.add(row[3].strip())
    if not seen:
        return None
    if len(seen) > 1:
        # Non-uniform src_commit -> mixed/partial deploy. Surface it.
        return "MIXED:" + ",".join(sorted(c[:12] for c in seen))
    return next(iter(seen))


def check_deploy_current(exe: str) -> tuple[str, str, str]:
    """Check deployed exe src_commit matches git HEAD. Returns (name, status, detail)."""
    try:
        exe_dir = Path(exe).resolve().parent
        manifest = exe_dir / ".deployed_manifest.csv"
        if not manifest.exists():
            return ("stale_deploy_check", "WARN",
                    f".deployed_manifest.csv not found at {exe_dir}")

        src_commit = _read_manifest_src_commit(manifest)
        if not src_commit:
            return ("stale_deploy_check", "WARN",
                    "src_commit not found in .deployed_manifest.csv")
        if src_commit.startswith("MIXED:"):
            return ("stale_deploy_check", "WARN",
                    f"non-uniform src_commit across manifest rows ({src_commit[6:]}) "
                    "- mixed/partial deploy")

        result = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            capture_output=True, text=True,
            **console_child.child_kwargs(redirected=True),
        )
        head = result.stdout.strip()
        if not head:
            return ("stale_deploy_check", "WARN", "git rev-parse HEAD failed")

        if src_commit == head:
            return ("stale_deploy_check", "PASS",
                    f"deployed={src_commit[:12]} == HEAD")
        else:
            return ("stale_deploy_check", "WARN",
                    f"deployed={src_commit[:12]} != HEAD={head[:12]} (stale deploy?)")
    except Exception as exc:
        return ("stale_deploy_check", "WARN", f"check skipped: {exc}")


def parse_only_mode(
    dump_path: Path,
    pass_fired_specs: list[tuple[str, int]] | None = None,
) -> int:
    """--parse-only: validate gate-parsing logic against an existing dump. Returns exit code."""
    print(f"[verify_executor_slice] --parse-only: reading {dump_path}")
    ds = parse_dump(dump_path)
    fg = extract_frame_graph(ds)

    rows: list[tuple[str, str, str]] = []
    # Print all executor/dryrun/ambient/fbo metrics
    report_keys = [
        "executor_owned_passes",
        "executor_validation_failures",
        "executor_owned_wrappers",
        "executor_validated_top_level_passes",
        "executor_apply_state_passes",
        "executor_scheduled_passes",
        "executor_skipped_deferred_passes",
        "ambient_probe_mismatches",
        "ambient_probe_samples",
        "fbo_mismatches",
        "fbo_samples",
    ]
    for k in report_keys:
        v = read_counter(fg, k)
        rows.append((k, "INFO", str(v)))

    dr = fg.get("frame_graph_dryrun", {})
    for k, v in dr.items():
        rows.append((f"frame_graph_dryrun.{k}", "INFO", str(v)))

    # Also validate gate→counter map resolves
    rows.append(("--- gate proof-counter resolution ---", "", ""))
    for gate, path in GATE_COUNTER_MAP.items():
        val = read_counter(fg, path)
        rows.append((f"{gate} -> frame_graph.{path}", "INFO", str(val)))

    # Also surface the per-pass apply-state map.
    by_pass = fg.get("executor_apply_state_by_pass")
    if isinstance(by_pass, dict):
        rows.append(("--- executor_apply_state_by_pass ---", "", ""))
        for k in sorted(by_pass):
            rows.append((f"apply_state_by_pass.{k}", "INFO", str(by_pass[k])))

    exit_code = 0
    if pass_fired_specs:
        rows.append(("--- --assert-pass-fired ---", "", ""))
        assert_rows, all_ok = assert_passes_fired(fg, pass_fired_specs)
        rows.extend(assert_rows)
        if not all_ok:
            exit_code = 1

    print_gate_table(rows)
    print("[verify_executor_slice] --parse-only complete (no smoke run)")
    if pass_fired_specs:
        print(f"[verify_executor_slice] --assert-pass-fired: "
              f"{'PASS' if exit_code == 0 else 'FAIL'}")
    return exit_code


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Canonical executor-slice verification gauntlet "
                    "(OFF/ON/dryrun smoke + stale-deploy check).")
    ap.add_argument("--exe", default=_DEFAULT_EXE,
                    help=f"Path to mc2.exe (default: {_DEFAULT_EXE})")
    ap.add_argument("--mission", action="append", default=[],
                    metavar="STEM",
                    help="Mission stem(s) to run (repeatable; default: mc2_01 mc2_24)")
    ap.add_argument("--expect-validated-top-level-min", type=int, default=None,
                    metavar="N",
                    help="Minimum executor_validated_top_level_passes in ON run")
    ap.add_argument("--with-dryrun", action="store_true",
                    help="Also run a dryrun ON smoke (MC2_FRAMEGRAPH_DRYRUN=1)")
    ap.add_argument("--parse-only", metavar="DUMP_JSON",
                    help="Skip smoke; just parse the given latest_render_state.json "
                         "and print executor metrics (for testing without a live run)")
    ap.add_argument("--assert-pass-fired", action="append", default=[],
                    metavar="NAME[:MIN]",
                    help="Assert frame_graph.executor_apply_state_by_pass[NAME] >= MIN "
                         "(default MIN=1). Repeatable. Reads the per-pass apply-state "
                         "map (PER-PASS-APPLY-COUNTERS-1). Valid NAMEs: "
                         + ", ".join(APPLY_PASS_NAMES))
    args = ap.parse_args()

    pass_fired_specs = [parse_pass_fired_spec(s) for s in args.assert_pass_fired]

    if args.parse_only:
        return parse_only_mode(Path(args.parse_only), pass_fired_specs)

    missions = args.mission or _DEFAULT_MISSIONS
    smoke_script = Path(__file__).resolve().parent / "run_smoke.py"
    exe = args.exe
    all_rows: list[tuple[str, str, str]] = []
    overall_pass = True

    # Stale-deploy check (before smoke to give early warning).
    deploy_row = check_deploy_current(exe)
    all_rows.append(deploy_row)
    if deploy_row[1] == "WARN":
        print(f"[verify_executor_slice] WARNING: {deploy_row[2]}", file=sys.stderr)

    exe_dir = Path(exe).resolve().parent
    dump_dir = exe_dir / "debug_state"
    dump_path = dump_dir / "latest_render_state.json"

    base_env: dict[str, str] = {
        "MC2_DEBUG_STATE_DUMP": "1",
        "MC2_DIAG_TAGS": "CONFIG,BUILD,DEVICE",
    }

    # -----------------------------------------------------------------------
    # Step 1: Smoke OFF (no executor gate) -- all executor counters must == 0
    # -----------------------------------------------------------------------
    print("[verify_executor_slice] Step 1: smoke OFF (no MC2_FRAMEGRAPH_EXECUTOR)")
    rc, stderr = _run_smoke(exe, missions, base_env, [], smoke_script)
    if rc != 0:
        print(f"[verify_executor_slice] FAIL: smoke OFF exited {rc}", file=sys.stderr)
        print(stderr, file=sys.stderr)
        all_rows.append(("smoke_OFF_exit", "FAIL", f"exit={rc}"))
        overall_pass = False
    else:
        all_rows.append(("smoke_OFF_exit", "PASS", "exit=0"))
        try:
            fg_off = extract_frame_graph(parse_dump(dump_path))
            off_rows = check_off_pass(fg_off)
            all_rows.extend(off_rows)
            if any(r[1] == "FAIL" for r in off_rows):
                overall_pass = False
        except Exception as exc:
            all_rows.append(("smoke_OFF_dump", "FAIL", f"cannot read dump: {exc}"))
            overall_pass = False

    # -----------------------------------------------------------------------
    # Step 2: Smoke ON (MC2_FRAMEGRAPH_EXECUTOR=1)
    # -----------------------------------------------------------------------
    print("[verify_executor_slice] Step 2: smoke ON (MC2_FRAMEGRAPH_EXECUTOR=1)")
    # Hoisted to outer scope so Step 4 (--assert-pass-fired) reads the executor-ON
    # frame_graph block rather than re-parsing dump_path, which a Step-3 dryrun run
    # would have overwritten with an executor-OFF dump (all-0 apply state -> false FAIL).
    fg_on = None
    on_env = {**base_env, "MC2_FRAMEGRAPH_EXECUTOR": "1"}
    rc, stderr = _run_smoke(exe, missions, on_env,
                            ["MC2_FRAMEGRAPH_EXECUTOR"], smoke_script)
    if rc != 0:
        print(f"[verify_executor_slice] FAIL: smoke ON exited {rc}", file=sys.stderr)
        print(stderr, file=sys.stderr)
        all_rows.append(("smoke_ON_exit", "FAIL", f"exit={rc}"))
        overall_pass = False
    else:
        all_rows.append(("smoke_ON_exit", "PASS", "exit=0"))
        try:
            fg_on = extract_frame_graph(parse_dump(dump_path))
            on_rows = check_on_pass(fg_on, args.expect_validated_top_level_min)
            all_rows.extend(on_rows)
            if any(r[1] == "FAIL" for r in on_rows):
                overall_pass = False
        except Exception as exc:
            all_rows.append(("smoke_ON_dump", "FAIL", f"cannot read dump: {exc}"))
            overall_pass = False

    # -----------------------------------------------------------------------
    # Step 3: (optional) Smoke dryrun ON (MC2_FRAMEGRAPH_DRYRUN=1)
    # -----------------------------------------------------------------------
    if args.with_dryrun:
        print("[verify_executor_slice] Step 3: smoke DRYRUN (MC2_FRAMEGRAPH_DRYRUN=1)")
        dr_env = {**base_env, "MC2_FRAMEGRAPH_DRYRUN": "1"}
        rc, stderr = _run_smoke(exe, missions, dr_env,
                                ["MC2_FRAMEGRAPH_DRYRUN"], smoke_script)
        if rc != 0:
            print(f"[verify_executor_slice] FAIL: smoke DRYRUN exited {rc}",
                  file=sys.stderr)
            print(stderr, file=sys.stderr)
            all_rows.append(("smoke_DRYRUN_exit", "FAIL", f"exit={rc}"))
            overall_pass = False
        else:
            all_rows.append(("smoke_DRYRUN_exit", "PASS", "exit=0"))
            try:
                fg_dr = extract_frame_graph(parse_dump(dump_path))
                dr_rows = check_dryrun_pass(fg_dr)
                all_rows.extend(dr_rows)
                if any(r[1] == "FAIL" for r in dr_rows):
                    overall_pass = False
            except Exception as exc:
                all_rows.append(("smoke_DRYRUN_dump", "FAIL",
                                 f"cannot read dump: {exc}"))
                overall_pass = False

    # -----------------------------------------------------------------------
    # Step 4: (optional) --assert-pass-fired against the latest dump.
    # The last smoke run with the executor gate ON populates
    # executor_apply_state_by_pass; assert the requested passes fired.
    # -----------------------------------------------------------------------
    if pass_fired_specs:
        print("[verify_executor_slice] Step 4: --assert-pass-fired "
              "(executor_apply_state_by_pass)")
        # Use the Step-2 executor-ON frame_graph block (fg_on). Do NOT re-parse
        # dump_path: a Step-3 --with-dryrun run overwrites it with an executor-OFF
        # dump whose executor_apply_state_by_pass is all-0, which would falsely FAIL.
        if fg_on is None:
            all_rows.append(("assert_pass_fired_dump", "FAIL",
                             "no executor-ON dump (Step 2 did not produce one)"))
            overall_pass = False
        else:
            assert_rows, all_ok = assert_passes_fired(fg_on, pass_fired_specs)
            all_rows.extend(assert_rows)
            if not all_ok:
                overall_pass = False

    # -----------------------------------------------------------------------
    # Summary table
    # -----------------------------------------------------------------------
    print_gate_table(all_rows)
    verdict = "PASS" if overall_pass else "FAIL"
    print(f"[verify_executor_slice] Overall: {verdict}")
    return 0 if overall_pass else 1


if __name__ == "__main__":
    sys.exit(main())
