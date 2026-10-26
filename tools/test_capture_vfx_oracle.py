from pathlib import Path
import sys
import json

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from capture_vfx_oracle import command
import capture_vfx_oracle as runner


@pytest.mark.parametrize("arm", ["mlr", "gpu"])
def test_capture_command_pins_lane_freeze_seed_and_two_repeats(arm):
    cmd = command(Path("poses.json"), Path("captures"), 164, arm, install="lane3")
    assert cmd[cmd.index("--exe") + 1].replace(chr(92), "/") == "A:/Games/mc2-opengl/mc2-win64-lane3/mc2.exe"
    assert cmd[cmd.index("--duration") + 1] == "90"
    assert cmd[cmd.index("--runs") + 1] == "2"
    assert cmd[cmd.index("--trigger-frame") + 1] == "164"
    assert "MC2_SMOKE_FREEZE_ANIM=1" in cmd
    assert "MC2_DETERMINISTIC_RNG=1" in cmd and "MC2_RNG_SEED=0xC0FFEE" in cmd
    assert "MC2_RENDER_BACKEND=vulkan" in cmd and "MC2_VK_SHARED_GBUFFER=1" in cmd
    assert "--backend" not in command(Path("poses.json"), Path("captures"), 164, arm, install="lane2")
    lane2 = command(Path("poses.json"), Path("captures"), 164, arm, install="lane2")
    assert lane2[lane2.index("--exe") + 1].replace(chr(92), "/").endswith("mc2-win64-lane2/mc2.exe")
    env = dict(cmd[i + 1].split("=", 1) for i, arg in enumerate(cmd) if arg == "--env-declare")
    effective, _ = runner.run_visual_capture.build_capture_env(Path("poses.json"), Path("captures"), 164, 8, True, env)
    assert effective["MC2_SMOKE_FIXED_TIMESTEP"] == "1"
    assert effective["MC2_SMOKE_SEED"] == "0xC0FFEE"
    assert ("MC2_GPU_PARTICLES=1" in cmd) == (arm == "gpu")
    assert ("MC2_GPU_PARTICLES=0" in cmd) == (arm == "mlr")
    assert all(f"{name}=0" in cmd for name in
               ("MC2_VFX_ORACLE_RENDER", "MC2_VFX_ORACLE_TUBE", "MC2_VFX_ORACLE_SHAPE"))
    assert "--no-lease" not in cmd and "--show-window" not in cmd
    assert "--no-kill" in cmd


def test_explicit_lane_install_keeps_absolute_path():
    install = str(Path("E:/mc2-installs/claude-mlr-retire-p1").resolve())
    cmd = command(Path("a"), Path("b"), 152, "gpu", install=install)
    assert Path(cmd[cmd.index("--exe") + 1]) == Path(install) / "mc2.exe"


def test_capture_rejects_invalid_arm_and_frame():
    with pytest.raises(ValueError):
        command(Path("a"), Path("b"), 0, "vulkan")
    with pytest.raises(ValueError):
        command(Path("a"), Path("b"), -1, "gpu")
    with pytest.raises(ValueError):
        command(Path("a"), Path("b"), 152, "gpu", "unsupported")
    with pytest.raises(ValueError):
        command(Path("a"), Path("b"), 152, "gpu", experiment="unknown")


@pytest.mark.parametrize("arm", ["mlr", "gpu"])
def test_harvested_experiment_pins_the_complete_candidate_and_no_gpu_sim(arm):
    gates = runner.gate_vector(arm, "harvested")
    enabled = "1" if arm == "gpu" else "0"
    assert set(gates.values()) == {enabled}
    cmd = command(Path("a"), Path("b"), 152, arm, experiment="harvested")
    assert all(f"{key}={value}" in cmd for key, value in gates.items())


def test_one_shot_fixture_is_an_explicit_capture_input():
    assert "MC2_FX_FORCE_SPAWN=1" in command(Path("a"), Path("b"), 152, "mlr", "1")


def test_camera_is_parked_before_effect_births():
    pose = {"pos": [2546.0, -1941.0, 0], "rot": 0.0, "proj": 85.0, "alt": 1600.0}
    value = runner.park_value(pose)
    assert value == "2546.0,-1941.0,0.0,85.0,1600.0@0"
    assert "MC2_CAMERA_PARK=" + value in command(Path("a"), Path("b"), 152, "gpu", "1", value)


