#!/usr/bin/env python3
"""check-doctest-count-locks.py -- DOCTEST-COUNT-LOCK-ARM-1 static tripwire.

WHY THIS EXISTS, WHICH IS THE WHOLE POINT.

tests/unit/test_rendercore.cpp:344 pins a number:

    CHECK(static_cast<int>(RendererFeature::COUNT) == 62);

That assertion is correct today and it is completely unarmed. Between 2026-07-16
and 2026-07-31 the lock read `== 46` while RendererFeature::COUNT was 61 --
fifteen feature appends walked past a tripwire nobody had connected to anything.
It was not "green while blind"; it was NEVER EVALUATED. The reason, verified:

  * tests/unit/test_rendercore.cpp IS in tests/unit/CMakeLists.txt (line 37) and
    IS a live TEST_CASE inside TEST_SUITE("RenderCore"). Nothing is #if 0'd.
  * But no wired check runs the binary. Repo-wide, `mc2_tests` appears in exactly
    one file under scripts/ -- scripts/run-unit-tests.sh, which is the runner
    itself. check-contracts.sh never invokes it. There are no CI workflows.
  * The one script that drives the suite, check-frame-graph.py, is in
    manual-checks.allowlist AND filters with `--ts=FrameGraph,ViewCurrency`,
    which excludes TEST_SUITE("RenderCore") even on a successful run.
  * build64-tests/ does not exist in any worktree; there is no mc2_tests.exe.
    The last recorded green run is in the body of commit 261297676 (2026-07-16),
    the same week the drift began.

That generalises to EVERY doctest case in tests/unit/ (638 TEST_CASEs), plus
test_render_contract_3, model_override_tests and static_natural_skip_policy_tests
(CTest-registered, but nothing in scripts/ invokes ctest). No C++ test binary in
this repo is executed by any gate.

check-contract-coverage.py (UNWIRED-CHECK-CLOSE-1) already closed exactly this
hole for scripts/ -- every check-* must be wired or allowlisted-with-a-reason.
It has no equivalent for tests/. This file is not that equivalent and does not
pretend to be: rebuilding the test tier into the gate set is a separate, larger
decision (it needs a compiler, so it cannot live in this ~5s-per-check
aggregator). What this file does is narrow and cheap -- it arms the specific
tripwires that a plain text comparison CAN check, so a count lock cannot drift
again while the binary stays dark.

WHAT IT CHECKS. For each row in LOCKS: the number asserted in the doctest must
equal the number the C++ header actually defines. Both sides are located by
SYMBOL + SHAPE, never by frozen line numbers, so either file may be reformatted
or moved within itself. If a lock's regex stops matching, that is a FAILURE, not
a skip -- a pin that silently stops finding its anchor is the same class of
defect this file exists to close.

Pure stdlib, no build, no git. Read-only.

Exit: 0 = every lock agrees, 1 = a lock drifted or an anchor vanished,
      2 = harness error.
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

TEST_RENDERCORE = "tests/unit/test_rendercore.cpp"

# (label, test_rel, test_re, source_rel, source_re, adjust)
#   test_re   -- must capture the asserted number in group 1
#   source_re -- must capture the authoritative number in group 1
#   adjust    -- added to the source number before comparing (for sentinel
#                patterns where the count is derived, not written down)
LOCKS = [
    ("RendererFeature::COUNT",
     TEST_RENDERCORE,
     r"CHECK\s*\(\s*static_cast<int>\s*\(\s*RendererFeature::COUNT\s*\)\s*==\s*(\d+)\s*\)",
     "RenderCore/RendererFeatureRegistry.h",
     r"^\s*COUNT\s*=\s*(\d+)\s*,",
     0),

    ("PipelineId::Count_",
     TEST_RENDERCORE,
     r"CHECK\s*\(\s*static_cast<uint32_t>\s*\(\s*PipelineId::Count_\s*\)\s*==\s*(\d+)u\s*\)",
     "RenderCore/PipelineRegistry.h",
     # Scoped to the PipelineId enum body: this header declares TWO `Count_`
     # sentinels (PipelineId and a second, smaller enum), and an unscoped
     # `Count_ = (\d+)` would match whichever came first. Cannot anchor on the
     # `// sentinel` trailing comment either -- this checker comment-strips
     # before matching, on purpose, so that prose can never satisfy a lock.
     r"enum\s+class\s+PipelineId[^{]*\{[^}]*?\bCount_\s*=\s*(\d+)",
     0),

    # kRenderPassIdCount = _SentinelLast - 1, and _SentinelLast is implicit
    # (no `=`), so the authoritative number is the last EXPLICIT pass id and the
    # count equals it. adjust=0 with source_re pinned to `UI = <n>` -- the enum's
    # own comment mandates new ids go BEFORE _SentinelLast, so the last explicit
    # value is always the highest.
    ("kRenderPassIdCount",
     TEST_RENDERCORE,
     r"CHECK\s*\(\s*kRenderPassIdCount\s*==\s*(\d+)u\s*\)",
     "RenderCore/RenderPassContract.h",
     r"^\s*UI\s*=\s*(\d+)\s*,",
     0),
]

# Locks whose two sides live in the SAME doctest file and must simply agree with
# each other (the header side is a constexpr the checker cannot evaluate, but
# C++ static_asserts already tie them together -- these rows catch the doctest
# copies drifting apart from one another).
SIBLING_LOCKS = [
    ("kRenderPassContractCount == kRenderPassIdCount",
     TEST_RENDERCORE,
     r"CHECK\s*\(\s*kRenderPassIdCount\s*==\s*(\d+)u\s*\)",
     r"CHECK\s*\(\s*kRenderPassContractCount\s*==\s*(\d+)\s*\)"),
    ("kFramePassOrderCount == kRenderPassIdCount",
     TEST_RENDERCORE,
     r"CHECK\s*\(\s*kRenderPassIdCount\s*==\s*(\d+)u\s*\)",
     r"CHECK\s*\(\s*kFramePassOrderCount\s*==\s*(\d+)\s*\)"),
]


def _find_one(rel: str, pattern: str, vfs: dict | None = None):
    """Return (number, lineno) or ('error', message)."""
    try:
        text = vfs[rel] if vfs and rel in vfs else checklib.read_text(rel)
    except OSError as e:
        return None, "cannot read %s: %s" % (rel, e)
    # Comment-strip so a number quoted in prose can never satisfy a lock. This
    # is the STRIPPER-AUDIT-1 lexer, so a `//****` banner cannot blank the file.
    stripped = checklib.strip_comments(text)
    m = re.search(pattern, stripped, re.M | re.S)
    if not m:
        return None, ("anchor not found in %s: /%s/ -- the pin stopped matching; "
                      "if the assertion moved or was reworded, update LOCKS in "
                      "the same commit" % (rel, pattern))
    return (int(m.group(1)), stripped[:m.start()].count("\n") + 1), None


def run_all(quiet: bool = False, vfs: dict | None = None) -> int:
    problems: list[str] = []

    for label, t_rel, t_re, s_rel, s_re, adjust in LOCKS:
        got, err = _find_one(t_rel, t_re, vfs)
        if err:
            problems.append("%s: %s" % (label, err))
            continue
        t_val, t_line = got
        got, err = _find_one(s_rel, s_re, vfs)
        if err:
            problems.append("%s: %s" % (label, err))
            continue
        s_val, s_line = got
        s_val += adjust
        if t_val != s_val:
            problems.append(
                "%s: doctest asserts %d (%s:%d) but the source defines %d "
                "(%s:%d). The doctest binary is not run by any wired check, so "
                "this drift is invisible until someone builds tests/unit by "
                "hand -- fix the lock in the same commit as the append."
                % (label, t_val, t_rel, t_line, s_val, s_rel, s_line))
        elif not quiet:
            print("  LOCK OK  %-28s %d   (%s:%d <-> %s:%d)"
                  % (label, t_val, t_rel, t_line, s_rel, s_line))

    for label, rel, a_re, b_re in SIBLING_LOCKS:
        ga, ea = _find_one(rel, a_re, vfs)
        gb, eb = _find_one(rel, b_re, vfs)
        if ea or eb:
            problems.append("%s: %s" % (label, ea or eb))
            continue
        if ga[0] != gb[0]:
            problems.append(
                "%s: %s:%d asserts %d but %s:%d asserts %d -- these count the "
                "same table and a C++ static_assert already ties them together, "
                "so one of the two doctest numbers is stale."
                % (label, rel, ga[1], ga[0], rel, gb[1], gb[0]))
        elif not quiet:
            print("  LOCK OK  %-28s %d   (sibling agreement)" % (label, ga[0]))

    if problems:
        print("")
        for p in problems:
            print("DOCTEST-COUNT-LOCK %s" % p)
        print("")
        print("[check-doctest-count-locks] FAIL: %d lock(s) drifted." % len(problems))
        return EXIT_VIOLATION

    if not quiet:
        print("[check-doctest-count-locks] PASS -- %d lock(s) agree with source."
              % (len(LOCKS) + len(SIBLING_LOCKS)))
    return EXIT_OK


def self_test() -> int:
    """Green on the real tree; red on each injected drift, one arm per lock."""
    if run_all(quiet=True) != EXIT_OK:
        print("--self-test: FAIL (not green on the current tree)", file=sys.stderr)
        return EXIT_VIOLATION

    arms = []

    # Arm 1: the exact historical regression -- roll the doctest number back to
    # the stale 46 and require this checker to catch what the unrun binary did not.
    t = checklib.read_text(TEST_RENDERCORE)
    arms.append(("stale RendererFeature::COUNT lock (the real 46-vs-61 drift)",
                 {TEST_RENDERCORE: t.replace(
                     "RendererFeature::COUNT) == 62", "RendererFeature::COUNT) == 46")}))

    # Arm 2: an append to the source with no matching doctest bump.
    h = checklib.read_text("RenderCore/RendererFeatureRegistry.h")
    arms.append(("source appended without bumping the lock",
                 {"RenderCore/RendererFeatureRegistry.h":
                  h.replace("    COUNT                    = 62,",
                            "    SomeNewFeature           = 62,\n"
                            "    COUNT                    = 63,")}))

    # Arm 3: a pin whose anchor vanished must FAIL, never silently skip.
    arms.append(("doctest assertion deleted (anchor vanished)",
                 {TEST_RENDERCORE: t.replace(
                     "CHECK(static_cast<int>(RendererFeature::COUNT) == 62);", "")}))

    # Arm 4: sibling doctest numbers drifting apart from each other.
    arms.append(("sibling count locks disagree",
                 {TEST_RENDERCORE: t.replace("kFramePassOrderCount == 11",
                                             "kFramePassOrderCount == 12")}))

    # Arm 5: MUST NOT FIRE -- a number that appears only in a comment must not
    # satisfy or break a lock (proves the comment-strip is load-bearing here).
    arms.append(("MUST-NOT-FIRE: number mentioned only in prose",
                 {TEST_RENDERCORE: t.replace(
                     "CHECK(static_cast<int>(RendererFeature::COUNT) == 62);",
                     "// CHECK(static_cast<int>(RendererFeature::COUNT) == 46);\n"
                     "    CHECK(static_cast<int>(RendererFeature::COUNT) == 62);")}))

    ok = True
    for i, (label, vfs) in enumerate(arms, 1):
        rc = run_all(quiet=True, vfs=vfs)
        must_fire = not label.startswith("MUST-NOT-FIRE")
        fired = rc != EXIT_OK
        good = fired == must_fire
        ok = ok and good
        print("  arm %d %-8s %-55s %s"
              % (i, "DETECTED" if fired else "quiet", label,
                 "OK" if good else "**WRONG**"))

    if not ok:
        print("--self-test: FAIL", file=sys.stderr)
        return EXIT_VIOLATION
    print("[check-doctest-count-locks] --self-test: PASS "
          "(green on tree, red on every injected drift, quiet on the decoy)")
    return EXIT_OK


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--quiet", "-q", action="store_true",
                    help="suppress per-lock OK lines")
    ap.add_argument("--self-test", action="store_true",
                    help="prove the checker fails on injected drift")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    return run_all(quiet=args.quiet)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as e:                     # noqa: BLE001
        print("check-doctest-count-locks: harness error: %s" % e, file=sys.stderr)
        sys.exit(EXIT_HARNESS_ERROR)
