"""Rule defaults: the lane rules (A:/Games/mc2-ops/lane-rules.md) are enforced by tool defaults; these fail when one regresses."""
import importlib
import os
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
sys.path.insert(0, str(ROOT / "tools"))

RULES = Path("A:/Games/mc2-ops/lane-rules.md")


class _Parsed(Exception):
    pass


def parsed_args(module, argv):
    """Run module.main() just far enough to capture the argparse Namespace of its parser."""
    import argparse
    orig = argparse.ArgumentParser.parse_args
    box = {}

    def grab(self, args=None, namespace=None):
        box["ns"] = orig(self, args, namespace)
        raise _Parsed

    argparse.ArgumentParser.parse_args = grab
    saved = sys.argv
    try:
        sys.argv = ["x"] + argv
        try:
            module.main(argv) if "argv" in module.main.__code__.co_varnames[:module.main.__code__.co_argcount] else module.main()
        except _Parsed:
            pass
    finally:
        argparse.ArgumentParser.parse_args = orig
        sys.argv = saved
    return box["ns"]


def test_run_smoke_default_duration_about_15s():
    import run_smoke
    assert run_smoke.DEFAULT_DURATION_S == 15


def test_run_visual_capture_default_duration_about_15s():
    import run_visual_capture as rvc
    assert rvc.default_duration(1, 120, 30) == 15


def test_sim_determinism_check_defaults_to_20():
    import sim_determinism
    assert parsed_args(sim_determinism, ["check", "mc2_01"]).duration == "20"


def test_battle_bench_defaults_and_exclusive_only_with_acceptance():
    import run_battle_bench as bench
    assert bench.resolve_lengths(None, None, False) == (1, 20)
    assert bench.resolve_lengths(None, None, True) == (2, 30)
    src = (ROOT / "scripts" / "run_battle_bench.py").read_text(encoding="utf-8")
    assert "exclusive_window(\"integrated battle benchmark\") if args.acceptance else host_lease.shared_window()" in src


def test_tracy_capture_defaults_to_no_csv():
    import tracy_capture_compare as t
    ns = parsed_args(t, ["capture", "--install", "x", "-o", "y.tracy"])
    assert ns.csv is False


def test_configure_ninja_compile_cache_on_by_default():
    import configure_ninja
    assert parsed_args(configure_ninja, []).no_compile_cache is False  # cache is ON unless --no-compile-cache
    import inspect
    # configure() is also called directly (ninja_build.warm, lane_merge verify): its own default must be ON too.
    assert inspect.signature(configure_ninja.configure).parameters["compile_cache"].default is True


def test_deploy_payload_refuses_implicit_target_from_lane_worktree():
    import deploy_payload as dp
    assert dp.implicit_target_refusal("A:/Games/mc2-flash5", None)
    assert dp.implicit_target_refusal("A:/Games/mc2-flash5", "E:/mc2-installs/x") is None
    assert dp.implicit_target_refusal("A:/Games/mc2-main", None) is None


@pytest.mark.skipif(not RULES.exists(), reason="lane-rules.md is machine-local")
def test_lane_rules_file_lint():
    text = RULES.read_text(encoding="utf-8")
    for heading in ("## 1.", "## 2.", "## 3.", "## 4.", "## 5.", "## 6.", "## 7."):
        assert heading in text, heading
    # lines that forbid a target (NEVER deploy to ... lane3 ...) may name it
    low = "\n".join(l for l in text.splitlines() if "NEVER" not in l).lower()
    for banned in ("lane3", "scratchpad", "merge window 3"):
        assert banned not in low, banned
