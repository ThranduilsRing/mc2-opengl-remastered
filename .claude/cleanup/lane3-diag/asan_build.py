"""Configure + build an ASan tree (build64-asan) in the lane3 worktree. Usage: asan_build.py [configure|build]"""
import os, subprocess, sys
ROOT = "A:/Games/mc2-lane3"
sys.path.insert(0, ROOT + "/scripts")
from bootstrap_worktree_build import cache_args, find_cmake  # noqa
from configure_ninja import NINJA, under_vcvars  # noqa

TREE = "build64-asan"
mode = sys.argv[1] if len(sys.argv) > 1 else "both"
if mode in ("configure", "both"):
    args = [find_cmake(), "-S", ROOT, "-B", f"{ROOT}/{TREE}", "-G", "Ninja",
            f"-DCMAKE_MAKE_PROGRAM={NINJA}", "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
            "-DMC2_PCH=ON", "-DMC2_ASAN=ON"] + cache_args(ROOT)
    rc = under_vcvars(subprocess.list2cmdline(args))
    if rc:
        sys.exit(f"configure failed rc={rc}")
if mode in ("build", "both"):
    rc = under_vcvars(subprocess.list2cmdline([NINJA, "-C", f"{ROOT}/{TREE}", "mc2"]))
    sys.exit(rc)
