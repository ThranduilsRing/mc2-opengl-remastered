#!/usr/bin/env python3
"""check-test-tier-coverage.py -- DARK-TESTS-RUN-1 structural guard for tests/.

WHY THIS EXISTS
---------------
check-contract-coverage.py (UNWIRED-CHECK-CLOSE-1) made it structurally
impossible for a script in scripts/ to be unwired: every check-* must be named
in a check-contracts.sh row or allowlisted with a reason. Its own docstring
names the gap it does not cover:

    "It has no equivalent for tests/."

That gap was not theoretical. On 2026-07-31 this lane measured the tests/ tier:

  * 638 TEST_CASE declarations across tests/unit/, executed by NO gate.
  * tests/unit/test_rendercore.cpp pinned RendererFeature::COUNT == 46 while
    the header said 62 -- fifteen appends past an assertion never once
    evaluated, because no wired caller ever ran the binary.
  * test_render_contract_3 did not even LINK (LNK2019 x2, two apply-state
    forwarders whose _impl lives in gos_postprocess.cpp). It had been broken
    for as long as nobody built it. gos_counter_stubs.cpp's own header had
    PREDICTED this in prose -- "currently green by omission" -- and prose
    predicting a break is not a guard against one.
  * build64-tests/ existed in none of the 52 worktrees.

This file does NOT run the tests; running them needs a compiler and so belongs
to the execution tier (scripts/run-unit-tests.sh), per the reason recorded for
check-frame-graph.py (since deleted) in manual-checks.allowlist. What it does is make the
DARKNESS itself detectable by cheap static means, so the specific ways a test
silently stops counting all become loud:

  (a) ORPHAN SOURCE   -- a tests/unit/test_*.cpp in no build target. It
                         compiles nowhere, so it runs nowhere, and it looks
                         exactly like a test that passes.
  (b) UNDRIVEN TARGET -- an add_executable() in a tests/ CMakeLists that no
                         documented runner names. Building it is then a thing
                         only a person who already knows about it can do.
  (c) UNDRIVEN CTEST  -- an add_test() registration no runner drives. `ctest`
                         appears in no script in this repo, so a CTest-only
                         target is registered to nothing.
  (d) DISABLED RATCHET-- TEST_CASEs inside `#if 0` or marked doctest::skip.
                         This is the arm that matters most: the cheapest way
                         to make a newly-armed test green is to disable it,
                         and that edit is invisible in a pass/fail count. The
                         ratchet is TWO-WAY -- disabling more fails, and
                         re-enabling without lowering the pin also fails, so
                         the number can only be deliberate.

Pure stdlib. No build, no git, no network. Read-only.

Exit: 0 = tier is accounted for, 1 = a violation, 2 = harness error.
"""
from __future__ import annotations

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import checklib  # noqa: E402

EXIT_OK = checklib.EXIT_OK
EXIT_VIOLATION = checklib.EXIT_VIOLATION
EXIT_HARNESS_ERROR = checklib.EXIT_HARNESS_ERROR

ROOT = checklib.ROOT

# CMakeLists files that define test binaries.
TEST_CMAKES = [
    "tests/unit/CMakeLists.txt",
    "tests/model_override/CMakeLists.txt",
    "tests/static_natural_skip/CMakeLists.txt",
]

# Files that COUNT as documented runners: a target named in one of these is
# reachable by a command someone can run without reading CMake.
RUNNERS = [
    "scripts/run-unit-tests.sh",
    "tools/build_contract_harnesses.py",
]

# Targets built by a tests/ CMakeLists that are deliberately NOT test binaries.
# Each row states why, so the exemption is falsifiable (same contract as
# scripts/manual-checks.allowlist).
NON_TEST_TARGETS = {
    "mc2_modlint":
        "Ships as the offline modder CLI (tools/mc2_modlint/), not a test "
        "binary -- it has no assertions and no pass/fail contract. It is built "
        "from tests/unit/CMakeLists.txt only because it links the same GL-free "
        "validator TUs as mc2_tests. Its BEHAVIOUR is covered by mc2_tests' "
        "validator suites, which do assert.",
}

