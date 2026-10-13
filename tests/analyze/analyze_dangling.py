#!/usr/bin/env python3
"""analyze_dangling - the lifetime gate for "a pointer into a std::string outlives the string", proven on a seeded negative before it is trusted.

WHAT: compiles tests/analyze/dangling_seed.cpp (every line that carries a `// SHAPE:<name>` tag is a dangling-pointer shape) and
tests/analyze/dangling_clean.cpp (the same calls with the owner kept alive) with two analyzers:
  cl /analyze        the C++ Core Check lifetime rule set; C26815 (pointer into a destroyed temporary) and C26816 (pointer into stack memory)
  clang-cl           -Wdangling-gsl -Wdangling-assignment-gsl -Wreturn-stack-address -Wdangling -Wdangling-field (clang is on the host, docs/host-tools.md)
A shape is CAUGHT when either analyzer reports a diagnostic on its tagged line. The script FAILS when a shape is missed by both, or when the
clean twin draws any lifetime diagnostic from either (a gate that cries wolf proves nothing). The measured result on this host (MSVC 14.44, clang 23):
the Core Check lifetime rules report NONE of the c_str() shapes (they model pointer types annotated as gsl::Pointer, and a `const char*` out of
std::string::c_str() is not one), so every shape is held by clang-cl alone. With --check FILE... the script runs clang-cl over those translation
units with the defines and include paths of a compile_commands.json and lists the lifetime diagnostics.
WHY: EString was copy-on-write, so a `const char*` taken from a by-value accessor stayed valid; the same code on std::string dangles and neither the
compiler nor ASan notices unless the freed block is reused (cxx23-step4-b1-notes.md section 5). The gate is trusted only for the shapes this script
proves it catches: a TU that is clean here is not proof about a shape the seeded file does not contain (a pointer stored through a function
boundary, a pointer kept in a container); those stay covered by the call-site list and the accessor rule (accessors return const std::string&).

  py -3 tests/analyze/analyze_dangling.py                 # the seeded self-test (exit 0 = every shape caught, clean twin silent)
  py -3 tests/analyze/analyze_dangling.py --verbose       # print each analyzer's diagnostics for the two files
  py -3 tests/analyze/analyze_dangling.py --check code/objective.cpp --compile-commands build64-ninja/compile_commands.json
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import configure_ninja as cn  # noqa: E402

SEED = os.path.join(HERE, "dangling_seed.cpp")
CLEAN = os.path.join(HERE, "dangling_clean.cpp")
RULESET = cn.VS + "/Team Tools/Static Analysis Tools/Rule Sets/CppCoreCheckLifetimeRules.ruleset"
CLANG_CL = os.environ.get("MC2_TOOL_CLANG_CL") or "C:/Program Files/LLVM/bin/clang-cl.exe"
#: The lifetime diagnostics of the two analyzers that mean "this pointer outlives its owner".
CL_CODES = {"C26815", "C26816"}
CLANG_FLAGS = ["-Wdangling", "-Wdangling-gsl", "-Wdangling-assignment-gsl", "-Wreturn-stack-address", "-Wdangling-field"]
_CL_DIAG = re.compile(r"^(?P<file>.+?)\((?P<line>\d+)(?:,\d+)?\)\s*:\s*(?:warning|error)\s+(?P<code>C\d+)")
_CLANG_DIAG = re.compile(r"^(?P<file>.+?)\((?P<line>\d+),\d+\):\s*warning:.*\[(?P<code>-W[\w-]+)\]")


def norm(p: str) -> str:
    return os.path.normcase(os.path.abspath(p))


def run_cl(src: str, extra: list[str] | None = None) -> tuple[list[tuple[str, int, str]], str]:
    """cl /analyze on one file -> ([(file, line, code)] for the lifetime codes, raw output)."""
    obj = tempfile.mkdtemp(prefix="mc2_an_")
    args = ["cl", "/nologo", "/c", "/std:c++latest", "/EHsc", "/W3", "/external:anglebrackets", "/external:W0", "/analyze:only",
            "/analyze:plugin", "EspXEngine.dll", f"/analyze:ruleset{RULESET}", f"/Fo{obj}\\", src] + (extra or [])
    r = subprocess.run(f'call "{cn.VCVARS}" >nul && {subprocess.list2cmdline(args)}', shell=True, capture_output=True, text=True, errors="replace")
    out = r.stdout + r.stderr
    hits = []
    for ln in out.splitlines():
        m = _CL_DIAG.match(ln.strip())
        if m and m.group("code") in CL_CODES:
            hits.append((norm(m.group("file")), int(m.group("line")), m.group("code")))
    return hits, out


def run_clang(src: str, extra: list[str] | None = None) -> tuple[list[tuple[str, int, str]], str]:
    """clang-cl on one file with the dangling warnings -> ([(file, line, flag)], raw output)."""
    args = [CLANG_CL, "/nologo", "/c", "/std:c++latest", "/EHsc", "/clang:-fsyntax-only"] + CLANG_FLAGS + [src] + (extra or [])
    r = subprocess.run(f'call "{cn.VCVARS}" >nul && {subprocess.list2cmdline(args)}', shell=True, capture_output=True, text=True, errors="replace")
    out = r.stdout + r.stderr
    hits = []
    for ln in out.splitlines():
        m = _CLANG_DIAG.match(ln.strip())
        if m and m.group("code") in set(CLANG_FLAGS):
            hits.append((norm(m.group("file")), int(m.group("line")), m.group("code")))
    return hits, out


def shapes(path: str) -> dict[str, int]:
    """`// SHAPE:<name>` tags of the seeded file -> {name: line}."""
    found = {}
    with open(path, encoding="utf-8") as f:
        for i, ln in enumerate(f, 1):
            m = re.search(r"//\s*SHAPE:(\w+)", ln)
            if m:
                found[m.group(1)] = i
    return found


def self_test(verbose: bool) -> int:
    seed_shapes = shapes(SEED)
    if len(seed_shapes) < 3:
        print(f"analyze_dangling: {SEED} carries {len(seed_shapes)} SHAPE tags, expected the three shapes of the notes plus B2")
        return 2
    results = {}
    for name, runner in (("cl /analyze", run_cl), ("clang-cl", run_clang)):
        seed_hits, seed_out = runner(SEED)
        clean_hits, clean_out = runner(CLEAN)
        results[name] = (seed_hits, clean_hits)
        if verbose:
            print(f"--- {name}: seeded file\n{seed_out.strip()}\n--- {name}: clean twin\n{clean_out.strip()}")
    failed = False
    print("analyze_dangling: shape -> analyzers that report it")
    for shape, line in sorted(seed_shapes.items()):
        who = []
        for name, (seed_hits, _clean) in results.items():
            codes = sorted({c for f, ln, c in seed_hits if f == norm(SEED) and ln == line})
            if codes:
                who.append(f"{name} {'/'.join(codes)}")
        print(f"  {shape:3s} line {line:3d}: {', '.join(who) if who else 'MISSED by both'}")
        if not who:
            failed = True
    for name, (_seed, clean_hits) in results.items():
        noisy = [h for h in clean_hits if h[0] == norm(CLEAN)]
        if noisy:
            failed = True
            print(f"  clean twin: {name} reported {len(noisy)} lifetime diagnostic(s): {noisy}")
    if failed:
        print("analyze_dangling: FAIL - a seeded shape is missed or the clean twin is flagged; the gate is not trusted for the missed shapes")
        return 1
    print("analyze_dangling: OK - every seeded shape is caught and the clean twin is silent")
    return 0


def compile_flags(entry: dict) -> list[str]:
    """Defines, include paths and the language level from a compile_commands.json entry (the analyzers need no other flag)."""
    toks = shlex.split(entry.get("command") or " ".join(entry.get("arguments", [])), posix=False)
    return [t for t in toks[1:] if re.match(r"^[-/](D|I|external:I|std:)", t)]


def check_files(files: list[str], cc_path: str | None) -> int:
    entries = {}
    if cc_path:
        with open(cc_path, encoding="utf-8") as f:
            for e in json.load(f):
                entries[norm(os.path.join(e.get("directory", ROOT), e["file"]))] = e
    total = 0
    for fpath in files:
        absf = norm(os.path.join(ROOT, fpath) if not os.path.isabs(fpath) else fpath)
        extra = compile_flags(entries[absf]) if absf in entries else []
        hits, out = run_clang(absf, extra)
        mine = [h for h in hits if h[0].startswith(norm(ROOT))]
        for f, ln, code in sorted(set(mine)):
            print(f"{os.path.relpath(f, ROOT)}({ln}): {code} (clang-cl)")
        if not hits and " error" in out:
            first_lines = " | ".join(out.splitlines()[:6])
            print(f"analyze_dangling: {fpath} did not parse under clang-cl (no verdict): {first_lines}")
        total += len(mine)
    print(f"analyze_dangling: {total} lifetime diagnostic(s) in {len(files)} file(s)")
    return 1 if total else 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--check", nargs="+", metavar="FILE", help="analyze these translation units after the self-test passes")
    ap.add_argument("--compile-commands", help="compile_commands.json for the include paths and defines of --check files")
    args = ap.parse_args()
    rc = self_test(args.verbose)
    if rc or not args.check:
        return rc
    return check_files(args.check, args.compile_commands)


if __name__ == "__main__":
    sys.exit(main())
