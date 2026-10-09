#!/usr/bin/env python3
"""ninja_build.py -- build the Ninja dev tree (build64-ninja/) under the MSVC env.

WHY: ninja needs cl/link on PATH (vcvars64) which a plain shell lacks; this wraps
that so the dev build is one short command. Extra args are passed to ninja
(e.g. -n for a dry run, -j8); non-flag args are targets.

USAGE:
  py -3 scripts/ninja_build.py                       # mc2 + mc2_launcher
  py -3 scripts/ninja_build.py mc2                   # one target
  py -3 scripts/ninja_build.py -n mc2                # dry run: what would rebuild
Configure first (once): py -3 scripts/configure_ninja.py
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from configure_ninja import DEFAULT_TARGETS, DEFAULT_TREE, NINJA, under_vcvars  # noqa: E402


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__))).replace("\\", "/")
    argv = sys.argv[1:]
    if not any(not a.startswith("-") for a in argv):
        argv += DEFAULT_TARGETS
    sys.exit(under_vcvars(subprocess.list2cmdline([NINJA, "-C", f"{root}/{DEFAULT_TREE}"] + argv)))


if __name__ == "__main__":
    main()