# --- arm (d) ratchet --------------------------------------------------------
# Deliberately-disabled doctest cases. Lower these when a case is re-enabled;
# the check FAILS if the real count drops below the pin, so debt cannot be
# silently re-added under a stale ceiling.
#
# Current debt, both in tests/unit/test_projection.cpp:
#   6 x `#if 0`          -- SCOPE-STOPPED projectZ/inverseProjectZ cases
#   1 x doctest::skip    -- the placeholder that stands in for them
DISABLED_IFZERO_PIN = 6
DISABLED_SKIP_PIN = 1

TEST_CASE_RE = re.compile(r"^[ \t]*(?:DOCTEST_)?TEST_CASE[ \t]*\(", re.M)
ADD_EXECUTABLE_RE = re.compile(r"\badd_executable\s*\(\s*([A-Za-z0-9_]+)")
ADD_TEST_NAME_RE = re.compile(r"\badd_test\s*\(\s*NAME\s+([A-Za-z0-9_]+)")


def _read(rel: str) -> str:
    return checklib.read_text(rel)


def _strip_cmake_comments(text: str) -> str:
    """Drop CMake # comments so a target named in prose cannot satisfy a check."""
    out = []
    for line in text.splitlines():
        # CMake has no string-embedded '#' in these files; a plain split is safe
        # here and deliberately conservative (it only ever removes text).
        out.append(line.split("#", 1)[0])
    return "\n".join(out)


def _count_disabled(text: str):
    """Return (ifzero_cases, skip_cases) for one doctest source.

    `#if 0` blocks are counted by walking the preprocessor nesting so a nested
    #if inside a disabled block cannot end it early.
    """
    ifzero = 0
    depth_stack = []       # True where that #if level is a disabling `#if 0`
    disabled_depth = None
    for line in text.splitlines():
        s = line.strip()
        if re.match(r"^#\s*if\b", s):
            is_zero = bool(re.match(r"^#\s*if\s+0\s*$", s))
            depth_stack.append(is_zero)
            if is_zero and disabled_depth is None:
                disabled_depth = len(depth_stack)
            continue
        if re.match(r"^#\s*endif\b", s):
            if depth_stack:
                popped_level = len(depth_stack)
                depth_stack.pop()
                if disabled_depth == popped_level:
                    disabled_depth = None
            continue
        if disabled_depth is not None and TEST_CASE_RE.match(line):
            ifzero += 1
    skips = len(re.findall(r"TEST_CASE\s*\([^)]*doctest::skip\s*\(\s*true\s*\)", text))
    return ifzero, skips


