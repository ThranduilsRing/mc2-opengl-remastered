"""Capture lease ordering and cleanup, without filesystem writes or child launches."""
import io
import subprocess
import sys
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import MagicMock, Mock

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import tracy_capture_compare as capture  # noqa: E402


def test_post_load_requires_ready_and_acknowledged_pose():
    ready = "[TIMING v1] event=mission_ready elapsed_ms=90000\n"
    park = "[CAMERA_PARK v1] frame=1 pos=99999,99999\n"
    assert not capture.post_load_ready(ready, "corner")
    assert not capture.post_load_ready(park, "corner")
    assert capture.post_load_ready(ready + park, "corner")
    assert capture.post_load_ready(ready, None)


def test_post_load_wait_excludes_loading_and_settling(tmp_path, monkeypatch):
    log = tmp_path / "game.log"
    clock = [0]
    monkeypatch.setattr(capture.time, "monotonic", lambda: clock[0])
    def advance(_):
        clock[0] += 1
        log.write_text("" if clock[0] < 90 else
                       "[TIMING v1] event=mission_ready elapsed_ms=90000\n"
                       "[CAMERA_PARK v1] frame=1 pos=99999,99999\n")
    log.write_text("")
    monkeypatch.setattr(capture.time, "sleep", advance)
    capture.wait_post_load(Mock(poll=lambda: None), log, "corner", settle=2)
    assert clock[0] == 92
    with pytest.raises(RuntimeError, match="did not settle"):
        capture.wait_post_load(Mock(poll=lambda: 0), log, "corner")


@pytest.mark.parametrize("count,duration,status", [(3, 10_000_000, "REJECTED"),
    (600, 50_000_000, "PASS"), (600, 10_000_000, "REJECTED"), (0, 0, "REJECTED")])
def test_window_rejects_loading_or_short_frames(count, duration, status):
    rows = [{"name": "Frame", "ns_since_start": str(100_000_000_000 + i * duration),
             "exec_time_ns": str(duration)} for i in range(count)]
    result = capture.window_stats(rows, 30)
    assert result["status"] == status
    assert result["frames_in_window"] == count


def test_capture_arm_overrides_are_declared():
    assert capture.CAPTURE_ARMS["visible0"] == ["MC2_FORCE_ALL_VISIBLE=0"]
    assert "MC2_TERRAIN_LOD_CHUNK_FORCE_LOD=2" in capture.CAPTURE_ARMS["lod"]


def test_pan_uses_engine_commands_and_both_endpoint_shots(monkeypatch):
    channel = Mock()
    channel.stat.return_value.st_size = 0
    monkeypatch.setattr(capture.time, "sleep", Mock())
    send = Mock()
    monkeypatch.setattr(capture, "camera_command", send)
    capture.pan_camera(channel, 7680, 2)
    commands = [call.args[1] for call in send.call_args_list]
    assert len(commands) == 21
    assert commands[0].startswith("bench_view 7680.000 7680.000 ")
    assert commands[-1].startswith("bench_view -7680.000 -7680.000 ")
    assert "screenshot pan_start" in commands[0] and "screenshot pan_end" in commands[-1]


def test_camera_command_waits_for_slow_first_frame_and_its_own_ack(tmp_path, monkeypatch):
    channel = tmp_path / "commands.txt"
    ack = tmp_path / "commands.txt.ack"
    channel.write_text("old command\n")
    ack.write_text("bench_view 1 2 -45 5853 12.5 -> ok\n")
    clock = [0]
    monkeypatch.setattr(capture.time, "monotonic", lambda: clock[0])
    def advance(_):
        clock[0] += .1
        if clock[0] >= .5 and channel.read_text() == "old command\n": channel.write_text("")
        if clock[0] >= .8 and channel.read_text().startswith("bench_view"):
            ack.write_text(ack.read_text() + "bench_view 1 2 -45 5853 12.5 -> ok\n")
            channel.write_text("")
    monkeypatch.setattr(capture.time, "sleep", advance)
    capture.camera_command(channel, "bench_view 1 2 -45 5853 12.5\n")
    assert clock[0] >= .8
    channel.write_text("stuck\n")
    with pytest.raises(RuntimeError, match="not consumed"):
        capture.camera_command(channel, "other\n", timeout=.2)


