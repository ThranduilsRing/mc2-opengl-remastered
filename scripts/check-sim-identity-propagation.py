#!/usr/bin/env python3
"""check-sim-identity-propagation.py -- SIM-IDENTITY-HASH-1 pipe-liveness.

WHY THIS EXISTS (it caught a real, already-committed bug in its own slice)
=========================================================================
The world-state hash crosses FOUR components before anything grades it:

  engine  code/sim_identity.cpp        -> writes the block
  engine  gos_visual_capture.cpp       -> embeds it in the per-bookmark sidecar
  python  run_visual_capture.py        -> lifts it out of the sidecar into the
                                          capture manifest
  python  run_golden_parity.py         -> carries it into the COMBINED manifest
  python  golden_compare.py            -> the gate reads it and refuses

The first version of this slice populated a local `sim_ids` map in
run_golden_parity.py and never put it in the combined manifest. Every link
looked right in isolation. The gate's own self-test passed (it was fed
hand-built manifests with the block at top level), the field-set contract
passed, the kernel tests passed -- and in the REAL pipeline the block never
arrived, `find_block()` returned None, every comparison came back UNVERIFIED,
and the gate graded everything while appearing installed.

That is the exact failure class this repo keeps paying for: a check that passes
its own self-test while dead. A gate is not "wired" because each end compiles;
it is wired when the payload actually traverses the pipe.

WHAT IT CHECKS
==============
Statically, on source text (no build, no capture, no engine):
  1. the engine collector emits a "sim_identity" key
  2. gos_visual_capture.cpp calls mc2SimIdentityWriteJson and splices the result
  3. run_visual_capture.py lifts `sim_identity` out of the sidecar AND puts it
     on each bookmark result
  4. run_golden_parity.py puts a `sim_identity` key in the COMBINED manifest
     dict -- the specific link that was missing
  5. golden_compare.py actually invokes the gate before grading

WHAT IT DOES NOT CHECK
======================
That the bytes are well-formed at runtime. No capture has ever run in this lane
(engine launches are blocked), so the JSON emitter is compile-verified and
schema-verified only. A malformed-but-present block would satisfy every rule
here and still land as UNVERIFIED. That gap is real and is named in the slice's
status notes rather than papered over.

VERIFY-THE-UNVERIFIED-1 HARDENING (2026-07-30)
==============================================
The first version of THIS CHECK had the very hole it was written to close.
Link 5's needles were `_sim_identity_gate(` and `_SIM_EXIT_UNGRADED` -- both
satisfied by the FUNCTION DEFINITION and the module-level constant. Replacing
golden_compare.py's `if not args.no_sim_identity_gate:` block with `if False:`
deletes the only call, makes exit 7 unreachable and grades every comparison --
and this check, check-sim-identity-fieldsets.py and sim_identity.py self-test
ALL stayed at exit 0. Measured, not theorised.

Two fixes, both structural rather than textual:
  * comments are STRIPPED before any substring match (string literals kept, so
    the C++ needles that live inside format strings still match). An identifier
    in a comment can no longer satisfy a link.
  * link 5 is checked with `ast`, not text: there must be a real Call node to
    `_sim_identity_gate` OUTSIDE its own definition, and a real `return
    _SIM_EXIT_UNGRADED`. A definition alone now FAILS.

Exit 0 = every link present. 1 = a link is missing (named). 2 = a file is
missing, which is never a pass.
"""
from __future__ import annotations

