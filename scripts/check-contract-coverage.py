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

PROVENANCE-FORGERY-CLOSE-1 adds the mirror-image failure. Coverage asked "is
every check WIRED?" and never asked "is any check wired TWICE?".
`bless_refusals_selftest` was a byte-identical duplicate of `bless_selftest` --
same command, fresh label -- so one selftest contributed two greens to the
board's headline count. The suite's number is a measurement; a measurement that
can silently double-count is not one. So:

  (d) no two rows may share a label, and no two rows may share a COMMAND. The
      command arm is the one that matters: a repeated label is obvious on sight,
      a repeated command under a new name is exactly what shipped.

UNWIRED-CHECKS-CLOSE-1 (2026-07-30) closes arm (b)'s own blindness. As shipped,
"driven by another runner" was a bare SUBSTRING test: `if name in text` over
every tracked .py/.sh/.json/.cmake. So a check was credited as watched when its
name appeared in a COMMENT, a docstring, or an English sentence -- the exact
comment-satisfiable class this file's own author diagnosed in their first census
pass and then shipped into the guard. Injection-proven before the fix: adding a
brand-new dark check and then ONE Python comment naming it moved it out of
"UNWIRED and unaccounted" into "driven by another runner" (75 -> 76) without a
line of it ever executing.

Nine real checks were dark behind that credit, each held up by prose alone --
e.g. check-new-gates.py (exit 1, 99 unregistered gates) was "accounted for" by
the sentence "Modeled on scripts/check-new-gates.py Mode A" in another checker's
docstring. A guard whose evidence is a sentence is not a guard.

  (b) is now INVOCATION-shaped, not mention-shaped. A caller must actually run
      the thing: a runner token governing the path (py -3 / python / bash / sh /
      pwsh / cmake COMMAND / a run_check row), or -- for Python -- the script
      name reaching a subprocess call, resolved through the AST so that variable
      indirection still counts (deploy_payload.py builds the path into `hygiene`
      and then subprocess.run()s it) while docstrings and comments never do.

Exit 0 = every check accounted for. 1 = at least one unaccounted.
Run with --selftest to prove all five arms still fire.
"""
import ast
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


def all_rows(aggregator_text):
    """(label, command) for EVERY run_check / run_check_tri row.

    Deliberately not filtered to rows naming a scripts/ file the way
    `rows_referencing_scripts` is: the duplicate that shipped ran
    `tools/visual_verify/bless_selftest.py`, which that filter drops.
    """
    out = []
    for line in aggregator_text.splitlines():
        if line.lstrip().startswith("#"):
            continue
        m = re.search(r'run_check(?:_tri)?\s+"([^"]+)"\s+"([^"]+)"', line)
        if m:
            out.append((m.group(1), m.group(2)))
    return out


def duplicate_rows(aggregator_text):
    """Arm (d): a label or a command registered more than once."""
    problems = []
    seen_label, seen_cmd = {}, {}
    for label, cmd in all_rows(aggregator_text):
        if label in seen_label:
            problems.append(
                f"duplicate contract row LABEL '{label}' -- registered twice. "
                f"Two rows with one name double-count a single check")
        seen_label[label] = cmd
        if cmd in seen_cmd:
            problems.append(
                f"duplicate contract row COMMAND: '{label}' runs exactly what "
                f"'{seen_cmd[cmd]}' already runs ({cmd}). One check, two greens "
                f"-- delete one row. This is the shape that shipped as "
                f"bless_refusals_selftest")
        else:
            seen_cmd[cmd] = label
    return problems


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


# A line that RUNS the script: a runner token governing the path. Anchored on
# the runner, not on the name, so "see scripts/check-foo.py" never matches.
RUNNER_RE = re.compile(
    r"""(?:py\s+-3|python[23]?|\bbash\b|\bsh\b|\bpwsh\b|powershell|
         COMMAND\b[^\n]*?|run_check(?:_tri)?\s+"[^"]+"\s+")
        [^\n]{0,120}?%s""",
    re.X)

COMMENT_RE = re.compile(r'^\s*(?:#|//|--(?!\s*\[)|\*|/\*)')

# Python constructs that actually spawn a process.
SPAWN_RE = re.compile(r"\b(?:subprocess\.\w+|Popen|check_call|check_output|"
                      r"os\.system|os\.exec\w*|runpy\.\w+)\b")


def _line_invokes(line, name):
    """True if this non-comment line runs `name` via an explicit runner."""
    if COMMENT_RE.match(line):
        return False
    return bool(re.search(RUNNER_RE.pattern % re.escape(name), line, re.X))


def _dotted(node):
    """Render a Call's func as a dotted name: subprocess.run, Popen, ..."""
    parts = []
    while isinstance(node, ast.Attribute):
        parts.append(node.attr)
        node = node.value
    if isinstance(node, ast.Name):
        parts.append(node.id)
    return ".".join(reversed(parts))


def _str_constants(node):
    return [n.value for n in ast.walk(node)
            if isinstance(n, ast.Constant) and isinstance(n.value, str)]


def _py_spawns(text, name):
    """True if `name` reaches a process-spawning call in this Python source.

    Matches STRING CONSTANTS in the AST, never raw source text. Using
    ast.get_source_segment here would be wrong in a way that is easy to miss:
    a segment is a raw slice, so a COMMENT sitting inside a multi-line call is
    part of it. That alone re-credited check-file-resolve-miss.py, whose only
    mention in run_smoke.py is a comment inside a long env-allowlist literal.

    So: docstrings and comments never count -- that is the whole point --
    while variable indirection still does, because deploy_payload.py
    legitimately does  hygiene = os.path.join(..., "check-deploy-hygiene.py")
    and then subprocess.run([sys.executable, hygiene, target]).
    """
    try:
        tree = ast.parse(text)
    except SyntaxError:
        return False

    carriers = set()          # variables holding the script path
    for node in ast.walk(tree):
        if isinstance(node, ast.Assign) and \
           any(name in s for s in _str_constants(node.value)):
            for tgt in node.targets:
                if isinstance(tgt, ast.Name):
                    carriers.add(tgt.id)

    for node in ast.walk(tree):
        if not isinstance(node, ast.Call) or not SPAWN_RE.search(_dotted(node.func)):
            continue
        if any(name in s for s in _str_constants(node)):
            return True
        if carriers and any(isinstance(n, ast.Name) and n.id in carriers
                            for n in ast.walk(node)):
            return True
    return False


