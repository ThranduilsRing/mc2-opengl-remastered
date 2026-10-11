#!/usr/bin/env python3
"""pchbench - configure a clean tree with the precompiled header on or off, build the big targets, and report compile times per target.

WHAT: `build` configures TREE (MC2_PCH=ON|OFF), builds the targets with ninja and prints the wall time; `report` reads two trees' .ninja_log
files and prints, per target, the summed per-object compile time and the change.
WHY: the PCH decision (keep a target's PCH only when it saves at least 15 %) needs the same numbers every time, from a clean build, and the
sum of per-object times is independent of how busy the host happened to be at the wall-clock level.

  py -3 tools/pchbench.py build build-pch-off --pch OFF
  py -3 tools/pchbench.py build build-pch-on  --pch ON
  py -3 tools/pchbench.py report build-pch-off build-pch-on
"""
from __future__ import annotations

import argparse
import collections
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import console_child  # noqa: E402

TARGETS = ("gameos", "mclib", "mc2")
_OBJ = re.compile(r"CMakeFiles/(gameos|mclib|mc2)\.dir/.*\.obj$")


def ninja_log_sums(log_path: str):
    """-> (Counter target -> summed ms of its objects, Counter target -> object count, Counter target -> ms spent building its PCH)."""
    cpu, count, pch = collections.Counter(), collections.Counter(), collections.Counter()
    with open(log_path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            parts = line.rstrip("\n").split("\t")
            if len(parts) < 4 or not parts[0].isdigit():
                continue
            ms, out = int(parts[1]) - int(parts[0]), parts[3].replace("\\", "/")
            if "cmake_pch" in out:
                m = re.search(r"CMakeFiles/(\w+)\.dir", out)
                if m:
                    pch[m.group(1)] += ms
                continue
            m = _OBJ.search(out)
            if m:
                cpu[m.group(1)] += ms
                count[m.group(1)] += 1
    return cpu, count, pch


def gain(off_ms: int, on_ms: int, pch_ms: int = 0) -> float:
    """Fraction of compile time saved (the PCH's own build counts against it)."""
    return 1.0 - (on_ms + pch_ms) / off_ms if off_ms else 0.0


def report(ref_tree: str, pch_tree: str):
    a, na, _ = ninja_log_sums(os.path.join(ref_tree, ".ninja_log"))
    b, nb, bp = ninja_log_sums(os.path.join(pch_tree, ".ninja_log"))
    rows = []
    for t in TARGETS:
        rows.append((t, na[t], nb[t], a[t] / 1000, b[t] / 1000, bp[t] / 1000, 100 * gain(a[t], b[t], bp[t])))
    return rows


def configure_and_build(tree: str, pch: str, targets, clean: bool) -> float:
    sys.path.insert(0, os.path.join(ROOT, "scripts"))
    from bootstrap_worktree_build import cache_args, find_cmake
    from configure_ninja import NINJA, under_vcvars
    tree_abs = tree if os.path.isabs(tree) else os.path.join(ROOT, tree)
    if clean and os.path.isdir(tree_abs):
        shutil.rmtree(tree_abs)
    root = ROOT.replace("\\", "/")
    args = [find_cmake(), "-S", root, "-B", tree_abs.replace("\\", "/"), "-G", "Ninja", f"-DCMAKE_MAKE_PROGRAM={NINJA}",
            "-DCMAKE_BUILD_TYPE=RelWithDebInfo", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", f"-DMC2_PCH={pch}"] + cache_args(root)
    if under_vcvars(subprocess.list2cmdline(args)):
        raise SystemExit("pchbench: configure failed")
    t0 = time.time()
    rc = under_vcvars(subprocess.list2cmdline([NINJA, "-C", tree_abs.replace("\\", "/"), "-k", "0"] + list(targets)))
    wall = time.time() - t0
    if rc:
        raise SystemExit(f"pchbench: build failed (rc={rc})")
    return wall


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build", help="clean-configure TREE with MC2_PCH=ON|OFF and build the targets")
    b.add_argument("tree")
    b.add_argument("--pch", choices=("ON", "OFF"), required=True)
    b.add_argument("targets", nargs="*", default=list(TARGETS))
    b.add_argument("--keep", action="store_true", help="do not delete an existing tree first (incremental)")
    r = sub.add_parser("report", help="per-target compile-time sums of two built trees")
    r.add_argument("ref_tree")
    r.add_argument("pch_tree")
    args = ap.parse_args(argv)
    if args.cmd == "build":
        wall = configure_and_build(args.tree, args.pch, args.targets, clean=not args.keep)
        print(f"BUILD {args.tree} MC2_PCH={args.pch} targets={args.targets} wall={wall:.0f}s")
        return 0
    ref = args.ref_tree if os.path.isabs(args.ref_tree) else os.path.join(ROOT, args.ref_tree)
    pch = args.pch_tree if os.path.isabs(args.pch_tree) else os.path.join(ROOT, args.pch_tree)
    for t, na, nb, off, on, own, g in report(ref, pch):
        print(f"{t:7s} objs {na}/{nb}  compile s off={off:.0f} on={on:.0f} (pch itself {own:.0f})  gain={g:.0f}%")
    return 0


if __name__ == "__main__":
    sys.exit(main())