import ast
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def strip_comments(text: str, lang: str) -> str:
    """Remove comments while PRESERVING string literals.

    Needed because several needles legitimately live inside string literals
    (the C++ emitter's `\\"sim_identity\\": {` format string, the Python dict
    keys). Blanking a comment to spaces keeps offsets stable and keeps the
    failure message honest. On any surprise this returns the ORIGINAL text --
    a stripper that throws must not turn a real link into a phantom failure.
    """
    if lang == "py":
        # Exact, via the real tokenizer: the hand scanner below cannot see
        # triple-quoted strings, and both files in LINKS carry long docstrings.
        # Getting that wrong the FALSE-FAILURE way (stripping live code) is the
        # only outcome that matters, so use the parser and fall back only if it
        # refuses the file.
        # Blank the COMMENT spans IN PLACE rather than re-joining tokens:
        # re-joining would normalise whitespace and a needle like
        # '"sim_identity": simid' would stop matching -- a false FAILURE, the
        # one outcome worse than the hole this closes.
        try:
            import io
            import tokenize as _tk
            lines = text.splitlines(keepends=True)
            for tok in _tk.generate_tokens(io.StringIO(text).readline):
                if tok.type != _tk.COMMENT:
                    continue
                (r0, c0), (r1, c1) = tok.start, tok.end
                if r0 != r1:
                    continue
                ln = lines[r0 - 1]
                lines[r0 - 1] = ln[:c0] + (" " * (c1 - c0)) + ln[c1:]
            return "".join(lines)
        except Exception:
            pass  # fall through to the scanner
    try:
        out = []
        i, n = 0, len(text)
        quote = ""          # active string delimiter, "" when not in a string
        while i < n:
            c = text[i]
            if quote:
                out.append(c)
                if c == "\\" and i + 1 < n:
                    out.append(text[i + 1])
                    i += 2
                    continue
                if c == quote:
                    quote = ""
                i += 1
                continue
            if c in ('"', "'"):
                quote = c
                out.append(c)
                i += 1
                continue
            if lang == "py" and c == "#":
                while i < n and text[i] != "\n":
                    i += 1
                continue
            if lang == "cpp" and c == "/" and i + 1 < n:
                if text[i + 1] == "/":
                    while i < n and text[i] != "\n":
                        i += 1
                    continue
                if text[i + 1] == "*":
                    j = text.find("*/", i + 2)
                    i = n if j < 0 else j + 2
                    continue
            out.append(c)
            i += 1
        return "".join(out)
    except Exception:
        return text


# (label, path, lang, [required substrings], why it matters)
LINKS = [
    ("engine collector emits the block",
     "code/sim_identity.cpp", "cpp",
     ['\\"sim_identity\\": {'],
     "without this there is no payload at all"),

    ("capture sidecar splices the block in",
     "GameOS/gameos/gos_visual_capture.cpp", "cpp",
     ["mc2SimIdentityWriteJson", "sib.data()"],
     "the block would never reach a file on disk"),

    ("run_visual_capture lifts it out of the sidecar",
     "scripts/run_visual_capture.py", "py",
     ['sc.get("sim_identity")', '"sim_identity": simid'],
     "the block would die at the sidecar and never reach a manifest"),

    ("run_golden_parity puts it in the COMBINED manifest",
     "scripts/run_golden_parity.py", "py",
     ['"sim_identity": _first_sim_identity(sim_ids)'],
     "THE LINK THAT WAS MISSING: the gate reads the combined manifest, so "
     "without this key every comparison is UNVERIFIED and grades silently"),
]

# Link 5 is structural, not textual -- see the HARDENING note above.
GATE_FILE = "scripts/golden_compare.py"
GATE_FUNC = "_sim_identity_gate"
GATE_EXIT = "_SIM_EXIT_UNGRADED"


def check_gate_is_called(path: Path):
    """Return a list of failure strings. Empty list = the gate is genuinely
    invoked and its refusal exit code is genuinely returned."""
    try:
        tree = ast.parse(path.read_text(encoding="utf-8", errors="replace"))
    except SyntaxError as e:
        return ["%s does not parse (%s)" % (GATE_FILE, e)]

    # Definition sites, so a recursive self-call inside the gate itself cannot
    # masquerade as a live caller.
    defs = [n for n in ast.walk(tree)
            if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef))
            and n.name == GATE_FUNC]
    def_spans = [(d.lineno, getattr(d, "end_lineno", d.lineno)) for d in defs]

    def inside_own_def(node) -> bool:
        ln = getattr(node, "lineno", -1)
        return any(a <= ln <= b for a, b in def_spans)

    calls = [n for n in ast.walk(tree)
             if isinstance(n, ast.Call)
             and isinstance(n.func, ast.Name)
             and n.func.id == GATE_FUNC
             and not inside_own_def(n)]

    returns = [n for n in ast.walk(tree)
               if isinstance(n, ast.Return)
               and isinstance(n.value, ast.Name)
               and n.value.id == GATE_EXIT]

    bad = []
    if not defs:
        bad.append("%s() is not defined at all" % GATE_FUNC)
    if not calls:
        bad.append("%s() is DEFINED BUT NEVER CALLED -- the refuse-to-grade "
                   "gate is dead and every comparison grades" % GATE_FUNC)
    if not returns:
        bad.append("no `return %s` -- the UNGRADED verdict can never reach the "
                   "process exit code" % GATE_EXIT)
    return bad


