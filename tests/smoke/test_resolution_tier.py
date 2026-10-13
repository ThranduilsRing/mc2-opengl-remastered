"""The weekly `resolution` smoke tier and the pinned capture/trace size (scripts/smoke_lib/render_resolution.py)."""
import ast
import importlib.util
from pathlib import Path

import pytest

from scripts.smoke_lib import render_resolution
from scripts.smoke_lib.manifest import parse_manifest

ROOT = Path(__file__).resolve().parents[2]


def test_parse_accepts_wxh_and_rejects_the_rest():
    assert render_resolution.parse("1920x1080") == (1920, 1080)
    assert render_resolution.parse(" 3840 X 2160 ") == (3840, 2160)
    for bad in ("1920", "x1080", "0x1080", "1920x-1", "axb", ""):
        with pytest.raises(ValueError):
            render_resolution.parse(bad)


def test_size_env_is_the_engine_pair():
    assert render_resolution.size_env(1280, 720) == {"MC2_WIDTH": "1280", "MC2_HEIGHT": "720"}
    assert render_resolution.pinned_env() == {"MC2_WIDTH": "1920", "MC2_HEIGHT": "1080"}


def test_manifest_parses_resolution_entries(tmp_path):
    p = tmp_path / "m.txt"
    p.write_text('tier1 mc2_01\nresolution mc2_01 resolution=1920x1080\nresolution mc2_01 resolution=BAD\n')
    entries = parse_manifest(p)
    res = [e for e in entries if e.tier == "resolution"]
    assert [e.resolution for e in res] == ["1920x1080", ""]   # a malformed size is dropped with a warning, not kept
    assert next(e for e in entries if e.tier == "tier1").resolution == ""


def test_shipped_manifest_has_one_entry_per_tier_size():
    entries = [e for e in parse_manifest(ROOT / "tests" / "smoke" / "smoke_missions.txt") if e.tier == "resolution"]
    assert [render_resolution.parse(e.resolution) for e in entries] == list(render_resolution.RESOLUTION_TIER_SIZES)
    assert {e.stem for e in entries} == {"mc2_01"}


def test_pin_applies_unless_the_caller_declares_the_whole_size():
    env = {"KEEP": "1"}
    assert render_resolution.pin_unless_declared(env, None) is env
    assert env == {"KEEP": "1", "MC2_WIDTH": "1920", "MC2_HEIGHT": "1080"}
    env = {}
    render_resolution.pin_unless_declared(env, {"MC2_WIDTH": "2560", "MC2_HEIGHT": "1440"})
    assert env == {}                                    # declared: the caller's own values go in later, the pin stays out
    for half in ({"MC2_WIDTH": "2560"}, {"MC2_HEIGHT": "1440"}):
        with pytest.raises(render_resolution.HalfDeclaredSize):
            render_resolution.pin_unless_declared({}, half)


def _load(path, name):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_capture_driver_builds_the_pinned_env(tmp_path):
    rvc = _load("scripts/run_visual_capture.py", "run_visual_capture_under_test")
    args = (tmp_path / "bm.json", tmp_path, 431, 30, True)
    env, _ = rvc.build_capture_env(*args, None)
    assert (env["MC2_WIDTH"], env["MC2_HEIGHT"]) == ("1920", "1080")
    env, _ = rvc.build_capture_env(*args, {"MC2_WIDTH": "2560", "MC2_HEIGHT": "1440"})
    assert (env["MC2_WIDTH"], env["MC2_HEIGHT"]) == ("2560", "1440")
    with pytest.raises(SystemExit):
        rvc.build_capture_env(*args, {"MC2_WIDTH": "2560"})   # 2560 beside the pinned 1080 would be a size nobody asked for


