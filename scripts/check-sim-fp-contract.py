#!/usr/bin/env python3
"""check-sim-fp-contract -- every sim translation unit is compiled /fp:precise with no FMA contraction.

WHY
---
The SIMID chain (sim_determinism) is bit-identical only while every sim TU rounds the same way. The flags live in
cmake/Mc2SimFpContract.cmake (root project after the vendored subdirectories, and every standalone project through
cmake/Mc2Standalone.cmake). This check replaced the preprocessor guard in mclib/math/glm_config.h, which only ran in TUs
that included GLM and was bypassed under clang-cl.

RULE
----
Source arm: the root CMakeLists.txt includes cmake/Mc2SimFpContract.cmake before its first first-party add_subdirectory,
Mc2Standalone.cmake includes it, and no first-party CMake file names a forbidden flag (/fp:fast, /fp:contract, /fp:strict,
-ffast-math, -ffp-model=fast|strict, -ffp-contract=fast|on; the contract module itself is exempt).
Compile-database arm: for every entry whose source is under code/, mclib/ or GameOS/: no forbidden flag anywhere on the
command line, the last /fp: switch is /fp:precise, and under clang-cl the last -ffp-contract= is off (clang-cl's /fp:precise
alone means contract=on). Default database: build64-ninja/compile_commands.json (the dev tree); more with --db.

USAGE
-----
  py -3 scripts/check-sim-fp-contract.py                 # source arm + dev-tree database; exit 1 on violation
  py -3 scripts/check-sim-fp-contract.py --db TREE/compile_commands.json
  py -3 scripts/check-sim-fp-contract.py --selftest      # rejection probes: every forbidden shape must fail
"""

import argparse
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_DB = os.path.join(ROOT, "build64-ninja", "compile_commands.json")
SIM_DIRS = ("code/", "mclib/", "gameos/")
MODULE = "cmake/Mc2SimFpContract.cmake"
FORBIDDEN_RE = re.compile(r"[-/](?:clang:)?(?:fp:(?:fast|contract|strict)|ffast-math|ffp-model=(?:fast|strict)|"
                          r"ffp-contract=(?:fast|on))\b", re.I)
FP_RE = re.compile(r"^[-/]fp:(\w+)$", re.I)
CONTRACT_RE = re.compile(r"^(?:[-/]clang:)?-ffp-contract=(\w+)$", re.I)
SKIP_DIRS = {".git", ".claude", "3rdparty", "node_modules", "artifacts", "_harness_out", "out"}


def rel_sim(path):
    p = os.path.abspath(path).replace("\\", "/")
    root = ROOT.replace("\\", "/").rstrip("/") + "/"
    if not p.lower().startswith(root.lower()):
        return None
    rel = p[len(root):]
    return rel if rel.lower().startswith(SIM_DIRS) else None


def check_entry(command, is_clang=None):
    """Return a list of problems for one compile command line."""
    tokens = command.split()
    if is_clang is None:
        is_clang = bool(tokens) and os.path.basename(tokens[0].strip('"')).lower().startswith("clang-cl")
    problems = []
    for t in tokens:
        if FORBIDDEN_RE.search(t):
            problems.append(f"forbidden flag {t}")
    fps = [m.group(1).lower() for m in (FP_RE.match(t) for t in tokens) if m]
    if not fps or fps[-1] != "precise":
        problems.append(f"last /fp: switch is {fps[-1] if fps else 'missing'}, needs /fp:precise")
    if is_clang:
        cs = [m.group(1).lower() for m in (CONTRACT_RE.match(t) for t in tokens) if m]
        if not cs or cs[-1] != "off":
            problems.append(f"clang-cl last -ffp-contract= is {cs[-1] if cs else 'missing'}, needs off")
    return problems


def check_db(path):
    with open(path, encoding="utf-8") as fh:
        db = json.load(fh)
    bad, n = [], 0
    for e in db:
        f = e.get("file", "")
        if not os.path.isabs(f):
            f = os.path.join(e.get("directory", ""), f)
        rel = rel_sim(f)
        if rel is None:
            continue
        n += 1
        cmd = e.get("command") or " ".join(e.get("arguments", []))
        for p in check_entry(cmd):
            bad.append(f"{rel}: {p}")
    return n, bad


def cmake_files(root):
    for dp, dns, fns in os.walk(root):
        dns[:] = [d for d in dns if d not in SKIP_DIRS and not d.startswith("build")]
        for fn in fns:
            if fn == "CMakeLists.txt" or fn.endswith(".cmake"):
                yield os.path.join(dp, fn)