def run_all(quiet: bool = False, vfs: dict | None = None) -> int:
    problems: list[str] = []

    def read(rel: str) -> str:
        if vfs and rel in vfs:
            return vfs[rel]
        return _read(rel)

    # ---- gather -----------------------------------------------------------
    try:
        cmake_text = {rel: read(rel) for rel in TEST_CMAKES}
        runner_text = "\n".join(read(rel) for rel in RUNNERS)
    except OSError as e:
        print("check-test-tier-coverage: cannot read a required file: %s" % e,
              file=sys.stderr)
        return EXIT_HARNESS_ERROR

    unit_cmake = _strip_cmake_comments(cmake_text["tests/unit/CMakeLists.txt"])

    # ---- arm (a): orphan test sources -------------------------------------
    unit_dir = os.path.join(ROOT, "tests", "unit")
    if vfs is not None and "__unit_sources__" in vfs:
        sources = list(vfs["__unit_sources__"])
    else:
        sources = sorted(f for f in os.listdir(unit_dir)
                         if f.startswith("test_") and f.endswith(".cpp"))
    for src in sources:
        if src not in unit_cmake:
            n = "?"
            try:
                n = str(len(TEST_CASE_RE.findall(read("tests/unit/" + src))))
            except OSError:
                pass
            problems.append(
                "ORPHAN SOURCE: tests/unit/%s (%s TEST_CASEs) is in no target in "
                "tests/unit/CMakeLists.txt. It compiles nowhere, so it runs "
                "nowhere -- and a test that never runs is indistinguishable from "
                "one that passes." % (src, n))
    if not problems and not quiet:
        print("  ARM (a) OK  %d test source(s), none orphaned" % len(sources))

    # ---- arm (b): undriven executable targets -----------------------------
    targets = []
    for rel, text in cmake_text.items():
        targets += ADD_EXECUTABLE_RE.findall(_strip_cmake_comments(text))
    undriven = []
    for t in sorted(set(targets)):
        if t in NON_TEST_TARGETS:
            continue
        if t not in runner_text:
            undriven.append(t)
    for t in undriven:
        problems.append(
            "UNDRIVEN TARGET: '%s' is built by a tests/ CMakeLists but named by "
            "none of the documented runners (%s). Nobody can run it without "
            "reading CMake, which is how test_render_contract_3 sat unlinkable "
            "for weeks. Add it to a runner, or to NON_TEST_TARGETS with a reason."
            % (t, ", ".join(RUNNERS)))
    if not undriven and not quiet:
        print("  ARM (b) OK  %d test target(s) driven, %d exempt"
              % (len(set(targets)) - len(NON_TEST_TARGETS), len(NON_TEST_TARGETS)))

    # ---- arm (c): undriven ctest registrations ----------------------------
    ctests = []
    for rel, text in cmake_text.items():
        ctests += ADD_TEST_NAME_RE.findall(_strip_cmake_comments(text))
    undriven_ct = [t for t in sorted(set(ctests)) if t not in runner_text]
    for t in undriven_ct:
        problems.append(
            "UNDRIVEN CTEST: add_test(NAME %s) is registered with CTest, but no "
            "documented runner names it and `ctest` is invoked by no script in "
            "this repo. A CTest-only registration is registered to nothing." % t)
    if not undriven_ct and not quiet:
        print("  ARM (c) OK  %d ctest registration(s) driven" % len(set(ctests)))

    # ---- arm (d): disabled-case two-way ratchet ---------------------------
    total_ifzero = 0
    total_skip = 0
    for src in sources:
        try:
            text = read("tests/unit/" + src)
        except OSError:
            continue
        a, b = _count_disabled(text)
        total_ifzero += a
        total_skip += b

    for label, got, pin in (("#if 0", total_ifzero, DISABLED_IFZERO_PIN),
                            ("doctest::skip", total_skip, DISABLED_SKIP_PIN)):
        if got > pin:
            problems.append(
                "DISABLED RATCHET: %d TEST_CASE(s) are disabled via %s but the "
                "pin allows %d. Disabling a test is the cheapest way to make a "
                "newly-armed assertion green, and it does not show up in a "
                "pass/fail count. If the disable is genuinely correct, raise the "
                "pin IN THE SAME COMMIT and say why." % (got, label, pin))
        elif got < pin:
            problems.append(
                "DISABLED RATCHET (stale pin): only %d TEST_CASE(s) are disabled "
                "via %s but the pin still allows %d. Lower the pin to %d -- a "
                "ceiling above the real number lets debt be re-added silently."
                % (got, label, pin, got))
    if not problems and not quiet:
        print("  ARM (d) OK  disabled cases at pin (#if 0=%d, skip=%d)"
              % (total_ifzero, total_skip))

    if problems:
        print("")
        for p in problems:
            print("TEST-TIER %s" % p)
        print("")
        print("[check-test-tier-coverage] FAIL: %d finding(s)." % len(problems))
        return EXIT_VIOLATION

    if not quiet:
        print("[check-test-tier-coverage] PASS -- %d source(s), %d target(s), "
              "%d ctest row(s) all accounted for."
              % (len(sources), len(set(targets)), len(set(ctests))))
    return EXIT_OK


