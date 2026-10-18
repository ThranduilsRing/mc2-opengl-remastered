#!/usr/bin/env python3
"""Build/run doctest, the engine-linked order seam and Python contracts.

--only/--asan restrict runs to standalone doctests; --unit-only skips Python.
--unit is compatibility syntax. RelWithDebInfo retains release fatal-path checks.
--compile-cache builds both trees (build-cc-tests, build-cc-order-seam) with Ninja through the shared sccache instead of the
VS-generator trees; tools/build_all_targets.py passes it by default.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import time

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "scripts"))
import console_child
import configure_ninja as cn
sys.path.insert(0, str(REPO_ROOT / "cmake"))
import build_driver as bd

# This migration index preserves named selection and coverage of all sixteen contracts.
REGISTERED_HARNESSES = [
    "contract_smoke_harness",
    "shader_contract_harness",
    "render_state_contract_harness",
    "objmgr_contract_harness",
    "ibl_registry_contract_harness",
    "render_pass_table_harness",
    "logistics_csv_harness",
    "camera_frustum_harness",
    "mech_texname_harness",
    "icon_atlas_harness",
    "timing_math_harness",
    "job_system_harness",
    "terrain_daylight_harness",
    "object_light_row_harness",
    "class_light_band_harness",
    "glstate_guard_harness",
]
PY_HARNESSES = [
    "tools/deploy_asset_contract_harness/deploy_asset_contract_harness.py",
    "tools/deploy_release_tree_contract_harness/deploy_release_tree_contract_harness.py",
    "tools/mech_glb_pack_contract_harness/mech_glb_pack_contract_harness.py",
    "tools/shader_variant_matrix_harness/shader_variant_matrix_harness.py",
]
_VS_CMAKE = ("C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/"
             "Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe")


def cmake_exe():
    return _VS_CMAKE if os.path.isfile(_VS_CMAKE) else "cmake"


def cached_configure(src, bdir, extra=()):
    """(cmake configure command, env) for a Ninja tree behind the shared compile cache."""
    flags, env = cn.cache_cmake_args(str(REPO_ROOT))
    return [cmake_exe(), "-S", Path(src).as_posix(), "-B", Path(bdir).as_posix(), "-G", "Ninja",
            f"-DCMAKE_MAKE_PROGRAM={cn.NINJA}", "-DCMAKE_BUILD_TYPE=RelWithDebInfo", *flags, *extra], env


def build_and_run_unit_tests(config, run, asan=False, only=None, preset=None,
                             keep_going=False, log_dir=None, jobs=None, cache=False):
    started = time.monotonic()
    tree = "build64-tests-asan" if asan else "build64-tests"
    bdir = REPO_ROOT / (("build-cc-" + tree.removeprefix("build64-")) if cache else bd.tree_name(tree, preset))
    cm = cmake_exe()
    cache_env = None
    if cache:
        cfg, cache_env = cached_configure(REPO_ROOT / "tests/unit", bdir, ["-DMC2_ASAN=" + ("ON" if asan else "OFF")])
        commands = [cfg, bd.build_args(cm, bdir, config, ["mc2_tests"], jobs or "2", "ninja", keep_going)]
    else:
        commands = [
            [cm] + bd.configure_args(REPO_ROOT, REPO_ROOT / "tests/unit", bdir, preset) +
            ["-DMC2_ASAN=" + ("ON" if asan else "OFF")],
            bd.build_args(cm, bdir, config, ["mc2_tests"], jobs, preset, keep_going),
        ]
    for stage, command in zip(("configure", "build"), commands):
        if cache or preset or log_dir is not None or jobs or keep_going:
            rc, output = bd.run(command, REPO_ROOT, "mc2_tests", stage, log_dir,
                                vcvars=bool(preset) or cache, capture=True, env=cache_env)
        else:
            result = subprocess.run(command, capture_output=True, text=True, cwd=REPO_ROOT,
                                    **console_child.child_kwargs(redirected=True))
            rc, output = result.returncode, result.stdout + result.stderr
        if rc:
            lines = output.strip().splitlines()
            errors = [line for line in lines if " error " in line or "fatal error" in line]
            detail = "\n".join(errors + lines[-12:])
            return False, f"{stage} failed:\n{detail}"
        if stage == "configure" and preset:
            try:
                bd.verify_compiler(bdir, preset)
            except RuntimeError as exc:
                return False, str(exc)
    if not run:
        return True, f"built in {time.monotonic() - started:.0f}s"
    suffix = ".exe" if os.name == "nt" else ""
    exe = bdir / config / ("mc2_tests" + suffix)
    if not exe.is_file():
        exe = bdir / ("mc2_tests" + suffix)
    if preset and not exe.is_file():
        return False, f"requested compiler binary missing in {bdir}"
    command = [str(exe), "--no-breaks"]
    if only:
        command += ["--test-case=" + ",".join(name + "/*" for name in only)]
    env = dict(os.environ)
    if asan:
        # The allocation probe owns leak checks; ASan supplies bounds and lifetime evidence.
        env.setdefault("ASAN_OPTIONS", "detect_leaks=0:halt_on_error=0:print_stacktrace=1")
    result = subprocess.run(command, cwd=REPO_ROOT, env=env, **console_child.child_kwargs())
    tag = "asan " if asan else ""
    verdict = "passed" if result.returncode == 0 else f"failed (rc={result.returncode})"
    return result.returncode == 0, f"{tag}doctest {verdict} in {time.monotonic() - started:.0f}s"


def run_python_harnesses():
    passed = True
    for path in PY_HARNESSES:
        result = subprocess.run([sys.executable, str(REPO_ROOT / path)], cwd=REPO_ROOT,
                                **console_child.child_kwargs())
        ok = result.returncode == 0
        print(f"  [{'PASS' if ok else 'FAIL'}] {path} (rc={result.returncode})")
        passed = ok and passed
    return passed


def build_and_run_order_seam(run, cache=False, jobs=None, seam_tree=None, seam_prebuilt=False):
    """Separate linkage avoids the standalone TacticalOrder stubs."""
    from bootstrap_worktree_build import cache_args
    bdir = REPO_ROOT / (seam_tree or ("build-cc-order-seam" if cache else "build64-order-seam"))
    cache_env = None
    if cache:
        cfg, cache_env = cached_configure(REPO_ROOT, bdir, cache_args(str(REPO_ROOT)))
        build = [cmake_exe(), "--build", str(bdir), "--target", "mc2_authored_order_seam",
                 "--parallel", str(jobs or "2").lstrip("j")]
        # --seam-tree: an already configured Ninja tree of the root project (tools/build_all_targets.py passes its own). The seam
        # links that tree's mc2 objects, so building it there costs the test TUs and a link instead of a second mc2 compile.
        # --seam-prebuilt: that tree's own build already produced the seam (build_all_targets builds it with stage 1); only the
        # exe is looked up, and a missing one fails here.
        commands = [] if seam_prebuilt else [build] if seam_tree else [cfg, build]
    else:
        commands = [
            [cmake_exe(), "-S", str(REPO_ROOT), "-B", str(bdir), "-A", "x64", *cache_args(str(REPO_ROOT))],
            [cmake_exe(), "--build", str(bdir), "--config", "RelWithDebInfo", "--target", "mc2_authored_order_seam"],
        ]
    for command in commands:
        if cache:
            rc, output = bd.run(command, REPO_ROOT, "order_seam", "cached", None, vcvars=True, capture=True, env=cache_env)
            if rc:
                print(output)
                return False
            continue
        result = subprocess.run(command, cwd=REPO_ROOT, timeout=900, capture_output=True,
                                text=True, **console_child.child_kwargs(redirected=True))
        if result.returncode:
            print(result.stdout + result.stderr)
            return False
    exe = bdir / ("mc2_authored_order_seam.exe" if cache else "RelWithDebInfo/mc2_authored_order_seam.exe")
    if cache and not exe.is_file():
        print(f"  missing {exe}")
        return False
    if not run:
        return True
    command = [str(exe), "--no-breaks"]
    census = "AuthoredOrderProduction/read-only stock and mod mission Part census"
    env = dict(os.environ)
    env.pop("MC2_SEAM_DISCONNECT_ADAPTER", None)
    env.pop("MC2_SEAM_DISCONNECT_EXECUTION", None)
    env.pop("AUTHORED_SEAM_DISCONNECT_EXECUTION", None)
    positive = subprocess.run(command + ["--test-case-exclude=" + census], cwd=REPO_ROOT, env=env, timeout=60,
                              **console_child.child_kwargs())
    if positive.returncode:
        return False
    if Path("A:/Games/mc2-opengl").is_dir():
        census_result = subprocess.run(command + ["--test-case=" + census], cwd=REPO_ROOT,
                                       env=env, timeout=60, **console_child.child_kwargs())
        print(f"  [{'PASS' if census_result.returncode == 0 else 'FAIL'}] installed-content census")
        if census_result.returncode:
            return False
    else:
        print("  [SKIP] installed-content census (A:/Games/mc2-opengl absent)")
    env["MC2_SEAM_DISCONNECT_ADAPTER"] = "1"
    negative = subprocess.run(command + ["--test-case=AuthoredOrderProduction/parsed patrol and traverse reach real general setter"],
                              cwd=REPO_ROOT, env=env, capture_output=True,
                              text=True, timeout=60,
                              **console_child.child_kwargs(redirected=True))
    # Only an installation assertion failure proves adapter dependence; a crash cannot.
    controlled = negative.returncode == 1 and "CHECK( installed )" in negative.stdout
    print(f"  [{'PASS' if controlled else 'FAIL'}] adapter-disconnected negative control "
          f"(rc={negative.returncode}, positive installation assertion failed={controlled})")
    if not controlled:
        print(negative.stdout + negative.stderr)
        return False
    env.pop("MC2_SEAM_DISCONNECT_ADAPTER", None)
    env["AUTHORED_SEAM_DISCONNECT_EXECUTION"] = "1"
    negative = subprocess.run(command + ["--test-case=AuthoredOrderProduction/standing route moves and survives general writes"],
                              cwd=REPO_ROOT, env=env, capture_output=True,
                              text=True, timeout=60,
                              **console_child.child_kwargs(redirected=True))
    controlled = (negative.returncode == 1 and
                  "CHECK( cursor_sequence )" in negative.stdout and
                  "CHECK( general_slot_authored )" in negative.stdout and
                  "REQUIRE" not in negative.stdout)
    print(f"  [{'PASS' if controlled else 'FAIL'}] execution-disconnected negative control "
          f"(rc={negative.returncode}, movement and persistence assertions failed={controlled})")
    if not controlled:
        print(negative.stdout + negative.stderr)
    return controlled


def main(argv=None):
    ap = argparse.ArgumentParser(description="Build and optionally run doctest and Python contracts.")
    ap.add_argument("--run", action="store_true")
    ap.add_argument("--only", action="append", help="select a migrated native contract (repeatable)")
    ap.add_argument("--config", default="RelWithDebInfo", choices=("RelWithDebInfo",))
    ap.add_argument("--unit", action="store_true", help="compatibility flag; native contracts now use doctest")
    ap.add_argument("--unit-only", action="store_true", help="omit Python contract suites")
    ap.add_argument("--asan", action="store_true", help="MSVC AddressSanitizer; implies --unit-only")
    ap.add_argument("--preset", choices=bd.PRESETS, help="compiler preset; omitted preserves the MSVC trees")
    ap.add_argument("--keep-going", action="store_true", help="Ninja -k 0 / NMake /K in the selected preset tree")
    ap.add_argument("--compile-cache", action="store_true",
                    help="Ninja trees (build-cc-*) through the shared sccache, PCH off; not with --preset")
    ap.add_argument("--seam-tree", help="with --compile-cache: build the order seam in this configured root-project Ninja tree "
                                        "(relative to the repo) instead of its own build-cc-order-seam")
    ap.add_argument("--seam-prebuilt", action="store_true",
                    help="with --seam-tree: do not build the seam, require its exe there (built by the caller's own build)")
    ap.add_argument("--log-dir", help="complete mc2_tests configure/build logs")
    ap.add_argument("-j", dest="jobs", help="parallel jobs")
    args = ap.parse_args(argv)
    if args.seam_prebuilt and not args.seam_tree:
        ap.error("--seam-prebuilt needs --seam-tree")
    if args.seam_tree and not args.compile_cache:
        ap.error("--seam-tree needs --compile-cache (the Ninja single-config layout)")
    if args.compile_cache and args.preset:
        ap.error("--compile-cache is the MSVC cl.exe path; do not combine it with --preset")
    if args.asan and bd.is_clang(args.preset):
        ap.error("--asan runtime deployment is currently MSVC-only")
    os.environ.setdefault("_MSPDBSRV_ENDPOINT_", f"sol-s4b2-contracts-{os.getpid()}")
    if args.only:
        unknown = set(args.only) - set(REGISTERED_HARNESSES)
        if unknown:
            ap.error("unregistered harness: " + ", ".join(sorted(unknown)))
    ok, info = build_and_run_unit_tests(args.config, args.run, args.asan, args.only,
                                       preset=args.preset, keep_going=args.keep_going,
                                       log_dir=args.log_dir, jobs=args.jobs, cache=args.compile_cache)
    print(f"  [{'PASS' if ok else 'FAIL'}] mc2_tests: {info}")
    if not ok:
        return 1
    if not (args.asan or args.only):
        try:
            ok = build_and_run_order_seam(args.run, args.compile_cache, args.jobs, args.seam_tree, args.seam_prebuilt)
        except subprocess.TimeoutExpired as error:
            print(f"  [TIMEOUT] production seam ({error.timeout}s), not a compiler failure")
            return 124
        print(f"  [{'PASS' if ok else 'FAIL'}] mc2_authored_order_seam")
        if not ok:
            return 1
    if args.run and not (args.unit_only or args.asan or args.only):
        ok = run_python_harnesses()
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