@pytest.mark.parametrize("opt_in,tag,exclusive,slots,accepted", [
    (False, "batch", True, 2, False), (True, "batch", True, 2, True),
    (True, "batch", False, 2, None), (True, "batch", True, 1, None),
    (True, "", True, 2, None)])
def test_owner_affinity_requires_explicit_exclusive_batch(monkeypatch, opt_in, tag, exclusive, slots, accepted):
    monkeypatch.setenv("MC2_NO_CPU_PIN", "1" if opt_in else "0")
    monkeypatch.setenv(capture.host_lease.BATCH_ENV, tag)
    monkeypatch.setattr(capture.host_lease, "slot_count", lambda: 2)
    monkeypatch.setattr(capture.host_lease, "slot_path", lambda i: i)
    monkeypatch.setattr(capture.host_lease, "_ours", lambda r: True)
    monkeypatch.setattr(capture.host_lease, "_records", lambda: [
        (i, {"exclusive": exclusive, "batch": "batch"}) for i in range(slots)])
    if accepted is None:
        with pytest.raises(RuntimeError, match="exclusive all-slot batch"):
            capture.owner_capture_mode()
    else:
        assert capture.owner_capture_mode() is accepted


@pytest.mark.parametrize("platform,cores,owner", [("nt", [0, 1], False), ("posix", [26, 27], False),
                                                ("nt", list(range(16)), True)])
def test_post_load_starts_recorder_after_pose_and_writes_row(harness, monkeypatch, platform, cores, owner):
    import psutil
    h = harness
    monkeypatch.setenv("MC2_NO_CPU_PIN", "1" if owner else "0")
    monkeypatch.delenv("MC2_FRAME_JOBS_WORKERS", raising=False)
    monkeypatch.setattr(psutil, "cpu_count", lambda: 16)
    monkeypatch.setattr(capture, "owner_capture_mode", lambda: owner)
    if owner:
        monkeypatch.setattr(Path, "read_text", lambda *a, **k: "[TerrainLOD v1] halfMap=7680.0")
        monkeypatch.setattr(Path, "write_text", Mock())
        pan_driver = Mock()
        monkeypatch.setattr(capture, "pan_camera", pan_driver)
        monkeypatch.setattr(capture, "camera_command", lambda *a: h.events.append("camera.ack"))
    monkeypatch.setattr(capture, "os", SimpleNamespace(name=platform, environ=capture.os.environ))
    process = Mock(cpu_affinity=Mock(return_value=cores))
    monkeypatch.setattr(psutil, "Process", lambda _: process)
    monkeypatch.setattr(capture.host_lease, "held_device", lambda: "AMD")
    monkeypatch.setattr(capture.host_lease, "lane_devices", lambda: ("AMD", "NVIDIA"))
    monkeypatch.setattr(capture.host_lease, "affinity_mask", lambda _: 3)
    h.install.__truediv__.return_value.read_bytes.return_value = b"exe"
    monkeypatch.setattr(capture, "wait_post_load", lambda *a: h.events.append("pose.settled"))
    monkeypatch.setattr(capture.subprocess, "run", Mock(return_value=SimpleNamespace(
        returncode=0, stdout="name,ns_since_start,exec_time_ns\nFrame,0,30000000000\n", stderr="")))
    monkeypatch.setattr(capture, "window_stats", lambda *a: {"status": "PASS"})
    stats_file = Mock()
    h.out.with_suffix.side_effect = lambda suffix: stats_file if suffix == ".stats.json" else Path("trace" + suffix)
    assert capture.capture_trace(h.install, "vk", "mc2_15", 30, h.out, post_load=True, pan=owner) == 0
    if owner:
        pan_driver.assert_called_once_with(Path("trace.commands.txt"), 7680.0, 29)
        assert h.events.index("camera.ack") < h.events.index("recorder.spawn")
    assert h.events.index("game.spawn") < h.events.index("pose.settled") < h.events.index("recorder.spawn")
    row = capture.json.loads(stats_file.write_text.call_args.args[0])
    assert row["status"] == "PASS" and row["affinity"] == cores and row["job_workers"] == (6 if owner else 1)
    assert h.game_spawn.call_args.kwargs["engine_default_workers"] is owner
    assert ("MC2_FRAME_JOBS_WORKERS" in h.game_spawn.call_args.kwargs["env"]) is not owner
    assert row["affinity_mask"] == sum(1 << c for c in cores)
    if platform == "nt" and not owner:
        process.cpu_affinity.assert_any_call(cores)
    else:
        process.cpu_affinity.assert_called_once_with()
    recorder = h.recorder_spawn.call_args.args[0]
    assert recorder[recorder.index("-s") + 1] == "30"


