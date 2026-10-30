"""CPU profile statistics and noise policy, without game/Tracy processes."""
import csv
from contextlib import nullcontext
import importlib.util
import io
from pathlib import Path
from types import SimpleNamespace

import pytest

PATH = Path(__file__).resolve().parents[2] / "tools/perf/mission_cpu_profile.py"
spec = importlib.util.spec_from_file_location("mission_cpu_profile", PATH)
profile = importlib.util.module_from_spec(spec)
spec.loader.exec_module(profile)


def test_real_percentiles_merge_same_named_events():
    rows = "name,ns,thread\nA,1000000,1\nA,9000000,2\nB,3000000,1\n"
    zones = profile.summarize(csv.DictReader(io.StringIO(rows)))
    assert zones["A"] == {"count": 2, "total_ms": 10, "p95_ms": 9, "worst_ms": 9}
    assert profile.p95(range(1, 101)) == 95


def test_startup_and_partial_events_are_excluded_from_mission_tables():
    rows = ("name,ns_since_start,ns\nload,0,1000000000\n"
            "Frame,1000000000,10000000\nGameLogic,1000000001,1000000\n"
            "partial,1009999999,100000\n")
    zones = profile.summarize(csv.DictReader(io.StringIO(rows)))
    assert "load" not in zones and "partial" not in zones
    assert zones["GameLogic"]["total_ms"] == 1


def test_bookmark_window_excludes_other_camera_frames():
    rows = ('name,ns_since_start,ns\nFrame,10,10\nDrawScreen,11,5\n'
            'Frame,30,20\nDrawScreen,31,15\nFrame,60,10\nDrawScreen,61,3\n')
    zones = profile.summarize(csv.DictReader(io.StringIO(rows)), [2, 3])
    assert zones['Frame']['total_ms'] == 20 / 1e6
    assert zones['DrawScreen']['worst_ms'] == 15 / 1e6


def test_frame_distribution_keeps_worst_hitch():
    result = profile.distribution([1] * 99 + [50])
    assert result == {'mean_ms': 1.49, 'p95_ms': 1, 'worst_ms': 50, 'count': 100}


def test_missing_bookmark_trace_frames_never_use_entire_trace():
    rows = 'name,ns_since_start,ns\nFrame,10,10\nDrawScreen,11,5\n'
    with pytest.raises(ValueError, match='frame range missing'):
        profile.summarize(csv.DictReader(io.StringIO(rows)), [2, 3])
    with pytest.raises(RuntimeError, match='alignment missing'):
        profile.summarize(csv.DictReader(io.StringIO(rows)), bookmarks={
            'orbit': {'last_cpu_frame': 3, 'frame_range': [1, 2]}})


@pytest.mark.parametrize("csv_text", ["name,total_ns\nA,100\n", "name,ns\n", "name,ns\nA,-1\n"])
def test_rejects_aggregate_empty_or_invalid_events(csv_text):
    with pytest.raises(ValueError):
        profile.summarize(csv.DictReader(io.StringIO(csv_text)))


def test_table_contains_both_rankings_and_sibling_split():
    zones = {"many": {"count": 100, "total_ms": 100, "p95_ms": 1},
             "hitch": {"count": 1, "total_ms": 20, "p95_ms": 20},
             **{name: {"count": 2, "total_ms": value, "p95_ms": value / 2}
                for name, value in zip(profile.SPLIT.values(), [10, 20, 10])}}
    text = profile.table("test", zones, {"status": "quiet", "host_cpu_p95": 5,
                                          "external_cpu_p95": 3, "reasons": []}, 1)
    assert "many" in text and "hitch" in text
    assert "50.0%" in text and "25.0%" in text
    assert "MISSING" not in text


