#!/usr/bin/env python3
"""check-gate-echo-reads.py -- keeps the two uncached environment readers out of decision code.

gate::raw<"X">() and gate::envText(name) return the live environment text. They exist so a log line can echo what
the user typed and so the callers that walk names they do not know at compile time can ask the process
environment. Neither may decide anything: a decision made from them skips the row's rule, range and default,
which is the defect the typed registry removes.

  E1  raw<"X"> may appear only inside the still-open argument list of a printf-family / MC2_DIAGFE / MC2_EVENT
      call, or be assigned to a `const char*` local whose every later use is itself inside such an argument
      list. Nothing weaker counts: a ternary on the same line does not (`atoi(e ? e : "0")` decides), and a
      print call nearby does not (`printf(...); if (raw<>())` decides).
  E2  envText( may be called only from the files in ENVTEXT_ALLOWED, each with a line budget. Each entry is a
      deliberate widening with its reason beside it.
  E3  boolState( reads a Bool gate by a name known only at run time, so it too bypasses the compile-time row
      lookup. It may be called only from the display tables in BOOLSTATE_ALLOWED (inspector and options window
      status columns, the feature-registry dump), each with a line budget.

USAGE
  py -3 scripts/check-gate-echo-reads.py [--quiet]
  py -3 scripts/check-gate-echo-reads.py --selftest
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCAN_DIRS = ("code", "mclib", "GameOS", "RenderCore", "GuiRuntime", "RenderWorld", "GameAdapters",
             "HostServices", "editor", "gui", "Gates", "EditorBridge", "Viewer")
EXTS = (".cpp", ".h", ".hpp", ".cc", ".inl")
# file -> max lines calling envText( ; why the name is truly dynamic there
ENVTEXT_ALLOWED = {
    "GameOS/gameos/gameosmain_init.cpp": 1,   # the launcher_env.json apply asks "did the shell already set this key?"
    "GameOS/gameos/gos_dev_shell.cpp": 1,  # the dev shell's get_gate answers for a variable NAME the client sends
    "HostServices/DefaultHostServices.cpp": 2,  # EnvConfig::str / flag take a caller-supplied runtime key
}
# file -> max lines calling boolState( ; every one is a status display driven by a table of gate names
BOOLSTATE_ALLOWED = {
    "GuiRuntime/EditorInspector.cpp": 3,           # the gate tables and the feature-bit display
    "GuiRuntime/GraphicsOptionsWindow_mech.cpp": 1,  # the mech options status column
    "RenderCore/RendererFeatureRegistry.h": 1,     # the generated registry dump, naming gates from its own table
}
# Gates/ defines the readers; the ratchet cares about their callers.
DEFINING_DIR = "Gates/"
PRINT_FAMILY_RX = re.compile(r"^(?:std::)?(?:printf|fprintf|snprintf|MC2_DIAGFE|MC2_EVENT)$")
# the gate name inside the quotes is blanked by blank(), so match any (blank) literal
RAW_RX = re.compile(r"\braw\s*<\s*\"[^\"]*\"\s*>\s*\(\s*\)")
RAW_LOCAL_RX = re.compile(r"\bconst\s+char\s*\*\s*(\w+)\s*=\s*(?:mc2::)?(?:gate::)?raw\s*<")
ENVTEXT_RX = re.compile(r"\benvText\s*\(")
BOOLSTATE_RX = re.compile(r"\bboolState\s*\(")
LOOKAHEAD_CHARS = 6000
NL = chr(10)
BS = chr(92)
QUOTES = chr(34) + chr(39)


def blank(text: str) -> str:
    """The text with comments and the CONTENT of string/char literals replaced by spaces (offsets and newlines
    kept), so parentheses and quotes inside them cannot confuse the argument-list scan."""
    out = list(text)
    n, i = len(text), 0
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != NL:
                out[i] = " "
                i += 1
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            while i < n and not (text[i] == "*" and i + 1 < n and text[i + 1] == "/"):
                if text[i] != NL:
                    out[i] = " "
                i += 1
            for k in (i, i + 1):
                if k < n:
                    out[k] = " "
            i += 2
        elif c in QUOTES:
            q = c
            i += 1
            while i < n and text[i] != q and text[i] != NL:
                if text[i] == BS and i + 1 < n:
                    out[i] = " "
                    i += 1
                out[i] = " "
                i += 1
            i += 1
        else:
            i += 1
    return "".join(out)


def enclosing_callees(t: str, pos: int) -> list[str]:
    """Names of the calls whose argument list is still open at `pos`, innermost first, back to the statement start."""
    names: list[str] = []
    depth = 0
    i = pos - 1
    while i >= 0:
        c = t[i]
        if c == ")":
            depth += 1
        elif c == "(":
            if depth == 0:
                j = i
                while j > 0 and t[j - 1].isspace():
                    j -= 1
                k = j
                while k > 0 and (t[k - 1].isalnum() or t[k - 1] in "_:"):
                    k -= 1
                names.append(t[k:j])
            else:
                depth -= 1
        elif depth == 0 and c in ";{}":
            break
        i -= 1
    return names


def in_print_args(t: str, pos: int) -> bool:
    return any(PRINT_FAMILY_RX.match(n) for n in enclosing_callees(t, pos))


def check_text(rel: str, text: str) -> list[str]:
    errs: list[str] = []
    if rel.startswith(DEFINING_DIR):
        return errs
    t = blank(text)

    def line_of(off: int) -> int:
        return t.count(NL, 0, off) + 1

    envtext_lines = 0
    for m in ENVTEXT_RX.finditer(t):
        envtext_lines += 1
        if rel not in ENVTEXT_ALLOWED:
            errs.append("%s:%d: envText( outside ENVTEXT_ALLOWED (use isSet<>/get<>/str<>)" % (rel, line_of(m.start())))
    if rel in ENVTEXT_ALLOWED and envtext_lines > ENVTEXT_ALLOWED[rel]:
        errs.append("%s: envText( on %d lines (max %d)" % (rel, envtext_lines, ENVTEXT_ALLOWED[rel]))
    boolstate_lines = 0
    for m in BOOLSTATE_RX.finditer(t):
        boolstate_lines += 1
        if rel not in BOOLSTATE_ALLOWED:
            errs.append("%s:%d: boolState( outside BOOLSTATE_ALLOWED (use is<>/get<>/state<>)" % (rel, line_of(m.start())))
    if rel in BOOLSTATE_ALLOWED and boolstate_lines > BOOLSTATE_ALLOWED[rel]:
        errs.append("%s: boolState( on %d lines (max %d)" % (rel, boolstate_lines, BOOLSTATE_ALLOWED[rel]))
    for m in RAW_RX.finditer(t):
        if in_print_args(t, m.start()):
            continue
        lm = RAW_LOCAL_RX.search(t[max(0, t.rfind(NL, 0, m.start())):m.end()])
        if not lm:
            errs.append("%s:%d: raw<> outside an open print/MC2_EVENT argument list" % (rel, line_of(m.start())))
            continue
        name = lm.group(1)
        use_rx = re.compile(r"\b%s\b" % re.escape(name))
        for u in use_rx.finditer(t, m.end(), min(len(t), m.end() + LOOKAHEAD_CHARS)):
            if not in_print_args(t, u.start()):
                errs.append("%s:%d: raw<> local '%s' used outside a print/MC2_EVENT argument list" % (
                    rel, line_of(u.start()), name))
                break
    return errs


def scan() -> list[str]:
    errs: list[str] = []
    for d in SCAN_DIRS:
        base = ROOT / d
        if not base.is_dir():
            continue
        for p in sorted(base.rglob("*")):
            if p.suffix not in EXTS or not p.is_file():
                continue
            rel = p.relative_to(ROOT).as_posix()
            errs.extend(check_text(rel, p.read_text(encoding="utf-8", errors="replace")))
    return errs


def selftest() -> int:
    ok = True
    launcher = next(iter(ENVTEXT_ALLOWED))

    def expect(name: str, rel: str, text: str, want_errors: bool) -> None:
        nonlocal ok
        got = bool(check_text(rel, text))
        if got != want_errors:
            ok = False
            print("[gate-echo-reads] selftest FAIL: " + name)

    expect("raw in printf", "code/a.cpp", 'fprintf(stderr, "%s", mc2::gate::raw<"MC2_A">());', False)
    expect("raw continuation line", "code/a.cpp",
           'MC2_DIAGFE(stderr, "x %s",\n   mc2::gate::raw<"MC2_A">() ? mc2::gate::raw<"MC2_A">() : "(unset)");', False)
    expect("raw echo local", "code/a.cpp",
           'const char* e = mc2::gate::raw<"MC2_A">();\nint x = 1;\nprintf("%s", e ? e : "(unset)");', False)
    expect("raw decision local", "code/a.cpp",
           'const char* e = mc2::gate::raw<"MC2_A">();\nint cap = e ? 1 : 2;', True)
    expect("raw bare decision", "code/a.cpp", 'if (mc2::gate::raw<"MC2_A">()) go();', True)
    # the two loopholes of the first version: a ternary on the line, and a print call within four lines above
    expect("loophole: ternary inside a non-print call", "code/a.cpp",
           'const char* e = mc2::gate::raw<"MC2_A">();\nint cap = atoi(e ? e : "0");', True)
    expect("loophole: print call just above a decision", "code/a.cpp",
           'printf("x");\nif (mc2::gate::raw<"MC2_A">()) go();', True)
    expect("raw in a closed print call then reused", "code/a.cpp",
           'const char* e = mc2::gate::raw<"MC2_A">();\nprintf("%s", e);\nif (e) go();', True)
    expect("raw nested in a call inside print", "code/a.cpp",
           'printf("%s", pick(mc2::gate::raw<"MC2_A">()));', False)
    expect("envText elsewhere", "code/a.cpp", 'if (mc2::gate::envText("MC2_A")) go();', True)
    expect("envText in an allowed file", launcher, 'if (mc2::gate::envText(key)) go();', False)
    expect("envText over budget", launcher, "envText(a);\nenvText(b);\nenvText(c);", True)
    expect("boolState elsewhere", "code/a.cpp", 'if (mc2::gate::boolState(name) == on) go();', True)
    expect("boolState in a display table", "GuiRuntime/EditorInspector.cpp",
           'const auto st = mc2::gate::boolState(d.envVar);', False)
    expect("boolState over budget", "GuiRuntime/GraphicsOptionsWindow_mech.cpp",
           'boolState(a);' + chr(10) + 'boolState(b);', True)
    expect("defining dir", "Gates/gate.cpp", "const char* envText(const char* n) { return n; }", False)
    expect("comment ignored", "code/a.cpp", '// raw<"MC2_A">() is echo only', False)
    expect("string ignored", "code/a.cpp", 'puts("raw<\\"MC2_A\\">() )(");', False)
    print("[gate-echo-reads] self-test " + ("PASSED" if ok else "FAILED"))
    return 0 if ok else 1


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    errs = scan()
    for e in errs:
        print("[gate-echo-reads] " + e)
    if errs:
        print("[gate-echo-reads] FAILED (%d)" % len(errs))
        return 1
    if not args.quiet:
        print("[gate-echo-reads] OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