@pytest.mark.parametrize("platform,suffix", [("nt", ".exe"), ("posix", "")])
@pytest.mark.parametrize("name", ["tracy-capture", "tracy-csvexport"])
def test_tracy_exe_uses_native_name(tmp_path, monkeypatch, platform, suffix, name):
    tool = tmp_path / (name + suffix)
    tool.write_bytes(b"tool")
    monkeypatch.setenv("TRACY_TOOLS_DIR", str(tmp_path))
    monkeypatch.setattr(capture, "os", SimpleNamespace(name=platform, environ=capture.os.environ))
    assert capture.tracy_exe(name + ".exe") == tool


@pytest.fixture
def harness(monkeypatch):
    h = SimpleNamespace(held=False, events=[], logs=[], children=[])

    def acquire(label, **kwargs):
        assert not h.held
        h.held = True
        h.events.append("acquire")

    def release():
        if not h.held:
            return
        assert h.held
        assert all(child.poll() is not None for child in h.children)
        assert all(log.closed for log in h.logs)
        h.events.append("release")
        h.held = False

    def child(name):
        proc = Mock(pid=len(h.children) + 1, returncode=None)

        def wait(timeout=None):
            assert h.held
            h.events.append(name + ".wait")
            proc.returncode = 0
            return 0

        def kill():
            assert h.held
            h.events.append(name + ".kill")
            proc.returncode = -1

        proc.poll.side_effect = lambda: proc.returncode
        proc.wait.side_effect = wait
        proc.kill.side_effect = kill
        h.children.append(proc)
        return proc

    def recorder_spawn(argv, **kwargs):
        assert h.held
        h.events.append("recorder.spawn")
        return child("recorder")

    def game_spawn(argv, **kwargs):
        assert h.held
        h.events.append("game.spawn")
        return child("game")

    def open_log(*args, **kwargs):
        assert h.held
        log = io.StringIO()
        h.logs.append(log)
        return log

    h.out = MagicMock(spec=Path)
    h.out.resolve.return_value = h.out
    h.out.is_file.return_value = True
    h.out.stat.return_value = SimpleNamespace(st_size=capture.MIN_TRACE_BYTES)
    h.out.with_suffix.side_effect = lambda suffix: Path("trace.tracy").with_suffix(suffix)
    h.install = MagicMock(spec=Path)
    h.install.__truediv__.return_value.is_file.return_value = True
    h.acquire = Mock(side_effect=acquire)
    h.release = Mock(side_effect=release)
    h.recorder_spawn = Mock(side_effect=recorder_spawn)
    h.game_spawn = Mock(side_effect=game_spawn)
    monkeypatch.setattr(capture, "Path", Mock(return_value=h.out))
    monkeypatch.setattr(capture, "open", open_log, raising=False)
    monkeypatch.setattr(capture, "tracy_exe", Mock(return_value=Path("tracy-capture.exe")))
    monkeypatch.setattr(capture.host_lease, "acquire", h.acquire)
    monkeypatch.setattr(capture.host_lease, "release", h.release)
    monkeypatch.setattr(capture.host_lease, "holds", lambda: h.held)
    monkeypatch.setattr(capture.subprocess, "Popen", h.recorder_spawn)
    monkeypatch.setattr(capture.subprocess, "run", Mock(side_effect=AssertionError("unexpected child")))
    monkeypatch.setattr(capture.nofocus, "spawn", h.game_spawn)
    monkeypatch.setattr(capture.nofocus, "hidden_kwargs", Mock(return_value={}))
    monkeypatch.setattr(capture.time, "sleep", Mock())
    monkeypatch.setattr(capture.render_options_pin, "record", Mock(return_value={"rejected": []}))
    monkeypatch.setattr(capture.render_options_pin, "write", Mock())
    h.run = lambda show_window=True: capture.capture_trace(h.install, "vk", "mc2_01", 15, h.out,
                                                          show_window=show_window)
    return h


