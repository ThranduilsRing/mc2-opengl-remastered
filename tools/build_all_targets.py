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
import console_child  # noqa: E402
from bootstrap_worktree_build import cache_args, find_cmake  # noqa: E402
sys.path.insert(0, os.path.join(ROOT, "cmake"))
import build_driver as bd  # noqa: E402

TREE = "build-alltargets"
EXTRA = ["-DENABLE_MC2FX=ON", "-DENABLE_MC2FX_PREVIEW=ON", "-DENABLE_MC2WEAPON_GUI=ON", "-DMC2_EDITOR_IN_ALL=ON"]
ROOT_TARGETS = ["all", "ui_editor"]
#: (source dir, build dir, target): projects with their own project() that no root build reaches.
STANDALONE = [
    ("tools/tg_import_dump", "build64-tgdump", "tg_import_dump"),
    ("tools/gate_dump", "build64-gate-dump", "gate_dump"),
    ("tools/mech_import_harness", "build64-harness", "mech_import_harness"),
]


def configure(root: str, preset=None, log_dir=None) -> int:
    root = root.replace("\\", "/")
    if preset:
        args = [find_cmake(), "--preset", preset]
        if preset != "clang-cl-nmake":
            args += [f"-DCMAKE_MAKE_PROGRAM={cn.NINJA}"]
    else:
        args = [find_cmake(), "-S", root, "-B", f"{root}/{TREE}", "-G", "Ninja", f"-DCMAKE_MAKE_PROGRAM={cn.NINJA}",
                "-DCMAKE_BUILD_TYPE=RelWithDebInfo"] + EXTRA + cache_args(root)
    rc, _ = bd.run(args, ROOT, "root", "configure", log_dir, vcvars=True)
    if rc:
        print(f"build_all_targets: configure failed (rc={rc})", file=sys.stderr)
    return rc


def build_root(args) -> int:
    root = ROOT.replace("\\", "/")
    tree = bd.tree_name(TREE, args.preset)
    marker = "Makefile" if args.preset == "clang-cl-nmake" else "build.ninja"
    if args.reconfigure or not os.path.exists(os.path.join(ROOT, tree, marker)):
        if configure(root, args.preset, args.log_dir):
            return 1
    try:
        bd.verify_compiler(os.path.join(ROOT, tree), args.preset)
    except RuntimeError as exc:
        print(f"[build_all_targets] FAIL: {exc}", file=sys.stderr)
        return 1
    if args.preset == "clang-cl-nmake":
        cmd = bd.build_args(find_cmake(), f"{root}/{tree}", "RelWithDebInfo", args.targets or ROOT_TARGETS,
                            preset=args.preset, keep_going=args.keep_going)
    else:
        cmd = [cn.NINJA, "-C", f"{root}/{tree}", f"-j{str(args.jobs).lstrip('j')}"]
        if args.keep_going:
            cmd += ["-k", "0"]
        cmd += args.targets or ROOT_TARGETS
    return bd.run(cmd, ROOT, "root", "build", args.log_dir, vcvars=True)[0]


def build_standalone(src: str, tree: str, target: str, jobs: str, preset=None, keep_going=False, log_dir=None) -> int:
    cmake = find_cmake()
    tree = bd.tree_name(tree, preset)
    src_abs, tree_abs = os.path.join(ROOT, src), os.path.join(ROOT, tree)
    print(f"[build_all_targets] standalone {src} -> {tree}", flush=True)
    cfg = ([cmake] + bd.configure_args(ROOT, src_abs, tree_abs, preset) if preset else
           [cmake, "-S", src_abs, "-B", tree_abs, "-G", "Visual Studio 17 2022", "-A", "x64"])
    rc, _ = bd.run(cfg, ROOT, target, "configure", log_dir, vcvars=bool(preset))
    if rc:
        print(f"[build_all_targets] FAIL: cmake generate of {src} (rc={rc})", file=sys.stderr)
        return rc
    try:
        bd.verify_compiler(tree_abs, preset)
    except RuntimeError as exc:
        print(f"[build_all_targets] FAIL: {exc}", file=sys.stderr)
        return 1
    rc, _ = bd.run(bd.build_args(cmake, tree_abs, "RelWithDebInfo", [target], jobs, preset, keep_going),
                   ROOT, target, "build", log_dir, vcvars=bool(preset))
    if rc:
        print(f"[build_all_targets] FAIL: build of {src} (rc={rc})", file=sys.stderr)
    return rc


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--reconfigure", action="store_true")
    ap.add_argument("--keep-going", action="store_true", help="Ninja -k 0 / NMake /K: report every failing target")
    ap.add_argument("--root-only", action="store_true", help="stage 1 only")
    ap.add_argument("--preset", choices=bd.PRESETS, help="compiler preset for EVERY stage; omitted preserves the MSVC trees")
    ap.add_argument("--log-dir", help="complete configure/build logs, one pair per project")
    ap.add_argument("-j", dest="jobs", default="6", help="parallel jobs (default 6)")
    ap.add_argument("targets", nargs="*", help="root-tree targets (default: all ui_editor); implies --root-only")
    args = ap.parse_args(argv)

    failed = []
    if build_root(args):
        failed.append("root tree")
    if args.root_only or args.targets:
        return 1 if failed else 0
    for src, tree, target in STANDALONE:
        if build_standalone(src, tree, target, args.jobs, args.preset, args.keep_going, args.log_dir):
            failed.append(src)
    cmd = [sys.executable, os.path.join(HERE, "build_contract_harnesses.py"), "--unit"]
    if args.preset:
        cmd += ["--preset", args.preset, "-j", args.jobs]
    if args.keep_going:
        cmd += ["--keep-going"]
    if args.log_dir:
        cmd += ["--log-dir", os.path.abspath(args.log_dir)]
    rc = subprocess.run(cmd,
                        **console_child.child_kwargs()).returncode
    if rc:
        failed.append("doctest corpus")
    if failed:
        print("[build_all_targets] FAILED: " + ", ".join(failed), file=sys.stderr)
        return 1
    print("[build_all_targets] every stage built")
    return 0


if __name__ == "__main__":
    sys.exit(main())