def test_tracy_driver_builds_the_pinned_env():
    tcc = _load("tools/tracy_capture_compare.py", "tracy_capture_compare_pin_under_test")
    env = tcc.game_env("gl", False, 0, None, [])
    assert (env["MC2_WIDTH"], env["MC2_HEIGHT"]) == ("1920", "1080")
    env = tcc.game_env("gl", False, 0, None, ["MC2_WIDTH=2560", "MC2_HEIGHT=1440"])
    assert (env["MC2_WIDTH"], env["MC2_HEIGHT"]) == ("2560", "1440")
    with pytest.raises(SystemExit):
        tcc.game_env("gl", False, 0, None, ["MC2_HEIGHT=1440"])


def test_smoke_runner_names_every_per_row_artifact_by_the_row_label():
    """The two `resolution` rows share a mission stem; an artifact named by the stem would be overwritten by the second row."""
    tree = ast.parse((ROOT / "scripts" / "run_smoke.py").read_text(encoding="utf-8"))
    bad = []
    for node in ast.walk(tree):
        # f"{e.stem}.ring_trace.log" parses as [FormattedValue(e.stem), Constant(".ring_trace.log")]
        if (isinstance(node, ast.JoinedStr) and len(node.values) == 2 and isinstance(node.values[0], ast.FormattedValue)
                and ast.unparse(node.values[0].value) == "e.stem" and isinstance(node.values[1], ast.Constant)
                and node.values[1].value.startswith((".crash_evidence", ".ring_trace", ".log"))):
            bad.append((node.lineno, node.values[1].value))
        # _crash_evidence.capture(<name>, ...) writes <name>.crash_evidence.json
        if (isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute) and node.func.attr == "capture"
                and ast.unparse(node.func.value) == "_crash_evidence" and ast.unparse(node.args[0]) != "label"):
            bad.append((node.lineno, "crash evidence named by " + ast.unparse(node.args[0])))
    assert not bad, f"per-row artifacts named by the mission stem: {bad}"


def test_tier1_is_pinned_and_the_other_tiers_keep_the_install_size():
    assert render_resolution.PINNED_TIERS == ("tier1",)
    assert render_resolution.asked_size("tier1") == (1920, 1080)
    assert render_resolution.tier_size_env("tier1") == {"MC2_WIDTH": "1920", "MC2_HEIGHT": "1080"}
    for tier in ("tier2", "tier3", "adhoc"):
        assert render_resolution.asked_size(tier) is None and render_resolution.tier_size_env(tier) == {}
    # an entry's own size wins in every tier (the `resolution` tier)
    assert render_resolution.asked_size("resolution", "3840x2160") == (3840, 2160)
    assert render_resolution.tier_size_env("tier1", "3840x2160") == {"MC2_WIDTH": "3840", "MC2_HEIGHT": "2160"}


def test_the_smoke_report_says_which_size_a_pinned_tier_ran_at():
    from scripts.smoke_lib import report
    assert "1920x1080" in report._resolution_line("tier1") and "pinned for tier1" in report._resolution_line("tier1")
    assert report._resolution_line("tier2") == ""


def test_the_current_golden_exists_and_was_blessed_at_the_pinned_size():
    ccr = _load("scripts/check-capture-resolution.py", "check_capture_resolution_under_test")
    assert ccr.check_current_golden() == []
    assert ccr.current_golden_dir().name == render_resolution.CURRENT_GOLDEN
    assert ccr.main(["--current"]) == 0


def test_the_current_golden_records_the_options_the_backend_and_the_frozen_animation():
    import json
    ccr = _load("scripts/check-capture-resolution.py", "check_capture_resolution_under_test2")
    env = json.loads((ccr.current_golden_dir() / "golden.json").read_text(encoding="utf-8"))["capture_env"]
    assert len(env["render_options"]) == 17
    assert env["declared_env"].get("MC2_SMOKE_FREEZE_ANIM") == "1"
    assert env["backend"] == {"requested": "gl", "drew": ["gl"]}
    assert env["resolution"] == f"{render_resolution.PINNED_WIDTH}x{render_resolution.PINNED_HEIGHT}"
