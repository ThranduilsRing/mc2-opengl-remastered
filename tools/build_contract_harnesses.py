#!/usr/bin/env python3
"""Build/run doctest, the engine-linked order seam and Python contracts.

--only/--asan restrict runs to standalone doctests; --unit-only skips Python.
--only takes a migrated contract name (runs name/*) or any doctest case name/wildcard from tests/unit.
The engine-linked authored-order seam (a whole second engine build) runs only with --seam.
--unit is compatibility syntax. RelWithDebInfo retains release fatal-path checks.
--compile-cache builds both trees (build-cc-tests, build-cc-order-seam) with Ninja through the shared sccache instead of the
VS-generator trees; tools/build_all_targets.py passes it by default.
"""
import argparse
import fnmatch
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "scripts"))
import console_child
import configure_ninja as cn
sys.path.insert(0, str(REPO_ROOT / "cmake"))
import build_driver as bd

# This migration index preserves named selection and coverage of the active contracts.
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
]
PY_HARNESSES = [
    "tools/deploy_asset_contract_harness/deploy_asset_contract_harness.py",
    "tools/deploy_release_tree_contract_harness/deploy_release_tree_contract_harness.py",
    "tools/mech_glb_pack_contract_harness/mech_glb_pack_contract_harness.py",
    "tools/shader_variant_matrix_harness/shader_variant_matrix_harness.py",
]
_VS_CMAKE = ("C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/"
             "Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe")


def doctest_case_names():
    """Test case names declared in tests/unit/test_*.cpp (a source scan; no build needed)."""
    pattern = re.compile(r'\bTEST_CASE(?:_FIXTURE)?\s*\(\s*(?:\w+\s*,\s*)?"([^"]+)"')
    names = set()
    for path in (REPO_ROOT / "tests/unit").glob("test_*.cpp"):
        names.update(pattern.findall(path.read_text(encoding="utf-8", errors="replace")))
    return names


def case_owner(only):
    """tests/unit source file(s) declaring a case matching any --only item (names the target that should own it)."""
    pattern = re.compile(r'\bTEST_CASE(?:_FIXTURE)?\s*\(\s*(?:\w+\s*,\s*)?"([^"]+)"')
    hits = set()
    for path in (REPO_ROOT / "tests/unit").glob("test_*.cpp"):
        names = pattern.findall(path.read_text(encoding="utf-8", errors="replace"))
        if any(fnmatch.filter(names, item) for item in only if item not in REGISTERED_HARNESSES):
            hits.add(path.relative_to(REPO_ROOT).as_posix())
    return sorted(hits)


def matched_case_count(exe, filt, env):
    """Cases the built mc2_tests binary would run for filt (doctest --count), or None when the output is unreadable."""
    try:
        proc = subprocess.Popen([str(exe), "--count", "--test-case=" + filt], cwd=REPO_ROOT, env=env,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace",
                                **console_child.child_kwargs(redirected=True))
        out = proc.communicate(timeout=120)[0]
    except (OSError, subprocess.SubprocessError):
        return None
    m = re.search(r"passing the current filters:\s*(\d+)", out)
    return int(m.group(1)) if m else None


def test_case_filter(only):
    """--test-case value for --only: a registered contract name selects name/*, anything else is a doctest case name or wildcard."""
    return ",".join(name + "/*" if name in REGISTERED_HARNESSES else name for name in only)


def unknown_selection(only):
    """Selections that match no registered contract and no doctest case in the sources."""
    names = None
    unknown = []
    for item in only:
        if item in REGISTERED_HARNESSES:
            continue
        names = doctest_case_names() if names is None else names
        if not fnmatch.filter(names, item):
            unknown.append(item)
    return unknown


def cmake_exe():
    return _VS_CMAKE if os.path.isfile(_VS_CMAKE) else "cmake"


def cached_configure(src, bdir, extra=()):
    """(cmake configure command, env) for a Ninja tree behind the shared compile cache."""
    flags, env = cn.cache_cmake_args(str(REPO_ROOT))
    return [cmake_exe(), "-S", Path(src).as_posix(), "-B", Path(bdir).as_posix(), "-G", "Ninja",
            f"-DCMAKE_MAKE_PROGRAM={cn.NINJA}", "-DCMAKE_BUILD_TYPE=RelWithDebInfo", *flags, *extra], env