def externally_called(checks):
    """name -> first caller path, for checks genuinely INVOKED outside the
    aggregator.

    A mention is not a caller. See the module docstring: crediting a bare
    substring is what kept nine dead checks green.
    """
    r = subprocess.run(["git", "ls-files"], cwd=ROOT,
                       capture_output=True, text=True)
    surfaces = [p for p in r.stdout.splitlines()
                if (p.endswith(CALLER_EXTS) or Path(p).name in ("CMakeLists.txt", "Makefile"))
                and not p.startswith("graphify-out/")
                # .claude/ holds PROSE -- handoffs, ledgers and design docs that
                # happen to be .json. META-FIX-DESIGNS-2026-07-16.json alone was
                # crediting four dead checks, purely because its English
                # sentences contain the words "bash" and "python" near a script
                # name. Notes are not runners. Hooks (added below) are.
                and not p.startswith(".claude/")]
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
        lines = None
        for name in checks:
            if name == base or name in found:
                continue
            if name not in text:          # cheap reject before the real test
                continue
            if rel.endswith(".py"):
                # Python gets the AST test ONLY. A line test would credit
                # new_lane.py for "Run `sh scripts/check-worktree-context.sh`
                # before editing." -- a sentence PRINTED AT a human, which runs
                # nothing. Executed command strings still count: they are
                # arguments to the spawn call, so the AST sees them.
                if _py_spawns(text, name):
                    found[name] = rel
                continue
            if lines is None:
                lines = text.splitlines()
            if any(_line_invokes(ln, name) for ln in lines):
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

    # --- arm (d): no row registered twice, by label OR by command -----------
    problems.extend(duplicate_rows(aggregator_text))

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

    # arm (d): THE REAL REGRESSION, re-injected verbatim. This is the exact
    # line that shipped in af4f79c8 -- a second label running the identical
    # bless selftest command -- appended to the live aggregator text.
    grade("duplicate row COMMAND under a fresh label",
          run(agg + '\nrun_check "bless_refusals_selftest"     '
                    '"py -3 tools/visual_verify/bless_selftest.py"\n',
              man, checks, tracked, head, ext)[0],
          "duplicate contract row COMMAND")

    # ...and the easier half: the same label twice.
    grade("duplicate row LABEL",
          run(agg + '\nrun_check "bless_selftest"           '
                    '"py -3 scripts/check-memory-size.py"\n',
              man, checks, tracked, head, ext)[0],
          "duplicate contract row LABEL")

    # --- arm (b) itself: a MENTION is not a CALLER --------------------------
    # UNWIRED-CHECKS-CLOSE-1. The arm that decides "something else runs this"
    # used to be `if name in text`, so the shapes below all counted as callers
    # and nine live checks went dark behind them. These are the genuine
    # article, copied from the files that actually did the false crediting --
    # not invented fixtures.
    def assert_(name, got, want):
        nonlocal ok
        if got == want:
            print(f"[selftest]   ok  {name}")
        else:
            print(f"[selftest] FAIL  {name}: got {got}, want {want}",
                  file=sys.stderr)
            ok = False

    T = "check-target.py"

    # NOT callers -- every one of these credited a dead check before the fix.
    assert_("bare comment is not a caller",
            _line_invokes(f"# see scripts/{T} for the pattern", T), False)
    assert_("prose sentence is not a caller",
            _line_invokes(f"Modeled on scripts/{T} Mode A (diff vs base)", T), False)
    assert_("module docstring is not a caller",
            _py_spawns(f'"""Clone of scripts/{T} per INFRA-5."""\nimport os\n', T), False)
    # The run_smoke.py shape: a comment INSIDE a multi-line spawn call. Raw
    # source-segment matching credits this; constant matching does not.
    assert_("comment inside a spawn call is not a caller",
            _py_spawns('import subprocess\n'
                       'subprocess.run([\n'
                       f'    "a",   # {T} sets both through this runner\n'
                       '    "b"])\n', T), False)
    # A command string PRINTED AT a human runs nothing (scripts/new_lane.py).
    assert_("printed advice is not a caller",
            _py_spawns(f'print("Run `sh scripts/{T}` before editing")\n', T), False)

    # ARE callers -- the fix must not throw these away.
    assert_("runner token governs the path",
            _line_invokes(f"py -3 scripts/{T} --quiet", T), True)
    assert_("cmake COMMAND is a caller",
            _line_invokes(f"COMMAND ${{BASH_EXECUTABLE}} ${{DIR}}/scripts/{T}", T), True)
    assert_("direct subprocess constant is a caller",
            _py_spawns(f'import subprocess\nsubprocess.run([sys.executable, "scripts/{T}"])\n', T), True)
    # deploy_payload.py's real shape: path built into a variable, then spawned.
    assert_("variable indirection is a caller",
            _py_spawns('import subprocess, os\n'
                       f'p = os.path.join(d, "{T}")\n'
                       'subprocess.run([sys.executable, p, target])\n', T), True)

    return 0 if ok else 1


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        sys.exit(selftest())
    sys.exit(main())
