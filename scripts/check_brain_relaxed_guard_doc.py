#!/usr/bin/env python3
"""
BRAIN-DISPATCH-HARNESS-1: check_brain_relaxed_guard_doc.py
Layer 5 — relaxed-guard doc consistency checker.

Invariant: the two RELAXED-CALL GUARD comment blocks in brain_special_dispatch.cpp
and the actual setGeneralTacOrder() call-sites in executeSpecialBody_Apply must
all agree on the permitted-verb count.

Method:
  header_count    = count "  //   <verb>" bullet lines in the top-of-file RELAXED-CALL GUARD block
                    (lines 27–40 region, starting with "// RELAXED-CALL GUARD —")
  inline_count    = count "//   <verb>" bullet lines in the inline RELAXED-CALL GUARD CONTRACT block
                    (lines 472–483 region, starting with "// RELAXED-CALL GUARD CONTRACT:")
  callsite_count  = count "warrior->setGeneralTacOrder(" lines inside executeSpecialBody_Apply()
                    (from "bool executeSpecialBody_Apply(" up to the next top-level function def)

  FAIL if any two of the three counts disagree.
  PASS if all three agree (currently expected value: 6).

Convention: scripts/check-*.py (--root/--quiet/--json, FAIL/WARN/PASS, exit 0/1/2)
  exit 0 = counts agree
  exit 1 = FAIL (counts disagree)
  exit 2 = source file not found

Usage:
  py -3 scripts/check_brain_relaxed_guard_doc.py [--root <worktree-root>] [--quiet] [--json]
"""

import argparse
import json
import re
import sys
from pathlib import Path

DISPATCH_CPP_REL = "code/brain_special_dispatch.cpp"
APPLY_CPP_NAME = "brain_special_apply.cpp"


def count_header_guard_verbs(lines):
    """Count bullet verb lines in the first RELAXED-CALL GUARD block (top of file).

    The block looks like:
      // RELAXED-CALL GUARD — ...:
      // Permitted order calls: ... for:
      //   Brain.CorePower false    → TACTICAL_ORDER_POWERDOWN
      //   Unit.Eject (...)         → TACTICAL_ORDER_EJECT
      ...
      // All fire IN THE ROOT BODY ...
    """
    in_block = False
    count = 0
    for line in lines:
        stripped = line.rstrip()
        if not in_block:
            if "RELAXED-CALL GUARD" in stripped and "CONTRACT" not in stripped:
                in_block = True
            continue
        # Verb bullet lines: "//   <uppercase or word>" with an arrow or alias
        if re.match(r'^//   [A-Za-z]', stripped):
            count += 1
        # Block ends at a known terminator
        elif (stripped == "" or
              "STILL FORBIDDEN" in stripped or
              "FSM-TODO" in stripped or
              "All fire IN THE ROOT" in stripped):
            break
        # Non-bullet comment lines that are part of block preamble — skip, don't break
        # e.g. "// Permitted order calls: ..."
    return count


def count_inline_guard_verbs(lines):
    """Extract the explicit verb count from the inline RELAXED-CALL GUARD CONTRACT block.

    The inline block contains a line like:
      //   Permitted verbs (SIX total): ...
    We parse the spelled-out number and return it as an integer.
    Fallback: count "→" arrows in the block.
    """
    WORD_TO_INT = {
        "one": 1, "two": 2, "three": 3, "four": 4, "five": 5,
        "six": 6, "seven": 7, "eight": 8, "nine": 9, "ten": 10,
    }
    in_block = False
    for line in lines:
        stripped = line.rstrip()
        if not in_block:
            if "RELAXED-CALL GUARD CONTRACT:" in stripped:
                in_block = True
            continue
        # Look for the explicit count line: "Permitted verbs (N total):" or "(SIX total)"
        m = re.search(r'Permitted verbs\s*\((\w+)\s+total\)', stripped, re.IGNORECASE)
        if m:
            token = m.group(1).lower()
            if token.isdigit():
                return int(token)
            if token in WORD_TO_INT:
                return WORD_TO_INT[token]
        # Block ends at non-indented comment or blank line
        if stripped == "" or (stripped.startswith("//") and not stripped.startswith("//   ")):
            break
    return -1  # not found


def count_callsites_in_function(lines, func_pattern_str, needle="warrior->setGeneralTacOrder("):
    """Count `needle` lines inside a named function.

    func_pattern_str: regex string that matches the function's opening line.
    needle: substring to count (default: setGeneralTacOrder call).
    Returns the count of matching lines found within the function body.
    """
    in_func = False
    depth = 0
    started = False
    count = 0
    func_pattern = re.compile(func_pattern_str)
    for line in lines:
        stripped = line.rstrip()
        if not in_func:
            if func_pattern.match(stripped):
                in_func = True
                depth = 0
                started = False
            continue
        depth += stripped.count('{') - stripped.count('}')
        if not started and depth > 0:
            started = True
        if started and depth <= 0:
            break
        if needle in stripped:
            count += 1
    return count


