#!/usr/bin/env python3
"""bootstrap_worktree_build.py -- one-command CMake configure for a fresh worktree.

WHY THIS EXISTS: the prebuilt 3rdparty binary deps (SDL2/GLEW/zlib/ffmpeg cmake
configs + libs + headers) live under 3rdparty/{cmake,lib,include,ffmpeg-lgpl-win64}
and are UNTRACKED by git. A brand-new `git worktree add` therefore has source but
no deps and no build64, and a bare `cmake -S . -B build64` fails one-find_package-
at-a-time (SDL2 -> GLEW -> ZLIB -> MC2_IMGUI). We kept rediscovering the exact
-D flags every time. This script captures them ONCE.

WHAT IT DOES (idempotent):
  1. Resolves the worktree root (parent of this scripts/ dir, or --worktree).
  2. If 3rdparty binary deps are missing, copies them from a DONOR worktree that
     has them (auto-discovered, or --donor). Tracked source subdirs (assimp,
     imgui, tracy, ...) are already present via git and are left alone.
  3. Runs `cmake configure` with the full, known-good -D var set, all rooted at
     THIS worktree's own 3rdparty.
  4. Prints the build command.

USAGE:
  py -3 scripts/bootstrap_worktree_build.py            # configure this worktree
  py -3 scripts/bootstrap_worktree_build.py --build    # configure THEN build mc2
  py -3 scripts/bootstrap_worktree_build.py --donor <path-to-worktree-with-3rdparty>

After this, the mc2-build skill / `cmake --build build64 --config RelWithDebInfo
--target mc2` works normally.
"""

import argparse
import os
import shutil
import subprocess
import sys

CMAKE = ("C:/Program Files (x86)/Microsoft Visual Studio/2022/Common7/IDE/"
         "CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe")
# BuildTools install (the canonical one on this box):
CMAKE_BT = ("C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/"
            "IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe")

# Untracked prebuilt-dependency dirs a fresh worktree lacks. Copied from a donor.
DEP_DIRS = ["cmake", "lib", "include", "ffmpeg-lgpl-win64"]
# Sentinel that proves a worktree's 3rdparty deps are populated.
DONOR_SENTINEL = os.path.join("3rdparty", "lib", "x64", "glew32.lib")


def log(m):
    print(f"[bootstrap-build] {m}")


def fail(m):
    print(f"[bootstrap-build] FAIL: {m}", file=sys.stderr)
    sys.exit(1)


def repo_root():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def find_cmake():
    for c in (CMAKE_BT, CMAKE):
        if os.path.isfile(c):
            return c
    # last resort: PATH
    found = shutil.which("cmake")
    if found:
        return found
    fail("cmake.exe not found (looked in VS2022 BuildTools + PATH)")


def find_donor(worktree, explicit):
    """A sibling worktree (or main repo) whose 3rdparty deps are populated."""
    if explicit:
        if os.path.isfile(os.path.join(explicit, DONOR_SENTINEL)):
            return explicit
        fail(f"--donor {explicit} has no {DONOR_SENTINEL}")
    # search: the worktrees dir this worktree lives in, plus the main repo root.
    candidates = []
    wt_parent = os.path.dirname(worktree)            # .../.claude/worktrees
    if os.path.isdir(wt_parent):
        for name in os.listdir(wt_parent):
            p = os.path.join(wt_parent, name)
            if p != worktree and os.path.isdir(p):
                candidates.append(p)
    # main repo root = three levels up from .claude/worktrees/<wt>
    main_repo = os.path.dirname(os.path.dirname(wt_parent))
    candidates.append(main_repo)
    for c in candidates:
        if os.path.isfile(os.path.join(c, DONOR_SENTINEL)):
            return c
    return None


