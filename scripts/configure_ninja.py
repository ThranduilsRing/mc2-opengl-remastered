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
  py -3 scripts/configure_ninja.py --build         # ... then build mc2
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
DEFAULT_TARGETS = ["mc2", "mc2_asset_viewer"] if os.name == "nt" else ["mc2"]


SCCACHE_EXE = os.environ.get("MC2_SCCACHE", "A:/Games/tools/sccache/sccache.exe")
SCCACHE_DIR = os.environ.get("SCCACHE_DIR", "G:/mc2-build/compile-cache")
# Every cached compile goes through this wrapper, not sccache directly: see its docstring (replayed /showIncludes paths).
LAUNCHER_SCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "cc_launcher.py").replace("\\", "/")


# The ONLY list of checkout roots that share compile-cache entries. FIXED: the sccache server reads it once at start, so it must
# never change (lanes get a slot from tools/lane_slot.py instead of a new worktree). Import it; do not copy it.
SLOT_COUNT = 12
SCCACHE_FIXED_ROOTS = (["A:/Games/mc2-main"] + [f"A:/Games/mc2-slot{n:02d}" for n in range(1, SLOT_COUNT + 1)]
                       + ["E:/mc2-installs/_precheck/tree"])


def _norm_root(p):
    return p.strip().replace("\\", "/").rstrip("/").lower()


def root_in_fixed_list(root):
    r = _norm_root(root)
    return any(r == _norm_root(f) or r.startswith(_norm_root(f) + "/") for f in SCCACHE_FIXED_ROOTS)


def compile_cache_env(root):
    """Environment for a cached build. One cache dir for every checkout; SCCACHE_BASEDIRS is the constant SCCACHE_FIXED_ROOTS so the
    absolute path of the checkout is stripped from the preprocessed text and every slot shares entries. The list never changes, so
    the server never needs a restart after a new lane (a root outside the list builds cold: see ensure_cache_server)."""
    legacy = SCCACHE_DIR.replace("\\", "/").rstrip("/").lower() == "e:/mc2-compile-cache"
    return {"SCCACHE_DIR": "G:/mc2-build/compile-cache" if legacy else SCCACHE_DIR,
            "SCCACHE_CACHE_SIZE": "50G" if legacy else os.environ.get("SCCACHE_CACHE_SIZE", "50G"),
            "SCCACHE_BASEDIRS": ";".join(SCCACHE_FIXED_ROOTS)}


def server_base_dirs(shown):
    """Parse the 'Base directories' line of `sccache -s` (already lower-cased) into a set of normalised roots."""
    for ln in shown.splitlines():
        if ln.startswith("base directories"):
            return {_norm_root(x) for x in ln[len("base directories"):].split(",") if x.strip()}
    return set()


def ensure_cache_server(root, env_extra):
    """The sccache server reads SCCACHE_BASEDIRS once, at start. Restart it only when its list differs from the constant
    (one-time migration), and never while a ninja/cl/link process is running."""
    if not root_in_fixed_list(root):
        print(f"[sccache] WARNING: {root} is NOT in the fixed base-dir list; it will not share the compile cache "
              "(cold build). Use tools/lane_slot.py acquire to get a slot worktree.", flush=True)
    env = dict(os.environ, **env_extra)
    shown = subprocess.run([SCCACHE_EXE, "-s"], env=env, capture_output=True, text=True,
                           **console_child.child_kwargs(redirected=True)).stdout.lower()
    wanted_dir = env_extra["SCCACHE_DIR"].replace("\\", "/").lower()
    if server_base_dirs(shown) != {_norm_root(r) for r in SCCACHE_FIXED_ROOTS} or wanted_dir not in shown.replace("\\", "/"):
        if os.name == "nt":
            try:
                inventory = subprocess.run(["tasklist", "/FO", "CSV", "/NH"], capture_output=True, text=True,
                                           timeout=15, **console_child.child_kwargs(redirected=True))
            except (OSError, subprocess.TimeoutExpired):
                # run() kills and reaps the child on timeout. An unknown inventory
                # must never authorize a restart while another lane may compile.
                print("[sccache] restart deferred: process inventory unavailable", flush=True)
                return
            active = inventory.stdout.lower()
            if inventory.returncode or any('"' + name + '"' in active for name in ("ninja.exe", "cl.exe", "link.exe")):
                print("[sccache] restart deferred: a ninja/cl/link process is running", flush=True)
                return
        subprocess.run([SCCACHE_EXE, "--stop-server"], env=env, capture_output=True, **console_child.child_kwargs(redirected=True))
        subprocess.run([SCCACHE_EXE, "--start-server"], env=env, capture_output=True, **console_child.child_kwargs(redirected=True))
        pin_cache_server()


def pin_cache_server():
    """The persistent cache server spawns cl/link itself, so it must carry the compile mask (CPU_SPLIT); never fails a build."""
    try:
        tools = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools")
        if tools not in sys.path:
            sys.path.append(tools)
        from lint_slots import pin_sccache_servers
        return pin_sccache_servers("compile")
    except Exception as exc:
        print(f"[sccache] server pin skipped: {exc}", flush=True)
        return []


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
    return subprocess.run(f'call "{VCVARS}" >nul 2>&1 && {cmdline}', shell=True, env=env,
                          **console_child.child_kwargs()).returncode


def configure(root, tree=DEFAULT_TREE, compile_cache=True, cache_pch=False):
    root = root.replace("\\", "/")
    wanted = compile_cache and os.environ.get("MC2_COMPILE_CACHE") != "0"  # default ON; opt out with --no-compile-cache / MC2_COMPILE_CACHE=0
    compile_cache = wanted and os.path.exists(SCCACHE_EXE)
    if wanted and not compile_cache:
        print(f"[configure_ninja] WARNING: compile cache OFF, sccache.exe not found at {SCCACHE_EXE} (set MC2_SCCACHE); this tree builds uncached and slower")
    args = [find_cmake(), "-S", root, "-B", f"{root}/{tree}", "-G", "Ninja",
            f"-DCMAKE_MAKE_PROGRAM={NINJA}", "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
            # Precompiled std-library header (cmake/pch/): about 30 % less compile time for gameos, mclib and mc2. Only this dev tree
            # turns it on; the clang-tidy / proof tree (tools/build_all_targets.py) keeps it off because clang cannot read an MSVC .pch.
            "-DMC2_PCH=ON", "-DENABLE_MC2FX=ON", "-DENABLE_MC2FX_PREVIEW=ON"] + cache_args(root)
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
                    help="also build mc2 after configuring")
    ap.add_argument("--no-compile-cache", action="store_true",
                    help="opt out of the default: build through sccache (shared G:/mc2-build/compile-cache; SCCACHE_DIR overrides), /Zi -> /Z7, PCH off")
    ap.add_argument("--cache-pch", action="store_true", help="keep the precompiled header on (the cached tree defaults to PCH off)")
    a = ap.parse_args()
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    tree = configure(root, a.build_dir, not a.no_compile_cache, a.cache_pch)
    print(f"[configure_ninja] configured -> {tree}")
    if a.build:
        build(tree)


if __name__ == "__main__":
    main()
