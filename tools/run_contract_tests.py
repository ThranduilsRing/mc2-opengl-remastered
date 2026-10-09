#!/usr/bin/env python3
"""SUBSYSTEM-HARNESS-ARC / HARNESS-TEMPLATE-1 — contract-harness runner.

Discovers registered subsystem contract harnesses, runs each, and reports an
aggregate PASS/FAIL. Harnesses are tiny standalone executables (see
docs/testing/subsystem-harness-arc-1.md) that force exact edge cases without
launching the game. This runner is the cheap "first proof" layer; tier1 smoke
remains the final integration gate.

Each registered harness must support the uniform CLI:
    --list / --test <name> / --json / --seed <n>
and return exit 0 on PASS, nonzero on failure.

Usage:
    py -3 tools/run_contract_tests.py
    py -3 tools/run_contract_tests.py --json
    py -3 tools/run_contract_tests.py --build-dir build64-contract --config RelWithDebInfo

Exit 0 iff every discovered harness reports PASS.
"""

import argparse
import json
import os
import subprocess
import sys
import time

# Explicit registries (NOT discovery — explicit beats clever; never auto-scan
# build dirs for stray/experimental harnesses).
#
# Native/exe harnesses: executable basename (no extension), found across build dirs.
REGISTERED_HARNESSES = [
    "contract_smoke_harness",
    "shader_contract_harness",
    "render_state_contract_harness",
    "objmgr_contract_harness",
    "ibl_registry_contract_harness",
    "render_pass_table_harness",
    "logistics_csv_harness",
    "camera_frustum_harness",
    "mech_texname_harness",
    "icon_atlas_harness",
    "timing_math_harness",
    "job_system_harness",
    "terrain_daylight_harness",
    "object_light_row_harness",
    # TREE-CONSOLIDATE-1 (2026-07-31): registered late. This is the harness
    # behind the "17 of 24 missions light the wall facing away from the sun"
    # measurement and the sign-flip guard on the SH-IBL probe rotation -- 70
    # tests of pure arithmetic -- and it shipped UNREGISTERED while both of its
    # same-day siblings directly above were registered. It was therefore never
    # run by tools/run_contract_tests.py and never built by
    # tools/build_contract_harnesses.py: a dark check, the third of that class
    # found on the day the class was closed twice. Verified before registering:
    # the binary exits 0 with "PASS (70 tests, 0 failures)".
    "class_light_band_harness",
    "glstate_guard_harness",
]

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Python harnesses: repo-relative path to the .py, invoked via `py -3 <path>`.
# Used when the contract's source of truth is Python (e.g. deploy_payload.py),
# where a C++ harness would be fake-green by construction.
PY_HARNESSES = [
    "tools/deploy_asset_contract_harness/deploy_asset_contract_harness.py",
    "tools/deploy_release_tree_contract_harness/deploy_release_tree_contract_harness.py",
    "tools/mech_glb_pack_contract_harness/mech_glb_pack_contract_harness.py",
    "tools/shader_variant_matrix_harness/shader_variant_matrix_harness.py",
]


def find_exe(name, build_dirs, config):
    """Locate a harness exe across candidate standalone build dirs.

    HARNESS-STALE-EXE-1: this used to return the FIRST existing candidate in a
    hardcoded dir list. That list named the old per-harness dirs
    (build64-shader, build64-ibl, ...) but NOT `build64-h-<name>`, which is
    where the canonical builder `tools/build_contract_harnesses.py` puts its
    output. On a tree that has both, the runner silently graded a months-old
    binary: shader_contract_harness and ibl_registry_contract_harness were
    reported FAIL here while the current build of the same harness exits 0.
    That is the same defect as the smoke runner's stale-exe trap -- a verdict
    on an artifact nobody identified -- only it fails green work instead of
    passing broken work.

    Two changes: the canonical `build64-h-<name>` dir is searched FIRST, and
    among existing candidates the NEWEST wins. Returns (path, shadowed) where
    `shadowed` lists older copies that were passed over, so the caller can say
    out loud which binary it graded.
    """
    exe = name + (".exe" if os.name == "nt" else "")
    candidates = []
    # Canonical per-harness dir written by tools/build_contract_harnesses.py.
    for bd in [os.path.join(REPO_ROOT, "build64-h-" + name)] + list(build_dirs):
        base = bd if os.path.isabs(bd) else os.path.join(REPO_ROOT, bd)
        candidates.append(os.path.join(base, config, exe))   # VS multi-config
        candidates.append(os.path.join(base, exe))            # single-config

    found = []
    seen = set()
    for c in candidates:
        real = os.path.normcase(os.path.abspath(c))
        if real in seen or not os.path.isfile(c):
            continue
        seen.add(real)
        found.append((os.path.getmtime(c), c))
    if not found:
        return None, []
    found.sort(key=lambda t: t[0], reverse=True)
    return found[0][1], [c for _, c in found[1:]]