def ensure_deps(worktree, donor_arg):
    tp = os.path.join(worktree, "3rdparty")
    missing = [d for d in DEP_DIRS if not os.path.exists(os.path.join(tp, d))]
    # include/ and ffmpeg may exist (tracked-ish); only copy what's truly absent,
    # but always require the glew sentinel to be present at the end.
    if not missing and os.path.isfile(os.path.join(worktree, DONOR_SENTINEL)):
        log("3rdparty deps already present.")
        return
    donor = find_donor(worktree, donor_arg)
    if not donor:
        fail("no donor worktree with populated 3rdparty/ found; pass --donor <path>")
    log(f"donor: {donor}")
    for d in DEP_DIRS:
        src = os.path.join(donor, "3rdparty", d)
        dst = os.path.join(tp, d)
        if os.path.exists(dst):
            continue
        if not os.path.exists(src):
            log(f"  donor lacks 3rdparty/{d} (skip)")
            continue
        log(f"  copying 3rdparty/{d} ...")
        shutil.copytree(src, dst)
    if not os.path.isfile(os.path.join(worktree, DONOR_SENTINEL)):
        fail(f"after copy, {DONOR_SENTINEL} still missing -- donor incomplete?")


def cache_args(worktree):
    """The -D cache vars every configure of this repo needs (any generator).

    Single-sourced here so the VS tree (build64), the Ninja dev tree
    (build64-ninja) and the clangd analysis tree cannot drift apart: a
    configure that differs in even one of these silently links different deps.
    """
    tp = worktree.replace("\\", "/") + "/3rdparty"
    return [
        "-DMC2_IMGUI=ON",
        f"-DCMAKE_PREFIX_PATH={tp}/cmake",
        f"-DGLEW_INCLUDE_DIR={tp}/include",
        f"-DGLEW_LIBRARY={tp}/lib/x64/glew32.lib",
        f"-DGLEW_SHARED_LIBRARY_RELEASE={tp}/lib/x64/glew32.lib",
        f"-DGLEW_STATIC_LIBRARY_RELEASE={tp}/lib/x64/glew32s.lib",
        f"-DZLIB_INCLUDE_DIR={tp}/include",
        f"-DZLIB_LIBRARY={tp}/lib/x64/zlib.lib",
    ]


def configure(worktree, cmake):
    br = worktree.replace("\\", "/")
    args = [cmake, "-S", br, "-B", f"{br}/build64",
            "-G", "Visual Studio 17 2022", "-A", "x64"] + cache_args(br)
    log("configuring ...")
    r = subprocess.run(args, creationflags=(getattr(subprocess, "CREATE_NO_WINDOW", 0) if not (sys.stdout and sys.stdout.isatty()) else 0))
    if r.returncode != 0:
        fail("cmake configure failed (see output above)")
    log("configure OK -> build64/")


def build(worktree, cmake):
    args = [cmake, "--build", f"{worktree}/build64",
            "--config", "RelWithDebInfo", "--target", "mc2"]
    log("building mc2 (RelWithDebInfo) ...")
    r = subprocess.run(args, creationflags=(getattr(subprocess, "CREATE_NO_WINDOW", 0) if not (sys.stdout and sys.stdout.isatty()) else 0))
    if r.returncode != 0:
        fail("build failed (see output above)")
    log("build OK -> build64/RelWithDebInfo/mc2.exe")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--worktree", default=repo_root(),
                    help="worktree root (default: parent of this scripts/ dir)")
    ap.add_argument("--donor", default=None,
                    help="worktree to copy 3rdparty deps from (auto if omitted)")
    ap.add_argument("--build", action="store_true",
                    help="also build the mc2 target after configuring")
    a = ap.parse_args()
    worktree = os.path.abspath(a.worktree)
    if not os.path.isfile(os.path.join(worktree, "CMakeLists.txt")):
        fail(f"{worktree} has no CMakeLists.txt -- not a worktree root")
    cmake = find_cmake()
    ensure_deps(worktree, a.donor)
    configure(worktree, cmake)
    if a.build:
        build(worktree, cmake)
    else:
        log("next: cmake --build build64 --config RelWithDebInfo --target mc2")


if __name__ == "__main__":
    main()
