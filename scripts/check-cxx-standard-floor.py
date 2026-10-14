#!/usr/bin/env python3
"""check-cxx-standard-floor -- standalone CMake projects that read gates must build as C++20+.

WHY
---
The typed gate registry (Gates/gate.h) needs class-type template parameters and consteval.
The in-tree projects inherit MC2_CXX_STANDARD from the root; the standalone ones (tests/unit,
tools/mech_import_harness, tools/tg_import_dump, ...) own a project() and used to pin C++17. The day an
engine file they compile reads a gate they stop building, far from the commit that caused it.

RULE
----
A CMake file with its own project() that is NOT added from the root CMakeLists.txt is a
standalone project. If any .cpp it lists (or that a listed .cpp #includes as a .cpp) contains a
raw MC2_ environment read -- getenv / GetEnvironmentVariable with an "MC2_ literal -- or
mentions Gates/ or gate::get, its effective C++ standard must be 20 or newer. Effective
standard = the include of cmake/Mc2Standalone.cmake (23), else the largest
CMAKE_CXX_STANDARD / CXX_STANDARD / cxx_std_N it sets, else 14 (CMake default for MSVC).
The root CMakeLists.txt must not offer 17 as an MC2_CXX_STANDARD option.

USAGE
-----
  py -3 scripts/check-cxx-standard-floor.py            # check; exit 1 on violation
  py -3 scripts/check-cxx-standard-floor.py --list     # print every standalone project and its verdict
"""

import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SKIP_DIRS = {"build", "out", "3rdparty", ".git", ".claude", "node_modules", "artifacts", "_harness_out"}

PROJECT_RE = re.compile(r"(?mi)^\s*project\s*\(")
SOURCE_RE = re.compile(r"[\w\${}/.\-]+\.cpp")
GATE_READ_RE = re.compile(
    r'(?:getenv|_wgetenv|GetEnvironmentVariable[AW]?)\s*\(\s*"MC2_|Gates/|gate::get\s*<')
CPP_INCLUDE_RE = re.compile(r'#\s*include\s+"([^"]+\.cpp)"')
STD_RES = (re.compile(r"CMAKE_CXX_STANDARD\s+(\d+)"), re.compile(r"\bCXX_STANDARD\s+(\d+)"),
           re.compile(r"cxx_std_(\d+)"))
SHARED_INCLUDE = "Mc2Standalone.cmake"


def find_cmake_files(root):
    out = []
    for dp, dns, fns in os.walk(root):
        dns[:] = [d for d in dns if d not in SKIP_DIRS and not d.startswith("build64")]
        if "CMakeLists.txt" in fns:
            out.append(os.path.join(dp, "CMakeLists.txt"))
    return out


def read(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            return fh.read()
    except OSError:
        return ""


def root_subdirs(root):
    """Directories the root CMakeLists adds (their project() inherits the root standard)."""
    txt = read(os.path.join(root, "CMakeLists.txt"))
    dirs = set()
    for m in re.finditer(r'add_subdirectory\s*\(\s*"?([^"\s)]+)', txt):
        d = m.group(1).replace("\\", "/")
        d = re.sub(r"^\./", "", d).rstrip("/")
        if d and "$" not in d:
            dirs.add(os.path.normpath(os.path.join(root, d)))
    return dirs


def effective_standard(text):
    if SHARED_INCLUDE in text:
        return 23
    vals = [int(v) for rx in STD_RES for v in rx.findall(text)]
    return max(vals) if vals else 14


def listed_sources(cmake_dir, text, root):
    found = []
    for tok in sorted(set(SOURCE_RE.findall(text))):
        rel = re.sub(r"^\$\{[A-Za-z_0-9]+\}/", "", tok)
        rel = re.sub(r"\$\{[A-Za-z_0-9]+\}", "", rel).lstrip("/")
        for base in (cmake_dir, root):
            p = os.path.normpath(os.path.join(base, rel))
            if os.path.isfile(p):
                found.append(p)
                break
    return found


def reads_gates(path, root, seen=None, depth=0):
    seen = seen if seen is not None else set()
    if path in seen or depth > 3:
        return None
    seen.add(path)
    txt = read(path)
    m = GATE_READ_RE.search(txt)
    if m:
        return "%s (%s)" % (os.path.relpath(path, root).replace("\\", "/"), m.group(0).strip()[:30])
    for inc in CPP_INCLUDE_RE.findall(txt):
        cand = os.path.normpath(os.path.join(os.path.dirname(path), inc))
        if os.path.isfile(cand):
            hit = reads_gates(cand, root, seen, depth + 1)
            if hit:
                return hit
    return None


def analyse(root):
    """-> list of (relpath, standard, gate_reader_or_None, verdict)."""
    inherited = root_subdirs(root)
    rows = []
    for cm in find_cmake_files(root):
        d = os.path.dirname(cm)
        if os.path.normpath(cm) == os.path.normpath(os.path.join(root, "CMakeLists.txt")):
            continue
        text = read(cm)
        if not PROJECT_RE.search(text) or os.path.normpath(d) in inherited:
            continue
        std = effective_standard(text)
        hit = None
        for src in listed_sources(d, text, root):
            hit = reads_gates(src, root)
            if hit:
                break
        verdict = "ok" if (hit is None or std >= 20) else "FAIL"
        rows.append((os.path.relpath(cm, root).replace("\\", "/"), std, hit, verdict))
    return rows


def root_offers_17(root):
    txt = read(os.path.join(root, "CMakeLists.txt"))
    for line in txt.splitlines():
        if "MC2_CXX_STANDARD" in line and re.search(r'\(?17\s*\|', line + " ") and "CACHE STRING" in line:
            return line.strip()
    return None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--root", default=ROOT)
    ap.add_argument("--list", action="store_true")
    args = ap.parse_args(argv)
    rows = analyse(args.root)
    bad = [r for r in rows if r[3] == "FAIL"]
    offer = root_offers_17(args.root)
    if args.list:
        for rel, std, hit, verdict in rows:
            print("%-52s c++%-3s %-4s %s" % (rel, std, verdict, hit or ""))
    if bad or offer:
        print("CXX-STANDARD-FLOOR FAILED")
        for rel, std, hit, _v in bad:
            print("  - %s builds as C++%s but compiles %s: include cmake/Mc2Standalone.cmake (C++20+)" % (rel, std, hit))
        if offer:
            print("  - the root CMakeLists.txt still offers C++17 as an MC2_CXX_STANDARD option: %s" % offer)
        return 1
    print("check-cxx-standard-floor: OK (%d standalone project(s), %d read gates)"
          % (len(rows), sum(1 for r in rows if r[2])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
