#!/usr/bin/env bash
# scripts/run-unit-tests.sh
# One-command build+run of the OFFLINE C++ test tier. Pure-logic only -- no GL
# context, no engine boot, no deploy, no smoke. This is the "prove it offline"
# loop for pure kernels (VIEW-CURRENCY-HARNESS-1 currency math,
# FRAME-GRAPH-SKELETON-1 resource-DAG validation, projection/xform/HZB
# conventions, etc). Seconds to run once warm; use it instead of a full
# build->deploy->smoke when the change is pure CPU logic.
#
# Usage:
#   bash scripts/run-unit-tests.sh                  # build + run every tier
#   bash scripts/run-unit-tests.sh --ts=FrameGraph  # doctest suite filter
#   bash scripts/run-unit-tests.sh --ts=ViewCurrency,FrameGraph
#   bash scripts/run-unit-tests.sh --doctest-only   # skip the non-doctest binaries
#   MC2_CMAKE=/path/to/cmake.exe bash scripts/run-unit-tests.sh   # override cmake
#
# ---------------------------------------------------------------------------
# DARK-TESTS-RUN-1 (2026-07-31): this script used to build and run ONE target.
# ---------------------------------------------------------------------------
# It configured build64-tests and ran mc2_tests, and that was the whole tier as
# far as any runner was concerned. Three other test binaries existed and were
# reachable only by someone reading CMake:
#
#   test_render_contract_3           -- did not even LINK (LNK2019 x2). Broken
#                                       for as long as nobody built it.
#   model_override_tests             -- CTest-registered; `ctest` is invoked by
#   static_natural_skip_policy_tests    no script in this repo, so the
#                                       registration drove nothing.
#
# All four are named here now, so scripts/check-test-tier-coverage.py can prove
# no test target is reachable by CMake-reading alone.
#
# Standalone cached build dir (build-cc-tests/) -- never touches the engine build64/
# cache. The two CTest targets are the exception: they are defined by the MAIN
# CMakeLists. Prefer the verified Ninja tree, then the legacy build64 cache.
# An explicitly requested engine tree must exist; a partial merge proof fails.
#
# Exit 0 iff configure, build, and every test in every tier pass.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CMAKE="${MC2_CMAKE:-C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe}"
CFG="${MC2_TEST_CONFIG:-RelWithDebInfo}"
JOBS="${MC2_BUILD_JOBS:-$(cat A:/Games/mc2-ops/JOBS 2>/dev/null || echo 2)}"
BUILD_DIR="${MC2_UNIT_BUILD_DIR:-$ROOT/build-cc-tests}"   # lane_merge verify sets its own, so it never collides with a lane's doctest run

DOCTEST_ONLY=0
DOCTEST_ARGS=()
for arg in "$@"; do
    case "$arg" in
        --doctest-only) DOCTEST_ONLY=1 ;;
        *) DOCTEST_ARGS+=("$arg") ;;
    esac
done

FAILED=0
note() { echo "[run-unit-tests] $*"; }

# ---------------------------------------------------------------------------
# Tier 1 -- standalone, GL-free (build64-tests/)
# ---------------------------------------------------------------------------
UNIT_CFG="$CFG/"
UNIT_CACHED=0
UNIT_PATH="${BUILD_DIR//\\//}"
if [[ "$UNIT_PATH" == */build-cc-tests ]]; then
    UNIT_CACHED=1
    BUILD_DIR="$ROOT/build-cc-tests"
    UNIT_CFG=""
    note "build cached standalone unit tier ($BUILD_DIR)"
    py -3 "$ROOT/tools/build_contract_harnesses.py" --compile-cache --unit-only -j "$JOBS"
else
    note "configure ($BUILD_DIR)"
    "$CMAKE" -S "$ROOT/tests/unit" -B "$BUILD_DIR" -G "Visual Studio 17 2022" -A x64 >/dev/null
    note "build mc2_tests ($CFG)"
    "$CMAKE" --build "$BUILD_DIR" --config "$CFG" --target mc2_tests --parallel "$JOBS"
