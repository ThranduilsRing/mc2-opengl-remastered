#!/usr/bin/env python3
"""check-order-gate-coverage.py — META-FIX design F static chokepoint.

Silent-order recurrence guard (issues #17 / #10 / #9 class): the combat
decision tree can say FIRE while fireWeapon quietly early-returns. Every such
gate must be OBSERVABLE (fireGateTrace) or DECLARED (KNOWN_UNCOVERED) — drift
in either direction fails check-contracts.sh with zero game launches.

Assertions:
  1. code/mech_weapons.cpp BattleMech::fireWeapon carries the FIREGATE-REGION-END
     sentinel comment; every `return` between the signature and the sentinel
     (COMMENT-STRIPPED FIRST — mech_weapons.cpp has four commented-out `return(4)`s
     inside /* */ blocks that a naive scanner false-positives on) has a
     fireGateTrace() on the same line or within the 4 preceding lines, or a
     KNOWN_UNCOVERED entry.
  2. code/warrior.cpp combatDecisionTree still contains the `"ev":"nofire"`
     emitter gated on MC2_FIRE_DECISION_TRACE.
  3. KNOWN_UNCOVERED entries are LIVE both ways: a declared-uncovered
     function that gains fireGateTrace() coverage fails (stale declaration —
     remove the entry); an undeclared uninstrumented fireWeapon body fails
     (silent gap — instrument it or declare it).

KNOWN_UNCOVERED today: GroundVehicle::fireWeapon (code/gvehicl_weapons.cpp) — zero
fireGateTrace instrumentation at landing; MP fire dead-ends there surface
only by hand until an instrumentation slice lands.

Runtime siblings: MC2_FIRE_DECISION_TRACE events, MC2_FIXTURE_ORDER_LIVENESS
fixture (its nightly consumer check-order-liveness.py was retired 2026-10-12, cull wave 1).

Usage:
  py -3 scripts/check-order-gate-coverage.py [--quiet]
  py -3 scripts/check-order-gate-coverage.py --self-test

Exit: 0 pass, 1 violation, 2 harness error (house convention).
"""

from __future__ import annotations

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import checklib  # noqa: E402

ROOT = checklib.ROOT
EXIT_OK = checklib.EXIT_OK
EXIT_VIOLATION = checklib.EXIT_VIOLATION
EXIT_HARNESS_ERROR = checklib.EXIT_HARNESS_ERROR

MECH = "code/mech_weapons.cpp"
WARRIOR = "code/warrior_combat.cpp"

SENTINEL = "FIREGATE-REGION-END"
TRACE_CALL = "fireGateTrace("
CONTEXT_LINES = 4   # trace must sit on the return line or <=4 lines above

# Declared-uncovered fireWeapon bodies: (rel_path, signature_regex, reason).
# BOTH-WAYS LIVE: if the body gains fireGateTrace coverage, the stale entry
# fails this checker until removed; a body listed here is exempt from the
# region scan (and gvehicl has no sentinel — the whole body is uncovered).
KNOWN_UNCOVERED = [
    (
        "code/gvehicl_weapons.cpp",
        r"long\s+GroundVehicle\s*::\s*fireWeapon\s*\(",
        "GroundVehicle::fireWeapon — zero instrumentation at META-FIX-BATCH-2 "
        "landing; MPlayer/vehicle fire dead-ends declared-not-closed "
        "(range_mp: gate coverage, no instrumentation)",
    ),
]


class Ctx:
    """Disk + in-memory overlay (self-test injects violations without disk
    mutation — house pattern from check-split-path-contracts.py)."""

    def __init__(self, vfs: dict | None = None):
        self.vfs = vfs or {}

    def read(self, rel: str) -> str:
        if rel in self.vfs:
            return self.vfs[rel]
        return checklib.read_text(rel)