def count_callsites_in_all_functions(lines, func_pattern_str, needle="warrior->setGeneralTacOrder("):
    """Sum `needle` lines over every function whose opening line matches func_pattern_str."""
    func_pattern = re.compile(func_pattern_str)
    total = 0
    in_func = False
    depth = 0
    started = False
    for line in lines:
        stripped = line.rstrip()
        if not in_func:
            if func_pattern.match(stripped):
                in_func = True
                depth = 0
                started = False
            else:
                continue
        depth += stripped.count('{') - stripped.count('}')
        if not started and depth > 0:
            started = True
        if started and depth <= 0:
            in_func = False
            continue
        if needle in stripped:
            total += 1
    return total


def count_clearmove_total(lines):
    """Count `warrior->clearMoveOrders(` calls anywhere in the source (code lines only)."""
    return sum(1 for line in lines
               if "warrior->clearMoveOrders(" in line.rstrip()
               and not line.lstrip().startswith("//"))


def count_clearmove_in_commit(lines):
    """Count `warrior->clearMoveOrders(` calls inside commitBrainIntents only."""
    return count_callsites_in_function(lines, r'^void\s+commitBrainIntents\s*\(',
                                       needle="warrior->clearMoveOrders(")


def count_settarget_total(lines):
    """Count `warrior->setTargetPriority(` calls anywhere in the source (code lines only)."""
    return sum(1 for line in lines
               if "warrior->setTargetPriority(" in line.rstrip()
               and not line.lstrip().startswith("//"))


def count_settarget_in_commit(lines):
    """Count `warrior->setTargetPriority(` calls inside commitBrainIntents only."""
    return count_callsites_in_function(lines, r'^void\s+commitBrainIntents\s*\(',
                                       needle="warrior->setTargetPriority(")


def count_callsites_in_apply(lines):
    """Count warrior->setGeneralTacOrder( lines inside executeSpecialBody_Apply.

    BRAIN-DECISION-INTENT-QUEUE-1: the gate-OFF path keeps all 6 callsites here
    (inside 'else' branches); the gate-ON emit path does NOT call setGeneralTacOrder.
    The checker counts the gate-OFF branches which must still equal 6.
    """
    # The effect handler bodies live in brain_special_apply.cpp; main() adds their callsites.
    return count_callsites_in_function(lines, r'^bool\s+executeSpecialBody_Apply\s*\(')


def count_callsites_in_commit(lines):
    """Count warrior->setGeneralTacOrder( lines inside commitBrainIntents.

    BRAIN-DECISION-INTENT-QUEUE-1: commitBrainIntents is the sole caller of
    setGeneralTacOrder when MC2_BRAIN_INTENT_QUEUE=1.  Its callsite count must
    also equal the verb count (one per verb = 6).
    Returns -1 if the function is not found (checker emits WARN, not FAIL — the
    commit function is new and may not exist in older branches).
    """
    count = count_callsites_in_function(lines, r'^void\s+commitBrainIntents\s*\(')
    # count_callsites_in_function returns 0 if function not found — distinguish:
    # If the function doesn't exist in source, count will be 0 and in_func never set.
    # Detect absence by checking if function header is present at all.
    commit_present = any(re.match(r'^void\s+commitBrainIntents\s*\(', line.rstrip()) for line in lines)
    if not commit_present:
        return -1
    return count