@pytest.mark.parametrize("show_window", [True, False])
@pytest.mark.parametrize("platform,exe", [("nt", "mc2.exe"), ("posix", "mc2")])
def test_lease_precedes_both_children_and_covers_normal_exit(harness, show_window, monkeypatch, platform, exe):
    h = harness
    monkeypatch.setattr(capture, "os", SimpleNamespace(name=platform, environ=capture.os.environ))
    assert h.run(show_window) == 0
    assert h.events == ["acquire", "recorder.spawn", "game.spawn", "game.wait",
                        "recorder.wait", "game.wait", "recorder.wait", "release"]
    h.acquire.assert_called_once()
    h.release.assert_called_once()
    assert not h.held
    kwargs = h.game_spawn.call_args.kwargs
    assert kwargs["show_window"] is show_window and kwargs["small_window"] is False
    assert kwargs["env"]["MC2_WINDOW_OFFSCREEN"] == "1"
    h.install.__truediv__.assert_called_with(exe)
    assert h.game_spawn.call_args.args[0][0] == str(h.install / exe)
    recorder = h.recorder_spawn.call_args.args[0]
    assert recorder[recorder.index("-s") + 1] == "15"


@pytest.mark.parametrize("exited,recorder_rc,expected", [(False, 0, 0), (True, 0, 1), (False, 1, 1)])
def test_via_menu_ignores_only_its_own_game_stop(harness, exited, recorder_rc, expected):
    h = harness
    spawn_game, spawn_recorder = h.game_spawn.side_effect, h.recorder_spawn.side_effect

    def game(*args, **kwargs):
        proc = spawn_game(*args, **kwargs)
        proc.returncode = 7 if exited else None
        proc.wait.side_effect = lambda timeout=None: proc.returncode
        return proc

    def recorder(*args, **kwargs):
        proc = spawn_recorder(*args, **kwargs)
        def wait(timeout=None):
            proc.returncode = recorder_rc
            return recorder_rc
        proc.wait.side_effect = wait
        return proc

    h.game_spawn.side_effect, h.recorder_spawn.side_effect = game, recorder
    assert capture.capture_trace(h.install, "vk", "mc2_01", 15, h.out, via_menu_boot=True) == expected
    assert ("game.kill" in h.events) is not exited
    h.release.assert_called_once()


@pytest.mark.parametrize("which", ["recorder", "game"])
def test_spawn_failure_reaps_existing_child_and_releases(harness, which):
    h = harness
    getattr(h, which + "_spawn").side_effect = OSError("spawn failed")
    with pytest.raises(OSError, match="spawn failed"):
        h.run()
    h.release.assert_called_once()
    assert not h.held and h.events[-1] == "release"
    if which == "game":
        assert h.events == ["acquire", "recorder.spawn", "recorder.kill", "recorder.wait", "release"]
    else:
        h.game_spawn.assert_not_called()


def test_busy_lease_starts_no_children(harness):
    h = harness
    h.acquire.side_effect = capture.host_lease.HostLeaseBusy("busy")
    with pytest.raises(capture.host_lease.HostLeaseBusy, match="busy"):
        h.run()
    h.recorder_spawn.assert_not_called()
    h.game_spawn.assert_not_called()
    h.release.assert_not_called()
    assert not h.logs


def test_interruption_before_game_spawn_reaps_recorder(harness, monkeypatch):
    h = harness
    monkeypatch.setattr(capture.time, "sleep", Mock(side_effect=KeyboardInterrupt))
    with pytest.raises(KeyboardInterrupt):
        h.run()
    assert h.events == ["acquire", "recorder.spawn", "recorder.kill", "recorder.wait", "release"]
    h.game_spawn.assert_not_called()


