"""Frame tails, catch-up bursts and failed workloads must survive the report boundary."""
import json
from pathlib import Path
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import run_battle_bench as bench


@pytest.fixture
def evidence(tmp_path):
    rows = [{"type": "world", "frame": 1, "sim_s": 0, "movers": 50, "units_alive": 50, "mechs": 48, "destroyed": 7},
            {"type": "world", "frame": 10, "sim_s": 3, "movers": 50, "units_alive": 49, "mechs": 48, "destroyed": 8},
            {"type": "orders", "sim_s": 1, "attacks": 48}]
    for frame in range(10, 110):
        ms = 100 if frame == 109 else 1
        rows += [{"type": "gpu", "frame": frame, "ms": ms, "recorded": 1, "valid": 1, "passes": {"VKIsl.Terrain": ms}},
                 {"type": "cpu", "frame": frame, "ms": ms, "ticks": 3 if frame == 109 else 1, "tick_total_ms": ms},
                 {"type": "tick", "frame": frame, "ms": ms}]
    (tmp_path / "commands.txt.ack").write_text("spawn clone 0 0 -> ok (spawned id=1 hp=1.000)\n", encoding="utf-8")
    return tmp_path, rows


def summarize(evidence):
    path, rows = evidence
    (path / "frames.jsonl").write_text("\n".join(json.dumps(r) for r in rows), encoding="utf-8")
    return bench.summarize(path, "[TIMING v1] event=mission_ready elapsed_ms=123.4", {"spawn": {"count": 1}})


def test_frame_tails_spikes_and_bursts_are_not_window_means(evidence):
    report = summarize(evidence)
    assert report["gpu_frame"]["p99_ms"] == 1
    assert report["gpu_frame"]["max_ms"] == 100
    assert report["gpu_frame"]["n"] == 100
    assert report["gpu_passes"]["terrain"]["timing"] == report["gpu_frame"]
    assert report["gpu_passes"]["water"]["timing"] is None
    assert len(report["tick_spikes_over_1ms"]) == 1
    assert report["catch_up_bursts"][0]["ticks"] == 3
    assert report["world"]["new_destructions"] == 1  # initial wrecks do not count
    assert report["load_ms"] == 123.4


def test_incomplete_timestamp_frame_refused(evidence):
    evidence[1][-3]["valid"] = 0
    with pytest.raises(ValueError, match="incomplete"):
        summarize(evidence)


def test_idle_workload_refused(evidence):
    evidence[1][1]["destroyed"] = 7
    with pytest.raises(ValueError, match="battle did not"):
        summarize(evidence)


def test_inert_clone_refused(evidence):
    (evidence[0] / "commands.txt.ack").write_text("spawn clone 0 0 -> ok (hp=0 WARNING:bad hp)\n", encoding="utf-8")
    with pytest.raises(ValueError, match="health"):
        summarize(evidence)


def test_diff_refuses_content_changes():
    before = {"scenario_sha256": "x", "content": {"sha256": "a"}}
    after = {"scenario_sha256": "x", "content": {"sha256": "b"}}
    with pytest.raises(ValueError, match="not comparable"):
        bench.diff(before, after)


def test_inactive_mod_bytes_do_not_change_stock_identity(tmp_path):
    (tmp_path / "mission.fst").write_bytes(b"stock")
    mod = tmp_path / "mods/inactive"
    mod.mkdir(parents=True)
    (mod / "mod.json").write_text('{"id":"inactive"}', encoding="utf-8")
    payload = mod / "unused.fst"
    payload.write_bytes(b"before")
    before = bench.content_identity(tmp_path)
    payload.write_bytes(b"after")
    after = bench.content_identity(tmp_path)
    assert before["sha256"] == after["sha256"]
    assert before["installed_mod_manifests"] == after["installed_mod_manifests"]
    assert before["active_mod"] is None


def test_shader_edits_change_build_identity_and_preserve_workload_identity(tmp_path):
    (tmp_path / "mission.fst").write_bytes(b"stock")
    shader = tmp_path / "shaders/test.comp"
    shader.parent.mkdir()
    shader.write_bytes(b"before")
    before = bench.content_identity(tmp_path)
    shader.write_bytes(b"after")
    after = bench.content_identity(tmp_path)
    assert before["sha256"] == after["sha256"]
    assert before["shaders_sha256"] != after["shaders_sha256"]


def test_first_seconds_tick_spikes_are_preserved(evidence):
    evidence[1].append({"type": "tick", "frame": 2, "ms": 500})
    report = summarize(evidence)
    assert report["sim_tick"]["max_ms"] == 500
    assert len(report["tick_spikes_over_1ms"]) == 2
    assert report["warmup_sim_seconds"] == 0


def test_scenario_uses_an_authored_mission_pilot(tmp_path):
    scenario = json.loads(bench.SCENARIO.read_text(encoding="utf-8"))
    channel = bench.prepare(tmp_path, scenario, "day")
    assert "spawn clone -980 2350 0 0 pmp_dagger" in channel.read_text(encoding="utf-8")
    assert "set_light day" not in channel.read_text(encoding="utf-8")


@pytest.mark.parametrize("sha,dirty", [("b" * 12, 0), ("a" * 12, 1)])
def test_runtime_build_substitution_is_rejected(sha, dirty):
    log = f"[BUILD_FINGERPRINT v1] sha={sha} dirty={dirty} branch=test built=2026-10-22 src=A:/test"
    with pytest.raises(ValueError, match="runtime build mismatch"):
        bench.verify_runtime(log, "a" * 40)


def test_bench_defaults_are_the_iteration_shape_until_acceptance():
    assert bench.resolve_lengths(None, None, False) == (1, 20)
    assert bench.resolve_lengths(None, None, True) == (2, 30)
    assert bench.resolve_lengths(3, 25, False) == (3, 25)


def test_visual_capture_default_duration_is_about_15s_and_grows_only_for_big_sweeps():
    capture = bench.capture
    assert capture.default_duration(1, 120, 30) == 15
    assert capture.default_duration(6, 120, 30) == 25
    assert capture.default_duration(1, 120, 30) <= 15 < capture.default_duration(12, 120, 30)


def test_run_smoke_falls_back_to_15s():
    import run_smoke
    assert run_smoke.DEFAULT_DURATION_S == 15