def test_noise_samples_during_game_and_foreign_games():
    row = {"host_cpu_percent": 60, "external_cpu_percent": 30,
           "other_games": [], "own_games": [{"pid": 1}]}
    samples = SimpleNamespace(rows=[row], error=None)
    assert profile.noise_report(samples)["status"] == "NOISY"
    row["external_cpu_percent"] = 5
    assert profile.noise_report(samples)["status"] == "quiet"
    row["other_games"] = [{"pid": 22, "name": "mc2.exe"}]
    assert "other game processes observed" in profile.noise_report(samples)["reasons"]
    samples.rows = []
    assert profile.noise_report(samples)["status"] == "NOISY"


def test_worker_ticks_are_added_without_subtracting_present():
    zones = {name: {"count": 2, "total_ms": 10, "p95_ms": 5}
             for name in [*profile.SPLIT.values(), "SimOverlap.Tick"]}
    text = profile.table("test", zones, {"status": "quiet", "host_cpu_p95": 5,
                                          "external_cpu_p95": 3, "reasons": []}, 1)
    assert "| sim tick/logic (GameLogic) | 20.000 | 5.000 | 50.0% |" in text
    assert "| present (SwapWindow) | 10.000 | 5.000 | 25.0% |" in text
    assert "2 batches" in text


def test_other_game_detection_excludes_own_game():
    class FakePS:
        @staticmethod
        def process_iter(_):
            return [SimpleNamespace(info={"pid": pid, "name": name, "exe": exe})
                    for pid, name, exe in [(1, "mc2.exe", "E:/lane/mc2.exe"),
                                           (2, "other.exe", "D:/Steam/steamapps/common/game/other.exe"),
                                           (3, "ninja.exe", "A:/tools/ninja.exe")]]
    assert [p["pid"] for p in profile.game_processes(FakePS, {1})] == [2]


def test_other_game_refusal_never_launches_capture(tmp_path, monkeypatch):
    monkeypatch.setattr(profile.host_lease, "exclusive_window", lambda *a, **k: nullcontext())
    monkeypatch.setattr(profile, "game_processes", lambda *a: [{"pid": 12, "name": "mc2.exe"}])
    def forbidden(*args, **kwargs):
        pytest.fail("capture launched despite another game")
    monkeypatch.setattr(profile, "capture_mission", forbidden)
    with pytest.raises(RuntimeError, match="refusing profile with other games"):
        profile.profile(SimpleNamespace(install=tmp_path, seconds=10), "mc2_03", tmp_path / "out", None)


def test_runner_does_not_receive_host_knobs_or_wait_for_failed_client(tmp_path, monkeypatch):
    monkeypatch.setattr(profile.tracy, "game_env", lambda *a: {"TRACY_PORT": "8086", "MC2_HOST_GAME_SLOTS": "2"})
    monkeypatch.setattr(profile.tracy.render_options_pin, "record", lambda *a: {})
    monkeypatch.setattr(profile.tracy.render_options_pin, "write", lambda *a: None)
    monkeypatch.setattr(profile.tracy, "tracy_exe", lambda *a: tmp_path / "recorder.exe")
    monkeypatch.setattr(profile.time, "sleep", lambda *a: None)
    child = SimpleNamespace(poll=lambda: 1, wait=lambda **k: pytest.fail("wait on failed client"))
    monkeypatch.setattr(profile.subprocess, "Popen", lambda *a, **k: child)
    monkeypatch.setattr(profile.tracy, "_stop_capture_child", lambda p: None)
    def failed_run(config):
        assert "MC2_HOST_GAME_SLOTS" not in config.env_extra
        assert config.env_extra["TRACY_PORT"] == "8086"
        assert config.env_extra["MC2_FORCE_ALL_VISIBLE"] == "0"
        return SimpleNamespace(stdout_text="failed", exit_code=1, killed_by_timeout=False)
    monkeypatch.setattr(profile.runner, "run_one", failed_run)
    assert profile.capture_mission(tmp_path, "mc2_01", 10, tmp_path / "test.tracy", []) == 1
