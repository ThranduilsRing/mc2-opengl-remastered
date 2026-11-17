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


@pytest.mark.parametrize("disabled", [False, True])
def test_presentation_off_only_exempts_absent_cull_report(disabled):
    from smoke_lib.gates import GateConfig, evaluate
    from smoke_lib.logparse import LogSummary
    summary = LogSummary()
    summary.mission_ready_ms = 1
    summary.marker_counts["cull_parity_false_cull"] = 1
    verdict = evaluate(summary, GateConfig(stem="mc2_01", presentation_disabled=disabled),
                       exit_code=0, walltime_s=1, killed_by_timeout=False)
    assert ("cull_parity_missing" in verdict.buckets) == (not disabled)
    assert "cull_parity_false_cull" in verdict.buckets


@pytest.mark.parametrize("platform, filename", [("win32", "mc2.exe"), ("linux", "mc2")])
def test_command_replay_uses_native_executable(monkeypatch, tmp_path, platform, filename):
    import importlib.util
    from types import SimpleNamespace
    from pathlib import Path
    spec = importlib.util.spec_from_file_location("command_replay", Path(ROOT) / "tools/command_replay.py")
    replay = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(replay)
    monkeypatch.setattr(sys, "platform", platform)
    monkeypatch.setattr(replay.console_child, "child_kwargs", lambda **kw: {})
    calls = []
    monkeypatch.setattr(replay.subprocess, "run", lambda command, **kw:
                        calls.append(command) or SimpleNamespace(stdout="", stderr="", returncode=1))
    args = SimpleNamespace(out=tmp_path, install=tmp_path, mission="mc2_01", seed=1, content="x", duration=30)
    with pytest.raises(RuntimeError):
        replay.run(args, "test", {}, "[REPLAY_COMPLETE v1]")
    assert calls[0][calls[0].index("--exe") + 1] == str(tmp_path / filename)
