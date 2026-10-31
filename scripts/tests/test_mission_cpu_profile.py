"""CPU profile statistics and noise policy, without game/Tracy processes."""
import csv
from contextlib import nullcontext
import importlib.util
import io
import json
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


def test_default_camera_is_one_low_sun_view_and_full_sweep_is_explicit():
    for mission in profile.CAMERA_MISSIONS:
        marks = profile.camera_bookmarks(mission)['bookmarks']
        assert [m['name'] for m in marks] == ['sun_facing_low']
        assert any(m['name'] == 'orbit' for m in profile.camera_bookmarks(mission, True)['bookmarks'])


def test_single_view_measurement_extends_through_mission_exit(tmp_path):
    mark = profile.camera_bookmarks('mc2_01')['bookmarks'][0]
    rows = [dict(type='view', frame=10)] + [dict(type='cpu', frame=f, ms=f) for f in range(10, 30)]
    (tmp_path / 'frames.jsonl').write_text('\n'.join(json.dumps(row) for row in rows))
    pose = {'cap_pos': mark['pos'], 'cap_rot': mark['rot'], 'cap_proj': mark['proj'], 'cap_alt': mark['alt']}
    (tmp_path / f"{tmp_path.name}_{mark['name']}.json").write_text(json.dumps(pose))
    measured = profile.bookmark_measurements(tmp_path, [mark], '')[mark['name']]
    assert measured['frame_range'] == [14, 30]
    assert measured['cpu_frame']['count'] == 16


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
    row['own_games'].append({'pid':2})
    assert 'multiple owned game processes observed' in profile.noise_report(samples)['reasons']
    row['own_games'].pop()
    row["other_games"] = [{"pid": 22, "name": "mc2.exe"}]
    assert "other game processes observed" in profile.noise_report(samples)["reasons"]
    samples.rows = []
    assert profile.noise_report(samples)["status"] == "NOISY"


def test_noise_uses_reserved_cores_and_excludes_pinned_work():
    # Busy desktop cores must not reject an otherwise quiet game reservation.
    result = profile.external_cpu([90] * 12 + [30, 30, 30, 30], [12, 13, 14, 15], 100, 16)
    assert result["external_cpu_percent"] == 1
    assert result["whole_machine_external_cpu_percent"] == 67.75
    outside = profile.external_cpu([90] * 12 + [30] * 4, [12, 13, 14, 15], 100, 16, False)
    assert outside['external_cpu_percent'] == 5
    assert outside['whole_machine_external_cpu_percent'] == result['whole_machine_external_cpu_percent']
    row = {"host_cpu_percent": 75, **result, "own_games": [{"pid": 1}], "other_games": []}
    samples = SimpleNamespace(rows=[row], error=None)
    assert profile.noise_report(samples)["status"] == "quiet"
    row["external_cpu_percent"] = 15
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


def test_battle_uses_main_thread_spawn_pools(tmp_path, monkeypatch):
    monkeypatch.setattr(profile.host_lease, 'exclusive_window', lambda *a, **k: nullcontext())
    monkeypatch.setattr(profile, 'game_processes', lambda *a: [])
    monkeypatch.setattr(profile, 'HostSamples', lambda *a: nullcontext())
    monkeypatch.setattr(profile, 'noise_report', lambda *a: {})
    def inspect(install, mission, seconds, trace, env):
        assert not any(value.startswith('MC2_MOVER_FIXTURE=') for value in env)
        commands = (trace.parent / 'commands.txt').read_text().splitlines()
        assert sum(line.startswith('spawn clone ') for line in commands) == 128
        assert sum(line.startswith('spawn vehicle ') for line in commands) == 2
        assert commands.index('select all') > max(i for i, line in enumerate(commands) if line.startswith('spawn '))
        raise RuntimeError('spawn pools checked')
    monkeypatch.setattr(profile, 'capture_mission', inspect)
    with pytest.raises(RuntimeError, match='spawn pools checked'):
        profile.profile(SimpleNamespace(install=tmp_path, seconds=20), 'battle', tmp_path / 'battle', None)


@pytest.mark.parametrize("battle_commands", [False, True])
def test_runner_does_not_receive_host_knobs_or_wait_for_failed_client(tmp_path, monkeypatch, battle_commands):
    channel = tmp_path / "commands.txt"
    channel.write_text("spawn clone 0 0\n")
    extra = {"MC2_UI_COMMAND_FILE": str(channel)} if battle_commands else {}
    monkeypatch.setenv("MC2_VK_DEVICE", "NVIDIA")
    monkeypatch.setattr(profile.tracy, "game_env", lambda *a: {"TRACY_PORT": "8086", "MC2_HOST_GAME_SLOTS": "2", **extra})
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
        assert config.env_extra["MC2_VK_DEVICE"] == "NVIDIA"
        return SimpleNamespace(stdout_text="failed", exit_code=1, killed_by_timeout=False)
    monkeypatch.setattr(profile.runner, "run_one", failed_run)
    assert profile.capture_mission(tmp_path, "mc2_01", 10, tmp_path / "test.tracy", []) == 1
    if battle_commands:
        assert channel.read_text() == "spawn clone 0 0\n"


@pytest.mark.parametrize('has_samples', [True, False])
def test_failed_capture_keeps_both_noise_numbers(tmp_path, monkeypatch, has_samples):
    install = tmp_path / 'install'
    install.mkdir()
    (install / 'mc2.exe').write_bytes(b'test')
    monkeypatch.setattr(profile.tracy, 'tracy_exe', lambda *a: tmp_path / 'recorder.exe')
    def fail(args, name, directory, psutil):
        directory.mkdir(parents=True)
        if has_samples:
            (directory / 'host-load.json').write_text(json.dumps({
                'status':'NOISY', 'external_cpu_p95':19, 'whole_machine_external_cpu_p95':41}))
        raise RuntimeError('capture failed')
    monkeypatch.setattr(profile, 'profile', fail)
    out = tmp_path / 'out'
    assert profile.main(['--install', str(install), '--missions', 'mc2_03', '--out', str(out)]) == 1
    report = json.loads(next(out.glob('*/profile.json')).read_text())
    noise = report['runs']['mc2_03']['noise']
    assert noise['external_cpu_p95'] == (19 if has_samples else None)
    assert noise['whole_machine_external_cpu_p95'] == (41 if has_samples else None)
    assert report['errors']['mc2_03'] == 'capture failed'
