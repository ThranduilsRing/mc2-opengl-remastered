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
  py -3 scripts/configure_ninja.py             # shared sccache is the DEFAULT; --no-compile-cache (or MC2_COMPILE_CACHE=0) opts out; see compile_cache_env()
Then build with: py -3 scripts/ninja_build.py
"""
import argparse
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bootstrap_worktree_build import cache_args, find_cmake  # noqa: E402
import console_child  # noqa: E402

VS = "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools"
VCVARS = VS + "/VC/Auxiliary/Build/vcvars64.bat"
NINJA = VS + "/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe"
DEFAULT_TREE = "build64-ninja"
DEFAULT_TARGETS = ["mc2", "mc2_launcher"]


SCCACHE_EXE = os.environ.get("MC2_SCCACHE", "A:/Games/tools/sccache/sccache.exe")
SCCACHE_DIR = os.environ.get("SCCACHE_DIR", "E:/mc2-compile-cache")
# Every cached compile goes through this wrapper, not sccache directly: see its docstring (replayed /showIncludes paths).
LAUNCHER_SCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "cc_launcher.py").replace("\\", "/")


def compile_cache_env(root):
    """Environment for a cached build. One cache dir for every worktree; SCCACHE_BASEDIRS lists every worktree root so the
    absolute path of the checkout is stripped from the preprocessed text and A:/Games/mc2-main and A:/Games/mc2-lane2 share
    entries (a server started earlier keeps its old list: run `sccache --stop-server` after adding a worktree)."""
    roots = {root.replace("\\", "/")}
    try:
        out = subprocess.run(["git", "-C", root, "worktree", "list", "--porcelain"], capture_output=True, text=True,
                             **console_child.child_kwargs(redirected=True)).stdout
        roots |= {ln[9:].strip().replace("\\", "/") for ln in out.splitlines() if ln.startswith("worktree ")}
    except OSError:
        pass
    return {"SCCACHE_DIR": SCCACHE_DIR, "SCCACHE_CACHE_SIZE": os.environ.get("SCCACHE_CACHE_SIZE", "30G"),
            "SCCACHE_BASEDIRS": ";".join(sorted(roots))}


def ensure_cache_server(root, env_extra):
    """The sccache server reads SCCACHE_BASEDIRS once, at start. A worktree created after the server started is not on its list
    and would never share entries, so restart the server when this root is missing (only ever when a new worktree first builds)."""
    env = dict(os.environ, **env_extra)
    want = root.replace("\\", "/").lower().rstrip("/") + "/"
    shown = subprocess.run([SCCACHE_EXE, "-s"], env=env, capture_output=True, text=True,
                           **console_child.child_kwargs(redirected=True)).stdout.lower()
    if want not in shown:
        subprocess.run([SCCACHE_EXE, "--stop-server"], env=env, capture_output=True, **console_child.child_kwargs(redirected=True))
        subprocess.run([SCCACHE_EXE, "--start-server"], env=env, capture_output=True, **console_child.child_kwargs(redirected=True))


def cache_cmake_args(root):
    """(cmake -D args, env) that put a tree behind the shared compile cache; the caller still decides PCH (the cache needs it off).
    Single-sourced for every tree that wants the cache: the dev tree, tools/build_all_targets.py and the harness corpus."""
    env = compile_cache_env(root)
    ensure_cache_server(root.replace("\\", "/"), env)
    launcher = ";".join([sys.executable.replace("\\", "/"), "-S", "-E", LAUNCHER_SCRIPT, root.replace("\\", "/"), SCCACHE_EXE])
    return ["-DMC2_COMPILE_CACHE=ON", f"-DCMAKE_C_COMPILER_LAUNCHER={launcher}",
            f"-DCMAKE_CXX_COMPILER_LAUNCHER={launcher}"], env


def tree_cache_current(root, tree):
    """True when the tree was configured with the include-path-rewriting launcher (a tree configured before it must be reconfigured)."""
    try:
        with open(f"{root}/{tree}/CMakeCache.txt", encoding="utf-8", errors="replace") as f:
            return "cc_launcher.py" in f.read()
    except OSError:
        return False


def tree_uses_cache(root, tree):
    try:
        with open(f"{root}/{tree}/CMakeCache.txt", encoding="utf-8", errors="replace") as f:
            return "MC2_COMPILE_CACHE:BOOL=ON" in f.read()
    except OSError:
        return False


def under_vcvars(cmdline, env_extra=None):
    """Run a command line inside the MSVC x64 environment (cl/link on PATH)."""
    env = dict(os.environ, **env_extra) if env_extra else None
    return subprocess.run(f'call "{VCVARS}" >nul && {cmdline}', shell=True, env=env).returncode


def configure(root, tree=DEFAULT_TREE, compile_cache=True, cache_pch=False):
    root = root.replace("\\", "/")
    compile_cache = compile_cache and os.environ.get("MC2_COMPILE_CACHE") != "0" and os.path.exists(SCCACHE_EXE)  # default ON; opt out with --no-compile-cache / MC2_COMPILE_CACHE=0
    args = [find_cmake(), "-S", root, "-B", f"{root}/{tree}", "-G", "Ninja",
            f"-DCMAKE_MAKE_PROGRAM={NINJA}", "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
            # Precompiled std-library header (cmake/pch/): about 30 % less compile time for gameos, mclib and mc2. Only this dev tree
            # turns it on; the clang-tidy / proof tree (tools/build_all_targets.py) keeps it off because clang cannot read an MSVC .pch.
            "-DMC2_PCH=ON"] + cache_args(root)
    env = None
    if compile_cache:
        # sccache does not cache precompiled-header users (/Yu), so the cached tree defaults to PCH off (--cache-pch keeps it).
        if not cache_pch:
            args[args.index("-DMC2_PCH=ON")] = "-DMC2_PCH=OFF"
        cache_flags, env = cache_cmake_args(root)
        args += cache_flags
    rc = under_vcvars(subprocess.list2cmdline(args), env)
    if rc:
        sys.exit(f"configure_ninja: cmake configure failed (rc={rc})")
    return f"{root}/{tree}"


def build(tree_path, targets=DEFAULT_TARGETS):
    root, tree = os.path.dirname(tree_path), os.path.basename(tree_path)
    env = compile_cache_env(root) if tree_uses_cache(root, tree) else None
    if env:
        ensure_cache_server(root, env)
    rc = under_vcvars(subprocess.list2cmdline([NINJA, "-C", tree_path] + targets), env)
    if rc:
        sys.exit(f"configure_ninja: ninja build failed (rc={rc})")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", default=DEFAULT_TREE,
                    help="tree dir name under the repo root (default: %(default)s)")
    ap.add_argument("--build", action="store_true",
                    help="also build mc2 + mc2_launcher after configuring")
    ap.add_argument("--compile-cache", action="store_true", help="(default now; kept for old command lines)")
    ap.add_argument("--no-compile-cache", action="store_true",
                    help="opt out of the default: build through sccache (shared E:/mc2-compile-cache, Defender-excluded; SCCACHE_DIR overrides), /Zi -> /Z7, PCH off")
    ap.add_argument("--cache-pch", action="store_true", help="with --compile-cache: keep the precompiled header on")
    a = ap.parse_args()
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    tree = configure(root, a.build_dir, not a.no_compile_cache, a.cache_pch)
    print(f"[configure_ninja] configured -> {tree}")
    if a.build:
        build(tree)


if __name__ == "__main__":
    main()
