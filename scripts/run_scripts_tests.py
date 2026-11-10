#!/usr/bin/env python3
"""run_scripts_tests.py -- the `scripts_tests` check-contracts row (pytest over scripts/tests/).

Each test module runs in its own pytest process, using the host JOBS limit, through the per-module result
cache of scripts/module_test_cache.py: a module that fully passed last time and whose recorded inputs (files read,
directories listed) are unchanged is not run again. See that file for exactly what is recorded and what is not.
tests/ for the tools live in run_tools_tests.py (the `tools_tests` row).

  py -3 scripts/run_scripts_tests.py [--no-cache] [--report] [--jobs N] [--list]

Exit 0 = every module green (or cached green); 1 = any failure (the failing module is re-run with --tb=short and its
tail printed). CONTRACTS_NO_CACHE=1 is the same as --no-cache.
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import console_child  # noqa: E402
import run_tools_tests as rtt  # noqa: E402

TESTS_REL = "scripts/tests"
OWNERSHIP_REL = "scripts/test_ownership.json"


def discover() -> list[str]:
    d = os.path.join(rtt.ROOT, "scripts", "tests")
    out = []
    for n in sorted(os.listdir(d)):
        if (n.startswith("test_") and n.endswith(".py")) or n.endswith("_test.py"):
            out.append(f"{TESTS_REL}/{n}")
    return out


def load_ownership(path: str | None = None) -> dict:
    path = path or os.path.join(rtt.ROOT, OWNERSHIP_REL)
    with open(path, encoding="utf-8") as stream:
        data = json.load(stream)
    if not isinstance(data, dict) or not isinstance(data.get("aggregate_owner"), str) \
            or not isinstance(data.get("tests"), list):
        raise ValueError(f"{path}: expected aggregate_owner and tests")
    return data


def ownership_problems(modules: list[str], data: dict) -> list[str]:
    """Every discovered module appears once and has exactly one executor."""
    issues = []
    aggregate = data["aggregate_owner"]
    owners_by_module: dict[str, list[str]] = {}
    for entry in data["tests"]:
        module, owners = entry.get("module"), entry.get("owners")
        if not isinstance(module, str) or not isinstance(owners, list) \
                or not all(isinstance(owner, str) for owner in owners):
            issues.append(f"invalid ownership entry: {entry!r}")
            continue
        if module in owners_by_module:
            issues.append(f"duplicate ownership entry: {module}")
            continue
        owners_by_module[module] = owners
        if not owners:
            issues.append(f"zero owners: {module}")
        if len(owners) != len(set(owners)):
            issues.append(f"duplicate owner: {module}")
        if aggregate in owners and len(owners) > 1:
            issues.append(f"duplicate execution: {module} (aggregate and direct owners)")
    # A discovered module with no entry is owned by the aggregate (scripts_tests): a new test file needs no row.
    for module in owners_by_module:
        if module not in modules:
            issues.append(f"stale ownership entry: {module}")
    return issues


def select_modules(modules: list[str], data: dict, requested: list[str] | None = None) -> list[str]:
    """Named modules run directly; otherwise only aggregate-owned modules run."""
    if requested:
        # a module is named by its file stem ("test_x"), file name ("test_x.py") or path ("scripts/tests/test_x.py")
        wanted = {}
        for name in requested:
            stem = name.replace("\\", "/").rsplit("/", 1)[-1]
            wanted[name] = f"{TESTS_REL}/{stem if stem.endswith('.py') else stem + '.py'}"
        unknown = sorted(name for name, module in wanted.items() if module not in modules)
        if unknown:
            raise ValueError("unknown test module: " + ", ".join(unknown))
        return [module for module in modules if module in set(wanted.values())]
    aggregate = data["aggregate_owner"]
    owners = {entry["module"]: entry["owners"] for entry in data["tests"]}
    return [module for module in modules if owners.get(module, [aggregate]) == [aggregate]]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--jobs", type=int, default=int(os.environ.get("MC2_TEST_JOBS", "") or rtt.default_jobs()))
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--no-cache", action="store_true")
    ap.add_argument("--report", action="store_true")
    ap.add_argument("modules", nargs="*", help="run these modules directly, regardless of aggregate ownership")
    args = ap.parse_args()
    mods = discover()
    try:
        ownership = load_ownership()
        issues = ownership_problems(mods, ownership)
        selection = select_modules(mods, ownership, args.modules)
    except (OSError, ValueError) as error:
        print(f"FAIL: {OWNERSHIP_REL}: {error}")
        return 1
    if issues:
        for issue in issues:
            print(f"FAIL: ownership: {issue}")
        return 1
    if args.list:
        print("\n".join(mods))
        return 0
    if not selection:
        print("FAIL: scripts_tests selected zero modules")
        return 1
    bad = 0
    passed = 0
    for mod, rc, failed, tail in rtt.run_all(selection, args.jobs, "scripts", args.no_cache, args.report):
        if rc == 0:
            passed += 1
            continue
        bad += 1
        print(f"FAIL: {mod} (rc={rc}) {' '.join(failed[:5])}")
        print(tail)  # preserve the original verdict even if a diagnostic rerun passes
        if rc == 124:
            continue  # a bounded timeout is already a verdict, not a reason to run the hang again
        p = subprocess.run([sys.executable, "-B", "-m", "pytest", "-q", "-p", "no:cacheprovider", "--tb=short", mod],
                           cwd=rtt.ROOT, capture_output=True, text=True, timeout=rtt.TIMEOUT_S,
                           **console_child.child_kwargs(redirected=True))
        print("\n".join((p.stdout + p.stderr).splitlines()[-60:]))
    print(f"scripts_tests: {len(selection)} modules, {passed} green, {bad} failing")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