@pytest.mark.parametrize("operation", ["record", "write"])
def test_options_failure_reaps_recorder_and_releases(harness, operation):
    h = harness
    getattr(capture.render_options_pin, operation).side_effect = OSError("options failed")
    with pytest.raises(OSError, match="options failed"):
        h.run()
    assert h.events == ["acquire", "recorder.spawn", "recorder.kill", "recorder.wait", "release"]
    h.game_spawn.assert_not_called()


@pytest.mark.parametrize("fail_on", [1, 2])
def test_log_open_failure_releases(harness, monkeypatch, fail_on):
    h = harness
    original_open = capture.open
    calls = 0

    def open_log(*args, **kwargs):
        nonlocal calls
        calls += 1
        if calls == fail_on:
            raise OSError("log failed")
        return original_open(*args, **kwargs)

    monkeypatch.setattr(capture, "open", open_log)
    with pytest.raises(OSError, match="log failed"):
        h.run()
    h.release.assert_called_once()
    assert not h.held
    h.game_spawn.assert_not_called()


def test_wait_failure_reaps_both_children_and_releases(harness):
    h = harness
    original_spawn = h.game_spawn.side_effect

    def game_spawn(*args, **kwargs):
        proc = original_spawn(*args, **kwargs)
        original_wait = proc.wait.side_effect

        def wait(timeout=None):
            if timeout is not None:
                raise RuntimeError("wait failed")
            return original_wait()

        proc.wait.side_effect = wait
        return proc

    h.game_spawn.side_effect = game_spawn
    with pytest.raises(RuntimeError, match="wait failed"):
        h.run()
    assert h.events == ["acquire", "recorder.spawn", "game.spawn", "game.kill", "game.wait",
                        "recorder.kill", "recorder.wait", "release"]


def test_trivial_trace_releases_on_system_exit(harness):
    h = harness
    h.out.stat.return_value.st_size = 0
    with pytest.raises(SystemExit, match="missing or trivial"):
        h.run()
    h.release.assert_called_once()
    assert not h.held


@pytest.mark.parametrize("which", ["recorder", "game"])
def test_timeout_reaps_children_before_release(harness, which):
    h = harness
    spawn = getattr(h, which + "_spawn")
    original_spawn = spawn.side_effect

    def timed_spawn(*args, **kwargs):
        proc = original_spawn(*args, **kwargs)
        original_wait = proc.wait.side_effect

        def wait(timeout=None):
            if timeout is not None:
                assert h.held
                raise subprocess.TimeoutExpired(which, timeout)
            return original_wait()

        proc.wait.side_effect = wait
        return proc

    spawn.side_effect = timed_spawn
    assert h.run() == 1
    assert which + ".kill" in h.events
    h.release.assert_called_once()
    assert not h.held


def test_vk_arm_leaves_island_defaults_to_the_engine(monkeypatch):
    # lane 2 owns VK semantics: the arm no longer forces MC2_VK_SHADOW_INTEROP; an explicit --env still sets it.
    monkeypatch.delenv("MC2_VK_SHADOW_INTEROP", raising=False)
    assert "MC2_VK_SHADOW_INTEROP" not in capture.VK_ENV
    assert "MC2_VK_SHADOW_INTEROP" not in capture.game_env("vk", True, None, None, [])
    assert capture.game_env("vk", True, None, None, ["MC2_VK_SHADOW_INTEROP=0"])["MC2_VK_SHADOW_INTEROP"] == "0"


def test_held_chain_lease_is_not_released_by_a_capture(harness):
    h = harness
    h.held = True                                   # main() took it for the whole command
    assert capture.capture_trace(h.install, "vk", "mc2_01", 15, h.out) == 0
    h.acquire.assert_not_called()
    h.release.assert_not_called()
    assert h.held


def test_gl_arm_is_refused_before_anything_runs():
    with pytest.raises(SystemExit, match="GL arm was retired"):
        capture.game_env("gl", False, 0, None, [])