def read(path):
    with open(path, encoding="utf-8", errors="replace") as fh:
        return fh.read()


def check_root_text(text):
    inc = text.find("include(cmake/Mc2SimFpContract.cmake)")
    first = text.find('add_subdirectory("./Gates"')
    if inc < 0:
        return ["CMakeLists.txt does not include cmake/Mc2SimFpContract.cmake"]
    if first >= 0 and inc > first:
        return ["CMakeLists.txt includes cmake/Mc2SimFpContract.cmake after its first first-party add_subdirectory"]
    return []


def check_cmake_text(rel, text):
    out = []
    for i, line in enumerate(text.splitlines(), 1):
        code = line.split("#", 1)[0]
        m = FORBIDDEN_RE.search(code)
        if m:
            out.append(f"{rel}:{i}: forbidden flag {m.group(0)}")
    return out


def source_arm():
    bad = check_root_text(read(os.path.join(ROOT, "CMakeLists.txt")))
    if "Mc2SimFpContract.cmake" not in read(os.path.join(ROOT, "cmake", "Mc2Standalone.cmake")):
        bad.append("cmake/Mc2Standalone.cmake does not include Mc2SimFpContract.cmake")
    for path in cmake_files(ROOT):
        rel = os.path.relpath(path, ROOT).replace("\\", "/")
        if rel == MODULE:
            continue
        bad += check_cmake_text(rel, read(path))
    return bad


def selftest():
    msvc = r"C:\cl.exe /nologo /TP /O2 /fp:precise -c A:\x\mclib\stuff\vector4d.cpp"
    clang = r"C:\LLVM\bin\clang-cl.exe /nologo /TP /O2 /fp:precise /clang:-ffp-contract=off -c a.cpp"
    must_pass = [(msvc, None), (clang, None)]
    must_fail = [(msvc + " /fp:fast", None), (msvc + " /fp:contract", None), (msvc + " /fp:strict", None),
                 (msvc.replace(" /fp:precise", ""), None), (msvc.replace("/fp:precise", "-fp:fast"), None),
                 (clang + " /clang:-ffp-contract=fast", None), (clang + " -ffp-contract=fast", None),
                 (clang + " /clang:-ffast-math", None), (clang + " /clang:-ffp-model=fast", None),
                 (clang + " /fp:fast", None), (clang.replace(" /clang:-ffp-contract=off", ""), None),
                 (clang + " /fp:strict", None)]
    errors = []
    for cmd, c in must_pass:
        if check_entry(cmd, c):
            errors.append(f"clean command rejected: {cmd} -> {check_entry(cmd, c)}")
    for cmd, c in must_fail:
        if not check_entry(cmd, c):
            errors.append(f"probe NOT rejected: {cmd}")
    if not check_cmake_text("probe/CMakeLists.txt", 'target_compile_options(x PRIVATE /fp:fast)\n'):
        errors.append("probe NOT rejected: CMake text naming /fp:fast")
    if check_cmake_text("probe/CMakeLists.txt", '# /fp:fast in a comment is not a flag\n'):
        errors.append("comment mention rejected")
    if not check_root_text('add_subdirectory("./Gates" "./out/Gates")\ninclude(cmake/Mc2SimFpContract.cmake)\n'):
        errors.append("probe NOT rejected: contract included after the first add_subdirectory")
    if not check_root_text('add_subdirectory("./Gates" "./out/Gates")\n'):
        errors.append("probe NOT rejected: contract not included")
    for e in errors:
        print("SELFTEST FAIL:", e)
    if not errors:
        print(f"selftest OK: {len(must_fail) + 3} rejection probes fired, {len(must_pass) + 1} clean inputs accepted")
    return 1 if errors else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", action="append", default=[])
    ap.add_argument("--no-default-db", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    bad = source_arm()
    dbs = list(a.db)
    if not a.no_default_db:
        dbs.insert(0, DEFAULT_DB)
    for db in dbs:
        if not os.path.isfile(db):
            bad.append(f"{db}: no compile database (configure the tree first)")
            continue
        n, b = check_db(db)
        if n == 0:
            bad.append(f"{db}: no sim TU found")
        print(f"{db}: {n} sim TUs, {len(b)} problems")
        bad += b
    for b in bad[:60]:
        print("FAIL:", b)
    if len(bad) > 60:
        print(f"... {len(bad) - 60} more")
    if bad:
        return 1
    print("sim fp contract OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