def test_lease_precedes_harness_deadline_and_argv_is_restored(monkeypatch):
    events = []
    argv = sys.argv
    monkeypatch.setattr(runner.host_lease, "ensure_held", lambda label: events.append("lease"))
    def harness():
        events.append("harness")
        assert sys.argv == ["harness.py", "--runs", "2"]
        return 1
    monkeypatch.setattr(runner.run_visual_capture, "main", harness)
    assert runner.capture(["python", "harness.py", "--runs", "2"]) == 1
    assert events == ["lease", "harness"]
    assert sys.argv is argv


@pytest.mark.parametrize("text,bad", [
    ("", True),
    ("[BUILD_FINGERPRINT v1] sha=deadbeef0000 dirty=0 branch=test built=now src=A:/Games/mc2-s3", True),
    ("[BUILD_FINGERPRINT v1] sha=5636f22bc357 dirty=1 branch=test built=now src=A:/Games/mc2-s3", True),
    ("[BUILD_FINGERPRINT v1] sha=5636f22bc357 dirty=0 branch=test built=now src=A:/Games/mc2-s3", False),
])
def test_capture_fingerprint_rejects_missing_wrong_or_dirty_build(text, bad):
    lines, failed = runner.validate_fingerprint(text, "5636f22bc357414fa75ca15c8fa52209995cae21")
    assert failed is bad
    assert lines


def test_plan_collects_all_scenes_and_propagates_failure(monkeypatch):
    root_main = runner.main
    calls = []
    scenes = [{"id": "ppc", "bookmarks": "a.json", "frame": 154, "fixture": "1"},
              {"id": "impact", "bookmarks": "b.json", "frame": 184, "fixture": "1"}]
    monkeypatch.setattr(Path, "read_text", lambda *a, **k: json.dumps({"scenes": scenes}))
    def child(argv):
        calls.append(argv)
        return 1 if len(calls) == 1 else 0
    monkeypatch.setattr(runner, "main", child)
    assert root_main(["--plan", "plan.json", "--out", "captures", "--install", "lane3"]) == 1
    assert len(calls) == 2
    assert calls[0][calls[0].index("--frame") + 1] == "154"
    assert calls[1][calls[1].index("--frame") + 1] == "184"
    assert all("--backend" not in argv and argv[argv.index("--install") + 1] == "lane3" for argv in calls)


def test_existing_capture_output_is_rejected_before_launch(monkeypatch):
    monkeypatch.setattr(Path, "exists", lambda path: True)
    monkeypatch.setattr(Path, "iterdir", lambda path: iter([Path("stale.png")]))
    monkeypatch.setattr(runner, "capture", lambda cmd: pytest.fail("stale capture launched"))
    with pytest.raises(SystemExit):
        runner.main(["poses.json", "--frame", "154", "--out", "captures"])


def test_unknown_install_is_one_log_line_and_no_launch(monkeypatch, capsys):
    monkeypatch.setattr(runner, "capture", lambda cmd: pytest.fail("capture launched for an unknown install"))
    assert runner.main(["poses.json", "--frame", "154", "--out", "captures", "--install", "lane9"]) == 2
    err = capsys.readouterr().err
    assert err.count("\n") == 1 and "lane9" in err and "usage:" not in err


def test_install_lane2_reaches_the_exe_path_through_main(monkeypatch, tmp_path):
    poses = tmp_path / "poses.json"
    pose = {"pos": [1.0, 2.0, 0], "rot": 0.0, "proj": 85.0, "alt": 1600.0}
    poses.write_text(json.dumps({"mission": "mc2_24", "bookmarks": [pose, pose]}), encoding="utf-8")
    launched = []
    monkeypatch.setattr(runner, "capture", lambda cmd: launched.append(cmd) or 0)
    monkeypatch.setattr(runner.subprocess, "check_output", lambda *a, **k: "0" * 40)
    runner.main([str(poses), "--frame", "154", "--out", str(tmp_path / "out"), "--arm", "gpu", "--install", "lane2"])
    assert len(launched) == 1
    assert launched[0][launched[0].index("--exe") + 1].replace(chr(92), "/").endswith("mc2-win64-lane2/mc2.exe")


def test_duplicate_plan_ids_fail_before_any_capture(monkeypatch):
    root_main = runner.main
    monkeypatch.setattr(Path, "read_text", lambda *a, **k: json.dumps({"scenes": [{"id": "ppc"}] * 2}))
    calls = []
    monkeypatch.setattr(runner, "main", lambda argv: calls.append(argv))
    with pytest.raises(SystemExit):
        root_main(["--plan", "plan.json", "--out", "captures"])
    assert calls == []