def run_harness(cmd):
    """Run a harness command (list) with --json, return (ok, parsed_or_raw).
    Works for native exes ([exe]) and Python harnesses ([python, script])."""
    try:
        proc = subprocess.run(list(cmd) + ["--json"], capture_output=True,
                              text=True, timeout=60, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    except Exception as e:  # noqa: BLE001
        return False, {"error": str(e)}
    ok = proc.returncode == 0
    out = proc.stdout.strip()
    try:
        parsed = json.loads(out) if out else {"error": "no output"}
    except json.JSONDecodeError:
        parsed = {"error": "invalid json", "raw": out, "stderr": proc.stderr.strip()}
        ok = False
    # Reconcile: exit code is authoritative for pass/fail.
    if isinstance(parsed, dict) and parsed.get("status") == "FAIL":
        ok = False
    return ok, parsed


def main():
    ap = argparse.ArgumentParser(description="Run MC2 subsystem contract harnesses.")
    ap.add_argument("--build-dir", action="append", default=None,
                    help="Build dir(s) to search for harness exes (repeatable). "
                         "Default: build64-contract, build64-harness.")
    ap.add_argument("--config", default="RelWithDebInfo",
                    help="Multi-config build config subdir (default RelWithDebInfo).")
    ap.add_argument("--json", action="store_true", help="Emit aggregate JSON.")
    args = ap.parse_args()

    build_dirs = args.build_dir or [
        "build64-contract", "build64-shader", "build64-renderstate",
        "build64-objmgr", "build64-ibl", "build64-iconatlas", "build64-rptable",
        "build64-logcsv", "build64-camfr", "build64-texname", "build64-harness",
        "build64-glstateguard", "build64-h-glstate_guard_harness"]

    results = []
    missing = []
    for name in REGISTERED_HARNESSES:
        exe, shadowed = find_exe(name, build_dirs, args.config)
        if exe is None:
            missing.append(name)
            results.append({"harness": name, "status": "MISSING"})
            continue
        # HARNESS-STALE-EXE-1: name the binary being graded. A verdict whose
        # artifact is unidentified is what let a months-old exe be reported as
        # this tree's result for weeks.
        built = time.strftime("%Y-%m-%d %H:%M", time.localtime(os.path.getmtime(exe)))
        if shadowed:
            print(f"  [exe] {name}: using {os.path.relpath(exe, REPO_ROOT)} "
                  f"(built {built}); ignored {len(shadowed)} older copy/copies: "
                  f"{', '.join(os.path.relpath(s, REPO_ROOT) for s in shadowed)}",
                  file=sys.stderr)
        ok, parsed = run_harness([exe])
        results.append({"harness": name, "status": "PASS" if ok else "FAIL",
                        "detail": parsed, "exe": os.path.relpath(exe, REPO_ROOT),
                        "exe_built": built})

    # Python harnesses: invoked via the current interpreter, source-of-truth lives
    # in Python (no build step, always present in a checkout).
    for rel in PY_HARNESSES:
        path = rel if os.path.isabs(rel) else os.path.join(REPO_ROOT, rel)
        name = os.path.splitext(os.path.basename(path))[0]
        if not os.path.isfile(path):
            missing.append(name)
            results.append({"harness": name, "status": "MISSING"})
            continue
        ok, parsed = run_harness([sys.executable, path])
        results.append({"harness": name, "status": "PASS" if ok else "FAIL",
                        "detail": parsed})

    fails = [r for r in results if r["status"] not in ("PASS",)]
    aggregate = "PASS" if not fails else "FAIL"

    if args.json:
        print(json.dumps({"status": aggregate, "harnesses": results}, indent=2))
    else:
        for r in results:
            print(f"  [{r['status']}] {r['harness']}")
        if missing:
            print(f"\nMISSING (build them first): {', '.join(missing)}")
        print(f"\nCONTRACT TESTS: {aggregate} "
              f"({len(results)} harness(es), {len(fails)} not-passing)")

    return 0 if aggregate == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