fi

note "run mc2_tests"
if [ "${#DOCTEST_ARGS[@]}" -gt 0 ]; then
    "$BUILD_DIR/${UNIT_CFG}mc2_tests.exe" "${DOCTEST_ARGS[@]}" || FAILED=1
else
    "$BUILD_DIR/${UNIT_CFG}mc2_tests.exe" || FAILED=1
fi

if [ "$DOCTEST_ONLY" -eq 1 ]; then
    note "--doctest-only: skipping test_render_contract_3 and the CTest tier"
    exit "$FAILED"
fi

# test_render_contract_3 has its own int main() and no doctest suite filter, so
# a --ts= run skips it rather than passing it an argument it cannot parse.
if [ "${#DOCTEST_ARGS[@]}" -gt 0 ]; then
    note "suite filter given: skipping test_render_contract_3 (no --ts support)"
else
    note "build test_render_contract_3 ($CFG)"
    if [ "$UNIT_CACHED" -eq 1 ]; then
        MC2_NINJA_TREE=build-cc-tests py -3 "$ROOT/scripts/ninja_build.py" -j"$JOBS" test_render_contract_3
    else
        "$CMAKE" --build "$BUILD_DIR" --config "$CFG" --target test_render_contract_3 --parallel "$JOBS"
    fi
    note "run test_render_contract_3"
    "$BUILD_DIR/${UNIT_CFG}test_render_contract_3.exe" || FAILED=1
fi

# ---------------------------------------------------------------------------
# Tier 2 -- CTest targets defined by the MAIN CMakeLists (build64/)
# ---------------------------------------------------------------------------
# These two are add_test()-registered but nothing in this repo runs `ctest`, so
# they are invoked directly here. They need the engine cache; when it is absent
# say so out loud -- a silently skipped tier is the failure this script exists
# to stop.
ENGINE_BUILD="$ROOT/${MC2_NINJA_TREE:-build64-ninja}"
if [ ! -f "$ENGINE_BUILD/CMakeCache.txt" ]; then
    if [ -n "${MC2_NINJA_TREE:-}" ]; then
        note "FAIL: requested engine cache is missing: $ENGINE_BUILD"
        exit 1
    fi
    ENGINE_BUILD="$ROOT/build64"
fi
ENGINE_CFG="$CFG/"
if [ -f "$ENGINE_BUILD/CMakeCache.txt" ] && grep -q '^CMAKE_GENERATOR:INTERNAL=Ninja[[:space:]]*$' "$ENGINE_BUILD/CMakeCache.txt"; then
    ENGINE_CFG=""
fi
if [ ! -f "$ENGINE_BUILD/CMakeCache.txt" ]; then
    note "SKIP (no engine cache at $ENGINE_BUILD): model_override_tests, static_natural_skip_policy_tests"
    note "     configure an engine tree to include them; they are NOT covered by this run."
else
    note "build engine test tier ($CFG; $ENGINE_BUILD)"
    if [ -z "$ENGINE_CFG" ]; then
        py -3 "$ROOT/scripts/ninja_build.py" -j"$JOBS" model_override_tests static_natural_skip_policy_tests
    else
        "$CMAKE" --build "$ENGINE_BUILD" --config "$CFG" --target model_override_tests static_natural_skip_policy_tests --parallel "$JOBS"
    fi
    note "run model_override_tests"
    "$ENGINE_BUILD/tests/model_override/${ENGINE_CFG}model_override_tests.exe" \
        "$ROOT/tests/model_override/fixtures" || FAILED=1
    note "run static_natural_skip_policy_tests"
    "$ENGINE_BUILD/tests/static_natural_skip/${ENGINE_CFG}static_natural_skip_policy_tests.exe" || FAILED=1
fi

if [ "$FAILED" -ne 0 ]; then
    note "FAIL -- at least one tier reported a failing test"
else
    note "PASS -- every tier green"
fi
exit "$FAILED"