def self_test() -> int:
    """Green on the real tree; red on each injected way a test goes dark."""
    if run_all(quiet=True) != EXIT_OK:
        print("--self-test: FAIL (not green on the current tree)", file=sys.stderr)
        return EXIT_VIOLATION

    unit_cmake = _read("tests/unit/CMakeLists.txt")
    proj = _read("tests/unit/test_projection.cpp")
    real_sources = sorted(f for f in os.listdir(os.path.join(ROOT, "tests", "unit"))
                          if f.startswith("test_") and f.endswith(".cpp"))

    arms = []

    # Arm 1: a new test file nobody added to CMakeLists -- permanently dark.
    arms.append(("orphan test source (added to tests/, never to CMake)",
                 {"__unit_sources__": real_sources + ["test_brand_new.cpp"],
                  "tests/unit/test_brand_new.cpp": "TEST_CASE(\"x\") { CHECK(1); }\n"}))

    # Arm 2: THE REAL REGRESSION -- test_render_contract_3 driven by nothing.
    # Re-injected by removing it from the runner, which is the state this lane
    # actually found (it was in no runner, and did not link).
    arms.append(("test target named by no runner (the real 2026-07-31 state)",
                 {"scripts/run-unit-tests.sh":
                  _read("scripts/run-unit-tests.sh").replace(
                      "test_render_contract_3", "SOMETHING_ELSE")}))

    # Arm 3: a ctest registration no runner drives.
    arms.append(("ctest registration driven by nothing",
                 {"tests/model_override/CMakeLists.txt":
                  _read("tests/model_override/CMakeLists.txt").replace(
                      "model_override_tests", "model_override_renamed")}))

    # Arm 4: silencing a failing test by disabling it -- the arm that matters.
    arms.append(("a TEST_CASE newly disabled with #if 0",
                 {"tests/unit/test_projection.cpp":
                  proj.replace("TEST_CASE(\"projection tests scope-stopped\"",
                               "#if 0\nTEST_CASE(\"newly silenced\") { CHECK(0); }\n#endif\n"
                               "TEST_CASE(\"projection tests scope-stopped\"")}))

    # Arm 5: a stale ceiling -- debt re-enabled but the pin left high.
    arms.append(("stale disabled-pin (debt paid, ceiling not lowered)",
                 {"tests/unit/test_projection.cpp": proj.replace("#if 0", "#if 1", 1)}))

    # Arm 6: MUST NOT FIRE -- a target named only in a CMake COMMENT must not
    # count as driven, and a comment must not create a phantom target either.
    arms.append(("MUST-NOT-FIRE: comment-only text changes nothing",
                 {"tests/unit/CMakeLists.txt":
                  unit_cmake + "\n# add_executable(phantom_tests phantom.cpp)\n"}))

    ok = True
    for i, (label, vfs) in enumerate(arms, 1):
        rc = run_all(quiet=True, vfs=vfs)
        must_fire = not label.startswith("MUST-NOT-FIRE")
        fired = rc != EXIT_OK
        good = fired == must_fire
        ok = ok and good
        print("  arm %d %-8s %-56s %s"
              % (i, "DETECTED" if fired else "quiet", label,
                 "OK" if good else "**WRONG**"))

    if not ok:
        print("--self-test: FAIL", file=sys.stderr)
        return EXIT_VIOLATION
    print("[check-test-tier-coverage] --self-test: PASS "
          "(green on tree, red on every injected darkness, quiet on the decoy)")
    return EXIT_OK


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--quiet", "-q", action="store_true",
                    help="suppress per-arm OK lines")
    ap.add_argument("--self-test", action="store_true",
                    help="prove the checker fails on each injected darkness")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    return run_all(quiet=args.quiet)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as e:                     # noqa: BLE001
        print("check-test-tier-coverage: harness error: %s" % e, file=sys.stderr)
        sys.exit(EXIT_HARNESS_ERROR)
