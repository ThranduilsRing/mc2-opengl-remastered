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
    """Locate a harness exe across candidate standalone build dirs."""
    exe = name + (".exe" if os.name == "nt" else "")
    candidates = []
    for bd in build_dirs:
        base = bd if os.path.isabs(bd) else os.path.join(REPO_ROOT, bd)
        candidates.append(os.path.join(base, config, exe))   # VS multi-config
        candidates.append(os.path.join(base, exe))            # single-config
    for c in candidates:
        if os.path.isfile(c):
            return c
    return None


def run_harness(cmd):
    """Run a harness command (list) with --json, return (ok, parsed_or_raw).
    Works for native exes ([exe]) and Python harnesses ([python, script])."""
    try:
        proc = subprocess.run(list(cmd) + ["--json"], capture_output=True,
                              text=True, timeout=60)
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
        "build64-logcsv", "build64-camfr", "build64-texname", "build64-harness"]

    results = []
    missing = []
    for name in REGISTERED_HARNESSES:
        exe = find_exe(name, build_dirs, args.config)
        if exe is None:
            missing.append(name)
            results.append({"harness": name, "status": "MISSING"})
            continue
        ok, parsed = run_harness([exe])
        results.append({"harness": name, "status": "PASS" if ok else "FAIL",
                        "detail": parsed})

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
