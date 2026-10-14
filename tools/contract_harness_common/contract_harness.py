"""SUBSYSTEM-HARNESS-ARC — Python harness framework.

Shared contract for the retained Python suites:
uniform CLI (--list / --test NAME / --json / --seed N), exit 0 = all selected
tests pass / 1 = failure / 2 = usage. tools/build_contract_harnesses.py runs
these suites alongside the native doctest corpus and trusts their exit codes:

    {"harness": "...", "tests": [{"name": "...", "status": "PASS", "ms": 1}],
     "status": "PASS", "elapsed_ms": 1}

STDOUT/STDERR CONTRACT: the framework owns stdout (report / JSON). Tests MUST
write any human diagnostics to stderr (print(..., file=sys.stderr)); writing to
stdout corrupts --json.

Usage (a harness .py):
    from contract_harness import Harness, Ctx
    def test_foo(t: Ctx) -> bool:
        t.check(1 + 1 == 2, "math")
        return True
    h = Harness("my_harness")
    h.add("foo", test_foo)
    h.add("demo_fail", test_demo, in_default=False)  # only via --test
    raise SystemExit(h.run())

This is intentionally minimal — it is NOT a second test framework, just the
shared CLI/JSON/exit-code contract for Python harnesses.
"""

import argparse
import json
import sys
import time


class Ctx:
    """Per-test context. A test fails by returning False or recording a failure
    via check()/fail(). Randomized tests should seed off self.seed."""

    def __init__(self, seed=0):
        self.seed = seed
        self.failures = 0
        self.first_failure = ""

    def fail(self, msg):
        self.failures += 1
        if not self.first_failure:
            self.first_failure = str(msg)

    def check(self, cond, msg=""):
        """Record a failure if cond is falsy. Does not raise — the test keeps
        running so multiple checks report."""
        if not cond:
            self.fail(msg or "check failed")
        return bool(cond)


class Harness:
    def __init__(self, name):
        self.name = name
        self._tests = []  # list of (name, fn, in_default)

    def add(self, test_name, fn, in_default=True):
        """in_default=False registers a test that runs ONLY when named via
        --test (e.g. an intentional-failure demo). It still appears in --list."""
        self._tests.append((test_name, fn, in_default))

    def run(self, argv=None):
        ap = argparse.ArgumentParser(prog=self.name, add_help=True)
        ap.add_argument("--list", action="store_true")
        ap.add_argument("--test", default=None)
        ap.add_argument("--json", action="store_true")
        ap.add_argument("--seed", type=int, default=0)
        args = ap.parse_args(argv)

        if args.list:
            self._emit_list(args.json)
            return 0

        only = args.test
        results = []
        ran_any = False
        for tname, fn, in_default in self._tests:
            if only is None:
                if not in_default:
                    continue
            elif tname != only:
                continue
            ran_any = True
            results.append(self._run_one(tname, fn, args.seed))

        if only is not None and not ran_any:
            print(f"ERROR: no test named '{only}'", file=sys.stderr)
            return 2

        return self._emit_results(results, args.json)

    # -- internals --

    def _run_one(self, tname, fn, seed):
        ctx = Ctx(seed)
        t0 = time.perf_counter()
        ret = False
        try:
            ret = fn(ctx)
        except Exception as e:  # noqa: BLE001
            ctx.fail(f"exception: {e!r}")
        ms = int(round((time.perf_counter() - t0) * 1000))
        passed = bool(ret) and ctx.failures == 0
        msg = ""
        if not passed:
            msg = ctx.first_failure or (
                "test returned false (no detail)" if ret else "test returned false")
        return {"name": tname, "passed": passed, "ms": ms, "message": msg}

    def _emit_list(self, as_json):
        names = [t[0] for t in self._tests]
        if as_json:
            print(json.dumps({"harness": self.name, "tests": names}))
        else:
            print(f"{self.name} tests:")
            for n in names:
                print(f"  {n}")

    def _emit_results(self, results, as_json):
        fails = sum(1 for r in results if not r["passed"])
        total = sum(r["ms"] for r in results)
        status = "FAIL" if fails else "PASS"
        if as_json:
            tests = []
            for r in results:
                entry = {"name": r["name"],
                         "status": "PASS" if r["passed"] else "FAIL",
                         "ms": r["ms"]}
                if not r["passed"]:
                    entry["message"] = r["message"]
                tests.append(entry)
            print(json.dumps({"harness": self.name, "tests": tests,
                              "status": status, "elapsed_ms": total}))
        else:
            for r in results:
                tail = "" if r["passed"] else f" - {r['message']}"
                print(f"  [{'PASS' if r['passed'] else 'FAIL'}] "
                      f"{r['name']} ({r['ms']} ms){tail}")
            n = len(results)
            print(f"{self.name}: {status} ({n} test{'' if n == 1 else 's'}, "
                  f"{fails} failure{'' if fails == 1 else 's'}, {total} ms)")
        return 1 if fails else 0
