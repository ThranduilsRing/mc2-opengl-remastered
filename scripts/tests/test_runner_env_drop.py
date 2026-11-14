"""The smoke runner does not forward ambient MC2_* names that are not gates (harness knobs, typos)."""
import os
import sys
import json

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))

from smoke_lib import runner  # noqa: E402


def test_a_name_that_is_not_a_gate_is_dropped():
    env = {"MC2_CURSOR_NO_PARK": "1", "MC2_ZZZ": "1", "MC2_LOG": "1", "PATH": "x", "HOME_MC2_X": "1"}
    assert runner.unregistered_ambient(env) == {"MC2_CURSOR_NO_PARK": "1", "MC2_ZZZ": "1"}


def test_a_name_this_run_sets_itself_is_never_dropped_so_a_typo_there_still_fails_at_startup():
    env = {"MC2_TYPO_IN_MATRIX": "1"}
    assert runner.unregistered_ambient(env, {"MC2_TYPO_IN_MATRIX": "1"}) == {}
    assert runner.unregistered_ambient({"MC2_CONFIG_STRICT": "0"}) == {}      # FORCED_ENV name, also a gate


def test_registered_gates_pass_through():
    assert runner.unregistered_ambient({"MC2_LIGHT_GRID_DIM": "32", "MC2_SMOKE_MODE": "1"}) == {}


def test_the_table_is_read_from_the_committed_json():
    names = runner.known_gate_names()
    assert names is not None and "MC2_LOG" in names and "MC2_CURSOR_NO_PARK" not in names


def test_box_layer_overrides_ambient_search_paths(monkeypatch, tmp_path):
    monkeypatch.setattr(sys, "platform", "linux")
    lib = tmp_path / "validation.so"
    lib.touch()
    monkeypatch.setattr(runner.Path, "read_text", lambda self: json.dumps({"layer": {
        "api_version": "1.4.350", "implementation_version": "1", "library_path": str(lib)}}))
    env = {"MC2_TOOLCHAINS": "/workspace/toolchains", "VK_LAYER_PATH": "/apt",
           "VK_ADD_LAYER_PATH": "/old", "MC2_VULKAN_VALIDATION": "sync"}
    log = runner.box_validation_layer(env)
    assert env["VK_LAYER_PATH"] == "/workspace/toolchains/vvl-1.4.350.0/explicit_layer.d"
    assert "VK_ADD_LAYER_PATH" not in env and "api=1.4.350" in log and "preset=sync" in log
    lib.unlink()
    with pytest.raises(RuntimeError, match="missing or mismatched"):
        runner.box_validation_layer(env)


def test_non_box_layer_selection_is_preserved():
    env = {"VK_LAYER_PATH": "/custom"}
    assert runner.box_validation_layer(env) == ""
    assert env == {"VK_LAYER_PATH": "/custom"}
