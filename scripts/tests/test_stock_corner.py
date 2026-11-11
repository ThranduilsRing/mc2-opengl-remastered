"""Stock replay never substitutes a synthetic battle or averages away tails."""
import json
from pathlib import Path
import sys
from types import SimpleNamespace

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import run_battle_bench as bench


def evidence(tmp_path, values=(6.06, 6.061, 8.33, 8.331)):
    directory = tmp_path / "stock"
    channel = bench.prepare_stock(directory)
    channel.with_suffix(".txt.ack").write_text(
        f"bench_output {directory / 'frames.jsonl'} -> ok\n{bench.view_command(bench.CORNER)} -> ok\n")
    rows = [{"type": "clock", "frame": 1},
            {"type": "cpu", "frame": 2, "ms": 9999},
            {"type": "view", "frame": 2, "sim_s": .1, "pos": [99999, 99999],
             "yaw": -45, "alt": 5853.466, "pitch": 12.5}]
    for frame, value in enumerate(values, 3):
        rows += [{"type": "cpu", "frame": frame, "ms": value},
                 {"type": "gpu", "frame": frame, "ms": 1, "recorded": 1, "valid": 1}]
    (directory / "frames.jsonl").write_text("\n".join(json.dumps(r) for r in rows))
    return directory


def test_stock_commands_only_arm_output_and_corner(tmp_path):
    path = bench.prepare_stock(tmp_path / "fresh")
    lines = path.read_text().splitlines()
    assert len(lines) == 2
    assert lines[1] == "bench_view 99999 99999 -45 5853.4658 12.5"
    assert not any(x in path.read_text() for x in ("spawn", "attack", "set_light", "powerup"))
    with pytest.raises(FileExistsError):
        bench.prepare_stock(tmp_path / "fresh")


def test_strict_thresholds_and_post_pose_boundary(tmp_path):
    result = bench.summarize_stock(evidence(tmp_path), "event=mission_ready")
    assert result["cpu_work"]["n"] == 4
    assert result["cpu_work"]["over_6_06_ms"] == 3
    assert result["cpu_work"]["over_8_33_ms"] == 1
    assert result["cpu_work"]["p99_ms"] == 8.331
    assert result["sampling"]["first_frame"] == 3
    assert "not measured display" in result["metric"]


def test_p99_is_nearest_rank(tmp_path):
    result = bench.summarize_stock(evidence(tmp_path, list(range(1, 101))), "event=mission_ready")
    assert result["cpu_work"]["p99_ms"] == 99
    assert result["cpu_work"]["max_ms"] == 100


@pytest.mark.parametrize("values", [[], [-1], [float("nan")], [float("inf")], [True], ["bad"]])
def test_bad_cpu_samples_refused(tmp_path, values):
    with pytest.raises(ValueError, match="CPU samples"):
        bench.summarize_stock(evidence(tmp_path, values), "event=mission_ready")


def test_missing_ack_refused(tmp_path):
    directory = evidence(tmp_path)
    (directory / "commands.txt.ack").write_text("bench_output -> blocked")
    with pytest.raises(ValueError, match="acknowledgement"):
        bench.summarize_stock(directory, "event=mission_ready")


@pytest.mark.parametrize("present,dirty,sha", [(False, False, "a" * 12), (True, True, "a" * 12), (True, False, "b" * 12)])
def test_historical_provenance_rejection(monkeypatch, present, dirty, sha):
    monkeypatch.setattr(bench.ms, "exe_block", lambda _: {"sha256": "hash", "embedded_fingerprint":
        {"present": present, "dirty": dirty, "sha": sha}})
    with pytest.raises(ValueError, match="provenance"):
        bench.stock_identity(Path("mc2.exe"), "a" * 40)


def test_stock_runner_has_no_camera_thread_and_pins_mode(monkeypatch, tmp_path):
    monkeypatch.setenv("MC2_RENDER_SCALE", "0.25")
    monkeypatch.setattr(bench, "stock_identity", lambda *a: {})
    monkeypatch.setattr(bench, "machine_load", lambda: {})
    monkeypatch.setattr(bench.render_options_pin, "record", lambda *a: {"values": {}})
    monkeypatch.setattr(bench, "verify_runtime", lambda *a: None)
    monkeypatch.setattr(bench, "summarize_stock", lambda *a: {})
    def run(cfg):
        assert cfg.stem == "mc2_10" and cfg.duration == 20 and cfg.show_window
        assert cfg.env_extra["MC2_FPS_CAP"] == cfg.env_extra["MC2_VSYNC"] == "0"
        assert cfg.env_extra["MC2_WIDTH"] == "1920"
        assert "MC2_VISUAL_BOOKMARK_CAPTURE" not in cfg.env_extra
        assert "MC2_SIM_OVERLAP" not in cfg.env_extra
        assert "MC2_RENDER_SCALE" not in bench.os.environ
        assert not (tmp_path / "stock/commands.txt").read_text().find("bench_attack") >= 0
        return SimpleNamespace(stdout_text='[TIMING v1] event=mission_ready elapsed_ms=1234\n'
                               '[TIMING v1] event=mission_quit elapsed_ms=21234\n'
                               '[VK_PROFILE v1] verdict=ok device="fixture" api=1.3 instance_api=1.3 '
                               'floor=1.2 required_missing=- wanted_missing=- items=- enabled=- extensions=-', exit_code=0, killed_by_timeout=False,
                               verdict=SimpleNamespace(passed=True), walltime_s=23)
    monkeypatch.setattr(bench.runner, "run_one", run)
    result = bench.stock_run(tmp_path / "mc2.exe", tmp_path / "stock", "mc2_10", "a" * 40)
    assert result["actual_process_wall_s"] == 23
    assert result["actual_mission_duration_s"] == 20
    assert bench.os.environ["MC2_RENDER_SCALE"] == "0.25"