def main():
    parser = argparse.ArgumentParser(description="Brain relaxed-guard doc consistency checker")
    parser.add_argument("--root",    default=".", help="Worktree root (default: .)")
    parser.add_argument("--source",  default=None, help=f"Override source file path (default: <root>/{DISPATCH_CPP_REL})")
    parser.add_argument("--quiet", "-q", action="store_true")
    parser.add_argument("--json",    action="store_true")
    args = parser.parse_args()

    root = Path(args.root).resolve()
    src_path = Path(args.source) if args.source else root / DISPATCH_CPP_REL

    findings = []

    if not src_path.is_file():
        findings.append(("FAIL", f"source not found: {src_path}"))
        _report(findings, args)
        return 2

    with open(src_path, "r", encoding="utf-8", errors="replace") as f:
        lines = f.readlines()

    header_count   = count_header_guard_verbs(lines)
    inline_count   = count_inline_guard_verbs(lines)
    callsite_count = count_callsites_in_apply(lines)
    # The effect handlers of executeSpecialBody_Apply live in a sibling piece next to the source file.
    apply_path = src_path.with_name(APPLY_CPP_NAME)
    if apply_path.is_file():
        with open(apply_path, "r", encoding="utf-8", errors="replace") as f:
            callsite_count += count_callsites_in_all_functions(f.readlines(), r'^(?:static\s+)?(?:void|bool)\s+apply\w+\s*\(')
    commit_count   = count_callsites_in_commit(lines)

    # DISPATCH-INTENT-CLEARMOVEORDERS-1: clearMoveOrders() is a movement function and is
    # FORBIDDEN in dispatch. Every call must live inside commitBrainIntents (the sole
    # permitted mutator). total > commit ⇒ a direct/forbidden call leaked into dispatch.
    clearmove_total  = count_clearmove_total(lines)
    clearmove_commit = count_clearmove_in_commit(lines)
    # UNITQUERY-SETTARGETPRIORITY-1: setTargetPriority() is contract-forbidden in dispatch;
    # every call must be inside commitBrainIntents (the sole permitted mutator).
    settarget_total  = count_settarget_total(lines)
    settarget_commit = count_settarget_in_commit(lines)

    if commit_count >= 0:
        findings.append(("INFO", f"header_guard_verbs={header_count}  inline_guard_verbs={inline_count}  "
                                  f"apply_callsite_count={callsite_count}  commit_callsite_count={commit_count}"))
    else:
        findings.append(("INFO", f"header_guard_verbs={header_count}  inline_guard_verbs={inline_count}  "
                                  f"apply_callsite_count={callsite_count}  commit_callsite_count=absent"))

    ok_apply  = (header_count == inline_count == callsite_count)
    ok_commit = (commit_count < 0) or (commit_count == callsite_count)
    ok = (ok_apply and ok_commit and (clearmove_total == clearmove_commit)
          and (settarget_total == settarget_commit))

    if header_count != callsite_count:
        findings.append(("FAIL", f"header doc ({header_count} verbs) != apply callsite count ({callsite_count})"))
    if inline_count != callsite_count:
        findings.append(("FAIL", f"inline doc ({inline_count} verbs) != apply callsite count ({callsite_count})"))
    if header_count != inline_count:
        findings.append(("FAIL", f"header doc ({header_count}) != inline doc ({inline_count})"))
    if commit_count >= 0 and commit_count != callsite_count:
        findings.append(("FAIL", f"commitBrainIntents callsite count ({commit_count}) != apply callsite count ({callsite_count}) — verb drift"))
    if commit_count < 0:
        findings.append(("WARN", "commitBrainIntents not found in source — BRAIN-DECISION-INTENT-QUEUE-1 not yet landed"))

    # DISPATCH-INTENT-CLEARMOVEORDERS-1 invariant.
    clearmove_ok = (clearmove_total == clearmove_commit)
    findings.append(("INFO", f"clearMoveOrders_total={clearmove_total}  clearMoveOrders_in_commit={clearmove_commit}"))
    if not clearmove_ok:
        findings.append(("FAIL", f"warrior->clearMoveOrders() called outside commitBrainIntents "
                                  f"({clearmove_total} total vs {clearmove_commit} in commit) — "
                                  f"FORBIDDEN-CALL contract violation (DISPATCH-INTENT-CLEARMOVEORDERS-1)"))

    settarget_ok = (settarget_total == settarget_commit)
    findings.append(("INFO", f"setTargetPriority_total={settarget_total}  setTargetPriority_in_commit={settarget_commit}"))
    if not settarget_ok:
        findings.append(("FAIL", f"warrior->setTargetPriority() called outside commitBrainIntents "
                                  f"({settarget_total} total vs {settarget_commit} in commit) — "
                                  f"FORBIDDEN-CALL contract violation (UNITQUERY-SETTARGETPRIORITY-1)"))

    if ok:
        if commit_count >= 0:
            findings.append(("PASS", f"all counts agree: {callsite_count} permitted verbs (apply={callsite_count}, commit={commit_count})"))
        else:
            findings.append(("PASS", f"apply counts agree: {callsite_count} permitted verbs (commit function absent — pre-rung5)"))

    _report(findings, args)

    fail_count = sum(1 for lvl, _ in findings if lvl == "FAIL")
    return 1 if fail_count > 0 else 0



def _report(findings, args):
    if args.json:
        print(json.dumps([{"level": lvl, "message": msg} for lvl, msg in findings], indent=2))
        return
    for lvl, msg in findings:
        if args.quiet and lvl in ("PASS", "INFO"):
            continue
        print(f"{lvl}: {msg}")


if __name__ == "__main__":
    sys.exit(main())