def _body_span(stripped: str, signature_re: str):
    """Return (start_line, end_line, body_start_line_texts) of the brace-
    matched body whose signature matches signature_re, as 1-based line
    numbers into `stripped`. None if not found."""
    m = re.search(signature_re, stripped)
    if not m:
        return None
    brace = stripped.find("{", m.end())
    if brace < 0:
        return None
    body = checklib.extract_brace_body(stripped, brace)
    if body is None:
        return None
    start_line = stripped.count("\n", 0, m.start()) + 1
    end_line = stripped.count("\n", 0, brace + len(body)) + 1
    return (start_line, end_line)


def check_mech_region(ctx: Ctx) -> list[str]:
    problems = []
    raw = ctx.read(MECH)
    stripped = checklib.strip_comments(raw)   # newline-preserving
    raw_lines = raw.splitlines()
    stripped_lines = stripped.splitlines()

    span = _body_span(stripped,
                      r"long\s+BattleMech\s*::\s*fireWeapon\s*\(")
    if span is None:
        return ["mech: cannot locate BattleMech::fireWeapon body in %s" % MECH]
    start_line, end_line = span

    # Sentinel lives in a COMMENT — find it in the RAW text (line numbers are
    # preserved by strip_comments), and require it inside the body span.
    sentinel_line = None
    for i, line in enumerate(raw_lines, start=1):
        if SENTINEL in line and start_line <= i <= end_line:
            sentinel_line = i
            break
    if sentinel_line is None:
        return ["mech: sentinel comment %r missing from "
                "BattleMech::fireWeapon (%s lines %d..%d) — the checker "
                "cannot delimit the pre-delivery gate region"
                % (SENTINEL, MECH, start_line, end_line)]

    return_re = re.compile(r"\breturn\b")
    for i in range(start_line, sentinel_line):          # 1-based, excl sentinel
        line = stripped_lines[i - 1]
        if not return_re.search(line):
            continue
        window_start = max(start_line, i - CONTEXT_LINES)
        window = stripped_lines[window_start - 1:i]     # incl. the return line
        if any(TRACE_CALL in w for w in window):
            continue
        problems.append(
            "mech: untraced early return at %s:%d (pre-delivery region) — "
            "add fireGateTrace(...) within the %d preceding lines or a "
            "KNOWN_UNCOVERED entry: %r"
            % (MECH, i, CONTEXT_LINES, raw_lines[i - 1].strip()))
    return problems


def check_nofire_emitter(ctx: Ctx) -> list[str]:
    problems = []
    text = ctx.read(WARRIOR)
    # The emitter string literal survives comment stripping (it is code).
    stripped = checklib.strip_comments(text)
    if '\\"ev\\":\\"nofire\\"' not in stripped and '"ev":"nofire"' not in stripped:
        problems.append(
            "warrior: the combatDecisionTree 'nofire' emitter string is gone "
            "from %s — refusal classification is the behavioral half of the "
            "#17 guard (ISSUE-17, commit e1eeaa05)" % WARRIOR)
    if "MC2_FIRE_DECISION_TRACE" not in stripped:
        problems.append(
            "warrior: MC2_FIRE_DECISION_TRACE gate vanished from %s" % WARRIOR)
    return problems


def check_known_uncovered(ctx: Ctx) -> list[str]:
    problems = []
    for rel, sig_re, reason in KNOWN_UNCOVERED:
        stripped = checklib.strip_comments(ctx.read(rel))
        span = _body_span(stripped, sig_re)
        if span is None:
            problems.append("known-uncovered: cannot locate %r in %s — stale "
                            "entry? (%s)" % (sig_re, rel, reason))
            continue
        start_line, end_line = span
        body_lines = stripped.splitlines()[start_line - 1:end_line]
        n_traces = sum(1 for l in body_lines if TRACE_CALL in l)
        if n_traces > 0:
            problems.append(
                "known-uncovered: %s now has %d fireGateTrace call(s) but is "
                "still declared KNOWN_UNCOVERED — remove the checker entry "
                "(stale declaration). Entry reason: %s"
                % (rel, n_traces, reason))
    return problems


