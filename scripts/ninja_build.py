#!/usr/bin/env python3
"""ninja_build.py -- build the Ninja dev tree (build64-ninja/) under the MSVC env.

WHY: ninja needs cl/link on PATH (vcvars64) which a plain shell lacks; this wraps
that so the dev build is one short command. Extra args are passed to ninja
(e.g. -n for a dry run, -j8); non-flag args are targets.

USAGE:
  py -3 scripts/ninja_build.py                       # mc2 + mc2_launcher
  py -3 scripts/ninja_build.py mc2                   # one target
  py -3 scripts/ninja_build.py -n mc2                # dry run: what would rebuild
  py -3 scripts/ninja_build.py --warm --base <ref> -j3   # lane prep: detach HEAD at <ref>, then build this tree in the background
Configure first (once): py -3 scripts/configure_ninja.py (--warm configures a missing tree itself)

--warm returns at once; the build runs detached at BelowNormal priority and logs to <tree>/warm.log, whose last line is
"[warm] done rc=<n> elapsed=<s>s". --base needs a clean tracked tree (it runs `git switch --detach <ref>`). No daemon: one build, then it exits.
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import console_child  # noqa: E402
from configure_ninja import (DEFAULT_TARGETS, DEFAULT_TREE, NINJA, compile_cache_env, configure, ensure_cache_server, tree_uses_cache,
                             under_vcvars)  # noqa: E402


def warm(root, argv, tree):
    """Start `ninja_build.py <argv> --warm-run` detached (log in the tree) and return."""
    base = None
    if "--base" in argv:
        i = argv.index("--base")
        base = argv[i + 1]
        del argv[i:i + 2]
    if base:
        dirty = subprocess.run(["git", "-C", root, "status", "--porcelain", "--untracked-files=no"], capture_output=True, text=True,
                               **console_child.child_kwargs(redirected=True)).stdout.strip()
        if dirty:
            sys.exit("warm: tracked changes present, refusing to switch the base:\n" + dirty)
        switched = subprocess.run(["git", "-C", root, "switch", "--detach", base], capture_output=True, text=True,
                                  **console_child.child_kwargs(redirected=True))
        if switched.returncode:
            sys.exit("warm: git switch --detach failed: " + switched.stderr.strip())
    if not os.path.exists(f"{root}/{tree}/build.ninja"):
        configure(root, tree)
    log = f"{root}/{tree}/warm.log"
    env = dict(os.environ, _MSPDBSRV_ENDPOINT_=f"warm-{os.getpid()}")
    flags = 0x00000008 | 0x00000200 | 0x00004000          # DETACHED_PROCESS | NEW_PROCESS_GROUP | BELOW_NORMAL_PRIORITY_CLASS
    with open(log, "w") as out:
        # background-launch: allow detached console build (ninja), not the game; DETACHED_PROCESS is in `flags`
        child = subprocess.Popen([sys.executable, os.path.abspath(__file__), *argv, "--warm-run"], cwd=root, env=env, stdin=subprocess.DEVNULL,
                                 stdout=out, stderr=subprocess.STDOUT, creationflags=flags if os.name == "nt" else 0)
    print(f"warm: pid {child.pid} building {tree} at {base or 'HEAD'}; log {log}")


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__))).replace("\\", "/")
    argv = sys.argv[1:]
    tree = os.environ.get("MC2_NINJA_TREE") or DEFAULT_TREE
    if "--warm" in argv:
        argv.remove("--warm")
        return warm(root, argv, tree)
    started = None
    if "--warm-run" in argv:
        argv.remove("--warm-run")
        started = time.monotonic()
    if not any(not a.startswith("-") for a in argv):
        argv += DEFAULT_TARGETS
    # The host is shared by several lanes and the owner; an uncapped ninja pegs every core. MC2_BUILD_JOBS (owner default 2,
    # 2026-10-15) caps a build that names no -j of its own.
    if not any(a.startswith("-j") for a in argv):
        argv.insert(0, "-j" + os.environ.get("MC2_BUILD_JOBS", "2"))
    # MC2_NINJA_TREE names another tree under the repo root (lane_merge's verify builds in build-merge/ so it never collides with a lane's own
    # build); it is configured on first use.
    if tree != DEFAULT_TREE and not os.path.exists(f"{root}/{tree}/build.ninja"):
        configure(root, tree)
    env = compile_cache_env(root) if tree_uses_cache(root, tree) else None  # sccache tree (the default; configure_ninja.py --no-compile-cache opts out)
    if env:
        ensure_cache_server(root, env)
    rc = under_vcvars(subprocess.list2cmdline([NINJA, "-C", f"{root}/{tree}"] + argv), env)
    if started is not None:
        print(f"[warm] done rc={rc} elapsed={time.monotonic() - started:.0f}s", flush=True)
    sys.exit(rc)


if __name__ == "__main__":
    main()
