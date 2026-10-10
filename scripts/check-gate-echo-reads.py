#!/usr/bin/env python3
"""check-gate-echo-reads.py -- keeps the two uncached environment readers out of decision code.

gate::raw<"X">() and gate::envText(name) return the live environment text. They exist so a log line can echo what
the user typed and so the launcher-env applier can ask the process environment before the registry is valid.
Neither may decide anything: a decision made from them skips the row's rule, range and default, which is the
defect the typed registry removes.

  E1  raw<"X"> may appear only inside a printf-family / MC2_DIAGFE / MC2_EVENT argument list, or be assigned to a
      local that is afterwards used only as `name ? name : "<fallback text>"` (or `name && name[0] ? ...`).
  E2  envText( may be called only from GameOS/gameos/gameosmain.cpp (the [LAUNCHER_ENV v1] block, at most two
      call lines) -- everywhere else the typed readers apply.

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
             "HostServices", "editor", "gui", "Gates")
EXTS = (".cpp", ".h", ".hpp", ".cc", ".inl")
ENVTEXT_FILE = "GameOS/gameos/gameosmain.cpp"
ENVTEXT_MAX_LINES = 2
# Gates/ defines the two readers; the ratchet cares about their callers.
DEFINING_DIR = "Gates/"

PRINT_RX = re.compile(r"\b(?:printf|fprintf|snprintf|MC2_DIAGFE|MC2_EVENT)\s*\(")
RAW_RX = re.compile(r"\braw\s*<\s*\"MC2_[A-Z0-9_]+\"\s*>\s*\(\s*\)")
RAW_LOCAL_RX = re.compile(r"\bconst\s+char\s*\*\s*(\w+)\s*=\s*(?:mc2::)?(?:gate::)?raw\s*<")
ENVTEXT_RX = re.compile(r"\benvText\s*\(")
LOOKBACK = 4
LOOKAHEAD = 90


def strip_comment(line: str) -> str:
    i = line.find("//")
    return line if i < 0 else line[:i]


def check_text(rel: str, text: str) -> list[str]:
    errs: list[str] = []
    lines = [strip_comment(l) for l in text.splitlines()]
    in_defining = rel.startswith(DEFINING_DIR)
    envtext_lines = 0
    for i, line in enumerate(lines):
        if not in_defining and ENVTEXT_RX.search(line):
            envtext_lines += 1
            if rel != ENVTEXT_FILE:
                errs.append("%s:%d: envText( outside %s (use isSet<>/get<>/str<>)" % (rel, i + 1, ENVTEXT_FILE))
        if in_defining or not RAW_RX.search(line):
            continue
        window = lines[max(0, i - LOOKBACK):i + 1]
        if any(PRINT_RX.search(w) for w in window):
            continue
        m = RAW_LOCAL_RX.search(line)
        if m:
            name = m.group(1)
            use_rx = re.compile(r"\b%s\b" % re.escape(name))
            n_ = re.escape(name)
            echo_rx = re.compile(r"\b%s\s*(?:&&\s*%s\s*\[\s*0\s*\]\s*)?\?\s*%s\s*:\s*\"[^\"]*\"" % (n_, n_, n_))
            bad = False
            for j in range(i + 1, min(len(lines), i + 1 + LOOKAHEAD)):
                if use_rx.search(lines[j]) and not echo_rx.search(lines[j]):
                    bad = True
                    errs.append("%s:%d: raw<> local '%s' used for more than echo" % (rel, j + 1, name))
                    break
            if not bad:
                continue
            continue
        errs.append("%s:%d: raw<> outside a print/MC2_EVENT argument list" % (rel, i + 1))
    if envtext_lines > ENVTEXT_MAX_LINES and rel == ENVTEXT_FILE:
        errs.append("%s: envText( on %d lines (max %d)" % (rel, envtext_lines, ENVTEXT_MAX_LINES))
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
    expect("envText elsewhere", "code/a.cpp", 'if (mc2::gate::envText("MC2_A")) go();', True)
    expect("envText in launcher file", ENVTEXT_FILE, 'if (mc2::gate::envText(key)) go();', False)
    expect("envText three lines", ENVTEXT_FILE, "envText(a);\nenvText(b);\nenvText(c);", True)
    expect("defining dir", "Gates/gate.cpp", "const char* envText(const char* n) { return n; }", False)
    expect("comment ignored", "code/a.cpp", '// raw<"MC2_A">() is echo only', False)
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