def test_vk_measurement_without_the_backend_receipt_is_refused(tmp_path):
    log = tmp_path / "run.game.log"
    log.write_text("[BOOT] engine up\n", encoding="utf-8")
    with pytest.raises(SystemExit, match="Vulkan backend selected"):
        capture.assert_drew_vk(log)


def test_vk_measurement_that_fell_back_to_gl_is_refused(tmp_path):
    log = tmp_path / "run.game.log"
    log.write_text("Vulkan backend selected\n[RENDER_BACKEND] fail-soft to GL\n", encoding="utf-8")
    with pytest.raises(SystemExit, match="REFUSING"):
        capture.assert_drew_vk(log)


def test_measure_pose_refuses_a_run_that_did_not_draw_vk(tmp_path, monkeypatch):
    monkeypatch.setattr(capture, "capture_trace", lambda *a, **k: None)
    monkeypatch.setattr(capture, "assert_window_was_visible", lambda *a, **k: None)
    (tmp_path / "p.game.log").write_text("[BOOT] engine up\n", encoding="utf-8")
    with pytest.raises(SystemExit, match="Vulkan backend selected"):
        capture._measure_pose(Path("inst"), tmp_path, "p", "mc2_10", "", 5, [], Path("exp"), "DrawScreen")


def test_selfcheck_pair_is_the_two_resolution_tiers():
    assert capture.SELFCHECK_SIZES == ((1920, 1080), (3840, 2160))


def test_compare_takes_no_capture_lease(monkeypatch):
    events = []
    monkeypatch.setattr(capture.host_lease, "acquire", lambda label: events.append("acquire"))
    monkeypatch.setattr(capture.host_lease, "release", lambda: events.append("release"))
    monkeypatch.setattr(capture, "cmd_compare", lambda args: events.append("compare") or 0)
    monkeypatch.setattr(sys, "argv", ["tracy_capture_compare.py", "compare", "a.tracy", "b.tracy"])
    assert capture.main() == 0
    assert events == ["compare"]


@pytest.mark.parametrize("platform,filename", [("nt", "tracy-csvexport.exe"), ("posix", "tracy-csvexport")])
def test_zone_summary_prints_top_zones_and_writes_csv_only_when_asked(monkeypatch, tmp_path, capsys, platform, filename):
    monkeypatch.setattr(capture, "os", SimpleNamespace(name=platform))
    exe = tmp_path / filename
    exe.write_text("")
    monkeypatch.setattr(capture, "tracy_dir", lambda: tmp_path)
    table = "name,src_file,src_line,total_ns,total_perc,counts,mean_ns\nsmall,a,1,1000000,1,5,200000\nbig,a,2,9000000,9,3,3000000\n"
    monkeypatch.setattr(capture.subprocess, "run", lambda *a, **k: SimpleNamespace(returncode=0, stdout=table, stderr=""))
    capture.zone_summary(tmp_path / "t.tracy", top=1)
    out = capsys.readouterr().out
    assert "big" in out and "small" not in out and not list(tmp_path.glob("*.csv"))
    capture.zone_summary(tmp_path / "t.tracy", top=1, csv_out=tmp_path / "t.zones.csv")
    assert (tmp_path / "t.zones.csv").read_text(encoding="utf-8") == table


@pytest.mark.parametrize("receipt,expected", [
    ("[SMOKE v1] event=tick_budget ticks=900\n", 0),
    ("[SMOKE v1] event=tick_budget ticks=38\n", 1),
    ("[SMOKE v1] event=tick_budget ticks=900\nCrash Report", 1),
])
def test_tick_capture_waits_for_disconnect_and_requires_budget(harness, monkeypatch, receipt, expected):
    h = harness
    monkeypatch.setattr(Path, "read_text", lambda *args, **kwargs: receipt)
    assert capture.capture_trace(h.install, "vk", "mc2_03", 15, h.out, ticks=900) == expected
    recorder_cmd = h.recorder_spawn.call_args.args[0]
    game_cmd = h.game_spawn.call_args.args[0]
    assert "-s" not in recorder_cmd
    assert game_cmd[-3:] == ["--headless", "--ticks", "900"]
    assert h.events[-1] == "release"
