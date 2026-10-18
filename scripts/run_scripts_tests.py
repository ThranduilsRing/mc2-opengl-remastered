#!/usr/bin/env python3
"""run_scripts_tests.py -- the `scripts_tests` check-contracts row (pytest over scripts/tests/).

Each test module runs in its own pytest process, two at a time (the lane's 2-core cap), through the per-module result
cache of scripts/module_test_cache.py: a module that fully passed last time and whose recorded inputs (files read,
directories listed) are unchanged is not run again. See that file for exactly what is recorded and what is not.
tests/ for the tools live in run_tools_tests.py (the `tools_tests` row).

  py -3 scripts/run_scripts_tests.py [--no-cache] [--report] [--jobs N] [--list]

Exit 0 = every module green (or cached green); 1 = any failure (the failing module is re-run with --tb=short and its
tail printed). CONTRACTS_NO_CACHE=1 is the same as --no-cache.
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import console_child  # noqa: E402
import run_tools_tests as rtt  # noqa: E402

TESTS_REL = "scripts/tests"


def discover() -> list[str]:
    d = os.path.join(rtt.ROOT, "scripts", "tests")
    out = []
    for n in sorted(os.listdir(d)):
        if (n.startswith("test_") and n.endswith(".py")) or n.endswith("_test.py"):
            out.append(f"{TESTS_REL}/{n}")
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--jobs", type=int, default=int(os.environ.get("MC2_TEST_JOBS", "") or min(2, os.cpu_count() or 2)))
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--no-cache", action="store_true")
    ap.add_argument("--report", action="store_true")
    args = ap.parse_args()
    mods = discover()
    if args.list:
        print("\n".join(mods))
        return 0
    if not mods:
        print("FAIL: scripts_tests discovered zero modules")
        return 1
    bad = 0
    passed = 0
    for mod, rc, failed, tail in rtt.run_all(mods, args.jobs, "scripts", args.no_cache, args.report):
        if rc == 0:
            passed += 1
            continue
        bad += 1
        print(f"FAIL: {mod} (rc={rc}) {' '.join(failed[:5])}")
        p = subprocess.run([sys.executable, "-B", "-m", "pytest", "-q", "-p", "no:cacheprovider", "--tb=short", mod],
                           cwd=rtt.ROOT, capture_output=True, text=True, timeout=rtt.TIMEOUT_S,
                           **console_child.child_kwargs(redirected=True))
        print("\n".join((p.stdout + p.stderr).splitlines()[-60:]))
    print(f"scripts_tests: {len(mods)} modules, {passed} green, {bad} failing")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