def main() -> int:
    failures = []
    missing_files = []
    for label, rel, lang, needles, why in LINKS:
        p = ROOT / rel
        if not p.exists():
            missing_files.append(rel)
            continue
        text = strip_comments(
            p.read_text(encoding="utf-8", errors="replace"), lang)
        absent = [n for n in needles if n not in text]
        if absent:
            failures.append((label, rel, absent, why))

    gate_path = ROOT / GATE_FILE
    if not gate_path.exists():
        missing_files.append(GATE_FILE)
    else:
        bad = check_gate_is_called(gate_path)
        if bad:
            failures.append(("golden_compare runs the gate before grading",
                             GATE_FILE, bad,
                             "the verdict would be computed by nobody"))

    if missing_files:
        for rel in missing_files:
            print("[sim-identity-propagation] MISSING FILE: %s" % rel)
        print("[sim-identity-propagation] RESULT: FAIL (files missing -- the "
              "chain was not checked, which is NOT a pass)")
        return 2

    if failures:
        for label, rel, absent, why in failures:
            print("[sim-identity-propagation] BROKEN LINK: %s" % label)
            print("    file    : %s" % rel)
            print("    missing : %s" % ", ".join(repr(a) for a in absent))
            print("    why     : %s" % why)
        print("[sim-identity-propagation] RESULT: FAIL (%d broken link(s)) -- "
              "the refuse-to-grade gate is INSTALLED BUT DEAD; comparisons will "
              "return UNVERIFIED and grade anyway." % len(failures))
        return 1

    print("[sim-identity-propagation] all %d links present (comments stripped; "
          "the gate call is AST-verified, not text-matched): engine -> sidecar "
          "-> capture manifest -> combined manifest -> gate" % (len(LINKS) + 1))
    print("[sim-identity-propagation] RESULT: PASS")
    return 0


# ---------------------------------------------------------------------------
# Self-test. Every case below is a regression that was MEASURED against the
# live tree (inject -> run -> restore), not invented: the first two were green
# under the original text-matching version of this check, which is why they are
# here. Pure functions only -- no repo files are read, so a self-test pass says
# nothing about the real chain and vice versa. That is deliberate: this proves
# the MECHANISM discriminates, main() proves the TREE is wired.
# ---------------------------------------------------------------------------
_GATE_LIVE = '''
_SIM_EXIT_UNGRADED = 7

def _sim_identity_gate(a, b, c, d=False):
    return {"verdict": "GRADED"}

def main():
    si = _sim_identity_gate(1, 2, 3, 4)
    if si["verdict"] == "UNGRADED":
        return _SIM_EXIT_UNGRADED
    return 0
'''

# The exact injection that defeated the original check: definition kept,
# call deleted. Both identifiers are still present as raw text.
_GATE_DEAD = '''
_SIM_EXIT_UNGRADED = 7

def _sim_identity_gate(a, b, c, d=False):
    return {"verdict": "GRADED"}

def main():
    if False:
        si = None
        if si is not None:
            return _SIM_EXIT_UNGRADED
    return 0
'''

# Call kept, refusal swallowed: the verdict can never reach the exit code.
_GATE_SWALLOWED = '''
_SIM_EXIT_UNGRADED = 7

def _sim_identity_gate(a, b, c, d=False):
    return {"verdict": "GRADED"}

def main():
    si = _sim_identity_gate(1, 2, 3, 4)
    if si["verdict"] == "UNGRADED":
        pass
    return 0
'''

# A recursive self-call must NOT count as a live caller.
_GATE_ONLY_RECURSES = '''
_SIM_EXIT_UNGRADED = 7

def _sim_identity_gate(a, b, c, d=False):
    if a:
        return _sim_identity_gate(0, b, c, d)
    return {"verdict": "GRADED"}

def main():
    return _SIM_EXIT_UNGRADED
'''


