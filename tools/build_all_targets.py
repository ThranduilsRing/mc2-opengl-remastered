#!/usr/bin/env python3
"""build_all_targets - build EVERY target of the repo, opt-in tools included, in one Ninja tree.

WHAT: configures build-alltargets/ with ENABLE_MC2FX, ENABLE_MC2FX_PREVIEW, ENABLE_MC2WEAPON_GUI and
MC2_EDITOR_IN_ALL on, then builds the CMake `all` target (game, EditRel, ui_editor, asset viewer, mc2fx,
mc2fx_preview, mc2weapon_gui, the tool stubs and the test harnesses).
WHY: the default dev tree builds mc2 and the launcher only. A deletion that is dead for mc2 can still be
referenced by an opt-in tool (mc2fx globs mclib/mlr and mclib/stuff), and the dead-code audit needs a
build that shows it before the commit, not after.

  py -3 tools/build_all_targets.py                 # configure if needed, then build everything
  py -3 tools/build_all_targets.py --reconfigure   # re-run cmake (after adding/removing sources)
  py -3 tools/build_all_targets.py -j6 --keep-going
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import configure_ninja as cn  # noqa: E402
from bootstrap_worktree_build import cache_args, find_cmake  # noqa: E402

TREE = "build-alltargets"
EXTRA = ["-DENABLE_MC2FX=ON", "-DENABLE_MC2FX_PREVIEW=ON", "-DENABLE_MC2WEAPON_GUI=ON", "-DMC2_EDITOR_IN_ALL=ON"]


def configure(root: str) -> None:
    root = root.replace("\\", "/")
    args = [find_cmake(), "-S", root, "-B", f"{root}/{TREE}", "-G", "Ninja", f"-DCMAKE_MAKE_PROGRAM={cn.NINJA}",
            "-DCMAKE_BUILD_TYPE=RelWithDebInfo"] + EXTRA + cache_args(root)
    rc = cn.under_vcvars(subprocess.list2cmdline(args))
    if rc:
        sys.exit(f"build_all_targets: configure failed (rc={rc})")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--reconfigure", action="store_true")
    ap.add_argument("--keep-going", action="store_true", help="ninja -k 0: report every failing target")
    ap.add_argument("-j", dest="jobs", default="6", help="parallel jobs (default 6)")
    ap.add_argument("targets", nargs="*", help="default: all")
    args = ap.parse_args(argv)
    root = ROOT.replace("\\", "/")
    if args.reconfigure or not os.path.exists(os.path.join(ROOT, TREE, "build.ninja")):
        configure(root)
    cmd = [cn.NINJA, "-C", f"{root}/{TREE}", f"-j{str(args.jobs).lstrip('j')}"]
    if args.keep_going:
        cmd += ["-k", "0"]
    cmd += args.targets or ["all"]
    return cn.under_vcvars(subprocess.list2cmdline(cmd))


if __name__ == "__main__":
    sys.exit(main())
