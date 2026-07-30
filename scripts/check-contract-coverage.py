#!/usr/bin/env python3
"""check-contract-coverage.py -- the contract that guards the contracts.

UNWIRED-CHECK-CLOSE-1 (2026-07-30).

WHY THIS EXISTS
---------------
A prior debloat pass measured this repo's check suite and found 21 of 117
check scripts wired into NO executable caller at all. It then did the thing
nobody had done: it ran them. SIX WERE FAILING, and had been for weeks, with
nobody watching. Among them:

  * check-memory-size.py       -- a live ratchet, red on 3 bloated files
  * check-frame-graph.py       -- exit 127 on EVERY invocation since it was
                                  added; it could not execute at all
  * check-claude-md-pointer.sh -- red for 14 days; the commit that broke it
                                  asserts in its own message that it did not
  * check-unified-projection-retirement.sh -- red for ~2 MONTHS; two later
                                  arcs reversed its invariant in reviewed
                                  commits, by people who had no signal the
                                  gate existed, BECAUSE IT WAS NEVER WIRED

That is the whole failure mode: a check nobody runs is not a guard, it is a
file that looks like a guard. Writing one costs an afternoon and buys nothing.

So this check makes the state structurally unreachable. Every check script in
scripts/ must be either:

  (a) WIRED   -- named in a run_check/run_check_tri row in check-contracts.sh,
                 so it runs on every suite invocation; or
  (b) MANUAL  -- listed in scripts/manual-checks.allowlist with a written
                 reason, because it needs a build, a cooked asset tree, a live
                 engine dump or a smoke log and genuinely cannot run in a
                 seconds-scale static aggregator.

Neither list may be silently empty and a script may not be in both. A new
check script that its author forgets to wire FAILS THIS CHECK on the very
first suite run after it lands.

It also closes the sibling failure the same debloat pass found twice in one
day: an aggregator row landing in a commit while the script it names is still
UNTRACKED. Every clone between that commit and the one that finally adds the
script gets a spurious suite FAILURE from a check that does not exist. So:

  (c) every run_check row must name a script that git actually TRACKS.

Exit 0 = every check accounted for. 1 = at least one unaccounted.
Run with --selftest to prove all three arms still fire.
"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCRIPTS = ROOT / "scripts"
AGGREGATOR = SCRIPTS / "check-contracts.sh"
MANUAL_LIST = SCRIPTS / "manual-checks.allowlist"

# Scripts that are infrastructure for the suite rather than checks in it.
NOT_A_CHECK = {
    "check-contracts.sh",           # the aggregator itself
    "check-contract-coverage.py",   # this file
}

CHECK_RE = re.compile(r"^check[-_].*\.(py|sh)$")
# Matches the script path inside a run_check / run_check_tri row.
ROW_RE = re.compile(r"""run_check(?:_tri)?\s+"[^"]+"\s+"([^"]+)""")


def discover_checks():
    return sorted(
        p.name for p in SCRIPTS.iterdir()
        if p.is_file() and CHECK_RE.match(p.name) and p.name not in NOT_A_CHECK
    )


def wired_scripts(aggregator_text):
    """Every scripts/check-*.{py,sh} named in a run_check row."""
    out = set()
    for cmd in ROW_RE.findall(aggregator_text):
        for m in re.finditer(r"scripts/(check[-_][\w.-]+\.(?:py|sh))", cmd):
            out.add(m.group(1))
    return out


def rows_referencing_scripts(aggregator_text):
    """(label, script) for every run_check row naming a scripts/ file."""
    out = []
    for line in aggregator_text.splitlines():
        m = re.search(r'run_check(?:_tri)?\s+"([^"]+)"\s+"([^"]+)', line)
        if not m:
            continue
        label, cmd = m.group(1), m.group(2)
        for s in re.finditer(r"scripts/([\w.-]+\.(?:py|sh))", cmd):
            out.append((label, s.group(1)))
    return out


def parse_manual(text):
    """name -> reason. Blank lines and #-comments ignored."""
    manual = {}
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        name, _, reason = line.partition(":")
        manual[name.strip()] = reason.strip()
    return manual


def tracked_files():
    r = subprocess.run(["git", "ls-files", "scripts/"],
                       cwd=ROOT, capture_output=True, text=True)
    return {Path(p).name for p in r.stdout.splitlines()}


def head_aggregator_text():
    """The COMMITTED aggregator. Arm (c) grades commits, not the working tree:
    a row + its script landing together in one commit is correct even though
    both are briefly untracked while the lane is mid-flight. What hurts every
    clone is a COMMIT in which the row is tracked and the script is not."""
    r = subprocess.run(["git", "show", "HEAD:scripts/check-contracts.sh"],
                       cwd=ROOT, capture_output=True, text=True)
    return r.stdout if r.returncode == 0 else ""


# Executable surfaces that count as "something runs this". A check driven by a
# deploy script, the smoke runner, cmake or a hook is WATCHED -- it just is not
# watched by the aggregator. Only a check reachable from NOTHING is dark. Doc
# mentions never count: a check named only in a .md is exactly the state this
# whole lane exists to abolish.
CALLER_EXTS = (".py", ".sh", ".ps1", ".bat", ".cmd", ".json", ".yml", ".yaml",
               ".cmake", ".toml", ".txt")


def externally_called(checks):
    """name -> first caller path, for checks invoked outside the aggregator."""
    r = subprocess.run(["git", "ls-files"], cwd=ROOT,
                       capture_output=True, text=True)
    surfaces = [p for p in r.stdout.splitlines()
                if (p.endswith(CALLER_EXTS) or Path(p).name in ("CMakeLists.txt", "Makefile"))
                and not p.startswith("graphify-out/")]
    hooks = ROOT / ".claude" / "hooks"
    if hooks.is_dir():
        surfaces += [str(p.relative_to(ROOT)) for p in hooks.iterdir() if p.is_file()]

    found = {}
    for rel in surfaces:
        base = Path(rel).name
        if base in ("check-contracts.sh", "check-contract-coverage.py",
                    "manual-checks.allowlist"):
            continue
        try:
            text = (ROOT / rel).read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for name in checks:
            if name == base or name in found:
                continue
            if name in text:
                found[name] = rel
    return found


def run(aggregator_text=None, manual_text=None, checks=None, tracked=None,
        head_text=None, external_map=None):
    if aggregator_text is None:
        aggregator_text = AGGREGATOR.read_text(encoding="utf-8", errors="replace")
    if manual_text is None:
        manual_text = (MANUAL_LIST.read_text(encoding="utf-8", errors="replace")
                       if MANUAL_LIST.exists() else "")
    if checks is None:
        checks = discover_checks()
    if tracked is None:
        tracked = tracked_files()
    if head_text is None:
        head_text = head_aggregator_text()

    wired = wired_scripts(aggregator_text)
    manual = parse_manual(manual_text)
    external = externally_called(checks) if external_map is None else external_map
    problems = []

    # --- arm (a)/(b): every check accounted for -----------------------------
    for name in checks:
        in_wired = name in wired
        in_manual = name in manual
        in_external = name in external
        if in_wired and in_manual:
            problems.append(
                f"{name}: listed in manual-checks.allowlist AND wired into "
                f"check-contracts.sh -- pick one (wired wins; drop the manual row)")
        elif not in_wired and not in_manual and not in_external:
            problems.append(
                f"{name}: UNWIRED and unaccounted -- nothing executable runs "
                f"it. Add a run_check row to check-contracts.sh, or a row to "
                f"manual-checks.allowlist stating why it cannot run there")
        elif in_manual and not manual[name]:
            problems.append(
                f"{name}: in manual-checks.allowlist with NO reason -- an "
                f"unexplained exemption is how a check goes dark")

    # --- arm (c): no COMMITTED row may name an untracked script ------------
    for label, script in rows_referencing_scripts(head_text or ""):
        if script not in tracked:
            problems.append(
                f"committed row '{label}' names scripts/{script}, which git "
                f"does NOT track -- every clone of this commit gets a spurious "
                f"FAIL from a check that does not exist. Commit the script in "
                f"the same commit as its row")

    # --- stale manual rows --------------------------------------------------
    for name in manual:
        if name not in checks:
            problems.append(
                f"manual-checks.allowlist names {name}, which is not a check "
                f"script in scripts/ -- stale row, delete it")

    return problems, len(checks), len(wired), len(manual), len(external)


def main():
    problems, n_checks, n_wired, n_manual, n_external = run()
    print(f"[contract-coverage] {n_checks} check scripts: "
          f"{n_wired} wired in the aggregator, {n_external} driven by another "
          f"runner, {n_manual} manual-with-reason")
    if problems:
        print(f"\n[contract-coverage] FAIL: {len(problems)} unaccounted:",
              file=sys.stderr)
        for p in problems:
            print(f"  {p}", file=sys.stderr)
        return 1
    print("[contract-coverage] OK: every check script is wired or "
          "explicitly manual with a stated reason")
    return 0


def selftest():
    """Inject each real defect and prove the arm fires. No fixtures: these are
    the genuine article -- an unwired script, a row naming an untracked file,
    and a reasonless exemption."""
    ok = True
    agg = AGGREGATOR.read_text(encoding="utf-8", errors="replace")
    man = MANUAL_LIST.read_text(encoding="utf-8", errors="replace") if MANUAL_LIST.exists() else ""
    tracked = tracked_files()
    checks = discover_checks()
    head = head_aggregator_text()
    # externally_called() walks every tracked file; hoist it out of the five
    # run() calls below so the selftest stays a seconds-scale check.
    ext = externally_called(checks)

    def grade(name, problems, want_substr):
        nonlocal ok
        hit = [p for p in problems if want_substr in p]
        if hit:
            print(f"[selftest]   ok  {name} -> fires")
        else:
            print(f"[selftest] FAIL  {name} -> did NOT fire", file=sys.stderr)
            ok = False

    # live tree must be clean, else the arms below are unreadable
    live = run(agg, man, checks, tracked, head, ext)[0]
    if live:
        print("[selftest] FAIL: live tree already has coverage problems; "
              "fix those before grading the selftest", file=sys.stderr)
        for p in live:
            print(f"    {p}", file=sys.stderr)
        return 1
    print("[selftest]   ok  live tree is fully accounted")

    # arm (a): a brand-new check script nobody wired
    grade("unwired new check",
          run(agg, man, checks + ["check-selftest-phantom.py"], tracked, head, ext)[0],
          "UNWIRED and unaccounted")

    # arm (c): a COMMITTED row naming a script git does not track. Injected
    # into the HEAD text, not the working tree, because that is what arm (c)
    # grades -- a row and its script landing together in one commit is fine.
    grade("committed row names untracked script",
          run(agg, man, checks, tracked,
              head + '\nrun_check "phantom" "py -3 scripts/check-phantom-untracked.py"\n',
              ext)[0],
          "does NOT track")

    # arm (b): an exemption with no stated reason
    grade("reasonless manual exemption",
          run(agg, man + "\ncheck-selftest-phantom.py\n",
              checks + ["check-selftest-phantom.py"], tracked, head, ext)[0],
          "NO reason")

    # stale manual row
    grade("stale manual row",
          run(agg, man + "\ncheck-long-since-deleted.py: some reason\n",
              checks, tracked, head, ext)[0],
          "stale row")

    return 0 if ok else 1


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        sys.exit(selftest())
    sys.exit(main())
