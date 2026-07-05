#!/usr/bin/env python3
"""DEV-EFFICIENCY-BOOTSTRAP-1 #3 — one-command contract-harness builder.

Builds every registered C++ contract harness into a CONSISTENT canonical dir and
(optionally) runs the suite. Eliminates the recurring "MISSING because I built
into the wrong build dir / forgot a harness / used build64-contract_smoke" class.

Zero registry duplication: the harness set comes from run_contract_tests.py's
REGISTERED_HARNESSES, and every harness lives at tools/<exe_name>/, so the source
dir is derived (tools/<name>) and each builds into build64-h-<name>. The suite is
then run with --build-dir pointed at exactly those dirs (no reliance on the
runner's irregular default dir names).

Usage:
  py -3 tools/build_contract_harnesses.py            # build all harnesses
  py -3 tools/build_contract_harnesses.py --run      # build then run the suite
  py -3 tools/build_contract_harnesses.py --run --only icon_atlas_harness
  py -3 tools/build_contract_harnesses.py --config RelWithDebInfo
  py -3 tools/build_contract_harnesses.py --unit-only --run   # doctest unit tier only
  py -3 tools/build_contract_harnesses.py --run --unit        # both tiers

Exit 0 iff every selected harness built (and, with --run, the suite passed).
CLI-only by design (writes build dirs / runs slow builds) — never wired into MCP.
"""

import argparse
import os
import subprocess
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUNNER = os.path.join(REPO_ROOT, "tools", "run_contract_tests.py")

# Canonical VS CMake (matches the project rails); fall back to PATH cmake.
_VS_CMAKE = ("C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/"
             "Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe")


def cmake_exe():
    return _VS_CMAKE if os.path.isfile(_VS_CMAKE) else "cmake"


def registered_harnesses():
    """Import the runner and read its explicit C++ registry (single source)."""
    sys.path.insert(0, os.path.join(REPO_ROOT, "tools"))
    import run_contract_tests as rct  # noqa: E402
    return list(rct.REGISTERED_HARNESSES)


def build_one(name, config):
    src = os.path.join(REPO_ROOT, "tools", name)
    bdir = os.path.join(REPO_ROOT, f"build64-h-{name}")
    if not os.path.isdir(src):
        return False, f"no source dir tools/{name}/ (not a standalone C++ harness?)"
    cm = cmake_exe()
    cfg = subprocess.run([cm, "-S", src, "-B", bdir, "-A", "x64"],
                         capture_output=True, text=True, cwd=REPO_ROOT)
    if cfg.returncode != 0:
        return False, "configure failed:\n" + cfg.stderr.strip()[-600:]
    bld = subprocess.run([cm, "--build", bdir, "--config", config],
                         capture_output=True, text=True, cwd=REPO_ROOT)
    if bld.returncode != 0:
        tail = (bld.stdout + bld.stderr).strip().splitlines()[-8:]
        return False, "build failed:\n" + "\n".join(tail)
    return True, bdir


# The doctest unit tier (tests/unit/mc2_tests) is a DIFFERENT shape from the
# tools/<name>/ contract harnesses above: one doctest binary, no per-harness
# --json/status contract, source at tests/unit/ (not tools/<name>/). It is the
# cheaper tier for GL-free engine logic under test (MINIMAL-VIABLE-HARNESS-1/2).
# Wired here so a single command covers both tiers; kept on its own path so it
# never pollutes REGISTERED_HARNESSES (which would report MISSING/FAIL for it by
# construction). Exit code is authoritative: doctest returns non-zero on any
# failed assertion.
def build_and_run_unit_tests(config, run):
    src = os.path.join(REPO_ROOT, "tests", "unit")
    bdir = os.path.join(REPO_ROOT, "build64-tests")
    cm = cmake_exe()
    cfg = subprocess.run([cm, "-S", src, "-B", bdir, "-A", "x64"],
                         capture_output=True, text=True, cwd=REPO_ROOT)
    if cfg.returncode != 0:
        return False, "configure failed:\n" + cfg.stderr.strip()[-600:]
    bld = subprocess.run([cm, "--build", bdir, "--config", config,
                          "--target", "mc2_tests"],
                         capture_output=True, text=True, cwd=REPO_ROOT)
    if bld.returncode != 0:
        tail = (bld.stdout + bld.stderr).strip().splitlines()[-8:]
        return False, "build failed:\n" + "\n".join(tail)
    if not run:
        return True, "built (not run; pass --run to execute)"
    exe = os.path.join(bdir, config, "mc2_tests" + (".exe" if os.name == "nt" else ""))
    if not os.path.isfile(exe):
        exe = os.path.join(bdir, "mc2_tests" + (".exe" if os.name == "nt" else ""))
    rc = subprocess.run([exe], cwd=REPO_ROOT).returncode
    return (rc == 0), ("doctest passed" if rc == 0 else f"doctest failed (rc={rc})")


def main():
    ap = argparse.ArgumentParser(description="Build (and optionally run) contract harnesses.")
    ap.add_argument("--run", action="store_true", help="run the suite after building")
    ap.add_argument("--only", action="append", default=None,
                    help="build only the named harness(es) (repeatable)")
    ap.add_argument("--config", default="RelWithDebInfo")
    ap.add_argument("--unit", action="store_true",
                    help="also build the doctest unit tier (tests/unit/mc2_tests); "
                         "with --run, execute it too")
    ap.add_argument("--unit-only", action="store_true",
                    help="build/run ONLY the doctest unit tier, skip the C++ harnesses")
    args = ap.parse_args()

    # --unit-only: the cheap doctest tier by itself (MINIMAL-VIABLE-HARNESS-*).
    if args.unit_only:
        ok, info = build_and_run_unit_tests(args.config, args.run)
        print(f"  [{'PASS' if ok else 'FAIL'}] mc2_tests (doctest unit tier): {info}")
        return 0 if ok else 1

    names = registered_harnesses()
    if args.only:
        names = [n for n in names if n in set(args.only)]
        if not names:
            print(f"[build-harnesses] none of {args.only} are registered", file=sys.stderr)
            return 2

    build_dirs = []
    failures = []
    for n in names:
        ok, info = build_one(n, args.config)
        if ok:
            build_dirs.append(info)
            print(f"  [BUILT] {n}")
        else:
            failures.append((n, info))
            print(f"  [FAIL ] {n}: {info.splitlines()[0]}", file=sys.stderr)
            if "glew32.lib" in info or "3rdparty" in info:
                print("          (hint: unzip 3rdparty.zip in this worktree for "
                      "harnesses that link glew, e.g. render_state)", file=sys.stderr)

    print(f"\n[build-harnesses] built {len(build_dirs)}/{len(names)}; "
          f"{len(failures)} failed")

    # Optional doctest unit tier alongside the C++ harnesses.
    unit_ok = True
    if args.unit:
        u_ok, u_info = build_and_run_unit_tests(args.config, args.run)
        unit_ok = u_ok
        print(f"  [{'PASS' if u_ok else 'FAIL'}] mc2_tests (doctest unit tier): {u_info}")

    if failures and not args.run:
        return 1

    if args.run:
        cmd = [sys.executable, RUNNER]
        for bd in build_dirs:
            cmd += ["--build-dir", os.path.basename(bd)]
        print("\n[build-harnesses] running suite...")
        rc = subprocess.run(cmd, cwd=REPO_ROOT).returncode
        return 1 if (failures or rc != 0 or not unit_ok) else 0

    return 1 if (failures or not unit_ok) else 0


if __name__ == "__main__":
    raise SystemExit(main())