def build_and_run_unit_tests(config, run, asan=False, only=None, preset=None,
                             keep_going=False, log_dir=None, jobs=None, cache=False, build_root=None):
    started = time.monotonic()
    tree = "build64-tests-asan" if asan else "build64-tests"
    bdir = Path(build_root or REPO_ROOT) / (("build-cc-" + tree.removeprefix("build64-")) if cache else bd.tree_name(tree, preset))
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
    env = dict(os.environ)
    if only:
        command += ["--test-case=" + test_case_filter(only)]
        if matched_case_count(exe, test_case_filter(only), env) == 0:
            owners = case_owner(only)
            return False, (f"--only {', '.join(only)} matched 0 doctest cases in {exe.name}"
                           + (f"; declared in {', '.join(owners)} (is that file in the mc2_tests target, and is the binary rebuilt?)"
                              if owners else "; no tests/unit/test_*.cpp declares it"))
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
            [cmake_exe(), "--build", str(bdir), "--config", "RelWithDebInfo", "--target", "mc2_authored_order_seam",
             "--parallel", str(jobs or "2").lstrip("j")],
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
    # Engine-linked fixtures use relative paths; keep them away from worktree scratch.
    with tempfile.TemporaryDirectory(prefix="mc2-order-seam-") as fixture_dir:
        shutil.copytree(REPO_ROOT / "tests/fixtures/brain_runtime",
                        Path(fixture_dir) / "tests/fixtures/brain_runtime")
        command = [str(exe), "--no-breaks"]
        census = "AuthoredOrderProduction/read-only stock and mod mission Part census"
        env = dict(os.environ)
        env.pop("MC2_SEAM_DISCONNECT_ADAPTER", None)
        env.pop("MC2_SEAM_DISCONNECT_EXECUTION", None)
        env.pop("AUTHORED_SEAM_DISCONNECT_EXECUTION", None)
        positive = subprocess.run(command + ["--test-case-exclude=" + census], cwd=fixture_dir, env=env, timeout=60,
                                  **console_child.child_kwargs())
        if positive.returncode:
            return False
        if Path("A:/Games/mc2-opengl").is_dir():
            census_result = subprocess.run(command + ["--test-case=" + census], cwd=fixture_dir,
                                           env=env, timeout=60, **console_child.child_kwargs())
            print(f"  [{'PASS' if census_result.returncode == 0 else 'FAIL'}] installed-content census")
            if census_result.returncode:
                return False
        else:
            print("  [SKIP] installed-content census (A:/Games/mc2-opengl absent)")
        env["MC2_SEAM_DISCONNECT_ADAPTER"] = "1"
        negative = subprocess.run(command + ["--test-case=AuthoredOrderProduction/parsed patrol and traverse reach real general setter"],
                                  cwd=fixture_dir, env=env, capture_output=True,
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
                                  cwd=fixture_dir, env=env, capture_output=True,
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
    ap.add_argument("--only", action="append",
                    help="a migrated contract name (runs name/*) or a doctest case name/wildcard (repeatable)")
    ap.add_argument("--seam", action="store_true",
                    help="also build and run the engine-linked authored-order seam (a second engine build; off by default)")
    ap.add_argument("--config", default="RelWithDebInfo", choices=("RelWithDebInfo",))
    ap.add_argument("--unit", action="store_true", help="compatibility flag; native contracts now use doctest")
    ap.add_argument("--unit-only", action="store_true", help="omit Python contract suites")
    ap.add_argument("--python-only", action="store_true", help="run only Python contracts; merge gate reuses its verified native unit receipt")
    ap.add_argument("--asan", action="store_true", help="MSVC AddressSanitizer; implies --unit-only")
    ap.add_argument("--preset", choices=bd.PRESETS, help="compiler preset; omitted preserves the MSVC trees")
    ap.add_argument("--keep-going", action="store_true", help="Ninja -k 0 / NMake /K in the selected preset tree")
    ap.add_argument("--compile-cache", action="store_true",
                    help="Ninja trees (build-cc-*) through the shared sccache, PCH off; not with --preset")
    ap.add_argument("--seam-tree", help="with --compile-cache: build the order seam in this configured root-project Ninja tree "
                                        "(relative to the repo) instead of its own build-cc-order-seam")
    ap.add_argument("--seam-prebuilt", action="store_true",
                    help="with --seam-tree: do not build the seam, require its exe there (built by the caller's own build)")
    ap.add_argument("--build-root", help="parent directory for the doctest build tree")
    ap.add_argument("--log-dir", help="complete mc2_tests configure/build logs")
    ap.add_argument("-j", dest="jobs", help="parallel jobs")
    args = ap.parse_args(argv)
    if args.python_only and (not args.run or args.only or args.unit_only or args.unit or args.asan or args.seam):
        ap.error("--python-only needs --run and cannot select native unit/seam checks")
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
        unknown = unknown_selection(args.only)
        if unknown:
            ap.error("no registered contract or doctest case matches: " + ", ".join(sorted(unknown)))
    if args.python_only:
        ok = True
        print("[contracts] Python-only selection; native units are not rerun")
    else:
        ok, info = build_and_run_unit_tests(args.config, args.run, args.asan, args.only,
                                           preset=args.preset, keep_going=args.keep_going,
                                           log_dir=args.log_dir, jobs=args.jobs, cache=args.compile_cache, build_root=args.build_root)
        print(f"  [{'PASS' if ok else 'FAIL'}] mc2_tests: {info}")
        if not ok:
            return 1
    if args.seam and not (args.asan or args.only):
        if args.build_root and not args.seam_tree:
            args.seam_tree = str(Path(args.build_root) / ("build-cc-order-seam" if args.compile_cache else "build64-order-seam"))
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