def run_all(ctx: Ctx, quiet: bool) -> int:
    all_problems = []
    for label, fn in (("mech firegate region", check_mech_region),
                      ("nofire emitter", check_nofire_emitter),
                      ("known-uncovered ledger", check_known_uncovered)):
        try:
            probs = fn(ctx)
        except OSError as e:
            print("order-gate-coverage: HARNESS ERROR in %s: %s" % (label, e),
                  file=sys.stderr)
            return EXIT_HARNESS_ERROR
        if not quiet:
            print("  %-24s %s" % (label, "OK" if not probs
                                  else "%d problem(s)" % len(probs)))
        all_problems.extend(probs)
    if all_problems:
        print("order-gate-coverage: FAIL", file=sys.stderr)
        for p in all_problems:
            print("  " + p, file=sys.stderr)
        return EXIT_VIOLATION
    if not quiet:
        print("order-gate-coverage: PASS")
    return EXIT_OK


def self_test() -> int:
    if run_all(Ctx(), quiet=True) != EXIT_OK:
        print("order-gate-coverage --self-test: FAIL (not green on current "
              "tree)", file=sys.stderr)
        return EXIT_VIOLATION

    failures = []
    mech_raw = checklib.read_text(MECH)

    # 1. A NEW untraced silent gate lands before the sentinel (the exact
    #    recurrence this checker exists for — refutation (a): range_mp was
    #    the 10th, untraced gate).
    t = mech_raw.replace(
        "\tbool inRange = weaponInRange(weaponIndex, distanceToTarget, MapCellDiagonal);",
        "\tif (targetTime < 0.0f)\n\t\treturn(4);\n"
        "\tbool inRange = weaponInRange(weaponIndex, distanceToTarget, MapCellDiagonal);",
        1)
    failures.append(("untraced new gate", run_all(Ctx({MECH: t}), True)))

    # 2. Sentinel deleted — region can no longer be delimited.
    t = mech_raw.replace("FIREGATE-REGION-END", "REGION-END-GONE", 1)
    failures.append(("sentinel deleted", run_all(Ctx({MECH: t}), True)))

    # 3. nofire emitter deleted from combatDecisionTree.
    t = checklib.read_text(WARRIOR).replace('\\"ev\\":\\"nofire\\"',
                                            '\\"ev\\":\\"gone\\"')
    failures.append(("nofire emitter gone", run_all(Ctx({WARRIOR: t}), True)))

    # 4. KNOWN_UNCOVERED goes stale: gvehicl gains instrumentation while the
    #    declaration stays.
    gv = checklib.read_text("code/gvehicl_weapons.cpp")
    t = gv.replace(
        "long GroundVehicle::fireWeapon (GameObjectPtr target, float targetTime, long weaponIndex, long attackType, long aimLocation, glm::vec3* targetPoint, float &dmgDone) {",
        "long GroundVehicle::fireWeapon (GameObjectPtr target, float targetTime, long weaponIndex, long attackType, long aimLocation, glm::vec3* targetPoint, float &dmgDone) {\n"
        "\tfireGateTrace(0, weaponIndex, \"selftest\");",
        1)
    failures.append(("stale KNOWN_UNCOVERED",
                     run_all(Ctx({"code/gvehicl_weapons.cpp": t}), True)))

    bad = [name for name, rc in failures if rc == EXIT_OK]
    for name, rc in failures:
        print("  self-test %-24s %s" % (name,
              "detected (rc=%d)" % rc if rc != EXIT_OK else "NOT DETECTED"))
    if bad:
        print("order-gate-coverage --self-test: FAIL (undetected: %s)"
              % ", ".join(bad), file=sys.stderr)
        return EXIT_VIOLATION
    print("order-gate-coverage --self-test: PASS (green on tree, red on all "
          "%d synthetic violations)" % len(failures))
    return EXIT_OK


def main() -> int:
    args = sys.argv[1:]
    if "--self-test" in args:
        return self_test()
    quiet = "--quiet" in args or "-q" in args
    return run_all(Ctx(), quiet)


if __name__ == "__main__":
    sys.exit(main())