def _selftest() -> int:
    import tempfile
    import os

    fails = []

    def check(name, ok):
        print("  %-62s %s" % (name, "PASS" if ok else "*** FAIL ***"))
        if not ok:
            fails.append(name)

    # --- comment stripping: needles in comments must NOT satisfy a link -----
    py_real = 'd = {\n    "sim_identity": simid,\n}\n'
    py_commented = 'd = {\n    # "sim_identity": simid,\n}\n'
    check("py: real code keeps the needle",
          '"sim_identity": simid' in strip_comments(py_real, "py"))
    check("py: a needle inside a # comment is stripped away",
          '"sim_identity": simid' not in strip_comments(py_commented, "py"))
    check("py: a # INSIDE a string literal is not a comment",
          'a#b' in strip_comments("x = 'a#b'\n", "py"))
    # Regression guard for the FALSE-FAILURE direction: every LINKS file starts
    # with a long docstring, and a stripper that mis-parses one (or that
    # re-joins tokens and normalises spacing) would delete live code or break
    # an exact-spacing needle. Either way a correctly-wired tree would FAIL.
    py_doc = ('"""A docstring with a # hash and a \'quote\'.\n\n'
              'Second paragraph, still inside the docstring.\n"""\n'
              'd = {\n    "sim_identity": simid,   # trailing comment\n}\n')
    stripped = strip_comments(py_doc, "py")
    check("py: a needle after a triple-quoted docstring survives",
          '"sim_identity": simid' in stripped)
    check("py: exact spacing is preserved (no token re-join)",
          '    "sim_identity": simid,' in stripped)
    check("py: the trailing comment after that needle is still stripped",
          'trailing comment' not in stripped)

    cpp_real = 'buf += "  \\"sim_identity\\": {";\n'
    cpp_commented = '// buf += "  \\"sim_identity\\": {";\n'
    cpp_block = '/* buf += "  \\"sim_identity\\": {"; */\n'
    check("cpp: a needle inside a STRING LITERAL survives stripping",
          '\\"sim_identity\\": {' in strip_comments(cpp_real, "cpp"))
    check("cpp: a needle inside a // comment is stripped away",
          '\\"sim_identity\\": {' not in strip_comments(cpp_commented, "cpp"))
    check("cpp: a needle inside a /* */ comment is stripped away",
          '\\"sim_identity\\": {' not in strip_comments(cpp_block, "cpp"))
    check("cpp: a // INSIDE a string literal is not a comment",
          'http://x' in strip_comments('const char* u = "http://x";\n', "cpp"))

    # --- AST gate-liveness -------------------------------------------------
    tmp = tempfile.mkdtemp(prefix="simid_prop_selftest_")
    try:
        def verdict(src):
            p = Path(tmp) / "g.py"
            p.write_text(src, encoding="utf-8")
            return check_gate_is_called(p)

        check("ast: a genuinely wired gate passes", verdict(_GATE_LIVE) == [])
        bad = verdict(_GATE_DEAD)
        check("ast: DEFINED-BUT-NEVER-CALLED fails (the original blind spot)",
              any("NEVER CALLED" in b for b in bad))
        bad = verdict(_GATE_SWALLOWED)
        check("ast: a swallowed `return _SIM_EXIT_UNGRADED` fails",
              any("_SIM_EXIT_UNGRADED" in b for b in bad))
        bad = verdict(_GATE_ONLY_RECURSES)
        check("ast: a recursive self-call does not count as a live caller",
              any("NEVER CALLED" in b for b in bad))
        bad = verdict("def f(:\n")
        check("ast: an unparsable file FAILS rather than passing empty",
              bad != [])
    finally:
        try:
            os.remove(os.path.join(tmp, "g.py"))
            os.rmdir(tmp)
        except OSError:
            pass

    if fails:
        print("[sim-identity-propagation selftest] RESULT: FAIL (%d)" % len(fails))
        return 1
    print("[sim-identity-propagation selftest] RESULT: ALL PASS")
    return 0


if __name__ == "__main__":
    if "--selftest" in sys.argv or "--self-test" in sys.argv:
        sys.exit(_selftest())
    sys.exit(main())
