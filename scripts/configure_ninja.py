#!/usr/bin/env python3
"""configure_ninja.py -- configure the Ninja single-config dev tree (build64-ninja/).

WHY: the VS-generator tree (build64/) relies on MSBuild's mtime/tracker
heuristics: 1-second timestamp granularity misses same-second edits, building
`--target mc2` skips mclib recompiles, and class-layout changes need a
clean-first. Ninja tracks every header via depfiles and hashes its command
lines, so those stale-binary traps cannot occur. build64/ stays as the
release/fallback tree; build64-ninja/ is the day-to-day dev tree.

The tree is also exported with compile_commands.json for clangd. -D vars come
from bootstrap_worktree_build.cache_args so both trees configure identically.

USAGE:
  py -3 scripts/configure_ninja.py                 # configure build64-ninja/
  py -3 scripts/configure_ninja.py --build         # ... then build mc2 + mc2_launcher
  py -3 scripts/configure_ninja.py --build-dir build-analysis   # configure-only variant
Then build with: py -3 scripts/ninja_build.py
"""
import argparse
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bootstrap_worktree_build import cache_args, find_cmake  # noqa: E402

VS = "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools"
VCVARS = VS + "/VC/Auxiliary/Build/vcvars64.bat"
NINJA = VS + "/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe"
DEFAULT_TREE = "build64-ninja"
DEFAULT_TARGETS = ["mc2", "mc2_launcher"]


def under_vcvars(cmdline):
    """Run a command line inside the MSVC x64 environment (cl/link on PATH)."""
    return subprocess.run(f'call "{VCVARS}" >nul && {cmdline}', shell=True).returncode


def configure(root, tree=DEFAULT_TREE):
    root = root.replace("\\", "/")
    args = [find_cmake(), "-S", root, "-B", f"{root}/{tree}", "-G", "Ninja",
            f"-DCMAKE_MAKE_PROGRAM={NINJA}", "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
            # Precompiled std-library header (cmake/pch/): about 30 % less compile time for gameos, mclib and mc2. Only this dev tree
            # turns it on; the clang-tidy / proof tree (tools/build_all_targets.py) keeps it off because clang cannot read an MSVC .pch.
            "-DMC2_PCH=ON"] + cache_args(root)
    rc = under_vcvars(subprocess.list2cmdline(args))
    if rc:
        sys.exit(f"configure_ninja: cmake configure failed (rc={rc})")
    return f"{root}/{tree}"


def build(tree_path, targets=DEFAULT_TARGETS):
    rc = under_vcvars(subprocess.list2cmdline([NINJA, "-C", tree_path] + targets))
    if rc:
        sys.exit(f"configure_ninja: ninja build failed (rc={rc})")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", default=DEFAULT_TREE,
                    help="tree dir name under the repo root (default: %(default)s)")
    ap.add_argument("--build", action="store_true",
                    help="also build mc2 + mc2_launcher after configuring")
    a = ap.parse_args()
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    tree = configure(root, a.build_dir)
    print(f"[configure_ninja] configured -> {tree}")
    if a.build:
        build(tree)


if __name__ == "__main__":
    main()
