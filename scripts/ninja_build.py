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
from configure_ninja import (DEFAULT_TARGETS, DEFAULT_TREE, NINJA, compile_cache_env, configure, ensure_cache_server, tree_uses_cache,
                             under_vcvars)  # noqa: E402


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__))).replace("\\", "/")
    argv = sys.argv[1:]
    if not any(not a.startswith("-") for a in argv):
        argv += DEFAULT_TARGETS
    # The host is shared by several lanes and the owner; an uncapped ninja pegs every core. MC2_BUILD_JOBS (owner default 2,
    # 2026-10-15) caps a build that names no -j of its own.
    if not any(a.startswith("-j") for a in argv):
        argv.insert(0, "-j" + os.environ.get("MC2_BUILD_JOBS", "2"))
    # MC2_NINJA_TREE names another tree under the repo root (lane_merge's verify builds in build-merge/ so it never collides with a lane's own
    # build); it is configured on first use.
    tree = os.environ.get("MC2_NINJA_TREE") or DEFAULT_TREE
    if tree != DEFAULT_TREE and not os.path.exists(f"{root}/{tree}/build.ninja"):
        configure(root, tree)
    env = compile_cache_env(root) if tree_uses_cache(root, tree) else None  # opt-in sccache tree (configure_ninja.py --compile-cache)
    if env:
        ensure_cache_server(root, env)
    sys.exit(under_vcvars(subprocess.list2cmdline([NINJA, "-C", f"{root}/{tree}"] + argv), env))


if __name__ == "__main__":
    main()
