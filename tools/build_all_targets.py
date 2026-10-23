#!/usr/bin/env python3
"""build_all_targets - build EVERY target of the repo: the root tree, its opt-in tools, and the standalone projects.

WHAT (three stages, all by default):
  1. root tree  build-alltargets/  configured with ENABLE_MC2FX, ENABLE_MC2FX_PREVIEW, ENABLE_MC2WEAPON_GUI and MC2_EDITOR_IN_ALL,
     building the CMake `all` target plus ui_editor (EXCLUDE_FROM_ALL): game, EditRel, asset viewer, mc2fx, mc2fx_preview,
     mc2weapon_gui, the tool stubs and the root-tree test targets.
  2. standalone projects that have their own `project()` and build tree: tools/tg_import_dump, tools/gate_dump,
     tools/mech_import_harness (own CMake generate + build each; a stale source list fails at generate time).
  3. tools/build_contract_harnesses.py --unit : the shared doctest corpus (tests/unit/mc2_tests), including native contracts.
WHY: a deletion that is dead for mc2 can still be listed by an opt-in tool or by a standalone project that no root build
touches. The dead-code audit's first all-targets build covered stage 1 only, and tools/tg_import_dump (stage 2) kept listing six
deleted Stuff sources until a reviewer found it.
NOT covered: native Linux builds, and the Viewer (its block in the root CMakeLists is commented out).

  py -3 tools/build_all_targets.py                 # all three stages
  py -3 tools/build_all_targets.py --root-only     # stage 1 only (quick check after a mclib change)
  py -3 tools/build_all_targets.py --reconfigure   # re-run cmake in the root tree (after adding/removing sources)
  py -3 tools/build_all_targets.py -j6 --keep-going
  py -3 tools/build_all_targets.py --no-compile-cache   # the original uncached trees (VS-generator standalone projects, PCH on)

COMPILE CACHE (default on, off with --no-compile-cache or when --preset is given): every stage compiles through the shared sccache
(scripts/configure_ninja.py cache_cmake_args: E:/mc2-installs/_sccache, /Zi -> /Z7, PCH off). The standalone projects and the doctest
corpus use Ninja trees (build-cc-*) in that mode, because the VS generator cannot run a compiler launcher.
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import configure_ninja as cn  # noqa: E402
import console_child  # noqa: E402
from bootstrap_worktree_build import cache_args, find_cmake  # noqa: E402
sys.path.insert(0, os.path.join(ROOT, "cmake"))
import build_driver as bd  # noqa: E402

TREE = "build-alltargets"


def cc_tree(tree: str) -> str:
    """Cached-mode tree of a standalone project: build64-tgdump -> build-cc-tgdump (Ninja; the uncached tree is a VS-generator tree)."""
    return "build-cc-" + tree.removeprefix("build64-")


EXTRA = ["-DENABLE_MC2FX=ON", "-DENABLE_MC2FX_PREVIEW=ON", "-DENABLE_MC2WEAPON_GUI=ON", "-DMC2_EDITOR_IN_ALL=ON"]
ROOT_TARGETS = ["all", "ui_editor"]
#: (source dir, build dir, target): projects with their own project() that no root build reaches.
STANDALONE = [
    ("tools/tg_import_dump", "build64-tgdump", "tg_import_dump"),
    ("tools/gate_dump", "build64-gate-dump", "gate_dump"),
    ("tools/mech_import_harness", "build64-harness", "mech_import_harness"),
]


def configure(root: str, preset=None, log_dir=None, cache=False, build_root=None) -> int:
    root = root.replace("\\", "/")
    tree_path = os.path.join(build_root or root, bd.tree_name(TREE, preset)).replace("\\", "/")
    cache_env = None
    if preset:
        args = [find_cmake(), "--preset", preset]
        if build_root:
            args += ["-B", tree_path]
        if preset != "clang-cl-nmake":
            args += [f"-DCMAKE_MAKE_PROGRAM={cn.NINJA}"]
    else:
        args = [find_cmake(), "-S", root, "-B", tree_path, "-G", "Ninja", f"-DCMAKE_MAKE_PROGRAM={cn.NINJA}",
                "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
                # cl.exe reads its own .pch (only the clang-cl presets cannot): same precompiled std header as the dev tree.
                "-DMC2_PCH=ON"] + EXTRA + cache_args(root)
        if cache:
            # sccache does not cache /Yu compiles: the cached tree is PCH off, like the cached dev tree.
            cache_flags, cache_env = cn.cache_cmake_args(root)
            args[args.index("-DMC2_PCH=ON")] = "-DMC2_PCH=OFF"
            args += cache_flags
    rc, _ = bd.run(args, ROOT, "root", "configure", log_dir, vcvars=True, env=cache_env)
    if rc:
        print(f"build_all_targets: configure failed (rc={rc})", file=sys.stderr)
    return rc


def build_root(args) -> int:
    root = ROOT.replace("\\", "/")
    tree = bd.tree_name(TREE, args.preset)
    output_root = getattr(args, "build_root", None) or root
    tree_path = os.path.join(output_root, tree).replace("\\", "/")
    marker = "Makefile" if args.preset == "clang-cl-nmake" else "build.ninja"
    cache = use_cache(args)
    # A tree configured in the other mode is reconfigured, not silently reused (the -D vars would not change on their own).
    built = os.path.exists(os.path.join(tree_path, marker))
    stale_mode = (not args.preset) and built and (cn.tree_uses_cache(output_root, tree) != cache or (cache and not cn.tree_cache_current(output_root, tree)))
    if args.reconfigure or stale_mode or not built:
        if configure(root, args.preset, args.log_dir, cache, getattr(args, "build_root", None)):
            return 1
    try:
        bd.verify_compiler(tree_path, args.preset)
    except RuntimeError as exc:
        print(f"[build_all_targets] FAIL: {exc}", file=sys.stderr)
        return 1
    if args.preset == "clang-cl-nmake":
        cmd = bd.build_args(find_cmake(), tree_path, "RelWithDebInfo", args.targets or ROOT_TARGETS,
                            preset=args.preset, keep_going=args.keep_going)
    else:
        cmd = [cn.NINJA, "-C", tree_path, f"-j{str(args.jobs).lstrip('j')}"]
        if args.keep_going:
            cmd += ["-k", "0"]
        # Cached mode builds the order seam here too: it links this tree's mc2 objects, so it costs four test TUs and a link.
        cmd += args.targets or ROOT_TARGETS + (["mc2_authored_order_seam"] if cache else [])
    return bd.run(cmd, ROOT, "root", "build", args.log_dir, vcvars=True,
                  env=cn.compile_cache_env(root) if cache else None)[0]


def use_cache(args) -> bool:
    return not args.no_compile_cache and not args.preset


def build_standalone(src: str, tree: str, target: str, jobs: str, preset=None, keep_going=False, log_dir=None,
                     cache=False, build_root=None) -> int:
    cmake = find_cmake()
    tree = cc_tree(tree) if cache else bd.tree_name(tree, preset)
    src_abs, tree_abs = os.path.join(ROOT, src), os.path.join(build_root or ROOT, tree)
    print(f"[build_all_targets] standalone {src} -> {tree}" + (" (compile cache)" if cache else ""), flush=True)
    env = None
    if cache:
        cache_flags, env = cn.cache_cmake_args(ROOT)
        cfg = [cmake, "-S", src_abs.replace("\\", "/"), "-B", tree_abs.replace("\\", "/"), "-G", "Ninja",
               f"-DCMAKE_MAKE_PROGRAM={cn.NINJA}", "-DCMAKE_BUILD_TYPE=RelWithDebInfo"] + cache_flags
    else:
        cfg = ([cmake] + bd.configure_args(ROOT, src_abs, tree_abs, preset) if preset else
               [cmake, "-S", src_abs, "-B", tree_abs, "-G", "Visual Studio 17 2022", "-A", "x64"])
    rc, _ = bd.run(cfg, ROOT, target, "configure", log_dir, vcvars=bool(preset) or cache, env=env)
    if rc:
        print(f"[build_all_targets] FAIL: cmake generate of {src} (rc={rc})", file=sys.stderr)
        return rc
    try:
        bd.verify_compiler(tree_abs, preset)
    except RuntimeError as exc:
        print(f"[build_all_targets] FAIL: {exc}", file=sys.stderr)
        return 1
    rc, _ = bd.run(bd.build_args(cmake, tree_abs, "RelWithDebInfo", [target], jobs, preset or ("ninja" if cache else None),
                                 keep_going),
                   ROOT, target, "build", log_dir, vcvars=bool(preset) or cache, env=env)
    if rc:
        print(f"[build_all_targets] FAIL: build of {src} (rc={rc})", file=sys.stderr)
    return rc


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--reconfigure", action="store_true")
    ap.add_argument("--keep-going", action="store_true", help="Ninja -k 0 / NMake /K: report every failing target")
    ap.add_argument("--root-only", action="store_true", help="stage 1 only")
    ap.add_argument("--preset", choices=bd.PRESETS, help="compiler preset for EVERY stage; omitted preserves the MSVC trees")
    ap.add_argument("--no-compile-cache", action="store_true",
                    help="opt out of the shared sccache: the original trees (PCH on, VS-generator standalone projects)")
    ap.add_argument("--build-root", help="parent directory for every build tree; sources and shared cache stay in place")
    ap.add_argument("--log-dir", help="complete configure/build logs, one pair per project")
    ap.add_argument("-j", dest="jobs", default="6", help="parallel jobs (default 6)")
    ap.add_argument("targets", nargs="*", help="root-tree targets (default: all ui_editor); implies --root-only")
    args = ap.parse_args(argv)
    if args.build_root:
        args.build_root = os.path.abspath(args.build_root).replace("\\", "/")

    failed = []
    t0 = time.monotonic()
    stage_t = []

    def lap(name):
        nonlocal t0
        now = time.monotonic()
        stage_t.append((name, now - t0))
        print(f"[build_all_targets] stage {name}: {now - t0:.0f}s", flush=True)
        t0 = now

    if build_root(args):
        failed.append("root tree")
    lap("root tree")
    if args.root_only or args.targets:
        return 1 if failed else 0
    for src, tree, target in STANDALONE:
        if build_standalone(src, tree, target, args.jobs, args.preset, args.keep_going, args.log_dir, use_cache(args), args.build_root):
            failed.append(src)
        lap(src)
    cmd = [sys.executable, os.path.join(HERE, "build_contract_harnesses.py"), "--unit", "--seam"]
    if args.preset:
        cmd += ["--preset", args.preset, "-j", args.jobs]
    if args.keep_going:
        cmd += ["--keep-going"]
    if use_cache(args):
        cmd += ["--compile-cache", "-j", args.jobs, "--seam-tree", os.path.join(args.build_root, TREE) if args.build_root else TREE, "--seam-prebuilt"]
    if args.build_root:
        cmd += ["--build-root", args.build_root]
    if args.log_dir:
        cmd += ["--log-dir", os.path.abspath(args.log_dir)]
    rc = subprocess.run(cmd,
                        **console_child.child_kwargs()).returncode
    if rc:
        failed.append("doctest corpus")
    lap("doctest corpus")
    print("[build_all_targets] stage times: " + ", ".join(f"{n} {t:.0f}s" for n, t in stage_t), flush=True)
    if failed:
        print("[build_all_targets] FAILED: " + ", ".join(failed), file=sys.stderr)
        return 1
    print("[build_all_targets] every stage built")
    return 0


if __name__ == "__main__":
    sys.exit(main())
