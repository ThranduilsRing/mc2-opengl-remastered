#!/usr/bin/env python3
"""run_tools_tests.py -- the `tools_tests` check-contracts row.

WHY THIS EXISTS
---------------
scripts/tests/ is run by the `scripts_tests` row, but ~60 more pytest modules
live next to the tools they test (tools/terrain_beautify, tools/unit_converter,
tools/blender_pipeline, tests/smoke, tests/terrain_gen, ...). Nothing ran them,
so they could rot green-by-omission (see the SCRIPTS-TESTS-WIRE-1 note in
check-contracts.sh). Owner ruling 2026-10-09: they are live, so run them.

WHAT IT DOES
------------
* Discovers `test_*.py` / `*_test.py` via `git ls-files` (committed state, so a
  stray untracked file cannot flap the row), minus scripts/tests/ (its own row)
  and EXCLUDE (already driven by another row).
* Runs each module in its OWN pytest process (several modules share a basename
  such as test_manifest.py, which collides in one process without __init__.py),
  up to JOBS at a time. `-B -p no:cacheprovider` keep it read-only.
* Reconciles failures against scripts/tools_tests.known-failures, the same
  discipline as the repo's KNOWN rows:
    - a failure NOT in the ledger            -> FAIL (regression)
    - a ledgered failure that now PASSES      -> FAIL (stale ledger: delete line)
    - a ledger entry whose module is gone     -> FAIL (stale ledger)
  Ledger lines (`#` comments and blanks ignored):
    FAIL <nodeid-or-module> :: <first line of the failure>
  A module-level entry pins a collection error or a failing script-style test.
* Script-style modules (plain `python file.py` tests; pytest exits 5 with
  nothing collected) are run as `py -3 file.py` and must exit 0.
* Exit 0 = all green; 3 = every failure is pinned (the row renders KNOWN, not
  PASS); 1 = regression or stale ledger.

Pure stdlib. Read-only.
"""
from __future__ import annotations

import argparse
import concurrent.futures as cf
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import console_child  # noqa: E402

# Every spawn captures its output, so child_kwargs(redirected=True) adds the
# no-window flag only when this process has no console (no conhost per child).

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LEDGER = os.path.join(ROOT, "scripts", "tools_tests.known-failures")

# Modules driven by a different check-contracts row; running them again here
# would only double the cost.
EXCLUDE = {
    "tools/cxx17_ratchet/test_cxx17_ratchet.py",  # row cxx17_ratchet_selftest
    # Assembles release zips and launches a game smoke: needs artifacts + a
    # user-present session, never part of an offline contracts row.
    "scripts/test_release_zip_install.py",
}

PYTEST = [sys.executable, "-B", "-m", "pytest", "-q", "-p", "no:cacheprovider",
          "--tb=no", "-rfE"]
TIMEOUT_S = 600


def discover() -> list[str]:
    out = subprocess.run(
        ["git", "ls-files", "*test_*.py", "*_test.py"],
        cwd=ROOT, capture_output=True, text=True, check=True,
        **console_child.child_kwargs(redirected=True)).stdout
    mods = []
    for p in out.splitlines():
        base = os.path.basename(p)
        if not (base.startswith("test_") or base.endswith("_test.py")):
            continue
        if p.startswith("scripts/tests/") or p in EXCLUDE:
            continue
        if not os.path.isfile(os.path.join(ROOT, p)):
            continue  # deleted in the working tree, not yet committed
        mods.append(p)
    return sorted(mods)


def load_ledger() -> dict[str, str]:
    fails: dict[str, str] = {}
    if not os.path.isfile(LEDGER):
        return fails
    with open(LEDGER, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            m = re.match(r"FAIL (\S+) :: (.+)$", line)
            if not m:
                raise SystemExit(f"{LEDGER}:{n}: malformed ledger line: {line!r}")
            fails[m.group(1)] = m.group(2)
    return fails


def run_module(mod: str) -> tuple[str, int, list[str], str]:
    """Run one module; retry ONCE when it failed without naming a test.

    A non-zero exit with no FAILED/ERROR node id is an infrastructure hiccup
    (measured: a transient WinError 2 during collection while ~16 contract rows
    ran in parallel), not a test verdict. A real test failure is deterministic
    and is never retried.
    """
    res = _run_module_once(mod)
    if res[1] != 0 and not res[2]:
        res = _run_module_once(mod)
    return res


def _run_module_once(mod: str) -> tuple[str, int, list[str], str]:
    """Return (module, rc, failed node ids, tail of output)."""
    try:
        p = subprocess.run(PYTEST + [mod], cwd=ROOT, capture_output=True,
                           text=True, timeout=TIMEOUT_S,
                           **console_child.child_kwargs(redirected=True))
        text, rc = p.stdout + p.stderr, p.returncode
        if rc == 5:  # nothing collected: script-style test, run it directly
            p = subprocess.run([sys.executable, "-B", mod], cwd=ROOT,
                               capture_output=True, text=True,
                               timeout=TIMEOUT_S,
                               **console_child.child_kwargs(redirected=True))
            text, rc = p.stdout + p.stderr, p.returncode
            if rc != 0:  # report as a module-level failure
                text += f"\nFAILED {mod} - script exited {rc}"
    except subprocess.TimeoutExpired:
        return mod, 124, [], f"timed out after {TIMEOUT_S}s"
    failed = []
    for line in text.splitlines():
        m = re.match(r"(?:FAILED|ERROR) (\S+)", line)
        if m:
            failed.append(m.group(1))
    return mod, rc, failed, "\n".join(text.splitlines()[-8:])


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--jobs", type=int,
                    default=min(8, os.cpu_count() or 4))
    ap.add_argument("--list", action="store_true", help="print modules and exit")
    args = ap.parse_args()

    mods = discover()
    if args.list:
        print("\n".join(mods))
        return 0
    if not mods:
        print("VIOLATION: tools_tests discovered zero modules (row would pass on an empty set)")
        return 1

    led_fail = load_ledger()
    bad: list[str] = []
    seen_fail: set[str] = set()
    passed = 0

    with cf.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        for mod, rc, failed, tail in ex.map(run_module, mods):
            if rc == 0:
                passed += 1
                continue
            if not failed:  # timeout / crash / interrupted with no FAILED line
                bad.append(f"{mod}: pytest rc={rc} with no FAILED/ERROR lines\n{tail}")
                continue
            for node in failed:
                if node in led_fail:
                    seen_fail.add(node)
                else:
                    bad.append(f"{node}: NEW failure (not in the ledger)")

    all_mods = set(mods)
    for node in led_fail:
        if node not in seen_fail:
            mod = node.split("::")[0]
            why = ("module no longer exists" if mod not in all_mods
                   else "now passes")
            bad.append(f"ledger entry stale ({why}): FAIL {node}")

    for b in bad:
        print("VIOLATION:", b)
    if bad:
        return 1
    print(f"tools_tests: {len(mods)} modules, {passed} fully green, "
          f"{len(seen_fail)} failing node(s) pinned in the ledger")
    if seen_fail:
        for n in sorted(seen_fail):
            print(f"KNOWN: {n} -- {led_fail[n]}")
        return 3
    return 0


if __name__ == "__main__":
    sys.exit(main())
